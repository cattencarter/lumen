/**
 * @file lumenaictl.h
 * @brief A local control endpoint, so an assistant can drive the viewer.
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
#ifndef LUMEN_AICTL_H
#define LUMEN_AICTL_H

#include "llsingleton.h"
#include "llsd.h"
#include "llhttpnode.h"   // <Lumen> a socket reply held while the user is asked
#include "v3dmath.h"
#include "v3math.h"
#include "llquaternion.h"

#include <boost/signals2.hpp>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class LLMessageSystem;

class LLViewerInventoryItem;
class LLViewerObject;
class LLPumpIO;

/**
 * Owns the endpoint's lifetime and answers its requests.
 *
 * Deliberately not started automatically: see start(), and the settings it
 * reads. An endpoint inside the program holding someone's Second Life
 * credentials is opt-in.
 */
class LumenAIControl : public LLSingleton<LumenAIControl>
{
    LLSINGLETON(LumenAIControl);
    ~LumenAIControl();

public:
    /**
     * Bring the endpoint up if the user has enabled it.
     *
     * Safe to call when disabled, when already running, or when the port is
     * busy; each is reported and none is fatal. Returns true only when the
     * endpoint is actually listening afterwards.
     */
    bool start();

    /** Stop answering. The pump owns the socket, so this only detaches us. */
    void stop();

    bool isRunning() const { return mRunning; }

    /**
     * Where the avatar should be looking while a photo is framed, if anywhere.
     *
     * In third person the viewer points the avatar's head along the camera's
     * own view axis, nudged by where the mouse is on screen
     * (`LLAgentCamera::updateLookAt`). That is right when the camera sits
     * behind you -- you look where you are looking. It is wrong when the
     * camera has been placed in FRONT of you for a portrait: the same rule
     * makes her look away from the lens, and every twitch of the mouse drags
     * her eyes with it. At 0.68 m from her face the mouse may as well be her
     * face.
     *
     * Returns false unless a shot is framed, so the viewer's own behaviour is
     * untouched at every other moment.
     */
    static bool photoGaze(LLVector3& world_dir_out);

    /** Called by `camera`; cleared by `reset`. */
    void setPhotoGaze(bool on, const LLVector3d& camera_pos = LLVector3d::zero,
                      const std::string& mode = "camera");
    U16  port() const { return mPort; }

    /**
     * One captured event, with the sequence number a caller reads back from.
     *
     * Sequence numbers are monotonic and never reused, so a caller that holds
     * the last one it saw can ask for everything since without missing or
     * repeating a line, however irregularly it polls.
     */
    struct Event
    {
        U64 seq;
        LLSD data;
    };

    /**
     * Answer one JSON-RPC request.
     *
     * There is no credential to check. The endpoint is bound to loopback and
     * the node refuses anything carrying a browser's `Origin` or
     * `Sec-Fetch-Site` before this is reached; see Decisions 4 for why a token
     * was removed rather than made optional.
     */
    std::string handleRequest(const std::string& body);

    /**
     * <Lumen> Task 022: what the viewer knows an object to be called -- its
     * root's name, as look_nearby has it -- asking the region in the
     * background when it does not know yet (empty until the answer lands).
     */
    std::string objectNameFor(const LLUUID& id);

    /**
     * <Lumen> A held call made again by the viewer itself while it waits for
     * the region -- not by the model. The test log writes it as AGAIN, not
     * CALL: counted as calls, a search held for eight seconds read as the model
     * "spamming" it 67 times (2026-10-07).
     */
    struct RepeatScope
    {
        RepeatScope();
        ~RepeatScope();
    private:
        bool mWas;
    };

    /**
     * <Lumen> What inTheWay could and could not do, so that "the space it
     * fills was checked" is only ever said when it was (the review,
     * 2026-10-06).
     */
    struct WayCheck
    {
        bool flat = false;          // under 8 cm along its up axis: a rug, a mat
        bool cut_short = false;     // a line through it was not followed to its end
        std::vector<std::string> unseen;   // what it could not see, in words
    };

