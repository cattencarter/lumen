/**
 * @file lumenaispeech_win.cpp
 * @brief Whisper, on the computer itself, behind the Assistant's mic button.
 *
 * Copyright (C) 2026 Catten Carter
 * Based on Phoenix Firestorm and on the Second Life Viewer.
 * Licensed under the GNU Lesser General Public License, version 2.1.
 *
 * WHY NOT WINDOWS' OWN. The first version called Windows.Media.SpeechRecognition,
 * the counterpart of Apple's Speech framework. On the author's laptop it
 * prepared its dictation, opened the microphone and answered "Unknown" with no
 * words -- also from a PowerShell script outside Lumen, while Win+H heard him
 * fine on the same machine (2026-10-02). Others have reported the same since
 * 2022, with no fix. Windows' older on-device recogniser (SAPI) heard
 * something "in the direction of it" at a confidence of 0.02, and is being
 * retired. So the words are worked out by Whisper (whisper.cpp, MIT licence),
 * which the Windows build compiles from source and ships in whisper\ beside
 * the viewer (.github/workflows/lumen-windows.yml, viewer_manifest.py).
 *
 * HOW. The microphone is recorded here with waveIn -- part of Windows, nothing
 * to install -- at 16 kHz, the rate Whisper wants, and a pause is found by
 * loudness rather than by words, so the take can end itself as it does on the
 * Mac. The take is written as a WAV file and whisper-cli.exe, started with no
 * window, turns it into text; then both files are deleted. Whisper is a
 * separate program rather than linked in: it is compiled with its own
 * settings, and a crash in it cannot take the viewer down.
 * It does not show words AS they are said, as Apple's does -- the text arrives
 * a second or two after the pause, with "Writing down what you said..." in the
 * Assistant's status bar meanwhile.
 *
 * THE MODEL is a one-time download the person agrees to first: the author,
 * 2026-10-02, *"the installation must be easy and painless, guide the user
 * through, and give the option to say no."* English only (ggml-small.en,
 * quantised, 190 MB): Windows was never going to listen in Danish anyway, the
 * English-only model is the better one at that size, and base.en at 148 MB
 * did not understand the author well enough. It is fetched by
 * Windows' own curl.exe from Hugging Face, pinned to one revision, and
 * checked against its SHA-256 before it is used. It lives in
 * %LOCALAPPDATA%\Lumen\whisper -- local, not roaming, so a company's roaming
 * profile does not carry 190 MB around -- and Preferences can remove it.
 *
 * Every program is started with its working folder set to that folder and
 * given only plain ASCII file names, because whisper-cli reads its arguments in
 * the system code page and a user name with an "ø" in it would otherwise break
 * the paths. The log says what happened, never what was said.
 */

#include "linden_common.h"

#include "lumenaispeech.h"

#include "llerror.h"
#include "llwin32headers.h"

