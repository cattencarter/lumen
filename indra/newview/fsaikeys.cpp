/**
 * @file fsaikeys.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsaikeys.h"

#include "llbutton.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "lllineeditor.h"
#include "llsdutil.h"
#include "llsecapi.h"
#include "llviewercontrol.h"
#include "lltextbox.h"
#include "lltrans.h"

namespace
{
    // The data_type under which every provider's key is filed. `mfa_hash` and
    // `credential` are the store's other tenants; this is ours.
    const std::string AI_KEY_STORE = "ai_api_key";

    // The single field inside each stored record. A map rather than a bare
    // string so a provider can later carry more (an org id, a base URL)
    // without moving anyone's saved key.
    const std::string FIELD_KEY = "key";

    std::string trimmed(const std::string& in)
    {
        // People paste keys, and a paste carries whatever the page had around
        // it. A leading space is not a different key; it is the same key that
        // will fail authentication for a reason nobody can see.
        const std::string ws = " \t\r\n";
        const size_t first = in.find_first_not_of(ws);
        if (first == std::string::npos)
        {
            return std::string();
        }
        const size_t last = in.find_last_not_of(ws);
        return in.substr(first, last - first + 1);
    }
}

namespace FSAIKeys
{
    const std::string ANTHROPIC = "anthropic";
    const std::string OPENAI    = "openai";
    /**
     * A model running on this computer, speaking OpenAI's dialect.
     *
     * Its own choice rather than a field under OpenAI, on the author's point:
     * pointing at Ollama is not "using OpenAI", and a box that says *leave this
     * empty to use OpenAI* under a provider named "(API key)" contradicts
     * itself. It needs no key, it has its own address and model, and nothing
     * it sends leaves the machine.
     */
    const std::string LOCAL     = "local";

    const std::vector<std::string>& providers()
    {
        static const std::vector<std::string> p = { ANTHROPIC, OPENAI };
        return p;
    }

    std::string displayName(const std::string& provider)
    {
        if (provider == ANTHROPIC) return "Anthropic";
        if (provider == OPENAI)    return "OpenAI";
        if (provider == LOCAL)     return "the local model";
        return provider;
    }

    std::string get(const std::string& provider)
    {
        if (!gSecAPIHandler)
        {
            return std::string();
        }

        // getProtectedData returns undefined LLSD for anything absent, and
        // this runs before login, so neither the store nor the entry can be
        // assumed to exist.
        const LLSD record = gSecAPIHandler->getProtectedData(AI_KEY_STORE, provider);
        if (!record.isMap() || !record.has(FIELD_KEY))
        {
            return std::string();
        }
        return record[FIELD_KEY].asString();
    }

    bool has(const std::string& provider)
    {
        return !get(provider).empty();
    }

    void set(const std::string& provider, const std::string& key)
    {
        if (!gSecAPIHandler)
        {
            LL_WARNS("FSAIKeys") << "No security handler; cannot save a key." << LL_ENDL;
            return;
        }

        const std::string clean = trimmed(key);
        if (clean.empty())
        {
            return;
        }

        LLSD record = LLSD::emptyMap();
        record[FIELD_KEY] = clean;
        gSecAPIHandler->setProtectedData(AI_KEY_STORE, provider, record);

        // Without this the key lives only in memory and is gone at exit --
        // the destructor deliberately does not write. See the note in the
        // header; it cost a read of llsechandler_basic.cpp to find.
        gSecAPIHandler->syncProtectedMap();

        LL_INFOS("FSAIKeys") << "Saved an API key for " << provider << LL_ENDL;
    }

    void clear(const std::string& provider)
    {
        if (!gSecAPIHandler)
        {
            return;
        }
        gSecAPIHandler->deleteProtectedData(AI_KEY_STORE, provider);
        gSecAPIHandler->syncProtectedMap();

        LL_INFOS("FSAIKeys") << "Removed the API key for " << provider << LL_ENDL;
    }

    std::string hint(const std::string& provider)
    {
        const std::string key = get(provider);
        if (key.empty())
        {
            return std::string();
        }

        // Enough to tell two keys apart, not enough to use. Short keys show
        // only their tail, so a nearly-empty value cannot be read off whole.
        const size_t tail = 4;
        if (key.size() <= tail + 4)
        {
            return "..." + key.substr(key.size() - std::min(tail, key.size()));
        }

        const size_t head = std::min<size_t>(7, key.size() - tail);
        return key.substr(0, head) + "..." + key.substr(key.size() - tail);
    }

    bool looksPlausible(const std::string& provider, const std::string& key)
    {
        const std::string clean = trimmed(key);
        if (clean.empty())
        {
            return false;
        }

        // Advisory. These prefixes are someone else's convention and they have
        // changed before, so a mismatch warns and still saves.
        if (provider == ANTHROPIC)
        {
            return clean.rfind("sk-ant-", 0) == 0;
        }
        if (provider == OPENAI)
        {
            return clean.rfind("sk-", 0) == 0;
        }
        return true;
    }
}

