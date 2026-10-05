/**
 * @file lumenaiundo.cpp
 * @brief Undo and redo for the inventory, this login: the assistant's changes and the person's.
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

#include "lumenaiundo.h"

#include "aoengine.h"           // <Lumen> the AO folder, for the shared rules
#include "fsfloaterwearablefavorites.h"   // <Lumen> the wearable favourites folder, likewise
#include "fslslbridge.h"        // <Lumen> the LSL bridge's folder, likewise
#include "llagent.h"
#include "llapp.h"
#include "llappviewer.h"        // <Lumen> gDisconnected: logged out, the viewer still open
#include "llcallbacklist.h"     // <Lumen> the frame: what the observer saw is looked at there
#include "llcoros.h"
#include "lleventcoro.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"   // <Lumen> the person's changes count once it has all loaded
#include "llinventoryobserver.h"
#include "llmenugl.h"           // <Lumen> the gear menu's Undo and Redo name themselves
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lltrans.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerfoldertype.h"   // <Lumen> "New Folder"
#include "llviewerinventory.h"
#include "lumenfolders.h"
#include "rlvactions.h"
#include "rlvdefines.h"
#include "rlvinventory.h"   // <Lumen> RLV's shared folder is its own housekeeping
#include "rlvlocks.h"

#include <algorithm>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    // At most this many steps: one is a whole action, so thirty covers a long
    // tidying session -- and ten could be used up by single moves before a
    // bad batch was noticed. The oldest drops off the end. (The author,
    // 2026-10-04.)
    const size_t MAX_STEPS = 30;

    // The person's changes this close together, of the same kind and to the
    // same place, are one step: one drag of twenty items, one delete of a
    // multiple selection.
    const F64 GROUP_SECONDS = 2.0;
    // A folder the person has just made, renamed within this long, is one
    // step: "New Folder" and then its name.
    const F64 NAMED_SOON = 120.0;

    // How long a claim waits for the change it announced. A rename under
    // load can take a while to come back.
    const F64 CLAIM_KEEPS = 300.0;

    // Above this many steps a run is paced in a coroutine rather than done in
    // the call: thousands of moves in one frame are thousands of requests to
    // Second Life at once.
    const size_t RUN_NOW_LIMIT = 100;

    // A reply carries at most this many names per list, with the full count.
    const S32 LIST_CAP = 30;

    // How many things the question names, and how many it leaves to "and N more".
    const S32 SHOWN = 5;

    // <Lumen> How long an undo waits for a folder it asked Second Life to make
    // again before it reports without it; the same as a bulk run waits.
    const F64 FOLDER_WAIT = 30.0;

    // ---- the inventory as it is now -----------------------------------------

    bool exists(const LLUUID& id)
    {
        return gInventory.getItem(id) || gInventory.getCategory(id);
    }

    LLUUID parentOf(const LLUUID& id)
    {
        if (LLViewerInventoryItem* i = gInventory.getItem(id)) return i->getParentUUID();
        if (LLViewerInventoryCategory* c = gInventory.getCategory(id)) return c->getParentUUID();
        return LLUUID::null;
    }

    std::string nameOf(const LLUUID& id)
    {
        if (LLViewerInventoryItem* i = gInventory.getItem(id)) return i->getName();
        if (LLViewerInventoryCategory* c = gInventory.getCategory(id)) return c->getName();
        return std::string();
    }

    LLUUID trashId()
    {
        // <Lumen> Once a frame: the viewer finds it by scanning the top of the
        // inventory, and a plan asks for every thing it looks at -- with
        // thousands of product folders delivered to the top, that was most of
        // an undo's plan.
        static LLUUID trash;
        static U32 frame = 0;
        const U32 now = LLFrameTimer::getFrameCount();
        if (trash.isNull() || frame != now)
        {
            trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
            frame = now;
        }
        return trash;
    }

    bool within(const LLUUID& id, const LLUUID& top)
    {
        return top.notNull() && (id == top || gInventory.isObjectDescendentOf(id, top));
    }

    bool inTrash(const LLUUID& id)
    {
        return within(id, trashId());
    }

    /** A folder something can be put back into: there, and not in the Trash. */
    bool liveFolder(const LLUUID& id)
    {
        return id.notNull() && gInventory.getCategory(id) && !inTrash(id);
    }

    std::string pathOf(const LLUUID& folder)
    {
        if (folder.isNull()) return std::string();
        if (folder == gInventory.getRootFolderID()) return "(top of inventory)";
        std::vector<std::string> parts;
        LLUUID id = folder;
        const LLUUID root = gInventory.getRootFolderID();
        for (S32 guard = 0; guard < 32 && id.notNull() && id != root; ++guard)
        {
            LLViewerInventoryCategory* c = gInventory.getCategory(id);
            if (!c) break;
            parts.push_back(c->getName());
            id = c->getParentUUID();
        }
        std::string out;
        for (auto it = parts.rbegin(); it != parts.rend(); ++it)
        {
            if (!out.empty()) out += "/";
            out += *it;
        }
        return out;
    }

    void addCapped(LLSD& list, const LLSD& entry)
    {
        if (!list.isArray()) list = LLSD::emptyArray();
        if ((S32)list.size() < LIST_CAP) list.append(entry);
    }

    LLSD entry(const std::string& name, const std::string& text, const char* key = "why")
    {
        LLSD e;
        e["name"] = name;
        e[key] = text;
        return e;
    }

    // <Lumen>
    /** Logged out -- or disconnected, the viewer still open behind its message. */
    bool offline()
    {
        return gDisconnected || !gAgent.getRegion();
    }
    // </Lumen>
}


// =============================================================================
// <Lumen> The rules: what the assistant may move, rename, or put somewhere.
// One copy, for every path; see the header.
// =============================================================================

namespace
{
    std::string refused(LumenInventoryRules::Rule* out, LumenInventoryRules::Rule r, const std::string& why)
    {
        if (out) *out = r;
        return why;
    }

    // <Lumen> Said to the person in the end, so in their words and with the
    // way out: the menu item is "Unprotect" on the folder's right-click menu.
    const char* const UNPROTECT = ", a folder you protected (right-click > Unprotect to change that)";
}

std::string LumenInventoryRules::offLimits(const LLUUID& id, const char* it, Rule* rule)
{
    if (rule) *rule = ALLOWED;
    const std::string s(it ? it : "it");
    if (id.isNull() || !gInventory.getObject(id))
        return refused(rule, NOT_IN_INVENTORY, s + " is not in their inventory");
    // The Library is the one thing outside their own root.
    if (!within(id, gInventory.getRootFolderID()))
        return refused(rule, LIBRARY, s + " is in the Library, which is Linden Lab's, not theirs");
    if (within(id, gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT)))
        return refused(rule, CURRENT_OUTFIT, s + " is in Current Outfit, which is what they are wearing");
    if (within(id, FSLSLBridge::instance().getBridgeFolder()))
        return refused(rule, LSL_BRIDGE, s + " is the LSL bridge's, which the viewer needs where it is");
    if (within(id, AOEngine::instance().getAOFolder()))
        return refused(rule, ANIMATION_OVERRIDER, s + " belongs to their animation overrider, which finds "
                                                      "things by where they are");
    // <Lumen> The third folder the viewer locks, whatever its Lock setting
    // says: what is in it is what the wearable favourites window shows.
    if (within(id, FSFloaterWearableFavorites::getFavoritesFolder()))
        return refused(rule, WEARABLE_FAVORITES, s + " is in their wearable favourites, a folder the viewer "
                                                     "keeps for its own window");
    // <Lumen> The viewer's own folders inside #Lumen and #Firestorm (#AO,
    // #LSL Bridge, #Wearable Favorites ...) are found by NAME, and the ones a
    // feature is not using today -- a leftover #AO under the root that is not
    // the shared one -- are found again the day it is. All of them, in use or
    // not, and what is in them.
    {
        LLUUID walk = id;
        for (S32 guard = 0; guard < 64 && walk.notNull(); ++guard)
        {
            LLViewerInventoryCategory* c = gInventory.getCategory(walk);
            const LLUUID up = c ? c->getParentUUID() : parentOf(walk);
            if (c && !c->getName().empty() && c->getName()[0] == '#')
            {
                LLViewerInventoryCategory* p = gInventory.getCategory(up);
                if (p && (p->getName() == LumenFolders::FIRESTORM_FOLDER
                          || p->getName() == LumenFolders::LUMEN_FOLDER))
                {
                    return refused(rule, NAMED_FOLDER, s + " is in " + p->getName() + "/" + c->getName()
                                                       + ", one of the viewer's own folders, which it "
                                                         "finds by name");
                }
            }
            walk = up;
        }
    }
    // Moving something listed out of Marketplace unlists it, and moving
    // something in changes what is for sale: neither is the assistant's.
    if (depth_nesting_in_marketplace(id) >= 0)
        return refused(rule, MARKETPLACE, s + " is in Marketplace listings, which have rules of their own");
    return std::string();
}

// <Lumen>
LLUUID LumenInventoryRules::protectedFolderOf(const LLUUID& id)
{
    // Live: the viewer refills this set the moment the setting changes.
    const uuid_set_t& kept = gInventory.getProtectedCategories();
    if (kept.empty() || id.isNull()) return LLUUID::null;
    LLUUID walk = id;
    for (S32 guard = 0; guard < 64 && walk.notNull(); ++guard)
    {
        if (kept.count(walk)) return walk;
        walk = parentOf(walk);
    }
    return LLUUID::null;
}

std::string LumenInventoryRules::inProtected(const LLUUID& id, bool carried, Rule* rule)
{
    if (rule) *rule = ALLOWED;
    const uuid_set_t& kept = gInventory.getProtectedCategories();
    if (kept.empty()) return std::string();
    const LLUUID at = protectedFolderOf(id);
    if (at.notNull() && at == id)
        return refused(rule, PROTECTED_FOLDER, std::string("it is a folder you protected (right-click > "
                                                           "Unprotect to change that)"));
    if (at.notNull())
        return refused(rule, PROTECTED_FOLDER, "it is in \"" + nameOf(at) + "\"" + UNPROTECT);
    // A folder moved or deleted carries what is in it along.
    if (carried && gInventory.getCategory(id))
    {
        for (const LLUUID& k : kept)
        {
            if (k != id && within(k, id))
                return refused(rule, PROTECTED_FOLDER, "it holds \"" + nameOf(k) + "\"" + UNPROTECT);
        }
    }
    return std::string();
}
// </Lumen>

