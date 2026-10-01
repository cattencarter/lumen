/**
 * @file lumenaichat.h
 * @brief Somewhere to command the assistant, inside the viewer.
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
#ifndef LUMEN_AICHAT_H
#define LUMEN_AICHAT_H

#include "llfloater.h"
#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

class LLLineEditor;
class LLTextEditor;
class LLTextBox;
class LLButton;

/**
 * A window you type into, and an assistant that can actually do things.
 *
 * Until now Lumen was driven from outside: a host application held the model
 * and reached in through the MCP endpoint. This is the same viewer holding the
 * model itself, so there is no second application to install and nothing to
 * configure but a key.
 *
 * **It goes through the same front door.** Every tool call is a JSON-RPC
 * request handed to `LumenAIControl::handleRequest`, exactly as Claude Desktop's
 * would be. Not `dispatch()` directly, and not a parallel implementation:
 * that way the idempotency, the ambiguity refusals, the no-copy confirmation
 * and the action log are all inherited rather than reimplemented, and
 * `read_actions` shows what this window did alongside everything else. One
 * path to keep correct instead of two that drift.
 *
 * **A floater, deliberately not an entry in the conversations list.** Putting
 * an assistant among someone's friends invites exactly one mistake -- thinking
 * you are talking to a person when you are not, or the reverse -- and the
 * person this is built for is the one least able to afford it.
 */
/**
 * Answers instant messages, in your voice, while you are away.
 *
 * Not a bot in Linden Lab's sense: the account is a person's own, used by that
 * person, and this is the same auto-response Firestorm has shipped for years
 * with the fixed text replaced by a written one. LL's rule is about an account
 * *primarily operating* as a scripted agent, which this is not.
 *
 * The purpose is continuity rather than notification. "She is away" is what a
 * canned reply already says; what it cannot do is keep a conversation, or a
 * character in a roleplay, from simply falling over the moment its owner steps
 * out of the room.
 *
 * Four things bound it, and each exists for a reason rather than for caution:
 *
 *  - **It only speaks while AFK**, driven by the viewer's own idle timer.
 *  - **It carries no tools.** It answers; it does not act. An assistant that
 *    could undress the avatar or give things away while nobody is watching is
 *    a different and much larger decision than this one.
 *  - **It counts.** Every reply is a paid request, so somebody who spams you
 *    could otherwise run up a bill while you sleep. There is a cap per person
 *    and a cap in total.
 *  - **The per-person cap is also the loop guard**, and deliberately so: the
 *    incoming-message signal does not carry the message type, so we cannot
 *    tell an auto-response from a person typing. Two of these talking to each
 *    other therefore stop after a handful of turns rather than never.
 */
class LumenAIAutoResponder : public LLSingleton<LumenAIAutoResponder>
{
    LLSINGLETON(LumenAIAutoResponder);
public:
    /** Offered every incoming instant message. Decides, and usually declines. */
    void consider(const LLSD& data);

    /**
     * Turned on and off by asking, not by a checkbox and not by a timer.
     *
     * The first version watched gAgent.getAFK(). That is a guess about intent
     * -- the idle timer fires while you sit reading, and has not fired yet when
     * you stand up -- where "answer for me until I am back" is a statement of
     * it. And a checkbox stays ticked: switched on for one lunch break, still
     * on three weeks later, which is exactly the case where it answers
     * something you would not have wanted it to.
     *
     * It is also the whole premise of this viewer. Asking in your own words is
     * the thing Lumen exists to allow; putting this behind a preferences panel
     * rebuilt the barrier it is meant to remove (Decisions 40, same argument).
     */
    /**
     * @param ims         answer instant messages
     * @param local_chat  answer in local chat when somebody says the user's name
     *
     * Two independent channels, not one with an extra. Asked to "answer in
     * local chat", the first version also started answering IMs -- announced,
     * but not asked for. Doing what was asked is easier to predict than doing
     * what was probably meant.
     */
    void arm(bool on, const std::string& note, bool ims, bool local_chat,
             const std::vector<std::string>& also_called = std::vector<std::string>(),
             const std::set<LLUUID>& only = std::set<LLUUID>(),
             bool on_arrival = false,
             const std::string& say = std::string());

