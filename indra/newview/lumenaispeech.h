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
 * macOS only for now (Apple's Speech framework, lumenaispeech_mac.mm). On a Mac
 * English is recognised on the machine; Danish goes to Apple's servers (checked
 * on the Mac Studio: supportsOnDeviceRecognition false for da-DK).
 *
 * Everything here is called on the main thread. The framework answers on its
 * own threads; those answers are kept behind a lock and handed over by poll(),
 * which the Assistant window calls every frame while it is listening.
 */

#ifndef LUMEN_AISPEECH_H
#define LUMEN_AISPEECH_H

#include <string>

namespace LumenAISpeech
{
    struct Update
    {
        bool        changed  = false;   // text is new since the last poll
        std::string text;               // everything heard so far, this time
        bool        finished = false;   // listening has ended; text is the last word
        std::string error;              // why it ended badly, in plain words; empty when not
        bool        nothing_heard = false;   // finished with no words, nobody's fault (silence)
    };

#if LL_DARWIN
    /** This platform can do it at all. */
    bool supported();

    /**
     * Start listening. `language` is a locale such as "da-DK", or empty for the
     * computer's own language. The first time, macOS asks the user about the
     * microphone and speech recognition; the answer arrives later, so a true
     * return means "started or asking", and poll() says what happened.
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
#else
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
#endif
}

#endif // LUMEN_AISPEECH_H