std::string LumenInventoryRules::held(const LLUUID& id, bool folder, Rule* rule)
{
    std::string why = offLimits(id, "it", rule);
    if (!why.empty()) return why;
    // <Lumen> Protected by the user: the folder itself, and -- stricter than
    // the viewer's own rule -- anything inside it.
    why = inProtected(id, false, rule);
    if (!why.empty() || !folder) return why;

    LLViewerInventoryCategory* cat = gInventory.getCategory(id);
    if (!cat) return refused(rule, NOT_IN_INVENTORY, "it is not a folder in their inventory");
    // A folder holding the bridge's or the AO's would carry it off with it.
    const LLUUID bridge = FSLSLBridge::instance().getBridgeFolder();
    if (bridge.notNull() && within(bridge, id))
        return refused(rule, LSL_BRIDGE, "it holds the LSL bridge's folder, which the viewer needs where it is");
    const LLUUID ao = AOEngine::instance().getAOFolder();
    if (ao.notNull() && within(ao, id))
        return refused(rule, ANIMATION_OVERRIDER, "it holds their animation overrider's folder, which finds "
                                                  "things by where they are");
    // <Lumen> ...and the wearable favourites, likewise.
    const LLUUID favs = FSFloaterWearableFavorites::getFavoritesFolder();
    if (favs.notNull() && within(favs, id))
        return refused(rule, WEARABLE_FAVORITES, "it holds their wearable favourites folder, which the viewer "
                                                 "keeps for its own window");
    if (LLFolderType::lookupIsProtectedType(cat->getPreferredType()))
        return refused(rule, SYSTEM_FOLDER, "it is one of Second Life's own folders");
    const std::string& name = cat->getName();
    if (name == LumenFolders::FIRESTORM_FOLDER || name == LumenFolders::LUMEN_FOLDER || name == RLV_ROOT_FOLDER)
        return refused(rule, NAMED_FOLDER, "it is a folder the viewer and RLV look for by name");
    return std::string();
}

std::string LumenInventoryRules::notInto(const LLUUID& dest, Rule* rule)
{
    if (rule) *rule = ALLOWED;
    if (dest.isNull() || !gInventory.getCategory(dest))
        return refused(rule, NOT_IN_INVENTORY, "that folder is not in their inventory");
    std::string why = offLimits(dest, "that folder", rule);
    if (!why.empty()) return why;
    if (inTrash(dest))
        return refused(rule, TRASH, "that is the Trash, and putting something there is deleting it");
    return std::string();
}

std::string LumenInventoryRules::notMove(const LLUUID& id, bool folder, const LLUUID& dest, Rule* rule)
{
    std::string why = held(id, folder, rule);
    if (!why.empty()) return why;
    // <Lumen> A folder holding a protected one would carry it off.
    if (folder)
    {
        why = inProtected(id, true, rule);
        if (!why.empty()) return why;
    }
    why = notInto(dest, rule);
    if (!why.empty()) return why;
    if (folder && within(dest, id))
        return refused(rule, INSIDE_ITSELF, "a folder cannot go inside itself");
    if (RlvActions::isRlvEnabled()
        && !(folder ? RlvFolderLocks::instance().canMoveFolder(id, dest)
                    : RlvFolderLocks::instance().canMoveItem(id, dest)))
        return refused(rule, RLV, "an RLV lock the user is wearing holds it where it is");
    return std::string();
}

std::string LumenInventoryRules::notRename(const LLUUID& id, bool folder, Rule* rule)
{
    std::string why = held(id, folder, rule);
    if (!why.empty()) return why;
    if (folder)
    {
        if (RlvActions::isRlvEnabled() && !RlvFolderLocks::instance().canRenameFolder(id))
            return refused(rule, RLV, "an RLV lock the user is wearing keeps its name");
        // Its own check also has the viewer's locked folders and who owns it.
        if (!get_is_category_renameable(&gInventory, id))
            return refused(rule, VIEWER, "the viewer does not allow renaming that folder");
        return std::string();
    }
    LLViewerInventoryItem* item = gInventory.getItem(id);
    if (!item) return refused(rule, NOT_IN_INVENTORY, "it is not in their inventory");
    if (item->getIsLinkType())
        return refused(rule, NOT_MODIFIABLE, "it is a link, which takes its name from what it points to");
    if (item->getInventoryType() == LLInventoryType::IT_CALLINGCARD)
        return refused(rule, NOT_MODIFIABLE, "the viewer does not rename calling cards");
    if (!item->getPermissions().allowModifyBy(gAgent.getID()))
        return refused(rule, NOT_MODIFIABLE, "it is not theirs to modify");
    // Items may always be renamed under RLV today; asked anyway, so a lock
    // that one day keeps an item's name is kept here too.
    if (RlvActions::isRlvEnabled() && !RlvFolderLocks::instance().canRenameItem(id))
        return refused(rule, RLV, "an RLV lock the user is wearing keeps its name");
    return std::string();
}

// <Lumen> delete_item, a batch's deletes, and undo putting a thing back in
// the Trash. Before this the single delete asked only the viewer's own check,
// which lets go of the bridge's, the AO's and the favourites' folders when
// their Lock setting is off, and knows nothing of what is inside a protected
// folder.
std::string LumenInventoryRules::notDelete(const LLUUID& id, bool folder, Rule* rule)
{
    std::string why = held(id, folder, rule);
    if (!why.empty()) return why;
    if (folder)
    {
        why = inProtected(id, true, rule);
        if (!why.empty()) return why;
    }
    if (RlvActions::isRlvEnabled() && RlvFolderLocks::instance().hasLockedFolder(RLV_LOCK_ANY)
        && !(folder ? RlvFolderLocks::instance().canRemoveFolder(id)
                    : RlvFolderLocks::instance().canRemoveItem(id)))
        return refused(rule, RLV, "an RLV lock the user is wearing holds it where it is");
    return std::string();
}

// <Lumen> The name being given, beside the names folders have now (the review
// of 2026-10-04): new_folder could make a second #Firestorm at the top, which
// findCategoryByName may then return in place of the real one -- the AO and
// the favourites looking gone -- and undo could never remove it again.
std::string LumenInventoryRules::notNamed(const LLUUID& parent, const std::string& name, Rule* rule,
                                          const std::string& parent_name)
{
    if (rule) *rule = ALLOWED;
    if (name.empty() || name[0] != '#') return std::string();
    if (parent.notNull() && parent == gInventory.getRootFolderID())
    {
        if (name == LumenFolders::FIRESTORM_FOLDER || name == LumenFolders::LUMEN_FOLDER
            || name == RLV_ROOT_FOLDER)
            return refused(rule, NAMED_FOLDER, "\"" + name + "\" at the top of the inventory is a name "
                                               "the viewer and RLV find their own folder by, so it "
                                               "would be taken for theirs");
        return std::string();
    }
    std::string above = parent_name;
    if (parent.notNull())
    {
        LLViewerInventoryCategory* p = gInventory.getCategory(parent);
        above = p ? p->getName() : std::string();
    }
    if (above == LumenFolders::FIRESTORM_FOLDER || above == LumenFolders::LUMEN_FOLDER)
        return refused(rule, NAMED_FOLDER, "a #-name inside " + above + " is one the viewer finds its "
                                           "own folders by, so it would be taken for one of theirs");
    return std::string();
}
// </Lumen>

// =============================================================================
// Chain: where a folder sat, by id and by name, so it can be made again.
// =============================================================================


LumenAIUndo::Chain LumenAIUndo::Chain::of(const LLUUID& folder_id)
{
    Chain c;
    const LLUUID root = gInventory.getRootFolderID();
    LLUUID id = folder_id;
    for (S32 guard = 0; guard < 32 && id.notNull() && id != root; ++guard)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(id);
        if (!cat) break;
        c.folders.emplace_back(id, cat->getName());
        id = cat->getParentUUID();
    }
    std::reverse(c.folders.begin(), c.folders.end());
    return c;
}


// =============================================================================
// Making a folder again, when the one something came from is gone.
// =============================================================================

namespace
{
    /**
     * Finds the deepest folder of a chain that is still there, and makes the
     * rest below it by name -- reusing one of that name if somebody has made it
     * again by hand. Several things waiting for the same lost folder share one
     * new folder, never one each.
     */
    struct Remaker
    {
        std::map<LLUUID, LLUUID> made;                                  // lost id -> its replacement
        std::map<LLUUID, std::vector<std::function<void(LLUUID)>>> waiting;

        static LLUUID childNamed(const LLUUID& parent, const std::string& name)
        {
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(parent, cats, items);
            if (!cats) return LLUUID::null;
            for (const auto& c : *cats)
            {
                if (c && c->getName() == name && c->getPreferredType() == LLFolderType::FT_NONE)
                    return c->getUUID();
            }
            return LLUUID::null;
        }

        void ensure(const LumenAIUndo::Chain& chain, size_t from, LLUUID base,
                    std::function<void(LLUUID)> done)
        {
            // Everything that exists, walked down by id or by name.
            while (from < chain.folders.size())
            {
                const LLUUID lost = chain.folders[from].first;
                if (liveFolder(lost)) { base = lost; ++from; continue; }
                auto m = made.find(lost);
                if (m != made.end() && liveFolder(m->second)) { base = m->second; ++from; continue; }
                const LLUUID same = childNamed(base, chain.folders[from].second);
                if (same.notNull()) { made[lost] = same; base = same; ++from; continue; }
                break;
            }
            if (from >= chain.folders.size()) { done(base); return; }
            // <Lumen> Never made where nothing may be put: in Marketplace
            // listings, the bridge's folder, the Trash.
            if (!LumenInventoryRules::notInto(base).empty()) { done(LLUUID::null); return; }

            const LLUUID lost = chain.folders[from].first;
            auto w = waiting.find(lost);
            const bool first = (w == waiting.end());
            const LumenAIUndo::Chain copy = chain;
            waiting[lost].push_back([this, copy, from, done](LLUUID got)
            {
                if (got.isNull()) { done(LLUUID::null); return; }
                ensure(copy, from + 1, got, done);
            });
            if (!first) return;

            LL_INFOS("LumenAIUndo") << "making folder \"" << chain.folders[from].second
                                    << "\" again" << LL_ENDL;
            gInventory.createNewCategory(base, LLFolderType::FT_NONE, chain.folders[from].second,
                [this, lost](const LLUUID& new_id)
                {
                    if (new_id.notNull()) made[lost] = new_id;
                    std::vector<std::function<void(LLUUID)>> calls;
                    calls.swap(waiting[lost]);
                    waiting.erase(lost);
                    for (auto& c : calls) c(new_id);
                });
        }
    };
}

// =============================================================================
// Plans and running them
// =============================================================================

