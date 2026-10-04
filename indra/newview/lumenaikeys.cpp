/**
 * @file lumenaikeys.cpp
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

#include "lumenaikeys.h"
#include "llnotificationsutil.h"
#include "lumenaictl.h"
#include "lumenaichat.h"
#include "lumenaiclaude.h"
#include "lumenaicodex.h"
#include "lumenaivibe.h"     // <Lumen>
#include "lumenaispeech.h"   // <Lumen> the mic's languages
#include "llsdjson.h"
#include "llcoros.h"
#include "lleventcoro.h"

#include "llbutton.h"
#include "llcheckboxctrl.h"   // <Lumen> the switches on Permissions
#include "llclipboard.h"
#include "llcombobox.h"
#include "llfloaterreg.h"
#include "llfloaterpreference.h"   // <Lumen> saveIgnoredNotifications
#include "lllineeditor.h"
#include "llsdutil.h"
#include "llsecapi.h"
#include "llviewercontrol.h"
#include "lltextbox.h"
#include <algorithm>   // <Lumen> std::find over the key providers
#include "lltrans.h"
#include "llnotifications.h"
#include "llnotificationtemplate.h"
#include "llradiogroup.h"
#include "llui.h"
#include "lluictrlfactory.h"

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

    /**
     * This session's check value, asked of the endpoint through its own front
     * door, the way the in-viewer Assistant asks everything. It is what a
     * caller that really reached the tools can repeat and one guessing cannot.
     */
    std::string liveSessionCheck()
    {
        const std::string reply = LumenAIControl::instance().handleRequest(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"status\"}");
        try
        {
            const LLSD r = LlsdFromJson(boost::json::parse(reply));
            return r["result"]["session_check"].asString();
        }
        catch (...)
        {
        }
        return std::string();
    }

    std::string lowercased(std::string s)
    {
        LLStringUtil::toLower(s);
        return s;
    }
}

namespace LumenAIKeys
{
    const std::string NONE      = "none";
    const std::string ANTHROPIC = "anthropic";
    const std::string OPENAI    = "openai";
    // <Lumen> Mistral's API speaks OpenAI's dialect at its own address with
    // its own key -- the author's idea, 2026-09-30, "api should be just like
    // the other api choices".
    const std::string MISTRAL   = "mistral";
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
    const std::string CODEX     = "codex";
    const std::string CLAUDECODE = "claudecode";
    const std::string VIBE = "vibe";   // <Lumen>

    const std::vector<std::string>& providers()
    {
        static const std::vector<std::string> p = { ANTHROPIC, OPENAI, MISTRAL };
        return p;
    }

    std::string displayName(const std::string& provider)
    {
        if (provider == NONE)      return "no assistant";
        if (provider == ANTHROPIC) return "Anthropic";
        if (provider == OPENAI)    return "OpenAI";
        if (provider == MISTRAL)   return "Mistral";
        if (provider == LOCAL)     return "the local model";
        if (provider == CODEX)     return "Codex";
        if (provider == CLAUDECODE) return "Claude Code";
        if (provider == VIBE) return "Mistral Vibe";   // <Lumen>
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
            LL_WARNS("LumenAIKeys") << "No security handler; cannot save a key." << LL_ENDL;
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

        LL_INFOS("LumenAIKeys") << "Saved an API key for " << provider << LL_ENDL;
    }

