/**
 * @file lumenaiundo.cpp
 * @brief What the assistant changed in the inventory, and how to put it back.
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
#include "lumenaichat.h"   // <Lumen> where a long undo says it finished

#include "aoengine.h"           // <Lumen> the AO folder, for the shared rules
#include "fsfloaterwearablefavorites.h"   // <Lumen> the wearable favourites folder, likewise
#include "fslslbridge.h"        // <Lumen> the LSL bridge's folder, likewise
#include "llagent.h"
#include "llapp.h"
#include "llbutton.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "llframetimer.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"   // <Lumen> is a snapshot whole?
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "lltexteditor.h"
#include "lltimer.h"
#include "llviewerinventory.h"
#include "lumenfolders.h"
#include "rlvactions.h"
#include "rlvdefines.h"
#include "rlvlocks.h"

#include <sqlite3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <unordered_map>
#include <unordered_set>

namespace
{
    const char* UNDO_FILE = "inventory_undo.db";
    const char* LOCK_FILE = "inventory_undo.lock";   // <Lumen> one viewer at a time

    // Not a cache, so there is no "rebuild on mismatch": a newer shape must
    // migrate. Bump only with a migration beside it.
    // <Lumen> 2: changes.undone_at (which steps an undo really did, so a later
    // undo tries only the rest) and snapshots.partial (taken before the whole
    // inventory had loaded). The migration is in openFile().
    const int SCHEMA_VERSION = 2;

    const char* SCHEMA =
        "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sets ("
        "  id INTEGER PRIMARY KEY, at INTEGER NOT NULL, words TEXT NOT NULL,"
        "  source TEXT NOT NULL, undone_at INTEGER, undo_note TEXT);"
        "CREATE TABLE IF NOT EXISTS changes ("
        "  set_id INTEGER NOT NULL, seq INTEGER NOT NULL, kind TEXT NOT NULL,"
        "  object_id TEXT NOT NULL, folder INTEGER NOT NULL,"
        "  name_before TEXT NOT NULL, name_after TEXT NOT NULL,"
        "  parent_before TEXT NOT NULL, parent_after TEXT NOT NULL, chain TEXT NOT NULL,"
        "  undone_at INTEGER);"
        "CREATE INDEX IF NOT EXISTS changes_set ON changes(set_id);"
        "CREATE INDEX IF NOT EXISTS changes_object ON changes(object_id);"
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "  id INTEGER PRIMARY KEY, at INTEGER NOT NULL, reason TEXT NOT NULL,"
        "  items INTEGER NOT NULL, folders INTEGER NOT NULL,"
        "  capture_ms INTEGER NOT NULL, write_ms INTEGER NOT NULL,"
        "  partial INTEGER NOT NULL DEFAULT 0);"
        // One row per object per stretch of snapshots in which it did not
        // change: valid for from_snap <= S < to_snap (to_snap NULL = still).
        // An unchanged inventory therefore costs one snapshots row.
        "CREATE TABLE IF NOT EXISTS nodes ("
        "  id TEXT NOT NULL, folder INTEGER NOT NULL, parent TEXT NOT NULL,"
        "  name TEXT NOT NULL, from_snap INTEGER NOT NULL, to_snap INTEGER);"
        "CREATE INDEX IF NOT EXISTS nodes_open ON nodes(to_snap);";

    // Kept: a week of snapshots, at most ten, and never fewer than the three
    // newest whatever their age. Change sets: thirty days.
    const S64 SNAP_KEEP_SECONDS = 7 * 24 * 3600;
    const int SNAP_KEEP_MAX = 10;
    const int SNAP_KEEP_MIN = 3;
    const S64 SET_KEEP_SECONDS = 30 * 24 * 3600;

    // Above this many steps a run is paced in a coroutine rather than done in
    // the call: thousands of moves in one frame are thousands of requests to
    // Second Life at once.
    const size_t RUN_NOW_LIMIT = 100;

    // A reply carries at most this many names per list, with the full count.
    const S32 LIST_CAP = 30;

    // <Lumen> How long an undo waits for a folder it asked Second Life to make
    // again before it reports without it; the same as a bulk run waits.
    const F64 FOLDER_WAIT = 30.0;

    // <Lumen> How long a reply waits for the record to be written before it
    // says so instead. A snapshot of 65,000 items is written in under a tenth
    // of a second; this is for a slow disk, not the usual case.
    const F32 FLUSH_SECONDS = 2.f;

    // <Lumen> Held by another viewer: ask again this often.
    const F64 LOCK_RETRY = 15.0;

    std::string whenText(S64 t)
    {
        if (t <= 0) return std::string();
        time_t tt = (time_t)t;
        std::tm lt{};
#if LL_WINDOWS
        localtime_s(&lt, &tt);
#else
        localtime_r(&tt, &lt);
#endif
        char buf[32];
        strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &lt);
        return buf;
    }

    std::string col(sqlite3_stmt* st, int i)
    {
        const unsigned char* t = sqlite3_column_text(st, i);
        return t ? std::string(reinterpret_cast<const char*>(t)) : std::string();
    }

    void bindText(sqlite3_stmt* st, int i, const std::string& s)
    {
        sqlite3_bind_text(st, i, s.c_str(), (int)s.size(), SQLITE_TRANSIENT);
    }

    /**
     * Run a prepared statement one step; say so in the log when it fails.
     * <Lumen> And say so to the caller: a write that is not checked is how a
     * failed insert once went unnoticed, and how a snapshot could be
     * committed half written.
     */
    bool stepDone(sqlite3* db, sqlite3_stmt* st, const char* what)
    {
        const int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        {
            LL_WARNS("LumenAIUndo") << what << " failed: " << sqlite3_errmsg(db) << LL_ENDL;
            return false;
        }
        return true;
    }

    bool prepared(sqlite3* db, const char* sql, sqlite3_stmt** st)
    {
        if (sqlite3_prepare_v2(db, sql, -1, st, nullptr) == SQLITE_OK) return true;
        LL_WARNS("LumenAIUndo") << "cannot prepare: " << sqlite3_errmsg(db) << " -- " << sql << LL_ENDL;
        *st = nullptr;
        return false;
    }

    bool execOn(sqlite3* db, const char* sql)
    {
        char* err = nullptr;
        if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK)
        {
            LL_WARNS("LumenAIUndo") << "SQL failed: " << (err ? err : "(no message)") << LL_ENDL;
            if (err) sqlite3_free(err);
            return false;
        }
        return true;
    }

    // <Lumen>
    /**
     * End a transaction: COMMIT when everything in it went, ROLLBACK when
     * anything did not -- or when the COMMIT itself fails, which leaves the
     * transaction open, so that every later write would have gone into it and
     * been lost with it. Never left open either way.
     */
    bool finishTransaction(sqlite3* db, bool ok, const char* what)
    {
        if (ok) ok = execOn(db, "COMMIT;");
        if (!ok)
        {
            LL_WARNS("LumenAIUndo") << what << ": not written, rolled back" << LL_ENDL;
            if (!sqlite3_get_autocommit(db)) execOn(db, "ROLLBACK;");
        }
        return ok;
    }

    /** Does this error say the FILE is damaged -- not busy, full or read-only? */
    bool corruptCode(int rc)
    {
        rc &= 0xff;   // the primary code, from an extended one
        return rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB;
    }

    bool hasColumn(sqlite3* db, const char* table, const char* column)
    {
        bool found = false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, llformat("PRAGMA table_info(%s);", table).c_str(), -1, &st, nullptr)
            == SQLITE_OK)
        {
            while (!found && sqlite3_step(st) == SQLITE_ROW) found = col(st, 1) == column;
            sqlite3_finalize(st);
        }
        return found;
    }
    // </Lumen>

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
        return gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
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
// </Lumen>

// =============================================================================
// Chain: where a folder sat, by id and by name, so it can be made again.
// =============================================================================

std::string LumenAIUndo::Chain::serialise() const
{
    std::string out;
    for (const auto& f : folders)
    {
        std::string n = f.second;
        for (char& ch : n) if (ch == '\t' || ch == '\n') ch = ' ';
        out += f.first.asString() + "\t" + n + "\n";
    }
    return out;
}

LumenAIUndo::Chain LumenAIUndo::Chain::parse(const std::string& text)
{
    Chain c;
    size_t start = 0;
    while (start < text.size())
    {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(start, end - start);
        const size_t tab = line.find('\t');
        if (tab != std::string::npos)
        {
            c.folders.emplace_back(LLUUID(line.substr(0, tab)), line.substr(tab + 1));
        }
        start = end + 1;
    }
    return c;
}

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
    S64 row = 0;           // <Lumen> undo: the change it reverses, marked once done
};