    /**
     * Offered every line of nearby chat. Answers only when spoken to.
     *
     * Local chat is a room, not a conversation: everyone within earshot sees
     * every word, and a scripted object can talk too. So this is off unless
     * asked for separately, and even then it speaks only when the message
     * carries the avatar's name -- which is how people address each other in a
     * roleplay, and is the difference between holding a scene open and
     * answering every passer-by in a busy region.
     */
    void considerChat(const LLSD& data);
    bool armed() const { return mArmed; }
    const std::string& note() const { return mNote; }

private:
    bool shouldAnswer(const LLSD& data, std::string& why_not) const;
    bool withinTimeLimit() const;
    /**
     * <Lumen> Asked again after every wait inside a reply. A reply takes
     * seconds to write and seconds to "type", and the user can come back,
     * switch answering off, or be put under RLV in between; everything
     * shouldAnswer() checked was true when it started, not when it is sent.
     */
    bool stillWanted(U32 gen, bool speak_aloud, const LLUUID& to,
                     std::string& why) const;
    void replyTo(const LLUUID& from_id, const std::string& from,
                 const LLUUID& session_id, bool speak_aloud,
                 const std::string& latest);

    /// Bumped by every arm(), so a reply begun under an earlier arming is
    /// dropped even when answering was switched off and straight back on.
    U32                   mArmGen = 0;
    bool                  mArmed = false;
    bool                  mIMs = true;
    bool                  mLocalChat = false;
    /// Names given for this stint, on top of the saved ones.
    std::vector<std::string> mExtraNames;
    /**
     * Who is currently talking TO us in local chat, and until when.
     *
     * A name is how a conversation *opens*, not how every line of it is
     * written. "hi princess" got no answer because it carries no name, which
     * was correct by the rule and wrong for the conversation. So being named
     * starts a window, and inside it every line from that person is answered.
     */
    std::map<LLUUID, F64> mTalkingToMe;
    /// When arming happened, so the time limit can be measured from it.
    F64                   mArmedAt = 0.0;
    std::string           mNote;
    std::map<LLUUID, S32> mRepliesTo;
    // <Lumen> when non-empty, ONLY these people are answered
    std::set<LLUUID> mOnly;

    // <Lumen> "tell him I'm away when he arrives" -- a different trigger from
    // "when he writes". Someone who turns up and says nothing never writes.
    bool             mOnArrival = false;
    std::set<LLUUID> mSeen;        //< already near when armed, or already told
    std::set<LLUUID> mWasOnline;   //< online when armed, so logging in is not arriving
    std::set<LLUUID> mToldFirst;   //< has had the exact words already
    std::string      mSay;         //< exact words to relay, if any
    std::map<LLUUID, F32> mArrivedAt;  //< first sighting, so the greeting can settle
    LLFrameTimer     mArrivalPoll;
    bool             mArrivalWatch = false;
    void watchForArrivals();
    void checkArrivals();
    S32                   mRepliesTotal = 0;
    std::set<LLUUID>      mInFlight;
};

class LumenAIChatFloater : public LLFloater
{
public:
    /**
     * <Lumen> Offer the login summary; do not presume it is wanted.
     *
     * Counting what arrived is free -- the viewer has it. WRITING the summary
     * is the whole cost, measured: input almost entirely cached, 407 output
     * tokens doing all the work. So the offer appears instantly and nothing
     * is spent unless the user asks.
     */
    static void offerAtLogin();
    static void summariseNow();   //< the offer was accepted

    LumenAIChatFloater(const LLSD& key);
    ~LumenAIChatFloater() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

    /** Pay the prompt-processing cost while the window opens, not when they ask. */
    void warmLocalModel();

public:
    /**
     * Send the prompt and tools to a local server once, and say how it went.
     *
     * Public and static because Preferences calls it: the author's idea, and a
     * better one than warming on window-open alone -- *"den kan jo bare starte
     * naar man vaelger model som en test?"* It answers the question that panel
     * could not answer at all, which is whether the address and the model name
     * are right. `report` is called on the main thread when it finishes.
     */
    static void warmLocal(const std::string& url, const std::string& model,
                          std::function<void(bool ok, F64 seconds,
                                             const std::string& detail)> report);

