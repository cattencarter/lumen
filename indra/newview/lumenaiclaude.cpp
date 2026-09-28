/**
 * @file lumenaiclaude.cpp
 * @brief Drive Claude Code, so the Assistant can run on a Claude subscription.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#include "llviewerprecompiledheaders.h"

#include "lumenaiclaude.h"

#include "lldir.h"
#include "llsdjson.h"
#include "llfile.h"
#include "lleventcoro.h"   // llcoro::suspend, for runToResult
#include "lltimer.h"

#if !LL_WINDOWS
#include <cstdio>          // <Lumen> popen, for tooOld()
#include <climits>         //   PATH_MAX, for updateCommand()
#include <cstdlib>         //   realpath
#include <sys/stat.h>
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

    /**
     * Somewhere harmless to run it.
     *
     * **Claude Code reads a CLAUDE.md out of its working directory**, so
     * whatever directory it starts in becomes instructions. Starting it in the
     * user's home, or wherever the viewer happens to have been launched from,
     * would hand a stranger's file to the model as though we had written it.
     * An empty directory of our own has nothing to say.
     *
     * **But it is not the only CLAUDE.md that loads, and this used to claim it
     * was.** Claude Code also reads the user's own ~/.claude/CLAUDE.md, and
     * every CLAUDE.md in the folders above the working directory -- and this
     * one sits inside the home folder, so ~/CLAUDE.md is among them. Nothing in
     * the installed CLI's options stops that and keeps the sign-in: `--bare`
     * skips the discovery but forces API-key authentication and never reads
     * the OAuth login, which is the one thing this provider exists to use, and
     * `--safe-mode` names MCP servers among what it switches off, and those
     * are the viewer's tools.
     * So a Claude Code user's personal instructions do reach this assistant.
     * What start() does keep out is listed there.
     */
    std::string workDir()
    {
        const std::string d = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "claude_code");
        LLFile::mkdir(d);
        return d;
    }
}

std::string LumenAIClaude::cliPath()
{
    // **A GUI application does not get the user's shell PATH**, so `claude`
    // resolving in Terminal says nothing about whether the viewer can find it.
    // Look where the installers put it, in the order they are likely.
    const std::string home = homeDir();
    const std::string s    = gDirUtilp->getDirDelimiter();

    std::vector<std::string> candidates;
    candidates.push_back("/opt/homebrew/bin" + s + "claude");
    candidates.push_back("/usr/local/bin" + s + "claude");
    if (!home.empty())
    {
        candidates.push_back(home + s + ".local" + s + "bin" + s + "claude");
        candidates.push_back(home + s + ".claude" + s + "local" + s + "claude");
        candidates.push_back(home + s + ".bun" + s + "bin" + s + "claude");
    }

    for (size_t i = 0; i < candidates.size(); ++i)
    {
        if (gDirUtilp->fileExists(candidates[i])) return candidates[i];
    }
    return std::string();
}

bool LumenAIClaude::installed()
{
    return !cliPath().empty();
}