struct LumenAIUndo::Plan
{
    std::vector<Step> steps;
    LLSD left_alone = LLSD::emptyArray();
    LLSD cannot = LLSD::emptyArray();
    LLSD refused = LLSD::emptyArray();   // <Lumen> restore: held where it is by the rules
    S32 left_alone_count = 0;
    S32 cannot_count = 0;
    S32 refused_count = 0;               // <Lumen>
    S32 earlier = 0;                     // <Lumen> undo: put back by an earlier undo of the set
    bool again = false;                  // <Lumen> undo: the set was undone before
    bool partial = false;                // <Lumen> restore: not a snapshot of all of it
    S64 set_id = 0;
    S64 snap_id = 0;
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
        S32 outstanding = 0;   // moves waiting for a folder to be made again
        bool all_started = false;
        bool reported = false;
        bool in_call = false;  // <Lumen> run() has not answered yet
        bool gave_up = false;  // <Lumen> stopped waiting for folders to be made again
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
        for (const LumenAIUndo::Step& f : folders) doStep(f, st);   // never waits: no folder is made
        if (st->finished) st->finished(st->summary(), st->rows, !st->in_call);
    }

    /**
     * A folder Second Life never makes again would leave the run waiting for
     * ever -- never reported, and an undo never marked. After a while it
     * reports without it; what still waited stays where it is.
     */
    void waitForFolders(const std::shared_ptr<RunState>& st)
    {
        if (st->outstanding == 0 || st->reported) return;
        LLCoros::instance().launch("LumenAIUndoWait", [st]()
        {
            const F64 give_up = LLTimer::getTotalSeconds() + FOLDER_WAIT;
            while (!st->reported && LLTimer::getTotalSeconds() < give_up)
            {
                if (LLApp::isExiting()) return;
                llcoro::suspendUntilTimeout(0.25f);
            }
            if (st->reported) return;
            LL_WARNS("LumenAIUndo") << st->outstanding << " still waiting for a folder to be made "
                                    << "again; reporting without them" << LL_ENDL;
            st->gave_up = true;
            st->couldNot(llformat("%d more", st->outstanding),
                         "Second Life did not make the folder they were in again in time, so they "
                         "stay where they are");
            st->failed_count += st->outstanding - 1;
            st->outstanding = 0;
            finishIfDone(st);
        });
    }
    // </Lumen>

    /** Move now; true when the viewer's own model shows it where it belongs. */
    bool moveNow(const LLUUID& id, bool folder, const LLUUID& target, std::string& why,
                 LumenInventoryRules::Rule* rule = nullptr)
    {
        // <Lumen> The rules every path shares: move_item's, a batch's, this.
        why = LumenInventoryRules::notMove(id, folder, target, rule);
        if (!why.empty()) return false;
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
        switch (s.kind)
        {
        case LumenAIUndo::Step::RENAME:
        {
            if (!exists(s.id)) { why = "it is no longer in inventory"; break; }
            why = LumenInventoryRules::notRename(s.id, s.folder, &rule);   // <Lumen>
            if (!why.empty()) break;
            if (s.folder)
            {
                rename_category(&gInventory, s.id, s.name);
            }
            else
            {
                LLSD updates; updates["name"] = s.name;
                update_inventory_item(s.id, updates, NULL);
            }
            ++st->renamed;
            st->didIt(s, "renamed back to \"" + s.name
                         + "\" (Second Life confirms a rename a moment later)");
            return;
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
            if (s.folder)
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
                if (!get_is_category_removable(&gInventory, s.id))
                {
                    why = "the viewer does not allow removing that folder";
                    break;
                }
                gInventory.removeCategory(s.id);
            }
            else
            {
                if (!get_is_item_removable(&gInventory, s.id, true))
                {
                    why = "the viewer does not allow removing it";
                    break;
                }
                gInventory.removeItem(s.id);
            }
            if (!inTrash(s.id)) { why = "the viewer did not move it to the Trash"; break; }
            st->didIt(s, s.folder ? "the folder it made, now empty, moved to the Trash"
                                  : "moved back to the Trash");
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
            // <Lumen> Not a folder made again for something that may not move.
            why = LumenInventoryRules::inProtected(s.id, s.folder, &rule);
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

// <Lumen> A long undo or restore says it has finished where the person asked
// for it: in the Assistant window, once the turn that started it has ended (a
// note in the middle of the assistant's own reply would read as part of it),
// as a bulk run does. A notice when that window is not on screen.
namespace
{
    void announce(const std::string& line)
    {
        LLCoros::instance().launch("LumenAIUndoSays", [line]()
        {
            const F64 give_up = LLTimer::getTotalSeconds() + 60.0;
            while (LumenAIChatFloater::turnRunning() && LLTimer::getTotalSeconds() < give_up)
            {
                if (LLApp::isExiting()) return;
                llcoro::suspendUntilTimeout(0.5f);
            }
            if (!LumenAIChatFloater::postFromViewer(std::string(), line))
            {
                LLSD args; args["MESSAGE"] = line;
                LLNotificationsUtil::add("SystemMessageTip", args);
            }
        });
    }
}

// <Lumen> How many undos and restores are still putting things back. Emptying
// the Trash waits for none: it would purge what they are taking out of it.
namespace { S32 sPuttingBack = 0; }

bool LumenAIUndo::puttingBack() const
{
    return sPuttingBack > 0;
}

LLSD LumenAIUndo::run(Plan& plan, const Finished& finished)
{
    std::shared_ptr<RunState> st = std::make_shared<RunState>();
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
        if (s.kind == Step::TRASH && s.folder) st->folder_steps.push_back(s);
        else steps.push_back(s);
    }

    LLSD out;
    out["cannot"] = plan.cannot;
    out["cannot_count"] = plan.cannot_count;
    if (plan.refused_count > 0)
    {
        out["refused"] = plan.refused;
        out["refused_count"] = plan.refused_count;
    }
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
        out["waiting_for_folders"] = st->outstanding;
        out["finished"] = st->reported;
        waitForFolders(st);
        return out;
    }

    // Many steps: paced, so neither the frame nor Second Life takes them at once.
    out["left_alone"] = plan.left_alone;
    out["left_alone_count"] = plan.left_alone_count;
    LLCoros::instance().launch("LumenAIUndoRun", [st, steps]()
    {
        S32 n = 0;
        for (const Step& s : steps)
        {
            if (LLApp::isExiting()) return;
            if (!gInventory.isInventoryUsable()) break;   // logged out part way: say what was done
            doStep(s, st);
            if (++n % 25 == 0) llcoro::suspendUntilTimeout(0.1f);
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
// The file, and the thread that writes it
// =============================================================================

LumenAIUndo::LumenAIUndo()
{
}

LumenAIUndo::~LumenAIUndo()
{
    stopWorker();
    close();
    unlockRecord();
}

void LumenAIUndo::close()
{
    if (mRead)
    {
        sqlite3_close(mRead);
        mRead = nullptr;
    }
}

bool LumenAIUndo::available()
{
    return ensureOpen();
}

bool LumenAIUndo::ensureOpen()
{
    if (mRead) return true;
    // <Lumen> Tried once a login -- except when it could not be had for now
    // (another viewer holding it, a busy disk), which is asked again now and
    // then: that viewer may have been closed since.
    if (mTried && (mRetryAt <= 0.0 || LLTimer::getTotalSeconds() < mRetryAt)) return false;
    // The account's own folder is set at login; before that there is nobody
    // whose inventory this would be.
    if (gDirUtilp->getLindenUserDir().empty() || !gInventory.isInventoryUsable())
    {
        mWhyUnavailable = "the inventory has not loaded yet";
        return false;
    }
    mTried = true;
    mRetryAt = 0.0;
    mWhyUnavailable.clear();
    return open();
}

// <Lumen>
bool LumenAIUndo::lockRecord()
{
    if (mLock) return true;
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, LOCK_FILE);
    if (sqlite3_open_v2(path.c_str(), &mLock, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
    {
        LL_WARNS("LumenAIUndo") << "cannot open " << path << ": "
                                << (mLock ? sqlite3_errmsg(mLock) : "?") << LL_ENDL;
        unlockRecord();
        mWhyUnavailable = "its lock file could not be opened";
        return false;
    }
    // Two viewers on one account would both number their change sets from
    // the same MAX(id), and each undo the other's. So the record is held: an
    // exclusive transaction on a file of its own, never committed. The
    // operating system keeps that lock while this connection is open and lets
    // go when the viewer quits or dies, so a crash leaves nothing stale
    // behind. Nothing is written, so no journal. A second viewer is told
    // SQLITE_BUSY at once -- there is no busy timeout on this connection.
    char* err = nullptr;
    const int rc = sqlite3_exec(mLock, "PRAGMA journal_mode=OFF; BEGIN EXCLUSIVE;", nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        const int code = rc & 0xff;
        LL_WARNS("LumenAIUndo") << "the inventory record is not ours to use: "
                                << (err ? err : "(no message)") << LL_ENDL;
        if (err) sqlite3_free(err);
        unlockRecord();
        if (code == SQLITE_BUSY || code == SQLITE_LOCKED)
        {
            mWhyUnavailable = "another Lumen on this computer, logged in as this avatar, is using it "
                              "-- it is free again once that one is closed";
            mRetryAt = LLTimer::getTotalSeconds() + LOCK_RETRY;
        }
        else
        {
            mWhyUnavailable = "its lock file could not be used";
        }
        return false;
    }
    return true;
}

void LumenAIUndo::unlockRecord()
{
    if (mLock)
    {
        sqlite3_close(mLock);   // the open transaction is rolled back, and the lock goes with it
        mLock = nullptr;
    }
}

LumenAIUndo::OpenResult LumenAIUndo::openFile()
{
    // Only an error that says the FILE is damaged sets it aside. Busy, full,
    // read-only or an I/O error is the computer's state, not the record's:
    // the record is left where it is and asked for again later.
    auto failed = [this](const char* what, int code) -> OpenResult
    {
        LL_WARNS("LumenAIUndo") << what << " failed on " << mPath << ": "
                                << (mRead ? sqlite3_errmsg(mRead) : "?") << LL_ENDL;
        return corruptCode(code) ? BAD_FILE : NOT_NOW;
    };

    const int rc = sqlite3_open_v2(mPath.c_str(), &mRead, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK)
    {
        const OpenResult r = failed("opening the record", rc);
        close();
        return r;
    }
    sqlite3_busy_timeout(mRead, 3000);

    // The whole file is read, once a login, here on the main thread. Measured
    // at 8 ms for a record of 65,000 items (8.4 MB). It stays here because the
    // writer thread could only check it after this thread had already read
    // and changed the file -- the WAL switch, the schema, the migration --
    // and a damaged file found that late would leave the first changes of
    // the login half in one file and half in the next.
    {
        LLTimer t;
        sqlite3_stmt* st = nullptr;
        int step = sqlite3_prepare_v2(mRead, "PRAGMA quick_check;", -1, &st, nullptr);
        std::string verdict;
        if (step == SQLITE_OK)
        {
            step = sqlite3_step(st);
            if (step == SQLITE_ROW) verdict = col(st, 0);
            sqlite3_finalize(st);
        }
        if (step != SQLITE_ROW) return failed("checking the record", step);
        LL_INFOS("LumenAIUndo") << "record checked in " << (S32)(t.getElapsedTimeF32() * 1000.f)
                                << " ms" << LL_ENDL;
        if (verdict != "ok")
        {
            LL_WARNS("LumenAIUndo") << "the record failed its check: " << verdict << LL_ENDL;
            return BAD_FILE;
        }
    }
    if (!execOn(mRead, "PRAGMA journal_mode=WAL;")) return failed("WAL", sqlite3_errcode(mRead));

    // Is this file from a shape we know? Asked before anything in it changes.
    S32 version = 0;
    {
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(mRead, "SELECT v FROM meta WHERE k='schema';", -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW) version = atoi(col(st, 0).c_str());
            sqlite3_finalize(st);
        }
        else if (corruptCode(sqlite3_errcode(mRead)))
        {
            return failed("reading the record's version", sqlite3_errcode(mRead));
        }
        // Otherwise there is no meta table yet: a new file.
    }
    if (version > SCHEMA_VERSION)
    {
        // Written by a newer Lumen. Reading it might misread it; leave it be.
        LL_WARNS("LumenAIUndo") << "the inventory record is from a newer version ("
                                << version << "); not used" << LL_ENDL;
        return TOO_NEW;
    }

    if (!execOn(mRead, SCHEMA)) return failed("the schema", sqlite3_errcode(mRead));

    // Version 1 to 2: two columns, added where they are missing (a new file
    // has them from the schema above), in one transaction with the version.
    if (version < SCHEMA_VERSION)
    {
        bool ok = execOn(mRead, "BEGIN;");
        if (ok && !hasColumn(mRead, "changes", "undone_at"))
        {
            // An undo that version 1 recorded counted as whole, so it reads as whole.
            ok = execOn(mRead, "ALTER TABLE changes ADD COLUMN undone_at INTEGER;")
              && execOn(mRead, "UPDATE changes SET undone_at=(SELECT s.undone_at FROM sets s "
                               "WHERE s.id=changes.set_id) WHERE set_id IN "
                               "(SELECT id FROM sets WHERE undone_at IS NOT NULL);");
        }
        if (ok && !hasColumn(mRead, "snapshots", "partial"))
        {
            ok = execOn(mRead, "ALTER TABLE snapshots ADD COLUMN partial INTEGER NOT NULL DEFAULT 0;");
        }
        if (ok)
        {
            ok = execOn(mRead, llformat("INSERT OR REPLACE INTO meta VALUES('schema','%d');",
                                        SCHEMA_VERSION).c_str());
        }
        const int code = ok ? SQLITE_OK : sqlite3_errcode(mRead);
        if (!finishTransaction(mRead, ok, "bringing the record up to date"))
        {
            return failed("bringing the record up to date", code);
        }
        if (version > 0)
        {
            LL_INFOS("LumenAIUndo") << "inventory record brought from version " << version << " to "
                                    << SCHEMA_VERSION << LL_ENDL;
        }
    }
    return OPENED;
}

void LumenAIUndo::setAside()
{
    // Not a cache: never delete it. Put it aside where it can still be
    // looked at, and start a new one.
    const std::string aside = mPath + ".set-aside-" + llformat("%lld", (long long)time(nullptr));
    LL_WARNS("LumenAIUndo") << "the inventory record is damaged; kept as " << aside
                            << " and a new one started" << LL_ENDL;
    LLFile::rename(mPath, aside);
    LLFile::rename(mPath + "-wal", aside + "-wal");
    LLFile::rename(mPath + "-shm", aside + "-shm");
}
// </Lumen>

bool LumenAIUndo::open()
{
    mPath = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, UNDO_FILE);
    if (!lockRecord()) return false;   // <Lumen>

    // <Lumen> Set aside only a damaged file, and only once: a new file that
    // fails too is the computer's trouble, not the record's.
    OpenResult r = openFile();
    if (r == BAD_FILE)
    {
        close();
        setAside();
        r = openFile();
    }
    if (r != OPENED)
    {
        close();
        unlockRecord();
        if (r == TOO_NEW)
        {
            mWhyUnavailable = "it was written by a newer version of Lumen";
        }
        else if (r == BAD_FILE)
        {
            mWhyUnavailable = "it was damaged, and a new one could not be started";
        }
        else
        {
            mWhyUnavailable = "its file could not be opened for now (a busy, full or read-only disk)";
            mRetryAt = LLTimer::getTotalSeconds() + LOCK_RETRY;
        }
        return false;
    }
    // </Lumen>

    auto maxOf = [this](const char* sql) -> S64
    {
        S64 v = 0;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(mRead, sql, -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        return v;
    };
    // Safe to number from here only because the record is held: no other
    // viewer is numbering from the same place.
    mNextSet  = maxOf("SELECT IFNULL(MAX(id),0) FROM sets;") + 1;
    mNextSnap = maxOf("SELECT IFNULL(MAX(id),0) FROM snapshots;") + 1;

    startWorker();
    LL_INFOS("LumenAIUndo") << "inventory record open: " << mPath << LL_ENDL;
    return true;
}

void LumenAIUndo::startWorker()
{
    const std::string path = mPath;
    mStop = false;
    mWorker = std::thread([this, path]()
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READWRITE, nullptr) != SQLITE_OK)
        {
            LL_WARNS("LumenAIUndo") << "the writer could not open the record" << LL_ENDL;
            if (db) sqlite3_close(db);
            db = nullptr;
        }
        else
        {
            sqlite3_busy_timeout(db, 10000);
        }
        for (;;)
        {
            std::function<void(sqlite3*)> job;
            {
                std::unique_lock<std::mutex> lock(mMutex);
                mWake.wait(lock, [this] { return mStop || !mJobs.empty(); });
                if (mJobs.empty())
                {
                    if (mStop) break;
                    continue;
                }
                job = std::move(mJobs.front());
                mJobs.pop_front();
                mWorking = true;
            }
            if (db)
            {
                job(db);
                // <Lumen> No job leaves a transaction open: one that did
                // would take every later write into it, and lose them with it.
                if (!sqlite3_get_autocommit(db))
                {
                    LL_WARNS("LumenAIUndo") << "a write left its transaction open; rolled back" << LL_ENDL;
                    execOn(db, "ROLLBACK;");
                }
            }
            {
                std::lock_guard<std::mutex> lock(mMutex);
                mWorking = false;
                if (mJobs.empty()) mIdle.notify_all();
            }
        }
        if (db) sqlite3_close(db);
    });
}

void LumenAIUndo::stopWorker()
{
    if (!mWorker.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mStop = true;
    }
    mWake.notify_all();
    mWorker.join();   // what was posted is written first
}

void LumenAIUndo::post(std::function<void(sqlite3*)> job)
{
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mJobs.push_back(std::move(job));
    }
    mWake.notify_one();
}

