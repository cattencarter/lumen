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
 * Lumen keeps its things in one top-level inventory folder, and until
 * 2026-09-17 that folder was called **`#Firestorm`**.
 *
 * Five features share it -- the AO engine, the particle editor, wearable
 * favourites, the inventory protections and the LSL bridge -- so the name was
 * inherited rather than chosen, and it sat at the top of the user's inventory
 * saying Firestorm in a viewer that is not Firestorm. Decisions 5 is about
 * exactly this, and the author's instruction was plain: *"we should not use
 * their name if we can avoid it, to not make them mad."*
 *
 * **Renaming the constant alone would have been silent data loss.** Every one
 * of those five features looks the folder up by name and *creates it if it is
 * missing*. Point them at `#Lumen` on a machine that has a `#Firestorm` and
 * they each make a fresh empty one -- and the AO engine then finds no `#AO`
 * inside it and reports the user's whole animation-override setup as absent.
 * Nothing errors. The configuration is still on Linden Lab's servers, in a
 * folder nobody is looking at any more. That is this project's oldest failure
 * shape (Findings 49): nothing breaks, something is merely not there.
 */
namespace LumenFolders
{
    /** What the folder was called before 2026-09-17. */
    extern const std::string LEGACY_ROOT_FOLDER;

    /**
     * Rename a leftover `#Firestorm` folder to `#Lumen`, once, in place.
     *
     * A **rename**, deliberately, not a create-and-move: one operation, the
     * folder keeps its id, and everything inside it -- the AO, saved particle
     * scripts, wearable favourites, the bridge folder -- comes along without
     * being touched. Moving items one at a time would be dozens of round trips
     * and a half-migrated inventory if any of them failed.
     *
     * Safe to call more than once and on a fresh account: it does nothing when
     * `#Lumen` already exists, and nothing when there is no `#Firestorm`.
     *
     * Must run after `gInventory.isInventoryUsable()`, or it will find neither
     * folder and conclude there is nothing to do -- which would be the silent
     * failure this exists to prevent.
     */
    void migrateRootFolder();
}

#endif // LUMEN_FOLDERS_H
