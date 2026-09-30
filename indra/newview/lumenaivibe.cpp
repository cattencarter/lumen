/**
 * @file lumenaivibe.cpp
 * @brief Drive Mistral Vibe, so the Assistant can run on a Mistral subscription.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#include "llviewerprecompiledheaders.h"

#include "lumenaivibe.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdjson.h"
#include "lleventcoro.h"
#include "lltimer.h"
#include "llviewerwindow.h"
#include "llwindow.h"

#include <boost/json.hpp>

#include "lumenaiwin.h"   // <Lumen>

#if LL_WINDOWS
#include <wincred.h>      // <Lumen> signedIn(): Credential Manager, by name only
#else
#include <cstdlib>
#include <unistd.h>
#endif

namespace
{
    /** The user's home, which is NOT gDirUtilp->getOSUserDir(). */
    std::string homeDir()
    {
        const char* h = getenv("HOME");
        if (!h || !*h) h = getenv("USERPROFILE");
        return (h && *h) ? std::string(h) : std::string();
    }

    /** A string as a TOML basic string, quotes included. */
    std::string toml(const std::string& in)
    {
        std::string out = "\"";
        for (char c : in)
        {
            if (c == '"' || c == '\\') out += '\\';
            if (c == '\n') { out += "\\n"; continue; }
            out += c;
        }
        return out + "\"";
    }

    bool writeFile(const std::string& path, const std::string& text)
    {
        llofstream out(path.c_str(), std::ios::out | std::ios::trunc | std::ios::binary);
        if (!out.is_open()) return false;
        out << text;
        out.close();
        return !out.fail();
    }

    /** Whether a .env file names the key, without keeping what it says. */
    bool envHasKey(const std::string& path)
    {
        llifstream in(path.c_str());
        if (!in.is_open()) return false;
        std::string line;
        while (std::getline(in, line))
        {
            if (line.compare(0, 16, "MISTRAL_API_KEY=") == 0 && line.size() > 16) return true;
        }
        return false;
    }

    /**
     * Vibe keeps the key in the Keychain, and only falls back to a `.env`
     * when the Keychain refuses. A person who signed in with their own `vibe
     * --setup` on such a machine has it in ~/.vibe/.env, which Lumen's own
     * folder does not read -- so link to it rather than copy a secret.
     */
    void linkUserEnv()
    {
#if !LL_WINDOWS
        const std::string mine = gDirUtilp->add(LumenAIVibe::home(), ".env");
        const std::string theirs = homeDir() + "/.vibe/.env";
        if (!gDirUtilp->fileExists(mine) && envHasKey(theirs))
        {
            symlink(theirs.c_str(), mine.c_str());
        }
#endif
    }

    // <Lumen> What Vibe's process needs in its environment on Windows, set in
    // the viewer's own (LLProcess passes it on, and nothing else the viewer
    // starts reads these): its settings folder, no telemetry, and UTF-8 --
    // Python on Windows otherwise reads stdin in the old ANSI code page, and a
    // Danish question arrives with its letters mangled.
#if LL_WINDOWS
    void setVibeEnvironment()
    {
        _putenv_s("VIBE_HOME", LumenAIVibe::home().c_str());
        _putenv_s("VIBE_ENABLE_TELEMETRY", "false");
        _putenv_s("PYTHONUTF8", "1");
    }
#endif

    /** The assistant's words in one streamed entry, or empty. */
    std::string assistantText(const LLSD& entry)
    {
        if (entry["type"].asString() != "message" || entry["role"].asString() != "assistant")
        {
            return std::string();
        }
        std::string text;
        const LLSD& content = entry["content"];
        for (LLSD::array_const_iterator it = content.beginArray(); it != content.endArray(); ++it)
        {
            if ((*it)["type"].asString() == "text") text += (*it)["text"].asString();
        }
        return text;
    }
}