struct LumenAIUndo::Step
{
    enum Kind { MOVE, RENAME, TRASH } kind = MOVE;
    LLUUID id;
    bool folder = false;
    LLUUID target;         // MOVE: where it goes, if that folder is still there
    Chain chain;           // MOVE: how to make it again if not
    std::string name;      // RENAME: the name it gets back
    std::string label;     // its name as the person knows it
    S64 row = 0;           // <Lumen> the change it reverses (its index + 1), marked once done
    // <Lumen> How the plan saw it: where it was, for a step that moves it,
    // and its name, for a rename. Changed by the time the step comes -- by
    // hand, or by move_item while a long one is still going -- and it is left
    // alone and listed, never overwritten. Each step asks only after what it
    // changes, so a rename and a move of one thing never trip each other.
    LLUUID expect_parent;
    std::string expect_name;
    // <Lumen> TRASH of a folder made then: only if it is empty by the time
    // everything else has moved out of it. A folder the person deleted,
    // deleted again by a redo, goes whole.
    bool only_empty = false;
};

struct LumenAIUndo::Plan
{
    std::vector<Step> steps;
    LLSD left_alone = LLSD::emptyArray();
    LLSD cannot = LLSD::emptyArray();
    LLSD not_yet = LLSD::emptyArray();   // <Lumen> a rename Second Life has not confirmed yet
    S32 left_alone_count = 0;
    S32 cannot_count = 0;
    S32 not_yet_count = 0;
    std::vector<S64> already;            // changes already as they would be put: done, nothing to do
    S64 step_id = 0;
    bool redo = false;
};

namespace
{
    struct RunState
    {
        LLSD done = LLSD::emptyArray();
        LLSD failed = LLSD::emptyArray();
        LLSD left_alone = LLSD::emptyArray();   // <Lumen> the plan's, and what the run left
        S32 done_count = 0;
        S32 failed_count = 0;
        S32 left_alone_count = 0;
        S32 renamed = 0;
        S32 outstanding = 0;   // moves waiting for a folder to be made again, renames for their answer
        S32 renames_waiting = 0;   // <Lumen> ...of those, renames
        bool all_started = false;
        bool reported = false;
        bool in_call = false;  // <Lumen> run() has not answered yet
        bool gave_up = false;  // <Lumen> stopped waiting for folders to be made again
        // <Lumen> Clear in the Assistant window: asked between steps.
        bool stop = false;
        bool stopped_by_user = false;
        bool stopped = false;  // <Lumen> logged out part way
        S32 not_done = 0;      // <Lumen> never started, because it stopped
        std::vector<S64> rows; // <Lumen> the changes put back
        // <Lumen> Folders it made, to the Trash: held back until every move
        // out of them has finished -- those waiting for a folder too -- and
        // even then only if they are empty.
        std::vector<LumenAIUndo::Step> folder_steps;
        LumenAIUndo::Finished finished;
        std::shared_ptr<Remaker> remaker = std::make_shared<Remaker>();

        LLSD summary() const
        {
            LLSD s;
            s["put_back"] = done_count;
            s["could_not"] = failed_count;
            s["done"] = done;
            s["failed"] = failed;
            s["left_alone"] = left_alone;
            s["left_alone_count"] = left_alone_count;
            // <Lumen>
            if (not_done > 0) s["not_done"] = not_done;
            if (stopped_by_user) s["stopped_by_user"] = true;
            if (stopped) s["stopped"] = true;
            return s;
        }

        // <Lumen>
        void didIt(const LumenAIUndo::Step& s, const std::string& what)
        {
            ++done_count;
            if (s.row) rows.push_back(s.row);
            addCapped(done, entry(s.label, what, "what"));
        }

        void couldNot(const std::string& label, const std::string& why)
        {
            ++failed_count;
            addCapped(failed, entry(label, why));
        }

        void leftIt(const std::string& label, const std::string& why)
        {
            ++left_alone_count;
            addCapped(left_alone, entry(label, why));
        }
        // </Lumen>
    };

    void doStep(const LumenAIUndo::Step& s, const std::shared_ptr<RunState>& st);

    // <Lumen>
    /**
     * Everything started and every folder answered: the folders it made go
     * to the Trash now, if they are empty, and then the report. Once.
     */
    void finishIfDone(const std::shared_ptr<RunState>& st)
    {
        if (!st->all_started || st->outstanding > 0 || st->reported) return;
        st->reported = true;
        std::vector<LumenAIUndo::Step> folders;
        folders.swap(st->folder_steps);
        // <Lumen> Stopped: the folders it made stay too -- what was to come out
        // of them may not have.
        if (st->stopped || st->stopped_by_user) st->not_done += (S32)folders.size();
        else for (const LumenAIUndo::Step& f : folders) doStep(f, st);   // never waits: no folder is made
        if (st->finished) st->finished(st->summary(), st->rows, !st->in_call);
    }

    /**
     * A folder Second Life never makes again would leave the run waiting for
     * ever -- never reported, and an undo never marked. After a while it
     * reports without it; what still waited stays where it is.
     * <Lumen> Renames wait here too, for Second Life to confirm them; the
     * while runs from the last answer, so a long queue that is moving is not
     * cut off. Stop ends the wait at once.
     */
    void waitForFolders(const std::shared_ptr<RunState>& st)
    {
        if (st->outstanding == 0 || st->reported) return;
        LLCoros::instance().launch("LumenAIUndoWait", [st]()
        {
            F64 give_up = LLTimer::getTotalSeconds() + FOLDER_WAIT;
            S32 last = st->outstanding;
            while (!st->reported && !st->stop && LLTimer::getTotalSeconds() < give_up)
            {
                if (LLApp::isExiting()) return;
                llcoro::suspendUntilTimeout(0.25f);
                if (st->outstanding < last)   // <Lumen> answers are coming
                {
                    last = st->outstanding;
                    give_up = LLTimer::getTotalSeconds() + FOLDER_WAIT;
                }
            }
            if (st->reported) return;
            LL_WARNS("LumenAIUndo") << st->outstanding << " still waiting for Second Life (a folder "
                                    << "made again, or a name); reporting without them" << LL_ENDL;
            st->gave_up = true;
            if (st->stop)   // <Lumen>
            {
                st->stopped_by_user = true;
                st->not_done += st->outstanding;
            }
            else
            {
                st->couldNot(llformat("%d more", st->outstanding),
                             "Second Life had not answered in time -- for the folder they were in, "
                             "made again, or for a name put back -- so they are not counted as put "
                             "back");
                st->failed_count += st->outstanding - 1;
            }
            st->outstanding = 0;
            st->renames_waiting = 0;
            finishIfDone(st);
        });
    }

    /**
     * <Lumen> Has it changed since the plan saw it? Then it is left alone and
     * listed. See Step::expect_parent.
     */
    bool changedSince(const LumenAIUndo::Step& s, std::string& why)
    {
        if (s.kind == LumenAIUndo::Step::RENAME)
        {
            const std::string now = nameOf(s.id);
            if (now == s.expect_name) return false;
            why = "renamed since this was planned; it is called \"" + now + "\"";
            return true;
        }
        const LLUUID now = parentOf(s.id);
        if (now == s.expect_parent) return false;
        why = "moved since this was planned; it is in " + pathOf(now);
        return true;
    }
    // </Lumen>

    /** Move now; true when the viewer's own model shows it where it belongs. */
    bool moveNow(const LLUUID& id, bool folder, const LLUUID& target, std::string& why,
                 LumenInventoryRules::Rule* rule = nullptr)
    {
        // <Lumen> The rules every path shares: move_item's, a batch's, this.
        why = LumenInventoryRules::notMove(id, folder, target, rule);
        if (!why.empty()) return false;
        LumenAIUndo::instance().claimParent(id, target);   // <Lumen> not the person's
        if (folder)
        {
            LLViewerInventoryCategory* c = gInventory.getCategory(id);
            if (!c) { why = "it is no longer in inventory"; return false; }
            gInventory.changeCategoryParent(c, target, false);
        }
        else
        {
            LLViewerInventoryItem* i = gInventory.getItem(id);
            if (!i) { why = "it is no longer in inventory"; return false; }
            gInventory.changeItemParent(i, target, false);
        }
        // The local move is immediate, so if it is not there now it did not go.
        if (parentOf(id) != target)
        {
            why = "the viewer did not move it";
            return false;
        }
        return true;
    }

