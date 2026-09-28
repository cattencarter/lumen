/**
 * @file lumenaiclaude.h
 * @brief Drive Claude Code, so the Assistant can run on a Claude subscription.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#ifndef LUMEN_AICLAUDE_H
#define LUMEN_AICLAUDE_H

#include "llprocess.h"
#include "llsd.h"

#include <string>
#include <vector>

/**
 * One turn of Claude Code, as a child process we read a line at a time.
 *
 * **A process per turn, not a process kept alive**, which is the whole reason
 * this is simpler than the Codex client beside it. Codex needed a socket, a
 * WebSocket handshake and a thread id, because its app-server holds the
 * conversation. Claude Code holds the conversation itself and hands back a
 * `session_id`; passing it to `--resume` on the next turn continues where it
 * left off. Checked before building on it: told a colour in one call and asked
 * for it in the next, it answered correctly, same session id.
 *
 * So there is no long-lived pipe to keep fed, nothing to reconnect, and a turn
 * that goes wrong takes its process with it.
 *
 * **`--output-format stream-json` means this path can stream**, which the HTTP
 * providers cannot: their layer only hands back a whole response
 * (`HttpHandler` has nothing but `onCompleted`). Here the answer arrives as
 * `content_block_delta` events and can be written into the window as it comes.
 */
class LumenAIClaude
{
public:
    LumenAIClaude() {}
    ~LumenAIClaude() { stop(); }

    /** Where the CLI is, whether it is there at all. */
    static std::string cliPath();
    static bool        installed();

    /**
     * <Lumen> Installed, but too old for Lumen to start safely.
     *
     * start() runs Claude Code with `--restricted`, which is what keeps it
     * from handing the model the user's shell and files. A copy that predates
     * the flag refuses to start at all -- "error: unknown option
     * '--restricted'" -- while the panel said it was ready, because being
     * installed was the whole test. So the copy is asked whether it has the
     * flag, from its own `--help`, rather than compared against a version
     * number somebody would have to keep right. Once per file: an update
     * replaces the file, and the answer is asked again.
     *
     * False when the answer could not be read, so a broken probe never
     * reports a working copy as too old. `version`, when given, is what
     * `--version` said.
     */
    static bool        tooOld(std::string* version = nullptr);
    /** What to tell somebody whose copy is too old, in words they can act on. */
    static std::string tooOldText(const std::string& version, bool inPanel = false);
    /**
     * How to update THIS copy, which depends on how it was installed: the
     * native installer updates itself with `claude update`, a Homebrew cask
     * only through brew, and an npm install through npm. `forPerson` is what
     * to type in Terminal; otherwise the command the setup window runs, with
     * full paths, since a viewer started from the Dock has no useful PATH.
     */
    static std::string updateCommand(bool forPerson);

    /**
     * Why this provider cannot work on this platform at all, or empty when it
     * can. On Windows the CLI is looked for under Unix names and the setup
     * runs /bin/sh, so "not installed" would be the wrong reason to give.
     */
    static std::string unavailableHere();

    /**
     * Start one turn. False sets `why`.
     *
     * `resume` continues an earlier conversation and may be empty for the
     * first. `system` is Lumen's own prompt, appended to Claude Code's rather
     * than replacing it, so its own conventions for calling tools survive.
     */
    bool start(const std::string& prompt,
               const std::string& system,
               const std::string& model,
               const std::string& resume,
               U16                port,
               std::string&       why);

    /** One complete JSON line from the child, or false when none is waiting. */
    bool poll(LLSD& out);

    /**
     * Start a turn and wait for its `result` line, from inside a coroutine.
     *
     * For the one-question checks (Preferences' Test, the setup window's
     * sign-in check), which had each written this loop themselves and each
     * thrown away a result line read after the process had exited. True with
     * the result line in `result`; false with a reason in `why` -- including
     * what Claude Code wrote on stderr, when it said anything there.
     */
    bool runToResult(const std::string& prompt, const std::string& model, U16 port,
                     F64 seconds, LLSD& result, std::string& why);

    bool running() const;
    void stop();

    /**
     * What the process wrote on stderr, as of the last stop(). Empty when it
     * said nothing there. A failure Claude Code reports only on stderr -- a
     * bad argument, a sign-in problem -- otherwise leaves no reason anywhere.
     */
    const std::string& errorText() const { return mLastError; }

private:
    /** Whether a whole line is waiting on stdout. */
    bool lineWaiting();
    /** Move whatever stderr holds into mLastError; returns what was new. */
    std::string collectError();

    LLProcessPtr mProc;
    std::string  mLastError;
};

#endif // LUMEN_AICLAUDE_H
