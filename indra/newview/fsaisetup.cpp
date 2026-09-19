/**
 * @file fsaisetup.cpp
 * @brief A window that sets Codex up FOR the user, one checked step at a time.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#include "llviewerprecompiledheaders.h"

#include "fsaisetup.h"

#include "llbutton.h"
#include "lltextbox.h"
#include "lldir.h"

namespace
{
    /**
     * What each step runs.
     *
     * All three go through `/bin/sh -c` rather than being executed directly,
     * and the first one has to: it is a pipeline, and a pipe is a shell
     * feature. LLProcess runs an executable, not a command line, so without a
     * shell the `| sh` would arrive as two more arguments to curl.
     */
    const char* command(int step)
    {
        switch (step)
        {
            // OpenAI's own installer, from OpenAI's own domain. Named in the
            // window before the button is pressed.
            case 0: return "curl -fsSL https://chatgpt.com/codex/install.sh | sh";
            // Opens the user's browser. The password is typed into OpenAI's
            // page, never into Lumen, and never passes through this process.
            case 1: return "\"$HOME/.codex/packages/standalone/current/bin/codex\" login";
            case 2: return "\"$HOME/.codex/packages/standalone/current/bin/codex\" app-server daemon start";
            default: return "";
        }
    }

    /** How long to let a step run before saying so. Signing in is a person
        reading a web page, so it gets much longer than a download. */
    F32 patience(int step) { return (step == 1) ? 300.f : 180.f; }
}

FSAISetupFloater::FSAISetupFloater(const LLSD& key) : LLFloater(key) {}

FSAISetupFloater::~FSAISetupFloater()
{
    // A half-finished install is worse than none, so a running step is NOT
    // killed when the window closes -- `autokill` is false and the process is
    // simply let go. `codex login` in particular may be sitting on a browser
    // the user is still typing into.
    mProc.reset();
}

std::string FSAISetupFloater::codexDir()
{
    const char* h = getenv("HOME");
    if (!h || !*h) h = getenv("USERPROFILE");
    if (!h || !*h) return std::string();
    return std::string(h) + gDirUtilp->getDirDelimiter() + ".codex" + gDirUtilp->getDirDelimiter();
}

/**
 * Read off the filesystem every time, never cached.
 *
 * The user may well do a step by hand, or have done it months ago, so the
 * window must never rely on having watched it happen. This is also why there
 * is no "current step" stored anywhere: the state is the three files.
 */
bool FSAISetupFloater::done(EStep step)
{
    const std::string dir = codexDir();
    if (dir.empty()) return false;
    const std::string sep = gDirUtilp->getDirDelimiter();
    switch (step)
    {
        case STEP_INSTALL:
            return gDirUtilp->fileExists(dir + "packages" + sep + "standalone" + sep
                                         + "current" + sep + "bin" + sep + "codex");
        case STEP_SIGNIN:
            return gDirUtilp->fileExists(dir + "auth.json");
        case STEP_START:
            return gDirUtilp->fileExists(dir + "app-server-control" + sep
                                         + "app-server-control.sock");
        default:
            return false;
    }
}

bool FSAISetupFloater::postBuild()
{
    for (int i = 0; i < STEP_COUNT; ++i)
    {
        const std::string n = llformat("do_%d", i + 1);
        if (LLButton* b = findChild<LLButton>(n))
        {
            b->setCommitCallback([this, i](LLUICtrl*, const LLSD&) { onDo((EStep)i); });
        }
    }
    if (LLButton* c = findChild<LLButton>("close_btn"))
    {
        c->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    }
    return true;
}

void FSAISetupFloater::onOpen(const LLSD&)
{
    mFailed = false;
    mNote.clear();
    refresh();
}

void FSAISetupFloater::onDo(EStep step)
{
    if (mRunning != STEP_COUNT) return;     // one at a time
    run(step);
    refresh();
}

void FSAISetupFloater::run(EStep step)
{
    mFailed = false;
    mNote.clear();

    LLProcess::Params p;
    p.executable = "/bin/sh";
    p.args.add("-c");
    p.args.add(command(step));
    // NOT autokill: see the destructor. A download or a browser sign-in that
    // is killed halfway leaves the user worse off than never having started.
    p.autokill = false;

    mProc = LLProcess::create(p);
    if (!mProc)
    {
        mFailed = true;
        mNote   = "Lumen could not start that. Nothing has been changed.";
        return;
    }
    mRunning = step;
    mSince.reset();
    mPoll.reset();
}

void FSAISetupFloater::draw()
{
    // Polling on the frame loop is fine at this rate and needs no timer of its
    // own; the checks are three fileExists calls.
    if (mRunning != STEP_COUNT && mPoll.getElapsedTimeF32() > 0.5f)
    {
        mPoll.reset();

        if (done(mRunning))
        {
            // The check passed, which is the only thing that counts. The
            // process may still be alive -- `codex login` lingers after
            // writing auth.json -- and waiting for it would leave the window
            // saying "working" about something already finished.
            mRunning = STEP_COUNT;
            refresh();
        }
        else if (mProc && !mProc->isRunning())
        {
            // It exited without the thing appearing. Do NOT read an exit code
            // and call that success: what matters is whether the file is
            // there, and it is not.
            mFailed  = true;
            mRunning = STEP_COUNT;
            mNote    = "That finished, but it did not leave what Lumen was looking for. "
                       "Try it once more; if it keeps happening, the panel behind this "
                       "window has the command you can run by hand.";
            refresh();
        }
        else if (mSince.getElapsedTimeF32() > patience(mRunning))
        {
            mNote = "This is taking longer than usual. It is still going -- you can leave "
                    "this window open.";
            refresh();
        }
        else
        {
            refresh();
        }
    }
    LLFloater::draw();
}

void FSAISetupFloater::refresh()
{
    const bool all = done(STEP_INSTALL) && done(STEP_SIGNIN) && done(STEP_START);

    for (int i = 0; i < STEP_COUNT; ++i)
    {
        const bool  ok      = done((EStep)i);
        const bool  running = (mRunning == (EStep)i);
        // Earlier steps must be done first: signing in needs the program, and
        // starting it needs both.
        bool ready = !ok && (mRunning == STEP_COUNT);
        for (int j = 0; j < i && ready; ++j) ready = done((EStep)j);

        if (LLTextBox* m = findChild<LLTextBox>(llformat("mark_%d", i + 1)))
        {
            m->setText(std::string(ok ? "[done]" : running ? "[ ... ]" : "[    ]"));
        }
        if (LLButton* b = findChild<LLButton>(llformat("do_%d", i + 1)))
        {
            b->setEnabled(ready);
            b->setLabel(std::string(ok ? "Done" : running ? "Working..." : "Do this"));
        }
    }

    if (LLTextBox* s = findChild<LLTextBox>("summary"))
    {
        std::string t;
        if (!mNote.empty())
        {
            t = mNote;
        }
        else if (all)
        {
            t = "All three are done. Close this window, then type to your assistant. "
                "Your ChatGPT subscription pays for it, so there is no separate bill.";
        }
        else if (mRunning != STEP_COUNT)
        {
            t = (mRunning == STEP_SIGNIN)
                ? "Your web browser should have opened. Sign in there the way you normally "
                  "do, then come back -- this window notices on its own."
                : "Working. This can take a few minutes; you can leave the window open.";
        }
        else
        {
            t = "Do the steps in order. Each one checks itself when it finishes.";
        }
        s->setText(t);
    }
}
