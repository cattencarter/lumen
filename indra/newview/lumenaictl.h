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

#include <boost/signals2.hpp>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>

class LLHTTPNode;
class LLViewerInventoryItem;
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

        /** Everything after `since`, oldest first, at most `limit`. */
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
    LLUUID mFollowing;
    bool   mFollowListenerUp = false;
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

    /**
     * Start fetching one notecard's text into mNotecards. Idempotent.
     * Returns false when there is nothing to fetch (no asset, or no region).
     */
    bool startNotecardFetch(LLViewerInventoryItem* item);

    /** Where getInvItemAsset lands. Static because the asset system takes a C callback. */
    static void onNotecardLoaded(const LLUUID& asset_id, LLAssetType::EType type,
                                 void* user_data, S32 status, LLExtStat ext_status);

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
    // </Lumen>

    bool        mSubscribed;
    Stream      mMessages;   //< IM, group chat and ad-hoc, from one signal
    Stream      mChat;       //< nearby chat

    boost::signals2::connection mMessageConnection;
    boost::signals2::connection mChatConnection;

    std::deque<Action> mActions;

    /** Ours alone, so servicing it cannot disturb the message system. */
    LLPumpIO*   mPump;
};

#endif // LUMEN_AICTL_H
