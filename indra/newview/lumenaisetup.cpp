/**
 * @file lumenaisetup.cpp
 * @brief A window that sets Codex up FOR the user, one checked step at a time.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#include "llviewerprecompiledheaders.h"

#include "lumenaisetup.h"

#include "llbutton.h"
#include "lltextbox.h"
#include "lldir.h"
#include "llviewercontrol.h"
#include "lumenaikeys.h"
#include "lumenaiclaude.h"
#include "lumenaivibe.h"     // <Lumen>
#include "lumenaicodex.h"
#include "lumenaictl.h"
#include "lumenaiwin.h"      // <Lumen> PowerShell and Windows arguments
#include "llcoros.h"
#include "lleventcoro.h"
#include "lltimer.h"

namespace
{
    /**
     * What each step runs.
     *
     * On Windows each goes through PowerShell instead, with no window --
     * the installers are PowerShell one-liners there (`irm ... | iex`).
     *
     * On macOS all three go through `/bin/sh -c` rather than being executed directly,
     * and the first one has to: it is a pipeline, and a pipe is a shell
     * feature. LLProcess runs an executable, not a command line, so without a
     * shell the `| sh` would arrive as two more arguments to curl.
     */
    std::string command(const std::string& provider, int step)
    {
        const bool codex = (provider == LumenAIKeys::CODEX);
#if LL_WINDOWS
        // <Lumen> On Windows each step is a PowerShell command (see run()),
        // and each installer is the vendor's own install.ps1 -- all three read
        // and run in a Windows 11 VM on 2026-09-30 (Findings 43). None needs
        // administrator rights, and nothing else is installed first: uv, for
        // Vibe, fetches its own Python. Codex's third step is not a command
        // on Windows at all: Lumen starts its own server (run()).
        // The two sign-ins get EMPTY input (`$null |`), as they do on the Mac:
        // the console they would otherwise read has no window, and a program
        // that thinks a person is at the keyboard may wait at a menu nobody
        // can see. With nothing to read they just open the browser.
        if (provider == LumenAIKeys::VIBE)
        {
            return (step == 0)
                ? std::string("irm https://astral.sh/uv/install.ps1 | iex; "
                              "& \"$env:USERPROFILE\\.local\\bin\\uv.exe\" tool install mistral-vibe")
                : std::string();
        }
        if (codex)
        {
            if (step == 0) return "irm https://chatgpt.com/codex/install.ps1 | iex";
            if (step == 1) return "$null | & \"" + LumenAICodex::cliPath() + "\" login";
            return std::string();
        }
        if (step == 0) return "irm https://claude.ai/install.ps1 | iex";
        if (step == 1)
        {
            const std::string cli = LumenAIClaude::cliPath();
            return cli.empty()
                ? std::string("$null | & \"$env:USERPROFILE\\.local\\bin\\claude.exe\" auth login")
                : "$null | & \"" + cli + "\" auth login";
        }
        return std::string();
#else
        // <Lumen> Mistral Vibe's own installer: uv from Astral if it is missing
        // (checksum-checked by the script), then `uv tool install mistral-vibe`.
        // Signing in is not a command at all -- see run().
        if (provider == LumenAIKeys::VIBE)
        {
            return (step == 0) ? std::string("curl -LsSf https://mistral.ai/vibe/install.sh | bash")
                               : std::string();
        }
        // </Lumen>
        // <Lumen> Step 1 for Claude Code ran a bare `claude`, which the shell
        // resolves on PATH -- and a viewer launched from the Dock has launchd's
        // PATH, where none of the five places the installer puts it are
        // (lumenaiclaude.cpp, cliPath). The shell answered "command not found"
        // at once, the step read that as finished, and the browser sign-in it
        // promised never opened. Use the path the provider itself found.
        if (!codex && step == 1)
        {
            const std::string cli = LumenAIClaude::cliPath();
            return cli.empty() ? std::string("claude auth login")
                               : "\"" + cli + "\" auth login";
        }
        // A copy too old for Lumen is updated by its own updater, not
        // reinstalled over: it keeps the sign-in, and it is the command
        // Anthropic documents for exactly this.
        if (!codex && step == 0 && LumenAIClaude::tooOld())
        {
            return LumenAIClaude::updateCommand(false);
        }
        // </Lumen>
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
#endif
    }

    /** How long to let a step run before saying so. Signing in is a person
        reading a web page, so it gets much longer than a download. */
    F32 patience(int step) { return (step == 1) ? 300.f : 180.f; }
}

LumenAISetupFloater::LumenAISetupFloater(const LLSD& key) : LLFloater(key) {}