bool LumenAIUndo::flush()
{
    // Bounded: a snapshot being written takes a fraction of a second, and a
    // reply that would wait longer than this says so instead.
    LLTimer t;
    bool settled = false;
    {
        std::unique_lock<std::mutex> lock(mMutex);
        settled = mIdle.wait_for(lock, std::chrono::milliseconds((S64)(FLUSH_SECONDS * 1000.f)),
                                 [this] { return mJobs.empty() && !mWorking; });
    }
    // <Lumen> Said in the log when it is felt, so a slow disk can be seen.
    const S32 ms = (S32)(t.getElapsedTimeF32() * 1000.f);
    if (ms > 20 || !settled)
    {
        LL_INFOS("LumenAIUndo") << "waited " << ms << " ms for the record to be written"
                                << (settled ? "" : ", and stopped waiting") << LL_ENDL;
    }
    return settled;
}

void LumenAIUndo::changed()
{
    // Called from the writer thread too, so it only raises a flag; the window
    // reads the record again on the main thread when it next draws.
    mDirty = true;
}

// =============================================================================
// Recording
// =============================================================================

void LumenAIUndo::beginRequest(const std::string& words)
{
    mInRequest = true;
    mWords = words;
    mRequestSet = 0;
}

void LumenAIUndo::endRequest()
{
    mInRequest = false;
    mWords.clear();
    mRequestSet = 0;
}

void LumenAIUndo::prepare()
{
    if (!ensureOpen()) return;
    if (!mSnapshotThisLogin)
    {
        mSnapshotThisLogin = true;
        takeSnapshot("before the assistant's first change this login");
    }
}

S64 LumenAIUndo::currentSet()
{
    if (!ensureOpen()) return 0;
    // <Lumen> Never one that has been undone: undo() lets go of it.
    if (mInRequest && mRequestSet != 0 && !mClosed.count(mRequestSet)) return mRequestSet;

    const S64 id = mNextSet++;
    if (mInRequest) mRequestSet = id;
    // <Lumen> The order of changes is NOT restarted per set any more: a bulk
    // run writes to its own set while a turn writes to another, and a counter
    // reset by one would have put the other's later changes before its
    // earlier ones -- which undo, newest first, reads as the order to reverse.
    // It only has to rise within a set, and it does.
    addSet(id, mInRequest ? mWords : std::string(), mInRequest ? "assistant" : "endpoint");
    return id;
}

// <Lumen>
/**
 * The set a change goes in: the one asked for, unless that one has been
 * undone (or is being). Something landing in an undone set could never be
 * undone with it -- a folder Second Life made after "undo that", or a run's
 * folder answered after the run was undone. It joins the turn in progress,
 * or a set of its own, either of which "undo" can still reach.
 */
S64 LumenAIUndo::liveSet(S64 wanted)
{
    if (wanted == 0) return currentSet();
    if (!mClosed.count(wanted)) return wanted;
    LL_INFOS("LumenAIUndo") << "a change for change set " << wanted
                            << ", which is undone, goes in another set" << LL_ENDL;
    if (mInRequest) return currentSet();
    std::map<S64, S64>::const_iterator late = mLateSets.find(wanted);
    if (late != mLateSets.end() && !mClosed.count(late->second)) return late->second;
    if (!ensureOpen()) return 0;
    const S64 id = mNextSet++;
    addSet(id, llformat("(arrived after change set %lld was undone)", (long long)wanted), "late");
    mLateSets[wanted] = id;
    return id;
}
// </Lumen>

void LumenAIUndo::addSet(S64 id, const std::string& words, const std::string& source)
{
    const S64 at = (S64)time(nullptr);
    post([this, id, at, words, source](sqlite3* db)
    {
        sqlite3_stmt* st = nullptr;
        if (prepared(db, "INSERT INTO sets(id,at,words,source) VALUES(?,?,?,?);", &st))
        {
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_int64(st, 2, at);
            bindText(st, 3, words);
            bindText(st, 4, source);
            stepDone(db, st, "recording a change set");
            sqlite3_finalize(st);
        }
        changed();   // <Lumen> once it is there to be read
    });
}

// <Lumen> A bulk run's own set, outside the turn's. See the header.
S64 LumenAIUndo::beginBatch(const std::string& what)
{
    if (!ensureOpen()) return 0;
    const S64 id = mNextSet++;
    const std::string words = (mInRequest && !mWords.empty()) ? mWords + "  [" + what + "]" : what;
    addSet(id, words, "bulk");
    mRunning.insert(id);   // not undone while it is still being written
    return id;
}

void LumenAIUndo::endBatch(S64 set_id)
{
    // Nothing to close: every change was written as it happened. The window
    // reads the record again, so the finished run shows whole.
    if (set_id == 0) return;
    mRunning.erase(set_id);
    mStopAsked.erase(set_id);
    changed();
}