#include <mmsystem.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
    // ---- the model -------------------------------------------------------

    // Pinned to one revision of the repository, and checked against its own
    // SHA-256 (Hugging Face's LFS record), so nothing else can arrive under
    // this name.
    //
    // small.en, quantised to 5 bits: the author tried base.en (148 MB) first,
    // 2026-10-02, and *"it doesn't understand me well enough and I speak
    // pretty good english. the longer time is something we'll have to live
    // with."* So the next size up, at nearly its full accuracy in a third of
    // its 488 MB.
    const wchar_t* const MODEL_FILE = L"ggml-small.en-q5_1.bin";
    const wchar_t* const MODEL_PART = L"ggml-small.en-q5_1.bin.part";
    const wchar_t* const MODEL_URL =
        L"https://huggingface.co/ggerganov/whisper.cpp/resolve/"
        L"5359861c739e955e79d9a303bcbc70fb988958b1/ggml-small.en-q5_1.bin";
    const long long MODEL_BYTES = 190098681LL;
    const char* const MODEL_SHA256 =
        "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30";
    // The first model this used, removed once the new one is in place.
    const wchar_t* const OLD_MODEL_FILE = L"ggml-base.en.bin";

    // ---- the same clock as the Mac's --------------------------------------

    const double PAUSE_ENDS = 1.8;    // seconds of quiet after speech
    const double LONGEST    = 55.0;   // one take at most
    const double FIRST_WORD = 10.0;   // silence before anything is said
    const int    RATE       = 16000;  // what Whisper wants
    const double WRITE_TIMEOUT = 90.0;   // whisper-cli on a slow computer

    double now()
    {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    const char* const NOTHING_HEARD =
        "Nothing was heard. Windows listens with its default microphone, which is chosen "
        "in Settings > System > Sound > Input.";
    const char* const ONLY_SILENCE =
        "Windows opened the microphone but sent only silence. Check that Settings > "
        "Privacy & security > Microphone lets desktop apps use it, and that the "
        "microphone is not muted.";
    const char* const NO_MICROPHONE =
        "Lumen could not open a microphone. Check that one is connected and chosen in "
        "Settings > System > Sound > Input, and that Settings > Privacy & security > "
        "Microphone lets desktop apps use it.";

    // ---- paths -----------------------------------------------------------

    /** %LOCALAPPDATA%\Lumen\whisper, made when asked for. Empty if impossible. */
    std::wstring dataDir(bool make)
    {
        const wchar_t* local = _wgetenv(L"LOCALAPPDATA");
        if (!local || !*local) return std::wstring();
        std::wstring lumen = std::wstring(local) + L"\\Lumen";
        std::wstring dir = lumen + L"\\whisper";
        if (make)
        {
            CreateDirectoryW(lumen.c_str(), nullptr);
            CreateDirectoryW(dir.c_str(), nullptr);
        }
        return dir;
    }

    std::wstring inData(const wchar_t* name)
    {
        const std::wstring dir = dataDir(false);
        return dir.empty() ? std::wstring() : dir + L"\\" + name;
    }

    /** whisper\whisper-cli.exe beside the viewer, where the installer puts it. */
    std::wstring whisperExe()
    {
        wchar_t self[MAX_PATH * 2] = {};
        const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        if (n == 0 || n >= MAX_PATH * 2) return std::wstring();
        std::wstring dir(self, n);
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash == std::wstring::npos) return std::wstring();
        return dir.substr(0, slash) + L"\\whisper\\whisper-cli.exe";
    }

    std::wstring curlExe()
    {
        const wchar_t* root = _wgetenv(L"SystemRoot");
        return std::wstring((root && *root) ? root : L"C:\\Windows") + L"\\System32\\curl.exe";
    }

    long long fileSize(const std::wstring& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA a;
        if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return -1;
        if (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) return -1;
        return (static_cast<long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    }

    bool modelPresent()
    {
        return fileSize(inData(MODEL_FILE)) == MODEL_BYTES;
    }

    /** The first line of a small log file, for an error message. */
    std::string firstLine(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return std::string();
        char buf[512] = {};
        DWORD got = 0;
        ReadFile(f, buf, sizeof(buf) - 1, &got, nullptr);
        CloseHandle(f);
        std::string s(buf, got);
        const size_t end = s.find_first_of("\r\n");
        if (end != std::string::npos) s.resize(end);
        // Only printable ASCII goes into a message: a log is not trusted text.
        std::string out;
        for (char c : s) if (c >= 32 && c < 127) out.push_back(c);
        return out;
    }

    /**
     * Start a program with no window, working in `dir`, with stdin from NUL
     * and stdout and stderr to `log` (in `dir`). The handle is the caller's.
     */
    HANDLE launch(const std::wstring& exe, const std::wstring& args, const std::wstring& dir,
                  const std::wstring& log, DWORD& error)
    {
        SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
        HANDLE in  = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa,
                                 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        HANDLE out = CreateFileW((dir + L"\\" + log).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa,
                                 CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput  = in;
        si.hStdOutput = out;
        si.hStdError  = out;
        PROCESS_INFORMATION pi = {};

        // The command line is the program's own path in quotes, then the
        // arguments, which are built here from fixed words and ASCII names.
        std::wstring line = L"\"" + exe + L"\" " + args;
        std::vector<wchar_t> buf(line.begin(), line.end());
        buf.push_back(0);
        const BOOL ok = CreateProcessW(exe.c_str(), buf.data(), nullptr, nullptr, TRUE,
                                       CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi);
        error = ok ? 0 : GetLastError();
        if (in != INVALID_HANDLE_VALUE) CloseHandle(in);
        if (out != INVALID_HANDLE_VALUE) CloseHandle(out);
        if (!ok) return nullptr;
        CloseHandle(pi.hThread);
        return pi.hProcess;
    }

    /** SHA-256 of a file, as lowercase hex; empty if it could not be read. */
    std::string sha256Of(const std::wstring& path)
    {
        BCRYPT_ALG_HANDLE alg = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return std::string();
        std::string hex;
        if (BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0)
        {
            HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                std::vector<unsigned char> buf(1 << 20);
                DWORD got = 0;
                bool ok = true;
                while (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got > 0)
                {
                    if (BCryptHashData(hash, buf.data(), got, 0) != 0) { ok = false; break; }
                }
                CloseHandle(f);
                unsigned char digest[32] = {};
                if (ok && BCryptFinishHash(hash, digest, sizeof(digest), 0) == 0)
                {
                    static const char* const digits = "0123456789abcdef";
                    for (unsigned char b : digest)
                    {
                        hex.push_back(digits[b >> 4]);
                        hex.push_back(digits[b & 15]);
                    }
                }
            }
            BCryptDestroyHash(hash);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
        return hex;
    }

    // ---- the download ----------------------------------------------------

    struct Download
    {
        std::mutex  m;
        bool        running = false;
        bool        cancel  = false;
        std::string error;     // the last attempt's, until the next one
    };
    // Never destroyed: a download thread may still be finishing at exit.
    Download* sDownload = new Download();

    std::string curlError(DWORD code, const std::string& said)
    {
        switch (code)
        {
        case 6:  return "the computer could not find huggingface.co. Is it online?";
        case 7:  return "huggingface.co did not answer. Is the computer online, or does a firewall block it?";
        case 28: return "the connection was too slow and timed out.";
        case 35:
        case 60: return "Windows did not trust the connection to Hugging Face. A company network that "
                        "inspects traffic can cause this.";
        case 23: return "the file could not be written. Is the disk full?";
        case 22: return "Hugging Face refused the request (" + (said.empty() ? std::string("an HTTP error") : said) + ").";
        default: break;
        }
        return "the download stopped (curl " + std::to_string(code) + (said.empty() ? std::string() : ": " + said) + ").";
    }

    void runDownload()
    {
        auto fail = [](const std::string& why)
        {
            LL_WARNS("LumenAISpeech") << "Model download failed: " << why << LL_ENDL;
            std::lock_guard<std::mutex> lock(sDownload->m);
            sDownload->running = false;
            sDownload->error = why;
        };

        const std::wstring dir = dataDir(true);
        const std::wstring part = dir + L"\\" + MODEL_PART;
        const std::wstring model = dir + L"\\" + MODEL_FILE;

        long long have = fileSize(part);
        if (have > MODEL_BYTES)
        {
            DeleteFileW(part.c_str());
            have = -1;
        }
        if (have < MODEL_BYTES)
        {
            // -C - carries on from a .part a cancelled or broken attempt left.
            std::wstring args = L"--fail --location --silent --show-error --retry 3 "
                                L"--connect-timeout 30 --stderr download.log ";
            if (have > 0) args += L"-C - ";
            args += L"--output ";
            args += MODEL_PART;
            args += L" \"";
            args += MODEL_URL;
            args += L"\"";

            DWORD error = 0;
            HANDLE p = launch(curlExe(), args, dir, L"download-out.log", error);
            if (!p)
            {
                fail("Windows' own download program, curl.exe, could not be started (error " +
                     std::to_string(error) + ").");
                return;
            }
            LL_INFOS("LumenAISpeech") << "Downloading the speech model" << (have > 0 ? ", resuming" : "") << LL_ENDL;
            for (;;)
            {
                if (WaitForSingleObject(p, 250) == WAIT_OBJECT_0) break;
                bool cancel = false;
                {
                    std::lock_guard<std::mutex> lock(sDownload->m);
                    cancel = sDownload->cancel;
                }
                if (cancel)
                {
                    TerminateProcess(p, 1);
                    WaitForSingleObject(p, 5000);
                    CloseHandle(p);
                    DeleteFileW(part.c_str());
                    LL_INFOS("LumenAISpeech") << "Model download stopped by the user" << LL_ENDL;
                    std::lock_guard<std::mutex> lock(sDownload->m);
                    sDownload->running = false;
                    sDownload->error.clear();
                    return;
                }
            }
            DWORD code = 0;
            GetExitCodeProcess(p, &code);
            CloseHandle(p);
            if (code != 0)
            {
                std::string said = firstLine(dir + L"\\download.log");
                const std::string prefix = "curl: ";
                if (said.compare(0, prefix.size(), prefix) == 0) said = said.substr(prefix.size());
                fail(curlError(code, said));
                return;
            }
        }

        if (fileSize(part) != MODEL_BYTES)
        {
            DeleteFileW(part.c_str());
            fail("the file that arrived was the wrong size, so it was thrown away. Try again.");
            return;
        }
        if (sha256Of(part) != MODEL_SHA256)
        {
            DeleteFileW(part.c_str());
            fail("the file that arrived was not the one Lumen asked for (its checksum did not "
                 "match), so it was thrown away. Try again.");
            return;
        }
        if (!MoveFileExW(part.c_str(), model.c_str(), MOVEFILE_REPLACE_EXISTING))
        {
            fail("the file could not be put in place (error " + std::to_string(GetLastError()) + ").");
            return;
        }
        DeleteFileW((dir + L"\\download.log").c_str());
        DeleteFileW((dir + L"\\download-out.log").c_str());
        DeleteFileW((dir + L"\\" + OLD_MODEL_FILE).c_str());
        LL_INFOS("LumenAISpeech") << "Speech model downloaded and checked" << LL_ENDL;
        std::lock_guard<std::mutex> lock(sDownload->m);
        sDownload->running = false;
        sDownload->error.clear();
    }

    // ---- one take --------------------------------------------------------

    struct Take
    {
        std::mutex m;
        // Set by the main thread.
        bool stop   = false;
        bool cancel = false;
        // Set by the recording thread.
        bool   opened    = false;
        bool   any       = false;    // speech was heard
        double lastVoice = 0.0;      // when it was last heard
        bool   writing   = false;    // whisper-cli is turning it into text
        bool   done      = false;
        bool   silenceOnly = false;  // every sample was zero
        std::string text;
        std::string error;
    };

    enum Phase { IDLE, LISTENING, WRITING };
    Phase  sPhase   = IDLE;
    double sStarted = 0.0;
    bool   sSaidWriting = false;
    bool   sEnglishNoted = false;
    std::shared_ptr<Take> sTake;

    /** Whisper's marks for what is not speech: [BLANK_AUDIO], (music), ... */
    std::string cleaned(const std::string& raw)
    {
        std::string out;
        int depth = 0;
        for (char c : raw)
        {
            if (c == '[' || c == '(') { ++depth; continue; }
            if ((c == ']' || c == ')') && depth > 0) { --depth; continue; }
            if (depth > 0) continue;
            out.push_back((c == '\r' || c == '\n' || c == '\t') ? ' ' : c);
        }
        // Collapse runs of spaces and trim.
        std::string tidy;
        for (char c : out)
        {
            if (c == ' ' && (tidy.empty() || tidy.back() == ' ')) continue;
            tidy.push_back(c);
        }
        while (!tidy.empty() && tidy.back() == ' ') tidy.pop_back();
        return tidy;
    }

    bool writeWav(const std::wstring& path, const std::vector<int16_t>& pcm)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return false;
        const uint32_t data = (uint32_t)(pcm.size() * sizeof(int16_t));
        unsigned char h[44] = {};
        auto put32 = [&h](int at, uint32_t v) { for (int i = 0; i < 4; ++i) h[at + i] = (unsigned char)(v >> (8 * i)); };
        auto put16 = [&h](int at, uint16_t v) { h[at] = (unsigned char)v; h[at + 1] = (unsigned char)(v >> 8); };
        memcpy(h, "RIFF", 4); put32(4, 36 + data); memcpy(h + 8, "WAVE", 4);
        memcpy(h + 12, "fmt ", 4); put32(16, 16); put16(20, 1); put16(22, 1);
        put32(24, RATE); put32(28, RATE * 2); put16(32, 2); put16(34, 16);
        memcpy(h + 36, "data", 4); put32(40, data);
        DWORD wrote = 0;
        bool ok = WriteFile(f, h, sizeof(h), &wrote, nullptr) && wrote == sizeof(h);
        if (ok && data > 0)
        {
            ok = WriteFile(f, pcm.data(), data, &wrote, nullptr) && wrote == data;
        }
        CloseHandle(f);
        return ok;
    }

    std::string readUtf8File(const std::wstring& path)
    {
        HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f == INVALID_HANDLE_VALUE) return std::string();
        std::string s;
        char buf[4096];
        DWORD got = 0;
        while (ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0 && s.size() < 65536) s.append(buf, got);
        CloseHandle(f);
        if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s.erase(0, 3);
        return s;
    }

    void finish(const std::shared_ptr<Take>& take, const std::string& text, const std::string& error)
    {
        std::lock_guard<std::mutex> lock(take->m);
        take->text = text;
        take->error = error;
        take->writing = false;
        take->done = true;
    }

    /** Whisper turns the take into text. Runs on the recording thread. */
    void transcribe(const std::shared_ptr<Take>& take, std::vector<int16_t>& pcm)
    {
        const std::wstring dir = dataDir(true);
        if (dir.empty() || !modelPresent())
        {
            finish(take, std::string(), "The speech model is missing. Click the mic to download it again.");
            return;
        }
        // Loud enough to hear: Whisper copes with quiet sound, not with sound
        // near the floor. The laptop this was written for gave 8 out of 100.
        int peak = 1;
        for (int16_t s : pcm) peak = (std::max)(peak, std::abs((int)s));
        const double gain = (std::min)(20.0, 29000.0 / peak);
        if (gain > 1.05)
        {
            for (int16_t& s : pcm)
            {
                const double v = s * gain;
                s = (int16_t)(std::max)(-32767.0, (std::min)(32767.0, v));
            }
        }
        const std::wstring wav = dir + L"\\take.wav";
        const std::wstring txt = dir + L"\\take.txt";
        DeleteFileW(txt.c_str());
        if (!writeWav(wav, pcm))
        {
            finish(take, std::string(), "What was said could not be saved for Whisper to read.");
            return;
        }
        {
            std::lock_guard<std::mutex> lock(take->m);
            take->writing = true;
        }

        const unsigned hw = std::thread::hardware_concurrency();
        const unsigned threads = (std::max)(2u, (std::min)(8u, hw / 2));
        std::wstring args = L"-m ";
        args += MODEL_FILE;
        args += L" -f take.wav -l en -nt -np -sns -ng -t " + std::to_wstring(threads) + L" -otxt -of take";

        const double began = now();
        DWORD error = 0;
        HANDLE p = launch(whisperExe(), args, dir, L"whisper.log", error);
        if (!p)
        {
            DeleteFileW(wav.c_str());
            finish(take, std::string(), "Whisper could not be started (error " + std::to_string(error) +
                   "). Reinstalling Lumen puts it back.");
            return;
        }
        bool cancelled = false;
        bool timedOut = false;
        for (;;)
        {
            if (WaitForSingleObject(p, 100) == WAIT_OBJECT_0) break;
            {
                std::lock_guard<std::mutex> lock(take->m);
                cancelled = take->cancel;
            }
            if (cancelled || now() - began > WRITE_TIMEOUT)
            {
                timedOut = !cancelled;
                TerminateProcess(p, 1);
                WaitForSingleObject(p, 5000);
                break;
            }
        }
        DWORD code = 0;
        GetExitCodeProcess(p, &code);
        CloseHandle(p);
        // What was said is not kept: the recording goes as soon as it is read.
        DeleteFileW(wav.c_str());
        const std::string raw = readUtf8File(txt);
        DeleteFileW(txt.c_str());

        if (cancelled)
        {
            finish(take, std::string(), std::string());
            return;
        }
        if (timedOut)
        {
            finish(take, std::string(), "Whisper took too long to write down what was said.");
            return;
        }
        if (code != 0)
        {
            const std::string said = firstLine(dir + L"\\whisper.log");
            LL_WARNS("LumenAISpeech") << "whisper-cli exited with " << code << LL_ENDL;
            finish(take, std::string(), "Whisper could not write down what was said (code " +
                   std::to_string(code) + (said.empty() ? std::string() : ": " + said) + ").");
            return;
        }
        DeleteFileW((dir + L"\\whisper.log").c_str());
        const std::string text = cleaned(raw);
        LL_INFOS("LumenAISpeech") << "Whisper wrote " << text.size() << " characters in "
                                  << (int)((now() - began) * 1000) << " ms" << LL_ENDL;
        finish(take, text, std::string());
    }

    /** Records one take until told to stop, then has Whisper write it down. */
    void record(std::shared_ptr<Take> take)
    {
        HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        WAVEFORMATEX fmt = {};
        fmt.wFormatTag = WAVE_FORMAT_PCM;
        fmt.nChannels = 1;
        fmt.nSamplesPerSec = RATE;
        fmt.wBitsPerSample = 16;
        fmt.nBlockAlign = 2;
        fmt.nAvgBytesPerSec = RATE * 2;

        HWAVEIN in = nullptr;
        const MMRESULT opened = waveInOpen(&in, WAVE_MAPPER, &fmt, (DWORD_PTR)ready, 0, CALLBACK_EVENT);
        if (opened != MMSYSERR_NOERROR)
        {
            LL_WARNS("LumenAISpeech") << "waveInOpen failed: " << opened << LL_ENDL;
            CloseHandle(ready);
            finish(take, std::string(), opened == MMSYSERR_ALLOCATED
                   ? std::string("The microphone is in use by another program.")
                   : std::string(NO_MICROPHONE));
            return;
        }
        LL_INFOS("LumenAISpeech") << "Listening" << LL_ENDL;
        {
            std::lock_guard<std::mutex> lock(take->m);
            take->opened = true;
        }

        const int BUFFERS = 8;
        const int SAMPLES = RATE / 10;   // 100 ms each
        std::vector<std::vector<int16_t>> data(BUFFERS, std::vector<int16_t>(SAMPLES));
        std::vector<WAVEHDR> headers(BUFFERS);
        for (int i = 0; i < BUFFERS; ++i)
        {
            WAVEHDR& h = headers[i];
            memset(&h, 0, sizeof(h));
            h.lpData = (LPSTR)data[i].data();
            h.dwBufferLength = SAMPLES * sizeof(int16_t);
            waveInPrepareHeader(in, &h, sizeof(h));
            waveInAddBuffer(in, &h, sizeof(h));
        }
        waveInStart(in);

        std::vector<int16_t> pcm;
        pcm.reserve(RATE * 60);
        // Loudness, in 20 ms frames: the quietest so far is the room, and
        // speech is clearly above it. The floor rises slowly, so a fan that
        // starts does not read as somebody talking forever.
        const int FRAME = RATE / 50;
        double floorLevel = -1.0;
        int loudRun = 0;
        long long firstVoice = -1, lastVoiceAt = -1;
        bool anyNonZero = false;
        bool cancelled = false;

        for (bool stopping = false; !stopping; )
        {
            WaitForSingleObject(ready, 50);
            {
                std::lock_guard<std::mutex> lock(take->m);
                stopping = take->stop || take->cancel;
                cancelled = take->cancel;
            }
            for (int i = 0; i < BUFFERS; ++i)
            {
                WAVEHDR& h = headers[i];
                if (!(h.dwFlags & WHDR_DONE)) continue;
                const int got = (int)(h.dwBytesRecorded / sizeof(int16_t));
                const int16_t* s = data[i].data();
                for (int f = 0; f + FRAME <= got; f += FRAME)
                {
                    double sum = 0.0;
                    for (int k = 0; k < FRAME; ++k)
                    {
                        const double v = s[f + k];
                        sum += v * v;
                        if (s[f + k] != 0) anyNonZero = true;
                    }
                    const double rms = std::sqrt(sum / FRAME);
                    if (floorLevel < 0.0 || rms < floorLevel) floorLevel = rms;
                    else floorLevel += (rms - floorLevel) * 0.002;
                    const bool loud = rms > (std::max)(floorLevel * 3.0, 60.0);
                    loudRun = loud ? loudRun + 1 : 0;
                    const long long at = (long long)pcm.size() + f;
                    if (loudRun >= 3)   // 60 ms of it, not a click
                    {
                        if (firstVoice < 0) firstVoice = at;
                        lastVoiceAt = at;
                        std::lock_guard<std::mutex> lock(take->m);
                        take->any = true;
                        take->lastVoice = now();
                    }
                }
                pcm.insert(pcm.end(), s, s + got);
                h.dwFlags &= ~WHDR_DONE;
                if (!stopping) waveInAddBuffer(in, &h, sizeof(h));
            }
        }
        waveInReset(in);
        for (WAVEHDR& h : headers) waveInUnprepareHeader(in, &h, sizeof(h));
        waveInClose(in);
        CloseHandle(ready);

        if (cancelled)
        {
            finish(take, std::string(), std::string());
            return;
        }
        if (!anyNonZero)
        {
            std::lock_guard<std::mutex> lock(take->m);
            take->silenceOnly = true;
            take->done = true;
            return;
        }
        if (firstVoice < 0)
        {
            finish(take, std::string(), std::string());   // nothing said
            return;
        }
        // From a little before the first word to a little after the last.
        const long long from = (std::max)(0LL, firstVoice - RATE * 3 / 10);
        const long long to = (std::min)((long long)pcm.size(), lastVoiceAt + RATE / 2);
        std::vector<int16_t> spoken(pcm.begin() + from, pcm.begin() + to);
        transcribe(take, spoken);
    }

    /** Is Windows' own language English? Then nobody needs telling. */
    bool windowsSpeaksEnglish()
    {
        return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_ENGLISH;
    }
}

