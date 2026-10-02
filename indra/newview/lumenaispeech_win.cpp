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
 * **Every speech object lives on one worker thread of its own, in the
 * multi-threaded apartment.** The viewer's main thread is a single-threaded
 * apartment (llappviewerwin32.cpp, at start-up) whose window messages are
 * pumped on another thread, so an object made there may never get Windows'
 * answers delivered -- the first version did exactly that on the author's
 * laptop: no complaint, and no words. The main thread only posts start, stop
 * and cancel to the worker, and reads what was heard from behind a lock, the
 * same shape as the Mac file: a pause, a long take or a long silence before
 * the first word ends it, by the same clock the Mac uses.
 *
 * No precompiled header (CMakeLists.txt), and the viewer's one header here,
 * llerror.h, comes after the WinRT headers, so windows.h and its macros never
 * stand in front of them. The log says what happened, never what was said.
 */

#include "lumenaispeech.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Media.SpeechRecognition.h>

#include "llerror.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

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
        std::string note;                   // to say once, when listening begins
    };
    Heard sHeard;

    enum Phase { IDLE, STARTING, LISTENING, STOPPING };
    Phase  sPhase = IDLE;
    double sStarted = 0.0;
    double sStoppedAt = 0.0;

    // Each start() gets a new number; an answer carrying an old one belongs
    // to a session already thrown away and is ignored.
    std::atomic<int> sGeneration{ 0 };

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
    const char* NOTHING_HEARD =
        "Nothing was heard. Windows listens with its default microphone, which is chosen "
        "in Settings > System > Sound > Input.";
    const char* NO_ENGLISH =
        "Windows cannot take dictation in its own language, and has no English to fall "
        "back on. Adding English in Settings > Time & language > Language & region lets "
        "the mic listen in English.";

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
            return NOTHING_HEARD;
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
        sHeard.note.clear();
    }

    /** Everything heard so far; caller holds the lock. */
    std::string textLocked()
    {
        if (sHeard.hypothesis.empty()) return sHeard.committed;
        if (sHeard.committed.empty()) return sHeard.hypothesis;
        return sHeard.committed + " " + sHeard.hypothesis;
    }

    void failStart(int generation, const std::string& why)
    {
        if (generation != sGeneration.load()) return;
        std::lock_guard<std::mutex> lock(sHeard.m);
        if (sHeard.start_error.empty()) sHeard.start_error = why;
    }

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
     * `note` is set, once ever, when English stands in.
     */
    Language pickLanguage(const std::string& wanted, std::string& note, bool& noted)
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
        if (english && !noted)
        {
            const std::string name = own ? to_string(own.DisplayName()) : std::string("this language");
            note = "Windows cannot take dictation in " + name + ", so the mic listens in English.";
            noted = true;
        }
        return english;
    }

    // ------------------------------------------------------------------
    // The worker. Only this thread ever touches a speech object.
    // ------------------------------------------------------------------

    struct Command
    {
        enum Kind { START, STOP, CANCEL };
        Kind        kind = CANCEL;
        int         generation = 0;
        std::string language;
    };

    struct Worker
    {
        std::mutex              m;
        std::condition_variable cv;
        std::deque<Command>     queue;

        SpeechRecognizer recognizer{ nullptr };
        SpeechRecognizer::HypothesisGenerated_revoker onHypothesis;
        SpeechContinuousRecognitionSession::ResultGenerated_revoker onResult;
        SpeechContinuousRecognitionSession::Completed_revoker onCompleted;
        bool languageNoted = false;
    };
    // Made once and never destroyed: the worker may still be waiting on its
    // lock when the viewer exits, and must not find the lock already gone.
    Worker* sWorker = nullptr;

    void release(Worker& w)
    {
        w.onHypothesis.revoke();
        w.onResult.revoke();
        w.onCompleted.revoke();
        if (w.recognizer)
        {
            try { w.recognizer.Close(); } catch (...) {}
        }
        w.recognizer = nullptr;
    }

    void doStart(Worker& w, const Command& c)
    {
        release(w);   // anything left of a session before
        const int generation = c.generation;
        if (generation != sGeneration.load()) return;   // cancelled before it began

        std::string note;
        Language lang = pickLanguage(c.language, note, w.languageNoted);
        if (!lang)
        {
            LL_WARNS("LumenAISpeech") << "No language Windows can take dictation in" << LL_ENDL;
            failStart(generation, NO_ENGLISH);
            return;
        }
        LL_INFOS("LumenAISpeech") << "Listening in " << to_string(lang.LanguageTag())
                                  << (c.language.empty() ? std::string(" (Windows' own)")
                                                         : " (asked for " + c.language + ")")
                                  << LL_ENDL;

        w.recognizer = SpeechRecognizer(lang);
        w.recognizer.Constraints().Append(
            SpeechRecognitionTopicConstraint(SpeechRecognitionScenario::Dictation, L"dictation"));

        w.onHypothesis = w.recognizer.HypothesisGenerated(auto_revoke,
            [generation](const SpeechRecognizer&, const SpeechRecognitionHypothesisGeneratedEventArgs& args)
            {
                if (generation != sGeneration.load()) return;
                try
                {
                    const std::string said = to_string(args.Hypothesis().Text());
                    bool first = false;
                    {
                        std::lock_guard<std::mutex> lock(sHeard.m);
                        if (said != sHeard.hypothesis)
                        {
                            sHeard.hypothesis = said;
                            sHeard.changed = true;
                            sHeard.last_change = now();
                            if (!said.empty() && !sHeard.any) { sHeard.any = true; first = true; }
                        }
                    }
                    if (first) LL_INFOS("LumenAISpeech") << "First words heard" << LL_ENDL;
                }
                catch (...) {}
            });

        SpeechContinuousRecognitionSession session = w.recognizer.ContinuousRecognitionSession();
        w.onResult = session.ResultGenerated(auto_revoke,
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

        w.onCompleted = session.Completed(auto_revoke,
            [generation](const SpeechContinuousRecognitionSession&,
                         const SpeechContinuousRecognitionCompletedEventArgs& args)
            {
                if (generation != sGeneration.load()) return;
                try
                {
                    const SpeechRecognitionResultStatus status = args.Status();
                    LL_INFOS("LumenAISpeech") << "Session ended, status " << static_cast<int>(status) << LL_ENDL;
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

        // On this thread, in the multi-threaded apartment, waiting is allowed.
        const SpeechRecognitionCompilationResult compiled = w.recognizer.CompileConstraintsAsync().get();
        LL_INFOS("LumenAISpeech") << "Dictation prepared, status " << static_cast<int>(compiled.Status()) << LL_ENDL;
        if (compiled.Status() != SpeechRecognitionResultStatus::Success)
        {
            failStart(generation, fromStatus(compiled.Status()));
            release(w);
            return;
        }
        if (generation != sGeneration.load())
        {
            release(w);
            return;
        }
        session.StartAsync().get();
        LL_INFOS("LumenAISpeech") << "Listening" << LL_ENDL;
        std::lock_guard<std::mutex> lock(sHeard.m);
        sHeard.started = true;
        sHeard.last_change = now();
        if (!note.empty()) sHeard.note = note;
    }

    void doStop(Worker& w, const Command& c)
    {
        if (!w.recognizer || c.generation != sGeneration.load()) return;
        // The words still being heard come back as one last result, then
        // the session's Completed -- poll() waits a little for both.
        w.recognizer.ContinuousRecognitionSession().StopAsync().get();
    }

    void doCancel(Worker& w)
    {
        if (w.recognizer)
        {
            try { w.recognizer.ContinuousRecognitionSession().CancelAsync().get(); } catch (...) {}
        }
        release(w);
    }

    void run(Worker* w)
    {
        try
        {
            init_apartment(apartment_type::multi_threaded);
        }
        catch (const hresult_error& e)
        {
            LL_WARNS("LumenAISpeech") << "Speech thread got no apartment: " << to_string(e.message()) << LL_ENDL;
        }
        for (;;)
        {
            Command c;
            {
                std::unique_lock<std::mutex> lock(w->m);
                w->cv.wait(lock, [w] { return !w->queue.empty(); });
                c = w->queue.front();
                w->queue.pop_front();
            }
            try
            {
                switch (c.kind)
                {
                case Command::START:  doStart(*w, c); break;
                case Command::STOP:   doStop(*w, c);  break;
                case Command::CANCEL: doCancel(*w);   break;
                }
            }
            catch (const hresult_error& e)
            {
                LL_WARNS("LumenAISpeech") << "Speech failed: 0x" << std::hex
                                          << static_cast<uint32_t>(static_cast<int32_t>(e.code())) << std::dec
                                          << " " << to_string(e.message()) << LL_ENDL;
                if (c.kind == Command::START) failStart(c.generation, fromError(e));
                release(*w);
            }
            catch (...)
            {
                LL_WARNS("LumenAISpeech") << "Speech failed" << LL_ENDL;
                if (c.kind == Command::START) failStart(c.generation, "Speech recognition could not start.");
                release(*w);
            }
        }
    }

    void post(Command::Kind kind, int generation, const std::string& language = std::string())
    {
        if (!sWorker)
        {
            sWorker = new Worker();
            std::thread(run, sWorker).detach();
        }
        {
            std::lock_guard<std::mutex> lock(sWorker->m);
            Command c;
            c.kind = kind;
            c.generation = generation;
            c.language = language;
            sWorker->queue.push_back(c);
        }
        sWorker->cv.notify_one();
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
    const int generation = ++sGeneration;
    post(Command::START, generation, language);
    // Whatever goes wrong now arrives through poll(), from the worker.
    why.clear();
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
    post(Command::STOP, sGeneration.load());
    sPhase = STOPPING;
    sStoppedAt = now();
}

void LumenAISpeech::cancel()
{
    ++sGeneration;   // nothing still on its way is wanted now
    post(Command::CANCEL, sGeneration.load());
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
            if (started && !sHeard.note.empty())
            {
                u.note = sHeard.note;   // once, the first time it is so
                sHeard.note.clear();
            }
        }
        if (!why.empty())
        {
            LL_INFOS("LumenAISpeech") << "Could not start: " << why << LL_ENDL;
            ++sGeneration;
            post(Command::CANCEL, sGeneration.load());
            sPhase = IDLE;
            u.finished = true;
            u.error = why;
            return u;
        }
        if (!started)
        {
            // Preparing dictation and opening the microphone takes a moment;
            // if Windows never answers, give up rather than wait for ever.
            if (now() - sStarted > FIRST_WORD)
            {
                LL_WARNS("LumenAISpeech") << "Windows never said it was listening" << LL_ENDL;
                ++sGeneration;
                post(Command::CANCEL, sGeneration.load());
                sPhase = IDLE;
                u.finished = true;
                u.error = "Windows' speech recognition did not start.";
            }
            return u;
        }
        sPhase = LISTENING;
        sStarted = now();
        return u;   // listening now
    }

    bool final_result = false;
    const char* why_ended = nullptr;
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
            if (sHeard.any && t - sHeard.last_change > PAUSE_ENDS) why_ended = "a pause";
            else if (t - sStarted > LONGEST)                       why_ended = "the longest take";
            else if (!sHeard.any && t - sStarted > FIRST_WORD)     why_ended = "no first word";
            else return u;
        }
    }

    if (sPhase == LISTENING && !final_result)
    {
        LL_INFOS("LumenAISpeech") << "Stopping after " << why_ended << LL_ENDL;
        stop();   // takes no lock
        return u;
    }
    if (sPhase == STOPPING && !final_result && now() - sStoppedAt < FINAL_GRACE)
    {
        return u;
    }

    // Done: the last word is in, or it is not coming.
    LL_INFOS("LumenAISpeech") << "Done listening, " << (u.text.empty() ? "no words" : "words heard") << LL_ENDL;
    ++sGeneration;
    post(Command::CANCEL, sGeneration.load());
    sPhase = IDLE;
    u.finished = true;
    if (!u.text.empty()) u.error.clear();   // words beat a late complaint
    else if (u.error.empty()) u.error = NOTHING_HEARD;
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