/** Claude Code needs no background service, so it stops after two. */
int LumenAISetupFloater::steps() const
{
    return (mProvider == LumenAIKeys::CODEX) ? 3 : 2;   // Vibe, like Claude Code, needs two
}

std::string LumenAISetupFloater::unavailableHere() const
{
    if (mProvider == LumenAIKeys::VIBE) return LumenAIVibe::unavailableHere();   // <Lumen>
    return (mProvider == LumenAIKeys::CODEX) ? LumenAICodex::unavailableHere()
                                             : LumenAIClaude::unavailableHere();
}

LumenAISetupFloater::~LumenAISetupFloater()
{
    // A half-finished install is worse than none, so a running step is NOT
    // killed when the window closes -- `autokill` is false and the process is
    // simply let go. `codex login` in particular may be sitting on a browser
    // the user is still typing into.
    mProc.reset();
}

std::string LumenAISetupFloater::codexDir()
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
bool LumenAISetupFloater::done(EStep step) const
{
    // <Lumen> Mistral Vibe: signed in is the same test Vibe makes itself -- a
    // key in its settings folder or in the Keychain under its own name (its
    // auth_state), asked without reading the key.
    if (mProvider == LumenAIKeys::VIBE)
    {
        if (step == STEP_INSTALL) return LumenAIVibe::installed();
        if (step == STEP_SIGNIN)  return LumenAIVibe::signedIn();
        return true;
    }
    // </Lumen>
    if (mProvider != LumenAIKeys::CODEX)
    {
        // Claude Code.
        // <Lumen> Installed is not done if the copy is too old to start.
        if (step == STEP_INSTALL) return LumenAIClaude::installed() && !LumenAIClaude::tooOld();
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
                                         + "current" + sep + "bin" + sep
                                         + LumenAIWin::exe("codex"));   // <Lumen> .exe on Windows
        case STEP_SIGNIN:
            return gDirUtilp->fileExists(dir + "auth.json");
        case STEP_START:
            // Somebody listening, not a file. A socket file outlives the
            // process that made it, and a stale one marked this step done and
            // greyed out its button -- the only fix short of Terminal.
            return LumenAICodex::listening();
        default:
            return false;
    }
}

bool LumenAISetupFloater::postBuild()
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

void LumenAISetupFloater::onOpen(const LLSD& key)
{
    // Which provider, from whoever opened it. Codex unless told otherwise, so
    // an old call site cannot silently become a Claude Code window.
    mProvider = key.has("provider") ? key["provider"].asString() : LumenAIKeys::CODEX;
    mProved = false;
    mFailed = false;
    // Said at once rather than after a button press, where it applies.
    mNote = unavailableHere();
    refresh();
}

void LumenAISetupFloater::signedIn(bool ok, const std::string& why)
{
    mProved = ok;
    mFailed = !ok;
    mNote   = ok ? std::string()
                 : "It installed, but signing in did not take. Try step 2 again "
                   "   the browser window has to be finished before it counts.";
    // What Claude Code itself said, when it said anything: "did not take" with
    // no reason sends somebody round the same step again for nothing.
    if (!ok && !why.empty()) mNote += "\n\n" + why.substr(0, 300);
    refresh();
}

// <Lumen>
void LumenAISetupFloater::vibeSignedIn(bool ok, const std::string& why)
{
    // <Lumen> Asked afresh: the check below and every one after it read a
    // three-second cache, and a sign-in that finished inside those three
    // seconds -- the browser already signed in to Mistral -- left the cached
    // "no" in place. Nothing polls once the step is over, so step 2 went on
    // looking undone, and the author pressed it again for a second browser
    // page (2026-09-30).
    if (ok) LumenAIVibe::signedIn(true);
    mRunning = STEP_COUNT;
    mFailed  = !ok && !done(STEP_SIGNIN);
    mNote    = mFailed
        ? "Signing in did not finish. Press the button again -- the browser page has to be "
          "completed before it counts." + (why.empty() ? std::string() : "\n\n" + why.substr(0, 300))
        : std::string();
    refresh();
}
// </Lumen>

void LumenAISetupFloater::onDo(EStep step)
{
    if (mRunning != STEP_COUNT) return;     // one at a time
    run(step);
    refresh();
}