    /**
     * Ask a provider one real question and say whether it answered.
     *
     * <Lumen> One Test button beside the provider list needs one entry
     * point, and the dialect knowledge already lives here rather than in
     * Preferences   which key goes in which header, which shape the body
     * takes, where the system prompt belongs. Putting the test in the panel
     * would have been a second copy of all three.
     *
     * Anthropic, OpenAI and a local model only. Codex and Claude Code are
     * separate PROGRAMS, so their test is a check of the installation and
     * stays where that check already is.
     *
     * It costs a few tokens on a paid provider, which is the point: a test
     * that does not spend anything has not proved the key works.
     */
    static void testProvider(const std::string& provider,
                             std::function<void(bool ok,
                                                const std::string& detail)> report);

private:
    void onFocusReceived() override;

private:
    LLTextEditor* mTranscript = nullptr;
    LLLineEditor* mInput      = nullptr;
    LLTextBox*    mStatus     = nullptr;
    LLButton*     mSendBtn    = nullptr;

    // <Lumen> Talking instead of typing (lumenaispeech.h). What is said goes
    // into the typing box, after whatever was typed before the mic was
    // clicked. It is sent when the person stops talking (LumenAISpeechAutoSend,
    // the author's preference, 2026-10-01), or waits for Enter with that off.
    // With LumenAISpeechKeepListening as well, one click is a conversation:
    // it listens again after each answer until the mic is clicked off.
    LLButton*   mMicBtn          = nullptr;
    bool        mListening       = false;
    bool        mKeepListening   = false;   // the mic stays on across answers
    F64         mHeardAt         = 0.0;     // last words heard, for ending a silent conversation
    bool        mVoiceMicWasOpen = false;   // voice chat's mic, closed while we listen
    std::string mSpokenPrefix;
    void onMic();
    bool startListening();
    void endListening(const std::string& error);
    void stopMic(const std::string& note);
    void giveVoiceBack();
    void onClose(bool app_quitting) override;
    // </Lumen>

    // Provider-native message history. Anthropic and OpenAI disagree about
    // how a tool call and its result are written down, and translating between
    // them loses things quietly, so the history is kept in whichever shape the
    // current provider speaks. Changing provider therefore starts a new
    // conversation, which the window says out loud rather than pretending the
    // old one carried over.
    LLSD        mMessages;
    std::string mHistoryProvider;

    bool mBusy = false;
    void draw() override;   // <Lumen> lets a finished step stay readable
    /**
     * <Lumen> Which turn is current. A turn records it when it starts and asks
     * again after every wait; Clear moves it on, so a turn still running when
     * the conversation was cleared stops where it is instead of acting, and
     * writing, into the new one.
     */
    S32  mTurnGen = 0;
    /** Stop whatever turn is running, here and in Codex or Claude Code. */
    void abandonTurn();
    /** Tell Codex to stop the turn in flight, if it has one. */
    void interruptCodexTurn();
    /** Codex and Claude Code reach the tools over the endpoint; open it. */
    bool ensureEndpoint(const std::string& provider);
    /// Whether the transcript currently carries a "no key" notice, so it is
    /// said once and withdrawn once rather than repeated or left standing.
    bool mSaidNoKey = false;
    /// Provider and model as last announced, so a change can be noticed.
    std::string mAnnounced;

    // Whether "Lumen:" has already been written this turn. A model often
    // narrates, calls tools, then reports -- two labelled blocks read as two
    // separate replies when they are one answer.

    // Whether the current line is the running list of things being done, so
    // the next one can be added to it instead of starting a new line. Four
    // tools used to mean four lines in a window that is mostly transcript.


    // Tokens this window has spent since it opened. Not persisted: it answers
    // "what is this costing me right now", which is the question someone
    // actually asks, and a lifetime total would need a currency and a price
    // list that both change without telling us.
    S32 mSessionIn  = 0;
    S32 mSessionOut = 0;