    void doStep(const LumenAIUndo::Step& s, const std::shared_ptr<RunState>& st)
    {
        std::string why;
        // <Lumen> Which rule said no: a folder protected since the plan leaves
        // the thing alone, and says so -- it is not a failure.
        LumenInventoryRules::Rule rule = LumenInventoryRules::ALLOWED;
        // <Lumen> Changed since the plan saw it: left alone, never overwritten.
        // (Already in the Trash, where the step was taking it, is not a change.)
        if (exists(s.id) && !(s.kind == LumenAIUndo::Step::TRASH && inTrash(s.id))
            && changedSince(s, why))
        {
            st->leftIt(s.label, why);
            return;
        }
        switch (s.kind)
        {
        case LumenAIUndo::Step::RENAME:
        {
            if (!exists(s.id)) { why = "it is no longer in inventory"; break; }
            why = LumenInventoryRules::notRename(s.id, s.folder, &rule);   // <Lumen>
            if (!why.empty()) break;
            // <Lumen> Counted when Second Life answers, as the name it then
            // has: under AIS the viewer's own copy changes only with the
            // answer, and a refused rename never changes it. Until then the
            // run waits, as for a folder being made again.
            ++st->renamed;
            ++st->outstanding;
            ++st->renames_waiting;
            LumenAIUndo::instance().renameSent(s.id);
            LumenAIUndo::instance().claimName(s.id, s.name);   // <Lumen> not the person's
            std::shared_ptr<RunState> keep = st;
            const LumenAIUndo::Step step = s;
            LLPointer<LLInventoryCallback> answered = new LLBoostFuncInventoryCallback(
                [keep, step](const LLUUID&)
                {
                    if (LumenAIUndo::instanceExists()) LumenAIUndo::instance().renameAnswered(step.id);
                    if (keep->gave_up) return;   // reported without it already
                    if (nameOf(step.id) == step.name)
                        keep->didIt(step, "renamed back to \"" + step.name + "\"");
                    else
                        keep->couldNot(step.label, "Second Life did not take the name back");
                    --keep->outstanding;
                    --keep->renames_waiting;
                    finishIfDone(keep);
                });
            if (s.folder)
            {
                rename_category(&gInventory, s.id, s.name, answered);
            }
            else
            {
                LLSD updates; updates["name"] = s.name;
                update_inventory_item(s.id, updates, answered);
            }
            return;
            // </Lumen>
        }
        case LumenAIUndo::Step::TRASH:
        {
            if (!exists(s.id)) { why = "it is already gone"; break; }
            if (inTrash(s.id))
            {
                st->didIt(s, "already in the Trash");
                return;
            }
            // <Lumen> The shared rules for anything going to the Trash.
            why = LumenInventoryRules::notDelete(s.id, s.folder, &rule);
            if (!why.empty()) break;
            if (s.folder && s.only_empty)
            {
                // <Lumen> Checked again now, on the viewer's own model, after
                // every move out of it has finished. The plan counted it
                // empty by simulating those moves; one that was refused (an
                // RLV lock) or never happened leaves its thing in here, and
                // the folder must not take it to the Trash. Anything in it --
                // or anything not loaded -- and it stays.
                LLViewerInventoryCategory* cat = gInventory.getCategory(s.id);
                if (!cat) { why = "it is no longer in inventory"; break; }
                if (!gInventory.isCategoryComplete(s.id))
                {
                    st->leftIt(s.label, "the viewer has not loaded all of the folder it made, so it "
                                        "may hold things; it stays");
                    return;
                }
                if (cat->getViewerDescendentCount() > 0)
                {
                    st->leftIt(s.label, "the folder it made has things in it now, so it stays");
                    return;
                }
                // </Lumen>
            }
            if (s.folder)
            {
                if (!get_is_category_removable(&gInventory, s.id))
                {
                    why = "the viewer does not allow removing that folder";
                    break;
                }
                LumenAIUndo::instance().claimParent(s.id, trashId());   // <Lumen> not the person's
                gInventory.removeCategory(s.id);
            }
            else
            {
                if (!get_is_item_removable(&gInventory, s.id, true))
                {
                    why = "the viewer does not allow removing it";
                    break;
                }
                LumenAIUndo::instance().claimParent(s.id, trashId());   // <Lumen> not the person's
                gInventory.removeItem(s.id);
            }
            if (!inTrash(s.id)) { why = "the viewer did not move it to the Trash"; break; }
            st->didIt(s, s.only_empty ? "the folder made then, now empty, moved to the Trash"
                                      : "moved to the Trash");
            return;
        }
        case LumenAIUndo::Step::MOVE:
        {
            if (!exists(s.id)) { why = "it is no longer in inventory"; break; }
            if (liveFolder(s.target))
            {
                if (moveNow(s.id, s.folder, s.target, why, &rule))
                {
                    st->didIt(s, "back in " + pathOf(s.target));
                    return;
                }
                break;
            }
            if (s.chain.folders.empty())
            {
                why = "the folder it was in is gone, and the record does not say where that was";
                break;
            }
            // <Lumen> Not a folder made again for something that may not move:
            // what the move would ask of the thing itself -- Marketplace, an
            // RLV-held folder, a protected one, a folder carrying one -- is
            // asked first, so a refused move leaves no empty folder behind
            // that nothing records. Only where it goes waits for the folder.
            why = LumenInventoryRules::held(s.id, s.folder, &rule);
            if (why.empty() && s.folder) why = LumenInventoryRules::inProtected(s.id, true, &rule);
            if (!why.empty()) break;
            // The folder it came from is gone: make it again, then move.
            ++st->outstanding;
            std::shared_ptr<RunState> keep = st;
            const LumenAIUndo::Step step = s;
            keep->remaker->ensure(s.chain, 0, gInventory.getRootFolderID(),
                [keep, step](LLUUID dest)
                {
                    if (keep->gave_up) return;   // <Lumen> reported without it already
                    std::string w;
                    LumenInventoryRules::Rule r = LumenInventoryRules::ALLOWED;   // <Lumen>
                    if (dest.isNull())
                    {
                        keep->couldNot(step.label, "the folder it was in could not be made again");
                    }
                    else if (exists(step.id) && changedSince(step, w))   // <Lumen> while it waited
                    {
                        keep->leftIt(step.label, w);
                    }
                    else if (moveNow(step.id, step.folder, dest, w, &r))
                    {
                        keep->didIt(step, "back in " + pathOf(dest) + " (that folder was made again)");
                    }
                    else if (r == LumenInventoryRules::PROTECTED_FOLDER)   // <Lumen>
                    {
                        keep->leftIt(step.label, w);
                    }
                    else
                    {
                        keep->couldNot(step.label, w);
                    }
                    --keep->outstanding;
                    finishIfDone(keep);
                });
            return;
        }
        }
        // <Lumen>
        if (rule == LumenInventoryRules::PROTECTED_FOLDER) st->leftIt(s.label, why);
        else st->couldNot(s.label, why);
    }
}

// <Lumen> How many undos and redos are still putting things back. Emptying
// the Trash waits for none: it would purge what they are taking out of it.
// <Lumen> And the runs themselves, for Stop and Clear to reach.
namespace
{
    S32 sPuttingBack = 0;
    std::vector<std::weak_ptr<RunState>> sRuns;

    // <Lumen> How many renames a paced run lets wait for Second Life at once:
    // each is a request in the same small pool the inventory's own loading
    // uses, and a few hundred queued starved it.
    const S32 RENAMES_IN_FLIGHT = 40;
}

bool LumenAIUndo::puttingBack() const
{
    return sPuttingBack > 0;
}

LLSD LumenAIUndo::run(Plan& plan, const Finished& finished)
{
    std::shared_ptr<RunState> st = std::make_shared<RunState>();
    // <Lumen> Reachable by Stop until it has reported.
    sRuns.erase(std::remove_if(sRuns.begin(), sRuns.end(),
                               [](const std::weak_ptr<RunState>& w) { return w.expired(); }),
                sRuns.end());
    sRuns.push_back(st);
    ++sPuttingBack;
    st->finished = [finished](const LLSD& s, const std::vector<S64>& rows, bool later)
    {
        if (sPuttingBack > 0) --sPuttingBack;
        if (finished) finished(s, rows, later);
    };
    st->left_alone = plan.left_alone;
    st->left_alone_count = plan.left_alone_count;

    // <Lumen> Folders to the Trash go last, after everything else has moved
    // out of them; see finishIfDone().
    std::vector<Step> steps;
    steps.reserve(plan.steps.size());
    for (const Step& s : plan.steps)
    {
        if (s.kind == Step::TRASH && s.folder && s.only_empty) st->folder_steps.push_back(s);
        else steps.push_back(s);
    }

    LLSD out;
    out["cannot"] = plan.cannot;
    out["cannot_count"] = plan.cannot_count;
    out["steps"] = (S32)plan.steps.size();

    if (steps.size() <= RUN_NOW_LIMIT)
    {
        st->in_call = true;
        for (const Step& s : steps) doStep(s, st);
        st->all_started = true;
        finishIfDone(st);
        st->in_call = false;
        out["put_back"] = st->done_count;
        out["could_not"] = st->failed_count;
        out["done"] = st->done;
        out["failed"] = st->failed;
        out["left_alone"] = st->left_alone;
        out["left_alone_count"] = st->left_alone_count;
        // Report at the end only when something is still on its way; what
        // finished in the call is already in this reply.
        // <Lumen> A rename is put back once Second Life confirms it: under AIS
        // that is a moment after this answer, so it is not counted yet.
        out["waiting_for_folders"] = st->outstanding - st->renames_waiting;
        if (st->renames_waiting > 0)
        {
            out["waiting_for_names"] = st->renames_waiting;
            out["waiting_note"] = llformat("Names being put back, waiting for Second Life to confirm "
                                           "them: %d. They are not in put_back yet; the viewer tells "
                                           "the user itself when they are done.", st->renames_waiting);
        }
        out["finished"] = st->reported;
        waitForFolders(st);
        return out;
    }

    // Many steps: paced, so neither the frame nor Second Life takes them at once.
    out["left_alone"] = plan.left_alone;
    out["left_alone_count"] = plan.left_alone_count;
    LLCoros::instance().launch("LumenAIUndoRun", [st, steps]()
    {
        size_t n = 0;
        for (; n < steps.size(); ++n)
        {
            if (LLApp::isExiting()) return;
            // <Lumen> Logged out part way -- or disconnected, the viewer still
            // open behind its message, where a move changes only the viewer's
            // own copy and the message is dropped -- or asked to stop: what is
            // left is not done, and an undo's set is not marked undone.
            if (!gInventory.isInventoryUsable() || offline()) { st->stopped = true; break; }
            if (st->stop) { st->stopped_by_user = true; break; }
            // <Lumen> Renames wait for their answer a few dozen at a time.
            if (steps[n].kind == Step::RENAME && st->renames_waiting >= RENAMES_IN_FLIGHT)
            {
                F64 give_up = LLTimer::getTotalSeconds() + FOLDER_WAIT;
                S32 last = st->renames_waiting;
                while (st->renames_waiting >= RENAMES_IN_FLIGHT && !st->stop
                       && LLTimer::getTotalSeconds() < give_up)
                {
                    llcoro::suspendUntilTimeout(0.1f);
                    if (st->renames_waiting < last)
                    {
                        last = st->renames_waiting;
                        give_up = LLTimer::getTotalSeconds() + FOLDER_WAIT;
                    }
                }
                if (st->stop) { st->stopped_by_user = true; break; }
            }
            doStep(steps[n], st);
            if ((n + 1) % 25 == 0) llcoro::suspendUntilTimeout(0.1f);
        }
        st->not_done += (S32)(steps.size() - n);   // <Lumen>
        if (st->stopped || st->stopped_by_user)
        {
            LL_INFOS("LumenAIUndo") << "putting back stopped after " << n << " of " << steps.size()
                                    << (st->stopped_by_user ? ", as asked" : ", logged out") << LL_ENDL;
        }
        st->all_started = true;
        finishIfDone(st);
        waitForFolders(st);
    });
    out["paced"] = true;
    out["finished"] = false;
    return out;
}


// =============================================================================
// The list: steps, and the changes in each
// =============================================================================

struct LumenAIUndo::Change
{
    enum Kind { MOVE, RENAME, TRASH, NEW_FOLDER } kind = MOVE;
    LLUUID id;
    bool folder = false;
    LLUUID parent_before, parent_after;
    std::string name_before, name_after;
    Chain chain_before, chain_after;   // where those folders sat, to make one again
    bool undone = false;   // put back by the last undo of its step: a redo does these
    bool gone = false;     // emptied from the Trash, given away or rezzed
};

struct LumenAIUndo::Entry
{
    S64 id = 0;
    bool assistant = false;
    std::string words;     // the person's request, for the assistant's
    std::string what;      // a bulk run's own line
    std::vector<Change> changes;
    std::unordered_map<LLUUID, size_t> last;   // each thing's latest change here
    F64 when = 0.0;        // its latest change
    bool stopped = false;  // a bulk run that stopped part way
    S32 rev = 0;           // changes added to it: a question about it is about this many
};

namespace
{
    // The inventory's own news, looked at once a frame rather than inside
    // the notification: by then the tool that made a change has said so.
    class UndoObserver : public LLInventoryObserver
    {
    public:
        void changed(U32 mask) override
        {
            if (LumenAIUndo::instanceExists())
                LumenAIUndo::instance().observed(gInventory.getChangedIDs(),
                                                 (mask & LLInventoryObserver::CREATE) != 0);
        }
    };

    void undoIdle(void*)
    {
        if (LLApp::isExiting()) return;
        if (LLStartUp::getStartupState() < STATE_STARTED) return;
        LumenAIUndo::instance().tick();
    }