void LumenAISetupFloater::run(EStep step)
{
    mFailed = false;
    mNote.clear();

    // Every step goes through /bin/sh, which Windows does not have, so there
    // it could only ever fail with "could not start that" and no reason.
    const std::string here = unavailableHere();
    if (!here.empty())
    {
        mFailed = true;
        mNote   = here;
        return;
    }

    // <Lumen> Mistral Vibe signs in through its own helper, which gives Lumen
    // the page to open and waits until the person is done there. Not a shell
    // command: its own setup is a full-screen terminal program.
    if (mProvider == LumenAIKeys::VIBE && step == STEP_SIGNIN)
    {
        mProc.reset();   // the finished install, which draw() would read as this step failing
        mRunning = step;
        mSince.reset();
        mPoll.reset();
        LLHandle<LLFloater> h = getHandle();
        LLCoros::instance().launch("LumenAIVibeSignIn", [h]()
        {
            LumenAIVibeSignIn signin;
            std::string why;
            const bool ok = signin.run([h]() { return !h.isDead(); }, why);
            if (LumenAISetupFloater* f = dynamic_cast<LumenAISetupFloater*>(h.get()))
            {
                f->vibeSignedIn(ok, why);
            }
        });
        return;
    }
    // </Lumen>

    // <Lumen> Codex on Windows: the third step is Lumen's own server, not a
    // daemon to start from a shell. draw() polls listening() for it as it
    // does for the daemon.
#if LL_WINDOWS
    if (mProvider == LumenAIKeys::CODEX && step == STEP_START)
    {
        mProc.reset();
        if (!LumenAICodex::startService() && !LumenAICodex::listening())
        {
            mFailed = true;
            mNote   = "Lumen could not start Codex. Try once more in a minute; if it keeps "
                      "happening, check that step 2 (signing in) is ticked.";
            return;
        }
        mRunning = step;
        mSince.reset();
        mPoll.reset();
        return;
    }
#endif
    // </Lumen>

    LLProcess::Params p;
#if LL_WINDOWS
    // <Lumen> Windows PowerShell, by its full path, with the command as one
    // argument, and no window -- as on the Mac, this window says what is
    // happening and the browser does the signing in. (It was a visible
    // PowerShell window at first; the author: "it doesn't sound all that
    // user friendly".)
    p.executable = LumenAIWin::powershell();
    p.hidden = true;
    p.args.add("-NoProfile");
    p.args.add("-NonInteractive");   // a question with no window to answer it fails, not waits
    p.args.add("-ExecutionPolicy");
    p.args.add("Bypass");
    p.args.add("-Command");
    p.args.add(LumenAIWin::arg(command(mProvider, step)));
#else
    p.executable = "/bin/sh";
    p.args.add("-c");
    p.args.add(command(mProvider, step));
#endif
    // NOT autokill: see the destructor. A download or a browser sign-in that
    // is killed halfway leaves the user worse off than never having started.
    p.autokill = false;
    // <Lumen> From the home folder, never from wherever the viewer happens to
    // run. Codex's background service keeps the folder it was started in, and
    // the viewer runs inside its own app bundle -- so the next Lumen update
    // replaced that folder under the running service, and every conversation
    // after it failed with "failed to load configuration: No such file or
    // directory" until the service was restarted by hand. Found 2026-09-29,
    // after a rebuild did exactly what an update does.
    {
        const char* home = getenv("HOME");
        if (!home || !*home) home = getenv("USERPROFILE");   // <Lumen> Windows
        p.cwd = (home && *home) ? std::string(home)
                                : gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "");
    }
    // </Lumen>

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

