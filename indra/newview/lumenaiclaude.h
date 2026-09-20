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

    bool running() const;
    void stop();

private:
    LLProcessPtr mProc;
};

#endif // LUMEN_AICLAUDE_H