    void toast(const std::string& line)
    {
        LLSD args;
        args["MESSAGE"] = line;
        LLNotificationsUtil::add("LumenUndoDone", args);
    }

    std::string countOf(S32 items, S32 folders)
    {
        auto one = [](S32 n, const char* what) -> std::string
        {
            return n == 1 ? llformat("1 %s", what) : llformat("%d %ss", n, what);
        };
        if (folders == 0) return one(items, "item");
        if (items == 0) return one(folders, "folder");
        return one(items, "item") + " and " + one(folders, "folder");
    }

    /** A folder as the question names it: its own name, short. */
    std::string folderName(const LLUUID& id)
    {
        if (id.isNull()) return "?";
        if (id == gInventory.getRootFolderID()) return "the top of the inventory";
        if (id == trashId()) return "Trash";
        const std::string n = nameOf(id);
        return n.empty() ? std::string("a folder that is gone") : n;
    }

    std::string lastName(const LumenAIUndo::Chain& chain, const LLUUID& id)
    {
        if (!chain.folders.empty() && chain.folders.back().first == id) return chain.folders.back().second;
        return folderName(id);
    }

    std::string quoted(const std::string& s) { return "\"" + s + "\""; }

    const S32 MENU_LABEL_MAX = 64;
}

LumenAIUndo::LumenAIUndo()
{
}

LumenAIUndo::~LumenAIUndo()
{
    // <Lumen> At quit the inventory deletes every observer still registered
    // (cleanupInventory) long before singletons go: ours is then gone, and
    // deleting it again crashed every quit after login. Ours to delete only
    // while the inventory still holds it.
    if (mObserver && gInventory.containsObserver(mObserver))
    {
        gInventory.removeObserver(mObserver);
        delete mObserver;
    }
    mObserver = nullptr;
}

// static
void LumenAIUndo::registerMenu()
{
    static bool done = false;
    if (done) return;
    done = true;

    auto ask = [](bool redo)
    {
        if (!LumenAIUndo::instanceExists()) return;
        LumenAIUndo& u = LumenAIUndo::instance();
        LLSD pv = u.preview(redo);
        if (pv.has("error"))
        {
            toast(pv["error"].asString());
            return;
        }
        if (pv["nothing"].asBoolean())
        {
            // Nothing in it can be put back: it is not offered again.
            u.perform(redo, pv["step"].asInteger());
            return;
        }
        LLSD subs;
        subs["STEP"] = pv["name"];
        subs["TEXT"] = pv["text"];
        LLSD payload;
        payload["step"] = pv["step"];
        payload["rev"] = pv["rev"];
        payload["redo"] = redo;
        LLNotificationsUtil::add(redo ? "LumenAskRedo" : "LumenAskUndo", subs, payload,
            [](const LLSD& n, const LLSD& r)
            {
                if (LLNotificationsUtil::getSelectedOption(n, r) != 0) return;
                if (!LumenAIUndo::instanceExists()) return;
                const LLSD out = LumenAIUndo::instance().perform(n["payload"]["redo"].asBoolean(),
                                                                 n["payload"]["step"].asInteger(),
                                                                 n["payload"]["rev"].asInteger());
                if (out.has("error")) toast(out["error"].asString());
            });
    };
    auto label = [](LLUICtrl* ctrl, bool redo) -> bool
    {
        const bool there = LumenAIUndo::instanceExists();
        if (LLMenuItemGL* item = dynamic_cast<LLMenuItemGL*>(ctrl))
            item->setLabel(there ? LumenAIUndo::instance().menuLabel(redo)
                                 : std::string(redo ? "Redo" : "Undo"));
        return there && LumenAIUndo::instance().canUndo(redo);
    };
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Lumen.Inventory.Undo",
        [ask](LLUICtrl*, const LLSD&) { ask(false); });
    LLUICtrl::CommitCallbackRegistry::defaultRegistrar().add("Lumen.Inventory.Redo",
        [ask](LLUICtrl*, const LLSD&) { ask(true); });
    LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Lumen.Inventory.UndoEnable",
        [label](LLUICtrl* c, const LLSD&) { return label(c, false); });
    LLUICtrl::EnableCallbackRegistry::defaultRegistrar().add("Lumen.Inventory.RedoEnable",
        [label](LLUICtrl* c, const LLSD&) { return label(c, true); });

    gIdleCallbacks.addFunction(undoIdle, nullptr);
}

// =============================================================================
// Watching the inventory
// =============================================================================

void LumenAIUndo::startWatching()
{
    // A login: the list starts empty.
    mUndo.clear();
    mRedo.clear();
    mBusy.reset();
    mKnown.clear();
    mClaims.clear();
    mPending.clear();
    mCreated.clear();
    mTrashedFrom.clear();
    mRequestStep = 0;

    const F64 t0 = LLTimer::getTotalSeconds();
    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(gInventory.getRootFolderID(), cats, items, LLInventoryModel::INCLUDE_TRASH);
    mKnown.reserve(cats.size() + items.size() + 4096);
    for (const LLPointer<LLViewerInventoryCategory>& c : cats)
        if (c) mKnown[c->getUUID()] = Known{ c->getParentUUID(), c->getName() };
    for (const LLPointer<LLViewerInventoryItem>& i : items)
        if (i) mKnown[i->getUUID()] = Known{ i->getParentUUID(), i->getName() };

    if (!mObserver) mObserver = new UndoObserver();
    if (!gInventory.containsObserver(mObserver)) gInventory.addObserver(mObserver);
    mWatching = true;
    mPersonToo = false;
    LL_INFOS("LumenAIUndo") << "watching the inventory: " << mKnown.size() << " things known, in "
                            << llformat("%.0f", (LLTimer::getTotalSeconds() - t0) * 1000.0) << " ms"
                            << LL_ENDL;
}

void LumenAIUndo::observed(const std::set<LLUUID>& ids, bool created)
{
    if (!mWatching) return;
    mPending.insert(ids.begin(), ids.end());
    if (created) mCreated.insert(ids.begin(), ids.end());
}

void LumenAIUndo::tick()
{
    if (!mWatching)
    {
        if (!gInventory.isInventoryUsable()) return;
        startWatching();
    }
    if (!mPersonToo && LLInventoryModelBackgroundFetch::instance().isEverythingFetched())
    {
        mPersonToo = true;
        LL_INFOS("LumenAIUndo") << "the whole inventory has loaded: the person's own changes are steps "
                                << "from now on" << LL_ENDL;
    }

    const F64 now = LLTimer::getTotalSeconds();
    if (now > mNextSweep)
    {
        mNextSweep = now + 10.0;
        for (auto it = mClaims.begin(); it != mClaims.end(); )
        {
            if (now > it->second.until) it = mClaims.erase(it);
            else ++it;
        }
    }

    if (mPending.empty()) return;
    std::set<LLUUID> ids, created;
    ids.swap(mPending);
    created.swap(mCreated);

    const LLUUID root = gInventory.getRootFolderID();
    const std::string new_folder = LLViewerFolderType::lookupNewCategoryName(LLFolderType::FT_NONE);
    std::set<LLUUID> removed;
    for (const LLUUID& id : ids)
    {
        const LLInventoryObject* obj = gInventory.getObject(id);
        auto known = mKnown.find(id);
        if (!obj)
        {
            if (known != mKnown.end())
            {
                removed.insert(id);
                mKnown.erase(known);
            }
            mClaims.erase(id);
            continue;
        }
        const LLUUID parent = obj->getParentUUID();
        const std::string& name = obj->getName();
        const bool folder = gInventory.getCategory(id) != nullptr;
        auto claim = mClaims.find(id);

        if (known == mKnown.end())
        {
            // Arrived: loaded, delivered, or made. Only a folder the person
            // made in the viewer is a step -- "New Folder", empty, where they
            // keep their own things, marked by the viewer as just made (one
            // given by somebody arrives unmarked) -- and not one the
            // assistant made.
            mKnown[id] = Known{ parent, name };
            if (mPersonToo && folder && name == new_folder && created.count(id) && claim == mClaims.end()
                && within(id, root) && !inTrash(id) && !housekeeping(parent))
            {
                Change c;
                c.kind = Change::NEW_FOLDER;
                c.id = id;
                c.folder = true;
                c.parent_after = parent;
                c.name_before = c.name_after = name;
                c.chain_after = Chain::of(parent);
                personChange(c);
            }
            continue;
        }

        const Known was = known->second;
        known->second = Known{ parent, name };
        bool moved = was.parent != parent;
        bool renamed = was.name != name;
        if (!moved && !renamed) continue;

        // Said by whoever made it: the assistant's tools, undo and redo.
        if (claim != mClaims.end())
        {
            if (moved && claim->second.has_parent && claim->second.parent == parent)
            {
                moved = false;
                claim->second.has_parent = false;
            }
            if (renamed && claim->second.has_name && claim->second.name == name)
            {
                renamed = false;
                claim->second.has_name = false;
            }
            if (!claim->second.has_parent && !claim->second.has_name) mClaims.erase(claim);
        }
        if (!mPersonToo || (!moved && !renamed)) continue;
        if (!within(id, root)) continue;   // the Library

        if (moved)
        {
            const LLUUID trash = trashId();
            const bool from_trash = was.parent == trash || inTrash(was.parent);
            const bool to_trash = inTrash(id);
            if (!(from_trash && to_trash) && !housekeeping(was.parent) && !housekeeping(parent))
            {
                Change c;
                c.kind = (to_trash && !from_trash) ? Change::TRASH : Change::MOVE;
                c.id = id;
                c.folder = folder;
                c.parent_before = was.parent;
                c.parent_after = parent;
                c.name_before = c.name_after = name;
                c.chain_before = Chain::of(was.parent);
                c.chain_after = Chain::of(parent);
                if (c.kind == Change::TRASH) mTrashedFrom[id] = was.parent;
                personChange(c);
            }
        }
        if (renamed && !inTrash(id) && !housekeeping(parent))
        {
            Change c;
            c.kind = Change::RENAME;
            c.id = id;
            c.folder = folder;
            c.parent_before = c.parent_after = parent;
            c.name_before = was.name;
            c.name_after = name;
            personChange(c);
        }
    }
    if (!removed.empty()) purged(removed);
}

/**
 * The viewer's own places, where nothing is a step: the ones the shared rules
 * keep the assistant out of (Current Outfit, the bridge's, the AO's, the
 * favourites', the viewer's #-folders, Marketplace, the Library).
 */
bool LumenAIUndo::housekeeping(const LLUUID& folder) const
{
    if (folder.isNull()) return true;
    // <Lumen> RLV's shared folder too: a "give to #RLV" is moved in and
    // renamed by RLV itself, and things worn from there are renamed by it.
    const LLUUID rlv = RlvInventory::instance().getSharedRootID();
    if (rlv.notNull() && within(folder, rlv)) return true;
    return !LumenInventoryRules::offLimits(folder).empty();
}

