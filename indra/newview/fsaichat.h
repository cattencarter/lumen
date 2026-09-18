/**
 * @file fsaichat.h
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
#ifndef FS_AICHAT_H
#define FS_AICHAT_H

#include "llfloater.h"
#include "llsingleton.h"
#include "llsd.h"
#include "lluuid.h"

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
 * request handed to `FSAIControl::handleRequest`, exactly as Claude Desktop's
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
class FSAIAutoResponder : public LLSingleton<FSAIAutoResponder>
{
    LLSINGLETON(FSAIAutoResponder);
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
             const std::vector<std::string>& also_called = std::vector<std::string>());

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
    void replyTo(const LLUUID& from_id, const std::string& from,
                 const LLUUID& session_id, bool speak_aloud,
                 const std::string& latest);

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
    S32                   mRepliesTotal = 0;
    std::set<LLUUID>      mInFlight;
};

class FSAIChatFloater : public LLFloater
{
public:
    FSAIChatFloater(const LLSD& key);
    ~FSAIChatFloater() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;
    void onFocusReceived() override;

private:
    LLTextEditor* mTranscript = nullptr;
    LLLineEditor* mInput      = nullptr;
    LLTextBox*    mStatus     = nullptr;
    LLButton*     mSendBtn    = nullptr;

    // Provider-native message history. Anthropic and OpenAI disagree about
    // how a tool call and its result are written down, and translating between
    // them loses things quietly, so the history is kept in whichever shape the
    // current provider speaks. Changing provider therefore starts a new
    // conversation, which the window says out loud rather than pretending the
    // old one carried over.
    LLSD        mMessages;
    std::string mHistoryProvider;

    bool mBusy = false;
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

    /** Names the provider and model once, at the top of a new conversation. */
    void sayHeader();
    void sayNote(const std::string& text);
    void refreshKeyNotice();
    void refreshTitle();
    void runCodexTurn(const std::string& user_text);
    std::unique_ptr<class FSAICodex> mCodex;
    std::string mCodexThread;
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

#endif // FS_AICHAT_H
