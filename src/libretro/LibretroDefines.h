/*
 *  Copyright (C) 2016-2021 Team Kodi (https://kodi.tv)
 *
 *  SPDX-License-Identifier: GPL-2.0-or-later
 *  See LICENSE.md for more information.
 */

#pragma once

/*!
 * \brief The "system" directory of the frontend
 *
 * This directory can be used to store system specific content such as BIOSes,
 * configuration data, etc.
 */
#define LIBRETRO_SYSTEM_DIRECTORY_NAME  "system"

/*!
 * \brief The directory the frontend's system layers are merged into
 *
 * libretro asks for a single system directory, so the layers the frontend
 * offers -- its shared BIOS folder, game resource add-ons, this add-on's own
 * folder -- are collapsed into one directory and the core is given that.
 */
#define LIBRETRO_MERGED_SYSTEM_DIRECTORY_NAME  "system-merged"

/*!
 * \brief The name the frontend gives a client's own resources folder
 *
 * Such a layer holds whatever the add-on ships, so it is only a source of
 * system files through the "system" subfolder inside it, never in its own
 * right.
 */
#define LIBRETRO_CLIENT_RESOURCES_DIRECTORY_NAME  "resources"

/*!
 * \brief The "save" directory of the frontend
 *
 * This directory can be used to store SRAM, memory cards, high scores, etc,
 * if the libretro core cannot use the regular memory interface
 * retro_get_memory_data().
 */
#define LIBRETRO_SAVE_DIRECTORY_NAME  "save"
