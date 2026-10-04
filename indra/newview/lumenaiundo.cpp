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

#include "llagent.h"
#include "llbutton.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "lleventcoro.h"
#include "llfloaterreg.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llnotificationsutil.h"
#include "llscrolllistctrl.h"
#include "lltexteditor.h"
#include "lltimer.h"
#include "llviewerinventory.h"
#include "rlvactions.h"
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

namespace
{
    const char* UNDO_FILE = "inventory_undo.db";

    // Not a cache, so there is no "rebuild on mismatch": a newer shape must
    // migrate. Bump only with a migration beside it.
    const int SCHEMA_VERSION = 1;

    const char* SCHEMA =
        "CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS sets ("
        "  id INTEGER PRIMARY KEY, at INTEGER NOT NULL, words TEXT NOT NULL,"
        "  source TEXT NOT NULL, undone_at INTEGER, undo_note TEXT);"
        "CREATE TABLE IF NOT EXISTS changes ("
        "  set_id INTEGER NOT NULL, seq INTEGER NOT NULL, kind TEXT NOT NULL,"
        "  object_id TEXT NOT NULL, folder INTEGER NOT NULL,"
        "  name_before TEXT NOT NULL, name_after TEXT NOT NULL,"
        "  parent_before TEXT NOT NULL, parent_after TEXT NOT NULL, chain TEXT NOT NULL);"
        "CREATE INDEX IF NOT EXISTS changes_set ON changes(set_id);"
        "CREATE INDEX IF NOT EXISTS changes_object ON changes(object_id);"
        "CREATE TABLE IF NOT EXISTS snapshots ("
        "  id INTEGER PRIMARY KEY, at INTEGER NOT NULL, reason TEXT NOT NULL,"
        "  items INTEGER NOT NULL, folders INTEGER NOT NULL,"
        "  capture_ms INTEGER NOT NULL, write_ms INTEGER NOT NULL);"
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

    /** Run a prepared statement to the end; say so in the log when it fails. */
    void stepLogged(sqlite3* db, sqlite3_stmt* st, const char* what)
    {
        const int rc = sqlite3_step(st);
        if (rc != SQLITE_DONE && rc != SQLITE_ROW)
        {
            LL_WARNS("LumenAIUndo") << what << " failed: " << sqlite3_errmsg(db) << LL_ENDL;
        }
    }

    bool prepared(sqlite3* db, const char* sql, sqlite3_stmt** st)
    {
        if (sqlite3_prepare_v2(db, sql, -1, st, nullptr) == SQLITE_OK) return true;
        LL_WARNS("LumenAIUndo") << "cannot prepare: " << sqlite3_errmsg(db) << " -- " << sql << LL_ENDL;
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

    /** Why the viewer's own rules keep this from going there, or empty. */
    std::string whyNot(const LLUUID& id, bool folder, const LLUUID& target)
    {
        const LLUUID lib = gInventory.getLibraryRootFolderID();
        const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);
        if (within(target, lib)) return "the folder it belongs in is in the Library";
        if (within(target, cof)) return "the folder it belongs in is Current Outfit";
        if (inTrash(target))     return "the folder it belongs in is in the Trash";
        if (within(id, lib))     return "it is in the Library";
        if (folder)
        {
            LLViewerInventoryCategory* c = gInventory.getCategory(id);
            if (!c) return "it is no longer in inventory";
            if (LLFolderType::lookupIsProtectedType(c->getPreferredType()))
                return "it is one of Second Life's own folders";
            if (within(target, id)) return "a folder cannot go inside itself";
        }
        if (RlvActions::isRlvEnabled()
            && !(folder ? RlvFolderLocks::instance().canMoveFolder(id, target)
                        : RlvFolderLocks::instance().canMoveItem(id, target)))
        {
            return "an RLV lock the user is wearing holds it where it is";
        }
        return std::string();
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
};

struct LumenAIUndo::Plan
{
    std::vector<Step> steps;
    LLSD left_alone = LLSD::emptyArray();
    LLSD cannot = LLSD::emptyArray();
    S32 left_alone_count = 0;
    S32 cannot_count = 0;
    S64 set_id = 0;
    S64 snap_id = 0;
};

namespace
{
    struct RunState
    {
        LLSD done = LLSD::emptyArray();
        LLSD failed = LLSD::emptyArray();
        S32 done_count = 0;
        S32 failed_count = 0;
        S32 renamed = 0;
        S32 outstanding = 0;   // moves waiting for a folder to be made again
        bool all_started = false;
        bool reported = false;
        std::function<void(const LLSD&)> finished;
        std::shared_ptr<Remaker> remaker = std::make_shared<Remaker>();

