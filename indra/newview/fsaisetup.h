/**
 * @file fsaisetup.h
 * @brief A window that sets Codex up FOR the user, one checked step at a time.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * The Preferences panel used to print a shell command with a Copy button and
 * ask the user to find Terminal, paste it, press Return and come back. The
 * author's objection, and it is the right one: *"jeg synes ikke codex
 * installationen er nem for nye mennesker at gore"*, for *"folk der ikke ved
 * noget om computere men blot har chatgpt appen installeret"*.
 *
 * Somebody who has only ever used the ChatGPT app does not know what Terminal
 * is, and telling them to open it is the same barrier this whole project
 * exists to remove -- moved out of the viewer and onto their desk. So the
 * viewer runs the three commands itself and checks after each one.
 *
 * **What that means, stated plainly because it is a real decision:** pressing
 * the first button makes Lumen download and run OpenAI's own installer script.
 * The window says so, in those words, before the button is pressed. Nothing
 * runs without a click.
 */

#ifndef FS_AISETUP_H
#define FS_AISETUP_H

#include "llfloater.h"
#include "llprocess.h"
#include "llframetimer.h"

class LLButton;
class LLTextBox;

class FSAISetupFloater : public LLFloater
{
public:
    FSAISetupFloater(const LLSD& key);
    ~FSAISetupFloater() override;

    bool postBuild() override;
    /** The asynchronous Claude Code check reports here. */
    void signedIn(bool ok);
    void onOpen(const LLSD& key) override;
    void draw() override;

private:
    /** The things that have to be true, in the order they become true.
     *  Codex needs all three; Claude Code needs no background service and
     *  stops after two. */
    enum EStep { STEP_INSTALL = 0, STEP_SIGNIN, STEP_START, STEP_COUNT };

    /** Which provider this window is setting up: FSAIKeys::CODEX or CLAUDECODE. */
    std::string mProvider;
    /** How many of the three apply to it. */
    int steps() const;

    /**
     * Is this step already satisfied?
     *
     * Read off the filesystem for everything it CAN be read for. The one
     * exception is Claude Code being signed in: it keeps its credentials in
     * the macOS keychain rather than in a file, so there is nothing to stat.
     * Guessing from the keychain's item name would be reading an
     * implementation detail of somebody else's program, so instead that step
     * is proved the only honest way   by asking Claude Code a real question
     * and seeing it answer, which is stronger evidence than any file.
     */
    bool done(EStep step) const;
    /** Where Codex keeps its things. Empty when the system reports no home. */
    static std::string codexDir();

    void run(EStep step);
    void refresh() override;
    void onDo(EStep step);

    LLProcessPtr  mProc;
    EStep         mRunning  = STEP_COUNT;   // STEP_COUNT == nothing running
    LLFrameTimer  mPoll;
    LLFrameTimer  mSince;                   // how long the current step has run
    bool          mFailed   = false;
    std::string   mNote;
    /** Claude Code answered since this window opened. See done(). */
    bool          mProved   = false;
};

#endif // FS_AISETUP_H