    /**
     * <Lumen> place and set: the solid things inside the space `obj` would
     * fill with its root at `new_root`, its own frame (`frame`, box
     * [lmin, lmax] from its root, as it stands now) turned to `to_new`.
     * Deepest first; `words` names the first four for a sentence; `how`
     * says what the check could not do.
     */
    LLSD inTheWay(LLViewerObject* obj, const std::vector<LLViewerObject*>& solid,
                  const LLQuaternion& frame, const LLVector3& lmin, const LLVector3& lmax,
                  const LLVector3& new_root, const LLQuaternion& to_new,
                  const LLVector3& wall_n, std::string& words, WayCheck& how) const;

    // <Lumen> worn_by and creator links.
    //
    // A reply from the in-world bridge arrives later, over HTTP, so it is
    // parked until the caller asks again (Findings 19). profileLink is here
    // rather than in the .cpp's anonymous namespace because itemToLLSD needs
    // it too, and one spelling of the link beats two that drift.
    static std::string profileLink(const LLUUID& agent_id);
    static std::string groupLink(const LLUUID& group_id);   // <Lumen>
    static bool wornRequestPending(const LLUUID& who);
    static void beginWornRequest(const LLUUID& who);
    static bool takeWornReply(const LLUUID& who, LLSD& out);
    static void finishWornReply(const LLUUID& who, const LLSD& data);
    // </Lumen>

    // <Lumen> The viewer asks the user itself before a dangerous action.
    //
    // A question put by the viewer, in its own window, cannot be talked past
    // the way a model can be talked into "the user already said yes". Each
    // dangerous handler calls askUser() after its own checks and just before
    // it acts. The first call puts the question on screen and answers
    // "waiting"; the same call made again once the user has answered goes
    // ahead or is refused. A remembered answer ("Always choose this option")
    // settles it at once, with nothing shown.
    //
    // Keyed by the call's fingerprint -- tool and exact arguments -- so a Yes
    // covers the very call that was shown to the user, once, and nothing else.

    /** Whether this question is still waiting for the user. */
    bool askPending(const LLUUID& ask_id);
    /** Take a question off the screen because nobody is waiting for it any more. */
    void withdrawAsk(const LLUUID& ask_id);
    /**
     * The question the request just handled is waiting on, if any, and forget
     * it. Read by the socket, which then holds its reply until the user answers.
     */
    LLUUID takeWaitingAsk();
    /** Hold a socket reply until the user answers, or HOLD seconds pass. */
    void holdForAnswer(LLHTTPNode::ResponsePtr response, const std::string& body,
                       const std::string& waiting_reply, const LLUUID& ask_id);

    // An answer that is a round trip to the region away, rather than a
    // person's decision: an object's contents, an object's properties. The
    // handler answers `settling` and says for how long with mSettle; the
    // socket then holds its reply and makes the SAME call again every quarter
    // second until the answer is not `settling` any more or the time is up,
    // and the Assistant's own turn waits the same way in its coroutine. So a
    // model makes one call instead of "ask again in a second" three times.
    // Only for handlers where the same call again CONTINUES what the first
    // began, rather than doing it twice.
    /** How long the request just handled wants its reply held, or 0; and forget it. */
    F64 takeSettle();
    /** Hold a socket reply while the call settles, for at most `seconds`. */
    void holdToSettle(LLHTTPNode::ResponsePtr response, const std::string& body,
                      const std::string& reply, F64 seconds);
    // </Lumen>

    // <Lumen> A reply the away-responder sent in the user's name, for
    // read_actions: which channel ("im", "local_chat", "arrival"), to whom and
    // how long -- never the words. The responder sends directly rather than
    // through handleRequest, so nothing else would record it.
    void noteAutomaticReply(const std::string& channel, const LLUUID& to, size_t characters);
    // </Lumen>


private:

