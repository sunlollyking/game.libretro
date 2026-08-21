/*
 *  Copyright (C) 2014-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "cheevos/Cheevos.h"
#include "cheevos/CheevosEnvironment.h"
#include "input/ButtonMapper.h"
#include "input/ControllerTopology.h"
#include "input/InputManager.h"
#include "libretro-common/libretro.h"
#include "libretro/LibretroEnvironment.h"
#include "log/Log.h"
#include "log/LogAddon.h"
#include "settings/Settings.h"
#include "GameInfoLoader.h"

#include "client.h"

#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace LIBRETRO;

#define GAME_CLIENT_NAME_UNKNOWN      "Unknown libretro core"
#define GAME_CLIENT_VERSION_UNKNOWN   "0.0.0"

namespace
{
/*!
 * \brief Layout of a savestate that carries achievement progress
 *
 *   [ emulator state ][ achievement progress ][ footer ]
 *
 * rc_client keeps its own view of how far along each achievement's trigger is.
 * Rewinding or loading a savestate moves emulator memory backwards without
 * moving that view, which lets a partially-satisfied trigger stay satisfied
 * across a rewind and fire on a run that didn't earn it. Saving the progress
 * alongside the emulator state keeps the two in step.
 *
 * The footer sits at the very end rather than the front so that savestates
 * written before this existed stay loadable: their trailing bytes won't match
 * the magic, and the blob is then treated as emulator state in its entirety.
 */
constexpr uint8_t SAVESTATE_MAGIC[4] = {'K', 'R', 'A', '1'};
constexpr uint32_t SAVESTATE_VERSION = 2;

struct SavestateFooter
{
  uint8_t magic[4];
  uint32_t version;

  /*!
   * \brief Length of the emulator state at the front of the blob
   *
   * Recorded explicitly rather than derived from the other sizes. The space
   * reserved for progress is usually larger than what the runtime writes, so
   * the leftover sits between the progress data and this footer - deriving the
   * emulator state's length from the total would silently include that gap and
   * hand the core a longer buffer than it saved.
   */
  uint64_t coreSize;

  /// \brief Length of the achievement progress that follows the emulator state
  uint64_t progressSize;
};

static_assert(sizeof(SavestateFooter) == 24, "SavestateFooter must not carry padding");

constexpr size_t FOOTER_SIZE = sizeof(SavestateFooter);

/*!
 * \brief Space set aside for achievement progress in every savestate
 *
 * Kodi asks for the savestate size once, when the game is loaded, and caches
 * it. That happens before rc_client has finished identifying the game over the
 * network, so neither the progress size nor even the achievement count is
 * known yet - the reserve cannot be derived from the set being played.
 *
 * It is therefore sized for the worst case instead: rc_client stores a header
 * plus roughly 40 bytes per active achievement, and the largest sets published
 * by RetroAchievements run to a few hundred achievements. 8 KiB covers about
 * 200 of them.
 *
 * Kept deliberately tight because it is charged against every frame held in
 * the rewind buffer, not just explicit savestates. A set that needs more than
 * this saves emulator state alone rather than overrunning the buffer, and
 * ProgressSize() logs the shortfall so the figure can be revisited.
 */
constexpr size_t PROGRESS_RESERVE_BYTES = 8 * 1024;
} // namespace

void SAFE_DELETE_GAME_INFO(std::vector<CGameInfoLoader*>& vec)
{
  for (std::vector<CGameInfoLoader*>::iterator it = vec.begin(); it != vec.end(); ++it)
    delete *it;
  vec.clear();
}

CGameLibRetro::CGameLibRetro()
{
}

CGameLibRetro::~CGameLibRetro()
{
  /* TODO
  m_clientBridge.AudioEnable(false);
  */

  CInputManager::Get().ClosePorts();

  m_client.retro_deinit();

  CControllerTopology::GetInstance().Clear();

  CLibretroEnvironment::Get().Deinitialize();
  CCheevosEnvironment::Get().Deinitialize();

  CCheevos::Get().Deinitialize();

  CLog::Get().SetType(SYS_LOG_TYPE_CONSOLE);

  SAFE_DELETE_GAME_INFO(m_gameInfo);
}