    void clear(const std::string& provider)
    {
        if (!gSecAPIHandler)
        {
            return;
        }
        gSecAPIHandler->deleteProtectedData(AI_KEY_STORE, provider);
        gSecAPIHandler->syncProtectedMap();

        LL_INFOS("LumenAIKeys") << "Removed the API key for " << provider << LL_ENDL;
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

LumenPanelPreferenceAIKeys::LumenPanelPreferenceAIKeys()
:   LLPanelPreference()
{
}

// <Lumen> ---- Preferences > AI > Permissions -------------------------------
//
// Every question the viewer asks before the assistant acts is a LumenAsk
// notification with the viewer's own "Always choose this option". Its state
// is two things the viewer already keeps: whether the question is shown (the
// ignore flag), and, when it is not, which button it answers with (the saved
// "Default<name>" response in the "ignores" settings). Reading and writing
// exactly those is what makes this tab and Preferences > Notifications >
// Alerts agree, and what askUser() sees: a remembered Yes (option 0) means
// allow, any other remembered answer means No.
namespace
{
    const char* const PERM_PREFIX = "LumenAsk";
    const char* const PERM_LEAD   = "When the assistant wants to ";

    LLControlGroup* ignoreSettings()
    {
        auto& groups = LLUI::getInstance()->mSettingGroups;
        auto it = groups.find("ignores");
        return it == groups.end() ? nullptr : it->second;
    }

    std::string permissionState(const std::string& name)
    {
        LLNotificationTemplatePtr t = LLNotifications::instance().getTemplate(name);
        if (!t || !t->mForm || !t->mForm->getIgnored()) return "ask";
        LLControlGroup* ignores = ignoreSettings();
        const std::string key = "Default" + name;
        if (ignores && ignores->controlExists(key))
        {
            const LLSD saved = ignores->getLLSD(key);
            if (saved.has("Yes") && saved["Yes"].asBoolean()) return "allow";
        }
        return "refuse";
    }

    void setPermissionState(const std::string& name, const std::string& state)
    {
        LLNotificationTemplatePtr t = LLNotifications::instance().getTemplate(name);
        if (!t || !t->mForm) return;
        if (state == "ask")
        {
            t->mForm->setIgnored(false);
        }
        else
        {
            LLControlGroup* ignores = ignoreSettings();
            const std::string key = "Default" + name;
            if (ignores && ignores->controlExists(key))
            {
                LLSD response = LLSD::emptyMap();
                response[state == "allow" ? "Yes" : "No"] = true;
                ignores->setLLSD(key, response);
            }
            t->mForm->setIgnored(true);
        }
        LL_INFOS("LumenAI") << "permission " << name << " set to " << state << LL_ENDL;
    }

    // <Lumen> Things the assistant cannot do at all until the person turns
    // them on: the check box on Permissions, the setting it stands for, and
    // the warning shown when it is ticked. A LumenConfirm name, which the
    // assistant may not answer, and the setting is a LumenAI one, which it may
    // not set. Off by default; unticking needs no warning. A new switch is a
    // line here and a check_box in panel_preferences_ai.xml.
    struct OptIn
    {
        const char* checkbox;
        const char* setting;
        const char* warning;
    };
    const OptIn OPT_INS[] =
    {
        { "allow_bulk_inventory", "LumenAIAllowBulkInventory", "LumenConfirmBulkInventory" },
        { "allow_save_scripts",   "LumenAIAllowSaveScripts",   "LumenConfirmSaveScripts" },
    };
}

void LumenPanelPreferenceAIKeys::buildOptIns()
{
    for (const OptIn& o : OPT_INS)
    {
        LLCheckBoxCtrl* box = findChild<LLCheckBoxCtrl>(o.checkbox);
        if (!box) continue;
        const std::string checkbox = o.checkbox;
        const std::string warning  = o.warning;
        box->setCommitCallback([this, checkbox, warning](LLUICtrl* ctrl, const LLSD&)
        {
            // <Lumen> Ticked or not, an earlier "Turn it on" no longer stands.
            mOptInConfirmed.erase(checkbox);
            if (!ctrl->getValue().asBoolean()) return;   // turning it off is always safe
            // Ticked: say what it means. Anything but "Turn it on" -- Leave it
            // off, the close box -- unticks it again. Nothing is written
            // either way until OK, like the questions below it.
            LLHandle<LLPanel> h = getHandle();
            LLNotificationsUtil::add(warning, LLSD(), LLSD(),
                [h, checkbox](const LLSD& notification, const LLSD& response)
            {
                LumenPanelPreferenceAIKeys* p = dynamic_cast<LumenPanelPreferenceAIKeys*>(h.get());
                if (!p) return;
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                {
                    p->mOptInConfirmed.insert(checkbox);   // <Lumen> see saveOptIns
                    return;
                }
                if (LLCheckBoxCtrl* b = p->findChild<LLCheckBoxCtrl>(checkbox)) b->set(false);
            });
        });
    }
}

void LumenPanelPreferenceAIKeys::loadOptIns()
{
    mOptInConfirmed.clear();   // <Lumen> what the boxes show now is the settings
    for (const OptIn& o : OPT_INS)
    {
        if (LLCheckBoxCtrl* box = findChild<LLCheckBoxCtrl>(o.checkbox))
        {
            box->set(gSavedSettings.getBOOL(o.setting));
        }
    }
}

void LumenPanelPreferenceAIKeys::saveOptIns()
{
    // Not bound to the settings on purpose: a bound check box writes the
    // setting the moment it is clicked, so the switch would be on while its
    // warning was still on the screen. And the settings are not in the
    // snapshot Preferences takes on OK and puts back on close, because no
    // control is bound to them, so what is written here stays.
    //
    // <Lumen> A switch is written ON only when its warning was answered "Turn
    // it on" in this visit to the panel. OK pressed while the warning is
    // still up -- by the assistant closing Preferences, which presses OK --
    // would otherwise have saved it on, and "Leave it off" a moment later
    // only unticked a box nobody saved again (the review of 2026-10-04).
    for (const OptIn& o : OPT_INS)
    {
        LLCheckBoxCtrl* box = findChild<LLCheckBoxCtrl>(o.checkbox);
        if (!box) continue;
        const bool want = box->get();
        if (want == gSavedSettings.getBOOL(o.setting)) continue;
        if (want && !mOptInConfirmed.count(o.checkbox))
        {
            LL_INFOS("LumenAI") << "switch " << o.setting << " not turned on: its warning was not "
                                   "answered" << LL_ENDL;
            continue;
        }
        gSavedSettings.setBOOL(o.setting, want);
        LL_INFOS("LumenAI") << "switch " << o.setting << " turned " << (want ? "on" : "off") << LL_ENDL;
    }
}

void LumenPanelPreferenceAIKeys::buildPermissionRows()
{
    LLPanel* list = findChild<LLPanel>("perm_list");
    if (!list) return;

    // Label, then name, so sorting orders the rows as they read.
    std::vector<std::pair<std::string, std::string>> found, fixed;
    for (auto it = LLNotifications::instance().templatesBegin();
         it != LLNotifications::instance().templatesEnd(); ++it)
    {
        const std::string& name = it->first;
        if (name.compare(0, strlen(PERM_PREFIX), PERM_PREFIX) != 0) continue;
        LLNotificationTemplatePtr t = it->second;
        if (!t || !t->mForm) continue;
        // <Lumen> A question that cannot be remembered (no ignore element:
        // deleting in bulk, emptying the Trash) still gets a row, saying it
        // asks every time -- left out, the list would suggest the assistant
        // does those without asking. Its label is the window title
        // without the question mark, since it has no "When the assistant
        // wants to..." text to borrow.
        if (t->mForm->getIgnoreType() == LLNotificationForm::IGNORE_NO)
        {
            std::string label = t->mLabel;
            while (!label.empty() && (label.back() == '?' || label.back() == ' ')) label.pop_back();
            fixed.emplace_back(label.empty() ? name : label, name);
            continue;
        }
        std::string label = t->mForm->getIgnoreMessage();
        if (label.compare(0, strlen(PERM_LEAD), PERM_LEAD) == 0) label = label.substr(strlen(PERM_LEAD));
        if (!label.empty()) label[0] = (char)toupper((unsigned char)label[0]);
        found.emplace_back(label.empty() ? name : label, name);
    }
    std::sort(found.begin(), found.end());
    // The ones that always ask go last, so the choices stay together.
    std::sort(fixed.begin(), fixed.end());
    const size_t settable = found.size();
    found.insert(found.end(), fixed.begin(), fixed.end());

    // Made first, placed after: the list is sized to hold every row, and the
    // scroll container around it (perm_scroll) scrolls when that is taller
    // than the tab. It used to be a fixed panel, and with nineteen questions
    // the last five ran over the note below it and off the bottom.
    std::vector<std::pair<LLPanel*, std::string>> rows;   // name empty: always asks
    S32 total = 4;
    for (size_t i = 0; i < found.size(); ++i)
    {
        const auto& f = found[i];
        LLPanel* row = LLUICtrlFactory::getInstance()->createFromFile<LLPanel>(
            "panel_lumen_permission_row.xml", NULL, LLPanel::child_registry_t::instance());
        if (!row) continue;
        row->getChild<LLTextBox>("question")->setText(f.first);
        row->getChild<LLTextBox>("question")->setToolTip(f.first);
        total += row->getRect().getHeight();
        if (i >= settable)
        {
            // <Lumen> No choices to offer, and nothing for OK to write.
            row->getChild<LLRadioGroup>("choice")->setVisible(false);
            row->getChild<LLTextBox>("always_asks")->setVisible(true);
            rows.emplace_back(row, std::string());
            continue;
        }
        rows.emplace_back(row, f.second);
    }
    const S32 width = list->getRect().getWidth();
    list->reshape(width, total);
    S32 top = total - 2;
    for (auto& r : rows)
    {
        const S32 h = r.first->getRect().getHeight();
        r.first->setRect(LLRect(0, top, width, top - h));
        list->addChild(r.first);
        top -= h;
        if (r.second.empty()) continue;   // <Lumen> always asks: not a PermRow
        mPermRows.push_back({ r.second, r.first->getChild<LLRadioGroup>("choice") });
    }

    if (LLButton* all = findChild<LLButton>("perm_ask_all"))
    {
        all->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            for (PermRow& r : mPermRows)
            {
                if (r.choice) r.choice->setSelectedByValue(LLSD("ask"), true);
            }
        });
    }
}

