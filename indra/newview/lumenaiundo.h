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
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

struct sqlite3;
class LLButton;   // <Lumen>
class LLScrollListCtrl;
class LLTextEditor;

// <Lumen>
/**
 * What the assistant may do to something in the inventory: ONE set of rules
 * for every path that moves it, renames it, deletes it or puts something into
 * a folder -- move_item, rename_item, delete_item, new_folder, a batch, undo,
 * restore and undelete.
 * Before there was one, undo and restore had a shorter list than move_item,
 * and could pull a listed item out of Marketplace or put the LSL bridge's
 * folder somewhere else.
 *
 * Each answers why not, as a clause ("it is in the Library, which is Linden
 * Lab's, not theirs"), or empty when it may. The caller makes the sentence;
 * `rule`, when given, says which rule it was, for a caller that has words of
 * its own for one of them.
 *
 * <Lumen> A folder the user PROTECTED (right-click > Protect) is stricter here
 * than the viewer's own rule, on purpose. The viewer keeps only the folder
 * itself where it is; for the assistant nothing inside it is moved out,
 * renamed or deleted either -- protecting a folder says "leave this alone",
 * and a batch "everything named pumpkin" must not drain one. Putting things
 * INTO it stays allowed: that harms nothing. The set is read live every time,
 * so protecting a folder mid-session counts at once. The folders the viewer
 * LOCKS -- the LSL bridge's, the AO's and the wearable favourites -- are off
 * limits whatever the Lock settings say: the assistant has no reason to touch
 * them.
 */
namespace LumenInventoryRules
{
    enum Rule
    {
        ALLOWED = 0,
        NOT_IN_INVENTORY,
        LIBRARY,
        CURRENT_OUTFIT,
        TRASH,
        LSL_BRIDGE,
        ANIMATION_OVERRIDER,
        WEARABLE_FAVORITES, // <Lumen> the wearable favourites folder, locked like the two above
        MARKETPLACE,
        SYSTEM_FOLDER,      // one of Second Life's own folders
        NAMED_FOLDER,       // #Firestorm, #Lumen, #RLV
        PROTECTED_FOLDER,   // protected by the user: the folder, or anything in it
        INSIDE_ITSELF,
        RLV,
        NOT_MODIFIABLE,     // no-modify, a link, a calling card
        VIEWER              // the viewer's own check said no
    };