// Here rather than beside the run, so the Assistant window and the history
// window can both stop one without knowing where runs are made.
bool LumenAIUndo::stopBulk()
{
    if (mRunning.empty()) return false;
    for (S64 id : mRunning)
    {
        if (mStopAsked.insert(id).second)
            LL_INFOS("LumenAIUndo") << "bulk run in change set " << id << " asked to stop" << LL_ENDL;
    }
    return true;
}
// </Lumen>

void LumenAIUndo::addChange(S64 set_id, const std::string& kind, const LLUUID& id, bool folder,
                            const std::string& name_before, const std::string& name_after,
                            const LLUUID& parent_before, const LLUUID& parent_after,
                            const std::string& chain)
{
    if (set_id == 0) return;
    const S32 seq = ++mSeq;
    // <Lumen> A delete is also kept in memory until it is on disk, so
    // undelete straight after it never waits for the file.
    const bool trash = (kind == "trash");
    if (trash)
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        PendingTrash& p = mPendingTrash[id];
        p.parent = parent_before;
        p.chain = chain;
        p.seq = seq;
    }
    post([this, set_id, seq, kind, id, folder, name_before, name_after, parent_before, parent_after,
          chain, trash](sqlite3* db)
    {
        sqlite3_stmt* st = nullptr;
        bool ok = false;
        if (prepared(db,
                "INSERT INTO changes(set_id,seq,kind,object_id,folder,name_before,name_after,"
                "parent_before,parent_after,chain) VALUES(?,?,?,?,?,?,?,?,?,?);", &st))
        {
            sqlite3_bind_int64(st, 1, set_id);
            sqlite3_bind_int(st, 2, seq);
            bindText(st, 3, kind);
            bindText(st, 4, id.asString());
            sqlite3_bind_int(st, 5, folder ? 1 : 0);
            bindText(st, 6, name_before);
            bindText(st, 7, name_after);
            bindText(st, 8, parent_before.asString());
            bindText(st, 9, parent_after.asString());
            bindText(st, 10, chain);
            ok = stepDone(db, st, "recording a change");
            sqlite3_finalize(st);
        }
        // <Lumen> On disk now, so the file answers for it. Not written: it
        // stays in memory, and undelete can still use it this login.
        if (trash && ok)
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            auto p = mPendingTrash.find(id);
            if (p != mPendingTrash.end() && p->second.seq == seq) mPendingTrash.erase(p);
        }
        changed();
    });
}

void LumenAIUndo::recordMove(const LLUUID& id, bool folder, const LLUUID& from, const LLUUID& to,
                             S64 set_id)
{
    const std::string name = nameOf(id);
    addChange(liveSet(set_id), "move", id, folder, name, name, from, to,
              Chain::of(from).serialise());
}

void LumenAIUndo::recordRename(const LLUUID& id, bool folder, const std::string& before,
                               const std::string& after, S64 set_id)
{
    const LLUUID p = parentOf(id);
    addChange(liveSet(set_id), "rename", id, folder, before, after, p, p, std::string());
}

void LumenAIUndo::recordTrash(const LLUUID& id, bool folder, const LLUUID& from, S64 set_id)
{
    const std::string name = nameOf(id);
    addChange(liveSet(set_id), "trash", id, folder, name, name, from, parentOf(id),
              Chain::of(from).serialise());
}

void LumenAIUndo::recordUntrash(const LLUUID& id, bool folder, const LLUUID& to)
{
    const std::string name = nameOf(id);
    addChange(currentSet(), "untrash", id, folder, name, name, trashId(), to, std::string());
}

S64 LumenAIUndo::setForNewFolder()
{
    return currentSet();
}

void LumenAIUndo::recordNewFolder(S64 set_id, const LLUUID& id, const LLUUID& parent)
{
    // <Lumen> Its id arrives a moment after the request: its set may have
    // been undone in between (see liveSet).
    if (set_id == 0) return;
    const std::string name = nameOf(id);
    addChange(liveSet(set_id), "new_folder", id, true, name, name, LLUUID::null, parent, std::string());
}

bool LumenAIUndo::lastTrashedFrom(const LLUUID& id, LLUUID& parent_out, Chain& chain_out)
{
    if (!ensureOpen()) return false;
    // <Lumen> No wait for the file: a delete still on its way to it is in
    // memory, and is the newest anyway. Everything else is on disk.
    {
        std::lock_guard<std::mutex> lock(mStateMutex);
        auto p = mPendingTrash.find(id);
        if (p != mPendingTrash.end())
        {
            parent_out = p->second.parent;
            chain_out = Chain::parse(p->second.chain);
            return true;
        }
    }
    bool found = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mRead,
            "SELECT parent_before, chain FROM changes WHERE object_id=? AND kind='trash' "
            "ORDER BY rowid DESC LIMIT 1;", -1, &st, nullptr) == SQLITE_OK)
    {
        bindText(st, 1, id.asString());
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            parent_out = LLUUID(col(st, 0));
            chain_out = Chain::parse(col(st, 1));
            found = true;
        }
        sqlite3_finalize(st);
    }
    return found;
}

// =============================================================================
// Snapshots
// =============================================================================

std::vector<LumenAIUndo::Node> LumenAIUndo::captureTree() const
{
    std::vector<Node> out;
    const LLUUID root = gInventory.getRootFolderID();
    if (root.isNull()) return out;

    LLInventoryModel::cat_array_t cats;
    LLInventoryModel::item_array_t items;
    gInventory.collectDescendents(root, cats, items, LLInventoryModel::EXCLUDE_TRASH);

    // What is not the person's arrangement to restore: what they wear (the
    // outfit folder changes with every garment) and Marketplace listings,
    // which have rules of their own.
    std::set<LLUUID> skip;
    const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
    const LLUUID market = gInventory.findCategoryUUIDForType(LLFolderType::FT_MARKETPLACE_LISTINGS);
    for (const LLUUID& top : { cof, market })
    {
        if (top.isNull()) continue;
        LLInventoryModel::cat_array_t sc;
        LLInventoryModel::item_array_t si;
        gInventory.collectDescendents(top, sc, si, LLInventoryModel::EXCLUDE_TRASH);
        for (const auto& c : sc) skip.insert(c->getUUID());
        for (const auto& i : si) skip.insert(i->getUUID());
    }
    const LLUUID trash = trashId();

    out.reserve(cats.size() + items.size());
    for (const auto& c : cats)
    {
        if (!c || skip.count(c->getUUID()) || c->getUUID() == trash) continue;
        out.push_back(Node{ c->getUUID(), c->getParentUUID(), c->getName(), true });
    }
    for (const auto& i : items)
    {
        if (!i || skip.count(i->getUUID())) continue;
        out.push_back(Node{ i->getUUID(), i->getParentUUID(), i->getName(), false });
    }
    return out;
}

S64 LumenAIUndo::snapshotBefore(const std::string& reason)
{
    if (!ensureOpen()) return 0;
    mSnapshotThisLogin = true;
    return takeSnapshot(reason);
}

// <Lumen>
LumenAIUndo::SnapState LumenAIUndo::snapshotState(S64 snap_id)
{
    std::lock_guard<std::mutex> lock(mStateMutex);
    std::map<S64, SnapState>::const_iterator f = mSnapStates.find(snap_id);
    return f != mSnapStates.end() ? f->second : SNAP_UNKNOWN;
}
// </Lumen>