// ---------------------------------------------------------------------------

FSPanelPreferenceAIKeys::FSPanelPreferenceAIKeys()
:   LLPanelPreference()
{
}

bool FSPanelPreferenceAIKeys::postBuild()
{
    // Follow the setting itself. Hanging this on the combo's own commit would
    // miss a change made anywhere else -- the same mistake the Assistant
    // window's title made three times over.
    if (LLControlVariablePtr c = gSavedSettings.getControl("LumenAIProvider"))
    {
        mProviderConn = c->getSignal()->connect(
            boost::bind(&FSPanelPreferenceAIKeys::refresh, this));
    }

    LLPanelPreference::postBuild();

    mRows.clear();
    for (const std::string& provider : FSAIKeys::providers())
    {
        Row row;
        row.provider = provider;
        row.editor   = getChild<LLLineEditor>("key_" + provider);
        row.status   = getChild<LLTextBox>("status_" + provider);

        if (row.editor)
        {
            row.editor->setKeystrokeCallback(
                [this, provider](LLLineEditor*, void*) { onKeyEdited(provider); }, nullptr);
        }

        if (LLButton* clear_btn = findChild<LLButton>("clear_" + provider))
        {
            clear_btn->setCommitCallback(
                [this, provider](LLUICtrl*, const LLSD&) { onClear(provider); });
        }

        mRows.push_back(row);
    }

    syncModelCombo(findChild<LLComboBox>("model_anthropic"), "LumenAIAnthropicModel");
    syncModelCombo(findChild<LLComboBox>("model_openai"),    "LumenAIOpenAIModel");

    if (LLButton* mem = findChild<LLButton>("memory_btn"))
    {
        mem->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("ai_memory");
        });
    }

    refresh();
    return true;
}

void FSPanelPreferenceAIKeys::syncModelCombo(LLComboBox* combo, const std::string& setting)
{
    if (!combo)
    {
        return;
    }

    const std::string value = gSavedSettings.getString(setting);
    if (value.empty())
    {
        return;
    }

    // LLComboBox offers no "does this value exist" and no selectByValue of its
    // own, so ask by doing: setValue selects the matching item when there is
    // one, and changes nothing when there is not.
    combo->setValue(LLSD(value));

    if (combo->getValue().asString() != value)
    {
        // A model the user typed. Offer it in the list so it can be selected,
        // then show it -- otherwise the panel displays something other than
        // the setting it is bound to, which is worse than an ugly label.
        combo->add(value, LLSD(value), ADD_BOTTOM);
        combo->setValue(LLSD(value));
        combo->setLabel(value);
    }
}