void LumenAIUndo::personChange(const Change& c)
{
    const F64 now = LLTimer::getTotalSeconds();
    Entry* top = mUndo.empty() ? nullptr : mUndo.back().get();
    if (top && !top->assistant && !top->changes.empty())
    {
        const Change& first = top->changes.front();
        // "New Folder", then the name they give it: one step.
        if (c.kind == Change::RENAME && top->changes.size() == 1 && first.kind == Change::NEW_FOLDER
            && first.id == c.id && now - top->when < NAMED_SOON)
        {
            top->changes.front().name_after = c.name_after;
            top->when = now;
            ++top->rev;
            ++mRevision;
            mRedo.clear();
            return;
        }
        // One drag, one delete of several: one step.
        if (now - top->when < GROUP_SECONDS && first.kind == c.kind
            && (c.kind == Change::MOVE || c.kind == Change::TRASH) && first.parent_after == c.parent_after)
        {
            addChange(*top, c);
            return;
        }
    }
    Entry& e = pushEntry(false, std::string());
    addChange(e, c);
}

void LumenAIUndo::purged(const std::set<LLUUID>& ids)
{
    auto mark = [&ids](std::deque<std::shared_ptr<Entry>>& list)
    {
        for (auto it = list.begin(); it != list.end(); )
        {
            Entry& e = **it;
            bool any_left = e.changes.empty();
            for (Change& c : e.changes)
            {
                if (ids.count(c.id)) c.gone = true;
                if (!c.gone) any_left = true;
            }
            // Only about what is gone now: nothing left to undo or redo.
            if (!any_left) it = list.erase(it);
            else ++it;
        }
    };
    mark(mUndo);
    mark(mRedo);
    if (mBusy) for (Change& c : mBusy->changes) if (ids.count(c.id)) c.gone = true;
    for (const LLUUID& id : ids) mTrashedFrom.erase(id);
}

// =============================================================================
// Recording the assistant's changes
// =============================================================================

void LumenAIUndo::beginRequest(const std::string& words)
{
    mInRequest = true;
    mWords = words;
    mRequestStep = 0;
    ++mRequestSerial;   // <Lumen>
}

void LumenAIUndo::endRequest()
{
    mInRequest = false;
    mWords.clear();
    mRequestStep = 0;
}

LumenAIUndo::Entry* LumenAIUndo::findEntry(S64 id)
{
    for (auto& e : mUndo) if (e->id == id) return e.get();
    return nullptr;
}

LumenAIUndo::Entry& LumenAIUndo::pushEntry(bool assistant, const std::string& words)
{
    // Steps that never had a change in them -- a turn that changed nothing
    // in the end -- are not kept, unless one is still being made.
    for (auto it = mUndo.begin(); it != mUndo.end(); )
    {
        const Entry& old = **it;
        if (old.changes.empty() && old.id != mRequestStep && !mRunning.count(old.id)) it = mUndo.erase(it);
        else ++it;
    }
    auto e = std::make_shared<Entry>();
    e->id = mNextId++;
    e->assistant = assistant;
    e->words = words;
    e->when = LLTimer::getTotalSeconds();
    mUndo.push_back(e);
    while (mUndo.size() > MAX_STEPS)
    {
        if (mUndo.front()->id == mRequestStep) mRequestStep = 0;
        mUndo.pop_front();
    }
    return *mUndo.back();
}

/**
 * The step a change goes in: the one asked for, unless it has been undone
 * since (or dropped off the end) -- then the turn's, as for a change with no
 * step named. Outside a turn, none: nothing is listed for a direct call.
 */
LumenAIUndo::Entry* LumenAIUndo::entryFor(S64 wanted)
{
    if (wanted < 0 || mUnlisted.count(wanted)) return nullptr;
    if (wanted > 0)
    {
        if (Entry* e = findEntry(wanted)) return e;
        LL_INFOS("LumenAIUndo") << "a change for step " << wanted << ", undone or gone since, goes in "
                                << "the step in progress" << LL_ENDL;
    }
    if (mInRequest)
    {
        if (mRequestStep)
        {
            if (Entry* e = findEntry(mRequestStep)) return e;
        }
        Entry& e = pushEntry(true, mWords);
        mRequestStep = e.id;
        return &e;
    }
    // <Lumen> A host application or a test calling the endpoint with no
    // conversation: not listed, unless the debug setting says to -- then
    // each change is a step of its own.
    static LLCachedControl<bool> list_them(gSavedSettings, "LumenAIUndoEndpointCalls", false);
    if (list_them) return &pushEntry(true, std::string());
    return nullptr;
}

void LumenAIUndo::addChange(Entry& e, const Change& c)
{
    const F64 now = LLTimer::getTotalSeconds();
    e.when = now;
    ++e.rev;
    ++mRevision;
    mRedo.clear();   // a new change after an undo: what could be redone is gone
    auto last = e.last.find(c.id);
    if (last != e.last.end())
    {
        Change& prev = e.changes[last->second];
        // Moved twice, or renamed twice, in one step: one change, from where
        // (or what) it was first to where (or what) it ended.
        if (prev.kind == c.kind && (c.kind == Change::MOVE || c.kind == Change::RENAME) && !prev.undone)
        {
            prev.parent_after = c.parent_after;
            prev.chain_after = c.chain_after;
            prev.name_after = c.name_after;
            return;
        }
    }
    e.last[c.id] = e.changes.size();
    e.changes.push_back(c);
}

void LumenAIUndo::claimParent(const LLUUID& id, const LLUUID& parent)
{
    Claim& c = mClaims[id];
    c.parent = parent;
    c.has_parent = true;
    c.until = LLTimer::getTotalSeconds() + CLAIM_KEEPS;
}

void LumenAIUndo::claimName(const LLUUID& id, const std::string& name)
{
    Claim& c = mClaims[id];
    c.name = name;
    c.has_name = true;
    c.until = LLTimer::getTotalSeconds() + CLAIM_KEEPS;
}

bool LumenAIUndo::recordMove(const LLUUID& id, bool folder, const LLUUID& from, const LLUUID& to,
                             S64 set_id)
{
    claimParent(id, to);
    Entry* e = entryFor(set_id);
    if (!e) return false;
    Change c;
    c.kind = Change::MOVE;
    c.id = id;
    c.folder = folder;
    c.parent_before = from;
    c.parent_after = to;
    c.name_before = c.name_after = nameOf(id);
    c.chain_before = Chain::of(from);
    c.chain_after = Chain::of(to);
    addChange(*e, c);
    return true;
}

bool LumenAIUndo::recordRename(const LLUUID& id, bool folder, const std::string& before,
                               const std::string& after, S64 set_id)
{
    claimName(id, after);
    Entry* e = entryFor(set_id);
    if (!e) return false;
    Change c;
    c.kind = Change::RENAME;
    c.id = id;
    c.folder = folder;
    c.parent_before = c.parent_after = parentOf(id);
    c.name_before = before;
    c.name_after = after;
    addChange(*e, c);
    return true;
}

S64 LumenAIUndo::setForLater()
{
    Entry* e = entryFor(0);
    return e ? e->id : -1;
}

void LumenAIUndo::renameSent(const LLUUID& id)
{
    ++mRenamesInFlight[id];
}

void LumenAIUndo::renameAnswered(const LLUUID& id)
{
    std::map<LLUUID, S32>::iterator f = mRenamesInFlight.find(id);
    if (f != mRenamesInFlight.end() && --f->second <= 0) mRenamesInFlight.erase(f);
}

void LumenAIUndo::recordTrash(const LLUUID& id, bool folder, const LLUUID& from, S64 set_id)
{
    const LLUUID now_in = parentOf(id);
    claimParent(id, now_in);
    mTrashedFrom[id] = from;
    Entry* e = entryFor(set_id);
    if (!e) return;
    Change c;
    c.kind = Change::TRASH;
    c.id = id;
    c.folder = folder;
    c.parent_before = from;
    c.parent_after = now_in;
    c.name_before = c.name_after = nameOf(id);
    c.chain_before = Chain::of(from);
    addChange(*e, c);
}

void LumenAIUndo::recordUntrash(const LLUUID& id, bool folder, const LLUUID& to)
{
    // Out of the Trash is a move from it: undone, it goes back in.
    recordMove(id, folder, trashId(), to);
}

S64 LumenAIUndo::setForNewFolder()
{
    Entry* e = entryFor(0);
    return e ? e->id : -1;
}

void LumenAIUndo::recordNewFolder(S64 set_id, const LLUUID& id, const LLUUID& parent)
{
    if (id.isNull()) return;
    claimParent(id, parent);   // never the person's "New Folder"
    Entry* e = entryFor(set_id == 0 ? -1 : set_id);
    if (!e) return;
    Change c;
    c.kind = Change::NEW_FOLDER;
    c.id = id;
    c.folder = true;
    c.parent_after = parent;
    c.name_before = c.name_after = nameOf(id);
    c.chain_after = Chain::of(parent);
    addChange(*e, c);
}

S64 LumenAIUndo::beginBatch(const std::string& what)
{
    Entry* e = nullptr;
    static LLCachedControl<bool> list_them(gSavedSettings, "LumenAIUndoEndpointCalls", false);
    if (mInRequest || list_them)
    {
        e = &pushEntry(true, mInRequest ? mWords : std::string());
        e->what = what;
    }
    if (!e)
    {
        // Not listed, but still a run: Clear reaches it by this id.
        const S64 id = mNextId++;
        mRunning.insert(id);
        mUnlisted.insert(id);
        return id;
    }
    mRunning.insert(e->id);   // not undone while it is still being made
    return e->id;
}

void LumenAIUndo::endBatch(S64 set_id, bool stopped)
{
    if (set_id == 0) return;
    mRunning.erase(set_id);
    mStopAsked.erase(set_id);
    mUnlisted.erase(set_id);
    if (stopped)
    {
        if (Entry* e = findEntry(set_id)) e->stopped = true;
    }
}

bool LumenAIUndo::stopBulk()
{
    bool any = false;
    for (S64 id : mRunning)
    {
        any = true;
        if (mStopAsked.insert(id).second)
            LL_INFOS("LumenAIUndo") << "bulk run in step " << id << " asked to stop" << LL_ENDL;
    }
    // And an undo or a redo still putting things back.
    for (const std::weak_ptr<RunState>& w : sRuns)
    {
        std::shared_ptr<RunState> st = w.lock();
        if (!st || st->reported) continue;
        any = true;
        if (!st->stop) LL_INFOS("LumenAIUndo") << "putting back asked to stop" << LL_ENDL;
        st->stop = true;
    }
    return any;
}

bool LumenAIUndo::lastTrashedFrom(const LLUUID& id, LLUUID& parent_out) const
{
    auto f = mTrashedFrom.find(id);
    if (f == mTrashedFrom.end()) return false;
    parent_out = f->second;
    return true;
}

