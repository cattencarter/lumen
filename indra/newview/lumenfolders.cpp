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
#include "llinventoryfunctions.h"   // rename_category
#include "llviewerinventory.h"

namespace LumenFolders
{
    const std::string FIRESTORM_FOLDER = "#Firestorm";
    const std::string LUMEN_FOLDER     = "#Lumen";
}

namespace
{
    // The AO engine's own folder name (aoengine.cpp, ROOT_AO_FOLDER), and the
    // bridge's (fslslbridge.cpp, FS_BRIDGE_FOLDER). Both are file-static
    // upstream, so they are spelled out here.
    const std::string AO_FOLDER     = "#AO";
    const std::string BRIDGE_FOLDER = "#LSL Bridge";

    bool               sDecided = false;
    const std::string* sShared  = &LumenFolders::FIRESTORM_FOLDER;

    LLUUID childNamed(const LLUUID& parent, const std::string& name)
    {
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(parent, cats, items);
        if (cats)
        {
            for (const auto& cat : *cats)
            {
                if (cat && cat->getName() == name) return cat->getUUID();
            }
        }
        return LLUUID::null;
    }

    /**
     * How many things are in this root's AO, and whether that is known.
     *
     * The AO engine makes an empty `#AO` by itself, so the folder being there
     * says nothing; animations inside it say somebody set one up. A folder whose
     * contents have not arrived yet counts as not known, and not known never
     * moves anything -- the next login looks again.
     */
    S32 aoItems(const LLUUID& root, bool& known)
    {
        known = true;
        const LLUUID ao = childNamed(root, AO_FOLDER);
        if (ao.isNull()) return 0;
        LLViewerInventoryCategory* top = gInventory.getCategory(ao);
        if (!top || top->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
        {
            known = false;
            return 0;
        }
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(ao, cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const auto& cat : cats)
        {
            if (cat->getVersion() == LLViewerInventoryCategory::VERSION_UNKNOWN)
            {
                known = false;
            }
        }
        return (S32)items.size();
    }

    /**
     * Give a folder an earlier Lumen renamed its old name back, and move Lumen's
     * bridge out of it into a `#Lumen` of its own.
     *
     * The bridge FOLDER is moved rather than the bridge rebuilt: the bridge
     * proves itself to the viewer with that folder's id, and a move keeps it.
     */
    void restore(const LLUUID& folder)
    {
        const LLUUID bridge = childNamed(folder, BRIDGE_FOLDER);

        LL_INFOS("LumenFolders") << "Giving \"" << LumenFolders::LUMEN_FOLDER
                                 << "\" its old name \"" << LumenFolders::FIRESTORM_FOLDER
                                 << "\" back (" << folder << "), so Firestorm finds its AO "
                                 << "again; contents are untouched." << LL_ENDL;
        rename_category(&gInventory, folder, LumenFolders::FIRESTORM_FOLDER);
        // Locally too, now: rename_category goes through AIS and the local
        // model learns the name only when the reply lands, while the AO and
        // the bridge look folders up by name in the meantime.
        if (LLViewerInventoryCategory* cat = gInventory.getCategory(folder))
        {
            LLPointer<LLViewerInventoryCategory> renamed = new LLViewerInventoryCategory(cat);
            renamed->rename(LumenFolders::FIRESTORM_FOLDER);
            gInventory.updateCategory(renamed);
        }

        if (bridge.isNull())
        {
            return;   // the bridge makes its own #Lumen when it next starts
        }
        gInventory.createNewCategory(gInventory.getRootFolderID(), LLFolderType::FT_NONE,
                                     LumenFolders::LUMEN_FOLDER,
            [bridge](const LLUUID& new_id)
            {
                LLViewerInventoryCategory* cat = gInventory.getCategory(bridge);
                if (!cat || new_id.isNull())
                {
                    return;
                }
                LL_INFOS("LumenFolders") << "Moving Lumen's bridge folder into its own \""
                                         << LumenFolders::LUMEN_FOLDER << "\"." << LL_ENDL;
                gInventory.changeCategoryParent(cat, new_id, false);
            });
    }

    /**
     * The rules, once a session:
     *   - both folders: `#Firestorm` -- unless only `#Lumen` holds a real AO
     *     (somebody an earlier Lumen renamed, who then went back to Firestorm
     *     and got a new empty one: Whisper). Their Lumen keeps its AO; moving
     *     it across is left to them, because merging two AOs is not a guess to
     *     make on somebody's behalf.
     *   - only `#Firestorm`: that.
     *   - only `#Lumen` holding a real AO: an earlier Lumen renamed it. Give it
     *     its name back (restore) and share it from now on.
     *   - only `#Lumen`, or neither: `#Lumen` -- never used Firestorm, or
     *     nothing to share yet.
     */
    void decide()
    {
        if (sDecided || !gInventory.isInventoryUsable())
        {
            return;
        }
        sDecided = true;

        const LLUUID fs = gInventory.findCategoryByName(LumenFolders::FIRESTORM_FOLDER);
        const LLUUID lu = gInventory.findCategoryByName(LumenFolders::LUMEN_FOLDER);

        if (fs.notNull() && lu.notNull())
        {
            bool fs_known = false, lu_known = false;
            const S32 fs_ao = aoItems(fs, fs_known);
            const S32 lu_ao = aoItems(lu, lu_known);
            const bool keep_lumen = fs_known && lu_known && lu_ao > 0 && fs_ao == 0;
            sShared = keep_lumen ? &LumenFolders::LUMEN_FOLDER : &LumenFolders::FIRESTORM_FOLDER;
            LL_INFOS("LumenFolders") << "Both folders exist; AO items: " << LumenFolders::FIRESTORM_FOLDER
                                     << " " << fs_ao << (fs_known ? "" : " (not all loaded)") << ", "
                                     << LumenFolders::LUMEN_FOLDER << " " << lu_ao
                                     << (lu_known ? "" : " (not all loaded)") << ". Using "
                                     << *sShared << "." << LL_ENDL;
        }
        else if (fs.notNull())
        {
            sShared = &LumenFolders::FIRESTORM_FOLDER;
        }
        else if (lu.notNull())
        {
            bool known = false;
            const S32 ao = aoItems(lu, known);
            if (known && ao > 0)
            {
                restore(lu);
                sShared = &LumenFolders::FIRESTORM_FOLDER;
            }
            else
            {
                sShared = &LumenFolders::LUMEN_FOLDER;
                if (!known)
                {
                    LL_INFOS("LumenFolders") << "\"" << LumenFolders::LUMEN_FOLDER << "\"'s AO has not "
                                             << "loaded yet; using it as it is, and looking again next "
                                             << "login." << LL_ENDL;
                }
            }
        }
        else
        {
            sShared = &LumenFolders::LUMEN_FOLDER;
        }
        LL_INFOS("LumenFolders") << "Shared folder: " << *sShared << LL_ENDL;
    }
}

const std::string& LumenFolders::sharedRoot()
{
    decide();
    return *sShared;
}

void LumenFolders::migrateRootFolder()
{
    if (!gInventory.isInventoryUsable())
    {
        // Saying so rather than returning quietly: deciding too early would
        // find neither folder and settle on the wrong one.
        LL_WARNS("LumenFolders") << "Asked to decide the shared folder before inventory was usable; "
                                    "the first lookup after it is will decide instead." << LL_ENDL;
        return;
    }
    decide();
}