void LumenPanelPreferenceAIKeys::loadPermissionStates()
{
    for (PermRow& r : mPermRows)
    {
        if (r.choice) r.choice->setSelectedByValue(LLSD(permissionState(r.name)), true);
    }
}

bool LumenPanelPreferenceAIKeys::savePermissionStates()
{
    bool changed = false;
    for (PermRow& r : mPermRows)
    {
        if (!r.choice) continue;
        const std::string want = r.choice->getSelectedValue().asString();
        if (want.empty() || want == permissionState(r.name)) continue;
        setPermissionState(r.name, want);
        changed = true;
    }
    return changed;
}
// </Lumen>

bool LumenPanelPreferenceAIKeys::postBuild()
{
    // Follow the setting itself. Hanging this on the combo's own commit would
    // miss a change made anywhere else -- the same mistake the Assistant
    // window's title made three times over.
    if (LLControlVariablePtr c = gSavedSettings.getControl("LumenAIProvider"))
    {
        mProviderConn = c->getSignal()->connect(
            boost::bind(&LumenPanelPreferenceAIKeys::refresh, this));
    }

    LLPanelPreference::postBuild();
    buildPermissionRows();   // <Lumen> Preferences > AI > Permissions
    buildOptIns();           // <Lumen> and the switches above them

    mRows.clear();
    for (const std::string& provider : LumenAIKeys::providers())
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
    syncModelCombo(findChild<LLComboBox>("model_mistral"),   "LumenAIMistralModel");
    syncModelCombo(findChild<LLComboBox>("model_codex"),     "LumenAICodexModel");
    syncModelCombo(findChild<LLComboBox>("model_claude"),    "LumenAIClaudeCodeModel");
    syncModelCombo(findChild<LLComboBox>("model_vibe"),      "LumenAIVibeModel");   // <Lumen>
    fillSpeechLanguages();   // <Lumen>

    // Copy the command rather than ask somebody to retype a curl line with a
    // pipe in it. Read from the panel, not from codexStatus(), so the button
    // can never copy something different from what is on screen.

    // **Nothing else notices that Codex has been installed.** The status is
    // read when the panel is built, so without this the person follows the
    // instructions, comes back, and is still told to install it -- which reads
    // as the instructions having failed.
    // **It runs a real turn, because the panel says it does.** The first
    // version of this button only stamped the time and redrew, under a line
    // promising it would "try it for real" -- a claim the code did not back,
    // which is the one thing this project will not ship. So it asks Claude Code
    // a question THROUGH the viewer's own endpoint and reports what came back:
    // that proves the CLI runs, that it is signed in, and that the tools are
    // reachable, which is three separate things a user would otherwise
    // discover one failure at a time.

    // <Lumen> One Test button for every provider, beside the list.
    //
    // It replaces three differently named buttons in three panels, and it adds
    // the two that never had one at all   Anthropic and OpenAI, which are
    // exactly where a wrong value is likeliest and stays silent until the
    // Assistant fails later, one layer away from the cause.
    //
    // What "test" MEANS differs, and that is the reason this dispatches rather
    // than doing one thing: Codex and Claude Code are separate PROGRAMS, so
    // theirs is a check of the installation; the other three are services, so
    // theirs is a real request that costs a few tokens. A test that spends
    // nothing has not proved a key works.
    //
    // The answer arrives in a popup. The panel text still carries the detail,
    // but a button whose only effect is a paragraph changing somewhere else
    // reads as a button that did nothing   the author: *"a small confirm popup
    // if all is ok"*.
    if (LLButton* tb = findChild<LLButton>("test_btn"))
    {
        tb->setCommitCallback([this](LLUICtrl*, const LLSD&)
        {
            const std::string provider = gSavedSettings.getString("LumenAIProvider");

            if (provider == LumenAIKeys::CODEX)
            {
                // A filesystem check, so it answers at once and the popup can
                // simply report what refresh() is about to show.
                const CodexState st = codexStatus();
                refresh();
                // <Lumen> Rarely seen: opening this panel has usually started it already.
                if (st.starting)
                {
                    say(true, "Codex's background service was not running, so Lumen has "
                              "started it. Lumen does this by itself whenever it is needed.");
                    return;
                }
                // </Lumen>
                say(st.ready, st.ready ? "Codex is installed, signed in and running."
                                       : st.text);
                return;
            }

            if (provider == LumenAIKeys::CLAUDECODE)
            {
                const std::string here = LumenAIClaude::unavailableHere();
                if (!here.empty())
                {
                    refresh();
                    say(false, here);
                    return;
                }
                if (!LumenAIClaude::installed())
                {
                    refresh();
                    say(false, "Claude Code is not installed on this computer.");
                    return;
                }
                // <Lumen> Installed but predating --restricted: the turn would
                // stop on its own flags, and the setup button is the way out.
                {
                    std::string version;
                    if (LumenAIClaude::tooOld(&version))
                    {
                        refresh();
                        say(false, LumenAIClaude::tooOldText(version));
                        return;
                    }
                }
                // </Lumen>

                // **Open the viewer's own connection first.** It listens only
                // while Codex or Claude Code is the chosen provider, and only
                // once something starts it -- at launch, or before an Assistant
                // turn. Switching to Claude Code here and pressing Test started
                // nothing, so Claude Code was pointed at port 0, reached no
                // tools, said so in plain words, and that sentence was shown as
                // this session's check value under "It works".
                LumenAIControl& ctl = LumenAIControl::instance();
                if (!ctl.isRunning() && !ctl.start())
                {
                    say(false, "The viewer could not open its own local connection, so Claude "
                               "Code would have had no tools to reach. Nothing was asked.");
                    return;
                }
                busy("Asking Claude Code... this takes a few seconds.");

                const std::string model = gSavedSettings.getString("LumenAIClaudeCodeModel");
                // The LIVE port: it is picked at random each start.
                const U16 port = ctl.port();
                // And the value to compare against. Without it any six
                // characters passed, which is the one thing it exists to stop.
                const std::string expect = liveSessionCheck();
                LLHandle<LLPanel> h = getHandle();
                LLCoros::instance().launch("LumenAIClaudeTest", [h, model, port, expect]()
                {
                    LumenAIClaude cc;
                    LLSD result;
                    std::string why, said;
                    bool ran = false;

                    if (!cc.runToResult("Call the second_life viewer tool with action=status and "
                                        "reply with ONLY the session_check value, nothing else.",
                                        model, port, 120.0, result, why))
                    {
                        said = why;
                    }
                    else
                    {
                        said = result["result"].asString();
                        ran  = !result["is_error"].asBoolean();
                        if (said.empty()) said = "Claude Code answered with nothing.";
                    }

                    LumenPanelPreferenceAIKeys* p =
                        dynamic_cast<LumenPanelPreferenceAIKeys*>(h.get());
                    if (!p) return;
                    if (said.size() > 220) said = said.substr(0, 220);

                    const bool reached = ran && !expect.empty()
                        && lowercased(said).find(lowercased(expect)) != std::string::npos;
                    if (reached)
                    {
                        p->say(true, "Claude Code ran, is signed in, and reached the viewer's own "
                                     "tools: it answered with this session's check value, "
                                     + expect + ".");
                    }
                    else if (ran)
                    {
                        p->say(false, expect.empty()
                            ? "Claude Code ran and is signed in, but the viewer could not read "
                              "its own check value, so whether it reached the tools is not "
                              "known. It said: " + said
                            : "Claude Code ran and is signed in, but did not reach the viewer's "
                              "own tools: its answer is not this session's check value. It "
                              "said: " + said);
                    }
                    else
                    {
#if LL_WINDOWS
                        p->say(false, said + "\n\nIf it says not logged in, press Set it up for "
                                             "me... again to sign in.");
#else
                        p->say(false, said + "\n\nIf it says not logged in, run  claude auth "
                                             "login  in Terminal once.");
#endif
                    }
                    p->refresh();
                });
                return;
            }

            // <Lumen> Mistral Vibe: the same proof as Claude Code -- a real turn
            // that has to fetch this session's check value through the viewer's
            // own tools, so "it works" means signed in AND reaching them.
            if (provider == LumenAIKeys::VIBE)
            {
                const std::string here = LumenAIVibe::unavailableHere();
                if (!here.empty()) { refresh(); say(false, here); return; }
                if (!LumenAIVibe::installed())
                {
                    refresh();
                    say(false, "Mistral Vibe is not installed on this computer.");
                    return;
                }
                if (!LumenAIVibe::signedIn())
                {
                    refresh();
                    say(false, "Mistral Vibe is installed but not signed in to a Mistral account.");
                    return;
                }
                LumenAIControl& ctl = LumenAIControl::instance();
                if (!ctl.isRunning() && !ctl.start())
                {
                    say(false, "The viewer could not open its own local connection, so Mistral "
                               "Vibe would have had no tools to reach. Nothing was asked.");
                    return;
                }
                busy("Asking Mistral Vibe... this takes a few seconds.");
                const std::string model  = gSavedSettings.getString("LumenAIVibeModel");
                const U16         port   = ctl.port();
                const std::string expect = liveSessionCheck();
                LLHandle<LLPanel> h = getHandle();
                LLCoros::instance().launch("LumenAIVibeTest", [h, model, port, expect]()
                {
                    LumenAIVibe vibe;
                    std::string said, why;
                    const bool ran = vibe.runToAnswer(
                        "Call the second_life viewer tool with action=status and reply with ONLY "
                        "the session_check value, nothing else.", model, port, 120.0, said, why);
                    LumenPanelPreferenceAIKeys* p =
                        dynamic_cast<LumenPanelPreferenceAIKeys*>(h.get());
                    if (!p) return;
                    if (!ran) said = why;
                    if (said.size() > 220) said = said.substr(0, 220);
                    const bool reached = ran && !expect.empty()
                        && lowercased(said).find(lowercased(expect)) != std::string::npos;
                    if (reached)
                    {
                        p->say(true, "Mistral Vibe ran, is signed in, and reached the viewer's own "
                                     "tools: it answered with this session's check value, "
                                     + expect + ".");
                    }
                    else if (ran)
                    {
                        p->say(false, "Mistral Vibe ran and is signed in, but did not reach the "
                                      "viewer's own tools: its answer is not this session's check "
                                      "value. It said: " + said);
                    }
                    else
                    {
                        p->say(false, said);
                    }
                    p->refresh();
                });
                return;
            }
            // </Lumen>

            // Anthropic, OpenAI, a local model: one real request.
            busy("Asking " + LumenAIKeys::displayName(provider) + "...");
            LLHandle<LLPanel> h = getHandle();
            LumenAIChatFloater::testProvider(provider,
                [h, provider](bool ok, const std::string& detail)
            {
                LumenPanelPreferenceAIKeys* p =
                    dynamic_cast<LumenPanelPreferenceAIKeys*>(h.get());
                if (!p) return;
                p->say(ok, ok ? LumenAIKeys::displayName(provider) + " answered."
                              : detail);
                p->refresh();
            });
        });
    }

    // <Lumen> Opens the guided window. Everything it does could be done
    // by hand from the website, and for a lot of people that is the harder path
    // rather than the safer one.
    if (LLButton* sb = findChild<LLButton>("codex_setup"))
    {
        sb->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("ai_setup",
                LLSD().with("provider", LumenAIKeys::CODEX));
        });
    }

    if (LLButton* sb = findChild<LLButton>("claude_setup"))
    {
        sb->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("ai_setup",
                LLSD().with("provider", LumenAIKeys::CLAUDECODE));
        });
    }
    // <Lumen>
    if (LLButton* sb = findChild<LLButton>("vibe_setup"))
    {
        sb->setCommitCallback([](LLUICtrl*, const LLSD&)
        {
            LLFloaterReg::showInstance("ai_setup",
                LLSD().with("provider", LumenAIKeys::VIBE));
        });
    }
    // </Lumen>


    // **The local panel could not tell you whether it worked.** Two typed
    // fields, no feedback, and the first sign of a wrong address or a wrong
    // model name was the Assistant failing later -- one layer away from the
    // cause, which is this project's favourite way to lose an afternoon.
    //
    // The author's idea, and it does two jobs with one request: *"den kan jo
    // bare starte naar man vaelger model som en test?"* It answers "is this
    // right", AND it leaves the server's prompt cache warm, so the ~20 seconds
    // of prompt processing is spent here instead of on their first question.

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

