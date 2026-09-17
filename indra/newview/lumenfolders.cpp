/**
 * @file lumenfolders.cpp
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

#include "llviewerprecompiledheaders.h"

#include "lumenfolders.h"

#include "llinventorymodel.h"
#include "llinventoryfunctions.h"   // ROOT_FIRESTORM_FOLDER (now "#Lumen"), rename_category
#include "llviewerinventory.h"

namespace LumenFolders
{
    const std::string LEGACY_ROOT_FOLDER = "#Firestorm";
}

void LumenFolders::migrateRootFolder()
{
    if (!gInventory.isInventoryUsable())
    {
        // Saying so rather than returning quietly. A migration that decided
        // there was nothing to migrate because it ran too early looks exactly
        // like a migration that worked.
        LL_WARNS("LumenFolders") << "Asked to migrate the root folder before inventory was usable; "
                                    "doing nothing. The AO and the bridge folder would not have "
                                    "been found either." << LL_ENDL;
        return;
    }

    // Already ours, or never theirs.
    const LLUUID ours = gInventory.findCategoryByName(ROOT_FIRESTORM_FOLDER);
    if (ours.notNull())
    {
        return;
    }

    const LLUUID theirs = gInventory.findCategoryByName(LEGACY_ROOT_FOLDER);
    if (theirs.isNull())
    {
        return;         // a fresh account; whoever needs it will create it
    }

    // One rename, in place. The folder keeps its id, so every link, every
    // observer and every saved reference to what is inside it stays valid --
    // which is the whole reason this is a rename and not a move.
    LL_INFOS("LumenFolders") << "Renaming inventory folder \"" << LEGACY_ROOT_FOLDER
                             << "\" to \"" << ROOT_FIRESTORM_FOLDER
                             << "\" (" << theirs << "); contents are untouched." << LL_ENDL;

    rename_category(&gInventory, theirs, ROOT_FIRESTORM_FOLDER);
}
