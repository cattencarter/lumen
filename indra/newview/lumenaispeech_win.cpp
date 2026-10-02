/**
 * @file lumenaispeech_win.cpp
 * @brief Windows' own speech recognition behind the Assistant's mic button.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * The counterpart of lumenaispeech_mac.mm: Windows.Media.SpeechRecognition,
 * called directly through C++/WinRT, so nothing is drawn but our own button --
 * unlike Win+H, which puts the system's dictation panel over the viewer.
 * Continuous dictation; Windows does it on Microsoft's servers, which is why
 * it needs Settings > Privacy & security > Speech > Online speech recognition
 * switched on, and says so in words when it is off.
 *
 * Same shape as the Mac file: everything here is called on the main thread,
 * the framework answers on its own threads, and its answers wait behind a lock
 * for poll(). A pause, a long take or a long silence before the first word
 * ends it here, by the same clock the Mac uses. No viewer headers on purpose,
 * and no precompiled header (CMakeLists.txt), so nothing drags windows.h and
 * its macros in front of the WinRT headers.
 */

#include "lumenaispeech.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.SpeechRecognition.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <mutex>

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Media::SpeechRecognition;
using winrt::Windows::Globalization::Language;

namespace
{
    // What the framework has said, from its own threads. Read by poll().
    struct Heard
    {
        std::mutex  m;
        std::string committed;      // phrases Windows has finished with
        std::string hypothesis;     // the phrase still being heard
        bool        changed      = false;
        bool        final_result = false;   // the session has ended
        std::string error;
        double      last_change  = 0.0;
        bool        any          = false;   // anything at all heard this time
        bool        silence      = false;   // ended on silence, nobody's fault
        bool        started      = false;   // the session is listening
        std::string start_error;            // it could not start, and why
    };
    Heard sHeard;

    enum Phase { IDLE, STARTING, LISTENING, STOPPING };
    Phase  sPhase = IDLE;
    double sStarted = 0.0;
    double sStoppedAt = 0.0;

    // Each start() gets a new number; an answer carrying an old one belongs
    // to a session already thrown away and is ignored.
    std::atomic<int> sGeneration{ 0 };

    SpeechRecognizer sRecognizer{ nullptr };
    SpeechRecognizer::HypothesisGenerated_revoker sOnHypothesis;
    SpeechContinuousRecognitionSession::ResultGenerated_revoker sOnResult;
    SpeechContinuousRecognitionSession::Completed_revoker sOnCompleted;

    // The same clock as the Mac's.
    const double PAUSE_ENDS  = 1.8;
    const double LONGEST     = 55.0;
    const double FIRST_WORD  = 10.0;
    const double FINAL_GRACE = 4.0;   // after stop(), for the last word to come back

    double now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // HRESULTs Windows gives for the two settings a person can fix.
    const int32_t SPEECH_PRIVACY_NOT_ACCEPTED = static_cast<int32_t>(0x80045509);
    const int32_t ACCESS_DENIED               = static_cast<int32_t>(0x80070005);

    const char* PRIVACY_OFF =
        "Windows' online speech recognition is switched off, and Lumen needs it to turn "
        "what you say into text. You can switch it on in Settings > Privacy & security > "
        "Speech.";
    const char* MIC_REFUSED =
        "Lumen could not use the microphone. Check that one is connected, and that "
        "Settings > Privacy & security > Microphone lets desktop apps use it.";

    std::string fromError(const hresult_error& e)
    {
        const int32_t code = e.code();
        if (code == SPEECH_PRIVACY_NOT_ACCEPTED) return PRIVACY_OFF;
        if (code == ACCESS_DENIED) return MIC_REFUSED;
        const std::string said = to_string(e.message());
        return "Speech recognition could not start"
            + (said.empty() ? std::string(".") : ": " + said);
    }

    std::string fromStatus(SpeechRecognitionResultStatus status)
    {
        switch (status)
        {
        case SpeechRecognitionResultStatus::TopicLanguageNotSupported:
        case SpeechRecognitionResultStatus::GrammarLanguageMismatch:
            return "Windows' speech recognition does not understand this language. It "
                   "listens in the speech language set in Settings > Time & language > "
                   "Speech.";
        case SpeechRecognitionResultStatus::NetworkFailure:
            return "Windows' speech recognition needs the internet, and could not reach "
                   "it.";
        case SpeechRecognitionResultStatus::MicrophoneUnavailable:
            return MIC_REFUSED;
        case SpeechRecognitionResultStatus::AudioQualityFailure:
            return "The sound was too quiet or too noisy to make out.";
        case SpeechRecognitionResultStatus::TimeoutExceeded:
        case SpeechRecognitionResultStatus::PauseLimitExceeded:
            return "Nothing was heard.";
        default:
            return "Speech recognition stopped.";
        }
    }