void LumenPanelPreferenceAIKeys::syncModelCombo(LLComboBox* combo, const std::string& setting)
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

void LumenPanelPreferenceAIKeys::onOpen(const LLSD& key)
{
    LLPanelPreference::onOpen(key);
    loadPermissionStates();   // <Lumen> what is remembered NOW, not at last open
    loadOptIns();             // <Lumen>

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
    syncModelCombo(findChild<LLComboBox>("model_mistral"),   "LumenAIMistralModel");
    syncModelCombo(findChild<LLComboBox>("model_codex"),     "LumenAICodexModel");
    syncModelCombo(findChild<LLComboBox>("model_claude"),    "LumenAIClaudeCodeModel");
    syncModelCombo(findChild<LLComboBox>("model_vibe"),      "LumenAIVibeModel");   // <Lumen>
    fillSpeechLanguages();   // <Lumen>

    refresh();
}

// <Lumen> The languages the mic can listen in, from the computer itself (the
// author, 2026-10-02: "what if a german user wants to speak in english").
// First the computer's own, which is what an empty setting means; then every
// language this Mac or Windows offers. A saved language this computer does not
// offer -- set on another machine -- is kept and shown, not silently replaced.
void LumenPanelPreferenceAIKeys::fillSpeechLanguages()
{
    LLComboBox* combo = findChild<LLComboBox>("speech_language");
    if (!combo) return;

#if LL_WINDOWS
    // Whisper hears English only, so there is no language to choose; the row
    // says instead whether its model is here, and fetches or removes it.
    if (LumenAISpeech::supported())
    {
        combo->setVisible(false);
        if (LLUICtrl* label = findChild<LLUICtrl>("speech_language_label")) label->setVisible(false);
        if (LLButton* btn = findChild<LLButton>("speech_setup_btn"))
        {
            btn->setVisible(true);
            btn->setCommitCallback([this](LLUICtrl*, const LLSD&)
            {
                mSpeechSetupWhy.clear();
                const LumenAISpeech::Setup state = LumenAISpeech::setupState().state;
                if (state == LumenAISpeech::Setup::Downloading)
                {
                    LumenAISpeech::cancelSetup();
                }
                else if (state == LumenAISpeech::Setup::Ready)
                {
                    LumenAISpeech::removeSetup();
                }
                else
                {
                    std::string why;
                    if (!LumenAISpeech::startSetup(why)) mSpeechSetupWhy = why;
                }
                refreshSpeechSetup();
            });
        }
        if (LLUICtrl* text = findChild<LLUICtrl>("speech_setup_text")) text->setVisible(true);
        refreshSpeechSetup();
        return;
    }
#endif

    const auto offered = LumenAISpeech::languages();
    if (offered.empty())
    {
        combo->setEnabled(false);   // no speech engine here
        return;
    }
    combo->removeall();
    combo->add("Same as the computer", LLSD(std::string()), ADD_BOTTOM);
    for (const auto& lang : offered)
    {
        combo->add(lang.second, LLSD(lang.first), ADD_BOTTOM);
    }

    const std::string value = gSavedSettings.getString("LumenAISpeechLanguage");
    if (value.empty())
    {
        combo->selectFirstItem();
        return;
    }
    combo->setValue(LLSD(value));
    if (combo->getValue().asString() != value)
    {
        combo->add(value, LLSD(value), ADD_BOTTOM);
        combo->setValue(LLSD(value));
    }
}

