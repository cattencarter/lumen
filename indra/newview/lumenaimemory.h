/**
 * @file lumenaimemory.h
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
#ifndef LUMEN_AIMEMORY_H
#define LUMEN_AIMEMORY_H

#include "llfloater.h"

#include <string>
#include <vector>

class LLTextEditor;
class LLTextBox;

/**
 * A short standing note about the person, sent with every message.
 *
 * Someone arriving from ChatGPT or Claude has an assistant that already knows
 * who they are, and starting again from nothing is a real loss -- arguably the
 * thing that would send them back. This is the cheap 90% of carrying that
 * across.
 *
 * **There is no API for it, and there is no point pretending otherwise.**
 * Neither ChatGPT nor Claude exposes "give me this user's saved memories", so
 * import means the person supplies the text: copied out of that product's own
 * memory screen, or taken from a data export. What Lumen can do is hold it,
 * keep it small, and put it in front of the model.
 *
 * **Small is the whole design.** This is prepended to every request, so every
 * word is paid for on every message, forever. A full conversation export is
 * tens of megabytes of mostly-irrelevant history and would be both ruinous and
 * useless; a memories list is a few hundred words and is almost all signal.
 * Hence a hard cap, shown as you type rather than enforced by surprise
 * afterwards.
 *
 * One per AVATAR, in that account's own folder -- see get().
 * Stored as plain text in the user's own settings directory, deliberately not
 * in the protected store: it is not a secret, and the person should be able to
 * open it, read it and edit it without going through this window.
 */
namespace LumenAIMemory
{
    /** Bytes. Past this the model is paying rent on text it will not use. */
    const size_t MAX_BYTES = 8000;

    /** Whether an avatar is logged in, so there is someone's note to read or write. */
    bool available();

    /** This avatar's note. Empty when nothing has been saved or nobody is logged in. */
    std::string get();

    /** Overwrite this avatar's note, truncated on a character boundary. False if nobody is logged in. */
    bool set(const std::string& text);

    /** This avatar's file, or empty before login. */
    std::string path();

    /**
     * What the person asked the assistant to remember -- "remember that Kwanita's
     * username is tyria06" -- kept apart from the note they wrote themselves, so
     * the assistant never rewrites their own words. One entry per line, dated,
     * oldest first. Per avatar, like the note, and sharing its MAX_BYTES budget.
     */
    std::vector<std::string> remembered();
    /** Add one entry. False, with a reason, when full, empty or nobody is logged in. */
    bool remember(const std::string& text, std::string& entry_out, std::string& why_not);
    /**
     * Remove one entry: `which` is its number from remembered(), counting from
     * 1, or words that pick out exactly one. More than one match removes
     * nothing and returns the candidates instead.
     */
    bool forget(const std::string& which, std::string& removed, std::string& why_not,
                std::vector<std::string>& candidates);
    /** Replace the whole list, for the editor. */
    bool setRemembered(const std::vector<std::string>& entries);
    /** Note plus list, in bytes, against MAX_BYTES. */
    size_t usedBytes();

    /**
     * Whether this entry was saved by the assistant's own remember this session.
     *
     * Codex reads memory once, when its conversation starts. An entry the
     * assistant saved it has already heard in that conversation; one typed into
     * the Memory window it has not -- and the difference decides whether the
     * person needs telling.
     */
    bool savedByAssistant(const std::string& entry);

    /**
     * Pull usable text out of whatever the person picked.
     *
     * Plain text and Markdown come through as they are. A JSON export is
     * reduced to the text it contains rather than parsed properly, because
     * every product's export shape is different and changes, and a wrong
     * parser fails silently where a crude one merely looks untidy. The result
     * is something to edit, never something to trust unread -- which is why
     * import lands in an editable box rather than saving straight away.
     */
    std::string extractImportable(const std::string& raw);
}

/**
 * The editor behind Preferences > AI > Memory.
 *
 * Its own floater rather than more rows on the preferences panel, which is
 * already within thirty pixels of the height the tab container allows.
 */
class LumenAIMemoryFloater : public LLFloater
{
public:
    LumenAIMemoryFloater(const LLSD& key);

    bool postBuild() override;
    void onOpen(const LLSD& key) override;

private:
    LLTextEditor* mText  = nullptr;
    LLTextEditor* mKept  = nullptr;   // what they asked to be remembered, one per line
    LLTextBox*    mCount = nullptr;

    void onImport();
    void onSave();
    void updateCount();
};

#endif // LUMEN_AIMEMORY_H