std::string LumenAIVibe::cliPath()
{
    // A GUI application does not get the user's shell PATH. The installer is
    // uv's, which puts it in ~/.local/bin unless told otherwise.
    const std::string home = homeDir();
    std::vector<std::string> candidates;
#if LL_WINDOWS
    // <Lumen> uv on Windows puts vibe.exe in %USERPROFILE%\.local\bin too
    // (seen in the Windows VM, 2026-09-30), and that folder is not on PATH.
    if (!home.empty()) candidates.push_back(home + "\\.local\\bin\\vibe.exe");
#else
    if (const char* xdg = getenv("XDG_BIN_HOME")) { if (*xdg) candidates.push_back(std::string(xdg) + "/vibe"); }
    if (!home.empty()) candidates.push_back(home + "/.local/bin/vibe");
    candidates.push_back("/opt/homebrew/bin/vibe");
    candidates.push_back("/usr/local/bin/vibe");
#endif
    for (const std::string& c : candidates)
    {
        if (gDirUtilp->fileExists(c)) return c;
    }
    return std::string();
}

bool LumenAIVibe::installed() { return !cliPath().empty(); }

std::string LumenAIVibe::acpPath()
{
    const std::string cli = cliPath();
    if (cli.empty()) return std::string();
    return cli.substr(0, cli.find_last_of("/\\") + 1) + LumenAIWin::exe("vibe-acp");
}

std::string LumenAIVibe::unavailableHere()
{
    // <Lumen> Works on Windows too since 2026-09-30 (Findings 43).
    return std::string();
}

std::string LumenAIVibe::home()
{
    const std::string d = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "vibe");
    LLFile::mkdir(d);
    LLFile::mkdir(gDirUtilp->add(d, "prompts"));
    LLFile::mkdir(gDirUtilp->add(d, "work"));
    return d;
}

bool LumenAIVibe::signedIn(bool fresh)
{
    static F64  s_at  = -100.0;
    static bool s_yes = false;
    const F64 now = LLTimer::getTotalSeconds();
    if (!fresh && now - s_at < 3.0) return s_yes;
    s_at = now;

    linkUserEnv();
    const char* env = getenv("MISTRAL_API_KEY");
    s_yes = (env && *env) || envHasKey(gDirUtilp->add(home(), ".env"));
#if LL_WINDOWS
    // <Lumen> On Windows Vibe's key is in Credential Manager, filed under
    // "ai.mistral.vibe" (read in the Windows VM with `cmdkey /list`, which
    // shows names only). Asked for by that name; the entry's secret is never
    // looked at, and the list is freed at once.
    if (!s_yes)
    {
        DWORD count = 0;
        PCREDENTIALW* creds = NULL;
        // The filter is a name prefix followed by "*", as Microsoft documents
        // it; the entry itself is "ai.mistral.vibe" (read with cmdkey in the
        // VM). Names only -- the secret is never looked at.
        if (CredEnumerateW(L"ai.mistral.vibe*", 0, &count, &creds) && creds)
        {
            s_yes = count > 0;
            CredFree(creds);
        }
    }
    return s_yes;
#else
    if (!s_yes)
    {
        // The service names Vibe writes under, current and older. Without -w
        // `security` prints only the entry's attributes, never the password.
        s_yes = system("/usr/bin/security find-generic-password -s ai.mistral.vibe "
                       "-a MISTRAL_API_KEY >/dev/null 2>&1") == 0
             || system("/usr/bin/security find-generic-password -s vibe "
                       "-a MISTRAL_API_KEY >/dev/null 2>&1") == 0;
    }
    return s_yes;
#endif
}