// <Lumen> The Windows row: what is there, and the one button that changes it.
// Called once a second by draw() while the panel is on screen, so a download
// started from the mic shows its progress here too.
void LumenPanelPreferenceAIKeys::refreshSpeechSetup()
{
#if LL_WINDOWS
    LLUICtrl* text = findChild<LLUICtrl>("speech_setup_text");
    LLButton* btn = findChild<LLButton>("speech_setup_btn");
    if (!text || !btn || !text->getVisible()) return;

    const LumenAISpeech::SetupState st = LumenAISpeech::setupState();
    auto mb = [](long long bytes) { return std::to_string((bytes + 500000) / 1000000) + " MB"; };
    std::string line, label;
    switch (st.state)
    {
    case LumenAISpeech::Setup::Ready:
        line = "Speech recognition: installed. Whisper runs on this computer and understands English.";
        label = "Remove";
        break;
    case LumenAISpeech::Setup::Downloading:
        line = "Speech recognition: downloading, " + mb(st.done) + " of " + mb(st.total) + ".";
        label = "Stop";
        break;
    case LumenAISpeech::Setup::Failed:
        line = "Speech recognition: the download did not finish -- " + st.error;
        label = "Try again";
        break;
    default:
        line = "Speech recognition: not installed. Whisper runs on this computer; it is a one-time "
               "download of " + LumenAISpeech::setupSize() + " from Hugging Face.";
        label = "Download";
        break;
    }
    if (!mSpeechSetupWhy.empty()) line = mSpeechSetupWhy;
    if (line + label != mSpeechSetupShown)
    {
        mSpeechSetupShown = line + label;
        text->setValue(LLSD(line));
        text->setToolTip(line);
        btn->setLabel(label);
    }
#endif
}
// </Lumen>

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
/**
 * What Codex is doing, and the one command that moves it forward.
 *
 * The author asked for something friendlier than a paragraph: *"hvis man
 * vaelger det og det ikke er installeret, kan der saa komme en guide til hvad
 * man skal gore og hvad det kraever?"* -- so this returns a COMMAND as well as
 * a sentence, and the panel puts that command somewhere it can be selected and
 * copied rather than retyped from a wall of prose.
 *
 * **Four states, not three.** Being signed in is a separate condition from
 * being installed, and it was missing: a fresh install has the binary and the
 * daemon and no ChatGPT account behind it, so the panel would have said Lumen
 * could talk to Codex and the first message would have failed with an
 * authentication error nowhere near the thing that was wrong.
 *
 * Each command was checked against `codex --help` on this machine rather than
 * remembered -- `login`, `app-server daemon`, and `doctor` are all real
 * subcommands. A confidently wrong command is worse than none, because the
 * person cannot tell our mistake from their own.
 */