    /** Is this -- or a folder above it -- somewhere the assistant leaves alone? `it` names it in the answer. */
    std::string offLimits(const LLUUID& id, const char* it = "it", Rule* rule = nullptr);
    /** <Lumen> The folder the user protected that this is, or is inside (the nearest), or null. */
    LLUUID protectedFolderOf(const LLUUID& id);
    /**
     * <Lumen> Why a protected folder keeps this as it is: it is one, or is in
     * one -- or, with `carried` (a folder being moved or deleted), it holds
     * one. Empty if none. Never asked of where something goes.
     */
    std::string inProtected(const LLUUID& id, bool carried = false, Rule* rule = nullptr);
    /** Why this may not be moved or renamed at all, wherever it would go. */
    std::string held(const LLUUID& id, bool folder, Rule* rule = nullptr);
    /** Why nothing may be put into this folder. */
    std::string notInto(const LLUUID& dest, Rule* rule = nullptr);
    /** Why this may not be moved into that folder: held(), notInto(), and the two together. */
    std::string notMove(const LLUUID& id, bool folder, const LLUUID& dest, Rule* rule = nullptr);
    /** Why this may not be renamed. */
    std::string notRename(const LLUUID& id, bool folder, Rule* rule = nullptr);
    /** <Lumen> Why this may not be put in the Trash. Worn things are the caller's to ask about. */
    std::string notDelete(const LLUUID& id, bool folder, Rule* rule = nullptr);
}
// </Lumen>

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
 * deleted, and a fresh one started -- and only a file SQLite says is damaged:
 * a busy, full or read-only disk is not a reason to start again.
 *
 * <Lumen> One viewer at a time: a second Lumen logged in as the same avatar
 * on the same computer finds the record held, says so, and asks again now
 * and then -- a viewer that was logged out by the second login is often
 * still open, showing its message.
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

    // <Lumen> `set_id` 0 is the request in progress, as always; a bulk run
    // passes the set beginBatch() gave it.
    void recordMove(const LLUUID& id, bool folder, const LLUUID& from, const LLUUID& to,
                    S64 set_id = 0);
    void recordRename(const LLUUID& id, bool folder, const std::string& before,
                      const std::string& after, S64 set_id = 0);
    /** Call AFTER it is in the Trash: where it is now is recorded too. */
    void recordTrash(const LLUUID& id, bool folder, const LLUUID& from, S64 set_id = 0);
    void recordUntrash(const LLUUID& id, bool folder, const LLUUID& to);
    /** A folder being made: the set it joins is fixed now, its id arrives later. */
    S64  setForNewFolder();
    void recordNewFolder(S64 set_id, const LLUUID& id, const LLUUID& parent);

    /**
     * A snapshot now, for a bulk operation. Returns its id, or 0. <Lumen> It
     * is written on the record's own thread a moment later, whole or not at
     * all: snapshotState() says which, and only a WRITTEN one may be named
     * to anybody as there.
     */
    S64  snapshotBefore(const std::string& reason);
    enum SnapState { SNAP_UNKNOWN, SNAP_PENDING, SNAP_WRITTEN, SNAP_FAILED };   // <Lumen>
    SnapState snapshotState(S64 snap_id);                                     // <Lumen>

    // <Lumen> ---- a bulk run (inventory / batch) ------------------------------
    /**
     * A bulk run is ONE change set of its own, whatever Assistant turn starts
     * or ends while it is still going -- a run of thousands is paced and
     * outlasts the turn that asked for it. Every change it makes is recorded
     * under the set this returns (0: the record is not available). `what` is
     * kept with it, after the person's own words when a turn is in progress.
     * Until endBatch(), the set cannot be undone: it is still being written.
     */
    S64  beginBatch(const std::string& what);
    void endBatch(S64 set_id);
    /** A bulk run is still going. Undo and restore wait for it. */
    bool bulkRunning() const { return !mRunning.empty(); }
    /**
     * Ask every bulk run to stop after the change in hand -- Clear in the
     * Assistant window, or Stop in the history window. What it did stays one
     * change set, undoable. True when a run was going.
     */
    bool stopBulk();
    /** Has this run been asked to stop? The run asks between changes. */
    bool stopAsked(S64 set_id) const { return mStopAsked.count(set_id) > 0; }
    // </Lumen>

    /**
     * Where this item was before the assistant last put it in the Trash.
     * <Lumen> Never waits for the record to be written: what is still on its
     * way to the file is answered from memory.
     */
    bool lastTrashedFrom(const LLUUID& id, LLUUID& parent_out, Chain& chain_out);

    // ---- reading and putting back ---------------------------------------------
    /**
     * <Lumen> `settled`: wait (briefly) for what is still being written, so
     * a change made a moment ago is in the answer. The window passes false
     * and reads what is on disk -- it reads again when more arrives -- so a
     * snapshot being written never holds up a frame.
     */
    LLSD history(S32 limit, bool settled = true);
    LLSD snapshots(bool settled = true);
    /**
     * Undo one change set; 0 is the newest one not yet undone. <Lumen> A set
     * undone before, with things that could not be put back then, may be
     * named again: only those are tried.
     */
    LLSD undo(S64 set_id);
    /** What restoring a snapshot would change, with up to `sample` examples of each. */
    LLSD previewRestore(S64 snap_id, S32 sample);
    /** Put the inventory back as the snapshot has it, as far as possible. */
    LLSD restore(S64 snap_id);
    /** An undo or a restore is still putting things back. */
    bool puttingBack() const;

    bool available();
    /** <Lumen> Why available() is false, in words for the person; empty if it is not. */
    std::string unavailableWhy() const { return mWhyUnavailable; }
    std::string path() const { return mPath; }

    /** Called by the writer thread and the runner: the window should read again. */
    void changed();
    bool takeDirty() { return mDirty.exchange(false); }

    /**
     * <Lumen> The end of a run: what it did, the change rows it put back,
     * and whether it ended after run() had already answered.
     */
    typedef std::function<void(const LLSD& summary, const std::vector<S64>& rows, bool later)> Finished;