// <Lumen> See the header. Found 2026-09-28 on a machine with 2.1.220: start()
// had been checked against `claude --help` of 2.1.278 on another, and the
// older copy stopped every turn before it began.
bool LumenAIClaude::tooOld(std::string* version)
{
#if LL_WINDOWS
    (void)version;
    return false;   // not ported at all; unavailableHere() says so
#else
    const std::string cli = cliPath();
    if (cli.empty()) return false;   // not installed is a different answer

    // stat follows the installer's symlink to the versioned file, so an update
    // -- which points the link at a new file -- changes what is compared here.
    struct stat st;
    if (stat(cli.c_str(), &st) != 0) return false;

    static std::string s_path;
    static time_t      s_mtime = 0;
    static off_t       s_size  = -1;
    static ino_t       s_ino   = 0;
    static bool        s_old   = false;
    static std::string s_version;

    if (s_path != cli || s_mtime != st.st_mtime || s_size != st.st_size || s_ino != st.st_ino)
    {
        s_path  = cli;
        s_mtime = st.st_mtime;
        s_size  = st.st_size;
        s_ino   = st.st_ino;
        s_old   = false;
        s_version.clear();

        // About 0.1 s, measured, and asked once per file rather than per turn.
        // The path is one of cliPath()'s fixed places, quoted for the spaces a
        // home folder can have.
        const std::string cmd = "\"" + cli + "\" --version 2>&1; echo LUMEN-HELP; \""
                              + cli + "\" --help 2>&1";
        std::string out;
        if (FILE* p = popen(cmd.c_str(), "r"))
        {
            char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
            pclose(p);
        }
        const size_t split = out.find("LUMEN-HELP");
        if (split != std::string::npos)
        {
            std::string first = out.substr(0, split);
            const size_t cut = first.find_first_of(" \r\n");
            if (cut != std::string::npos) first = first.substr(0, cut);
            s_version = first;

            const std::string help = out.substr(split);
            // Only a real help text counts as an answer: one that names its
            // other options but not this one.
            const bool is_help = help.find("--print") != std::string::npos
                              || help.find("Options:") != std::string::npos;
            s_old = is_help && help.find("--restricted") == std::string::npos;
        }
        LL_INFOS("LumenAI") << "Claude Code at " << cli << " is " << s_version
                            << (s_old ? ", too old: it has no --restricted" : "") << LL_ENDL;
    }

    if (version) *version = s_version;
    return s_old;
#endif
}

std::string LumenAIClaude::updateCommand(bool forPerson)
{
    const std::string cli = cliPath();
    std::string real = cli;
#if !LL_WINDOWS
    if (!cli.empty())
    {
        char buf[PATH_MAX];
        if (realpath(cli.c_str(), buf)) real = buf;
    }
#endif
    // A Homebrew cask lives in its Caskroom and refuses to update itself.
    const size_t cask = real.find("/Caskroom/claude-code/");
    if (cask != std::string::npos)
    {
        const std::string brew = real.substr(0, cask) + "/bin/brew";
        return forPerson ? std::string("brew upgrade claude-code")
                         : "\"" + brew + "\" upgrade claude-code";
    }
    // An npm install is a package in a global node_modules.
    if (real.find("/node_modules/") != std::string::npos)
    {
        return forPerson ? std::string("npm install -g @anthropic-ai/claude-code@latest")
                         : "\"" + cli + "\" update";
    }
    return forPerson ? std::string("claude update")
                     : "\"" + (cli.empty() ? std::string("claude") : cli) + "\" update";
}

std::string LumenAIClaude::tooOldText(const std::string& version, bool inPanel)
{
    return "Your copy of Claude Code" + (version.empty() ? std::string() : " (" + version + ")")
         + " is too old for Lumen. Lumen starts it in a restricted mode that keeps it away from "
           "your files and your shell, and this version does not have that mode, so Lumen will "
           "not use it.\n\nTo update it, press \"Set it up for me...\""
         + std::string(inPanel ? " below" : " in Preferences > AI")
         + " and Lumen does it for you. Or do it yourself: open Terminal, type  "
         + updateCommand(true) + "  and press Return. Then press Test.";
}
// </Lumen>

std::string LumenAIClaude::unavailableHere()
{
#if LL_WINDOWS
    // Not ported: cliPath() looks for Unix names, and every setup step runs
    // /bin/sh. Say that, rather than "not installed" about a program that may
    // well be installed.
    return "Claude Code does not work in Lumen on Windows yet. Anthropic, OpenAI "
           "or a local model work on every platform.";
#else
    return std::string();
#endif
}