// =============================================================================
// Naming a step
// =============================================================================

std::string LumenAIUndo::describe(const Entry& e) const
{
    const LLUUID trash = trashId();
    S32 moved_items = 0, moved_folders = 0, untrashed = 0, trashed_items = 0, trashed_folders = 0;
    S32 renamed = 0, made = 0;
    LLUUID dest;
    bool one_dest = true;
    const Change* first[4] = { nullptr, nullptr, nullptr, nullptr };
    const Change* first_untrash = nullptr;
    for (const Change& c : e.changes)
    {
        switch (c.kind)
        {
        case Change::MOVE:
            if (c.parent_before == trash || within(c.parent_before, trash))
            {
                ++untrashed;
                if (!first_untrash) first_untrash = &c;
                break;
            }
            (c.folder ? moved_folders : moved_items)++;
            if (dest.isNull()) dest = c.parent_after;
            else if (dest != c.parent_after) one_dest = false;
            if (!first[0]) first[0] = &c;
            break;
        case Change::TRASH:
            (c.folder ? trashed_folders : trashed_items)++;
            if (!first[1]) first[1] = &c;
            break;
        case Change::RENAME:
            ++renamed;
            if (!first[2]) first[2] = &c;
            break;
        case Change::NEW_FOLDER:
            ++made;
            if (!first[3]) first[3] = &c;
            break;
        }
    }
    std::vector<std::string> parts;
    if (moved_items + moved_folders > 0)
    {
        std::string what = (moved_items + moved_folders == 1) ? quoted(first[0]->name_after)
                                                              : countOf(moved_items, moved_folders);
        parts.push_back("moved " + what
                        + (one_dest ? " to " + lastName(first[0]->chain_after, dest) : std::string()));
    }
    if (untrashed > 0)
        parts.push_back("took " + (untrashed == 1 ? quoted(first_untrash->name_after)
                                                  : countOf(untrashed, 0)) + " out of the Trash");
    if (trashed_items + trashed_folders > 0)
        parts.push_back("deleted " + (trashed_items + trashed_folders == 1 ? quoted(first[1]->name_before)
                                                                           : countOf(trashed_items, trashed_folders)));
    if (renamed > 0)
        parts.push_back(renamed == 1 ? "renamed " + quoted(first[2]->name_before) + " to "
                                       + quoted(first[2]->name_after)
                                     : llformat("renamed %d things", renamed));
    if (made > 0)
        parts.push_back(made == 1 ? "made folder " + quoted(first[3]->name_after)
                                  : llformat("made %d folders", made));
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) out += (i ? ", " : "") + parts[i];
    if (out.empty()) out = "changed nothing";
    if (e.stopped) out += " (stopped part way)";
    return out;
}

LLSD LumenAIUndo::list() const
{
    auto one = [this](const Entry& e) -> LLSD
    {
        LLSD s;
        s["step"] = (LLSD::Integer)e.id;
        s["who"] = e.assistant ? "assistant" : "you";
        s["did"] = describe(e);
        if (!e.words.empty()) s["asked"] = e.words;
        s["changes"] = (S32)e.changes.size();
        if (mRunning.count(e.id)) s["still_running"] = true;
        return s;
    };
    LLSD out;
    out["undo"] = LLSD::emptyArray();
    out["redo"] = LLSD::emptyArray();
    for (auto it = mUndo.rbegin(); it != mUndo.rend(); ++it)
        if (!(*it)->changes.empty()) out["undo"].append(one(**it));
    for (auto it = mRedo.rbegin(); it != mRedo.rend(); ++it)
        out["redo"].append(one(**it));
    return out;
}

namespace
{
    template <class LIST> typename LIST::value_type newestWithChanges(const LIST& list)
    {
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (!(*it)->changes.empty()) return *it;
        return typename LIST::value_type();
    }
}

std::string LumenAIUndo::menuLabel(bool redo) const
{
    std::shared_ptr<Entry> e = redo ? newestWithChanges(mRedo) : newestWithChanges(mUndo);
    if (!e) return redo ? "Redo" : "Undo";
    std::string label = std::string(redo ? "Redo: " : "Undo: ") + (e->assistant ? "assistant " : "you ")
                      + describe(*e);
    LLWString w = utf8str_to_wstring(label);
    if ((S32)w.size() > MENU_LABEL_MAX)
    {
        w.resize(MENU_LABEL_MAX - 3);
        label = wstring_to_utf8str(w) + "...";
    }
    return label;
}

bool LumenAIUndo::canUndo(bool redo) const
{
    if (puttingBack() || bulkRunning()) return false;
    return (bool)(redo ? newestWithChanges(mRedo) : newestWithChanges(mUndo));
}

std::string LumenAIUndo::notNow(const char* what) const
{
    if (offline())
        return std::string("The viewer is not connected to Second Life (logged out), so nothing can be ")
               + what + " now.";
    if (bulkRunning())
        return std::string("A bulk change is still running, so nothing was ") + what + ". Clear in the "
               "Assistant window stops it; then try again.";
    if (puttingBack())
        return std::string("An undo or a redo is still putting things back, so nothing was ") + what
               + ". Clear in the Assistant window stops it.";
    return std::string();
}

// =============================================================================
// Planning an undo or a redo
// =============================================================================

bool LumenAIUndo::planFor(Entry& e, bool redo, Plan& plan, LLSD& error)
{
    plan.step_id = e.id;
    plan.redo = redo;
    const S32 n = (S32)e.changes.size();
    auto isTrash = [](const LLUUID& id) { return id == trashId() || inTrash(id); };
    for (S32 k = 0; k < n; ++k)
    {
        // Undo goes newest first; redo in the order it was done.
        const S32 i = redo ? k : n - 1 - k;
        const Change& c = e.changes[i];
        if (c.undone != redo) continue;   // undo: not undone yet; redo: undone last time
        const std::string shown = nameOf(c.id).empty() ? (redo ? c.name_before : c.name_after) : nameOf(c.id);
        auto leave = [&](const std::string& why)
        {
            ++plan.left_alone_count;
            addCapped(plan.left_alone, entry(shown, why));
        };
        auto cannot = [&](const std::string& why)
        {
            ++plan.cannot_count;
            addCapped(plan.cannot, entry(shown, why));
        };
        if (c.gone || !exists(c.id))
        {
            cannot("it is not in the inventory any more -- emptied from the Trash, given away or rezzed");
            continue;
        }
        const LLUUID cur = parentOf(c.id);
        Step s;
        s.id = c.id;
        s.folder = c.folder;
        s.label = shown;
        s.row = i + 1;
        s.expect_parent = cur;
        bool make = true;
        switch (c.kind)
        {
        case Change::MOVE:
        {
            const LLUUID from = redo ? c.parent_before : c.parent_after;   // where it should be now
            const LLUUID to = redo ? c.parent_after : c.parent_before;
            if (from == to) { plan.already.push_back(i + 1); make = false; break; }
            if (isTrash(from) ? !isTrash(cur) : cur != from)
            {
                leave("moved since; it is in " + pathOf(cur));
                make = false;
                break;
            }
            if (isTrash(to)) s.kind = Step::TRASH;
            else
            {
                s.kind = Step::MOVE;
                s.target = to;
                s.chain = redo ? c.chain_after : c.chain_before;
            }
            break;
        }
        case Change::TRASH:
            if (!redo)
            {
                if (!isTrash(cur))
                {
                    leave("taken out of the Trash since; it is in " + pathOf(cur));
                    make = false;
                    break;
                }
                s.kind = Step::MOVE;
                s.target = c.parent_before;
                s.chain = c.chain_before;
            }
            else
            {
                if (cur != c.parent_before)
                {
                    leave(isTrash(cur) ? std::string("it is in the Trash already")
                                       : "moved since; it is in " + pathOf(cur));
                    make = false;
                    break;
                }
                s.kind = Step::TRASH;
            }
            break;
        case Change::RENAME:
        {
            const std::string now_name = nameOf(c.id);
            const std::string should = redo ? c.name_before : c.name_after;
            const std::string give = redo ? c.name_after : c.name_before;
            make = false;
            if (now_name == give)
            {
                // Its old name: the rename was refused -- or is not confirmed
                // by Second Life yet, and putting it back now would be undone
                // by the answer.
                if (renamePending(c.id))
                {
                    ++plan.not_yet_count;
                    addCapped(plan.not_yet, entry(shown, "Second Life has not confirmed its new name yet"));
                }
                else plan.already.push_back(i + 1);
                break;
            }
            if (now_name != should)
            {
                leave("renamed since; it is called " + quoted(now_name));
                break;
            }
            s.kind = Step::RENAME;
            s.name = give;
            s.expect_name = now_name;
            make = true;
            break;
        }
        case Change::NEW_FOLDER:
            if (!redo)
            {
                if (isTrash(cur)) { plan.already.push_back(i + 1); make = false; break; }
                s.kind = Step::TRASH;
                s.only_empty = true;
            }
            else
            {
                if (!isTrash(cur))
                {
                    if (cur == c.parent_after) plan.already.push_back(i + 1);
                    else leave("moved since; it is in " + pathOf(cur));
                    make = false;
                    break;
                }
                s.kind = Step::MOVE;
                s.target = c.parent_after;
                s.chain = c.chain_after;
            }
            break;
        }
        if (!make) continue;

        // The shared rules, asked now so the question says it; asked again
        // as each step is done.
        LumenInventoryRules::Rule rule = LumenInventoryRules::ALLOWED;
        std::string why;
        if (s.kind == Step::RENAME) why = LumenInventoryRules::notRename(s.id, s.folder, &rule);
        else if (s.kind == Step::TRASH) why = LumenInventoryRules::notDelete(s.id, s.folder, &rule);
        else if (liveFolder(s.target)) why = LumenInventoryRules::notMove(s.id, s.folder, s.target, &rule);
        else
        {
            why = LumenInventoryRules::held(s.id, s.folder, &rule);
            if (why.empty() && s.folder) why = LumenInventoryRules::inProtected(s.id, true, &rule);
        }
        if (!why.empty())
        {
            if (rule == LumenInventoryRules::PROTECTED_FOLDER) leave(why);
            else cannot(why);
            continue;
        }
        plan.steps.push_back(s);
    }
    (void)error;
    return !plan.steps.empty() || !plan.already.empty();
}