ADDON_STATUS CGameLibRetro::Create()
{
  try
  {
    std::string dllPath = GameClientDllPath();
    if (dllPath.empty())
      throw ADDON_STATUS_UNKNOWN;

    CLog::Get().SetType(SYS_LOG_TYPE_ADDON);

    if (!m_client.Load(dllPath))
    {
      esyslog("Failed to load %s", dllPath.c_str());
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    unsigned int version = m_client.retro_api_version();
    if (version != 1)
    {
      esyslog("Expected libretro api v1, found version %u", version);
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    // Environment must be initialized before calling retro_init()
    CLibretroEnvironment::Get().InitializeEnvironment(this, &m_client, &m_clientBridge);
    CCheevosEnvironment::Get().Initialize();

    CButtonMapper::Get().LoadButtonMap();
    CControllerTopology::GetInstance().LoadTopology();



    m_client.retro_init();

    CLibretroEnvironment::Get().InitializeCallbacks();

    // Log core info
    retro_system_info systemInfo = { };
    m_client.retro_get_system_info(&systemInfo);

    // VFS support is derived from need_fullpath. This property means that the
    // libretro cores requires a valid pathname. Conversely, if need_fullpath
    // is false, the core can load from memory.
    m_supportsVFS = !systemInfo.need_fullpath;

    std::string libraryName = systemInfo.library_name ? systemInfo.library_name : "";
    std::string libraryVersion = systemInfo.library_version ? systemInfo.library_version : "";
    std::string extensions = systemInfo.valid_extensions ? systemInfo.valid_extensions : "";

    dsyslog("CORE: ----------------------------------");
    dsyslog("CORE: Library name:    %s", libraryName.c_str());
    dsyslog("CORE: Library version: %s", libraryVersion.c_str());
    dsyslog("CORE: Extensions:      %s", extensions.c_str());
    dsyslog("CORE: Supports VFS:    %s", m_supportsVFS ? "true" : "false");
    dsyslog("CORE: ----------------------------------");

    // Reject invalid properties
    std::set<std::string> coreExtensions; // TODO: Parse string from libretro API
    std::set<std::string> addonExtensions; // TODO: Convert char** to set<string>

    if (coreExtensions != addonExtensions)
    {
      std::string strAddonExtensions;// = StringUtils::Join(addonExtensions, "|"); // TODO
      esyslog("CORE: Extensions don't match addon.xml: %s", strAddonExtensions.c_str());
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    if (SupportsVFS() != m_supportsVFS)
    {
      esyslog("CORE: VFS support doesn't match addon.xml: %s", SupportsVFS() ? "true" : "false");
      throw ADDON_STATUS_PERMANENT_FAILURE;
    }

    /* TODO
    // Initialize libretro's extended audio interface
    m_clientBridge.AudioEnable(true);
    */
  }
  catch (const ADDON_STATUS& status)
  {
    return status;
  }

  if (!CSettings::Get().IsInitialized())
    return ADDON_STATUS_NEED_SETTINGS;

  return ADDON_STATUS_OK;
}

ADDON_STATUS CGameLibRetro::SetSetting(const std::string& settingName, const kodi::addon::CSettingValue& settingValue)
{
  if (settingName == "" || settingValue.empty())
    return ADDON_STATUS_UNKNOWN;

  CSettings::Get().SetSetting(settingName, settingValue);
  CLibretroEnvironment::Get().SetSetting(settingName, settingValue.GetString());

  return ADDON_STATUS_OK;
}

GAME_ERROR CGameLibRetro::LoadGame(const std::string& url)
{
  // Build info loader vector
  SAFE_DELETE_GAME_INFO(m_gameInfo);
  m_gameInfo.push_back(new CGameInfoLoader(url, m_supportsVFS));

  bool bResult = false;

  // Try to load via memory
  retro_game_info gameInfo;
  if (m_gameInfo[0]->Load())
  {
    m_gameInfo[0]->GetMemoryStruct(gameInfo);
    bResult = m_client.retro_load_game(&gameInfo);
  }

  if (!bResult)
  {
    // Fall back to loading via path
    m_gameInfo[0]->GetPathStruct(gameInfo);
    bResult = m_client.retro_load_game(&gameInfo);
  }

  if (bResult)
  {
    CCheevos::Get().Initialize(this, url,
        [this](unsigned int type, uint8_t*& data, size_t& size) -> bool {
          data = static_cast<uint8_t*>(m_client.retro_get_memory_data(type));
          size = m_client.retro_get_memory_size(type);
          return data != nullptr && size > 0;
        });
    return GAME_ERROR_NO_ERROR;
  }
  return GAME_ERROR_FAILED;
}

GAME_ERROR CGameLibRetro::LoadGameSpecial(SPECIAL_GAME_TYPE type, const std::vector<std::string>& urls)
{
  // TODO
  return GAME_ERROR_FAILED;
  /*
  retro_system_info info = { };
  m_client.retro_get_system_info(&info);
  const bool bSupportsVFS = !info.need_fullpath;

  // Build info loader vector
  SAFE_DELETE_GAME_INFO(m_gameInfo);
  for (const auto& url : urls)
    m_gameInfo.push_back(new CGameInfoLoader(url, bSupportsVFS));

  // Try to load via memory
  std::vector<retro_game_info> infoVec;
  infoVec.resize(urls.size());
  bool bLoadFromMemory = true;
  for (unsigned int i = 0; bLoadFromMemory && i < urls.size(); i++)
    bLoadFromMemory &= m_gameInfo[i]->GetMemoryStruct(infoVec[i]);
  if (bLoadFromMemory)
  {
    if (m_client.retro_load_game_special(type, infoVec.data(), urls.size()))
      return GAME_ERROR_NO_ERROR;
  }

  // Fall back to loading by path
  for (unsigned int i = 0; i < urls.size(); i++)
    m_gameInfo[i]->GetPathStruct(infoVec[i]);
  bool result = m_client.retro_load_game_special(type, infoVec.data(), urls.size());

  return result ? GAME_ERROR_NO_ERROR : GAME_ERROR_FAILED;
  */
}

GAME_ERROR CGameLibRetro::LoadStandalone()
{
  if (!m_client.retro_load_game(nullptr))
    return GAME_ERROR_FAILED;

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::UnloadGame()
{
  GAME_ERROR error = GAME_ERROR_FAILED;

  CCheevos::Get().Deinitialize();
  m_client.retro_unload_game();

  CLibretroEnvironment::Get().CloseStreams();

  error = GAME_ERROR_NO_ERROR;

  SAFE_DELETE_GAME_INFO(m_gameInfo);

  return error;
}

GAME_ERROR CGameLibRetro::GetGameTiming(game_system_timing& timing_info)
{
  retro_system_av_info retro_info = { };
  m_client.retro_get_system_av_info(&retro_info);

  timing_info.fps = retro_info.timing.fps;
  timing_info.sample_rate = retro_info.timing.sample_rate;

  // Report info to CLibretroEnvironment
  CLibretroEnvironment::Get().UpdateVideoGeometry(retro_info.geometry);

  // The geometry the hardware rendering stream needs is now known, so bring the
  // core's context up before the frontend asks anything that depends on it.
  // Failing here fails the load: a core that asked for hardware rendering and
  // did not get it cannot be run, and this is the last point at which the
  // frontend will still abandon the game cleanly.
  if (!CLibretroEnvironment::Get().Video().OpenHwStream())
    return GAME_ERROR_FAILED;

  return GAME_ERROR_NO_ERROR;
}

GAME_REGION CGameLibRetro::GetRegion()
{
  return m_client.retro_get_region() == RETRO_REGION_NTSC ? GAME_REGION_NTSC : GAME_REGION_PAL;
}

GAME_ERROR CGameLibRetro::RunFrame()
{
  // Trigger the frame time callback before running the core.
  uint64_t current = m_timer.microseconds();
  int64_t delta = 0;

  if (m_frameTimeLast > 0)
    delta = current - m_frameTimeLast;

  m_frameTimeLast = current;
  m_clientBridge.FrameTime(delta);

  CLibretroEnvironment::Get().OnFrameBegin();

  // Logged once, to separate a core that is not being run at all from one that
  // runs but never presents a frame. A black picture looks the same either way.
  static bool bLoggedFirstRun = false;
  if (!bLoggedFirstRun)
  {
    bLoggedFirstRun = true;
    kodi::Log(ADDON_LOG_INFO, "Running the core's first frame");
  }

  m_client.retro_run();

  CCheevos::Get().DoFrame();

  CLibretroEnvironment::Get().OnFrameEnd();

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::Reset()
{
  m_client.retro_reset();

  return GAME_ERROR_NO_ERROR;
}

/*!
 * \Brief Notify a core about audio being available for writing
 *
 * This enables finer-grained audio control by allowing the frontend to
 * control when audio data is sent to the frontend.
 *
 * When this function is called, audio data should be written to the
 * frontend via the AddStreamData() Game API callback in the same thread
 * that invoked AudioAvailable().
 *
 * This extended interface is not recommended for use with emulators which
 * have highly synchronous audio.
 *
 * This function is not part of the Game API yet. It has been implemented
 * here in case a libretro core requires the extended audio interface.
 */
GAME_ERROR CGameLibRetro::AudioAvailable()
{
  return m_clientBridge.AudioAvailable();
}

GAME_ERROR CGameLibRetro::HwContextReset()
{
  return m_clientBridge.HwContextReset();
}

GAME_ERROR CGameLibRetro::HwContextDestroy()
{
  return m_clientBridge.HwContextDestroy();
}

bool CGameLibRetro::HasFeature(const std::string& controller_id, const std::string& feature_name)
{
  return CButtonMapper::Get().GetLibretroIndex(controller_id, feature_name) >= 0;
}

game_input_topology* CGameLibRetro::GetTopology()
{
  return CControllerTopology::GetInstance().GetTopology();
}

void CGameLibRetro::FreeTopology(game_input_topology* topology)
{
  CControllerTopology::FreeTopology(topology);
}

void CGameLibRetro::SetControllerLayouts(const std::vector<kodi::addon::GameControllerLayout>& controllers)
{
  CInputManager::Get().SetControllerLayouts(controllers);
}

bool CGameLibRetro::EnableKeyboard(bool enable, const std::string& controller_id)
{
  bool bSuccess = false;

  if (enable)
  {
    bSuccess = CInputManager::Get().EnableKeyboard(controller_id);
  }
  else
  {
    CInputManager::Get().DisableKeyboard();
    bSuccess = true;
  }

  return bSuccess;
}

bool CGameLibRetro::EnableMouse(bool enable, const std::string& controller_id)
{
  bool bSuccess = false;

  if (enable)
  {
    bSuccess = CInputManager::Get().EnableMouse(controller_id);
  }
  else
  {
    CInputManager::Get().DisableMouse();
    bSuccess = true;
  }

  return bSuccess;
}

bool CGameLibRetro::ConnectController(bool connect, const std::string& port_address, const std::string& controller_id)
{
  std::string strPortAddress(port_address);
  std::string strController;

  if (connect)
    strController = controller_id;

  int port = CInputManager::Get().GetPortIndex(strPortAddress);
  if (port < 0)
  {
    if (!CInputManager::Get().IsKeyboard(strPortAddress) && !CInputManager::Get().IsMouse(strPortAddress))
      esyslog("Failed to connect controller, invalid port address: %s", strPortAddress.c_str());
  }
  else
  {
    libretro_device_t device = RETRO_DEVICE_NONE;

    if (connect)
    {
      device = CInputManager::Get().ConnectController(strPortAddress, controller_id);
    }
    else
    {
      CInputManager::Get().DisconnectController(strPortAddress);
    }

    // Get connection port override if specified
    int connectionPort = -1;
    if (CInputManager::Get().GetConnectionPortIndex(strPortAddress, connectionPort))
      port = connectionPort;

    if (port >= 0)
    {
      dsyslog("Setting port \"%s\" (libretro port %d) to controller \"%s\" (libretro device ID %u)",
          strPortAddress.c_str(), port, strController.c_str(), device);

      m_client.retro_set_controller_port_device(port, device);
    }
    else
    {
      dsyslog("Ignoring port \"%s\" with controller \"%s\" (libretro device ID %u)",
          strPortAddress.c_str(), strController.c_str(), device);
    }

    return true;
  }

  return false;
}

bool CGameLibRetro::InputEvent(const game_input_event& event)
{
  return CInputManager::Get().InputEvent(event);
}

size_t CGameLibRetro::SerializeSize()
{
  const size_t coreSize = m_client.retro_serialize_size();
  if (coreSize == 0)
    return 0;

  // Savestates keep their original layout when achievements aren't in use, so
  // nothing changes for players who don't use them
  if (!CCheevos::Get().IsActive())
  {
    kodi::Log(ADDON_LOG_DEBUG, "Savestate size: %zu bytes (achievements inactive)", coreSize);
    return coreSize;
  }

  const size_t total = coreSize + PROGRESS_RESERVE_BYTES + FOOTER_SIZE;

  // Logged because this figure is multiplied by the rewind buffer's frame
  // count, so it is worth being able to see what it actually is
  kodi::Log(ADDON_LOG_INFO,
            "Savestate size: %zu bytes (emulator %zu + achievement progress %zu + footer %zu)",
            total, coreSize, PROGRESS_RESERVE_BYTES, FOOTER_SIZE);

  return total;
}

GAME_ERROR CGameLibRetro::Serialize(uint8_t* data, size_t size)
{
  if (data == nullptr)
    return GAME_ERROR_INVALID_PARAMETERS;

  const size_t coreSize = m_client.retro_serialize_size();
  if (coreSize == 0 || size < coreSize)
    return GAME_ERROR_INVALID_PARAMETERS;

  if (!m_client.retro_serialize(data, coreSize))
    return GAME_ERROR_FAILED;

  // The buffer was sized by an earlier SerializeSize() call, so the room for
  // progress is whatever is left over rather than something to rely on. If the
  // runtime grew in between, SerializeProgress() reports 0 and the savestate
  // degrades to emulator state alone rather than overrunning the buffer.
  if (size >= coreSize + FOOTER_SIZE)
  {
    const size_t written =
        CCheevos::Get().SerializeProgress(data + coreSize, size - coreSize - FOOTER_SIZE);

    SavestateFooter footer{};
    std::memcpy(footer.magic, SAVESTATE_MAGIC, sizeof(footer.magic));
    footer.version = SAVESTATE_VERSION;
    footer.coreSize = coreSize;
    footer.progressSize = written;

    std::memcpy(data + size - FOOTER_SIZE, &footer, FOOTER_SIZE);
  }

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::Deserialize(const uint8_t* data, size_t size)
{
  if (data == nullptr || size == 0)
    return GAME_ERROR_INVALID_PARAMETERS;

  size_t coreSize = size;
  size_t progressSize = 0;

  if (size > FOOTER_SIZE)
  {
    SavestateFooter footer{};
    std::memcpy(&footer, data + size - FOOTER_SIZE, FOOTER_SIZE);

    // Every field is checked before it is trusted: this blob may have been
    // written by an older build or not by us at all. The lengths are bounds
    // checked against the blob rather than against retro_serialize_size(), so
    // that a savestate written by a different version of the core is still
    // handed the emulator state on its own instead of the whole blob - the
    // core is in a better position to decide whether it can load it.
    if (std::memcmp(footer.magic, SAVESTATE_MAGIC, sizeof(footer.magic)) == 0 &&
        footer.version == SAVESTATE_VERSION && footer.coreSize <= size - FOOTER_SIZE &&
        footer.progressSize <= size - FOOTER_SIZE - footer.coreSize)
    {
      coreSize = static_cast<size_t>(footer.coreSize);
      progressSize = static_cast<size_t>(footer.progressSize);

      if (coreSize != m_client.retro_serialize_size())
      {
        kodi::Log(ADDON_LOG_WARNING,
                  "Savestate holds %zu bytes of emulator state but the core now reports %zu; "
                  "letting the core decide whether it can load it",
                  coreSize, m_client.retro_serialize_size());
      }
    }
  }

  if (!m_client.retro_unserialize(data, coreSize))
    return GAME_ERROR_FAILED;

  // Restoring progress is best-effort. A savestate from a session that wasn't
  // logged in, or one written by an older build, just leaves the achievement
  // runtime where it is - the emulator state is still restored.
  if (progressSize > 0)
    CCheevos::Get().DeserializeProgress(data + coreSize, progressSize);

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::CheatReset()
{
  m_client.retro_cheat_reset();

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::GetMemory(GAME_MEMORY type, uint8_t*& data, size_t& size)
{
  data = static_cast<uint8_t*>(m_client.retro_get_memory_data(type));
  size = m_client.retro_get_memory_size(type);

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::SetCheat(unsigned int index, bool enabled, const std::string& code)
{
  m_client.retro_cheat_set(index, enabled, code.c_str());

  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::RCGenerateHashFromFile(std::string& hash,
                                                    unsigned int consoleID,
                                                    const std::string& filePath)
{
  // Handled internally by rc_client
  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::RCGetGameIDUrl(std::string& url, const std::string& hash)
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::RCGetPatchFileUrl(std::string& url,
                                               const std::string& username,
                                               const std::string& token,
                                               unsigned int gameID)
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::SetRetroAchievementsCredentials(const std::string& username,
                                                             const std::string& token)
{
  CCheevos::Get().SetCredentials(username, token);
  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::RCPostRichPresenceUrl(std::string& url,
                                                   std::string& postData,
                                                   const std::string& username,
                                                   const std::string& token,
                                                   unsigned int gameID,
                                                   const std::string& richPresence)
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::RCEnableRichPresence(const std::string& script)
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::RCGetRichPresenceEvaluation(std::string& evaluation,
                                                      unsigned int consoleID)
{
  // Handled internally by rc_client, which pushes rich presence to the
  // frontend through RCOnRichPresenceUpdated. Reported as not implemented so
  // that a frontend polling this doesn't read the empty string as a genuine
  // evaluation, matching the other legacy RetroAchievements entry points.
  evaluation.clear();

  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::ActivateAchievement(unsigned cheevo_id,
                                                  const std::string& memAddrExpression)
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}


GAME_ERROR CGameLibRetro::GetCheevoUrlId(
    const std::function<void(const std::string& achievementUrl, unsigned int cheevoId)>& callback)
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}

GAME_ERROR CGameLibRetro::RCSetHardcoreEnabled(bool enabled)
{
  CCheevos::Get().SetHardcoreEnabled(enabled);
  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::RCSetEncoreModeEnabled(bool enabled)
{
  CCheevos::Get().SetEncoreModeEnabled(enabled);
  return GAME_ERROR_NO_ERROR;
}

GAME_ERROR CGameLibRetro::RCResetRuntime()
{
  return GAME_ERROR_NOT_IMPLEMENTED;
}

bool CGameLibRetro::GetEjectState()
{
  return m_clientBridge.GetEjectState();
}

GAME_ERROR CGameLibRetro::SetEjectState(bool ejected)
{
  return m_clientBridge.SetEjectState(ejected);
}

unsigned int CGameLibRetro::GetImageIndex()
{
  return m_clientBridge.GetImageIndex();
}

GAME_ERROR CGameLibRetro::SetImageIndex(unsigned int imageIndex)
{
  return m_clientBridge.SetImageIndex(imageIndex);
}

unsigned int CGameLibRetro::GetImageCount()
{
  return m_clientBridge.GetImageCount();
}

GAME_ERROR CGameLibRetro::AddImageIndex()
{
  return m_clientBridge.AddImageIndex();
}

GAME_ERROR CGameLibRetro::ReplaceImageIndex(unsigned int imageIndex, const std::string& filePath)
{
  return m_clientBridge.ReplaceImageIndex(imageIndex, filePath);
}

GAME_ERROR CGameLibRetro::RemoveImageIndex(unsigned int imageIndex)
{
  return m_clientBridge.RemoveImageIndex(imageIndex);
}

GAME_ERROR CGameLibRetro::SetInitialImage(unsigned int imageIndex, const std::string& filePath)
{
  return m_clientBridge.SetInitialImage(imageIndex, filePath);
}

std::string CGameLibRetro::GetImagePath(unsigned int imageIndex)
{
  return m_clientBridge.GetImagePath(imageIndex);
}

std::string CGameLibRetro::GetImageLabel(unsigned int imageIndex)
{
  return m_clientBridge.GetImageLabel(imageIndex);
}

ADDONCREATOR(CGameLibRetro)