    LLSD dispatch(const std::string& method, const LLSD& params);
    LLSD toolStatus() const;

    /**
     * A bounded record of one kind of event.
     *
     * Bounded on purpose. The viewer can receive chat faster than anything
     * reads it, and an unbounded buffer in a process that already struggles
     * for memory is a leak with a long fuse. Old entries are dropped, and a
     * reader that has fallen too far behind is told so rather than quietly
     * handed a gap.
     */
    class Stream
    {
    public:
        explicit Stream(size_t capacity) : mCapacity(capacity), mNextSeq(1), mDropped(0) {}

        void append(const LLSD& data);

        /** Everything after `since`, oldest first, at most `limit`; with `since` 0, the newest `limit`. */
        LLSD read(U64 since, size_t limit) const;

    private:
        std::deque<Event> mEvents;
        size_t            mCapacity;
        U64               mNextSeq;
        U64               mDropped;   //< how many were discarded before the oldest held
    };

    /**
     * Service our own pump, once per frame.
     *
     * The endpoint cannot share `gServicePump`. That one is only pumped from
     * `LLMessageSystem::checkAllMessages`, which does not run until the
     * message system is up, so an endpoint attached to it is deaf on the login
     * screen: the socket is bound and listening, and nothing ever accepts.
     * Found the hard way, 2026-09-13.
     */
    bool        mPhotoGaze = false;
    LLVector3d  mPhotoEye;
    std::string mPhotoGazeMode;

    bool tick(const LLSD&);

    /** start() is a guard around this; nothing here may escape as an exception. */
    bool startInternal();

    /**
     * A write that has already happened, remembered by the caller's own id.
     *
     * Every write tool takes a `request_id`. If the same one arrives twice the
     * recorded outcome is returned and nothing happens a second time. This is
     * not a nicety: a timeout on the caller's side is indistinguishable from a
     * failure, so without it an assistant that retries sends the message
     * twice, and a message sent twice to a real person cannot be taken back.
     */
    struct Action
    {
        std::string request_id;
        std::string fingerprint; //< tool plus arguments, for retries with no id
        std::string tool;
        std::string outcome;     //< "ok" or "failed"
        LLSD        result;      //< what the first attempt returned, replayed verbatim
        LLSD        summary;     //< what the user is shown; carries no message text
        F64         when;        //< seconds, for the duplicate window
        std::string at;          //< the same moment, readable
    };

    /**
     * Record of what this endpoint has done, oldest first.
     *
     * Holds ids and outcomes. **Not message bodies and not inventory
     * contents** — a log that quotes what was said is a transcript of someone's
     * private conversations sitting in a file.
     */
    LLSD actionLog(size_t limit) const;

    /** Subscribe to the viewer's own message signals. Idempotent. */
    void subscribe();
    /** Get the streams subscribed even when the socket is switched off. */
    void listenForStreams();
    void showDisclaimerWhenLoggedIn();
    void watchForLogin();   // <Lumen> when this session reached the world

    /**
     * Who we are following, and the loop that keeps it going.
     *
     * LLAgent::startFollowPilot is not what its name suggests: it aims the
     * autopilot at where the leader is now and re-aims it every frame *while
     * walking*, but the moment the stop distance is reached the autopilot
     * finishes and nothing restarts it. So it follows somebody on the way to
     * them and then stands still while they walk off.
     */
    void keepFollowing();
    /** <Lumen> End a follow, if there is one; true when there was. */
    bool stopFollowing();
    LLUUID mFollowing;
    // <Lumen> The last walk_to, so status can say it ended short of where it
    // was going -- the user took over, or something blocked the way -- rather
    // than a model waiting for an arrival that is not coming.
    mutable bool mWalkActive = false;   // status clears it on arrival
    LLVector3d  mWalkTarget;
    std::string mWalkTo;
    bool   mFollowListenerUp = false;
    // <Lumen> Whether they were flying when the follow began: it never takes
    // off by itself. Whether the autopilot ran last frame, so the frame it
    // stops can be told apart -- reaching them, or being stopped short by the
    // user's own keys or a wall. And where it left them, so the user walking
    // off by themselves ends it rather than being dragged back.
    bool       mFollowFlying = false;
    bool       mFollowWasPiloting = false;
    LLVector3d mFollowRestAt;
    bool mStreamListenerUp = false;
    bool mDisclaimerListenerUp = false;