/**
 * Show the setup button OR the model row, never both.
 *
 * They sit at the same position, so the swap reads as one thing becoming
 * another rather than as a panel rearranging itself.
 */
void LumenPanelPreferenceAIKeys::setupOrModel(const std::string& who, bool ready)
{
    if (LLButton* b = findChild<LLButton>(who + "_setup")) b->setVisible(!ready);

    const char* rest[] = { "_lbl_model", "_lbl_effort", "_models_note" };
    for (const char* suffix : rest)
    {
        if (LLView* v = findChild<LLView>(who + suffix)) v->setVisible(ready);
    }
    // The combo's own name is NOT its control_name -- `model_codex` bound to
    // LumenAICodexModel. Reaching for the setting name found nothing, silently,
    // and the row stayed on screen while the label beside it vanished.
    if (LLView* v = findChild<LLView>("model_" + who))   // <Lumen> model_codex, model_claude, model_vibe
    {
        v->setVisible(ready);
    }
    if (who == "codex")
    {
        if (LLView* v = findChild<LLView>("effort_codex")) v->setVisible(ready);
    }
}

void LumenPanelPreferenceAIKeys::say(bool ok, const std::string& detail)
{
    // "It works" is the whole message on success. The detail is for the case
    // that needs acting on, and a paragraph of reassurance nobody reads is how
    // a popup becomes something people dismiss without looking.
    LLSD args;
    args["MESSAGE"] = ok ? "It works." + (detail.empty() ? std::string()
                                                         : "\n\n" + detail)
                         : "It did not work.\n\n" + detail;
    LLNotificationsUtil::add("GenericAlertOK", args);
}

void LumenPanelPreferenceAIKeys::busy(const std::string& text)
{
    // Every provider panel has its own status line, so the waiting message goes
    // to whichever one is on screen rather than to a name chosen here.
    const std::string provider = gSavedSettings.getString("LumenAIProvider");
    const char* which = (provider == LumenAIKeys::CODEX)      ? "codex_status"
                      : (provider == LumenAIKeys::CLAUDECODE) ? "claude_status"
                      : (provider == LumenAIKeys::VIBE)       ? "vibe_status"   // <Lumen>
                      : (provider == LumenAIKeys::LOCAL)      ? "local_status"
                      : NULL;
    if (which)
    {
        if (LLTextBox* t = findChild<LLTextBox>(which)) t->setText(text);
    }
}

LumenPanelPreferenceAIKeys::CodexState LumenPanelPreferenceAIKeys::codexStatus(bool probe)
{
    CodexState st;

    // Say that it cannot work here at all, rather than "not set up yet" about
    // a program that may well be installed.
    const std::string here = LumenAICodex::unavailableHere();
    if (!here.empty())
    {
        st.text = here;
        return st;
    }

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
        st.text = "Codex: cannot tell, because this system reports no home directory.";
        return st;
    }
    const std::string home(env_home);
    const std::string sep  = gDirUtilp->getDirDelimiter();
    const std::string dot  = home + sep + ".codex" + sep;
    const std::string cli  = LumenAICodex::cliPath();   // <Lumen> codex.exe on Windows
    const std::string auth = dot + "auth.json";
    const std::string sock = dot + "app-server-control" + sep + "app-server-control.sock";

    if (!gDirUtilp->fileExists(cli))
    {
        st.text = "Not set up yet. Codex is OpenAI's own small helper program, and it is "
                  "what lets Lumen use the ChatGPT subscription you already pay for instead "
                  "of a paid API key.";
#if LL_WINDOWS
        st.command = "irm https://chatgpt.com/codex/install.ps1 | iex";   // <Lumen> in PowerShell
#else
        st.command = "curl -fsSL https://chatgpt.com/codex/install.sh | sh";
#endif
        return st;
    }
    if (!gDirUtilp->fileExists(auth))
    {
        st.text = "Almost. Codex is installed but not signed in to your ChatGPT "
                  "account yet.";
#if LL_WINDOWS
        st.command = "& \"" + cli + "\" login";   // <Lumen> not on PATH, in PowerShell
#else
        st.command = "codex login";
#endif
        return st;
    }
    // <Lumen> Not running is what every restart of the computer leaves, so
    // Lumen starts it rather than handing the person the setup window again.
    // Only when that did not work is it theirs to see.
    const bool sock_there = LumenAICodex::socketPresent();   // <Lumen> Lumen's own server on Windows
    (void)sock;
    if ((!sock_there || (probe && !LumenAICodex::listening()))
        && LumenAICodex::startService())
    {
        st.starting = true;
        st.text = "Starting Codex's background service...";
        return st;
    }
    // </Lumen>
    if (!sock_there)
    {
        st.text = "Almost. Codex is installed and signed in, but it is not running "
                  "yet.";
#if LL_WINDOWS
        // <Lumen> On Windows Lumen runs Codex's server itself; there is no
        // service of Codex's own to start by hand.
        st.command = "";
        st.text += " Lumen could not start it -- quitting and reopening Lumen tries again.";
#else
        st.command = "codex app-server daemon start";
#endif
        return st;
    }
    // **The file is not the service.** A socket file outlives the process that
    // made it -- a crash, a force-quit, a restart -- and this used to call that
    // "running" while every Assistant turn found nobody there.
    if (probe && !LumenAICodex::listening())
    {
        st.text = "Almost. Codex is installed and signed in, but its background service "
                  "is not answering -- it may have stopped without tidying up.";
#if LL_WINDOWS
        st.command = "";
#else
        st.command = "codex app-server daemon start";
#endif
        return st;
    }

    st.ready = true;
    st.text  = "Ready. Codex is installed, signed in and running, and Lumen can reach it -- "
               "including the viewer's own tools, over the same endpoint any other assistant "
               "uses. Your ChatGPT account pays for this, so there is no API key and no "
               "separate bill; your plan's limits apply instead.\n"
