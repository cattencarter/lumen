/**
 * @file lumenaikeys.h
 * @brief AI provider API keys, kept in the viewer's protected store.
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
#ifndef LUMEN_AIKEYS_H
#define LUMEN_AIKEYS_H

#include "llpanel.h"
#include "llfloaterpreference.h"
#include "llframetimer.h"

#include <string>
#include <vector>

class LLLineEditor;
class LLComboBox;

/**
 * Where an AI provider's API key lives, and the one place anything should ask
 * for it.
 *
 * NOT `gSavedSettings`. A key here is a bearer credential with a bill attached,
 * and `settings.xml` is plain text that people paste into support threads
 * without thinking. This uses the same protected store that already holds the
 * user's Second Life password: RC4 keyed to a machine id
 * (`llsechandler_basic.cpp`). That is obfuscation rather than strong
 * encryption -- it will not survive someone with the disk and the will -- but
 * it does not travel, it does not decrypt on another machine, and it is not
 * sitting in a file anyone is asked to attach to a bug report. The user is
 * told as much in the panel rather than left to assume better.
 */
namespace LumenAIKeys
{
    // Provider ids. These are also the keys in the protected store, so
    // changing one orphans whatever a user already saved.
    // <Lumen> No assistant. Not a provider that fails, a provider that
    // is honestly absent   so the viewer is an ordinary viewer and says so.
    extern const std::string NONE;
    extern const std::string ANTHROPIC;
    extern const std::string OPENAI;
    extern const std::string LOCAL;
    extern const std::string CODEX;
    extern const std::string CLAUDECODE;

    // Every provider we offer, in the order the panel shows them.
    const std::vector<std::string>& providers();

    // Display name for a provider id ("Anthropic"), for labels and messages.
    std::string displayName(const std::string& provider);

    // The key itself. Empty when none is saved. Callers must not log it.
    std::string get(const std::string& provider);

    bool has(const std::string& provider);

    // Store a key and flush to disk. The flush is the part that is easy to
    // miss: setProtectedData() only touches an in-memory map, and
    // ~LLSecAPIBasicHandler deliberately does NOT write on the way out, so a
    // key set without syncProtectedMap() survives until the viewer closes and
    // is then silently gone.
    void set(const std::string& provider, const std::string& key);

    // Remove a key and flush, with the same caveat.
    void clear(const std::string& provider);

    // A safe thing to show a human: "sk-ant-...4pQ2". Never the whole key.
    // Empty when nothing is saved.
    std::string hint(const std::string& provider);

    // Does this look like a key for this provider? Advisory only -- a wrong
    // answer warns, it never refuses. Key formats belong to someone else and
    // they change.
    bool looksPlausible(const std::string& provider, const std::string& key);
}

/**
 * Preferences > AI.
 *
 * The field is never populated with a stored key -- not even masked. Leaving
 * it empty means "keep what is saved", typing means "replace", and Clear marks
 * the key for removal. Nothing is written until the floater's OK, so Cancel is
 * the undo, which is how every other preference here already behaves. That is
 * also why there is no confirmation dialogue for Clear.
 */
class LumenPanelPreferenceAIKeys : public LLPanelPreference
{
public:
    LumenPanelPreferenceAIKeys();

    bool postBuild() override;
    void apply() override;

    /** Move "Use" off a provider that has no key, when the other one does. */
    void followTheKey();
    void cancel(const std::vector<std::string> settings_to_skip = {}) override;
    void onOpen(const LLSD& key) override;

    // LLPanel declares this virtual; the floater calls it when the panel is
    // shown, which is exactly when the saved-key status needs recomputing.
    void refresh() override;
    /**
     * Notice that a provider became usable while the panel is on screen.
     *
     * <Lumen> The setup window ticked its last step and the panel behind it
     * went on offering "Set it up for me...", because nothing told it to look
     * again; only pressing Test did, which is a step nobody should have to know
     * to take. The author saw it immediately.
     *
     * The cure is the one this project keeps arriving at -- bind the fact to
     * where the fact changes, never to a step somebody might take. Here it
     * changes in two places, our own setup window and a Terminal we do not
     * control, so the only thing covering both is to look. Once a second, and
     * `refresh()` runs only when the answer actually changed, so the transient
     * "Asking..." a test leaves on screen is not wiped by a heartbeat.
     */
    void draw() override;
    // What Codex needs next, and the command that provides it. A sentence
    // alone made the panel a wall of prose with a shell command buried in it;
    // the command is separate so the panel can put it somewhere copyable.
    struct CodexState
    {
        std::string text;
        std::string command;    // empty when there is nothing left to run
        bool        ready = false;
    };
    /**
     * The Test button's answer, as a popup.
     *
     * <Lumen> Public because the asynchronous tests call it back through an
     * LLHandle once the panel may already be gone.
     */
    /** Show the setup button OR the model row for this provider, never both. */
    void setupOrModel(const std::string& who, bool ready);
    void say(bool ok, const std::string& detail);
    /** What the panel shows WHILE a test is in flight. */
    void busy(const std::string& text);

    CodexState codexStatus();
    CodexState claudeStatus();

    // **A button that changes nothing visible is a button that looks broken.**
    // The author pressed Check while Codex was already ready, the status was
    // redrawn identically, and nothing on screen said it had run. Stamping the
    // time is the smallest thing that distinguishes "checked, still fine" from
    // "the button does nothing".
    boost::signals2::connection mProviderConn;

private:
    struct Row
    {
        std::string   provider;
        LLLineEditor* editor  = nullptr;
        LLTextBox*    status  = nullptr;
        bool          pending_clear = false;
    };

    std::vector<Row> mRows;

    void onClear(const std::string& provider);
    void onKeyEdited(const std::string& provider);

    /**
     * Make a model combo show what is actually saved.
     *
     * `LLComboBox::setValue` looks the value up in its list and, when it is
     * not there, quietly changes nothing -- so a model the user typed saves
     * correctly and then displays as blank on the next visit, with the panel
     * disagreeing with the setting it is bound to. Adding the value as an item
     * first is what makes the display honest.
     */
    void syncModelCombo(LLComboBox* combo, const std::string& setting);

    /** See draw(). Seeded by refresh(), so switching provider never trips it. */
    LLFrameTimer mWatch;
    bool         mWasReady = false;
};

#endif // LUMEN_AIKEYS_H