bool LumenAIVibe::start(const std::string& prompt,
                        const std::string& system,
                        const std::string& model,
                        const std::string& resume,
                        U16                port,
                        std::string&       why)
{
    stop();
    mLastError.clear();

    const std::string cli = cliPath();
    if (cli.empty())
    {
        const std::string here = unavailableHere();
        why = here.empty() ? std::string("Mistral Vibe is not installed.") : here;
        return false;
    }

    const std::string dir  = home();
    const std::string work = gDirUtilp->add(dir, "work");

    // Tool descriptions are cached for a day, keyed on the address. A viewer
    // that gained an action since would go on offering yesterday's list, so
    // the cache goes at the first turn of every session.
    static bool s_fresh = false;
    if (!s_fresh)
    {
        gDirUtilp->deleteDirAndContents(gDirUtilp->add(gDirUtilp->add(dir, "logs"),
                                                       "mcp-descriptors"));
        s_fresh = true;
    }

    // **Settings, rewritten every turn**: the endpoint's port is new each time
    // the viewer starts. Everything that would reach past the viewer's tools
    // or report home is off. `system_prompt_id` replaces Vibe's own prompt,
    // which is about editing a codebase with a shell.
    std::string cfg =
        "# Written by Lumen before every turn. Your own Vibe settings are in ~/.vibe;\n"
        "# nothing here reads or changes them.\n"
        "active_model = " + toml(model.empty() ? std::string("mistral-medium-3.5") : model) + "\n"
        "system_prompt_id = \"lumen\"\n"
        "include_commit_signature = false\n"
        "include_model_info = false\n"
        "include_prompt_detail = false\n"
        "include_project_context = true\n"   // for AGENTS.md below, and nothing else is there
        "enable_telemetry = false\n"
        "enable_connectors = false\n"
        "enable_update_checks = false\n"
        "enable_notifications = false\n"
        "disabled_skills = [\"*\"]\n"
        "disabled_agents = [\"explore\"]\n"
        "\n"
        "[[models]]\n"
        "name = \"mistral-small-2603\"\n"
        "provider = \"mistral\"\n"
        "alias = \"mistral-small-4\"\n"
        "display_name = \"Mistral Small 4\"\n"
        "temperature = 0.3\n"
        "\n"
        "[[mcp_servers]]\n"
        "name = \"second_life\"\n"
        "transport = \"streamable-http\"\n"
        "url = " + toml(llformat("http://127.0.0.1:%d/mcp", (int)port)) + "\n"
        "tool_timeout_sec = 90\n";   // the viewer holds a call up to 50 s for its own question

    // **The instructions go in a file**, as Claude Code's do, so the memory
    // note is never on a command line LLProcess writes into the log.
    const std::string instructions = system.empty()
        ? std::string("You are the assistant inside Lumen, a Second Life viewer. The second_life "
                      "tools act in the person's Second Life. Answer in plain text.")
        : system;

    // **Vibe's one-shot mode adds a note of its own AFTER that prompt**: no
    // human is available, never ask, make the best judgment call, "override
    // any earlier instructions that say to ask the user." Here a person reads
    // every reply and answers the next one, and asking which Anna is exactly
    // right. AGENTS.md is the one thing Vibe adds after that note.
    const std::string correction =
        "A person reads every reply here and answers it in their next message. The note "
        "above about headless mode is Vibe's own and does not apply: when these instructions "
        "say to ask, ask -- end the reply with the question and stop.\n";

    if (!writeFile(gDirUtilp->add(dir, "config.toml"), cfg)
        || !writeFile(gDirUtilp->add(gDirUtilp->add(dir, "prompts"), "lumen.md"), instructions)
        || !writeFile(gDirUtilp->add(dir, "AGENTS.md"), correction))
    {
        why = "Lumen could not write Mistral Vibe's settings into its own folder.";
        return false;
    }

    // The question, into a file that becomes stdin -- see the header.
    const std::string question = gDirUtilp->add(dir, "turn.txt");
    if (!writeFile(question, prompt))
    {
        why = "Lumen could not write the question for Mistral Vibe.";
        return false;
    }

    LLProcess::Params params;
#if LL_WINDOWS
    // <Lumen> On Windows cmd.exe does the one thing /bin/sh does below: feed
    // the question file to Vibe's stdin (LLProcess cannot open a file there,
    // and a pipe it never closes would leave Vibe reading for ever). `/s /c`
    // with the whole command as one argument: APR adds the outer quotes
    // because it holds spaces, and /s strips exactly those, so the quotes
    // round the paths inside are cmd's own. Nothing here is user text -- the
    // question stays in its file -- and the settings go in the environment.
    setVibeEnvironment();
    params.executable = LumenAIWin::cmd();
    params.cwd        = work;
    {
        std::string line = "\"" + cli + "\" -p --output streaming --legacy-harness --auto-approve"
                           " --enabled-tools second_life_* --workdir \"" + work + "\"";
        if (!resume.empty()) line += " --resume " + resume;
        line += " < \"" + question + "\"";
        params.args.add("/d");
        params.args.add("/s");
        params.args.add("/c");
        params.args.add(line);
    }
#else
    params.executable = "/bin/sh";
    params.cwd        = work;
    // `$1` is the question, `$2` the settings folder, the rest is Vibe and its
    // arguments. Nothing is interpreted by the shell but the redirection.
    params.args.add("-c");
    params.args.add("f=\"$1\"; export VIBE_HOME=\"$2\" VIBE_ENABLE_TELEMETRY=false; "
                    "shift 2; exec \"$@\" < \"$f\"");
    params.args.add("sh");
    params.args.add(question);
    params.args.add(dir);
    params.args.add(cli);
    params.args.add("-p");                     // no text: the question is stdin
    params.args.add("--output");
    params.args.add("streaming");
    // Pinned. A cached experiment can otherwise move it to a second backend
    // with tools and features of its own, telemetry off or not.
    params.args.add("--legacy-harness");
    params.args.add("--auto-approve");
    params.args.add("--enabled-tools");
    params.args.add("second_life_*");
    params.args.add("--workdir");
    params.args.add(work);
    if (!resume.empty())
    {
        params.args.add("--resume");
        params.args.add(resume);
    }
#endif

    params.files.add(LLProcess::FileParam());                   // stdin: the shell redirects it
    params.files.add(LLProcess::FileParam().type("pipe"));      // stdout
    params.files.add(LLProcess::FileParam().type("pipe"));      // stderr
    params.autokill = true;   // an abandoned turn takes its process along
    params.hidden   = true;   // <Lumen> no console window on Windows (llprocess.h)

    mProc = LLProcess::create(params);
    if (!mProc)
    {
        why = "Could not start Mistral Vibe.";
        return false;
    }
    return true;
}