#if LL_WINDOWS
               "If the Assistant will not answer, `codex doctor` in PowerShell checks the "
#else
               "If the Assistant will not answer, `codex doctor` in Terminal checks the "
#endif
               "whole installation.";
    return st;
}

/**
 * What Claude Code needs next, and the command that provides it.
 *
 * Two states rather than Codex's four, because Claude Code needs no background
 * service: it is a command we run per turn. What it does need is to be
 * installed and to be signed in, and **only the second of those can be read
 * from a file**, so being logged in is left to the Test button rather than
 * guessed at here. A panel that says "signed in" because a file exists is the
 * kind of confident wrong answer this project keeps writing down.
 */
LumenPanelPreferenceAIKeys::CodexState LumenPanelPreferenceAIKeys::claudeStatus()
{
    CodexState st;

    if (!LumenAIClaude::installed())
    {
        // No prose: nothing reads it. The button appearing is the whole message,
        // and the Test popup writes its own.
        return st;
    }
    // <Lumen> An installed copy too old to start is not set up, and the setup
    // window's first step updates it. Being installed was the whole test here,
    // so the panel called 2.1.220 ready while every turn failed on its flags.
    // Cheap to ask every second: the answer is kept until the file changes.
    if (LumenAIClaude::tooOld())
    {
        return st;
    }
    // </Lumen>

    st.ready = true;

    return st;
}

// <Lumen> Mistral Vibe: installed and signed in. Nothing runs in the
// background, so that is all "set up" means.
LumenPanelPreferenceAIKeys::CodexState LumenPanelPreferenceAIKeys::vibeStatus()
{
    CodexState st;
    st.ready = LumenAIVibe::unavailableHere().empty() && LumenAIVibe::installed()
            && LumenAIVibe::signedIn();
    return st;
}
// </Lumen>

// <Lumen> A heartbeat, and the narrowest one that works. See the header.
//
// Three stat calls a second, and only while this panel is the one on screen.
// The comparison is what keeps it honest: refresh() clears the status line, so
// calling it every tick would wipe the "Asking..." a test in flight leaves
// there. It runs when the answer CHANGED, which is the only moment there is
// anything new to draw.
void LumenPanelPreferenceAIKeys::draw()
{
    if (mWatch.getElapsedTimeF32() > 1.f)
    {
        mWatch.reset();
        refreshSpeechSetup();   // <Lumen> a download's progress, on Windows
        const std::string provider = gSavedSettings.getString("LumenAIProvider");
        if (provider == LumenAIKeys::CODEX || provider == LumenAIKeys::CLAUDECODE
            || provider == LumenAIKeys::VIBE)   // <Lumen>
        {
            // Codex: files every second, and the socket knocked on only while
            // the files say ready and the panel does not -- a stale socket
            // waiting for its service to come back. A healthy service is never
            // connected to from here, and a dead one refuses at once.
            bool ready = (provider == LumenAIKeys::CODEX) ? codexStatus(false).ready
                       : (provider == LumenAIKeys::VIBE)  ? vibeStatus().ready   // <Lumen>
                                                       : claudeStatus().ready;
            if (provider == LumenAIKeys::CODEX && ready && !mWasReady)
            {
                ready = codexStatus(true).ready;
            }
            if (ready != mWasReady)
            {
                refresh();      // which re-seeds mWasReady
            }
        }
    }
    LLPanelPreference::draw();
}

