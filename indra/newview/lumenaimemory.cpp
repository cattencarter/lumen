/**
 * @file lumenaimemory.cpp
 * @brief What the assistant should already know about you.
 *
 * $LicenseInfo:firstyear=2026&license=fsviewerlgpl$
 * Lumen Viewer Source Code
 * Copyright (C) 2026, Catten Carter
 * Based on the Phoenix Firestorm Viewer, Copyright (C) 2026,
 * The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "lumenaimemory.h"

#include "llbutton.h"
#include "lldir.h"
#include "llfilepicker.h"
#include "lltextbox.h"
#include "lltexteditor.h"

#include <fstream>
#include <sstream>
#include "llagentui.h"

namespace
{
    const std::string MEMORY_FILE = "ai_memory.txt";
    const std::string REMEMBERED_FILE = "ai_remembered.txt";

    /**
     * Cut to a byte budget without splitting a character in half.
     *
     * Decisions 28 the hard way: a multi-byte character cut down the middle
     * makes the JSON that carries it unparseable, so the whole request fails
     * for a reason that has nothing to do with what was asked. A Danish name
     * is enough to trigger it.
     */
    std::string truncateUtf8(const std::string& in, size_t max_bytes)
    {
        if (in.size() <= max_bytes)
        {
            return in;
        }

        size_t cut = max_bytes;
        // Walk back off any continuation byte (10xxxxxx).
        while (cut > 0 && (static_cast<unsigned char>(in[cut]) & 0xC0) == 0x80)
        {
            --cut;
        }
        return in.substr(0, cut);
    }
}

namespace LumenAIMemory
{
    // **Each avatar has its own.** The author, 2026-09-23: *"each avatar should
    // have it's own memory, that is important."* It used to be one file in the
    // shared settings folder, so Catten and Whisper -- two people with two
    // lives -- were described to the assistant by the same note.
    //
    // **The old shared file is ignored, not carried over**, the author's call:
    // 0.1.1 had barely shipped, so nobody else is likely to have written one,
    // and copying it to every avatar handed one avatar's note to all the rest.
    // Anyone who did can bring it back with Import a file... in the editor.

    bool available()
    {
        return !gDirUtilp->getLindenUserDir().empty();
    }