// ---- the public half --------------------------------------------------------

bool LumenAISpeech::supported()
{
    // A build without Whisper -- built by hand rather than by the workflow --
    // has no mic button, as on a computer with no speech engine at all.
    static const bool there = fileSize(whisperExe()) > 0;
    return there;
}

bool LumenAISpeech::listening()
{
    return sPhase != IDLE;
}

bool LumenAISpeech::start(const std::string& /*language: English only here*/, std::string& why)
{
    if (sPhase != IDLE) return true;
    if (!supported())
    {
        why = "This copy of Lumen has no Whisper, so it cannot listen. Reinstalling Lumen puts it back.";
        return false;
    }
    if (!modelPresent())
    {
        why = "Speech recognition is not set up yet. Click the mic to set it up.";
        return false;
    }
    DeleteFileW(inData(OLD_MODEL_FILE).c_str());   // the first model, if a test left it
    sTake = std::make_shared<Take>();
    std::thread(record, sTake).detach();
    sPhase = LISTENING;
    sStarted = now();
    sSaidWriting = false;
    return true;
}

void LumenAISpeech::stop()
{
    if (sPhase != LISTENING || !sTake) return;
    std::lock_guard<std::mutex> lock(sTake->m);
    sTake->stop = true;
    sPhase = WRITING;
}

