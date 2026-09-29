/**
 * @file lumenfolders.h
 * @brief The one inventory folder Lumen makes, and how it stopped being theirs.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Lumen Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */
#ifndef LUMEN_FOLDERS_H
#define LUMEN_FOLDERS_H

#include <string>

/**
 * Which top-level inventory folder the features Lumen shares with Firestorm use.
 *
 * **Until 2026-09-30 Lumen renamed the user's `#Firestorm` folder to `#Lumen`**
 * (Decisions 117), so its own name was not at the top of somebody's inventory.
 * It kept the AO working in Lumen and broke it in Firestorm: the next time the
 * person used Firestorm it found no `#Firestorm`, made a new empty one, and
 * their AO looked gone -- and renaming it back only lasted until the next Lumen
 * login. The author, seeing both folders on Whisper's account: *"why would we
 * touch a folder firestorm made in the first place?"*
 *
 * So now:
 *   - the AO, wearable favourites, the particle editor and the inventory
 *     protections use **`#Firestorm` when it exists**, shared with Firestorm,
 *     so switching viewers costs nothing;
 *   - somebody who has never used Firestorm gets **`#Lumen`** for them instead;
 *   - Lumen's own LSL bridge always lives in **`#Lumen`** -- the one thing in
 *     there that is really Lumen's (fslslbridge.cpp uses LUMEN_FOLDER).
 *
 * ROOT_FIRESTORM_FOLDER (llinventoryfunctions.h) expands to sharedRoot(), so
 * those files keep their upstream text.
 */
namespace LumenFolders
{
    extern const std::string FIRESTORM_FOLDER;   // "#Firestorm"
    extern const std::string LUMEN_FOLDER;       // "#Lumen" -- Lumen's bridge

    /**
     * `#Firestorm` or `#Lumen`, decided once per session the first time it is
     * asked after inventory is usable (see decide() in lumenfolders.cpp for the
     * rules). Before that it answers `#Firestorm`, upstream's own value.
     */
    const std::string& sharedRoot();

    /**
     * Called at STATE_INVENTORY_CALLBACKS. Makes the decision above, and for an
     * account an earlier Lumen renamed -- a `#Lumen` holding a real AO, and no
     * `#Firestorm` -- gives the folder its old name back once and moves Lumen's
     * bridge out into a `#Lumen` of its own.
     */
    void migrateRootFolder();
}

#endif // LUMEN_FOLDERS_H
