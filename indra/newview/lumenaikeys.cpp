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
#include "llcoros.h"
#include "lleventcoro.h"

#include "llbutton.h"
#include "llclipboard.h"
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

namespace LumenAIKeys
{
    const std::string NONE      = "none";
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
    const std::string CODEX     = "codex";
    const std::string CLAUDECODE = "claudecode";

    const std::vector<std::string>& providers()
    {
        static const std::vector<std::string> p = { ANTHROPIC, OPENAI };
        return p;
    }

    std::string displayName(const std::string& provider)
    {
        if (provider == NONE)      return "no assistant";
        if (provider == ANTHROPIC) return "Anthropic";
        if (provider == OPENAI)    return "OpenAI";
        if (provider == LOCAL)     return "the local model";
        if (provider == CODEX)     return "Codex";
        if (provider == CLAUDECODE) return "Claude Code";
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
    syncModelCombo(findChild<LLComboBox>("model_codex"),     "LumenAICodexModel");
    syncModelCombo(findChild<LLComboBox>("model_claude"),    "LumenAIClaudeCodeModel");

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
                say(st.ready, st.ready ? "Codex is installed, signed in and running."
                                       : st.text);
                return;
            }

            if (provider == LumenAIKeys::CLAUDECODE)
            {
                if (!LumenAIClaude::installed())
                {
                    refresh();
                    say(false, "Claude Code is not installed on this computer.");
                    return;
                }
                busy("Asking Claude Code... this takes a few seconds.");

                const std::string model = gSavedSettings.getString("LumenAIClaudeCodeModel");
                // The LIVE port: it is picked at random each start.
                const U16 port = LumenAIControl::instance().port();
                LLHandle<LLPanel> h = getHandle();
                LLCoros::instance().launch("LumenAIClaudeTest", [h, model, port]()
                {
                    LumenAIClaude cc;
                    std::string why, said;
                    bool ok = false;

                    if (!cc.start("Call the second_life viewer tool with action=status and "
                                  "reply with ONLY the session_check value, nothing else.",
                                  std::string(), model, std::string(), port, why))
                    {
                        said = why;
                    }
                    else
                    {
                        const F64 until = LLTimer::getTotalSeconds() + 120.0;
                        LLSD msg;
                        while (LLTimer::getTotalSeconds() < until)
                        {
                            if (!cc.poll(msg))
                            {
                                if (!cc.running() && !cc.poll(msg)) break;
                                llcoro::suspend();
                                continue;
                            }
                            if (msg["type"].asString() != "result") continue;
                            ok   = !msg["is_error"].asBoolean();
                            said = msg["result"].asString();
                            break;
                        }
                        if (said.empty()) said = "Claude Code did not answer.";
                        cc.stop();
                    }

                    LumenPanelPreferenceAIKeys* p =
                        dynamic_cast<LumenPanelPreferenceAIKeys*>(h.get());
                    if (!p) return;
                    if (said.size() > 220) said = said.substr(0, 220);
                    p->say(ok,
                        ok ? "Claude Code ran, is signed in, and reached the viewer's own "
                             "tools. It answered with this session's check value: " + said
                           : said + "\n\nIf it says not logged in, run  claude auth login  "
                             "in Terminal once.");
                    p->refresh();
                });
                return;
            }

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
    syncModelCombo(findChild<LLComboBox>("model_codex"),     "LumenAICodexModel");
    syncModelCombo(findChild<LLComboBox>("model_claude"),    "LumenAIClaudeCodeModel");

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
    if (LLView* v = findChild<LLView>(who == "codex" ? "model_codex" : "model_claude"))
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
                      : (provider == LumenAIKeys::LOCAL)      ? "local_status"
                      : NULL;
    if (which)
    {
        if (LLTextBox* t = findChild<LLTextBox>(which)) t->setText(text);
    }
}