void FSPanelPreferenceAIKeys::onOpen(const LLSD& key)
{
    LLPanelPreference::onOpen(key);

    // Reopening the panel must not carry a half-typed key or an unapplied
    // Clear across from last time.
    for (Row& row : mRows)
    {
        row.pending_clear = false;
        if (row.editor)
        {
            row.editor->setText(LLStringUtil::null);
        }
    }

    syncModelCombo(findChild<LLComboBox>("model_anthropic"), "LumenAIAnthropicModel");
    syncModelCombo(findChild<LLComboBox>("model_openai"),    "LumenAIOpenAIModel");

    refresh();
}

/**
 * What Codex's state on this machine actually IS, checked rather than assumed.
 *
 * The author asked for Codex as a choice beside the two API keys. It is a
 * legitimate one -- OpenAI documents embedding Codex in your own product with
 * a ChatGPT login -- but it needs things installed that a key does not, and a
 * dropdown entry that silently does nothing is the failure this project keeps
 * meeting. So the panel looks at the two paths that decide it and says which
 * step is missing.
 *
 * **And it says plainly that the last step is not built yet**, because it is
 * not: the wire format of the app-server socket has not been worked out, so
 * choosing Codex today cannot answer a message. Offering it without that
 * sentence would be a tool claiming what it cannot do.
 */
std::string FSPanelPreferenceAIKeys::codexStatus()
{
    // **`getOSUserDir()` is NOT the home directory.** On macOS it is
    // ~/Library/Application Support/Lumen, so the first version of this check
    // looked for Codex inside the viewer's own data folder and reported "not
    // installed" about something that had just been installed -- with the
    // install command printed underneath, which is the worst kind of wrong:
    // confident, specific, and it would have had the author run an installer
    // twice.
    const char* env_home = getenv("HOME");
    if (!env_home || !*env_home) env_home = getenv("USERPROFILE");   // Windows
    if (!env_home || !*env_home)
    {
        return "Codex: cannot tell, because this system reports no home directory.";
    }
    const std::string home(env_home);
    const std::string sep  = gDirUtilp->getDirDelimiter();
    const std::string cli  = home + sep + ".codex" + sep + "packages" + sep + "standalone"
                           + sep + "current" + sep + "bin" + sep + "codex";
    const std::string sock = home + sep + ".codex" + sep + "app-server-control"
                           + sep + "app-server-control.sock";

    if (!gDirUtilp->fileExists(cli))
    {
        return "Codex is not installed. It is OpenAI's own command-line tool, and it lets "
               "Lumen use your ChatGPT subscription instead of a paid API key. Install it "
               "with:  curl -fsSL https://chatgpt.com/codex/install.sh | sh";
    }
    if (!gDirUtilp->fileExists(sock))
    {
        return "Codex is installed, but its background service is not running. Start it with:  "
               "codex app-server daemon start";
    }
    return "Codex is installed and running. NOT FINISHED: Lumen cannot talk to it yet, so "
           "choosing Codex will not answer anything. Use a key for now.";
}

void FSPanelPreferenceAIKeys::refresh()
{
    // **Show what was chosen and nothing else.** The panel used to show every
    // provider at once, which is why it was crowded enough that a new block
    // printed straight through two paragraphs. The author: *"man kan ikke
    // skifte indhold afhaengigt af hvad der er valgt i Use?"* -- one can, and
    // the three panels sit at the same position so only one is ever on screen.
    const std::string provider = gSavedSettings.getString("LumenAIProvider");
    if (LLPanel* p = findChild<LLPanel>("p_anthropic")) p->setVisible(provider == "anthropic");
    if (LLPanel* p = findChild<LLPanel>("p_openai"))    p->setVisible(provider == "openai");
    if (LLPanel* p = findChild<LLPanel>("p_codex"))     p->setVisible(provider == "codex");
    if (LLPanel* p = findChild<LLPanel>("p_local"))     p->setVisible(provider == "local");

    if (LLTextBox* cs = findChild<LLTextBox>("codex_status"))
    {
        cs->setText(codexStatus());
    }

    for (Row& row : mRows)
    {
        if (!row.status)
        {
            continue;
        }

        const std::string typed = row.editor ? trimmed(row.editor->getText()) : std::string();

        std::string text;
        if (row.pending_clear)
        {
            text = "Will be removed when you click OK.";
        }
        else if (!typed.empty())
        {
            text = FSAIKeys::looksPlausible(row.provider, typed)
                 ? "Will be saved when you click OK."
                 : "Will be saved when you click OK -- but this does not look "
                   "like a " + FSAIKeys::displayName(row.provider) + " key.";
        }
        else if (FSAIKeys::has(row.provider))
        {
            text = "Saved: " + FSAIKeys::hint(row.provider)
                 + ". Leave this empty to keep it.";
        }
        else
        {
            text = "No key saved.";
        }

        row.status->setText(text);
    }
}

