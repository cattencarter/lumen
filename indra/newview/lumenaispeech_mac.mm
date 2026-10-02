/**
 * @file lumenaispeech_mac.mm
 * @brief Apple's Speech framework behind the Assistant's mic button.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * Compiled with ARC (-fobjc-arc, set for this one file in CMakeLists.txt), so
 * the Objective-C objects below are released by the compiler, not by hand.
 * No viewer headers on purpose: this file only turns sound into words, and
 * lumenaichat.cpp decides what to do with them.
 */

#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <Speech/Speech.h>

#include "lumenaispeech.h"

#include <algorithm>
#include <atomic>
#include <mutex>

namespace
{
    // What the framework has said, from its own threads. Read by poll().
    struct Heard
    {
        std::mutex  m;
        std::string text;
        bool        changed      = false;
        bool        final_result = false;   // the framework says this is the last word
        std::string error;
        double      last_change  = 0.0;
        bool        any          = false;   // anything at all heard this time
        bool        silence      = false;   // Apple said "no speech detected"
    };
    Heard sHeard;

    enum Phase { IDLE, ASKING, LISTENING, STOPPING };
    Phase       sPhase = IDLE;
    double      sStarted = 0.0;
    double      sStoppedAt = 0.0;
    std::string sLanguage;

    // Set by the permission callbacks, which run on their own threads:
    // 0 waiting, 1 both granted, -1 speech refused, -2 microphone refused.
    std::atomic<int> sAuth{ 0 };

    AVAudioEngine*                         sEngine     = nil;
    SFSpeechRecognizer*                    sRecognizer = nil;
    SFSpeechAudioBufferRecognitionRequest* sRequest    = nil;
    SFSpeechRecognitionTask*               sTask       = nil;

    // How long a pause ends it, how long one go may last (Apple's servers stop
    // at about a minute), and how long to wait for a first word.
    const double PAUSE_ENDS  = 1.8;
    const double LONGEST     = 55.0;
    const double FIRST_WORD  = 10.0;
    const double FINAL_GRACE = 4.0;   // after stop(), for the last word to come back

    double now() { return CFAbsoluteTimeGetCurrent(); }

    const char* SPEECH_REFUSED =
        "Lumen is not allowed to use speech recognition. You can allow it in System "
        "Settings > Privacy & Security > Speech Recognition.";
    const char* MIC_REFUSED =
        "Lumen is not allowed to use the microphone. You can allow it in System "
        "Settings > Privacy & Security > Microphone.";

    void resetHeard()
    {
        std::lock_guard<std::mutex> lock(sHeard.m);
        sHeard.text.clear();
        sHeard.changed = false;
        sHeard.final_result = false;
        sHeard.error.clear();
        sHeard.last_change = now();
        sHeard.any = false;
        sHeard.silence = false;
    }

    void teardown()
    {
        if (sEngine)
        {
            [sEngine.inputNode removeTapOnBus:0];
            [sEngine stop];
        }
        sEngine = nil;
        sTask = nil;
        sRequest = nil;
        sRecognizer = nil;
    }

    void stopAudio()
    {
        if (sEngine)
        {
            [sEngine.inputNode removeTapOnBus:0];
            [sEngine stop];
        }
        [sRequest endAudio];
    }

    /** Both permissions are in place: open the microphone and start hearing. */
    bool begin(std::string& why)
    {
        NSLocale* locale = sLanguage.empty()
            ? [NSLocale currentLocale]
            : [NSLocale localeWithLocaleIdentifier:[NSString stringWithUTF8String:sLanguage.c_str()]];
        sRecognizer = [[SFSpeechRecognizer alloc] initWithLocale:locale];
        if (!sRecognizer)
        {
            why = "Speech recognition does not understand this language: "
                + std::string(locale.localeIdentifier.UTF8String) + ".";
            return false;
        }
        if (!sRecognizer.isAvailable)
        {
            why = "Speech recognition is not available right now. For some languages, "
                  "Danish among them, it needs the internet.";
            sRecognizer = nil;
            return false;
        }

        sRequest = [[SFSpeechAudioBufferRecognitionRequest alloc] init];
        sRequest.shouldReportPartialResults = YES;
        if (@available(macOS 13.0, *))
        {
            sRequest.addsPunctuation = YES;
        }

        sEngine = [[AVAudioEngine alloc] init];
        AVAudioInputNode* input = sEngine.inputNode;
        AVAudioFormat* format = [input outputFormatForBus:0];
        if (format.sampleRate <= 0 || format.channelCount == 0)
        {
            why = "No microphone was found.";
            teardown();
            return false;
        }
        SFSpeechAudioBufferRecognitionRequest* request = sRequest;
        [input installTapOnBus:0 bufferSize:1024 format:format
                         block:^(AVAudioPCMBuffer* buffer, AVAudioTime* when) {
            [request appendAudioPCMBuffer:buffer];
        }];
        [sEngine prepare];
        NSError* err = nil;
        if (![sEngine startAndReturnError:&err])
        {
            why = "The microphone could not be started"
                + (err ? ": " + std::string(err.localizedDescription.UTF8String) : std::string())
                + ".";
            teardown();
            return false;
        }

        sTask = [sRecognizer recognitionTaskWithRequest:sRequest
                                          resultHandler:^(SFSpeechRecognitionResult* result, NSError* error) {
            std::lock_guard<std::mutex> lock(sHeard.m);
            if (result)
            {
                const std::string said = result.bestTranscription.formattedString.UTF8String;
                if (said != sHeard.text)
                {
                    sHeard.text = said;
                    sHeard.changed = true;
                    sHeard.last_change = now();
                    if (!said.empty()) sHeard.any = true;
                }
                if (result.isFinal) sHeard.final_result = true;
            }
            if (error)
            {
                // Words beat a complaint: with something heard, an error just
                // means it is over. Without, say which it was -- 1110 is
                // Apple's "no speech detected", which is nobody's fault.
                if (!sHeard.any && error.code == 1110) sHeard.silence = true;
                if (!sHeard.any && sHeard.error.empty())
                {
                    sHeard.error = (error.code == 1110)
                        ? std::string("Nothing was heard.")
                        : "Speech recognition stopped: "
                          + std::string(error.localizedDescription.UTF8String);
                }
                sHeard.final_result = true;
            }
        }];

        sStarted = now();
        {
            std::lock_guard<std::mutex> lock(sHeard.m);
            sHeard.last_change = sStarted;
        }
        sPhase = LISTENING;
        return true;
    }
}

