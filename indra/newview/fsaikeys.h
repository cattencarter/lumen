/**
 * @file fsaikeys.h
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
#ifndef FS_AIKEYS_H
#define FS_AIKEYS_H

#include "llpanel.h"
#include "llfloaterpreference.h"

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
namespace FSAIKeys
{
    // Provider ids. These are also the keys in the protected store, so
    // changing one orphans whatever a user already saved.
    extern const std::string ANTHROPIC;
    extern const std::string OPENAI;
    extern const std::string LOCAL;

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
class FSPanelPreferenceAIKeys : public LLPanelPreference
{
public:
    FSPanelPreferenceAIKeys();

    bool postBuild() override;
    void apply() override;

    /** Move "Use" off a provider that has no key, when the other one does. */
    void followTheKey();
    void cancel(const std::vector<std::string> settings_to_skip = {}) override;
    void onOpen(const LLSD& key) override;

    // LLPanel declares this virtual; the floater calls it when the panel is
    // shown, which is exactly when the saved-key status needs recomputing.
    void refresh() override;
    std::string codexStatus();
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
};

#endif // FS_AIKEYS_H
