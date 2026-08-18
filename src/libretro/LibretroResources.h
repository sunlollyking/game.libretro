/*
 *  Copyright (C) 2016-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

class CGameLibRetro;

namespace LIBRETRO
{
  class CLibretroResources
  {
  public:
    CLibretroResources();
    ~CLibretroResources() { Deinitialize(); }

    void Initialize(CGameLibRetro* addon);
    void Deinitialize();

    const char* GetSystemDir() const { return m_systemDirectory.c_str(); }
    const char* GetContentDirectory() { return GetSystemDir(); } // Use system directory
    const char* GetSaveDirectory() const { return m_saveDirectory.c_str(); }

    const char* GetBasePath(const std::string& relPath);
    const char* GetBaseSystemPath(const std::string& relPath);
    std::string GetFullPath(const std::string& relPath);
    std::string GetFullSystemPath(const std::string& relPath);

    /*!
     * \brief Resolve a path a core is opening against the system layers
     *
     * A core is given one system directory but the frontend offers several
     * layers behind it. Anything the core opens underneath that directory is
     * looked for in each layer in turn, so the file it gets is the one the
     * layering says it should, resolved at the moment of the open rather than
     * staged on disk beforehand.
     *
     * \param path The absolute path the core asked for
     *
     * \return The path to open instead, or \p path unchanged if it is not
     *         under the system directory or no layer has it
     */
    std::string ResolveSystemPath(const std::string& path);

  private:
    const char* ApendSystemFolder(const std::string& path);

    /*!
     * \brief Resolve a resource directory to the system directory inside it
     *
     * A resource directory is normally a parent holding a "system" subfolder.
     * A layer that is already a system directory -- the frontend's shared BIOS
     * folder -- is used as it stands, so both shapes can be layered together.
     */
    static std::string ResolveSystemLayer(const std::string& resourceDirectory);

    /*!
     * \brief Collapse the system layers into the one directory a core is given
     *
     * libretro asks for a single system directory, so the layers have to be
     * merged before the core sees them. Earlier layers win: the first copy of
     * a given relative path is the one that survives.
     *
     * \param layers System directories, highest priority first
     * \param target Directory to build the merged view in
     */
    static void MergeSystemLayers(const std::vector<std::string>& layers,
                                  const std::string& target);

    //! \brief Copy one layer over the merged view, without displacing what's there
    static void MergeLayer(const std::string& layer,
                           const std::string& target,
                           const std::string& relPath,
                           unsigned int& filesLinked,
                           unsigned int& filesCopied);

    /*!
     * \brief Put a layer's file into the merged view
     *
     * Symlinked where the platform allows it, copied where it does not.
     * \return true if the file is now present in the merged view
     */
    static bool LinkOrCopy(const std::string& source,
                           const std::string& destination,
                           unsigned int& filesLinked,
                           unsigned int& filesCopied);

    /*!
     * \brief Whether a name in the merged view already holds a usable file
     *
     * Clears the name first if what's there is a symlink whose target has gone,
     * which reads as absent but still occupies the name.
     */
    static bool HasUsableFile(const std::string& path);

    CGameLibRetro*                     m_addon;
    std::vector<std::string>           m_resourceDirectories;
    std::map<std::string, std::string> m_pathMap;
    std::string                        m_systemDirectory;
    std::string                        m_saveDirectory;

    //! \brief The system directories behind m_systemDirectory, highest priority first
    std::vector<std::string>           m_systemLayers;
  };
} // namespace LIBRETRO