void LumenPanelPreferenceAIKeys::refresh()
{
    // **Show what was chosen and nothing else.** The panel used to show every
    // provider at once, which is why it was crowded enough that a new block
    // printed straight through two paragraphs. The author: *"man kan ikke
    // skifte indhold afhaengigt af hvad der er valgt i Use?"* -- one can, and
    // the three panels sit at the same position so only one is ever on screen.
    const std::string provider = gSavedSettings.getString("LumenAIProvider");
    // Nothing to test when nothing is chosen.
    if (LLButton* tb = findChild<LLButton>("test_btn")) tb->setVisible(provider != "none");
    if (LLPanel* p = findChild<LLPanel>("p_none"))      p->setVisible(provider == "none");
    if (LLPanel* p = findChild<LLPanel>("p_anthropic")) p->setVisible(provider == "anthropic");
    if (LLPanel* p = findChild<LLPanel>("p_openai"))    p->setVisible(provider == "openai");
    if (LLPanel* p = findChild<LLPanel>("p_mistral"))   p->setVisible(provider == "mistral");
    if (LLPanel* p = findChild<LLPanel>("p_codex"))     p->setVisible(provider == "codex");
    if (LLPanel* p = findChild<LLPanel>("p_claudecode")) p->setVisible(provider == "claudecode");
    if (LLPanel* p = findChild<LLPanel>("p_vibe"))      p->setVisible(provider == "vibe");   // <Lumen>
    if (LLPanel* p = findChild<LLPanel>("p_local"))     p->setVisible(provider == "local");

    const CodexState codex = codexStatus();
    // <Lumen> No standing status line. The author: *"when it's all set up we
    // just remove the button. no need to tell it's not set up"* -- and he is
    // right: the button being there IS the message, and a paragraph repeating
    // it in words is one more thing to read before doing the only thing on
    // offer. The box below is left for the transient "Asking..." while a test
    // is in flight, because a button that goes quiet for twenty seconds looks
    // broken.
    // Except where it cannot work at all: then that is the one thing to say.
    if (LLTextBox* cs = findChild<LLTextBox>("codex_status"))
        cs->setText(LumenAICodex::unavailableHere());
    // <Lumen> No command, and no Copy button. The author, looking at the
    // panel: *"this can all go, including the copy button and then we just
    // place 'set it up for me' at the top. we can put the manual steps on the
    // webpage."*
    //
    // Which is right, and the shell command was the last thing here written for
    // a reader who already knows what a command line is. The window does it;
    // the website carries the by-hand version for anybody who would rather see
    // what runs. A panel that offers both is a panel that asks somebody to
    // choose between two things they cannot tell apart.

    // <Lumen> The button and the model row occupy the same place, and only
    // one of them is ever there. The author: *"don't show the part about the
    // model and these are claude code's own aliases etc until the set up is
    // complete, then replace the set it up for me button with the drop down
    // and text."*
    //
    // Which is the right order: choosing a model is a question for somebody who
    // HAS one, and putting it in front of somebody who has not installed the
    // program yet is asking them to decide something they cannot act on. One
    // thing at a time, and the thing they can do is the thing on screen.
    setupOrModel("codex", codex.ready || codex.starting);   // <Lumen> no button flashing up for a second

    const CodexState claude = claudeStatus();
    setupOrModel("claude", claude.ready);
    // <Lumen>
    const CodexState vibe = vibeStatus();
    setupOrModel("vibe", vibe.ready);
    if (LLTextBox* vs = findChild<LLTextBox>("vibe_status"))
        vs->setText(LumenAIVibe::unavailableHere());
    // </Lumen>

    // Seeded here rather than in draw(), so that switching provider -- which
    // calls refresh() -- cannot look like a provider that just became ready.
    mWasReady = (provider == LumenAIKeys::CODEX)      ? codex.ready
              : (provider == LumenAIKeys::CLAUDECODE) ? claude.ready
              : (provider == LumenAIKeys::VIBE)       ? vibe.ready   // <Lumen>
              : false;
    if (LLTextBox* cs = findChild<LLTextBox>("claude_status"))
    {
        // <Lumen> The button is the whole message for somebody who has not set
        // it up. It is NOT for somebody who has, and whose copy has gone out of
        // date: they did this once, and the button coming back needs a reason
        // and a way out they can take themselves.
        std::string text = LumenAIClaude::unavailableHere();
        std::string version;
        if (text.empty() && LumenAIClaude::installed() && LumenAIClaude::tooOld(&version))
        {
            text = LumenAIClaude::tooOldText(version, true);
        }
        cs->setText(text);
        // </Lumen>
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
            text = LumenAIKeys::looksPlausible(row.provider, typed)
                 ? "Will be saved when you click OK."
                 : "Will be saved when you click OK -- but this does not look "
                   "like a " + LumenAIKeys::displayName(row.provider) + " key.";
        }
        else if (LumenAIKeys::has(row.provider))
        {
            text = "Saved: " + LumenAIKeys::hint(row.provider)
                 + ". Leave this empty to keep it.";
        }
        else
        {
            text = "No key saved.";
        }

        row.status->setText(text);
    }
}

void LumenPanelPreferenceAIKeys::onKeyEdited(const std::string& provider)
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

void LumenPanelPreferenceAIKeys::onClear(const std::string& provider)
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

void LumenPanelPreferenceAIKeys::apply()
{
    LLPanelPreference::apply();
    // <Lumen> Only what the user changed -- and then the snapshot Preferences
    // is about to put back. OK takes a copy of every remembered answer BEFORE
    // apply(), and closing the window runs cancel(), which restores that copy:
    // so "Always allow" was saved and undone in the same click, and the next
    // build asked anyway (found testing, 2026-09-28). Taking the copy again
    // here makes what it restores the choice just made.
    if (savePermissionStates())
    {
        if (LLFloaterPreference* prefs =
                LLFloaterReg::findTypedInstance<LLFloaterPreference>("preferences"))
        {
            prefs->saveIgnoredNotifications();
        }
    }
    saveOptIns();   // <Lumen> see there for why this is not undone by the close

    for (Row& row : mRows)
    {
        if (row.pending_clear)
        {
            LumenAIKeys::clear(row.provider);
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

        LumenAIKeys::set(row.provider, typed);

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
void LumenPanelPreferenceAIKeys::followTheKey()
{
    const std::string chosen = gSavedSettings.getString("LumenAIProvider");
    if (LumenAIKeys::has(chosen))
    {
        return;                                   // it can work; leave it alone
    }
    // <Lumen> "Has a key" is the wrong question for the providers that never
    // take one. Codex and Claude Code are the person's own subscription, and a
    // local model has an address: none of them ever "has a key", so choosing
    // one and pressing OK in Preferences silently switched to OpenAI whenever
    // an OpenAI key was saved -- the author: "no idea why it changed away from
    // codex". The same mistake Decisions 144 fixed for the away-responder.
    // This only ever means to rescue a KEY provider that has no key.
    //
    // **None is a choice too, and the most deliberate one on this panel**: it
    // is how somebody switches the assistant off. Rescuing it turned None back
    // into OpenAI on every OK whenever an OpenAI key was saved, so the next
    // thing typed was sent and billed although the panel said the assistant
    // was off. An empty setting reads as None everywhere else, so it stays.
    const std::vector<std::string>& keyed = LumenAIKeys::providers();
    if (std::find(keyed.begin(), keyed.end(), chosen) == keyed.end())
    {
        return;
    }

    // <Lumen> With a third key provider, "the other one" is whichever SINGLE
    // other one has a key -- two with keys is a choice that is the user's.
    std::string other;
    for (const std::string& p : keyed)
    {
        if (p == chosen || !LumenAIKeys::has(p)) continue;
        if (!other.empty()) return;               // more than one works; theirs to pick
        other = p;
    }
    if (other.empty())
    {
        return;                                   // none works; nothing to pick
    }

    // Not logged: the logging macros reach a private member of
    // LLPanelPreference from here. It does not need to be -- refresh() follows,
    // so the change is visible in the control itself rather than only in a file.
    gSavedSettings.setString("LumenAIProvider", other);
}

void LumenPanelPreferenceAIKeys::cancel(const std::vector<std::string> settings_to_skip)
{
    LLPanelPreference::cancel(settings_to_skip);
    loadPermissionStates();   // <Lumen> nothing was written; show what is stored
    loadOptIns();             // <Lumen> the same; after OK, what OK just wrote

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

static LLPanelInjector<LumenPanelPreferenceAIKeys> t_pref_ai_keys("panel_preference_ai");