S64 LumenAIUndo::takeSnapshot(const std::string& reason)
{
    LLTimer timer;
    std::shared_ptr<std::vector<Node>> nodes = std::make_shared<std::vector<Node>>(captureTree());
    const S64 capture_ms = (S64)(timer.getElapsedTimeF32() * 1000.f);
    if (nodes->empty()) return 0;

    // <Lumen> After a cache clear the background fetch is still bringing in
    // the inventory at the first change of a login, and what it has not
    // brought is not in the snapshot. It cannot wait -- the change is being
    // made now -- so it says so, here and wherever it is shown.
    const bool partial = !LLInventoryModelBackgroundFetch::instance().isEverythingFetched();

    const S64 snap = mNextSnap++;
    const S64 at = (S64)time(nullptr);
    {
        std::lock_guard<std::mutex> lock(mStateMutex);   // <Lumen>
        mSnapStates[snap] = SNAP_PENDING;
    }
    LL_INFOS("LumenAIUndo") << "snapshot " << snap << " (" << reason << "): " << nodes->size()
                            << " objects read in " << capture_ms << " ms"
                            << (partial ? ", before the whole inventory had loaded" : "") << LL_ENDL;

    post([this, nodes, snap, at, reason, capture_ms, partial](sqlite3* db)
    {
        LLTimer t;
        // <Lumen> Every step is checked, and the snapshot is committed whole
        // or not at all: a full disk or an I/O error part way rolls it back
        // rather than committing half a difference, and it is then said to
        // have failed rather than reported by its id.
        bool ok = execOn(db, "BEGIN;");
        // What the newest snapshot has, still open.
        struct Open { sqlite3_int64 row; std::string parent, name; };
        std::unordered_map<std::string, Open> open;
        sqlite3_stmt* st = nullptr;
        if (ok && prepared(db, "SELECT rowid,id,parent,name FROM nodes WHERE to_snap IS NULL;", &st))
        {
            int rc;
            while ((rc = sqlite3_step(st)) == SQLITE_ROW)
            {
                open[col(st, 1)] = Open{ sqlite3_column_int64(st, 0), col(st, 2), col(st, 3) };
            }
            if (rc != SQLITE_DONE)
            {
                LL_WARNS("LumenAIUndo") << "reading the last snapshot failed: " << sqlite3_errmsg(db) << LL_ENDL;
                ok = false;
            }
            sqlite3_finalize(st);
        }
        else
        {
            ok = false;
        }

        sqlite3_stmt* ins = nullptr;
        sqlite3_stmt* end = nullptr;
        ok = ok
            && prepared(db, "INSERT INTO nodes(id,folder,parent,name,from_snap,to_snap) "
                            "VALUES(?,?,?,?,?,NULL);", &ins)
            && prepared(db, "UPDATE nodes SET to_snap=? WHERE rowid=?;", &end);
        S64 items = 0, folders = 0, written = 0;
        for (size_t i = 0; ok && i < nodes->size(); ++i)
        {
            const Node& n = (*nodes)[i];
            (n.folder ? folders : items)++;
            const std::string id = n.id.asString();
            const std::string parent = n.parent.asString();
            auto o = open.find(id);
            if (o != open.end())
            {
                const bool same = (o->second.parent == parent && o->second.name == n.name);
                if (!same)
                {
                    sqlite3_bind_int64(end, 1, snap);
                    sqlite3_bind_int64(end, 2, o->second.row);
                    ok = stepDone(db, end, "ending a snapshot row");
                    sqlite3_reset(end);
                }
                open.erase(o);
                if (same || !ok) continue;
            }
            sqlite3_bind_text(ins, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins, 2, n.folder ? 1 : 0);
            sqlite3_bind_text(ins, 3, parent.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 4, n.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins, 5, snap);
            ok = stepDone(db, ins, "writing a snapshot row");
            sqlite3_reset(ins);
            ++written;
        }
        // What is no longer there ends at this snapshot.
        for (auto o = open.begin(); ok && o != open.end(); ++o)
        {
            sqlite3_bind_int64(end, 1, snap);
            sqlite3_bind_int64(end, 2, o->second.row);
            ok = stepDone(db, end, "ending a snapshot row");
            sqlite3_reset(end);
        }
        sqlite3_finalize(ins);   // a no-op on null
        sqlite3_finalize(end);

        const S64 write_ms = (S64)(t.getElapsedTimeF32() * 1000.f);
        if (ok && prepared(db, "INSERT INTO snapshots(id,at,reason,items,folders,capture_ms,write_ms,"
                               "partial) VALUES(?,?,?,?,?,?,?,?);", &st))
        {
            sqlite3_bind_int64(st, 1, snap);
            sqlite3_bind_int64(st, 2, at);
            bindText(st, 3, reason);
            sqlite3_bind_int64(st, 4, items);
            sqlite3_bind_int64(st, 5, folders);
            sqlite3_bind_int64(st, 6, capture_ms);
            sqlite3_bind_int64(st, 7, write_ms);
            sqlite3_bind_int(st, 8, partial ? 1 : 0);
            ok = stepDone(db, st, "recording a snapshot");
            sqlite3_finalize(st);
        }
        else
        {
            ok = false;
        }
        const bool committed = finishTransaction(db, ok, llformat("snapshot %lld", (long long)snap).c_str());
        {
            std::lock_guard<std::mutex> lock(mStateMutex);
            mSnapStates[snap] = committed ? SNAP_WRITTEN : SNAP_FAILED;
        }
        if (!committed)
        {
            changed();
            return;
        }

        // Keep: the newest few always, the rest for a week, never more than ten.
        const S64 now = (S64)time(nullptr);
        std::vector<std::pair<S64, S64>> all;   // id, at
        if (sqlite3_prepare_v2(db, "SELECT id,at FROM snapshots ORDER BY id DESC;", -1, &st, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step(st) == SQLITE_ROW)
                all.emplace_back(sqlite3_column_int64(st, 0), sqlite3_column_int64(st, 1));
            sqlite3_finalize(st);
        }
        S64 oldest_kept = 0;
        std::vector<S64> drop;
        for (size_t i = 0; i < all.size(); ++i)
        {
            const bool keep = (S32)i < SNAP_KEEP_MIN
                || ((S32)i < SNAP_KEEP_MAX && now - all[i].second < SNAP_KEEP_SECONDS);
            if (keep) oldest_kept = all[i].first;
            else      drop.push_back(all[i].first);
        }
        if (!drop.empty())
        {
            bool pruned = execOn(db, "BEGIN;");
            for (size_t i = 0; pruned && i < drop.size(); ++i)
                pruned = execOn(db, llformat("DELETE FROM snapshots WHERE id=%lld;", (long long)drop[i]).c_str());
            // A row that ended at or before the oldest snapshot kept is in none of them.
            pruned = pruned
                && execOn(db, llformat("DELETE FROM nodes WHERE to_snap IS NOT NULL AND to_snap<=%lld;",
                                       (long long)oldest_kept).c_str());
            finishTransaction(db, pruned, "pruning old snapshots");
        }
        {
            bool pruned = execOn(db, "BEGIN;")
                && execOn(db, llformat("DELETE FROM changes WHERE set_id IN (SELECT id FROM sets WHERE at<%lld);",
                                       (long long)(now - SET_KEEP_SECONDS)).c_str())
                && execOn(db, llformat("DELETE FROM sets WHERE at<%lld;",
                                       (long long)(now - SET_KEEP_SECONDS)).c_str());
            finishTransaction(db, pruned, "pruning old change sets");
        }

        LL_INFOS("LumenAIUndo") << "snapshot " << snap << " written: " << items << " items, "
                                << folders << " folders, " << written << " rows new, in "
                                << write_ms << " ms" << LL_ENDL;
        changed();
    });
    return snap;
}

bool LumenAIUndo::readSnapshot(S64 snap_id, std::vector<Node>& out, bool* partial)
{
    sqlite3_stmt* st = nullptr;
    bool exists_ = false;
    if (sqlite3_prepare_v2(mRead, "SELECT partial FROM snapshots WHERE id=?;", -1, &st, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, snap_id);
        exists_ = sqlite3_step(st) == SQLITE_ROW;
        if (exists_ && partial) *partial = sqlite3_column_int(st, 0) != 0;
        sqlite3_finalize(st);
    }
    if (!exists_) return false;
    if (sqlite3_prepare_v2(mRead,
            "SELECT id,folder,parent,name FROM nodes WHERE from_snap<=?1 "
            "AND (to_snap IS NULL OR to_snap>?1);", -1, &st, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, snap_id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            out.push_back(Node{ LLUUID(col(st, 0)), LLUUID(col(st, 2)), col(st, 3),
                                sqlite3_column_int(st, 1) != 0 });
        }
        sqlite3_finalize(st);
    }
    return true;
}

// =============================================================================
// Reading the record
// =============================================================================

LLSD LumenAIUndo::history(S32 limit, bool settled)
{
    LLSD out = LLSD::emptyArray();
    if (!ensureOpen()) return out;
    if (settled) flush();   // <Lumen> the window reads what is on disk; see the header
    if (limit <= 0 || limit > 100) limit = 20;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mRead,
            "SELECT s.id,s.at,s.words,s.source,s.undone_at,s.undo_note,"
            "  (SELECT COUNT(*) FROM changes c WHERE c.set_id=s.id),"
            "  (SELECT COUNT(*) FROM changes c WHERE c.set_id=s.id AND c.undone_at IS NULL) "
            "FROM sets s WHERE EXISTS(SELECT 1 FROM changes c WHERE c.set_id=s.id) "
            "ORDER BY s.id DESC LIMIT ?;", -1, &st, nullptr) != SQLITE_OK)
    {
        return out;
    }
    sqlite3_bind_int(st, 1, limit);
    while (sqlite3_step(st) == SQLITE_ROW)
    {
        LLSD s;
        const S64 id = sqlite3_column_int64(st, 0);
        s["change_set"] = (LLSD::Integer)id;
        s["when"] = whenText(sqlite3_column_int64(st, 1));
        const std::string words = col(st, 2);
        s["asked"] = words.empty() ? std::string("(not from the Assistant window)") : words;
        s["changes"] = sqlite3_column_int(st, 6);
        const S64 undone = sqlite3_column_int64(st, 4);
        s["undone"] = undone > 0;
        if (undone > 0)
        {
            s["undone_when"] = whenText(undone);
            if (!col(st, 5).empty()) s["undo_note"] = col(st, 5);
            // <Lumen> Some of it could not be put back then; undo with this
            // change_set tries those again.
            const S32 not_back = sqlite3_column_int(st, 7);
            if (not_back > 0) s["not_put_back"] = not_back;
        }
        // <Lumen> Still being written, or being undone, in this viewer.
        if (mRunning.count(id)) s["still_running"] = true;
        if (mUndoing.count(id)) s["being_undone"] = true;
        // What kinds, and the first few names, so it can be told apart.
        // <Lumen> Counted by SQLite, not row by row here: a bulk set can be
        // ten thousand changes, and the window reads this while it grows.
        LLSD kinds;
        LLSD names = LLSD::emptyArray();
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(mRead, "SELECT kind,COUNT(*) FROM changes WHERE set_id=? GROUP BY kind;",
                               -1, &c, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(c, 1, id);
            while (sqlite3_step(c) == SQLITE_ROW) kinds[col(c, 0)] = sqlite3_column_int(c, 1);
            sqlite3_finalize(c);
        }
        if (sqlite3_prepare_v2(mRead, "SELECT name_before FROM changes WHERE set_id=? ORDER BY seq LIMIT 4;",
                               -1, &c, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(c, 1, id);
            while (sqlite3_step(c) == SQLITE_ROW) names.append(col(c, 0));
            sqlite3_finalize(c);
        }
        s["kinds"] = kinds;
        s["examples"] = names;
        out.append(s);
    }
    sqlite3_finalize(st);
    return out;
}

LLSD LumenAIUndo::snapshots(bool settled)
{
    LLSD out = LLSD::emptyArray();
    if (!ensureOpen()) return out;
    if (settled) flush();   // <Lumen>
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mRead, "SELECT id,at,reason,items,folders,capture_ms,write_ms,partial "
                                  "FROM snapshots ORDER BY id DESC;", -1, &st, nullptr) == SQLITE_OK)
    {
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            LLSD s;
            s["snapshot"] = (LLSD::Integer)sqlite3_column_int64(st, 0);
            s["when"] = whenText(sqlite3_column_int64(st, 1));
            s["reason"] = col(st, 2);
            s["items"] = sqlite3_column_int(st, 3);
            s["folders"] = sqlite3_column_int(st, 4);
            s["capture_ms"] = sqlite3_column_int(st, 5);
            s["write_ms"] = sqlite3_column_int(st, 6);
            // <Lumen>
            if (sqlite3_column_int(st, 7) != 0)
            {
                s["partial"] = true;
                s["partial_note"] = "Taken before the viewer had loaded the whole inventory from Second "
                                    "Life, so it may not have everything in it.";
            }
            out.append(s);
        }
        sqlite3_finalize(st);
    }
    return out;
}

// =============================================================================
// Undo
// =============================================================================