    // <Lumen> The moment this session reached STATE_STARTED.  Second Life
    // delivers everything missed while away AT login, so "arrived after this"
    // is the same set as "arrived while you were away" -- derived rather than
    // remembered, so it is correct on a fresh install.  Null until seen.
    LLDate mLoggedInAt;
    bool   mLoginClockUp = false;
    // </Lumen>

    /**
     * Look up a previous write by the caller's request id.
     * Returns true and fills `out` when this id has been seen before.
     */
    bool recallAction(const std::string& request_id, LLSD& out) const;

    /**
     * The same question asked of the arguments instead of an id.
     *
     * Explicit ids are the clean mechanism and they are also the one that fails
     * quietly: a model that retries after a timeout has every reason to mint a
     * fresh id, and then the id-based check sees two different writes. So the
     * arguments themselves are fingerprinted, and an identical write arriving
     * inside `window` seconds is treated as that retry.
     *
     * This is a judgement call per tool, not a global one, which is why
     * `window` is a parameter. Saying the same thing twice in local chat is
     * something people genuinely do, so `say` passes 0 and opts out. Sending
     * the same instant message to the same person twice inside a minute is
     * almost always a retry, and the cost of being wrong there is one message
     * the user has to send again, against the cost of being right, which is
     * not sending a stranger the same thing twice.
     */
    bool recallRecent(const std::string& fingerprint, F64 window, LLSD& out) const;

    /** A stable fingerprint of a call: the tool and its arguments. */
    static std::string fingerprintOf(const std::string& tool, const LLSD& args);

    /**
     * Remember a completed write.
     *
     * `result` is replayed verbatim to a duplicate, so it holds whatever the
     * first attempt returned. `summary` is what `read_actions` shows and must
     * not contain message text: see actionLog().
     */
    void recordAction(const std::string& request_id, const std::string& fingerprint,
                      const std::string& tool, const std::string& outcome,
                      const LLSD& result, const LLSD& summary);

    // <Lumen> See askPending(). `notification` names a template in
    // notifications.xml; `subs` fills it. Returns true to go ahead; false
    // means `out` holds the reply instead -- the user said No, or the
    // question is on screen and not answered yet.
    bool askUser(const std::string& notification, const LLSD& subs,
                 const std::string& fingerprint, LLSD& out);
    static void onAskAnswered(const LLSD& notification, const LLSD& response);
    /** An object as the question names it: its own name, quoted, if known. */
    std::string askObjectName(LLViewerObject* object) const;
    void sweepAsks();
    void serviceHeldReplies();

    struct PendingAsk
    {
        LLUUID      id;                 //< the notification's id; also what callers wait on
        S32         state = 0;          //< 0 waiting, 1 yes, 2 no
        bool        remembered = false; //< answered by a saved choice, nothing shown
        bool        saved = false;      //< the user ticked "Always choose this option"
        F64         touched = 0.0;      //< last time a caller asked about it
        F64         answered = 0.0;
        std::string question;           //< the dialog's own words, for the waiting reply
    };
    std::unordered_map<std::string, PendingAsk> mAsks;   //< by fingerprint
    LLUUID mWaitingAsk;       //< set by askUser for the request being handled
    bool   mAnsweringAsk = false;   //< true inside LLNotificationsUtil::add
    F64    mSettle = 0.0;     //< set by a handler whose answer is a round trip away