    std::string path()
    {
        return available() ? gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, MEMORY_FILE)
                           : std::string();
    }

    namespace
    {
        bool readFile(const std::string& file, std::string& out)
        {
            std::ifstream f(file.c_str(), std::ios::binary);
            if (!f.good())
            {
                return false;
            }
            std::ostringstream ss;
            ss << f.rdbuf();
            out = ss.str();
            return true;
        }
    }

    std::string get()
    {
        std::string text;
        if (available() && readFile(path(), text))
        {
            return truncateUtf8(text, MAX_BYTES);
        }
        return std::string();   // nothing saved, or nobody logged in to own it
    }

    bool set(const std::string& text)
    {
        if (!available())
        {
            LL_WARNS("LumenAIMemory") << "Not saved: nobody is logged in, so there is no avatar "
                                         "it would belong to." << LL_ENDL;
            return false;
        }
        const std::string clipped = truncateUtf8(text, MAX_BYTES);

        std::ofstream f(path().c_str(), std::ios::binary | std::ios::trunc);
        if (!f.good())
        {
            LL_WARNS("LumenAIMemory") << "Could not write " << path() << LL_ENDL;
            return false;
        }
        f << clipped;

        LL_INFOS("LumenAIMemory") << "Memory saved for this avatar, " << clipped.size() << " bytes" << LL_ENDL;
        return true;
    }

    namespace
    {
        std::string rememberedPath()
        {
            return available() ? gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, REMEMBERED_FILE)
                               : std::string();
        }

        /** One line, trimmed: an entry that spans lines would become two on the next read. */
        std::string oneLine(const std::string& in)
        {
            std::string out;
            bool space = false;
            for (unsigned char c : in)
            {
                if (c == '\n' || c == '\r' || c == '\t' || c == ' ') { space = !out.empty(); continue; }
                if (space) { out += ' '; space = false; }
                out += (char)c;
            }
            return out;
        }

        std::string today()
        {
            time_t now = time(nullptr);
            char buf[16];
            strftime(buf, sizeof(buf), "%Y-%m-%d", localtime(&now));
            return buf;
        }

        /** An entry the person typed in the editor without a date gets today's. */
        bool dated(const std::string& e)
        {
            return e.size() > 11 && isdigit((unsigned char)e[0]) && e[4] == '-' && e[7] == '-';
        }

        bool writeRemembered(const std::vector<std::string>& entries)
        {
            std::ofstream f(rememberedPath().c_str(), std::ios::binary | std::ios::trunc);
            if (!f.good())
            {
                LL_WARNS("LumenAIMemory") << "Could not write " << rememberedPath() << LL_ENDL;
                return false;
            }
            for (const std::string& e : entries) f << e << "\n";
            return true;
        }

        std::string lowered(std::string s)
        {
            for (char& c : s) c = (char)tolower((unsigned char)c);
            return s;
        }
    }

    std::vector<std::string> remembered()
    {
        std::vector<std::string> out;
        std::string text;
        if (!available() || !readFile(rememberedPath(), text))
        {
            return out;
        }
        std::istringstream ss(text);
        std::string line;
        while (std::getline(ss, line))
        {
            line = oneLine(line);
            if (!line.empty()) out.push_back(line);
        }
        return out;
    }

    size_t usedBytes()
    {
        size_t n = get().size();
        for (const std::string& e : remembered()) n += e.size() + 1;
        return n;
    }

    bool remember(const std::string& text, std::string& entry_out, std::string& why_not)
    {
        if (!available())
        {
            why_not = "Nobody is logged in, so there is no avatar it would belong to.";
            return false;
        }
        const std::string what = oneLine(text);
        if (what.empty())
        {
            why_not = "Nothing to remember -- give the words, as the person said them.";
            return false;
        }
        std::vector<std::string> entries = remembered();
        // The date and one space, then the words -- read back through oneLine,
        // which is why the separator must be exactly one space: with two, the
        // comparison cut a letter off every entry and never matched.
        for (const std::string& e : entries)
        {
            if (lowered(dated(e) ? e.substr(11) : e) == lowered(what))
            {
                entry_out = e;
                why_not = "That is already remembered, word for word, so it was not added twice.";
                return false;
            }
        }
        const std::string entry = today() + " " + what;
        if (usedBytes() + entry.size() + 1 > MAX_BYTES)
        {
            why_not = llformat("Memory is full (%zu of %zu characters, counting the note they wrote "
                               "themselves). Nothing was saved. Tell them, and that they can make "
                               "room in Preferences > AI > Memory.", usedBytes(), MAX_BYTES);
            return false;
        }
        entries.push_back(entry);
        if (!writeRemembered(entries))
        {
            why_not = "The file could not be written.";
            return false;
        }
        entry_out = entry;
        LL_INFOS("LumenAIMemory") << "Remembered one thing for this avatar, "
                                  << what.size() << " characters" << LL_ENDL;
        return true;
    }

    bool forget(const std::string& which, std::string& removed, std::string& why_not,
                std::vector<std::string>& candidates)
    {
        candidates.clear();
        std::vector<std::string> entries = remembered();
        if (entries.empty())
        {
            why_not = "Nothing has been remembered for this avatar.";
            return false;
        }
        const std::string w = oneLine(which);
        size_t hit = std::string::npos;
        bool numeric = !w.empty();
        for (unsigned char c : w) numeric = numeric && isdigit(c);
        if (numeric)
        {
            const size_t n = (size_t)atoi(w.c_str());
            if (n < 1 || n > entries.size())
            {
                why_not = llformat("There is no entry %zu; there are %zu.", n, entries.size());
                candidates = entries;
                return false;
            }
            hit = n - 1;
        }
        else
        {
            // Every word must appear, in any order -- and exactly one entry
            // must hold them. Forgetting the wrong thing is not undoable here.
            std::istringstream ss(lowered(w));
            std::vector<std::string> words;
            std::string word;
            while (ss >> word) words.push_back(word);
            if (words.empty())
            {
                why_not = "Say which entry: its number from recall, or words from it.";
                candidates = entries;
                return false;
            }
            for (size_t i = 0; i < entries.size(); ++i)
            {
                const std::string e = lowered(entries[i]);
                bool all = true;
                for (const std::string& x : words) all = all && e.find(x) != std::string::npos;
                if (all) { candidates.push_back(entries[i]); hit = i; }
            }
            if (candidates.size() != 1)
            {
                why_not = candidates.empty()
                    ? "No remembered entry holds those words. Nothing was removed."
                    : "More than one entry holds those words, so nothing was removed. Ask which.";
                if (candidates.empty()) candidates = entries;
                return false;
            }
            candidates.clear();
        }
        removed = entries[hit];
        entries.erase(entries.begin() + hit);
        if (!writeRemembered(entries))
        {
            why_not = "The file could not be written.";
            return false;
        }
        LL_INFOS("LumenAIMemory") << "Forgot one thing for this avatar." << LL_ENDL;
        return true;
    }

    bool setRemembered(const std::vector<std::string>& in)
    {
        if (!available()) return false;
        std::vector<std::string> entries;
        for (const std::string& raw : in)
        {
            const std::string e = oneLine(raw);
            if (e.empty()) continue;
            entries.push_back(dated(e) ? e : today() + " " + e);
        }
        return writeRemembered(entries);
    }

    std::string extractImportable(const std::string& raw)
    {
        // Not JSON by the look of it: take it as written.
        const size_t first = raw.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
        {
            return std::string();
        }
        if (raw[first] != '{' && raw[first] != '[')
        {
            return raw;
        }

        // A JSON export. Pull out the string values and leave the structure
        // behind. Crude on purpose -- see the header: every product's export
        // shape differs and changes, and a crude reduction that looks untidy
        // beats a precise parser that silently returns nothing after the next
        // format change. The person edits the result before it is saved.
        std::string out;
        bool in_string = false;
        bool escaped   = false;
        std::string current;

        for (char c : raw)
        {
            if (!in_string)
            {
                if (c == '"') { in_string = true; current.clear(); }
                continue;
            }

            if (escaped)
            {
                // Keep the common escapes readable, drop the rest.
                if (c == 'n') current += '\n';
                else if (c == 't') current += ' ';
                else if (c == '"' || c == '\\' || c == '/') current += c;
                escaped = false;
                continue;
            }

            if (c == '\\') { escaped = true; continue; }

            if (c == '"')
            {
                in_string = false;

                // Skip the short tokens an export is mostly made of: keys like
                // "role", ids, timestamps. Prose is what is wanted.
                if (current.size() >= 24 && current.find(' ') != std::string::npos)
                {
                    out += current;
                    out += "\n";
                }
                continue;
            }

            current += c;
        }

        return out.empty() ? raw : out;
    }
}