LumenAIUndo::Plan LumenAIUndo::planUndo(S64 set_id, LLSD& error)
{
    Plan plan;
    if (!ensureOpen())
    {
        error = "The inventory record is not available: "
              + (mWhyUnavailable.empty() ? std::string("the inventory has not loaded yet") : mWhyUnavailable)
              + ".";
        return plan;
    }
    // <Lumen> "Undo" means the newest. While a bulk run is still being
    // written, that is the run -- which cannot be undone yet -- and never an
    // older, unrelated change in its place.
    if (set_id == 0 && !mRunning.empty())
    {
        error = llformat("The newest change is a bulk change (change set %lld) that is still running, so "
                         "nothing was undone. It can be undone once it has finished; the viewer says "
                         "so in the Assistant window. To stop it sooner: Stop in Comm > Assistant "
                         "Inventory History, or Clear in the Assistant window.",
                         (long long)*mRunning.rbegin());
        return plan;
    }
    // Nor anything older while one runs: the two would get in each other's way.
    if (!mRunning.empty() && !mRunning.count(set_id))
    {
        error = "A bulk change is still running, so nothing was undone. Ask again once it has finished, "
                "or stop it with Stop in Comm > Assistant Inventory History.";
        return plan;
    }
    // What was just recorded has to be in the file for this to see it.
    if (!flush())
    {
        error = "The record is still being written (a snapshot of a large inventory), so nothing was "
                "undone. The user can ask again in a little while.";
        return plan;
    }
    // </Lumen>
    sqlite3_stmt* st = nullptr;
    if (set_id == 0)
    {
        if (sqlite3_prepare_v2(mRead,
                "SELECT id FROM sets s WHERE undone_at IS NULL AND EXISTS(SELECT 1 FROM changes c "
                "WHERE c.set_id=s.id) ORDER BY id DESC LIMIT 1;", -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW) set_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
        if (set_id == 0)
        {
            error = "There is nothing to undo: the record holds no change by the assistant that is "
                    "not already undone.";
            return plan;
        }
    }
    // <Lumen>
    if (mRunning.count(set_id))
    {
        error = llformat("Change set %lld is a bulk change that is still running, so nothing was undone. "
                         "It can be undone once it has finished; the viewer says so in the Assistant "
                         "window.", (long long)set_id);
        return plan;
    }
    if (mUndoing.count(set_id))
    {
        error = llformat("An undo of change set %lld is still going, so it was not started again. The "
                         "viewer says when it has finished.", (long long)set_id);
        return plan;
    }
    // </Lumen>
    S64 undone = -1;
    if (sqlite3_prepare_v2(mRead, "SELECT IFNULL(undone_at,0) FROM sets WHERE id=?;", -1, &st, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, set_id);
        if (sqlite3_step(st) == SQLITE_ROW) undone = sqlite3_column_int64(st, 0);
        sqlite3_finalize(st);
    }
    if (undone < 0)
    {
        error = llformat("There is no change set %lld in the record -- inventory / history lists them.",
                         (long long)set_id);
        return plan;
    }
    // <Lumen> Undone before: planned again, but only what that undo did not
    // put back -- what was refused then may go now. Nothing left, and it says
    // so below.
    plan.again = undone > 0;
    plan.set_id = set_id;

    struct Row { std::string kind, nb, na; LLUUID id, pb, pa; bool folder; std::string chain;
                 S64 row; bool done; };
    std::vector<Row> rows;
    if (sqlite3_prepare_v2(mRead,
            "SELECT kind,object_id,folder,name_before,name_after,parent_before,parent_after,chain,"
            "rowid,IFNULL(undone_at,0) FROM changes WHERE set_id=? ORDER BY seq DESC;", -1, &st, nullptr)
        == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, set_id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            rows.push_back(Row{ col(st, 0), col(st, 3), col(st, 4), LLUUID(col(st, 1)),
                                LLUUID(col(st, 5)), LLUUID(col(st, 6)),
                                sqlite3_column_int(st, 2) != 0, col(st, 7),
                                sqlite3_column_int64(st, 8), sqlite3_column_int64(st, 9) > 0 });
        }
        sqlite3_finalize(st);
    }

    // Undone newest first, so the state each step expects is simulated: a
    // thing moved twice in one request is checked against where the later
    // move left it, not where it is now.
    std::map<LLUUID, LLUUID> simParent;
    std::map<LLUUID, std::string> simName;
    auto curParent = [&](const LLUUID& id) { auto f = simParent.find(id); return f != simParent.end() ? f->second : parentOf(id); };
    auto curName   = [&](const LLUUID& id) { auto f = simName.find(id);   return f != simName.end()   ? f->second : nameOf(id); };
    const LLUUID trash = trashId();
    auto alone = [&](const std::string& name, const std::string& why)
    {
        ++plan.left_alone_count;
        addCapped(plan.left_alone, entry(name, why));
    };
    auto gone = [&](const std::string& name, const std::string& why)
    {
        ++plan.cannot_count;
        addCapped(plan.cannot, entry(name, why));
    };

    for (const Row& r : rows)
    {
        const std::string label = r.nb.empty() ? r.na : r.nb;
        // <Lumen> Put back by an earlier undo of this set. Not simulated: the
        // inventory shows it already, and the next older step is checked
        // against the inventory -- so a hand change since is still seen.
        if (r.done) { ++plan.earlier; continue; }
        if (!exists(r.id))
        {
            if (r.kind == "new_folder") continue;   // already gone: nothing to put back
            gone(label, r.kind == "trash"
                 ? "it was emptied from the Trash, so it is gone for good -- nothing can bring it back"
                 : "it is no longer in inventory (emptied from the Trash, given away or rezzed)");
            continue;
        }
        const LLUUID now = curParent(r.id);
        // <Lumen> In a folder the user has protected since -- or, for a folder
        // that would move or go to the Trash, holding one: left alone, and
        // said. Into one is never asked: that harms nothing.
        auto guarded = [&](bool carried) -> bool
        {
            const std::string why = LumenInventoryRules::inProtected(r.id, carried);
            if (why.empty()) return false;
            alone(label, why);
            return true;
        };
        // </Lumen>
        if (r.kind == "move" || r.kind == "trash")
        {
            if (now != r.pa)
            {
                alone(label, r.kind == "trash"
                      ? "taken out of the Trash since; it is in " + pathOf(now)
                      : "moved again since; it is in " + pathOf(now));
                continue;
            }
            if (guarded(r.folder)) continue;   // <Lumen>
            Step s; s.kind = Step::MOVE; s.id = r.id; s.folder = r.folder; s.label = label;
            s.target = r.pb; s.chain = Chain::parse(r.chain); s.row = r.row;
            plan.steps.push_back(s);
            simParent[r.id] = r.pb;
        }
        else if (r.kind == "untrash")
        {
            if (now != r.pa) { alone(label, "moved since it came out of the Trash"); continue; }
            if (guarded(r.folder)) continue;   // <Lumen>
            Step s; s.kind = Step::TRASH; s.id = r.id; s.folder = r.folder; s.label = label;
            s.row = r.row;
            plan.steps.push_back(s);
            simParent[r.id] = trash;
        }
        else if (r.kind == "rename")
        {
            if (curName(r.id) != r.na) { alone(label, "renamed again since; it is called \"" + curName(r.id) + "\""); continue; }
            if (guarded(false)) continue;   // <Lumen>
            Step s; s.kind = Step::RENAME; s.id = r.id; s.folder = r.folder; s.name = r.nb;
            s.label = r.na; s.row = r.row;
            plan.steps.push_back(s);
            simName[r.id] = r.nb;
        }
        else if (r.kind == "new_folder")
        {
            if (inTrash(r.id) || now == trash) continue;
            // Still holding anything once this undo has moved out what it moves out?
            LLInventoryModel::cat_array_t* cats = nullptr;
            LLInventoryModel::item_array_t* items = nullptr;
            gInventory.getDirectDescendentsOf(r.id, cats, items);
            S32 holding = 0;
            if (cats)  for (const auto& c : *cats)  if (c && curParent(c->getUUID()) == r.id) ++holding;
            if (items) for (const auto& i : *items) if (i && curParent(i->getUUID()) == r.id) ++holding;
            if (holding > 0) { alone(label, "the folder it made has things in it now, so it stays"); continue; }
            if (now != r.pa) { alone(label, "the folder it made has been moved since, so it stays"); continue; }
            if (guarded(true)) continue;   // <Lumen>
            Step s; s.kind = Step::TRASH; s.id = r.id; s.folder = true; s.label = label;
            s.row = r.row;
            plan.steps.push_back(s);
            simParent[r.id] = trash;
        }
    }
    // <Lumen>
    if (plan.again && plan.steps.empty())
    {
        error = llformat("Change set %lld was already undone, %s", (long long)set_id, whenText(undone).c_str());
        if (plan.left_alone_count + plan.cannot_count > 0)
        {
            error = error.asString()
                  + llformat(", and nothing in it is left that can be put back: %d changed again since, "
                             "%d are gone from inventory.", plan.left_alone_count, plan.cannot_count);
        }
        else
        {
            error = error.asString() + ".";
        }
    }
    // </Lumen>
    return plan;
}

LLSD LumenAIUndo::undo(S64 set_id)
{
    LLSD error;
    Plan plan = planUndo(set_id, error);
    if (error.isDefined())
    {
        LLSD out; out["error"] = error; return out;
    }
    const S64 id = plan.set_id;
    const bool again = plan.again;
    const S32 earlier = plan.earlier;

    // <Lumen> Undone, or being: nothing more is written into it. The turn
    // that made it -- "no, undo that and put it in Autumn" -- writes what it
    // changes next into a new set, so that "undo that" afterwards undoes the
    // Autumn move and not something older (see liveSet()).
    mClosed.insert(id);
    mUndoing.insert(id);
    if (mRequestSet == id) mRequestSet = 0;

    LLSD out = run(plan, [this, id](const LLSD& s, const std::vector<S64>& rows, bool later)
    {
        mUndoing.erase(id);
        const S32 put_back = s["put_back"].asInteger();
        const S32 could_not = s["could_not"].asInteger();
        // Marked undone when something went back, or when nothing was
        // refused (all of it changed since, or gone -- a second try would
        // change nothing, and an unmarked set would stop every later "undo"
        // at itself). When everything it tried was refused, it stays as it
        // was and undo tries it again. Each change put back is marked on its
        // own, so a later undo of the set tries only the rest.
        const bool whole = put_back > 0 || could_not == 0;
        const std::string note = llformat("%d put back, %d could not be", put_back, could_not);
        const S64 now = (S64)time(nullptr);
        post([this, id, rows, whole, note, now](sqlite3* db)
        {
            bool ok = execOn(db, "BEGIN;");
            sqlite3_stmt* st = nullptr;
            if (ok && !rows.empty())
            {
                ok = prepared(db, "UPDATE changes SET undone_at=? WHERE rowid=?;", &st);
                for (size_t i = 0; ok && i < rows.size(); ++i)
                {
                    sqlite3_bind_int64(st, 1, now);
                    sqlite3_bind_int64(st, 2, rows[i]);
                    ok = stepDone(db, st, "marking a change put back");
                    sqlite3_reset(st);
                }
                sqlite3_finalize(st);
            }
            if (ok && prepared(db, whole ? "UPDATE sets SET undone_at=?, undo_note=? WHERE id=?;"
                                         : "UPDATE sets SET undo_note=? WHERE id=?;", &st))
            {
                int i = 1;
                if (whole) sqlite3_bind_int64(st, i++, now);
                bindText(st, i++, note);
                sqlite3_bind_int64(st, i, id);
                ok = stepDone(db, st, "marking a change set undone");
                sqlite3_finalize(st);
            }
            else
            {
                ok = false;
            }
            finishTransaction(db, ok, "recording an undo");
            changed();
        });
        if (later)
        {
            std::string message = "Undo finished: " + note + ".";
            if (could_not > 0)
            {
                message += llformat(" Undoing change set %lld again tries those, once whatever stopped "
                                    "them is gone.", (long long)id);
            }
            announce(message);
        }
        changed();
    });
    out["change_set"] = (LLSD::Integer)id;
    if (again)
    {
        out["again"] = true;
        out["put_back_before"] = earlier;
    }
    if (out["finished"].asBoolean() && out["could_not"].asInteger() > 0)
    {
        if (out["put_back"].asInteger() == 0)
        {
            out["not_marked_undone"] = true;
            out["try_again"] = llformat("Nothing could be put back, so change set %lld stays as it was: "
                                        "undo with change_set %lld tries it again, once whatever stopped "
                                        "it is gone.", (long long)id, (long long)id);
        }
        else
        {
            out["try_again"] = llformat("undo with change_set %lld tries the ones that could not be put "
                                        "back again, once whatever stopped them is gone.", (long long)id);
        }
    }
    // </Lumen>
    changed();
    return out;
}

