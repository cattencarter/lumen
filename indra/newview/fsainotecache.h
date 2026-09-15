/**
 * @file fsainotecache.h
 * @brief Notecard text kept between sessions, so a search is not a minute long.
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
#ifndef FS_AINOTECACHE_H
#define FS_AINOTECACHE_H

#include "llsingleton.h"
#include "lluuid.h"

#include <string>

struct sqlite3;

/**
 * Notecard bodies, on disk, keyed by item.
 *
 * Searching inside notecards meant fetching every one of 3,691 assets from
 * Second Life's servers on every search: about 74 seconds, repeated in full
 * each time, because nothing survived the session. The text does not change
 * unless someone edits it, so almost all of that was re-fetching what we had
 * already read.
 *
 * **It is a cache and nothing else, and that is what makes it safe.** Every row
 * can be re-fetched from Linden Lab, so the answer to any doubt about the file
 * is to delete it and start again -- never to repair it, and never to hand back
 * text that might be half-written. A cache that can be wrong is worse than no
 * cache, because the failure looks like a notecard whose contents changed.
 *
 * Storing it at all was the author's call (Decisions 62), on the grounds that
 * the same directory already holds full instant-message logs named after the
 * person, in plain text, written without asking. Notecard bodies are less
 * sensitive than that.
 */
class FSAINoteCache : public LLSingleton<FSAINoteCache>
{
    LLSINGLETON(FSAINoteCache);
    ~FSAINoteCache();

public:
    /**
     * The stored body for this item, if we hold it *for this asset*.
     *
     * `asset_id` is checked, not just the item id: editing a notecard gives it
     * a new asset, so the old body is stale rather than missing. That failure
     * would be invisible -- confidently returning yesterday's text -- which
     * makes it the more important of the two to get right.
     *
     * Returns false when there is nothing usable, and the caller fetches.
     */
    bool get(const LLUUID& item_id, const LLUUID& asset_id, std::string& body_out);

    /** Remember a body. Silently does nothing when the cache is unavailable. */
    void put(const LLUUID& item_id, const LLUUID& asset_id,
             const std::string& name, const std::string& body);

    /** Forget one card, for when a fetch shows what we held was wrong. */
    void forget(const LLUUID& item_id);

    /** How many bodies are stored. 0 when unavailable. */
    S32 count();

    /** Whether the cache opened at all. Everything degrades to the live fetch. */
    bool available() const { return mDb != nullptr; }

    /** Throw the whole thing away and start again. */
    void clear();

private:
    sqlite3*    mDb = nullptr;
    std::string mPath;

    bool open();
    void close();

    /**
     * Check the file and rebuild it if anything is wrong.
     *
     * Runs `PRAGMA quick_check` and compares the schema version. Either failing
     * deletes the file -- with its -wal and -shm, which is easy to forget and
     * leaves a half-deleted database behind.
     */
    bool verifyOrRebuild();

    bool exec(const char* sql);
};

#endif // FS_AINOTECACHE_H