LumenPanelPreferenceAIKeys::CodexState LumenPanelPreferenceAIKeys::codexStatus()
{
    CodexState st;

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
    const std::string cli  = dot + "packages" + sep + "standalone" + sep + "current"
                           + sep + "bin" + sep + "codex";
    const std::string auth = dot + "auth.json";
    const std::string sock = dot + "app-server-control" + sep + "app-server-control.sock";

    if (!gDirUtilp->fileExists(cli))
    {
        st.text = "Not set up yet. Codex is OpenAI's own small helper program, and it is "
                  "what lets Lumen use the ChatGPT subscription you already pay for instead "
                  "of a paid API key.";
        st.command = "curl -fsSL https://chatgpt.com/codex/install.sh | sh";
        return st;
    }
    if (!gDirUtilp->fileExists(auth))
    {
        st.text = "Almost. Codex is installed but not signed in to your ChatGPT "
                  "account yet.";
        st.command = "codex login";
        return st;
    }
    if (!gDirUtilp->fileExists(sock))
    {
        st.text = "Almost. Codex is installed and signed in, but it is not running "
                  "yet.";
        st.command = "codex app-server daemon start";
        return st;
    }

    st.ready = true;
    st.text  = "Ready. Codex is installed, signed in and running, and Lumen can reach it -- "
               "including the viewer's own tools, over the same endpoint any other assistant "
               "uses. Your ChatGPT account pays for this, so there is no API key and no "
               "separate bill; your plan's limits apply instead.\n"
               "If the Assistant will not answer, `codex doctor` in Terminal checks the "
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

    st.ready = true;

    return st;
}

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
        const std::string provider = gSavedSettings.getString("LumenAIProvider");
        if (provider == LumenAIKeys::CODEX || provider == LumenAIKeys::CLAUDECODE)
        {
            const bool ready = (provider == LumenAIKeys::CODEX) ? codexStatus().ready
                                                             : claudeStatus().ready;
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
    if (LLPanel* p = findChild<LLPanel>("p_codex"))     p->setVisible(provider == "codex");
    if (LLPanel* p = findChild<LLPanel>("p_claudecode")) p->setVisible(provider == "claudecode");
    if (LLPanel* p = findChild<LLPanel>("p_local"))     p->setVisible(provider == "local");

    const CodexState codex = codexStatus();
    // <Lumen> No standing status line. The author: *"when it's all set up we
    // just remove the button. no need to tell it's not set up"* -- and he is
    // right: the button being there IS the message, and a paragraph repeating
    // it in words is one more thing to read before doing the only thing on
    // offer. The box below is left for the transient "Asking..." while a test
    // is in flight, because a button that goes quiet for twenty seconds looks
    // broken.
    if (LLTextBox* cs = findChild<LLTextBox>("codex_status")) cs->setText(std::string());
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
    setupOrModel("codex", codex.ready);

    const CodexState claude = claudeStatus();
    setupOrModel("claude", claude.ready);

    // Seeded here rather than in draw(), so that switching provider -- which
    // calls refresh() -- cannot look like a provider that just became ready.
    mWasReady = (provider == LumenAIKeys::CODEX)      ? codex.ready
              : (provider == LumenAIKeys::CLAUDECODE) ? claude.ready
              : false;
    if (LLTextBox* cs = findChild<LLTextBox>("claude_status")) cs->setText(std::string());

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
    // This only ever meant to rescue a key provider, or none, with no key.
    if (chosen != LumenAIKeys::ANTHROPIC && chosen != LumenAIKeys::OPENAI && chosen != "none"
        && !chosen.empty())
    {
        return;
    }

    const std::string other = (chosen == LumenAIKeys::OPENAI)
                            ? LumenAIKeys::ANTHROPIC : LumenAIKeys::OPENAI;
    if (!LumenAIKeys::has(other))
    {
        return;                                   // neither works; nothing to pick
    }

    // Not logged: the logging macros reach a private member of
    // LLPanelPreference from here. It does not need to be -- refresh() follows,
    // so the change is visible in the control itself rather than only in a file.
    gSavedSettings.setString("LumenAIProvider", other);
}

void LumenPanelPreferenceAIKeys::cancel(const std::vector<std::string> settings_to_skip)
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

static LLPanelInjector<LumenPanelPreferenceAIKeys> t_pref_ai_keys("panel_preference_ai");
