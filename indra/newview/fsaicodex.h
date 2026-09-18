/**
 * @file fsaicodex.h
 * @brief Talking to OpenAI's Codex app-server, so the Assistant can run on a
 *        ChatGPT subscription instead of a paid API key.
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
#ifndef FS_AI_CODEX_H
#define FS_AI_CODEX_H

#include "llsd.h"
#include <string>

/**
 * A JSON-RPC connection to the Codex app-server.
 *
 * **It is a WebSocket over a unix domain socket**, which is the single fact
 * that cost an afternoon. Plain JSON on that socket is accepted and answered
 * with nothing at all -- no error, no close, just silence -- because the
 * server is waiting for an HTTP Upgrade. Guessing produced four wrong framings
 * in a row; reading `codex-rs/app-server-transport/src/transport/unix_socket.rs`
 * in openai/codex produced it in one, from the word `tokio_tungstenite`.
 *
 * OpenAI documents this interface for exactly this use: *"Use it when you want
 * a deep integration inside your own product: authentication, conversation
 * history, approvals, and streamed agent events."* Authentication is theirs --
 * the user signs in with `codex login`, and Lumen never sees a credential.
 *
 * **Non-blocking on purpose.** This runs in the viewer's own process, so a
 * blocking read would freeze the frame loop (Findings 12, 17 and 21 are all
 * that mistake). `poll()` returns what has arrived and nothing more; the
 * caller suspends its coroutine between calls.
 */
#include <vector>

class FSAICodex
{
public:
    FSAICodex();
    ~FSAICodex();

    /** Where the daemon listens, whether it is there, and whether Codex is installed. */
    static std::string socketPath();
    static bool        socketPresent();
    static std::string cliPath();
    static bool        cliInstalled();

    /**
     * The plugins the user's own Codex has switched on, by name.
     *
     * Read from `~/.codex/config.toml` rather than listed here, so a plugin
     * that did not exist when this was written is still found. Only the
     * `[plugins."NAME"]` headers are wanted, which is a line match rather than
     * TOML parsing -- and a file that cannot be read returns nothing, which is
     * the same as today.
     */
    static std::vector<std::string> enabledPlugins();

    /** Connect and perform the WebSocket handshake. False sets `why`. */
    bool connect(std::string& why);
    void close();
    bool connected() const { return mFd >= 0; }

    /** One JSON-RPC message out, as a masked text frame. */
    bool send(const LLSD& message);

    /** One complete message in, or false when nothing has arrived yet. */
    bool poll(LLSD& out);

private:
    bool handshake(std::string& why);
    bool drain();                      // read whatever is waiting, never blocking
    bool frame(std::string& payload);  // one text frame out of mIn, if complete

    S32         mFd;
    std::string mIn;
    bool        mUpgraded;
};

#endif // FS_AI_CODEX_H
