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

#include <string>

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
class FSAIChatFloater : public LLFloater
{
public:
    FSAIChatFloater(const LLSD& key);
    ~FSAIChatFloater() override;

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

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

    // Whether "Lumen:" has already been written this turn. A model often
    // narrates, calls tools, then reports -- two labelled blocks read as two
    // separate replies when they are one answer.
    bool mSpokeThisTurn = false;

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
    void sayTool(const std::string& label, bool failed);
    void sayNote(const std::string& text);

    /** What the turn just cost, and what the window has cost so far. */
    void sayUsage(S32 in, S32 out, S32 calls);
    void setBusy(bool busy, const std::string& note = std::string());
};

#endif // FS_AICHAT_H
