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
     * `--bare` would also skip that discovery, and is the wrong tool: it forces
     * API-key authentication and never reads the OAuth login, which is the one
     * thing this provider exists to use.
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

bool LumenAIClaude::start(const std::string& prompt,
                       const std::string& system,
                       const std::string& model,
                       const std::string& resume,
                       U16                port,
                       std::string&       why)
{
    stop();

    const std::string cli = cliPath();
    if (cli.empty())
    {
        why = "Claude Code is not installed.";
        return false;
    }

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
    params.args.add("--allowedTools");
    params.args.add("mcp__second_life__inventory,mcp__second_life__chat,"
                    "mcp__second_life__movement,mcp__second_life__viewer");

    // The endpoint, as a config string rather than a file, so there is no
    // temporary file to write, leave behind, or have somebody else edit.
    params.args.add("--mcp-config");
    params.args.add(llformat("{\"mcpServers\":{\"second_life\":"
                             "{\"type\":\"http\",\"url\":\"http://127.0.0.1:%d/mcp\"}}}",
                             (int)port));

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
    params.args.add(prompt);

    params.files.add(LLProcess::FileParam());                   // stdin, unused
    params.files.add(LLProcess::FileParam().type("pipe"));      // stdout
    params.files.add(LLProcess::FileParam().type("pipe"));      // stderr

    params.autokill = true;   // a turn that is abandoned takes its process along

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

bool LumenAIClaude::running() const
{
    return mProc && mProc->isRunning();
}

void LumenAIClaude::stop()
{
    if (mProc)
    {
        mProc->kill();
        mProc.reset();
    }
}
