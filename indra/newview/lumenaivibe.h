/**
 * @file lumenaivibe.h
 * @brief Drive Mistral Vibe, so the Assistant can run on a Mistral subscription.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#ifndef LUMEN_AIVIBE_H
#define LUMEN_AIVIBE_H

#include "llprocess.h"
#include "llsd.h"

#include <functional>

/**
 * Mistral Vibe, run the way Claude Code is: one process per turn, pointed at
 * the viewer's own endpoint, reporting as newline-delimited JSON, resumed by
 * session id for the next turn.
 *
 * What is different, all of it read out of Vibe 2.25.5's own source:
 *
 * - **Its own settings folder.** Vibe reads everything -- config, hooks, MCP
 *   servers, AGENTS.md, sessions -- from VIBE_HOME, and its sign-in is in the
 *   macOS Keychain, which does not depend on that folder. So Lumen gives it a
 *   folder of its own (home()), and the user's ~/.vibe -- their MCP servers,
 *   their hooks, their instructions -- never reaches a model that reads
 *   strangers' IMs.
 * - **Only the viewer's tools**, by `--enabled-tools second_life_*`, which in
 *   one-shot mode removes every other tool, shell and files included.
 * - **`--auto-approve`**, because one-shot mode denies every approval and a
 *   denial ends the whole turn with no answer. With nothing but our tools
 *   left, the viewer's own questions are the approval that matters.
 * - **stdin closed**, or it waits for it: Vibe reads stdin to the end
 *   whenever it is not a terminal. The question goes IN on stdin, from a
 *   file, which also keeps it out of the command line LLProcess logs.
 * - **No end-of-turn line.** The end is the process exiting; an answer is
 *   the assistant messages of this turn, and an error is stderr.
 */
class LumenAIVibe
{
public:
    LumenAIVibe() {}
    ~LumenAIVibe() { stop(); }

    /** Where the CLI is, whether it is there at all. */
    static std::string cliPath();
    static bool        installed();
    /** Where its sign-in helper is: `vibe-acp`, beside the CLI. */
    static std::string acpPath();

    /**
     * Whether Vibe has a Mistral key to use, from the same places Vibe looks:
     * the environment, a `.env` in its settings folder, the macOS Keychain.
     * Asks `security` without `-w`, so the key itself is never read. Cached
     * for a few seconds, because Preferences asks once a second.
     */
    // <Lumen> `fresh` skips the three-second cache: the setup window asks
    // straight after a sign-in, when a cached "no" is known to be stale.
    static bool        signedIn(bool fresh = false);

    /** Why this provider cannot work on this platform at all, or empty. */
    static std::string unavailableHere();

    /** Lumen's own VIBE_HOME. */
    static std::string home();

    /**
     * Start one turn. False sets `why`. `resume` is the session id of the
     * conversation so far, empty for the first turn.
     */
    bool start(const std::string& prompt,
               const std::string& system,
               const std::string& model,
               const std::string& resume,
               U16                port,
               std::string&       why);

    /** One complete JSON line from the child, or false when none is waiting. */
    bool poll(LLSD& out);
    bool lineWaiting();

    /**
     * Ask one question and wait for the answer, from inside a coroutine. For
     * Preferences' Test and the setup window's check. True with the text of
     * the assistant's reply in `answer`.
     */
    bool runToAnswer(const std::string& prompt, const std::string& model, U16 port,
                     F64 seconds, std::string& answer, std::string& why);

    bool running() const;
    void stop();

    /** What the process wrote on stderr, and any stdout line that was not JSON. */
    const std::string& errorText() const { return mLastError; }

private:
    std::string collectError();

    LLProcessPtr mProc;
    std::string  mLastError;
};

/**
 * Signing in with a Mistral account, the way Vibe's own setup does, without
 * a terminal: `vibe-acp` hands back a sign-in page, Lumen opens it in the
 * user's browser, and the helper waits until they have finished there, then
 * keeps the key in the Keychain exactly where `vibe --setup` would have. The
 * password is typed into Mistral's page; Lumen never sees it or the key.
 */
class LumenAIVibeSignIn
{
public:
    ~LumenAIVibeSignIn() { stop(); }

    /**
     * From inside a coroutine. `still_wanted` is asked between waits, so a
     * closed window stops the wait. True when the sign-in completed.
     */
    bool run(const std::function<bool()>& still_wanted, std::string& why);
    void stop();

private:
    bool send(const LLSD& message);
    bool reply(S32 id, F64 seconds, const std::function<bool()>& still_wanted,
               LLSD& out, std::string& why);

    LLProcessPtr mProc;
};

#endif // LUMEN_AIVIBE_H