    void resetHeard()
    {
        std::lock_guard<std::mutex> lock(sHeard.m);
        sHeard.committed.clear();
        sHeard.hypothesis.clear();
        sHeard.changed = false;
        sHeard.final_result = false;
        sHeard.error.clear();
        sHeard.last_change = now();
        sHeard.any = false;
        sHeard.silence = false;
        sHeard.started = false;
        sHeard.start_error.clear();
    }

    /** Everything heard so far; caller holds the lock. */
    std::string textLocked()
    {
        if (sHeard.hypothesis.empty()) return sHeard.committed;
        if (sHeard.committed.empty()) return sHeard.hypothesis;
        return sHeard.committed + " " + sHeard.hypothesis;
    }

    void teardown()
    {
        sOnHypothesis.revoke();
        sOnResult.revoke();
        sOnCompleted.revoke();
        if (sRecognizer)
        {
            try { sRecognizer.Close(); } catch (...) {}
        }
        sRecognizer = nullptr;
    }

    // Said once, the first time the mic listens in English instead of the
    // language Windows is set to. Empty when there is nothing to say.
    std::string sLanguageNote;
    bool        sLanguageNoted = false;

    bool sameTag(const std::string& a, const std::string& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
        {
            if (std::tolower(static_cast<unsigned char>(a[i]))
                != std::tolower(static_cast<unsigned char>(b[i]))) return false;
        }
        return true;
    }

    /**
     * The language asked for, if Windows can dictate in it; else Windows' own
     * speech language; else English -- the author, 2026-10-02: "if it isn't
     * supported", default to English. Dictation is offered in a short list of
     * languages and Danish is not on it. Null when not even English is there.
     */
    Language pickLanguage(const std::string& wanted)
    {
        const auto offered = SpeechRecognizer::SupportedTopicLanguages();
        auto find = [&offered](const std::string& tag) -> Language
        {
            for (const Language& l : offered)
            {
                if (sameTag(to_string(l.LanguageTag()), tag)) return l;
            }
            return nullptr;
        };

        if (!wanted.empty())
        {
            if (Language l = find(wanted)) return l;
        }
        Language own = SpeechRecognizer::SystemSpeechLanguage();
        if (own)
        {
            if (Language l = find(to_string(own.LanguageTag()))) return l;
        }

        // Not offered: English instead, the nearest one first.
        Language english = find("en-US");
        if (!english) english = find("en-GB");
        if (!english)
        {
            for (const Language& l : offered)
            {
                const std::string tag = to_string(l.LanguageTag());
                if (tag.size() >= 2 && sameTag(tag.substr(0, 2), "en")) { english = l; break; }
            }
        }
        if (english && !sLanguageNoted)
        {
            const std::string name = own ? to_string(own.DisplayName()) : std::string("this language");
            sLanguageNote = "Windows cannot take dictation in " + name
                + ", so the mic listens in English.";
        }
        return english;
    }