bool LumenAIVibe::poll(LLSD& out)
{
    if (!mProc) return false;
    boost::optional<LLProcess::ReadPipe&> opt = mProc->getOptReadPipe(LLProcess::STDOUT);
    if (!opt || !opt->contains('\n')) return false;

    const std::string line = opt->getline();
    if (line.empty()) return false;
    try
    {
        out = LlsdFromJson(boost::json::parse(line));
        if (out.isMap()) return true;
    }
    catch (...)
    {
    }
    // Some of Vibe's errors -- a bad setting, a missing folder -- are printed
    // on stdout as plain text. Kept as the reason, bounded.
    if (mLastError.size() < 4000) mLastError += line.substr(0, 1000) + "\n";
    return false;
}

bool LumenAIVibe::lineWaiting()
{
    if (!mProc) return false;
    boost::optional<LLProcess::ReadPipe&> opt = mProc->getOptReadPipe(LLProcess::STDOUT);
    return opt && opt->contains('\n');
}

std::string LumenAIVibe::collectError()
{
    if (!mProc) return std::string();
    boost::optional<LLProcess::ReadPipe&> opt = mProc->getOptReadPipe(LLProcess::STDERR);
    if (!opt || !opt->size()) return std::string();
    std::string fresh = opt->read(opt->size());
    const size_t room = (mLastError.size() < 4000) ? 4000 - mLastError.size() : 0;
    if (fresh.size() > room) fresh.resize(room);
    mLastError += fresh;
    return fresh;
}

bool LumenAIVibe::runToAnswer(const std::string& prompt, const std::string& model, U16 port,
                              F64 seconds, std::string& answer, std::string& why)
{
    answer.clear();
    if (!start(prompt, std::string(), model, std::string(), port, why)) return false;

    const F64 until = LLTimer::getTotalSeconds() + seconds;
    F64 exited_at = 0.0;
    LLSD msg;
    while (LLTimer::getTotalSeconds() < until)
    {
        const bool exited = !running();
        while (lineWaiting())
        {
            if (poll(msg))
            {
                const std::string said = assistantText(msg);
                if (!said.empty() && said.find("<vibe_stop_event>") == std::string::npos)
                {
                    if (!answer.empty()) answer += "\n";
                    answer += said;
                }
            }
        }
        if (exited)
        {
            if (exited_at == 0.0) exited_at = LLTimer::getTotalSeconds();
            else if (LLTimer::getTotalSeconds() - exited_at > 0.5) break;
        }
        llcoro::suspend();
    }

    const bool timed_out = running();
    stop();
    if (!answer.empty() && !timed_out) return true;
    const std::string err = mLastError.substr(0, mLastError.find('\n'));
    why = timed_out ? std::string("Mistral Vibe did not answer in time.")
                    : std::string("Mistral Vibe stopped without answering.");
    if (!err.empty()) why += " It said: " + err;
    return false;
}

