/**
 * @file lumenaiundo.h
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
#ifndef LUMEN_AIUNDO_H
#define LUMEN_AIUNDO_H

#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class LLInventoryObserver;

// <Lumen>
/**
 * What the assistant may do to something in the inventory: ONE set of rules
 * for every path that moves it, renames it, deletes it or puts something into
 * a folder -- move_item, rename_item, delete_item, new_folder, a batch, undo,
 * redo and undelete.
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
 * and a batch "everything named Christmas" must not drain one. Putting things
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
    /**
     * <Lumen> Why a folder -- made, or renamed -- may not be given this NAME
     * inside `parent`. The rules above ask about names folders have now; this
     * asks about the one being given. #Firestorm, #Lumen and #RLV at the top
     * of the inventory, and any #-name directly inside a #Firestorm or #Lumen
     * folder, are names the viewer and RLV find their own folders by: a second
     * one could be taken for theirs, and the assistant could not take it back
     * (held() refuses such a folder by its name). `parent_name` stands in for
     * a parent not made yet (a null `parent`), which is never the top.
     */
    std::string notNamed(const LLUUID& parent, const std::string& name, Rule* rule = nullptr,
                         const std::string& parent_name = std::string());
}
// </Lumen>

/**
 * Undo and redo for the inventory, for this login only -- like Ctrl-Z and
 * Ctrl-Y in any program. The author, 2026-10-04: what matters is the
 * assistant reading a request wrong and making a mess of the inventory, and
 * catching that soon after it happened.
 *
 * **One list of steps, newest last, at most thirty.** A step is one whole
 * action, named for what was done and who did it: "Assistant: moved 885 items
 * to Christmas" (an Assistant turn, or a bulk run, is one step), "You: deleted
 * 3 items" (the person's own drag, delete, rename or new folder in the viewer
 * -- grouped when close together, so one drag of twenty items is one step).
 * Undo reverses the newest; redo puts it back; any new change after an undo
 * clears what could be redone.
 *
 * **Nothing is kept on disk.** The list starts empty at login and is gone at
 * logout. Only one viewer can be logged in to an account at a time, so for
 * the whole of a login every change goes through this one -- which is what
 * lets undo be sure of what it is putting back. (Snapshots and their restore
 * went with the file they lived in: see Decisions.)
 *
 * **The person's own changes** are picked up by an inventory observer and
 * compared with Lumen's own copy of where each thing sits and what it is
 * called, so "before" is known. The assistant's changes are announced by the
 * tools that make them, and claimed, so the observer does not take them for
 * the person's; undo and redo claim theirs the same way. Nothing the viewer
 * does for itself is a step: Current Outfit, the bridge, the AO, the
 * favourites, Marketplace -- the places the shared rules keep the assistant
 * out of -- and nothing that merely arrives (deliveries, the inventory
 * loading). The person's changes are watched once the whole inventory has
 * loaded: before that, a folder arriving from Second Life can look like a
 * move.
 *
 * **Undo and redo are always asked first** -- a LumenAsk question, never
 * remembered, so the assistant cannot answer it -- showing what will change.
 * The plan is made when the question opens and made again on Accept; a thing
 * changed since is left alone and said. A toast says what was done.
 *
 * Where it stops: anything emptied from the Trash, given away or rezzed
 * cannot come back. Emptying the Trash drops the steps that were only about
 * what was in it, and the rest say what is gone.
 */
class LumenAIUndo : public LLSingleton<LumenAIUndo>
{
    LLSINGLETON(LumenAIUndo);
    ~LumenAIUndo();

public:
    /** One step of an undo or a redo as it runs, and the whole list of them. */
    struct Step;
    struct Plan;

    /** Where a folder sat, by id and by name, so it can be made again. */
    struct Chain
    {
        /** Folders from the top of the inventory down to the folder: id and name. */
        std::vector<std::pair<LLUUID, std::string>> folders;
        static Chain of(const LLUUID& folder_id);
    };

    /**
     * Undo and Redo in the inventory window's gear menu, and the watching of
     * the inventory. At start-up, before any inventory window is built.
     */
    static void registerMenu();

    // ---- the request in progress -------------------------------------------
    /**
     * An Assistant turn starts: what it changes from now on is ONE step, kept
     * with these words. Outside a turn -- a host application, or a test,
     * calling the endpoint directly -- nothing is listed (the debug setting
     * LumenAIUndoEndpointCalls lists each change as a step of its own).
     */
    void beginRequest(const std::string& words);
    void endRequest();
    /** <Lumen> Which Assistant request is in progress: a new number for each, 0 outside one. */
    U64  requestSerial() const { return mInRequest ? mRequestSerial : 0; }

    // ---- the assistant's changes ----------------------------------------------
    // `set_id` 0 is the step the request in progress writes to; a bulk run
    // passes the one beginBatch() gave it, and a change answered later the one
    // setForLater() gave when it was sent. A step undone since is not added
    // to: what arrives for it goes in the step in progress.
    // Each says whether the change went into the list (false outside a turn).
    bool recordMove(const LLUUID& id, bool folder, const LLUUID& from, const LLUUID& to,
                    S64 set_id = 0);
    /**
     * Call when the rename is SENT. Under AIS the viewer's own copy of the name
     * changes only when Second Life answers; recorded now, an undo straight
     * after it finds this step rather than an older one, and reads the old
     * name as "not confirmed yet" while renamePending(). Two renames of one
     * thing in one step are one: from the first name to the last.
     */
    bool recordRename(const LLUUID& id, bool folder, const std::string& before,
                      const std::string& after, S64 set_id = 0);
    /** The step a change Second Life answers for later goes in: fixed now. */
    S64  setForLater();
    void renameSent(const LLUUID& id);
    void renameAnswered(const LLUUID& id);
    bool renamePending(const LLUUID& id) const { return mRenamesInFlight.count(id) > 0; }
    /** Call AFTER it is in the Trash. */
    void recordTrash(const LLUUID& id, bool folder, const LLUUID& from, S64 set_id = 0);
    void recordUntrash(const LLUUID& id, bool folder, const LLUUID& to);
    /** A folder being made: the step it joins is fixed now, its id arrives later. */
    S64  setForNewFolder();
    void recordNewFolder(S64 set_id, const LLUUID& id, const LLUUID& parent);