    void failStart(int generation, const std::string& why)
    {
        if (generation != sGeneration.load()) return;
        std::lock_guard<std::mutex> lock(sHeard.m);
        if (sHeard.start_error.empty()) sHeard.start_error = why;
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
    resetHeard();
    // No apartment set-up here: the viewer's main thread is already a COM
    // single-threaded apartment (llappviewerwin32.cpp, at start-up), and the
    // speech objects are agile, so Windows' threads may call them directly.
    const int generation = ++sGeneration;

    try
    {
        Language lang = pickLanguage(language);
        if (!lang)
        {
            why = "Windows cannot take dictation in its own language, and has no English to "
                  "fall back on. Adding English in Settings > Time & language > Language & "
                  "region lets the mic listen in English.";
            return false;
        }
        sRecognizer = SpeechRecognizer(lang);
        sRecognizer.Constraints().Append(
            SpeechRecognitionTopicConstraint(SpeechRecognitionScenario::Dictation, L"dictation"));

        sOnHypothesis = sRecognizer.HypothesisGenerated(auto_revoke,
            [generation](const SpeechRecognizer&, const SpeechRecognitionHypothesisGeneratedEventArgs& args)
            {
                if (generation != sGeneration.load()) return;
                try
                {
                    const std::string said = to_string(args.Hypothesis().Text());
                    std::lock_guard<std::mutex> lock(sHeard.m);
                    if (said != sHeard.hypothesis)
                    {
                        sHeard.hypothesis = said;
                        sHeard.changed = true;
                        sHeard.last_change = now();
                        if (!said.empty()) sHeard.any = true;
                    }
                }
                catch (...) {}
            });

        SpeechContinuousRecognitionSession session = sRecognizer.ContinuousRecognitionSession();
        sOnResult = session.ResultGenerated(auto_revoke,
            [generation](const SpeechContinuousRecognitionSession&,
                         const SpeechContinuousRecognitionResultGeneratedEventArgs& args)
            {
                if (generation != sGeneration.load()) return;
                try
                {
                    const SpeechRecognitionResult result = args.Result();
                    std::lock_guard<std::mutex> lock(sHeard.m);
                    sHeard.hypothesis.clear();
                    if (result.Status() == SpeechRecognitionResultStatus::Success
                        && result.Confidence() != SpeechRecognitionConfidence::Rejected)
                    {
                        const std::string said = to_string(result.Text());
                        if (!said.empty())
                        {
                            if (!sHeard.committed.empty()) sHeard.committed += " ";
                            sHeard.committed += said;
                            sHeard.any = true;
                        }
                    }
                    sHeard.changed = true;
                    sHeard.last_change = now();
                }
                catch (...) {}
            });

        sOnCompleted = session.Completed(auto_revoke,
            [generation](const SpeechContinuousRecognitionSession&,
                         const SpeechContinuousRecognitionCompletedEventArgs& args)
            {
                if (generation != sGeneration.load()) return;
                try
                {
                    const SpeechRecognitionResultStatus status = args.Status();
                    std::lock_guard<std::mutex> lock(sHeard.m);
                    // Words beat a complaint: with something heard, the end of
                    // the session just means it is over.
                    if (!sHeard.any)
                    {
                        if (status == SpeechRecognitionResultStatus::TimeoutExceeded
                            || status == SpeechRecognitionResultStatus::PauseLimitExceeded)
                        {
                            sHeard.silence = true;
                        }
                        if (status != SpeechRecognitionResultStatus::Success
                            && status != SpeechRecognitionResultStatus::UserCanceled
                            && sHeard.error.empty())
                        {
                            sHeard.error = fromStatus(status);
                        }
                    }
                    sHeard.final_result = true;
                }
                catch (...) {}
            });

        // Compile the dictation grammar, then start listening -- both answer
        // later, on Windows' threads, and poll() picks the outcome up.
        // The handler keeps its own reference: sRecognizer belongs to the
        // main thread, and this runs on one of Windows'.
        SpeechRecognizer recognizer = sRecognizer;
        sRecognizer.CompileConstraintsAsync().Completed(
            [generation, recognizer](const IAsyncOperation<SpeechRecognitionCompilationResult>& op, AsyncStatus)
            {
                if (generation != sGeneration.load()) return;
                try
                {
                    const SpeechRecognitionCompilationResult compiled = op.GetResults();
                    if (compiled.Status() != SpeechRecognitionResultStatus::Success)
                    {
                        failStart(generation, fromStatus(compiled.Status()));
                        return;
                    }
                    // Cancelled meanwhile: the generation check above, or a
                    // closed recognizer throwing below, catches it.
                    recognizer.ContinuousRecognitionSession().StartAsync().Completed(
                        [generation](const IAsyncAction& start, AsyncStatus)
                        {
                            if (generation != sGeneration.load()) return;
                            try
                            {
                                start.GetResults();
                                std::lock_guard<std::mutex> lock(sHeard.m);
                                sHeard.started = true;
                                sHeard.last_change = now();
                            }
                            catch (const hresult_error& e) { failStart(generation, fromError(e)); }
                            catch (...) { failStart(generation, "Speech recognition could not start."); }
                        });
                }
                catch (const hresult_error& e) { failStart(generation, fromError(e)); }
                catch (...) { failStart(generation, "Speech recognition could not start."); }
            });
    }
    catch (const hresult_error& e)
    {
        why = fromError(e);
        teardown();
        return false;
    }
    catch (...)
    {
        why = "Speech recognition could not start.";
        teardown();
        return false;
    }

    sPhase = STARTING;
    sStarted = now();
    return true;
}

void LumenAISpeech::stop()
{
    if (sPhase == STARTING)
    {
        cancel();
        return;
    }
    if (sPhase != LISTENING) return;
    try
    {
        // The words still being heard come back as one last result, then
        // the session's Completed -- poll() waits a little for both.
        if (sRecognizer) sRecognizer.ContinuousRecognitionSession().StopAsync();
    }
    catch (...) {}
    sPhase = STOPPING;
    sStoppedAt = now();
}

void LumenAISpeech::cancel()
{
    ++sGeneration;   // nothing still on its way is wanted now
    try
    {
        if (sRecognizer && sPhase != STARTING) sRecognizer.ContinuousRecognitionSession().CancelAsync();
    }
    catch (...) {}
    teardown();
    sPhase = IDLE;
    resetHeard();
}

LumenAISpeech::Update LumenAISpeech::poll()
{
    Update u;
    if (sPhase == IDLE) return u;

    if (sPhase == STARTING)
    {
        std::string why;
        bool started = false;
        {
            std::lock_guard<std::mutex> lock(sHeard.m);
            why = sHeard.start_error;
            started = sHeard.started;
        }
        if (!why.empty())
        {
            ++sGeneration;
            teardown();
            sPhase = IDLE;
            u.finished = true;
            u.error = why;
            return u;
        }
        if (!started)
        {
            // Compiling the grammar and opening the microphone takes a moment;
            // if Windows never answers, give up rather than wait for ever.
            if (now() - sStarted > FIRST_WORD)
            {
                ++sGeneration;
                teardown();
                sPhase = IDLE;
                u.finished = true;
                u.error = "Windows' speech recognition did not start.";
            }
            return u;
        }
        sPhase = LISTENING;
        sStarted = now();
        if (!sLanguageNote.empty() && !sLanguageNoted)
        {
            u.note = sLanguageNote;   // once, the first time it is so
            sLanguageNoted = true;
        }
        return u;   // listening now
    }

    bool final_result = false;
    {
        std::lock_guard<std::mutex> lock(sHeard.m);
        u.changed = sHeard.changed;
        sHeard.changed = false;
        u.text = textLocked();
        final_result = sHeard.final_result;
        u.error = sHeard.error;
        // Ended with no words and nothing wrong: the person just did not
        // speak -- a timeout before the first word, or Windows' own silence.
        u.nothing_heard = u.text.empty() && (sHeard.error.empty() || sHeard.silence);

        // A pause ends it, as does a long take or a long silence before the
        // first word -- so nobody has to find the button again to stop.
        if (sPhase == LISTENING && !final_result)
        {
            const double t = now();
            if (!((sHeard.any && t - sHeard.last_change > PAUSE_ENDS)
                  || t - sStarted > LONGEST
                  || (!sHeard.any && t - sStarted > FIRST_WORD)))
            {
                return u;
            }
        }
    }

    if (sPhase == LISTENING && !final_result)
    {
        stop();   // takes no lock
        return u;
    }
    if (sPhase == STOPPING && !final_result && now() - sStoppedAt < FINAL_GRACE)
    {
        return u;
    }

    // Done: the last word is in, or it is not coming.
    ++sGeneration;
    teardown();
    sPhase = IDLE;
    u.finished = true;
    if (!u.text.empty()) u.error.clear();   // words beat a late complaint
    return u;
}

std::vector<std::pair<std::string, std::string>> LumenAISpeech::languages()
{
    std::vector<std::pair<std::string, std::string>> out;
    try
    {
        for (const Language& l : SpeechRecognizer::SupportedTopicLanguages())
        {
            out.emplace_back(to_string(l.LanguageTag()), to_string(l.DisplayName()));
        }
    }
    catch (...) {}
    std::sort(out.begin(), out.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    return out;
}

std::string LumenAISpeech::ownLanguage()
{
    try
    {
        if (Language own = SpeechRecognizer::SystemSpeechLanguage())
            return to_string(own.LanguageTag());
    }
    catch (...) {}
    return std::string();
}