private:
    struct Node
    {
        LLUUID id, parent;
        std::string name;
        bool folder = false;
    };
    // <Lumen> How opening the file went. Only BAD_FILE sets it aside.
    enum OpenResult { OPENED, BAD_FILE, NOT_NOW, TOO_NEW };
    bool open();
    OpenResult openFile();
    void setAside();
    bool lockRecord();
    void unlockRecord();
    // </Lumen>
    void close();
    bool ensureOpen();
    void startWorker();
    void stopWorker();
    void post(std::function<void(sqlite3*)> job);
    /** Wait until everything posted so far is on disk. Bounded: false if it is not. */
    bool flush();

    S64  currentSet();
    S64  liveSet(S64 wanted);   // <Lumen>
    void addSet(S64 id, const std::string& words, const std::string& source);   // <Lumen>
    void addChange(S64 set_id, const std::string& kind, const LLUUID& id, bool folder,
                   const std::string& name_before, const std::string& name_after,
                   const LLUUID& parent_before, const LLUUID& parent_after,
                   const std::string& chain);
    S64  takeSnapshot(const std::string& reason);
    std::vector<Node> captureTree() const;
    bool readSnapshot(S64 snap_id, std::vector<Node>& out, bool* partial = nullptr);

    Plan planUndo(S64 set_id, LLSD& error);
    Plan planRestore(S64 snap_id, LLSD& error);
    LLSD run(Plan& plan, const Finished& finished);

    sqlite3*    mRead = nullptr;     // main thread: reads only
    sqlite3*    mLock = nullptr;     // <Lumen> holds the lock file while the record is ours
    std::string mPath;
    bool        mTried = false;
    F64         mRetryAt = 0.0;      // <Lumen> held by another viewer: ask again after this
    std::string mWhyUnavailable;     // <Lumen>

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

    // <Lumen> Main thread only.
    std::set<S64> mClosed;           // undone, or being undone: nothing more is added to them
    std::set<S64> mUndoing;          // an undo of these is still going
    std::set<S64> mRunning;          // bulk runs still writing to these
    std::set<S64> mStopAsked;        // ...and the ones asked to stop
    std::map<S64, S64> mLateSets;    // closed set -> where what arrives for it late goes

    // <Lumen> Shared with the writer thread, under mStateMutex.
    struct PendingTrash { LLUUID parent; std::string chain; S32 seq = 0; };
    std::mutex  mStateMutex;
    std::unordered_map<LLUUID, PendingTrash> mPendingTrash;   // recorded, not on disk yet
    std::map<S64, SnapState> mSnapStates;   // snapshots taken this login, and how their write went

    // <Lumen> previewRestore() and restore() in one call plan once.
    S64         mPlannedSnap = 0;
    U32         mPlannedFrame = 0;
    std::shared_ptr<Plan> mPlanned;
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
    void onStop();      // <Lumen> a bulk run
    void onPreview();
    void onRestore();
    S64  selected(LLScrollListCtrl* list) const;
    void show(const std::string& text);

    LLScrollListCtrl* mSets = nullptr;
    LLScrollListCtrl* mSnaps = nullptr;
    LLTextEditor*     mText = nullptr;
    LLButton*         mStopBtn = nullptr;   // <Lumen>
    F64               mLastReload = 0.0;   // <Lumen> read again at most once a second
};

#endif // LUMEN_AIUNDO_H