    struct HeldReply
    {
        LLHTTPNode::ResponsePtr response;
        std::string body;
        std::string waiting_reply;
        LLUUID      ask_id;
        F64         since = 0.0;
        F64         settle_until = 0.0;   //< held for the region, not the user, until then
        F64         next_try = 0.0;
    };
    std::vector<HeldReply> mHeld;
    // </Lumen>

    /**
     * Notecard text, fetched asynchronously and held until someone reads it.
     *
     * A notecard lives in the asset system, not in inventory, so reading one is
     * a network round trip. This handler cannot wait for it -- it runs on the
     * frame loop, and a viewer that stops rendering while the assistant reads a
     * notecard is a viewer nobody uses. So the first call starts the fetch and
     * says so, and a later call finds the text here.
     *
     * Keyed by inventory item id. Values are maps: status, and text once ready.
     */
    LLSD mNotecards;

    /**
     * Names and descriptions of objects around the avatar, as the server tells
     * us them.
     *
     * An LLViewerObject has no name of its own -- a name arrives only in reply
     * to a properties request, and the viewer's own code feeds those replies to
     * whatever floater asked. Two hooks in llselectmgr.cpp call noteObjectName()
     * for EVERY reply, whoever asked: our own requests, Area Search's, and the
     * user's own selections.
     *
     * look_nearby asks in bulk, the way Area Search does: many objects per
     * message, a bounded number waiting at once. Asking one object per message
     * and at most 32 of them, and answering before any reply had landed, is how
     * a market 80 m away came back as "nothing like a market here".
     */
    struct ObjectLabel
    {
        std::string name;
        std::string desc;
    };
    std::unordered_map<LLUUID, ObjectLabel> mObjectLabels;
    /** Asked and not yet answered, with when -- the in-flight count per region. */
    std::unordered_map<LLUUID, F64> mNameAsked;
    /** Asked and never answered, with when, so it is not re-asked on every call. */
    std::unordered_map<LLUUID, F64> mNameGaveUp;
    /** Waiting to be asked, oldest first, so a busy region is paced not flooded. */
    std::deque<LLUUID> mNameQueue;
    std::unordered_set<LLUUID> mNameQueued;
    bool mNamingListenerUp = false;

    /** Queue these objects for a name, skipping any known, waiting or given up on. */
    void askNames(const std::vector<LLUUID>& ids);
    /** Send what the pacing allows, expire what never came back. Runs per frame while busy. */
    void pumpNaming();
    /** What we know an object is called, or NULL. */
    const ObjectLabel* objectLabel(const LLUUID& id) const;
    /** Whether a name for this object is still on its way. */
    bool nameOnItsWay(const LLUUID& id) const;

    /**
     * <Lumen> Task 017: the camera of the last build / picture, so a pixel in
     * it can be turned into a point in the world. Held in GLOBAL coordinates:
     * agent coordinates move whenever the user crosses into another region.
     */
    struct PictureCamera
    {
        bool       valid = false;
        LLVector3d origin;
        LLVector3  at, up;
        F32        fov = 0.f;
        S32        width = 0, height = 0;
        F64        taken = 0.0;
    };
    PictureCamera mLastPicture;
    bool mPlacing = false;   //< <Lumen> place is moving it through set; the space was checked

    /**
     * Second Life's own search -- places and events.
     *
     * This is the one kind of "find" that reaches outside the user's own
     * things. Everything else the assistant can search is theirs already:
     * inventory, landmarks, notecards, chat logs, what is nearby.
     *
     * The directory answers over UDP against a query id we mint, so it is
     * Findings 19's shape, like worn_by and web_presence: the first call
     * sends and answers `pending`, the second collects. Keyed by WHAT WAS
     * ASKED rather than by one "current search", so two questions in flight
     * cannot collect each other's answers.
     */
    struct DirSearch
    {
        LLUUID query_id;
        F64    asked      = 0.0;
        /** When the LAST reply landed: the directory answers in several. */
        F64    last_reply = 0.0;
        bool   answered   = false;
        U32    status   = 0;
        LLSD   rows;
    };
    /** Keyed "places|tapi market" / "events|market". */
    std::unordered_map<std::string, DirSearch> mDirSearches;
    /** Directory searches are spaced like the web ones: none sooner than this. */
    F64 mNextDirSearchAt = 0.0;
    /** The query id back to that key, because the reply carries only the id. */
    std::unordered_map<LLUUID, std::string> mDirByQuery;