LLSD LumenAIUndo::preview(bool redo)
{
    LLSD out;
    const std::string busy = notNow(redo ? "redone" : "undone");
    if (!busy.empty()) { out["error"] = busy; return out; }
    std::shared_ptr<Entry> e = redo ? newestWithChanges(mRedo) : newestWithChanges(mUndo);
    if (!e)
    {
        out["error"] = redo ? "There is nothing to redo."
                            : "There is nothing to undo: nothing has been changed in the inventory this "
                              "login, or it has all been undone.";
        return out;
    }
    Plan plan;
    LLSD error;
    planFor(*e, redo, plan, error);

    const std::string name = std::string(e->assistant ? "Assistant: " : "You: ") + describe(*e);
    out["step"] = (LLSD::Integer)e->id;
    out["rev"] = e->rev;   // what the question shows: Accept is for this, not more
    out["name"] = name;
    out["who"] = e->assistant ? "assistant" : "you";
    out["did"] = describe(*e);
    if (!e->words.empty()) out["asked"] = e->words;
    out["will_change"] = (S32)plan.steps.size();
    out["left_alone"] = plan.left_alone;
    out["left_alone_count"] = plan.left_alone_count;
    out["cannot"] = plan.cannot;
    out["cannot_count"] = plan.cannot_count;
    if (plan.not_yet_count > 0)
    {
        out["not_yet"] = plan.not_yet;
        out["not_yet_count"] = plan.not_yet_count;
    }
    out["nothing"] = plan.steps.empty() && plan.already.empty() && plan.not_yet_count == 0;

    // The lines the question shows. Short: a list of a hundred moves would
    // fill the screen, and the longer it is the less of it is read.
    const LLUUID trash = trashId();
    S32 moves = 0, move_folders = 0, out_of_trash = 0, to_trash = 0, made_away = 0, renames = 0;
    std::set<LLUUID> targets, sources;
    const Step* one_rename = nullptr;
    for (const Step& s : plan.steps)
    {
        if (s.kind == Step::RENAME) { ++renames; if (!one_rename) one_rename = &s; continue; }
        if (s.kind == Step::TRASH) { (s.only_empty ? made_away : to_trash)++; continue; }
        if (s.expect_parent == trash || within(s.expect_parent, trash)) ++out_of_trash;
        else { (s.folder ? move_folders : moves)++; sources.insert(s.expect_parent); }
        targets.insert(s.target);
    }
    std::vector<std::string> lines;
    if (moves + move_folders > 0)
    {
        std::string l = llformat("Move %s ", countOf(moves, move_folders).c_str()) + (redo ? "" : "back ");
        if (sources.size() == 1) l += "out of " + folderName(*sources.begin()) + ", ";
        l += targets.size() == 1 ? "to " + folderName(*targets.begin())
                                 : llformat("into %d folders", (S32)targets.size());
        if (redo) l += " again";
        lines.push_back(l + ".");
    }
    if (out_of_trash > 0)
        lines.push_back(redo ? llformat("Take %s out of the Trash again.", countOf(out_of_trash, 0).c_str())
                             : llformat("Take %s out of the Trash, back where %s.",
                                        countOf(out_of_trash, 0).c_str(),
                                        out_of_trash == 1 ? "it was" : "they were"));
    if (to_trash > 0)
        lines.push_back(llformat(redo ? "Put %s in the Trash again." : "Put %s back in the Trash.",
                                 countOf(to_trash, 0).c_str()));
    if (made_away > 0)
        lines.push_back(llformat("Put %s made then in the Trash, if empty.", countOf(0, made_away).c_str()));
    if (renames == 1)
        lines.push_back("Rename " + quoted(one_rename->expect_name) + (redo ? " to " : " back to ")
                        + quoted(one_rename->name) + ".");
    else if (renames > 1)
        lines.push_back(llformat(redo ? "Rename %d things again." : "Give %d things their names back.",
                                 renames));
    std::string text;
    // One change says itself in its own line; more get the summary first.
    if (plan.steps.size() > 1)
        for (const std::string& l : lines) text += l + "\n";
    S32 shown = 0;
    for (const Step& s : plan.steps)
    {
        if (shown == SHOWN) break;
        ++shown;
        const std::string indent = plan.steps.size() > 1 ? "    " : "";
        if (s.kind == Step::RENAME)
            text += indent + quoted(s.expect_name) + " \xE2\x86\x92 " + quoted(s.name) + "\n";
        else
            text += indent + s.label + ": " + folderName(s.expect_parent) + " \xE2\x86\x92 "
                  + (s.kind == Step::TRASH ? std::string("Trash") : lastName(s.chain, s.target)) + "\n";
    }
    if ((S32)plan.steps.size() > shown)
        text += llformat("    and %d more.\n", (S32)plan.steps.size() - shown);
    if (plan.left_alone_count > 0)
        text += llformat("Left alone, changed since: %d (", plan.left_alone_count)
              + plan.left_alone[0]["name"].asString() + ": " + plan.left_alone[0]["why"].asString() + ").\n";
    if (plan.cannot_count > 0)
        text += llformat("Cannot come back: %d (", plan.cannot_count)
              + plan.cannot[0]["name"].asString() + ": " + plan.cannot[0]["why"].asString() + ").\n";
    if (plan.not_yet_count > 0)
        text += llformat("Not yet: %d -- Second Life has not confirmed a new name; try again in a moment.\n",
                         plan.not_yet_count);
    if (plan.steps.empty())
        text += plan.not_yet_count > 0 ? "Nothing else to put back.\n" : "Nothing needs putting back.\n";
    if (!text.empty() && text.back() == '\n') text.pop_back();
    out["text"] = text;
    out["changes_shown"] = shown;
    return out;
}

LLSD LumenAIUndo::perform(bool redo, S64 step, S32 rev)
{
    LLSD out;
    const std::string busy = notNow(redo ? "redone" : "undone");
    if (!busy.empty()) { out["error"] = busy; return out; }
    std::deque<std::shared_ptr<Entry>>& list = redo ? mRedo : mUndo;
    std::shared_ptr<Entry> e = newestWithChanges(list);
    if (!e || e->id != step)
    {
        out["error"] = redo ? "Nothing was redone: the list changed after the question was asked."
                            : "Nothing was undone: the list changed after the question was asked -- "
                              "something new was done, or that step was already undone.";
        return out;
    }
    // <Lumen> The step itself grew while the question was up -- the
    // assistant's turn still adding to it: Accept was for what it showed.
    if (rev >= 0 && e->rev != rev)
    {
        out["error"] = std::string("Nothing was ") + (redo ? "redone" : "undone") + ": more was added "
                       "to that step after the question was asked. Ask again to see all of it.";
        out["grew"] = true;
        return out;
    }
    // Planned again now, after the question: what changed while it was up is left alone.
    Plan plan;
    LLSD error;
    planFor(*e, redo, plan, error);
    const std::string said = std::string(e->assistant ? "assistant " : "you ") + describe(*e);

    // Off its list while it runs, so it is not asked for twice.
    list.erase(std::find(list.begin(), list.end(), e));
    if (mRequestStep == e->id) mRequestStep = 0;   // the turn's later changes start a new step

    if (plan.steps.empty() && plan.already.empty() && plan.not_yet_count == 0)
    {
        // Nothing in it can be put back, or through: it is not offered again,
        // so the step under it can be.
        std::string why = plan.left_alone_count > 0 ? plan.left_alone[0]["why"].asString()
                        : plan.cannot_count > 0 ? plan.cannot[0]["why"].asString() : std::string();
        toast(std::string("Nothing in \"") + said + "\" can be " + (redo ? "redone" : "undone")
              + " now" + (why.empty() ? std::string() : " -- " + why) + ". It is off the list.");
        out["nothing"] = true;
        out["left_alone"] = plan.left_alone;
        out["left_alone_count"] = plan.left_alone_count;
        out["cannot"] = plan.cannot;
        out["cannot_count"] = plan.cannot_count;
        out["step"] = (LLSD::Integer)step;
        return out;
    }

    mBusy = e;
    const U64 revision = mRevision;
    const F64 started = LLTimer::getTotalSeconds();
    const std::vector<S64> already = plan.already;
    const S32 not_yet = plan.not_yet_count;
    const S32 cannot = plan.cannot_count;
    const std::string cannot_why = cannot > 0 ? plan.cannot[0]["why"].asString() : std::string();
    LL_INFOS("LumenAIUndo") << (redo ? "redo" : "undo") << " of step " << step << ": "
                            << plan.steps.size() << " to do" << LL_ENDL;

    LLSD result = run(plan, [this, e, redo, revision, started, already, not_yet, said, cannot, cannot_why]
        (const LLSD& s, const std::vector<S64>& rows, bool)
    {
        if (!LumenAIUndo::instanceExists()) return;
        std::vector<S64> all = rows;
        all.insert(all.end(), already.begin(), already.end());
        for (S64 row : all)
        {
            if (row >= 1 && row <= (S64)e->changes.size()) e->changes[row - 1].undone = !redo;
        }
        if (mBusy == e) mBusy.reset();
        // <Lumen> Back in the undo list where it belongs: under anything
        // changed while it ran, which is newer.
        auto backInUndo = [this, &e, started]()
        {
            auto at = mUndo.end();
            for (auto it = mUndo.begin(); it != mUndo.end(); ++it)
            {
                if ((*it)->when > started) { at = it; break; }
            }
            mUndo.insert(at, e);
            while (mUndo.size() > MAX_STEPS) mUndo.pop_front();
        };
        if (all.empty())
        {
            // Nothing done: back where it was if a name was only waiting for
            // Second Life, so it can be tried again; otherwise off the list.
            if (not_yet > 0)
            {
                if (redo) { if (mRevision == revision) mRedo.push_back(e); }
                else backInUndo();
            }
        }
        else if (redo)
        {
            backInUndo();   // it can be undone again
        }
        else if (mRevision == revision)
        {
            mRedo.push_back(e);   // a change made meanwhile -- new, or added to a step -- clears it
        }

        // The toast: what was done, and what could not be after all.
        const S32 put = (S32)already.size() + s["put_back"].asInteger();
        std::string line = std::string(redo ? "Redone: " : "Undone: ") + said + ". "
                         + llformat("%d put %s", put, redo ? "through" : "back") + ".";
        const S32 left = s["left_alone_count"].asInteger();
        if (left > 0) line += llformat(" %d left alone: changed since.", left);
        const S32 failed = s["could_not"].asInteger() + cannot;
        if (failed > 0)
        {
            std::string why = cannot_why;
            if (s["failed"].size() > 0) why = s["failed"][0]["why"].asString();
            line += llformat(" %d could not be", failed) + (why.empty() ? std::string(".") : " -- " + why + ".");
        }
        if (s.has("not_done") && s["not_done"].asInteger() > 0)
            line += llformat(" Stopped part way: %d not done.", s["not_done"].asInteger());
        if (not_yet > 0) line += llformat(" %d not yet: Second Life had not confirmed a name.", not_yet);
        toast(line);
    });
    result["step"] = (LLSD::Integer)step;
    result["did"] = said;
    result["already"] = (S32)already.size();
    if (plan.not_yet_count > 0)
    {
        result["not_yet"] = plan.not_yet;
        result["not_yet_count"] = plan.not_yet_count;
    }
    return result;
}
