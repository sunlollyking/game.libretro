/*
 *  Copyright (C) 2016-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#include "LibretroResources.h"
#include "LibretroDefines.h"
#include "log/Log.h"

#include "client.h"

#include <kodi/Filesystem.h>
#include <assert.h>
#include <utility>

#if defined(TARGET_POSIX)
#include <sys/stat.h>
#include <unistd.h>
#endif

using namespace LIBRETRO;

CLibretroResources::CLibretroResources() :
  m_addon(nullptr)
{
}

void CLibretroResources::Initialize(CGameLibRetro* addon)
{
  m_addon = addon;

  assert(m_addon != nullptr);

  std::vector<std::string> dirs;
  m_addon->ResourceDirectories(dirs);

  // The frontend hands over its system layers in priority order: its shared
  // BIOS folder, then any game resource add-ons, then this add-on's own folder.
  // Only the first was ever used, because libretro asks for one system
  // directory and the rest of the list had nowhere to go -- so a file present
  // in any layer but the first was invisible to the core, and installing a
  // resource add-on hid the user's own files behind it.
  std::vector<std::string> systemLayers;
  for (const auto& dir : dirs)
  {
    if (dir.empty())
      continue;

    std::string layer = ResolveSystemLayer(dir);
    if (!layer.empty())
      systemLayers.push_back(std::move(layer));

    m_resourceDirectories.push_back(dir);
  }

  // Merge them into one directory and give the core that. Kept under this
  // add-on's own profile so each core gets a view built from the layers that
  // apply to it, and so nothing is written back into a layer.
  m_systemDirectory = m_addon->ProfileDirectory() + "/" LIBRETRO_MERGED_SYSTEM_DIRECTORY_NAME;

  if (!kodi::vfs::DirectoryExists(m_systemDirectory))
  {
    dsyslog("Creating system directory: %s", m_systemDirectory.c_str());
    kodi::vfs::CreateDirectory(m_systemDirectory);
  }

  MergeSystemLayers(systemLayers, m_systemDirectory);

  m_saveDirectory = m_addon->ProfileDirectory() + "/" LIBRETRO_SAVE_DIRECTORY_NAME;

  // Ensure folder exists
  if (!kodi::vfs::DirectoryExists(m_saveDirectory))
  {
    dsyslog("Creating save directory: %s", m_saveDirectory.c_str());
    kodi::vfs::CreateDirectory(m_saveDirectory);
  }

  // A core finds its BIOS, fonts and other data files under this, and one
  // pointed somewhere else fails in whatever way that core fails when its data
  // is missing -- which is rarely a message saying so. Worth a line each time a
  // core starts, given this used to be whichever core loaded first.
  kodi::Log(ADDON_LOG_INFO, "System directory: %s", m_systemDirectory.c_str());
}

void CLibretroResources::Deinitialize()
{
  // All of this belongs to the add-on that is going away, and this object is
  // reached through a process-wide singleton. Anything left behind is picked
  // up by the next core loaded in the same session: the system directory is
  // only set while empty, so the first core to load owned it for every core
  // after it, and a Saturn core went looking for its BIOS in the N64 add-on's
  // directory. The cached paths and resource directories carry the same way.
  m_addon = nullptr;
  m_systemDirectory.clear();
  m_saveDirectory.clear();
  m_resourceDirectories.clear();
  m_pathMap.clear();
}

const char* CLibretroResources::GetBasePath(const std::string& relPath)
{
  auto it = m_pathMap.find(relPath);

  if (it == m_pathMap.end())
  {
    for (const auto& dir : m_resourceDirectories)
    {
      std::string resourcePath = dir + "/" + relPath;

      // Check for path existence
      if (kodi::vfs::FileExists(resourcePath, true))
      {
        m_pathMap.insert(std::make_pair(relPath, std::move(dir)));
        it = m_pathMap.find(relPath);
        break;
      }
    }
  }

  if (it != m_pathMap.end())
    return it->second.c_str();

  return nullptr;
}

const char* CLibretroResources::GetBaseSystemPath(const std::string& relPath)
{
  std::string systemPath = LIBRETRO_SYSTEM_DIRECTORY_NAME "/" + relPath;
  const char* basePath = GetBasePath(systemPath);
  if (basePath != nullptr)
    return ApendSystemFolder(basePath);

  return nullptr;
}

std::string CLibretroResources::CLibretroResources::GetFullPath(const std::string& relPath)
{
  const char* basePath = GetBasePath(relPath);

  if (basePath != nullptr)
    return std::string(basePath) + "/" + relPath;

  return "";
}

std::string CLibretroResources::GetFullSystemPath(const std::string& relPath)
{
  const char* baseSystemPath = GetBaseSystemPath(relPath);

  if (baseSystemPath != nullptr)
    return std::string(baseSystemPath) + "/" + relPath;

  return "";
}

std::string CLibretroResources::ResolveSystemLayer(const std::string& resourceDirectory)
{
  // The established shape is a parent holding a "system" subfolder
  const std::string systemSubfolder =
      resourceDirectory + "/" LIBRETRO_SYSTEM_DIRECTORY_NAME;
  if (kodi::vfs::DirectoryExists(systemSubfolder))
    return systemSubfolder;

  // Otherwise the layer may be a system directory in its own right, which is
  // how the frontend's shared BIOS folder arrives. Checking rather than
  // assuming lets both shapes sit in the same list without the frontend having
  // to say which is which.
  //
  // Except for an add-on's own resources folder. The frontend builds those by
  // appending "resources" to a path, and they hold whatever the add-on ships --
  // language files, buttonmaps, settings.xml. Treating one as a system
  // directory because it has no "system" subfolder would copy all of that into
  // the directory the core reads its BIOSes from. Name the exception rather
  // than trusting every layer to be shaped correctly.
  const size_t lastSlash = resourceDirectory.find_last_of("/\\");
  const std::string folderName =
      lastSlash == std::string::npos ? resourceDirectory : resourceDirectory.substr(lastSlash + 1);

  if (folderName == LIBRETRO_CLIENT_RESOURCES_DIRECTORY_NAME)
    return "";

  if (kodi::vfs::DirectoryExists(resourceDirectory))
    return resourceDirectory;

  return "";
}

bool CLibretroResources::HasUsableFile(const std::string& path)
{
#if defined(TARGET_POSIX)
  // Decide this by whether the path resolves to something readable, not by
  // whether the name is taken. A symlink left over from a previous merge whose
  // target has since been removed still occupies the name: it has to count as
  // absent, or the core is handed a link that opens nothing -- and it has to be
  // cleared, or nothing can be created in its place.
  struct stat resolved;
  if (::stat(path.c_str(), &resolved) == 0)
    return true;

  struct stat entry;
  if (::lstat(path.c_str(), &entry) == 0)
    ::unlink(path.c_str());

  return false;
#else
  return kodi::vfs::FileExists(path, false);
#endif
}

bool CLibretroResources::LinkOrCopy(const std::string& source,
                                    const std::string& destination,
                                    unsigned int& filesLinked,
                                    unsigned int& filesCopied)
{
#if defined(TARGET_POSIX)
  // Link rather than duplicate. A BIOS collection is tens of megabytes and the
  // merged view is per client, so copying charges that against every core
  // installed -- on the sort of storage these run from, that adds up.
  //
  // A link also keeps what a core writes where it has always gone. Cores do
  // write into their system directory: flycast keeps dc_nvmem.bin there. Copied
  // into the merged view, that write lands somewhere rebuilt on a later load
  // and is lost. Through a link it reaches the layer the file came from, which
  // is what happened before any of this existed.
  //
  // Only for real paths. A layer reached through Kodi's VFS has no name the
  // system call could use.
  if (source.find("://") == std::string::npos && destination.find("://") == std::string::npos)
  {
    if (::symlink(source.c_str(), destination.c_str()) == 0)
    {
      ++filesLinked;
      return true;
    }
  }
#endif

  // Windows without the privilege for symlinks, an Android volume that has no
  // concept of them, a VFS layer: copy, and behave as before.
  if (kodi::vfs::CopyFile(source, destination))
  {
    ++filesCopied;
    return true;
  }

  return false;
}

void CLibretroResources::MergeSystemLayers(const std::vector<std::string>& layers,
                                           const std::string& target)
{
  unsigned int filesLinked = 0;
  unsigned int filesCopied = 0;

  for (const auto& layer : layers)
  {
    // Skip a layer that is the target, which would otherwise merge onto itself
    if (layer == target)
      continue;

    MergeLayer(layer, target, "", filesLinked, filesCopied);
  }

  if (filesLinked > 0 || filesCopied > 0)
    dsyslog("Merged %u linked and %u copied file(s) from %u system layer(s) into %s", filesLinked,
            filesCopied, static_cast<unsigned int>(layers.size()), target.c_str());
}

void CLibretroResources::MergeLayer(const std::string& layer,
                                    const std::string& target,
                                    const std::string& relPath,
                                    unsigned int& filesLinked,
                                    unsigned int& filesCopied)
{
  const std::string sourceDir = relPath.empty() ? layer : layer + "/" + relPath;

  std::vector<kodi::vfs::CDirEntry> items;
  if (!kodi::vfs::GetDirectory(sourceDir, "", items))
    return;

  for (const auto& item : items)
  {
    const std::string itemRelPath = relPath.empty() ? item.Label() : relPath + "/" + item.Label();
    const std::string destination = target + "/" + itemRelPath;

    if (item.IsFolder())
    {
      // Cores look for BIOSes under subfolders as often as not -- "dc/" and
      // "Machines/" among them -- so the merge has to walk the whole tree
      if (!kodi::vfs::DirectoryExists(destination))
        kodi::vfs::CreateDirectory(destination);

      MergeLayer(layer, target, itemRelPath, filesLinked, filesCopied);
      continue;
    }

    // A file already here came from a higher-priority layer, or is something
    // the core itself wrote. Either way it stays: the user's own copy has to be
    // able to override what a resource add-on ships, and a core's own file must
    // not be replaced underneath it.
    if (HasUsableFile(destination))
      continue;

    if (!LinkOrCopy(item.Path(), destination, filesLinked, filesCopied))
      esyslog("Failed to merge %s into the system directory", item.Path().c_str());
  }
}

const char* CLibretroResources::ApendSystemFolder(const std::string& path)
{
  static std::map<std::string, std::string> pathCache;

  auto it = pathCache.find(path);
  if (it == pathCache.end())
  {
    const std::string systemPath = path + "/" LIBRETRO_SYSTEM_DIRECTORY_NAME;

    pathCache.insert(std::make_pair(path, std::move(systemPath)));
    it = pathCache.find(path);
  }

  if (it != pathCache.end())
    return it->second.c_str();

  return nullptr;

}
