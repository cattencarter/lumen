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
#include "llviewercontrol.h"
#include "fsaikeys.h"
#include "fsaiclaude.h"
#include "fsaictl.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "lltimer.h"

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
    const char* command(const std::string& provider, int step)
    {
        const bool codex = (provider == FSAIKeys::CODEX);
        switch (step)
        {
            case 0:
                // Both are the vendor's OWN installer, from the vendor's own
                // domain, named in the window before the button is pressed.
                //
                // Claude Code's is the NATIVE installer rather than
                // `npm install -g @anthropic-ai/claude-code`, which the panel
                // used to print. That needs Node.js, which somebody whose only
                // experience of this is the ChatGPT app does not have and
                // should not have to get: the whole step would become "first
                // install a thing you have never heard of".
                return codex ? "curl -fsSL https://chatgpt.com/codex/install.sh | sh"
                             : "curl -fsSL https://claude.ai/install.sh | bash";
            case 1:
                // Opens the user's browser. The password is typed into the
                // vendor's page, never into Lumen.
                return codex
                    ? "\"$HOME/.codex/packages/standalone/current/bin/codex\" login"
                    : "claude auth login";
            case 2:
                return "\"$HOME/.codex/packages/standalone/current/bin/codex\" app-server daemon start";
            default: return "";
        }
    }

    /** How long to let a step run before saying so. Signing in is a person
        reading a web page, so it gets much longer than a download. */
    F32 patience(int step) { return (step == 1) ? 300.f : 180.f; }
}

FSAISetupFloater::FSAISetupFloater(const LLSD& key) : LLFloater(key) {}

/** Claude Code needs no background service, so it stops after two. */
int FSAISetupFloater::steps() const
{
    return (mProvider == FSAIKeys::CODEX) ? 3 : 2;
}

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
bool FSAISetupFloater::done(EStep step) const
{
    if (mProvider != FSAIKeys::CODEX)
    {
        // Claude Code.
        if (step == STEP_INSTALL) return FSAIClaude::installed();
        if (step == STEP_SIGNIN)  return mProved;   // see the header
        return true;                                // no third step
    }

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

void FSAISetupFloater::onOpen(const LLSD& key)
{
    // Which provider, from whoever opened it. Codex unless told otherwise, so
    // an old call site cannot silently become a Claude Code window.
    mProvider = key.has("provider") ? key["provider"].asString() : FSAIKeys::CODEX;
    mProved = false;
    mFailed = false;
    mNote.clear();
    refresh();
}

void FSAISetupFloater::signedIn(bool ok)
{
    mProved = ok;
    mFailed = !ok;
    mNote   = ok ? std::string()
                 : "It installed, but signing in did not take. Try step 2 again "
                   "   the browser window has to be finished before it counts.";
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
    p.args.add(command(mProvider, step));
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
        else if (mProc && !mProc->isRunning()
                 && mProvider != FSAIKeys::CODEX && mRunning == STEP_SIGNIN)
        {
            // `claude auth login` has exited, and there is no file to look at:
            // Claude Code keeps its credentials in the macOS keychain. So the
            // step is proved the only honest way, by asking it a real question
            // through the viewer's own tools and seeing an answer come back.
            mRunning = STEP_COUNT;
            mNote = "Checking...";
            refresh();

            const std::string model = gSavedSettings.getString("LumenAIClaudeCodeModel");
            const U16 port = FSAIControl::instance().port();
            LLHandle<LLFloater> h = getHandle();
            LLCoros::instance().launch("FSAISetupClaude", [h, model, port]()
            {
                FSAIClaude cc;
                std::string why;
                bool ok = false;
                if (cc.start("Reply with the single word: ok", std::string(),
                             model, std::string(), port, why))
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
                        ok = !msg["is_error"].asBoolean();
                        break;
                    }
                    cc.stop();
                }
                FSAISetupFloater* f = dynamic_cast<FSAISetupFloater*>(h.get());
                if (!f) return;
                f->signedIn(ok);
            });
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
    const int  n   = steps();
    bool all = true;
    for (int i = 0; i < n; ++i) all = all && done((EStep)i);

    for (int i = 0; i < STEP_COUNT; ++i)
    {
        const bool  ok      = done((EStep)i);
        const bool  running = (mRunning == (EStep)i);
        // Earlier steps must be done first: signing in needs the program, and
        // starting it needs both.
        bool ready = !ok && (mRunning == STEP_COUNT);
        for (int j = 0; j < i && ready; ++j) ready = done((EStep)j);

        // A step this provider does not have is hidden rather than ticked:
        // Claude Code needs no background service, and a greyed third row
        // reads as something that failed.
        const bool applies = (i < n);
        for (const char* w : { "mark_%d", "title_%d", "desc_%d", "do_%d" })
        {
            if (LLView* v = findChild<LLView>(llformat(w, i + 1))) v->setVisible(applies);
        }
        if (!applies) continue;

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

    // The wording is the whole product for this window, and the two providers
    // are not the same story: one is a 230 MB download and a background
    // service, the other is a small program and nothing else.
    const bool codex = (mProvider == FSAIKeys::CODEX);
    setTitle(codex ? "Use your ChatGPT subscription" : "Use your Claude subscription");
    if (LLTextBox* t = findChild<LLTextBox>("intro"))
    {
        t->setText(std::string(codex
            ? "Lumen can use the ChatGPT subscription you already pay for, instead of "
              "asking you for a paid API key. Three things have to happen first, and "
              "Lumen can do all three for you."
            : "Lumen can use the Claude subscription you already pay for, instead of "
              "asking you for a paid API key. Two things have to happen first, and "
              "Lumen can do both for you."));
    }
    if (LLTextBox* t = findChild<LLTextBox>("title_1"))
        t->setText(std::string("1. Get the program that does the talking"));
    if (LLTextBox* t = findChild<LLTextBox>("desc_1"))
    {
        t->setText(std::string(codex
            ? "Lumen downloads this from OpenAI, at chatgpt.com. It is about 230 MB and "
              "you only ever do it once."
            : "Lumen downloads this from Anthropic, at claude.ai. You only ever do it "
              "once."));
    }
    if (LLTextBox* t = findChild<LLTextBox>("title_2"))
    {
        t->setText(std::string(codex ? "2. Sign in with your ChatGPT account"
                                     : "2. Sign in with your Claude account"));
    }
    if (LLTextBox* t = findChild<LLTextBox>("desc_2"))
    {
        t->setText(std::string(codex
            ? "Your web browser opens and you sign in the way you always do. You type "
              "your password into OpenAI's own page. Lumen never sees it and never keeps it."
            : "Your web browser opens and you sign in the way you always do. You type "
              "your password into Anthropic's own page. Lumen never sees it and never "
              "keeps it. When you come back, Lumen asks it a question to make sure."));
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