bool LumenAIVibe::running() const
{
    return mProc && mProc->isRunning();
}

void LumenAIVibe::stop()
{
    if (mProc)
    {
        const std::string fresh = collectError();
        if (!fresh.empty())
        {
            LL_WARNS("LumenAIVibe") << "Mistral Vibe wrote on stderr: " << fresh << LL_ENDL;
        }
        mProc->kill();
        mProc.reset();
    }
}

// ---------------------------------------------------------------------------

bool LumenAIVibeSignIn::send(const LLSD& message)
{
    if (!mProc || !mProc->isRunning()) return false;
    boost::optional<LLProcess::WritePipe&> opt = mProc->getOptWritePipe(LLProcess::STDIN);
    if (!opt) return false;
    opt->get_ostream() << boost::json::serialize(LlsdToJson(message)) << "\n" << std::flush;
    return true;
}

bool LumenAIVibeSignIn::reply(S32 id, F64 seconds, const std::function<bool()>& still_wanted,
                              LLSD& out, std::string& why)
{
    const F64 until = LLTimer::getTotalSeconds() + seconds;
    while (LLTimer::getTotalSeconds() < until)
    {
        if (!still_wanted()) { why = "Stopped."; return false; }
        boost::optional<LLProcess::ReadPipe&> opt;
        if (mProc) opt = mProc->getOptReadPipe(LLProcess::STDOUT);
        while (opt && opt->contains('\n'))
        {
            const std::string line = opt->getline();
            LLSD msg;
            try { msg = LlsdFromJson(boost::json::parse(line)); } catch (...) { continue; }
            if (!msg.has("id") || msg["id"].asInteger() != id) continue;   // a notification
            if (msg.has("error"))
            {
                why = msg["error"]["message"].asString();
                return false;
            }
            out = msg["result"];
            return true;
        }
        if (!mProc || !mProc->isRunning())
        {
            why = "Mistral Vibe's sign-in helper stopped.";
            if (mProc)
            {
                if (boost::optional<LLProcess::ReadPipe&> err =
                        mProc->getOptReadPipe(LLProcess::STDERR))
                {
                    const std::string said = err->read(err->size());
                    if (!said.empty()) why += " It said: " + said.substr(0, 300);
                }
            }
            return false;
        }
        llcoro::suspendUntilTimeout(0.1f);
    }
    why = "Mistral Vibe's sign-in helper did not answer.";
    return false;
}