    void onSend();
    void beginTurn(const std::string& text);
    void startCatchUp();
    void renderCatchUp(const LLSD& result, const LLSD& summaries);  // <Lumen>
    void flushCatchUp();   // <Lumen> end of turn: draw what was never worded
    void noteCatchUp(const std::string& tool, const LLSD& args,
                     const LLSD& structured, bool is_error);   // <Lumen>
    LLSD mLastCatchUp;          //< what catch_up returned this turn
    bool mCatchUpPending = false;  //< returned, not yet drawn
    bool mCatchUpDrawn   = false;  //< drawn, so the prose is redundant
    static LLUUID sCaughtUpFor;   //< which login has already been summarised
    void onClear();

    // The whole exchange, including however many tool round trips the model
    // needs, run in a coroutine. It must be: a provider call takes seconds,
    // and seconds on the frame loop is a viewer that has stopped drawing.
    void runTurn(const std::string& user_text);

    // One method per kind of line, because they want different treatment:
    // a person's turn, the assistant's prose, the dimmed record of a tool
    // that ran, and a note from the viewer itself.
    void sayUser(const std::string& text);
    void sayAssistant(const std::string& text);
    /** The action bar: what is happening right now, or nothing when idle. */
    void setActivity(const std::string& what);
    // <Lumen> "Thinking..." once a tool has answered -- but not before the
    // step it replaces has been readable. Tools answer in a tenth of a second,
    // and switching at once meant nobody saw what was being done.
    void thinkingAfterStep();
    F64  mActivityAt = 0.0;
    bool mThinkingPending = false;

    /** Names the provider and model once, at the top of a new conversation. */
    void sayHeader();
    void sayNote(const std::string& text);
    void refreshKeyNotice();
    void refreshTitle();
    void runCodexTurn(const std::string& user_text);
    void runClaudeCodeTurn(const std::string& user_text);
    void runVibeTurn(const std::string& user_text);   // <Lumen>
    std::unique_ptr<class LumenAICodex> mCodex;
    std::unique_ptr<class LumenAIClaude> mClaude;
    std::string mClaudeSession;   // Claude Code holds the conversation
    std::string mClaudeModel;     // what that session was started with
    // <Lumen> Mistral Vibe holds its conversation too, by session id, and
    // replays it at the start of every resumed run: what was already read.
    std::unique_ptr<class LumenAIVibe> mVibe;
    std::string mVibeSession;
    std::string mVibeModel;
    std::set<std::string> mVibeSeen;
    // </Lumen>
    std::string mCodexThread;
    std::string mCodexTurn;    // the turn in flight, so it can be interrupted
    std::string mCodexModel;   // what that thread was started with
    U16 mCodexPort = 0;        // <Lumen> the endpoint port that thread was given
    // The memory that thread was started with. Codex reads it once, at
    // thread/start, so a forget or an edit afterwards is invisible to it --
    // and the person is told, rather than the conversation being thrown away.
    std::string mCodexNote;
    std::set<std::string> mCodexEntries;
    std::string mCodexMemoryNoticed;   // the change already told about
    void noticeCodexMemory();
    bool mWarmed = false;      // the local prefix has been sent once

    // **Per CONNECTION, not per turn.** The handshake is once and the request
    // ids must keep climbing; both were being reset at the top of every turn
    // on a socket that was deliberately kept open, so the second question of
    // any session was answered with "Already initialized" and nothing else
    // ever ran. Reset these wherever the socket is (re)opened, and nowhere
    // else.
    bool mCodexReady = false;
    S32  mCodexRpcId = 0;
    // <Lumen> Set while a turn restarts Codex's background service after it
    // lost its folder, so the retry cannot loop.
    bool mCodexRepairing = false;
    std::vector<boost::signals2::connection> mModelConns;

    /** What the turn just cost, and what the window has cost so far. */
    /**
     * The turn's cost, and whether the cache actually did anything.
     *
     * `caching_expected` is true when we asked for it, which is what makes the
     * negative case reportable: without it, caching silently not working looks
     * exactly like caching working and simply not being mentioned.
     */
    void sayUsage(S32 in, S32 out, S32 cached, S32 created, S32 calls,
                  bool caching_expected);
    void setBusy(bool busy, const std::string& note = std::string());
};

#endif // LUMEN_AICHAT_H
