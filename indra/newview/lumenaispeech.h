/**
 * @file lumenaispeech.h
 * @brief Talk to the assistant instead of typing: the computer's own speech
 *        recognition, with the words landing in the Assistant's typing box.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * The author, 2026-10-01: a mic button, and *"what I don't want is a system
 * popup or window in front of the viewer"*. So this is NOT the system's
 * dictation (fn fn on a Mac, Win+H on Windows), which puts its own panel on
 * screen; it calls the speech framework directly and nothing is drawn but our
 * own button. The one thing the system shows, and nothing can stop it, is the
 * first-time permission question and the menu-bar microphone dot.
 *
 * macOS: Apple's Speech framework (lumenaispeech_mac.mm). English is recognised
 * on the machine; Danish goes to Apple's servers (checked on the Mac Studio:
 * supportsOnDeviceRecognition false for da-DK).
 * Windows: Whisper (whisper.cpp), on the computer itself (lumenaispeech_win.cpp).
 * Windows' own speech for programs was tried first and given up, measured on
 * the author's laptop, 2026-10-02: its online dictation answered "Unknown"
 * with no words even outside Lumen, while Win+H heard him fine, and the older
 * on-device recogniser heard "in the direction of it" at a confidence of 0.02.
 * Whisper's model is a download the person agrees to first -- the author:
 * *"the installation must be easy and painless, guide the user through, and
 * give the option to say no"* -- so the setup functions below say what is
 * missing, fetch it on request, and remove it again. English only.
 *
 * Everything here is called on the main thread. The framework answers on its
 * own threads; those answers are kept behind a lock and handed over by poll(),
 * which the Assistant window calls every frame while it is listening.
 */

#ifndef LUMEN_AISPEECH_H
#define LUMEN_AISPEECH_H

#include <string>
#include <utility>
#include <vector>

namespace LumenAISpeech
{
    struct Update
    {
        bool        changed  = false;   // text is new since the last poll
        std::string text;               // everything heard so far, this time
        bool        finished = false;   // listening has ended; text is the last word
        std::string error;              // why it ended badly, in plain words; empty when not
        bool        nothing_heard = false;   // finished with no words, nobody's fault (silence)
        std::string note;               // something to tell the person once, not an error
        std::string activity;           // for the status bar while this goes on; empty leaves it
    };

    /**
     * What the recogniser needs before it can listen. On a Mac Apple's is part
     * of the system, so it is always Ready. On Windows, Whisper's model is a
     * one-time download that the person agrees to first.
     */
    enum class Setup { Ready, NotInstalled, Downloading, Failed };

    struct SetupState
    {
        Setup       state = Setup::Ready;
        long long   done  = 0;          // bytes downloaded so far
        long long   total = 0;          // bytes in all
        std::string error;              // why the last attempt failed, in plain words
    };

#if LL_DARWIN || LL_WINDOWS
    /** This platform can do it at all. */
    bool supported();

    /**
     * Start listening. `language` is a locale such as "da-DK", or empty for the
     * computer's own language. One the computer cannot listen in falls back to
     * the computer's own, and that to English, with a note saying so. The first time, macOS asks the user about the
     * microphone and speech recognition; the answer arrives later, so a true
     * return means "started or asking", and poll() says what happened. Windows
     * asks nothing, but starts later too, with the same true return.
     * False with `why` when it cannot even begin.
     */
    bool start(const std::string& language, std::string& why);

    /** Stop listening; the last words still arrive through poll(). */
    void stop();

    /** Stop and throw away whatever was heard. */
    void cancel();

    bool listening();

    /** Called every frame while listening. Also ends a pause or a long take. */
    Update poll();

    /**
     * The languages this computer can listen in, as {tag, name}, sorted by
     * name -- for the choice in Preferences > AI > Assistant. The tag is what
     * LumenAISpeechLanguage holds; empty there means the computer's own.
     */
    std::vector<std::pair<std::string, std::string>> languages();

    /** The computer's own language as a tag ("en_DK", "en-US"), or empty. */
    std::string ownLanguage();
#endif

#if LL_WINDOWS
    SetupState setupState();

    /** Start the download; false with `why` when it cannot begin. */
    bool startSetup(std::string& why);

    /** Stop a download and throw away what came. */
    void cancelSetup();

    /** Delete the downloaded model; the mic asks again next time. */
    void removeSetup();

    /** How big the download is, in words ("148 MB"). */
    std::string setupSize();
#else
    inline SetupState setupState() { return SetupState(); }
    inline bool startSetup(std::string&) { return true; }
    inline void cancelSetup() {}
    inline void removeSetup() {}
    inline std::string setupSize() { return std::string(); }
#endif

#if !(LL_DARWIN || LL_WINDOWS)
    inline bool supported() { return false; }
    inline bool start(const std::string&, std::string& why)
    {
        why = "Talking to the assistant is not available on this computer yet.";
        return false;
    }
    inline void stop() {}
    inline void cancel() {}
    inline bool listening() { return false; }
    inline Update poll() { return Update(); }
    inline std::vector<std::pair<std::string, std::string>> languages() { return {}; }
    inline std::string ownLanguage() { return std::string(); }
#endif
}

#endif // LUMEN_AISPEECH_H