bool LumenAIVibeSignIn::run(const std::function<bool()>& still_wanted, std::string& why)
{
    stop();
    const std::string acp = LumenAIVibe::acpPath();
    if (acp.empty() || !gDirUtilp->fileExists(acp))
    {
        why = "Mistral Vibe is not installed yet.";
        return false;
    }

    LLProcess::Params params;
#if LL_WINDOWS
    // <Lumen> The helper itself, with its settings in the environment.
    setVibeEnvironment();
    params.executable = acp;
    params.cwd = gDirUtilp->add(LumenAIVibe::home(), "work");
#else
    params.executable = "/bin/sh";
    params.cwd = gDirUtilp->add(LumenAIVibe::home(), "work");
    params.args.add("-c");
    params.args.add("export VIBE_HOME=\"$1\" VIBE_ENABLE_TELEMETRY=false; shift; exec \"$@\"");
    params.args.add("sh");
    params.args.add(LumenAIVibe::home());
    params.args.add(acp);
#endif
    params.files.add(LLProcess::FileParam().type("pipe"));      // stdin: the protocol
    params.files.add(LLProcess::FileParam().type("pipe"));      // stdout: the protocol
    params.files.add(LLProcess::FileParam().type("pipe"));      // stderr
    params.autokill = true;
    params.hidden   = true;   // <Lumen> no console window on Windows (llprocess.h)
    mProc = LLProcess::create(params);
    if (!mProc)
    {
        why = "Could not start Mistral Vibe's sign-in helper.";
        return false;
    }

    // The client says it will open the page itself. Without that the helper
    // offers only the sign-in that opens a browser from inside itself, and
    // only a terminal one besides.
    LLSD caps;
    caps["_meta"]["browser-auth-delegated"] = true;
    LLSD init;
    init["jsonrpc"] = "2.0";
    init["id"] = 1;
    init["method"] = "initialize";
    init["params"]["protocolVersion"] = 1;
    init["params"]["clientCapabilities"] = caps;
    LLSD result;
    if (!send(init) || !reply(1, 60.0, still_wanted, result, why)) { stop(); return false; }

    bool offered = false;
    const LLSD& methods = result["authMethods"];
    for (LLSD::array_const_iterator it = methods.beginArray(); it != methods.endArray(); ++it)
    {
        if ((*it)["id"].asString() == "browser-auth-delegated") offered = true;
    }
    if (!offered)
    {
        why = "This copy of Mistral Vibe cannot sign in from Lumen. Open Terminal, type  vibe "
              "--setup  and press Return, then come back.";
        stop();
        return false;
    }

    LLSD begin;
    begin["jsonrpc"] = "2.0";
    begin["id"] = 2;
    begin["method"] = "authenticate";
    begin["params"]["methodId"] = "browser-auth-delegated";
    begin["params"]["_meta"]["action"] = "start";
    if (!send(begin) || !reply(2, 60.0, still_wanted, result, why)) { stop(); return false; }

    const LLSD& attempt = result["_meta"]["browser-auth-delegated"];
    const std::string url = attempt["signInUrl"].asString();
    const std::string attempt_id = attempt["attemptId"].asString();

    // Only Mistral's own page. The address comes from the helper, and a
    // browser is not something to open on anybody else's say-so.
    std::string host = url.compare(0, 8, "https://") == 0 ? url.substr(8) : std::string();
    host = host.substr(0, host.find_first_of("/?#"));
    const bool mistral = host == "mistral.ai"
        || (host.size() > 11 && host.compare(host.size() - 11, 11, ".mistral.ai") == 0);
    if (!mistral || attempt_id.empty())
    {
        why = "Mistral Vibe's sign-in helper did not give a Mistral sign-in page.";
        stop();
        return false;
    }
    if (gViewerWindow && gViewerWindow->getWindow())
    {
        gViewerWindow->getWindow()->spawnWebBrowser(url, true);
    }

    // The helper waits here until the person has finished in the browser,
    // or the attempt expires -- ten minutes or so.
    //
    // **And it gives up early by design.** Three failed checks with Mistral's
    // server in a row -- nine seconds -- end the wait with an error while the
    // attempt stays open, for the client to ask again. The first version took
    // that error as the end, told the author it had not finished, and his
    // second press started a second sign-in: "I was asked to sign in twice."
    // So ask again, same attempt, until it completes, is refused as unknown
    // (denied, expired, or already done), or ten minutes pass.
    const F64 give_up = LLTimer::getTotalSeconds() + 900.0;
    for (S32 id = 3; LLTimer::getTotalSeconds() < give_up; ++id)
    {
        LLSD finish;
        finish["jsonrpc"] = "2.0";
        finish["id"] = id;
        finish["method"] = "authenticate";
        finish["params"]["methodId"] = "browser-auth-delegated";
        finish["params"]["_meta"]["action"] = "complete";
        finish["params"]["_meta"]["attemptId"] = attempt_id;
        std::string failed;
        if (!send(finish)) break;
        if (reply(id, give_up - LLTimer::getTotalSeconds(), still_wanted, result, failed))
        {
            stop();
            return true;
        }
        LL_INFOS("LumenAIVibe") << "Sign-in check: " << failed << LL_ENDL;
        if (!still_wanted() || !mProc || !mProc->isRunning()
            || failed.find("Unknown browser sign-in attempt") != std::string::npos)
        {
            break;
        }
        why = failed;   // kept, in case the next one is the last
        llcoro::suspendUntilTimeout(2.f);
    }
    if (why.empty()) why = "Signing in did not finish in time.";
    stop();
    return false;
}

void LumenAIVibeSignIn::stop()
{
    if (mProc)
    {
        mProc->kill();
        mProc.reset();
    }
}