// =============================================================================
// Restoring a snapshot
// =============================================================================

LumenAIUndo::Plan LumenAIUndo::planRestore(S64 snap_id, LLSD& error)
{
    Plan plan;
    if (!ensureOpen())
    {
        error = "The inventory record is not available: "
              + (mWhyUnavailable.empty() ? std::string("the inventory has not loaded yet") : mWhyUnavailable)
              + ".";
        return plan;
    }
    // <Lumen> Not while a bulk run is going: the two would get in each
    // other's way, and "undo" is refused mid-run, so restore is what a model
    // reaches for next. Asked before the plan made in this frame is reused,
    // so a Yes given after the run started is refused too.
    if (!mRunning.empty())
    {
        error = "A bulk change is still running, so nothing was restored. Ask again once it has "
                "finished, or stop it with Stop in Comm > Assistant Inventory History.";
        return plan;
    }
    // <Lumen> previewRestore() and restore() in the same call -- the endpoint
    // does both once the question is answered -- plan once. Another frame
    // plans again: the inventory may have moved on.
    const U32 frame = LLFrameTimer::getFrameCount();
    if (mPlanned && mPlannedSnap == snap_id && mPlannedFrame == frame) return *mPlanned;
    if (!flush())
    {
        error = "The record is still being written (a snapshot of a large inventory), so nothing was "
                "changed. The user can ask again in a little while.";
        return plan;
    }
    // On the main thread, and the biggest read there is: said in the log, so
    // it can be measured on a real inventory.
    LLTimer timer;
    // </Lumen>
    std::vector<Node> nodes;
    if (!readSnapshot(snap_id, nodes, &plan.partial))
    {
        error = llformat("There is no snapshot %lld -- inventory / history lists them.", (long long)snap_id);
        return plan;
    }
    const S32 read_ms = (S32)(timer.getElapsedTimeF32() * 1000.f);
    plan.snap_id = snap_id;

    std::unordered_map<LLUUID, const Node*> byId;
    byId.reserve(nodes.size());
    for (const Node& n : nodes) byId[n.id] = &n;
    const LLUUID root = gInventory.getRootFolderID();

    // <Lumen> What is in Current Outfit now, once, rather than a walk up from
    // every node in the snapshot.
    std::unordered_set<LLUUID> in_outfit;
    {
        const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
        if (cof.notNull())
        {
            LLInventoryModel::cat_array_t cats;
            LLInventoryModel::item_array_t items;
            gInventory.collectDescendents(cof, cats, items, LLInventoryModel::EXCLUDE_TRASH);
            in_outfit.insert(cof);
            for (const auto& c : cats)  if (c) in_outfit.insert(c->getUUID());
            for (const auto& i : items) if (i) in_outfit.insert(i->getUUID());
        }
    }

    // Folders first, shallowest first, so a folder comes back before what is in it.
    // <Lumen> Each folder's depth worked out once, not once per thing in it.
    std::unordered_map<LLUUID, S32> depthOf;
    auto depth = [&](const Node& n)
    {
        std::vector<LLUUID> path;
        LLUUID p = n.parent;
        S32 base = 0;
        for (S32 guard = 0; guard < 64 && p.notNull() && p != root; ++guard)
        {
            auto known = depthOf.find(p);
            if (known != depthOf.end()) { base = known->second; break; }
            auto f = byId.find(p);
            if (f == byId.end()) break;
            path.push_back(p);
            p = f->second->parent;
        }
        S32 d = base;
        for (auto it = path.rbegin(); it != path.rend(); ++it) depthOf[*it] = ++d;
        return d;
    };
    std::vector<std::pair<S32, const Node*>> order;
    order.reserve(nodes.size());
    for (const Node& n : nodes) order.emplace_back((n.folder ? 0 : 1000) + depth(n), &n);
    std::sort(order.begin(), order.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    auto chainFor = [&](const LLUUID& folder)
    {
        Chain c;
        LLUUID p = folder;
        for (S32 guard = 0; guard < 64 && p.notNull() && p != root; ++guard)
        {
            auto f = byId.find(p);
            if (f == byId.end()) break;
            c.folders.emplace_back(p, f->second->name);
            p = f->second->parent;
        }
        std::reverse(c.folders.begin(), c.folders.end());
        return c;
    };
    // <Lumen> Held where it is by the rules every path shares: not in the
    // preview's counts or the question's, and listed with the reason. Asked
    // again when it runs, since a minute may pass in between.
    auto refuse = [&plan](const std::string& name, const std::string& why)
    {
        ++plan.refused_count;
        addCapped(plan.refused, entry(name, why));
    };

    for (const auto& o : order)
    {
        const Node& n = *o.second;
        // <Lumen> One look-up for whether it is there, where, and its name.
        LLInventoryObject* obj = gInventory.getObject(n.id);
        if (!obj)
        {
            ++plan.cannot_count;
            addCapped(plan.cannot, entry(n.name, n.folder
                ? "the folder is gone from inventory (emptied from the Trash)"
                : "gone from inventory -- emptied from the Trash, given away or rezzed"));
            continue;
        }
        if (n.folder)
        {
            LLViewerInventoryCategory* c = gInventory.getCategory(n.id);
            if (c && LLFolderType::lookupIsProtectedType(c->getPreferredType())) continue;
        }
        if (in_outfit.count(n.id)) continue;
        const LLUUID now = obj->getParentUUID();
        const std::string name_now = obj->getName();
        if (now != n.parent)
        {
            std::string why = LumenInventoryRules::held(n.id, n.folder);   // <Lumen>
            // <Lumen> A folder holding a protected one would carry it off.
            if (why.empty() && n.folder) why = LumenInventoryRules::inProtected(n.id, true);
            if (why.empty() && liveFolder(n.parent)) why = LumenInventoryRules::notInto(n.parent);
            if (!why.empty())
            {
                refuse(n.name, why);
            }
            else
            {
                Step s; s.kind = Step::MOVE; s.id = n.id; s.folder = n.folder; s.label = n.name;
                s.target = n.parent; s.chain = chainFor(n.parent);
                plan.steps.push_back(s);
            }
        }
        if (name_now != n.name)
        {
            const std::string why = LumenInventoryRules::notRename(n.id, n.folder);   // <Lumen>
            if (!why.empty())
            {
                refuse(name_now, why);
            }
            else
            {
                Step s; s.kind = Step::RENAME; s.id = n.id; s.folder = n.folder; s.name = n.name;
                s.label = name_now;
                plan.steps.push_back(s);
            }
        }
    }
    // <Lumen>
    LL_INFOS("LumenAIUndo") << "restoring snapshot " << snap_id << " planned on the main thread: "
                            << nodes.size() << " objects read in " << read_ms << " ms, "
                            << plan.steps.size() << " steps, " << (S32)(timer.getElapsedTimeF32() * 1000.f)
                            << " ms in all" << LL_ENDL;
    mPlanned = std::make_shared<Plan>(plan);
    mPlannedSnap = snap_id;
    mPlannedFrame = frame;
    // </Lumen>
    return plan;
}

LLSD LumenAIUndo::previewRestore(S64 snap_id, S32 sample)
{
    LLSD error;
    Plan plan = planRestore(snap_id, error);
    LLSD out;
    if (error.isDefined()) { out["error"] = error; return out; }
    if (sample <= 0) sample = 12;
    S32 moves = 0, out_of_trash = 0, renames = 0;
    LLSD examples = LLSD::emptyArray();
    for (const Step& s : plan.steps)
    {
        std::string what;
        if (s.kind == Step::RENAME)
        {
            ++renames;
            what = "renamed back to \"" + s.name + "\"";
        }
        else
        {
            if (inTrash(s.id)) { ++out_of_trash; what = "out of the Trash, into "; }
            else               { ++moves;        what = "moved back to "; }
            what += liveFolder(s.target) ? pathOf(s.target)
                                         : (s.chain.folders.empty() ? std::string("(top of inventory)")
                                                                    : s.chain.folders.back().second
                                                                      + " (made again)");
        }
        if ((S32)examples.size() < sample) examples.append(entry(s.label, what, "what"));
    }
    out["snapshot"] = (LLSD::Integer)snap_id;
    out["moves"] = moves;
    out["out_of_trash"] = out_of_trash;
    out["renames"] = renames;
    out["examples"] = examples;
    out["cannot"] = plan.cannot;
    out["cannot_count"] = plan.cannot_count;
    // <Lumen>
    if (plan.refused_count > 0)
    {
        out["refused"] = plan.refused;
        out["refused_count"] = plan.refused_count;
    }
    if (plan.partial)
    {
        out["partial"] = true;
        out["partial_note"] = "This snapshot was taken before the viewer had loaded the whole inventory "
                              "from Second Life, so it may not have everything in it. What it does not "
                              "have stays where it is now.";
    }
    // </Lumen>
    out["nothing_to_do"] = plan.steps.empty();
    return out;
}

LLSD LumenAIUndo::restore(S64 snap_id)
{
    LLSD error;
    Plan plan = planRestore(snap_id, error);
    mPlanned.reset();   // <Lumen> used, or about to be out of date
    if (error.isDefined())
    {
        LLSD out; out["error"] = error; return out;
    }
    if (plan.steps.empty())
    {
        LLSD out; out["nothing_to_do"] = true; out["cannot"] = plan.cannot;
        out["cannot_count"] = plan.cannot_count;
        if (plan.refused_count > 0) { out["refused"] = plan.refused; out["refused_count"] = plan.refused_count; }
        return out;
    }
    // A restore is a change too: take a snapshot first, so it can be undone
    // by restoring that one.
    const S64 before = takeSnapshot(llformat("before restoring snapshot %lld", (long long)snap_id));
    // <Lumen> ...and it has to be on disk before anything moves: a restore
    // that cannot be put back is not started. A restore is rare and asked
    // for, so the moment this waits is spent here and not on every frame.
    if (before == 0 || !flush() || snapshotState(before) != SNAP_WRITTEN)
    {
        LLSD out;
        out["error"] = "The snapshot that would let this restore be put back could not be written, so "
                       "nothing was changed. Say so; the viewer's log says why.";
        return out;
    }
    // </Lumen>
    LLSD out = run(plan, [this](const LLSD& s, const std::vector<S64>&, bool later)
    {
        if (later)
        {
            announce(llformat("Inventory restore finished: %d put back, %d could not be.",
                              s["put_back"].asInteger(), s["could_not"].asInteger()));
        }
        changed();
    });
    out["snapshot"] = (LLSD::Integer)snap_id;
    out["snapshot_before_restore"] = (LLSD::Integer)before;
    if (plan.partial) out["partial"] = true;   // <Lumen>
    return out;
}

// =============================================================================
// The window
// =============================================================================

LumenAIUndoFloater::LumenAIUndoFloater(const LLSD& key)
:   LLFloater(key)
{
}

bool LumenAIUndoFloater::postBuild()
{
    mSets  = getChild<LLScrollListCtrl>("change_sets");
    mSnaps = getChild<LLScrollListCtrl>("snapshots");
    mText  = getChild<LLTextEditor>("detail");
    getChild<LLButton>("undo_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onUndo(); });
    // <Lumen> Stop: a bulk run, from here as well as from Clear in the Assistant.
    mStopBtn = findChild<LLButton>("stop_btn");
    if (mStopBtn)
    {
        mStopBtn->setClickedCallback([this](LLUICtrl*, const LLSD&) { onStop(); });
        mStopBtn->setEnabled(false);
    }
    getChild<LLButton>("preview_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onPreview(); });
    getChild<LLButton>("restore_btn")->setClickedCallback([this](LLUICtrl*, const LLSD&) { onRestore(); });
    return true;
}

void LumenAIUndoFloater::onOpen(const LLSD& key)
{
    reload();
}

void LumenAIUndoFloater::draw()
{
    // <Lumen> At most once a second: a bulk run marks the record changed with
    // every one of its thousands of changes. The flag stays raised until then.
    if (LLTimer::getTotalSeconds() - mLastReload >= 1.0
        && LumenAIUndo::instanceExists() && LumenAIUndo::instance().takeDirty())
    {
        reload();
    }
    // <Lumen> Stop is there to press only while a bulk run is going.
    if (mStopBtn)
        mStopBtn->setEnabled(LumenAIUndo::instanceExists() && LumenAIUndo::instance().bulkRunning());
    LLFloater::draw();
}

void LumenAIUndoFloater::show(const std::string& text)
{
    if (mText) mText->setText(text);
}

S64 LumenAIUndoFloater::selected(LLScrollListCtrl* list) const
{
    LLScrollListItem* row = list ? list->getFirstSelected() : nullptr;
    return row ? (S64)row->getValue().asInteger() : 0;
}

void LumenAIUndoFloater::reload()
{
    mLastReload = LLTimer::getTotalSeconds();   // <Lumen>
    LumenAIUndo& u = LumenAIUndo::instance();
    if (!u.available())
    {
        // <Lumen> Said why: not loaded yet, or held by another viewer.
        const std::string why = u.unavailableWhy();
        show("Nothing to show: the record is not available"
             + (why.empty() ? std::string(".") : " -- " + why + "."));
        return;
    }
    const S64 keep_set = selected(mSets);
    const S64 keep_snap = selected(mSnaps);
    mSets->deleteAllItems();
    // <Lumen> What is on disk, without waiting for what is being written: a
    // snapshot being written never holds up a frame. More arriving raises the
    // flag draw() looks at, and it is read again.
    const LLSD sets = u.history(50, false);
    for (LLSD::array_const_iterator it = sets.beginArray(); it != sets.endArray(); ++it)
    {
        const LLSD& s = *it;
        LLSD row;
        row["value"] = s["change_set"];
        std::string what = llformat("%d change%s", s["changes"].asInteger(),
                                    s["changes"].asInteger() == 1 ? "" : "s");
        // <Lumen>
        if (s["still_running"].asBoolean())     what += ", still running";
        else if (s["being_undone"].asBoolean()) what += ", being undone";
        else if (s["undone"].asBoolean())
        {
            what += s.has("not_put_back") ? llformat(", undone but %d not put back", s["not_put_back"].asInteger())
                                          : std::string(", undone");
        }
        // </Lumen>
        row["columns"][0]["column"] = "when";  row["columns"][0]["value"] = s["when"];
        row["columns"][1]["column"] = "asked"; row["columns"][1]["value"] = s["asked"];
        row["columns"][2]["column"] = "what";  row["columns"][2]["value"] = what;
        mSets->addElement(row);
    }
    if (keep_set) mSets->selectByValue((LLSD::Integer)keep_set);

    mSnaps->deleteAllItems();
    const LLSD snaps = u.snapshots(false);   // <Lumen>
    for (LLSD::array_const_iterator it = snaps.beginArray(); it != snaps.endArray(); ++it)
    {
        const LLSD& s = *it;
        LLSD row;
        row["value"] = s["snapshot"];
        row["columns"][0]["column"] = "when";   row["columns"][0]["value"] = s["when"];
        // <Lumen> A snapshot taken before all of the inventory had loaded says so.
        row["columns"][1]["column"] = "reason";
        row["columns"][1]["value"] = s["reason"].asString()
            + (s["partial"].asBoolean() ? " -- not all of the inventory had loaded" : "");
        row["columns"][2]["column"] = "size";
        row["columns"][2]["value"] = llformat("%d", s["items"].asInteger());
        mSnaps->addElement(row);
    }
    if (keep_snap) mSnaps->selectByValue((LLSD::Integer)keep_snap);
}

namespace
{
    std::string describe(const LLSD& r)
    {
        std::string t;
        if (r.has("error")) return r["error"].asString();
        if (r["paced"].asBoolean())
        {
            t += llformat("Started: %d changes to put back. The viewer says when it has finished.\n",
                          r["steps"].asInteger());
        }
        else if (r.has("put_back"))
        {
            t += llformat("Put back: %d. Could not: %d.", r["put_back"].asInteger(),
                          r["could_not"].asInteger());
            if (r["waiting_for_folders"].asInteger() > 0)
                t += llformat(" Waiting for a folder to be made again: %d.",
                              r["waiting_for_folders"].asInteger());
            t += "\n";
        }
        if (r.has("moves"))
        {
            t += llformat("Restoring would move %d back, take %d out of the Trash and rename %d back.\n",
                          r["moves"].asInteger(), r["out_of_trash"].asInteger(), r["renames"].asInteger());
        }
        // <Lumen>
        if (r.has("partial_note")) t += r["partial_note"].asString() + "\n";
        if (r["again"].asBoolean())
        {
            t += llformat("An earlier undo of this put %d back already; this tried the rest.\n",
                          r["put_back_before"].asInteger());
        }
        if (r.has("try_again")) t += r["try_again"].asString() + "\n";
        // </Lumen>
        auto list = [&t](const LLSD& l, const char* head, const char* key)
        {
            if (!l.isArray() || l.size() == 0) return;
            t += std::string("\n") + head + "\n";
            for (LLSD::array_const_iterator i = l.beginArray(); i != l.endArray(); ++i)
                t += "  " + (*i)["name"].asString() + " -- " + (*i)[key].asString() + "\n";
        };
        list(r["examples"], "For example:", "what");
        list(r["done"], "Put back:", "what");
        list(r["failed"], "Could not:", "why");
        list(r["left_alone"], "Left alone:", "why");
        list(r["refused"], "Left where it is, by the rules the assistant follows:", "why");   // <Lumen>
        list(r["cannot"], "Cannot be brought back:", "why");
        if (r["nothing_to_do"].asBoolean()) t += "Nothing differs from that snapshot.\n";
        return t;
    }
}

void LumenAIUndoFloater::onUndo()
{
    const S64 id = selected(mSets);
    if (!id) { show("Choose a change in the list first."); return; }
    show(describe(LumenAIUndo::instance().undo(id)));
    reload();
}

// <Lumen> What it has done stays, as one change set: the Assistant window
// says how far it got, and Undo here puts it back.
void LumenAIUndoFloater::onStop()
{
    if (LumenAIUndo::instance().stopBulk())
        show("Stopping. What was done stays, and can be undone here once it has stopped.");
    else
        show("Nothing is running.");
}

void LumenAIUndoFloater::onPreview()
{
    const S64 id = selected(mSnaps);
    if (!id) { show("Choose a snapshot in the list first."); return; }
    show(describe(LumenAIUndo::instance().previewRestore(id, 20)));
}

void LumenAIUndoFloater::onRestore()
{
    const S64 id = selected(mSnaps);
    if (!id) { show("Choose a snapshot in the list first."); return; }
    const LLSD p = LumenAIUndo::instance().previewRestore(id, 0);
    if (p.has("error") || p["nothing_to_do"].asBoolean())
    {
        show(describe(p));
        return;
    }
    LLSD args;
    args["MOVES"] = p["moves"].asInteger();
    args["TRASH"] = p["out_of_trash"].asInteger();
    args["RENAMES"] = p["renames"].asInteger();
    LLHandle<LLFloater> h = getHandle();
    LLNotificationsUtil::add("LumenConfirmRestore", args, LLSD(),
        [h, id](const LLSD& n, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(n, response) != 0) return;
            const LLSD r = LumenAIUndo::instance().restore(id);
            if (LumenAIUndoFloater* f = dynamic_cast<LumenAIUndoFloater*>(h.get()))
            {
                f->show(describe(r));
                f->reload();
            }
        });
}