bool LumenAIClaude::start(const std::string& prompt,
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
        why = here.empty() ? std::string("Claude Code is not installed.") : here;
        return false;
    }
    // <Lumen> Refused here, with the reason, rather than started to fail on
    // its own flags with "error: unknown option '--restricted'". Dropping the
    // flag for an old copy is not on offer: it is the safety argument below.
    {
        std::string version;
        if (tooOld(&version))
        {
            why = tooOldText(version);
            return false;
        }
    }
    // </Lumen>

    LLProcess::Params params;
    params.executable = cli;
    params.cwd        = workDir();

    // **`--restricted` is not tidiness, it is the whole safety argument.**
    // Claude Code ships with Bash, a REPL and file editing, and Lumen has no
    // business handing a model the user's shell. The Codex path had to reach
    // this the hard way -- eleven plugins switched off by name after watching
    // it drive the screen -- and here it is one documented flag.
    params.args.add("--restricted");
    params.args.add("-p");
    params.args.add("--output-format");
    params.args.add("stream-json");
    params.args.add("--include-partial-messages");
    params.args.add("--verbose");

    // Only the viewer's own tools, named explicitly. Anything Claude Code can
    // still reach that we did not ask for is denied rather than prompted --
    // there is nobody at a terminal to answer a prompt.
    //
    // **So a tool missing here is a tool the model can see and never use.**
    // `build` was added to the viewer after this line was written and was not
    // added here: the model reached for it correctly, was told "you haven't
    // granted it yet", and asked the user to approve a prompt that does not
    // exist. actions-check.py now fails when a tool the viewer declares is
    // not named on this list.
    params.args.add("--allowedTools");
    params.args.add("mcp__second_life__inventory,mcp__second_life__chat,"
                    "mcp__second_life__movement,mcp__second_life__viewer,"
                    "mcp__second_life__build");

    // The endpoint, as a config string rather than a file, so there is no
    // temporary file to write, leave behind, or have somebody else edit.
    params.args.add("--mcp-config");
    params.args.add(llformat("{\"mcpServers\":{\"second_life\":"
                             "{\"type\":\"http\",\"url\":\"http://127.0.0.1:%d/mcp\"}}}",
                             (int)port));

    // **And ONLY the endpoint.** `--mcp-config` adds to the user's own MCP
    // servers rather than replacing them, so a Claude Code user's mail or
    // GitHub server -- tools they allowed for coding -- was started on every
    // turn and offered to a model reading strangers' IMs and notecards, where
    // none of the viewer's own checks would ever see a call to it.
    // `--restricted` already ignores the user, project and local settings
    // files, which is where their hooks and "always allow" rules live -- the
    // installed CLI's own help says so, and that it does NOT skip MCP servers
    // without this flag. `--disable-slash-commands` ("Disable all skills")
    // keeps their skills out for the same reason. Both flags checked against
    // `claude --help`, 2.1.278.
    params.args.add("--strict-mcp-config");
    params.args.add("--disable-slash-commands");

    if (!system.empty())
    {
        params.args.add("--append-system-prompt");
        params.args.add(system);
    }
    if (!model.empty())
    {
        params.args.add("--model");
        params.args.add(model);
    }
    if (!resume.empty())
    {
        params.args.add("--resume");
        params.args.add(resume);
    }

    // **The question goes in as an argument, not down stdin.** LLProcess runs
    // the executable directly rather than through a shell, so nothing in it is
    // interpreted -- quotes, newlines and apostrophes all arrive intact. Down
    // stdin it would need the pipe closed to signal end of input, which is one
    // more thing to get wrong for no gain.
    //
    // **After `--`, so it cannot be read as an option.** Its parser (commander,
    // read in the 2.1.278 binary) takes anything longer than one character
    // that starts with "-" as an option, so a pasted list starting "- " or
    // "-5 degrees please" was refused on stderr before anything ran. `--` ends
    // option parsing there, and also stops a variadic option such as
    // --mcp-config taking the prompt as a second value when none of the
    // optional ones above are passed.
    params.args.add("--");
    params.args.add(prompt);

    params.files.add(LLProcess::FileParam());                   // stdin, unused
    params.files.add(LLProcess::FileParam().type("pipe"));      // stdout
    params.files.add(LLProcess::FileParam().type("pipe"));      // stderr

    params.autokill = true;   // a turn that is abandoned takes its process along

    // **Every tool, with its description, from the first turn.** By default
    // Claude Code shows a model only the tool NAMES and makes it look each one
    // up before it can read what it does. Asked to cover two prims, Haiku
    // looked up `build` and `viewer`, never `movement` -- where finding an
    // object by name lives -- and asked the user for the object ids. The
    // other providers are handed all five descriptions every turn; this makes
    // Claude Code the same. "false" selects its standard mode (read from the
    // 2.1.278 binary: falsy ENABLE_TOOL_SEARCH -> "standard"). Set in our own
    // environment because LLProcess passes that on (APR_PROGRAM_PATH), and
    // nothing else the viewer starts reads it.