    /** Undo and redo say they will change these: what the observer then sees is not the person's. */
    void claimParent(const LLUUID& id, const LLUUID& parent);
    void claimName(const LLUUID& id, const std::string& name);

    // ---- a bulk run (inventory / batch) ----------------------------------------
    /**
     * A bulk run is ONE step of its own, whatever Assistant turn starts or
     * ends while it is still going. `what` is kept with it, after the
     * person's own words when a turn is in progress. Until endBatch() it is
     * not undone: it is still being made.
     */
    S64  beginBatch(const std::string& what);
    void endBatch(S64 set_id, bool stopped = false);
    bool bulkRunning() const { return !mRunning.empty(); }
    /**
     * Ask every bulk run, undo and redo to stop after the change in hand --
     * Clear in the Assistant window. What it did stays done. True when
     * something was going.
     */
    bool stopBulk();
    bool stoppable() const { return bulkRunning() || puttingBack(); }
    bool stopAsked(S64 set_id) const { return mStopAsked.count(set_id) > 0; }

    /** Where this was before it was last put in the Trash this login -- by the assistant or by hand. */
    bool lastTrashedFrom(const LLUUID& id, LLUUID& parent_out) const;

    // ---- the list, undo and redo ---------------------------------------------
    /** The steps that can be undone, newest first, and those that can be redone. */
    LLSD list() const;
    /** "Undo: assistant moved 885 items to Christmas", or plain "Undo" when there is none. */
    std::string menuLabel(bool redo) const;
    bool canUndo(bool redo) const;
    /**
     * What undoing (or redoing) the newest step would change now, for the
     * question: `step` is the one it is about, `text` the lines it shows.
     * An error when there is nothing, or something is still running.
     */
    LLSD preview(bool redo);
    /**
     * Do it -- for `step` only, which must still be the newest: the question
     * was about that one. Planned again now; what changed since is left alone.
     * The toast says how it went; the answer says it too.
     */
    LLSD perform(bool redo, S64 step, S32 rev = -1);
    /** An undo or a redo is still putting things back. */
    bool puttingBack() const;
    /** Why nothing may be put back now (logged out, something running), or empty. */
    std::string notNow(const char* what) const;

    /** The end of a run: what it did, the changes it did, and whether it ended after run() had answered. */
    typedef std::function<void(const LLSD& summary, const std::vector<S64>& rows, bool later)> Finished;

    // ---- the observer and the frame ------------------------------------------
    /** `created`: the viewer marked these as just made (AIS), not merely arrived. */
    void observed(const std::set<LLUUID>& ids, bool created);
    void tick();

private:
    struct Change;
    struct Entry;
    struct Known
    {
        LLUUID parent;
        std::string name;
    };
    struct Claim
    {
        LLUUID parent;
        std::string name;
        bool has_parent = false;
        bool has_name = false;
        F64 until = 0.0;
    };

    Entry* entryFor(S64 wanted);
    Entry* findEntry(S64 id);
    Entry& pushEntry(bool assistant, const std::string& words);
    void addChange(Entry& e, const Change& c);
    void personChange(const Change& c);
    void purged(const std::set<LLUUID>& ids);
    std::string describe(const Entry& e) const;
    bool housekeeping(const LLUUID& folder) const;
    void startWatching();

    bool planFor(Entry& e, bool redo, Plan& plan, LLSD& error);
    LLSD run(Plan& plan, const Finished& finished);

    std::deque<std::shared_ptr<Entry>> mUndo;   // oldest first; the newest is at the back
    std::deque<std::shared_ptr<Entry>> mRedo;   // the next to redo is at the back
    std::shared_ptr<Entry> mBusy;               // being undone or redone right now
    S64         mNextId = 1;
    S64         mRequestStep = 0;   // the step the turn in progress writes to, once it has one
    bool        mInRequest = false;
    std::string mWords;
    U64         mRevision = 0;      // changes recorded, ever: an undo or redo checks none came meanwhile
    U64         mRequestSerial = 0; // <Lumen> counts Assistant requests, for build / undo

    std::set<S64> mRunning;          // bulk runs still adding to these
    std::set<S64> mUnlisted;         // ...of those, runs outside a turn: not in the list
    std::set<S64> mStopAsked;        // ...and the ones asked to stop
    std::map<LLUUID, S32> mRenamesInFlight;   // sent, not answered yet: how many

    // Lumen's own copy of the inventory: where each thing sits and its name.
    std::unordered_map<LLUUID, Known> mKnown;
    std::unordered_map<LLUUID, Claim> mClaims;
    std::unordered_map<LLUUID, LLUUID> mTrashedFrom;
    std::set<LLUUID> mPending;       // changed, not looked at yet
    std::set<LLUUID> mCreated;       // ...of those, marked by the viewer as just made
    LLInventoryObserver* mObserver = nullptr;
    bool        mWatching = false;   // the observer is on and the copy made
    bool        mPersonToo = false;  // the whole inventory has loaded: the person's changes count
    F64         mNextSweep = 0.0;
};

#endif // LUMEN_AIUNDO_H