        LLSD summary() const
        {
            LLSD s;
            s["put_back"] = done_count;
            s["could_not"] = failed_count;
            s["done"] = done;
            s["failed"] = failed;
            return s;
        }

        void maybeFinish()
        {
            if (all_started && outstanding == 0 && !reported)
            {
                reported = true;
                if (finished) finished(summary());
            }
        }
    };

    /** Move now; true when the viewer's own model shows it where it belongs. */
    bool moveNow(const LLUUID& id, bool folder, const LLUUID& target, std::string& why)
    {
        why = whyNot(id, folder, target);
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
        switch (s.kind)
        {
        case LumenAIUndo::Step::RENAME:
        {
            if (!exists(s.id)) { why = "it is no longer in inventory"; break; }
            if (s.folder)
            {
                if (!get_is_category_renameable(&gInventory, s.id))
                {
                    why = "the viewer does not allow renaming that folder";
                    break;
                }
                rename_category(&gInventory, s.id, s.name);
            }
            else
            {
                LLViewerInventoryItem* i = gInventory.getItem(s.id);
                if (!i->getPermissions().allowModifyBy(gAgent.getID()) || i->getIsLinkType())
                {
                    why = "it cannot be renamed";
                    break;
                }
                LLSD updates; updates["name"] = s.name;
                update_inventory_item(s.id, updates, NULL);
            }
            ++st->renamed;
            ++st->done_count;
            addCapped(st->done, entry(s.label, "renamed back to \"" + s.name
                                     + "\" (Second Life confirms a rename a moment later)", "what"));
            return;
        }
        case LumenAIUndo::Step::TRASH:
        {
            if (!exists(s.id)) { why = "it is already gone"; break; }
            if (inTrash(s.id))
            {
                ++st->done_count;
                addCapped(st->done, entry(s.label, "already in the Trash", "what"));
                return;
            }
            if (s.folder)
            {
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
            ++st->done_count;
            addCapped(st->done, entry(s.label, s.folder ? "the folder it made, now empty, moved to the Trash"
                                                         : "moved back to the Trash", "what"));
            return;
        }
        case LumenAIUndo::Step::MOVE:
        {
            if (!exists(s.id)) { why = "it is no longer in inventory"; break; }
            if (liveFolder(s.target))
            {
                if (moveNow(s.id, s.folder, s.target, why))
                {
                    ++st->done_count;
                    addCapped(st->done, entry(s.label, "back in " + pathOf(s.target), "what"));
                    return;
                }
                break;
            }
            if (s.chain.folders.empty())
            {
                why = "the folder it was in is gone, and the record does not say where that was";
                break;
            }
            // The folder it came from is gone: make it again, then move.
            ++st->outstanding;
            const LLUUID id = s.id;
            const bool folder = s.folder;
            const std::string label = s.label;
            std::shared_ptr<RunState> keep = st;
            keep->remaker->ensure(s.chain, 0, gInventory.getRootFolderID(),
                [keep, id, folder, label](LLUUID dest)
                {
                    std::string w;
                    if (dest.isNull())
                    {
                        ++keep->failed_count;
                        addCapped(keep->failed, entry(label, "the folder it was in could not be made again"));
                    }
                    else if (moveNow(id, folder, dest, w))
                    {
                        ++keep->done_count;
                        addCapped(keep->done, entry(label, "back in " + pathOf(dest)
                                                   + " (that folder was made again)", "what"));
                    }
                    else
                    {
                        ++keep->failed_count;
                        addCapped(keep->failed, entry(label, w));
                    }
                    --keep->outstanding;
                    keep->maybeFinish();
                });
            return;
        }
        }
        ++st->failed_count;
        addCapped(st->failed, entry(s.label, why));
    }
}

LLSD LumenAIUndo::run(Plan& plan, const std::function<void(const LLSD&)>& finished)
{
    std::shared_ptr<RunState> st = std::make_shared<RunState>();
    LLSD out;
    out["left_alone"] = plan.left_alone;
    out["left_alone_count"] = plan.left_alone_count;
    out["cannot"] = plan.cannot;
    out["cannot_count"] = plan.cannot_count;
    out["steps"] = (S32)plan.steps.size();

    if (plan.steps.size() <= RUN_NOW_LIMIT)
    {
        for (const Step& s : plan.steps) doStep(s, st);
        out["put_back"] = st->done_count;
        out["could_not"] = st->failed_count;
        out["done"] = st->done;
        out["failed"] = st->failed;
        out["waiting_for_folders"] = st->outstanding;
        // Report at the end only when something is still on its way; what
        // finished in the call is already in this reply.
        const bool later = st->outstanding > 0;
        st->finished = [later, finished](const LLSD& s) { if (later && finished) finished(s); };
        st->all_started = true;
        st->maybeFinish();
        return out;
    }

    // Many steps: paced, so neither the frame nor Second Life takes them at once.
    st->finished = finished;
    std::vector<Step> steps = plan.steps;
    LLCoros::instance().launch("LumenAIUndoRun", [st, steps]()
    {
        S32 n = 0;
        for (const Step& s : steps)
        {
            doStep(s, st);
            if (++n % 25 == 0) llcoro::suspendUntilTimeout(0.1f);
        }
        st->all_started = true;
        st->maybeFinish();
    });
    out["paced"] = true;
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
    if (mTried) return false;
    // The account's own folder is set at login; before that there is nobody
    // whose inventory this would be.
    if (gDirUtilp->getLindenUserDir().empty() || !gInventory.isInventoryUsable()) return false;
    mTried = true;
    return open();
}

bool LumenAIUndo::open()
{
    mPath = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, UNDO_FILE);