#if LL_WINDOWS
    _putenv_s("ENABLE_TOOL_SEARCH", "false");
#else
    setenv("ENABLE_TOOL_SEARCH", "false", 1);
#endif

    mProc = LLProcess::create(params);
    if (!mProc)
    {
        why = "Could not start Claude Code.";
        return false;
    }
    return true;
}

bool LumenAIClaude::poll(LLSD& out)
{
    if (!mProc) return false;

    boost::optional<LLProcess::ReadPipe&> opt = mProc->getOptReadPipe(LLProcess::STDOUT);
    if (!opt) return false;
    LLProcess::ReadPipe& pipe = *opt;

    // One line at a time, and only when a whole one has arrived: `getline` on a
    // partial line would hand back half a JSON object, which parses as nothing
    // and is then gone.
    if (!pipe.contains('\n')) return false;

    const std::string line = pipe.getline();
    if (line.empty()) return false;

    try
    {
        out = LlsdFromJson(boost::json::parse(line));
    }
    catch (...)
    {
        // Claude Code prints the occasional plain line. Not an error, not JSON,
        // and not worth stopping a turn for.
        return false;
    }
    return out.isMap();
}

bool LumenAIClaude::lineWaiting()
{
    if (!mProc) return false;
    boost::optional<LLProcess::ReadPipe&> opt = mProc->getOptReadPipe(LLProcess::STDOUT);
    return opt && opt->contains('\n');
}

std::string LumenAIClaude::collectError()
{
    if (!mProc) return std::string();
    boost::optional<LLProcess::ReadPipe&> opt = mProc->getOptReadPipe(LLProcess::STDERR);
    if (!opt || !opt->size()) return std::string();
    std::string fresh = opt->read(opt->size());
    // Bounded: this goes into the log and, first line only, onto the screen.
    const size_t room = (mLastError.size() < 4000) ? 4000 - mLastError.size() : 0;
    if (fresh.size() > room) fresh.resize(room);
    mLastError += fresh;
    return fresh;
}

bool LumenAIClaude::runToResult(const std::string& prompt, const std::string& model,
                                U16 port, F64 seconds, LLSD& result, std::string& why)
{
    if (!start(prompt, std::string(), model, std::string(), port, why)) return false;

    const F64 until = LLTimer::getTotalSeconds() + seconds;
    F64 exited_at = 0.0;
    LLSD msg;
    while (LLTimer::getTotalSeconds() < until)
    {
        // Read whether it has exited BEFORE draining, so a line written in
        // between is picked up on the next pass rather than lost.
        const bool exited = !running();

        // **Every whole line waiting, not one.** After the process exits
        // several can be left in the pipe, and the result line is the last it
        // writes. The old loops read one line after exit, found it was not the
        // result, then read again and discarded what they had just read.
        while (lineWaiting())
        {
            if (poll(msg) && msg["type"].asString() == "result")
            {
                result = msg;
                stop();
                return true;
            }
        }

        if (exited)
        {
            // A short grace after exit: the pipe reader and the exit check run
            // on separate ticks, so the last lines can land a moment later.
            if (exited_at == 0.0) exited_at = LLTimer::getTotalSeconds();
            else if (LLTimer::getTotalSeconds() - exited_at > 0.5) break;
        }
        llcoro::suspend();
    }

    const bool timed_out = running();
    stop();
    const std::string err = mLastError.substr(0, mLastError.find('\n'));
    why = timed_out ? std::string("Claude Code did not answer in time.")
                    : std::string("Claude Code stopped without answering.");
    if (!err.empty()) why += " It said: " + err;
    return false;
}

bool LumenAIClaude::running() const
{
    return mProc && mProc->isRunning();
}

void LumenAIClaude::stop()
{
    if (mProc)
    {
        // Before the process goes: once it is reset, what it said on stderr
        // goes with it, and that is often the only reason a turn failed.
        const std::string fresh = collectError();
        if (!fresh.empty())
        {
            LL_WARNS("LumenAIClaude") << "Claude Code wrote on stderr: " << fresh << LL_ENDL;
        }
        mProc->kill();
        mProc.reset();
    }
}