    /** Send a directory query and remember it; returns the key to collect by. */
    std::string startDirSearch(const std::string& kind, const std::string& text);
    /** Record a reply against whichever search asked for it. */
    void noteDirRows(const LLUUID& query_id, const LLSD& rows, U32 status);

    /**
     * Reading where every landmark goes, quietly, after login.
     *
     * The author's call, 2026-09-23: landmarks before anything else, because
     * teleporting is what people do the moment they arrive. Only landmarks not
     * already in the database are read, so after the first session this is a
     * handful at most. Paced -- a few in flight at once -- and it stands aside
     * during a teleport, whose region handles it would be asking for.
     */
    std::deque<LLUUID> mLandmarkQueue;              // assets still to read
    std::unordered_set<LLUUID> mLandmarkQueued;
    std::unordered_map<LLUUID, F64> mLandmarkInFlight;
    std::unordered_set<LLUUID> mLandmarkNaming;     // loaded, waiting for the region's name
    // <Lumen> Map blocks asked for, by region handle. Asked WITHOUT a callback:
    // the world map keeps one pending lookup callback for the whole viewer, and
    // taking it lost the user's own SLURL clicks and typed locations.
    std::unordered_set<U64> mLandmarkMapAsked;
    std::unordered_set<LLUUID> mRegionsNoAnswer;    // asked this session and never answered
    std::unordered_set<LLUUID> mRegionsNowhere;     // answered: no such region
    S32  mLandmarksNowhere = 0;
    std::unordered_set<LLUUID> mLandmarkGaveUp;     // this session only
    bool mLandmarkFillUp = false;
    bool mLandmarkScanned = false;
    F64  mLandmarkFillStarted = 0.0;
    S32  mLandmarksReadThisSession = 0;
    void startLandmarkFill();
    void pumpLandmarks();
    void queueLandmark(const LLUUID& asset_id, bool first = false);

public:
    /** A landmark asset loaded, or its region was named: the two halves of a read. */
    static void landmarkLoaded(const LLUUID& asset_id);
    static void landmarkNamed(const LLUUID& asset_id, const std::string& region,
                              S32 x, S32 y, S32 z);
    /** A landmark just arrived in inventory. */
    static void landmarkAdded(const LLUUID& asset_id);

    /**
     * The directory answered. Chained from llstartup.cpp, which is where the
     * one handler per message name is registered -- so ours calls upstream's
     * first and then reads the same message again. Reading a message twice is
     * safe and is already what Firestorm does: their own search panel is
     * chained inside LLPanelDirBrowser's handler the same way.
     */
    static void onDirPlacesReply(LLMessageSystem* msg, void** user);
    static void onDirEventsReply(LLMessageSystem* msg, void** user);
    /** Landmarks still waiting to be read, for an honest answer meanwhile. */
    size_t landmarksStillReading() const { return mLandmarkQueue.size() + mLandmarkInFlight.size(); }
    /** Whether inventory has been looked through for landmarks at all yet. */
    bool landmarksScanned() const { return mLandmarkScanned; }

    /**
     * Called from llselectmgr.cpp when an object's properties arrive.
     *
     * Returns true when look_nearby asked for this one, so the selection
     * manager can skip warning that the object is not in its selection --
     * the same courtesy it already extends to Area Search.
     */
    static bool noteObjectName(const LLUUID& object_id, const std::string& name,
                               const std::string& desc);