void LumenAISpeech::cancel()
{
    if (sTake)
    {
        std::lock_guard<std::mutex> lock(sTake->m);
        sTake->cancel = true;
    }
    sTake.reset();   // the thread keeps its own copy and cleans up alone
    sPhase = IDLE;
}

LumenAISpeech::Update LumenAISpeech::poll()
{
    Update u;
    if (sPhase == IDLE || !sTake) return u;

    if (!sEnglishNoted && !windowsSpeaksEnglish())
    {
        sEnglishNoted = true;
        u.note = "Whisper on Windows understands English, so speak to the assistant in English.";
    }

    bool done = false, any = false, writing = false, silenceOnly = false;
    double lastVoice = 0.0;
    std::string text, error;
    {
        std::lock_guard<std::mutex> lock(sTake->m);
        done = sTake->done;
        any = sTake->any;
        writing = sTake->writing;
        silenceOnly = sTake->silenceOnly;
        lastVoice = sTake->lastVoice;
        text = sTake->text;
        error = sTake->error;
    }

    if (!done)
    {
        if (sPhase == LISTENING)
        {
            const double t = now();
            const char* why = nullptr;
            if (any && t - lastVoice > PAUSE_ENDS) why = "a pause";
            else if (t - sStarted > LONGEST)       why = "the longest take";
            else if (!any && t - sStarted > FIRST_WORD) why = "no first word";
            if (why)
            {
                LL_INFOS("LumenAISpeech") << "Stopping after " << why << LL_ENDL;
                stop();
            }
        }
        if (writing && !sSaidWriting)
        {
            sSaidWriting = true;
            u.activity = "Writing down what you said...";
        }
        return u;
    }

    sTake.reset();
    sPhase = IDLE;
    u.finished = true;
    u.changed = !text.empty();
    u.text = text;
    if (silenceOnly)
    {
        u.error = ONLY_SILENCE;
    }
    else if (!error.empty())
    {
        u.error = error;
    }
    else if (text.empty())
    {
        u.nothing_heard = true;
        u.error = NOTHING_HEARD;
    }
    return u;
}