void LumenAISetupFloater::draw()
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
                 && mProvider == LumenAIKeys::CLAUDECODE && mRunning == STEP_SIGNIN)
        {
            // `claude auth login` has exited, and there is no file to look at:
            // Claude Code keeps its credentials in the macOS keychain. So the
            // step is proved the only honest way, by asking it a real question
            // through the viewer's own tools and seeing an answer come back.
            mRunning = STEP_COUNT;
            mNote = "Checking...";
            refresh();

            const std::string model = gSavedSettings.getString("LumenAIClaudeCodeModel");
            // The question needs no tools, but Claude Code is still pointed at
            // the endpoint, and with nothing started that is port 0. Opening it
            // is harmless when it is already open, and the check does not
            // depend on it succeeding.
            LumenAIControl& ctl = LumenAIControl::instance();
            if (!ctl.isRunning()) ctl.start();
            const U16 port = ctl.port();
            LLHandle<LLFloater> h = getHandle();
            LLCoros::instance().launch("LumenAISetupClaude", [h, model, port]()
            {
                LumenAIClaude cc;
                LLSD result;
                std::string why;
                bool ok = false;
                if (cc.runToResult("Reply with the single word: ok", model, port, 120.0,
                                   result, why))
                {
                    ok = !result["is_error"].asBoolean();
                    if (!ok) why = result["result"].asString();
                }
                LumenAISetupFloater* f = dynamic_cast<LumenAISetupFloater*>(h.get());
                if (!f) return;
                f->signedIn(ok, why);
            });
        }
        else if (mProc && !mProc->isRunning())
        {
            // It exited without the thing appearing. Do NOT read an exit code
            // and call that success: what matters is whether the file is
            // there, and it is not.
            mFailed  = true;
            mNote    = "That finished, but it did not leave what Lumen was looking for. "
                       "Try it once more; if it keeps happening, the panel behind this "
                       "window has the command you can run by hand.";
            // <Lumen> An update that ran and left a copy still too old -- a
            // pinned version, a Homebrew cask brew would not upgrade -- says
            // exactly what to type, because the panel no longer carries
            // commands and "the panel behind this window" would send them to
            // look for one that is not there.
            std::string version;
            if (mProvider == LumenAIKeys::CLAUDECODE && mRunning == STEP_INSTALL
                && LumenAIClaude::installed() && LumenAIClaude::tooOld(&version))
            {
                mNote = "That finished, but Claude Code is still "
                      + (version.empty() ? std::string("too old") : version + ", which is too old")
                      + " for Lumen. Update it yourself: open Terminal, type  "
                      + LumenAIClaude::updateCommand(true)
                      + "  and press Return. Then come back and press Test.";
            }
            // </Lumen>
            mRunning = STEP_COUNT;
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

void LumenAISetupFloater::refresh()
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
    const bool codex = (mProvider == LumenAIKeys::CODEX);
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
    // <Lumen> An installed copy that is too old gets an update, said as one.
    std::string old_version;
    const bool claude_too_old = (mProvider == LumenAIKeys::CLAUDECODE)
                             && LumenAIClaude::tooOld(&old_version);
    if (LLTextBox* t = findChild<LLTextBox>("title_1"))
        t->setText(std::string(claude_too_old ? "1. Update the program that does the talking"
                                              : "1. Get the program that does the talking"));
    if (LLTextBox* t = findChild<LLTextBox>("desc_1"))
    {
        t->setText(std::string(codex
            ? "Lumen downloads this from OpenAI, at chatgpt.com. It is about 230 MB and "
              "you only ever do it once."
            : claude_too_old
            ? "Your copy of Claude Code" + (old_version.empty() ? std::string()
                                                               : " (" + old_version + ")")
              + " is too old for Lumen. This updates it from Anthropic, and keeps your "
                "sign-in."
            : std::string("Lumen downloads this from Anthropic, at claude.ai. You only ever "
                          "do it once.")));
    }
    // </Lumen>
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
    // <Lumen> Mistral Vibe's own story: a small program, and a Mistral account
    // -- a free one works, so "subscription" would promise a bill it may not have.
    const bool vibe = (mProvider == LumenAIKeys::VIBE);
    if (vibe)
    {
        setTitle("Use your Mistral account");
        if (LLTextBox* t = findChild<LLTextBox>("intro"))
            t->setText(std::string("Lumen can use Mistral Vibe with your Mistral account -- a free "
                                   "one works too -- instead of a key you paste. Two things have "
                                   "to happen first, and Lumen can do both for you."));
        if (LLTextBox* t = findChild<LLTextBox>("desc_1"))
            t->setText(std::string("Lumen downloads Mistral Vibe with Mistral's own installer, from "
                                   "mistral.ai. It also fetches uv, the small tool it installs "
                                   "with, if you do not have it. You only ever do this once."));
        if (LLTextBox* t = findChild<LLTextBox>("title_2"))
            t->setText(std::string("2. Sign in with your Mistral account"));
        if (LLTextBox* t = findChild<LLTextBox>("desc_2"))
            t->setText(std::string("Your web browser opens on Mistral's own page and you sign in "
                                   "there. Lumen never sees your password, and the key Mistral "
                                   "hands Vibe is kept in your computer's own password store."));
    }
    // </Lumen>

    if (LLTextBox* s = findChild<LLTextBox>("summary"))
    {
        std::string t;
        if (!mNote.empty())
        {
            t = mNote;
        }
        else if (all)
        {
            // The one line here that did not ask which provider it was about,
            // so a Claude Code user was told three steps and ChatGPT.
            t = codex
                ? "All three are done. Close this window, then type to your assistant. "
                  "Your ChatGPT subscription pays for it, so there is no separate bill."
                : vibe   // <Lumen>
                ? "Both are done. Close this window, then type to your assistant. Your "
                  "Mistral account covers it, within your plan's own limits."
                : "Both are done. Close this window, then type to your assistant. "
                  "Your Claude subscription pays for it, so there is no separate bill.";
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