bool LumenAISpeech::supported()
{
    return true;
}

bool LumenAISpeech::listening()
{
    return sPhase != IDLE;
}

bool LumenAISpeech::start(const std::string& language, std::string& why)
{
    if (sPhase != IDLE) return true;
    sLanguage = language;
    resetHeard();

    const SFSpeechRecognizerAuthorizationStatus speech = [SFSpeechRecognizer authorizationStatus];
    if (speech == SFSpeechRecognizerAuthorizationStatusDenied
        || speech == SFSpeechRecognizerAuthorizationStatusRestricted)
    {
        why = SPEECH_REFUSED;
        return false;
    }
    const AVAuthorizationStatus mic = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio];
    if (mic == AVAuthorizationStatusDenied || mic == AVAuthorizationStatusRestricted)
    {
        why = MIC_REFUSED;
        return false;
    }

    if (speech == SFSpeechRecognizerAuthorizationStatusAuthorized
        && mic == AVAuthorizationStatusAuthorized)
    {
        return begin(why);
    }

    // The first time: macOS asks the user, once each, and answers later.
    // poll() picks the answer up on the main thread and begins from there.
    sAuth = 0;
    sPhase = ASKING;
    void (^ask_mic)(void) = ^{
        if ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeAudio] == AVAuthorizationStatusAuthorized)
        {
            sAuth = 1;
            return;
        }
        [AVCaptureDevice requestAccessForMediaType:AVMediaTypeAudio completionHandler:^(BOOL granted) {
            sAuth = granted ? 1 : -2;
        }];
    };
    if (speech == SFSpeechRecognizerAuthorizationStatusAuthorized)
    {
        ask_mic();
    }
    else
    {
        [SFSpeechRecognizer requestAuthorization:^(SFSpeechRecognizerAuthorizationStatus status) {
            if (status != SFSpeechRecognizerAuthorizationStatusAuthorized)
            {
                sAuth = -1;
                return;
            }
            ask_mic();
        }];
    }
    return true;
}

void LumenAISpeech::stop()
{
    if (sPhase == ASKING)
    {
        cancel();
        return;
    }
    if (sPhase != LISTENING) return;
    stopAudio();
    sPhase = STOPPING;
    sStoppedAt = now();
}

void LumenAISpeech::cancel()
{
    [sTask cancel];
    teardown();
    sPhase = IDLE;
    resetHeard();
}

LumenAISpeech::Update LumenAISpeech::poll()
{
    Update u;
    if (sPhase == IDLE) return u;

    if (sPhase == ASKING)
    {
        const int auth = sAuth.load();
        if (auth == 0) return u;
        std::string why;
        if (auth == -1)      why = SPEECH_REFUSED;
        else if (auth == -2) why = MIC_REFUSED;
        else if (!begin(why)) { /* why is set */ }
        if (!why.empty())
        {
            teardown();
            sPhase = IDLE;
            u.finished = true;
            u.error = why;
            return u;
        }
        return u;   // listening now
    }

    bool final_result = false;
    {
        std::lock_guard<std::mutex> lock(sHeard.m);
        u.changed = sHeard.changed;
        sHeard.changed = false;
        u.text = sHeard.text;
        final_result = sHeard.final_result;
        u.error = sHeard.error;
        // Ended with no words and nothing wrong: the person just did not
        // speak -- a timeout before the first word, or Apple's 1110.
        u.nothing_heard = sHeard.text.empty() && (sHeard.error.empty() || sHeard.silence);

        // A pause ends it, as does a long take or a long silence before the
        // first word -- so nobody has to find the button again to stop.
        if (sPhase == LISTENING && !final_result)
        {
            const double t = now();
            if ((sHeard.any && t - sHeard.last_change > PAUSE_ENDS)
                || t - sStarted > LONGEST
                || (!sHeard.any && t - sStarted > FIRST_WORD))
            {
                // stop() takes no lock, so it is safe to call below.
            }
            else
            {
                return u;
            }
        }
    }

    if (sPhase == LISTENING && !final_result)
    {
        stop();
        return u;
    }
    if (sPhase == STOPPING && !final_result && now() - sStoppedAt < FINAL_GRACE)
    {
        return u;
    }

    // Done: the last word is in, or it is not coming.
    teardown();
    sPhase = IDLE;
    u.finished = true;
    if (!u.text.empty()) u.error.clear();   // words beat a late complaint
    return u;
}

std::vector<std::pair<std::string, std::string>> LumenAISpeech::languages()
{
    std::vector<std::pair<std::string, std::string>> out;
    NSLocale* here = [NSLocale currentLocale];
    for (NSLocale* l in [SFSpeechRecognizer supportedLocales])
    {
        NSString* tag = l.localeIdentifier;
        if (!tag) continue;
        NSString* name = [here localizedStringForLocaleIdentifier:tag];
        out.emplace_back(tag.UTF8String, name ? std::string(name.UTF8String) : std::string(tag.UTF8String));
    }
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}