// ---------------------------------------------------------------------------

LumenAIMemoryFloater::LumenAIMemoryFloater(const LLSD& key)
:   LLFloater(key)
{
}

bool LumenAIMemoryFloater::postBuild()
{
    mText  = getChild<LLTextEditor>("memory");
    mKept  = getChild<LLTextEditor>("remembered");
    mCount = getChild<LLTextBox>("count");

    if (mText)
    {
        mText->setKeystrokeCallback([this](LLTextEditor*) { updateCount(); });
    }
    if (mKept)
    {
        mKept->setKeystrokeCallback([this](LLTextEditor*) { updateCount(); });
    }
    if (LLButton* b = findChild<LLButton>("import_btn"))
    {
        b->setCommitCallback([this](LLUICtrl*, const LLSD&) { onImport(); });
    }
    if (LLButton* b = findChild<LLButton>("save_btn"))
    {
        b->setCommitCallback([this](LLUICtrl*, const LLSD&) { onSave(); });
    }
    if (LLButton* b = findChild<LLButton>("cancel_btn"))
    {
        b->setCommitCallback([this](LLUICtrl*, const LLSD&) { closeFloater(); });
    }
    return true;
}

void LumenAIMemoryFloater::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);

    // Whose note this is, in the title -- two avatars, two notes, and editing
    // the wrong one is the mistake to make impossible to miss.
    const bool can = LumenAIMemory::available();
    if (can)
    {
        std::string who;
        LLAgentUI::buildFullname(who);
        setTitle("What the assistant knows about " + who);
    }
    if (mText)
    {
        mText->setText(can ? LumenAIMemory::get()
                           : std::string("Each avatar has its own memory. Log in as the avatar "
                                         "this is for, then open this again."));
        mText->setEnabled(can);
    }
    if (mKept)
    {
        std::string lines;
        if (can)
        {
            for (const std::string& e : LumenAIMemory::remembered()) lines += e + "\n";
        }
        mKept->setText(lines);
        mKept->setEnabled(can);
    }
    if (LLButton* b = findChild<LLButton>("save_btn"))   b->setEnabled(can);
    if (LLButton* b = findChild<LLButton>("import_btn")) b->setEnabled(can);
    updateCount();
}

void LumenAIMemoryFloater::updateCount()
{
    if (!mCount || !mText)
    {
        return;
    }

    // Both parts share the budget, because both are sent with every message.
    size_t used = mText->getText().size();
    if (mKept) used += mKept->getText().size();
    std::string text = llformat("%zu of %zu characters", used, LumenAIMemory::MAX_BYTES);
    if (used > LumenAIMemory::MAX_BYTES)
    {
        text += "  -- too long; the end will be cut off when you save.";
    }
    mCount->setText(text);
}

void LumenAIMemoryFloater::onImport()
{
    LLFilePicker& picker = LLFilePicker::instance();
    if (!picker.getOpenFile(LLFilePicker::FFLOAD_ALL))
    {
        return;
    }

    std::ifstream f(picker.getFirstFile().c_str(), std::ios::binary);
    if (!f.good())
    {
        if (mCount) mCount->setText(std::string("Could not read that file."));
        return;
    }

    std::ostringstream ss;
    ss << f.rdbuf();

    const std::string usable = LumenAIMemory::extractImportable(ss.str());

    // Into the editor, never straight to disk. An import is a draft: the whole
    // point is that the person trims it to the few hundred words worth paying
    // for on every message from now on.
    if (mText)
    {
        mText->setText(usable);
    }
    updateCount();
}

void LumenAIMemoryFloater::onSave()
{
    if (mText)
    {
        LumenAIMemory::set(mText->getText());
    }
    if (mKept)
    {
        // One entry per line; a line typed here without a date gets today's.
        std::vector<std::string> entries;
        std::istringstream ss(mKept->getText());
        std::string line;
        while (std::getline(ss, line)) entries.push_back(line);
        LumenAIMemory::setRemembered(entries);
    }
    closeFloater();
}
