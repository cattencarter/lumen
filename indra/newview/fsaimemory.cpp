/**
 * @file fsaimemory.cpp
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

#include "fsaimemory.h"

#include "llbutton.h"
#include "lldir.h"
#include "llfilepicker.h"
#include "lltextbox.h"
#include "lltexteditor.h"

#include <fstream>
#include <sstream>

namespace
{
    const std::string MEMORY_FILE = "ai_memory.txt";

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

namespace FSAIMemory
{
    std::string path()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, MEMORY_FILE);
    }

    std::string get()
    {
        std::ifstream f(path().c_str(), std::ios::binary);
        if (!f.good())
        {
            return std::string();
        }
        std::ostringstream ss;
        ss << f.rdbuf();
        return truncateUtf8(ss.str(), MAX_BYTES);
    }

    void set(const std::string& text)
    {
        const std::string clipped = truncateUtf8(text, MAX_BYTES);

        std::ofstream f(path().c_str(), std::ios::binary | std::ios::trunc);
        if (!f.good())
        {
            LL_WARNS("FSAIMemory") << "Could not write " << path() << LL_ENDL;
            return;
        }
        f << clipped;

        LL_INFOS("FSAIMemory") << "Memory saved, " << clipped.size() << " bytes" << LL_ENDL;
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

FSAIMemoryFloater::FSAIMemoryFloater(const LLSD& key)
:   LLFloater(key)
{
}

bool FSAIMemoryFloater::postBuild()
{
    mText  = getChild<LLTextEditor>("memory");
    mCount = getChild<LLTextBox>("count");

    if (mText)
    {
        mText->setKeystrokeCallback([this](LLTextEditor*) { updateCount(); });
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

void FSAIMemoryFloater::onOpen(const LLSD& key)
{
    LLFloater::onOpen(key);

    if (mText)
    {
        mText->setText(FSAIMemory::get());
    }
    updateCount();
}

void FSAIMemoryFloater::updateCount()
{
    if (!mCount || !mText)
    {
        return;
    }

    const size_t used = mText->getText().size();
    std::string text = llformat("%zu of %zu characters", used, FSAIMemory::MAX_BYTES);
    if (used > FSAIMemory::MAX_BYTES)
    {
        text += "  -- too long; the end will be cut off when you save.";
    }
    mCount->setText(text);
}

void FSAIMemoryFloater::onImport()
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

    const std::string usable = FSAIMemory::extractImportable(ss.str());

    // Into the editor, never straight to disk. An import is a draft: the whole
    // point is that the person trims it to the few hundred words worth paying
    // for on every message from now on.
    if (mText)
    {
        mText->setText(usable);
    }
    updateCount();
}

void FSAIMemoryFloater::onSave()
{
    if (mText)
    {
        FSAIMemory::set(mText->getText());
    }
    closeFloater();
}