void FSPanelPreferenceAIKeys::onKeyEdited(const std::string& provider)
{
    for (Row& row : mRows)
    {
        if (row.provider == provider)
        {
            // Typing is the clearer intention of the two, so it cancels a
            // Clear that has not been committed yet.
            row.pending_clear = false;
            break;
        }
    }
    refresh();
}

void FSPanelPreferenceAIKeys::onClear(const std::string& provider)
{
    for (Row& row : mRows)
    {
        if (row.provider == provider)
        {
            row.pending_clear = true;
            if (row.editor)
            {
                row.editor->setText(LLStringUtil::null);
            }
            break;
        }
    }
    refresh();
}

void FSPanelPreferenceAIKeys::apply()
{
    LLPanelPreference::apply();

    for (Row& row : mRows)
    {
        if (row.pending_clear)
        {
            FSAIKeys::clear(row.provider);
            row.pending_clear = false;
            continue;
        }

        if (!row.editor)
        {
            continue;
        }

        const std::string typed = trimmed(row.editor->getText());
        if (typed.empty())
        {
            // Empty means "leave what is saved alone", never "delete it".
            continue;
        }

        FSAIKeys::set(row.provider, typed);

        // Do not leave the key sitting in a field behind the closed floater.
        row.editor->setText(LLStringUtil::null);
    }

    followTheKey();
    refresh();
}

/**
 * If "Use" points at a provider with no key, and the other one has one, move it.
 *
 * Reported from use: a key was saved for Anthropic while Use sat on OpenAI, and
 * nothing said so. The setup looks complete -- a key is visibly saved, a model
 * is chosen -- and the only symptom arrives later, as "there is no OpenAI key
 * saved yet" from a window the person did not connect to this screen.
 *
 * Deliberately NOT "always select whichever has a key". With keys for both, the
 * choice is the user's and this must not touch it. This only acts when the
 * current selection cannot work and exactly one alternative can, which is a
 * state nobody chooses on purpose.
 */
void FSPanelPreferenceAIKeys::followTheKey()
{
    const std::string chosen = gSavedSettings.getString("LumenAIProvider");
    if (FSAIKeys::has(chosen))
    {
        return;                                   // it can work; leave it alone
    }

    const std::string other = (chosen == FSAIKeys::OPENAI)
                            ? FSAIKeys::ANTHROPIC : FSAIKeys::OPENAI;
    if (!FSAIKeys::has(other))
    {
        return;                                   // neither works; nothing to pick
    }

    // Not logged: the logging macros reach a private member of
    // LLPanelPreference from here. It does not need to be -- refresh() follows,
    // so the change is visible in the control itself rather than only in a file.
    gSavedSettings.setString("LumenAIProvider", other);
}

void FSPanelPreferenceAIKeys::cancel(const std::vector<std::string> settings_to_skip)
{
    LLPanelPreference::cancel(settings_to_skip);

    // Cancel is the undo for both a typed key and a pending Clear, because
    // neither has touched the store yet.
    for (Row& row : mRows)
    {
        row.pending_clear = false;
        if (row.editor)
        {
            row.editor->setText(LLStringUtil::null);
        }
    }
    refresh();
}

static LLPanelInjector<FSPanelPreferenceAIKeys> t_pref_ai_keys("panel_preference_ai");