    /**
     * Do not let the viewer auto-open the next notecard by this name.
     *
     * Creating a notecard is two server round trips: an empty item, then the
     * text uploaded into it. If the user has "show new inventory" switched on,
     * the viewer opens the card in between, its preview finds no asset, and
     * they get "Notecard is missing from the database" for a card that is
     * about to be perfectly fine.
     *
     * Suppression is by name and not by id because the ordering forces it:
     * notifyObservers() -- which is what opens the card -- runs before the
     * creation callback that would tell us the id.
     */
    static void suppressAutoOpen(const std::string& name);

    /** Called from llviewermessage.cpp. True means: do not open this one. */
    static bool consumeAutoOpenSuppression(const std::string& name, LLAssetType::EType type);

private:
    /** Names registered by suppressAutoOpen, with when, so they expire. */
    LLSD mSuppressOpen;

    // <Lumen> Names create_notecard made, with when. Unlike mSuppressOpen these
    // are not consumed on arrival: a card with no asset yet that is on this
    // list is still uploading its text, not empty.
    LLSD mCreatedNotecards;
    bool justCreatedNotecard(const std::string& name) const;
    // </Lumen>

    /**
     * Start fetching one notecard's text into mNotecards. Idempotent.
     * Returns false when there is nothing to fetch (no asset, or no region).
     */
    bool startNotecardFetch(LLViewerInventoryItem* item);

    /** Where getInvItemAsset lands. Static because the asset system takes a C callback. */
    static void onNotecardLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status);

    // <Lumen> Notecards read in the background after login, once the landmarks
    // are done, so the first search inside them answers at once. Driven by the
    // search alone, 2,858 cards took fifteen calls -- each a round trip to the
    // model -- and about three minutes, the first time and only the first time.
    std::deque<LLUUID> mNotecardQueue;                 // items still to read
    std::unordered_map<LLUUID, F64> mNotecardInFlight;  // item -> when it was asked for
    bool mNotecardFillUp = false;
    bool mNotecardScanned = false;
    S32  mNotecardsReadThisSession = 0;
    S32  mNotecardsNotRead = 0;
    void startNotecardFill();
    void pumpNotecards();
    size_t notecardsStillReading() const { return mNotecardQueue.size() + mNotecardInFlight.size(); }
    // </Lumen>

    void onInstantMessage(const LLSD& data);
    void onNearbyChat(const LLSD& data);

    /**
     * A short random string, new every time the endpoint starts.
     *
     * There to settle one question a person cannot otherwise answer: did the
     * assistant actually call the tools, or is it telling me what it expects
     * to be true? An assistant that has called `status` can say this back. One
     * that is guessing cannot, because there is nothing to guess from -- it
     * did not exist until this session began.
     *
     * Prompted by an assistant confidently naming the wrong skirt.
     */
    std::string mSessionCheck;

    bool        mRunning;
    U16         mPort;
    // <Lumen> tick() switched the endpoint off after an exception. The pump it
    // was servicing still holds the old socket, so the next start() must drop
    // it rather than add a second server to it.
    bool        mPumpStale = false;
    // The provider was switched away from the two that need the socket; the
    // next tick closes it, from outside the pump's own callback.
    bool        mProviderLeft = false;
    boost::signals2::connection mProviderConnection;
    // </Lumen>

    bool        mSubscribed;
    Stream      mMessages;   //< IM, group chat and ad-hoc, from one signal
    Stream      mChat;       //< nearby chat
    /// <Lumen> Which of our own lines in mMessages the away-responder wrote,
    /// by seq, so catch_up does not count "back shortly" as the user having
    /// answered somebody.
    std::unordered_set<U64> mAutomaticReplySeqs;

    boost::signals2::connection mMessageConnection;
    boost::signals2::connection mChatConnection;

    std::deque<Action> mActions;

    /** Ours alone, so servicing it cannot disturb the message system. */
    LLPumpIO*   mPump;
};

#endif // LUMEN_AICTL_H