    for (int attempt = 0; attempt < 2; ++attempt)
    {
        if (sqlite3_open_v2(mPath.c_str(), &mRead,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
        {
            LL_WARNS("LumenAIUndo") << "cannot open " << mPath << ": "
                                    << (mRead ? sqlite3_errmsg(mRead) : "?") << LL_ENDL;
            close();
            return false;
        }
        sqlite3_busy_timeout(mRead, 3000);

        bool ok = false;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(mRead, "PRAGMA quick_check;", -1, &st, nullptr) == SQLITE_OK)
        {
            ok = sqlite3_step(st) == SQLITE_ROW && col(st, 0) == "ok";
            sqlite3_finalize(st);
        }
        if (ok)
        {
            ok = execOn(mRead, "PRAGMA journal_mode=WAL;") && execOn(mRead, SCHEMA);
        }
        if (ok) break;

        // Not a cache: never delete it. Put it aside where it can still be
        // looked at, and start a new one.
        close();
        const std::string aside = mPath + ".set-aside-" + llformat("%lld", (long long)time(nullptr));
        LL_WARNS("LumenAIUndo") << "the inventory record failed its check; kept as " << aside
                                << " and a new one started" << LL_ENDL;
        LLFile::rename(mPath, aside);
        LLFile::rename(mPath + "-wal", aside + "-wal");
        LLFile::rename(mPath + "-shm", aside + "-shm");
        if (attempt == 1) return false;
    }

    // Is this file from a shape we know?
    {
        sqlite3_stmt* st = nullptr;
        int version = 0;
        if (sqlite3_prepare_v2(mRead, "SELECT v FROM meta WHERE k='schema';", -1, &st, nullptr) == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW) version = atoi(col(st, 0).c_str());
            sqlite3_finalize(st);
        }
        if (version == 0)
        {
            execOn(mRead, llformat("INSERT OR REPLACE INTO meta VALUES('schema','%d');",
                                   SCHEMA_VERSION).c_str());
        }
        else if (version > SCHEMA_VERSION)
        {
            // Written by a newer Lumen. Reading it might misread it; leave it be.
            LL_WARNS("LumenAIUndo") << "the inventory record is from a newer version ("
                                    << version << "); not used" << LL_ENDL;
            close();
            return false;
        }
    }

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
            if (db) job(db);
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

void LumenAIUndo::flush()
{
    std::unique_lock<std::mutex> lock(mMutex);
    // Bounded: a snapshot being written can take a second or two, and a
    // reply that waits longer than that should say what it has instead.
    mIdle.wait_for(lock, std::chrono::seconds(10),
                   [this] { return mJobs.empty() && !mWorking; });
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
    if (mInRequest && mRequestSet != 0) return mRequestSet;

    const S64 id = mNextSet++;
    const S64 at = (S64)time(nullptr);
    const std::string words = mInRequest ? mWords : std::string();
    const std::string source = mInRequest ? "assistant" : "endpoint";
    if (mInRequest) mRequestSet = id;
    mSeq = 0;
    post([id, at, words, source](sqlite3* db)
    {
        sqlite3_stmt* st = nullptr;
        if (prepared(db, "INSERT INTO sets(id,at,words,source) VALUES(?,?,?,?);", &st))
        {
            sqlite3_bind_int64(st, 1, id);
            sqlite3_bind_int64(st, 2, at);
            bindText(st, 3, words);
            bindText(st, 4, source);
            stepLogged(db, st, "recording a change set");
            sqlite3_finalize(st);
        }
    });
    return id;
}

void LumenAIUndo::addChange(S64 set_id, const std::string& kind, const LLUUID& id, bool folder,
                            const std::string& name_before, const std::string& name_after,
                            const LLUUID& parent_before, const LLUUID& parent_after,
                            const std::string& chain)
{
    if (set_id == 0) return;
    const S32 seq = ++mSeq;
    post([=](sqlite3* db)
    {
        sqlite3_stmt* st = nullptr;
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
            stepLogged(db, st, "recording a change");
            sqlite3_finalize(st);
        }
    });
    changed();
}

void LumenAIUndo::recordMove(const LLUUID& id, bool folder, const LLUUID& from, const LLUUID& to)
{
    const std::string name = nameOf(id);
    addChange(currentSet(), "move", id, folder, name, name, from, to, Chain::of(from).serialise());
}

void LumenAIUndo::recordRename(const LLUUID& id, bool folder, const std::string& before,
                               const std::string& after)
{
    const LLUUID p = parentOf(id);
    addChange(currentSet(), "rename", id, folder, before, after, p, p, std::string());
}

void LumenAIUndo::recordTrash(const LLUUID& id, bool folder, const LLUUID& from)
{
    const std::string name = nameOf(id);
    addChange(currentSet(), "trash", id, folder, name, name, from, parentOf(id),
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
    const std::string name = nameOf(id);
    addChange(set_id, "new_folder", id, true, name, name, LLUUID::null, parent, std::string());
}

bool LumenAIUndo::lastTrashedFrom(const LLUUID& id, LLUUID& parent_out, Chain& chain_out)
{
    if (!ensureOpen()) return false;
    flush();
    bool found = false;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mRead,
            "SELECT parent_before, chain FROM changes WHERE object_id=? AND kind='trash' "
            "ORDER BY set_id DESC, seq DESC LIMIT 1;", -1, &st, nullptr) == SQLITE_OK)
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

S64 LumenAIUndo::takeSnapshot(const std::string& reason)
{
    LLTimer timer;
    std::shared_ptr<std::vector<Node>> nodes = std::make_shared<std::vector<Node>>(captureTree());
    const S64 capture_ms = (S64)(timer.getElapsedTimeF32() * 1000.f);
    if (nodes->empty()) return 0;

    const S64 snap = mNextSnap++;
    const S64 at = (S64)time(nullptr);
    LL_INFOS("LumenAIUndo") << "snapshot " << snap << " (" << reason << "): " << nodes->size()
                            << " objects read in " << capture_ms << " ms" << LL_ENDL;

    post([this, nodes, snap, at, reason, capture_ms](sqlite3* db)
    {
        LLTimer t;
        execOn(db, "BEGIN;");
        // What the newest snapshot has, still open.
        struct Open { sqlite3_int64 row; std::string parent, name; };
        std::unordered_map<std::string, Open> open;
        sqlite3_stmt* st = nullptr;
        if (sqlite3_prepare_v2(db, "SELECT rowid,id,parent,name FROM nodes WHERE to_snap IS NULL;",
                               -1, &st, nullptr) == SQLITE_OK)
        {
            while (sqlite3_step(st) == SQLITE_ROW)
            {
                open[col(st, 1)] = Open{ sqlite3_column_int64(st, 0), col(st, 2), col(st, 3) };
            }
            sqlite3_finalize(st);
        }

        sqlite3_stmt* ins = nullptr;
        sqlite3_stmt* end = nullptr;
        sqlite3_prepare_v2(db, "INSERT INTO nodes(id,folder,parent,name,from_snap,to_snap) "
                               "VALUES(?,?,?,?,?,NULL);", -1, &ins, nullptr);
        sqlite3_prepare_v2(db, "UPDATE nodes SET to_snap=? WHERE rowid=?;", -1, &end, nullptr);
        S64 items = 0, folders = 0, written = 0;
        for (const Node& n : *nodes)
        {
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
                    sqlite3_step(end); sqlite3_reset(end);
                }
                open.erase(o);
                if (same) continue;
            }
            sqlite3_bind_text(ins, 1, id.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(ins, 2, n.folder ? 1 : 0);
            sqlite3_bind_text(ins, 3, parent.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_text(ins, 4, n.name.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int64(ins, 5, snap);
            sqlite3_step(ins); sqlite3_reset(ins);
            ++written;
        }
        // What is no longer there ends at this snapshot.
        for (const auto& o : open)
        {
            sqlite3_bind_int64(end, 1, snap);
            sqlite3_bind_int64(end, 2, o.second.row);
            sqlite3_step(end); sqlite3_reset(end);
        }
        sqlite3_finalize(ins);
        sqlite3_finalize(end);

        const S64 write_ms = (S64)(t.getElapsedTimeF32() * 1000.f);
        if (sqlite3_prepare_v2(db, "INSERT INTO snapshots VALUES(?,?,?,?,?,?,?);", -1, &st, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(st, 1, snap);
            sqlite3_bind_int64(st, 2, at);
            bindText(st, 3, reason);
            sqlite3_bind_int64(st, 4, items);
            sqlite3_bind_int64(st, 5, folders);
            sqlite3_bind_int64(st, 6, capture_ms);
            sqlite3_bind_int64(st, 7, write_ms);
            sqlite3_step(st);
            sqlite3_finalize(st);
        }
        execOn(db, "COMMIT;");

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
            execOn(db, "BEGIN;");
            for (S64 d : drop)
                execOn(db, llformat("DELETE FROM snapshots WHERE id=%lld;", (long long)d).c_str());
            // A row that ended at or before the oldest snapshot kept is in none of them.
            execOn(db, llformat("DELETE FROM nodes WHERE to_snap IS NOT NULL AND to_snap<=%lld;",
                                (long long)oldest_kept).c_str());
            execOn(db, "COMMIT;");
        }
        execOn(db, llformat("DELETE FROM changes WHERE set_id IN (SELECT id FROM sets WHERE at<%lld);",
                            (long long)(now - SET_KEEP_SECONDS)).c_str());
        execOn(db, llformat("DELETE FROM sets WHERE at<%lld;", (long long)(now - SET_KEEP_SECONDS)).c_str());

        LL_INFOS("LumenAIUndo") << "snapshot " << snap << " written: " << items << " items, "
                                << folders << " folders, " << written << " rows new, in "
                                << write_ms << " ms" << LL_ENDL;
        changed();
    });
    return snap;
}

bool LumenAIUndo::readSnapshot(S64 snap_id, std::vector<Node>& out)
{
    sqlite3_stmt* st = nullptr;
    bool exists_ = false;
    if (sqlite3_prepare_v2(mRead, "SELECT 1 FROM snapshots WHERE id=?;", -1, &st, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, snap_id);
        exists_ = sqlite3_step(st) == SQLITE_ROW;
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

LLSD LumenAIUndo::history(S32 limit)
{
    LLSD out = LLSD::emptyArray();
    if (!ensureOpen()) return out;
    flush();
    if (limit <= 0 || limit > 100) limit = 20;
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mRead,
            "SELECT s.id,s.at,s.words,s.source,s.undone_at,s.undo_note,"
            "  (SELECT COUNT(*) FROM changes c WHERE c.set_id=s.id) "
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
        }
        // What kinds, and the first few names, so it can be told apart.
        LLSD kinds;
        LLSD names = LLSD::emptyArray();
        sqlite3_stmt* c = nullptr;
        if (sqlite3_prepare_v2(mRead, "SELECT kind,name_before,name_after FROM changes WHERE set_id=? "
                                      "ORDER BY seq;", -1, &c, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_int64(c, 1, id);
            while (sqlite3_step(c) == SQLITE_ROW)
            {
                const std::string k = col(c, 0);
                kinds[k] = kinds[k].asInteger() + 1;
                if (names.size() < 4) names.append(col(c, 1));
            }
            sqlite3_finalize(c);
        }
        s["kinds"] = kinds;
        s["examples"] = names;
        out.append(s);
    }
    sqlite3_finalize(st);
    return out;
}

LLSD LumenAIUndo::snapshots()
{
    LLSD out = LLSD::emptyArray();
    if (!ensureOpen()) return out;
    flush();
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mRead, "SELECT id,at,reason,items,folders,capture_ms,write_ms "
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
        error = "The inventory record is not available yet -- the inventory has not loaded.";
        return plan;
    }
    flush();
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
    if (undone > 0)
    {
        error = llformat("Change set %lld was already undone, %s.", (long long)set_id,
                         whenText(undone).c_str());
        return plan;
    }
    plan.set_id = set_id;

    struct Row { std::string kind, nb, na; LLUUID id, pb, pa; bool folder; std::string chain; };
    std::vector<Row> rows;
    if (sqlite3_prepare_v2(mRead,
            "SELECT kind,object_id,folder,name_before,name_after,parent_before,parent_after,chain "
            "FROM changes WHERE set_id=? ORDER BY seq DESC;", -1, &st, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int64(st, 1, set_id);
        while (sqlite3_step(st) == SQLITE_ROW)
        {
            rows.push_back(Row{ col(st, 0), col(st, 3), col(st, 4), LLUUID(col(st, 1)),
                                LLUUID(col(st, 5)), LLUUID(col(st, 6)),
                                sqlite3_column_int(st, 2) != 0, col(st, 7) });
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
        if (!exists(r.id))
        {
            if (r.kind == "new_folder") continue;   // already gone: nothing to put back
            gone(label, r.kind == "trash"
                 ? "it was emptied from the Trash, so it is gone for good -- nothing can bring it back"
                 : "it is no longer in inventory (emptied from the Trash, given away or rezzed)");
            continue;
        }
        const LLUUID now = curParent(r.id);
        if (r.kind == "move" || r.kind == "trash")
        {
            if (now != r.pa)
            {
                alone(label, r.kind == "trash"
                      ? "taken out of the Trash since; it is in " + pathOf(now)
                      : "moved again since; it is in " + pathOf(now));
                continue;
            }
            Step s; s.kind = Step::MOVE; s.id = r.id; s.folder = r.folder; s.label = label;
            s.target = r.pb; s.chain = Chain::parse(r.chain);
            plan.steps.push_back(s);
            simParent[r.id] = r.pb;
        }
        else if (r.kind == "untrash")
        {
            if (now != r.pa) { alone(label, "moved since it came out of the Trash"); continue; }
            Step s; s.kind = Step::TRASH; s.id = r.id; s.folder = r.folder; s.label = label;
            plan.steps.push_back(s);
            simParent[r.id] = trash;
        }
        else if (r.kind == "rename")
        {
            if (curName(r.id) != r.na) { alone(label, "renamed again since; it is called \"" + curName(r.id) + "\""); continue; }
            Step s; s.kind = Step::RENAME; s.id = r.id; s.folder = r.folder; s.name = r.nb;
            s.label = r.na;
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
            Step s; s.kind = Step::TRASH; s.id = r.id; s.folder = true; s.label = label;
            plan.steps.push_back(s);
            simParent[r.id] = trash;
        }
    }
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
    LLSD out = run(plan, [this, id](const LLSD& s)
    {
        const std::string note = llformat("%d put back, %d could not be",
                                          s["put_back"].asInteger(), s["could_not"].asInteger());
        post([id, note](sqlite3* db)
        {
            execOn(db, llformat("UPDATE sets SET undo_note='finished later: %s' WHERE id=%lld;",
                                note.c_str(), (long long)id).c_str());
        });
        LLSD args; args["MESSAGE"] = "Undo finished: " + note + ".";
        LLNotificationsUtil::add("SystemMessageTip", args);
        changed();
    });
    out["change_set"] = (LLSD::Integer)id;
    const S64 now = (S64)time(nullptr);
    post([id, now](sqlite3* db)
    {
        execOn(db, llformat("UPDATE sets SET undone_at=%lld WHERE id=%lld;",
                            (long long)now, (long long)id).c_str());
    });
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
        error = "The inventory record is not available yet -- the inventory has not loaded.";
        return plan;
    }
    flush();
    std::vector<Node> nodes;
    if (!readSnapshot(snap_id, nodes))
    {
        error = llformat("There is no snapshot %lld -- inventory / history lists them.", (long long)snap_id);
        return plan;
    }
    plan.snap_id = snap_id;

    std::unordered_map<LLUUID, const Node*> byId;
    for (const Node& n : nodes) byId[n.id] = &n;
    const LLUUID root = gInventory.getRootFolderID();
    const LLUUID cof = gInventory.findCategoryUUIDForType(LLFolderType::FT_CURRENT_OUTFIT);

    // Folders first, shallowest first, so a folder comes back before what is in it.
    auto depth = [&](const Node& n)
    {
        S32 d = 0;
        LLUUID p = n.parent;
        for (S32 guard = 0; guard < 64 && p.notNull() && p != root; ++guard, ++d)
        {
            auto f = byId.find(p);
            if (f == byId.end()) break;
            p = f->second->parent;
        }
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

    std::map<LLUUID, LLUUID> simParent;   // folders this plan moves

    for (const auto& o : order)
    {
        const Node& n = *o.second;
        if (!exists(n.id))
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
        const LLUUID now = parentOf(n.id);
        if (within(n.id, cof)) continue;
        if (now != n.parent)
        {
            Step s; s.kind = Step::MOVE; s.id = n.id; s.folder = n.folder; s.label = n.name;
            s.target = n.parent; s.chain = chainFor(n.parent);
            plan.steps.push_back(s);
            simParent[n.id] = n.parent;
        }
        if (nameOf(n.id) != n.name)
        {
            Step s; s.kind = Step::RENAME; s.id = n.id; s.folder = n.folder; s.name = n.name;
            s.label = nameOf(n.id);
            plan.steps.push_back(s);
        }
    }
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
    out["nothing_to_do"] = plan.steps.empty();
    return out;
}

LLSD LumenAIUndo::restore(S64 snap_id)
{
    LLSD error;
    Plan plan = planRestore(snap_id, error);
    if (error.isDefined())
    {
        LLSD out; out["error"] = error; return out;
    }
    if (plan.steps.empty())
    {
        LLSD out; out["nothing_to_do"] = true; out["cannot"] = plan.cannot;
        out["cannot_count"] = plan.cannot_count; return out;
    }
    // A restore is a change too: take a snapshot first, so it can be undone
    // by restoring that one.
    const S64 before = takeSnapshot(llformat("before restoring snapshot %lld", (long long)snap_id));
    LLSD out = run(plan, [this](const LLSD& s)
    {
        LLSD args;
        args["MESSAGE"] = llformat("Inventory restore finished: %d put back, %d could not be.",
                                   s["put_back"].asInteger(), s["could_not"].asInteger());
        LLNotificationsUtil::add("SystemMessageTip", args);
        changed();
    });
    out["snapshot"] = (LLSD::Integer)snap_id;
    out["snapshot_before_restore"] = (LLSD::Integer)before;
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
    if (LumenAIUndo::instanceExists() && LumenAIUndo::instance().takeDirty()) reload();
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
    LumenAIUndo& u = LumenAIUndo::instance();
    if (!u.available())
    {
        show("Nothing to show yet: the inventory has not loaded.");
        return;
    }
    const S64 keep_set = selected(mSets);
    const S64 keep_snap = selected(mSnaps);
    mSets->deleteAllItems();
    const LLSD sets = u.history(50);
    for (LLSD::array_const_iterator it = sets.beginArray(); it != sets.endArray(); ++it)
    {
        const LLSD& s = *it;
        LLSD row;
        row["value"] = s["change_set"];
        std::string what = llformat("%d change%s", s["changes"].asInteger(),
                                    s["changes"].asInteger() == 1 ? "" : "s");
        if (s["undone"].asBoolean()) what += ", undone";
        row["columns"][0]["column"] = "when";  row["columns"][0]["value"] = s["when"];
        row["columns"][1]["column"] = "asked"; row["columns"][1]["value"] = s["asked"];
        row["columns"][2]["column"] = "what";  row["columns"][2]["value"] = what;
        mSets->addElement(row);
    }
    if (keep_set) mSets->selectByValue((LLSD::Integer)keep_set);

    mSnaps->deleteAllItems();
    const LLSD snaps = u.snapshots();
    for (LLSD::array_const_iterator it = snaps.beginArray(); it != snaps.endArray(); ++it)
    {
        const LLSD& s = *it;
        LLSD row;
        row["value"] = s["snapshot"];
        row["columns"][0]["column"] = "when";   row["columns"][0]["value"] = s["when"];
        row["columns"][1]["column"] = "reason"; row["columns"][1]["value"] = s["reason"];
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
        list(r["left_alone"], "Left alone, because it changed again since:", "why");
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
