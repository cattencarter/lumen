/**
 * @file lumenaiundo.h
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
#ifndef LUMEN_AIUNDO_H
#define LUMEN_AIUNDO_H

#include "llfloater.h"
#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

struct sqlite3;
class LLScrollListCtrl;
class LLTextEditor;

/**
 * A record of every inventory change the assistant makes, and the means to
 * undo it -- as far as Second Life allows.
 *
 * Asked for so the assistant could tidy an inventory in bulk without that
 * being a leap of faith: "put everything named pumpkin into the Halloween
 * folder" is one sentence, and a misheard word in it moves hundreds of things.
 * The guard comes before the tool that needs it.
 *
 * Two layers, both on disk in the account's own folder (so per avatar and per
 * grid), and neither leaves the computer:
 *
 *  - **Change sets.** Every move, rename, delete to the Trash and new folder,
 *    with what it was before, grouped by the request that caused it and kept
 *    with the person's own words. inventory / undo reverses one, and leaves
 *    alone -- and lists -- anything changed by hand since.
 *  - **Snapshots** of the STRUCTURE: every folder and item, its name and
 *    where it is, no contents. Taken before the first assistant change of a
 *    login and before any bulk operation, so a change can be put right after
 *    the conversation that made it is gone. Stored as differences from the
 *    snapshot before, so a snapshot of an unchanged inventory costs nothing.
 *
 * **This is not a cache.** A cache can be thrown away and fetched again; this
 * cannot. A file that fails its check is set aside under another name, never
 * deleted, and a fresh one started.
 *
 * Where it stops, said everywhere it matters: anything emptied from the Trash
 * is gone, and so is a no-copy item given away or rezzed. Nothing here claims
 * a restore it did not see happen.
 *
 * Writes go through one worker thread with its own connection, so a snapshot
 * of a quarter of a million items never holds up a frame -- or a change being
 * recorded behind it.
 */
class LumenAIUndo : public LLSingleton<LumenAIUndo>
{
    LLSINGLETON(LumenAIUndo);
    ~LumenAIUndo();

public:
    /** One step of an undo or a restore, and the whole list of them. */
    struct Step;
    struct Plan;

    /** One step back towards how things were. */
    struct Chain
    {
        /** Folders from the top of the inventory down to the parent: id and name. */
        std::vector<std::pair<LLUUID, std::string>> folders;
        std::string serialise() const;
        static Chain parse(const std::string& text);
        static Chain of(const LLUUID& folder_id);
    };

    // ---- the request in progress -------------------------------------------
    /**
     * An Assistant turn starts: what it changes from now on is ONE change set,
     * kept with these words. Outside a turn -- a host application calling the
     * endpoint directly -- every change is a set of its own.
     */
    void beginRequest(const std::string& words);
    void endRequest();

    // ---- recording -----------------------------------------------------------
    /**
     * Call BEFORE changing anything. The first change of a login is preceded by
     * a snapshot, and it has to see the inventory as it was.
     */
    void prepare();

    void recordMove(const LLUUID& id, bool folder, const LLUUID& from, const LLUUID& to);
    void recordRename(const LLUUID& id, bool folder, const std::string& before,
                      const std::string& after);
    void recordTrash(const LLUUID& id, bool folder, const LLUUID& from);
    void recordUntrash(const LLUUID& id, bool folder, const LLUUID& to);
    /** A folder being made: the set it joins is fixed now, its id arrives later. */
    S64  setForNewFolder();
    void recordNewFolder(S64 set_id, const LLUUID& id, const LLUUID& parent);

    /** A snapshot now, for a bulk operation. Returns its id, or 0. */
    S64  snapshotBefore(const std::string& reason);

    /** Where this item was before the assistant last put it in the Trash. */
    bool lastTrashedFrom(const LLUUID& id, LLUUID& parent_out, Chain& chain_out);

    // ---- reading and putting back ---------------------------------------------
    LLSD history(S32 limit);
    LLSD snapshots();
    /** Undo one change set; 0 is the newest one not yet undone. */
    LLSD undo(S64 set_id);
    /** What restoring a snapshot would change, with up to `sample` examples of each. */
    LLSD previewRestore(S64 snap_id, S32 sample);
    /** Put the inventory back as the snapshot has it, as far as possible. */
    LLSD restore(S64 snap_id);

    bool available();
    std::string path() const { return mPath; }

    /** Called by the writer thread and the runner: the window should read again. */
    void changed();
    bool takeDirty() { return mDirty.exchange(false); }

private:
    struct Node
    {
        LLUUID id, parent;
        std::string name;
        bool folder = false;
    };
    bool open();
    void close();
    bool ensureOpen();
    void startWorker();
    void stopWorker();
    void post(std::function<void(sqlite3*)> job);
    /** Wait until everything posted so far is on disk. Bounded. */
    void flush();

    S64  currentSet();
    void addChange(S64 set_id, const std::string& kind, const LLUUID& id, bool folder,
                   const std::string& name_before, const std::string& name_after,
                   const LLUUID& parent_before, const LLUUID& parent_after,
                   const std::string& chain);
    S64  takeSnapshot(const std::string& reason);
    std::vector<Node> captureTree() const;
    bool readSnapshot(S64 snap_id, std::vector<Node>& out);

    Plan planUndo(S64 set_id, LLSD& error);
    Plan planRestore(S64 snap_id, LLSD& error);
    LLSD run(Plan& plan, const std::function<void(const LLSD&)>& finished);

    sqlite3*    mRead = nullptr;     // main thread: reads only
    std::string mPath;
    bool        mTried = false;

    std::thread mWorker;
    std::mutex  mMutex;
    std::condition_variable mWake, mIdle;
    std::deque<std::function<void(sqlite3*)>> mJobs;
    bool        mStop = false;
    bool        mWorking = false;

    S64         mNextSet = 1;
    S64         mNextSnap = 1;
    S64         mRequestSet = 0;     // the set the turn in progress writes to, once it has one
    bool        mInRequest = false;
    std::string mWords;
    bool        mSnapshotThisLogin = false;
    S32         mSeq = 0;
    std::atomic<bool> mDirty{ false };
};

/**
 * Comm > Assistant Inventory History: what the assistant changed, each with
 * Undo, and the snapshots, each with a preview and Restore.
 *
 * The window exists for the day the conversation is gone -- cleared, another
 * provider, a restart -- and something is still not where it was.
 */
class LumenAIUndoFloater : public LLFloater
{
public:
    LumenAIUndoFloater(const LLSD& key);
    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void draw() override;

    void reload();

private:
    void onUndo();
    void onPreview();
    void onRestore();
    S64  selected(LLScrollListCtrl* list) const;
    void show(const std::string& text);

    LLScrollListCtrl* mSets = nullptr;
    LLScrollListCtrl* mSnaps = nullptr;
    LLTextEditor*     mText = nullptr;
};

#endif // LUMEN_AIUNDO_H