std::vector<std::pair<std::string, std::string>> LumenAISpeech::languages()
{
    return { { "en", "English" } };
}

std::string LumenAISpeech::ownLanguage()
{
    return "en";
}

// ---- setting it up ----------------------------------------------------------

LumenAISpeech::SetupState LumenAISpeech::setupState()
{
    SetupState s;
    s.total = MODEL_BYTES;
    bool running = false;
    std::string error;
    {
        std::lock_guard<std::mutex> lock(sDownload->m);
        running = sDownload->running;
        error = sDownload->error;
    }
    if (running)
    {
        s.state = Setup::Downloading;
        s.done = (std::max)(0LL, fileSize(inData(MODEL_PART)));
        return s;
    }
    if (modelPresent())
    {
        s.state = Setup::Ready;
        s.done = MODEL_BYTES;
        return s;
    }
    s.state = error.empty() ? Setup::NotInstalled : Setup::Failed;
    s.error = error;
    return s;
}

bool LumenAISpeech::startSetup(std::string& why)
{
    if (!supported())
    {
        why = "This copy of Lumen has no Whisper, so there is nothing to set up.";
        return false;
    }
    if (dataDir(true).empty())
    {
        why = "Windows did not say where this user's local files go (LOCALAPPDATA).";
        return false;
    }
    if (fileSize(curlExe()) <= 0)
    {
        why = "Windows' own download program, curl.exe, is not on this computer. It comes "
              "with Windows 10 from 2018 onwards.";
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(sDownload->m);
        if (sDownload->running) return true;
        sDownload->running = true;
        sDownload->cancel = false;
        sDownload->error.clear();
    }
    std::thread(runDownload).detach();
    return true;
}

void LumenAISpeech::cancelSetup()
{
    std::lock_guard<std::mutex> lock(sDownload->m);
    if (sDownload->running) sDownload->cancel = true;
}

void LumenAISpeech::removeSetup()
{
    cancelSetup();
    if (sPhase != IDLE) cancel();
    DeleteFileW(inData(MODEL_FILE).c_str());
    DeleteFileW(inData(MODEL_PART).c_str());
    DeleteFileW(inData(OLD_MODEL_FILE).c_str());
    std::lock_guard<std::mutex> lock(sDownload->m);
    sDownload->error.clear();
    LL_INFOS("LumenAISpeech") << "Speech model removed" << LL_ENDL;
}

std::string LumenAISpeech::setupSize()
{
    return "190 MB";
}
