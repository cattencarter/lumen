/**
 * @file fsainotecache.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsainotecache.h"

#include "lldir.h"
#include "lltimer.h"

#include <sqlite3.h>

#include <cstdio>

namespace
{
    const char* CACHE_FILE = "notecard_cache.db";

    // Bumped whenever the shape of `cards` changes. A mismatch rebuilds rather
    // than migrates: this is a cache, and a migration that goes wrong is a
    // subtler problem than a refetch that takes a minute.
    const int SCHEMA_VERSION = 1;

    const char* SCHEMA =
        "CREATE TABLE IF NOT EXISTS meta ("
        "  k TEXT PRIMARY KEY,"
        "  v TEXT NOT NULL);"
        "CREATE TABLE IF NOT EXISTS cards ("
        "  item_id  TEXT PRIMARY KEY,"
        "  asset_id TEXT NOT NULL,"
        "  name     TEXT NOT NULL,"
        "  body     TEXT NOT NULL,"
        "  fetched  INTEGER NOT NULL);";
}

FSAINoteCache::FSAINoteCache()
{
    mPath = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, CACHE_FILE);
    open();
}

FSAINoteCache::~FSAINoteCache()
{
    close();
}

void FSAINoteCache::close()
{
    if (mDb)
    {
        sqlite3_close(mDb);
        mDb = nullptr;
    }
}

bool FSAINoteCache::exec(const char* sql)
{
    if (!mDb)
    {
        return false;
    }
    char* err = nullptr;
    const int rc = sqlite3_exec(mDb, sql, nullptr, nullptr, &err);
    if (rc != SQLITE_OK)
    {
        LL_WARNS("FSAINoteCache") << "SQL failed: " << (err ? err : "(no message)") << LL_ENDL;
        if (err)
        {
            sqlite3_free(err);
        }
        return false;
    }
    return true;
}

bool FSAINoteCache::verifyOrRebuild()
{
    bool ok = true;

    // 1. Is the file itself intact?
    //
    // Measured rather than assumed, 2026-09-15, by corrupting a real database
    // at three offsets and comparing:
    //
    //   damage          quick_check    integrity_check   SELECT count(*)
    //   unused space    ok             ok                400   (nothing broken)
    //   a data page     malformed      malformed         400
    //   deeper          "*** in ..."   same              400
    //
    // Two things that shape this function. `quick_check` caught everything the
    // slower `integrity_check` did, so the fast one is enough. And **a corrupt
    // database still answered SELECT with a plausible number every time** --
    // which is why this has to run at open rather than trusting a query to
    // fail. Serving half-corrupt text as though it were someone's notecard is
    // the failure this whole class of bug looks like.
    //
    // Note the two shapes of failure: severe damage makes sqlite3_step return
    // an error instead of a row, milder damage returns a row that is not "ok".
    // Both must be treated as failure, which is why the else branches below
    // are not oversights.
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mDb, "PRAGMA quick_check;", -1, &st, nullptr) == SQLITE_OK)
    {
        if (sqlite3_step(st) == SQLITE_ROW)
        {
            const unsigned char* answer = sqlite3_column_text(st, 0);
            if (!answer || std::string((const char*)answer) != "ok")
            {
                LL_WARNS("FSAINoteCache") << "Integrity check failed: "
                                          << (answer ? (const char*)answer : "(null)") << LL_ENDL;
                ok = false;
            }
        }
        else
        {
            ok = false;
        }
        sqlite3_finalize(st);
    }
    else
    {
        ok = false;
    }

    // 2. Is it the shape this build understands?
    if (ok)
    {
        int found = 0;
        if (sqlite3_prepare_v2(mDb, "SELECT v FROM meta WHERE k='schema';", -1, &st, nullptr)
            == SQLITE_OK)
        {
            if (sqlite3_step(st) == SQLITE_ROW)
            {
                const unsigned char* v = sqlite3_column_text(st, 0);
                found = v ? atoi((const char*)v) : 0;
            }
            sqlite3_finalize(st);
        }
        if (found != SCHEMA_VERSION)
        {
            LL_INFOS("FSAINoteCache") << "Schema " << found << ", wanted " << SCHEMA_VERSION
                                      << "; rebuilding." << LL_ENDL;
            ok = false;
        }
    }

    if (ok)
    {
        return true;
    }

    // Rebuild. Deleting the database means deleting its write-ahead log and
    // shared-memory file too: leaving those behind next to a fresh file is how
    // a "clean" rebuild comes back corrupt.
    close();
    LLFile::remove(mPath);
    LLFile::remove(mPath + "-wal");
    LLFile::remove(mPath + "-shm");

    if (sqlite3_open(mPath.c_str(), &mDb) != SQLITE_OK)
    {
        LL_WARNS("FSAINoteCache") << "Could not recreate the notecard cache; "
                                     "searches will fetch from the server as before." << LL_ENDL;
        close();
        return false;
    }
    return false;   // caller applies the schema
}

bool FSAINoteCache::open()
{
    if (sqlite3_open(mPath.c_str(), &mDb) != SQLITE_OK)
    {
        LL_WARNS("FSAINoteCache") << "Could not open " << mPath
                                  << "; notecard search will work, slowly." << LL_ENDL;
        close();
        return false;
    }

    // Write-ahead logging plus a full flush. The default journal can leave a
    // torn file if the viewer is killed or the machine loses power, and a
    // viewer being killed is not an unusual event.
    exec("PRAGMA journal_mode=WAL;");
    exec("PRAGMA synchronous=FULL;");
    exec("PRAGMA busy_timeout=2000;");

    const bool intact = verifyOrRebuild();
    if (!mDb)
    {
        return false;       // gone entirely; everything falls back to fetching
    }

    if (!intact)
    {
        exec("PRAGMA journal_mode=WAL;");
        exec("PRAGMA synchronous=FULL;");
    }

    if (!exec(SCHEMA))
    {
        close();
        return false;
    }

    char stamp[64];
    snprintf(stamp, sizeof(stamp),
             "INSERT OR REPLACE INTO meta (k,v) VALUES ('schema','%d');", SCHEMA_VERSION);
    exec(stamp);

    LL_INFOS("FSAINoteCache") << "Notecard cache ready at " << mPath
                              << " holding " << count() << " cards" << LL_ENDL;
    return true;
}

bool FSAINoteCache::get(const LLUUID& item_id, const LLUUID& asset_id, std::string& body_out)
{
    if (!mDb)
    {
        return false;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mDb, "SELECT body FROM cards WHERE item_id=? AND asset_id=?;",
                           -1, &st, nullptr) != SQLITE_OK)
    {
        return false;
    }

    // Both ids, always. Matching the item alone would happily return the text a
    // notecard used to have before it was edited.
    const std::string item = item_id.asString();
    const std::string asset = asset_id.asString();
    sqlite3_bind_text(st, 1, item.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, asset.c_str(), -1, SQLITE_TRANSIENT);

    bool found = false;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        const unsigned char* b = sqlite3_column_text(st, 0);
        const int n = sqlite3_column_bytes(st, 0);
        body_out.assign(b ? (const char*)b : "", b ? (size_t)n : 0);
        found = true;
    }
    sqlite3_finalize(st);
    return found;
}

void FSAINoteCache::put(const LLUUID& item_id, const LLUUID& asset_id,
                        const std::string& name, const std::string& body)
{
    if (!mDb)
    {
        return;
    }

    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mDb,
            "INSERT OR REPLACE INTO cards (item_id,asset_id,name,body,fetched) "
            "VALUES (?,?,?,?,?);", -1, &st, nullptr) != SQLITE_OK)
    {
        return;
    }

    const std::string item  = item_id.asString();
    const std::string asset = asset_id.asString();
    sqlite3_bind_text(st, 1, item.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, asset.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, name.c_str(),  -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, body.c_str(), (int)body.size(), SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, (sqlite3_int64)time(nullptr));

    if (sqlite3_step(st) != SQLITE_DONE)
    {
        LL_WARNS("FSAINoteCache") << "Could not store notecard " << item << LL_ENDL;
    }
    sqlite3_finalize(st);
}

void FSAINoteCache::forget(const LLUUID& item_id)
{
    if (!mDb)
    {
        return;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mDb, "DELETE FROM cards WHERE item_id=?;", -1, &st, nullptr)
        != SQLITE_OK)
    {
        return;
    }
    const std::string item = item_id.asString();
    sqlite3_bind_text(st, 1, item.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(st);
    sqlite3_finalize(st);
}

S32 FSAINoteCache::count()
{
    if (!mDb)
    {
        return 0;
    }
    sqlite3_stmt* st = nullptr;
    if (sqlite3_prepare_v2(mDb, "SELECT COUNT(*) FROM cards;", -1, &st, nullptr) != SQLITE_OK)
    {
        return 0;
    }
    S32 n = 0;
    if (sqlite3_step(st) == SQLITE_ROW)
    {
        n = sqlite3_column_int(st, 0);
    }
    sqlite3_finalize(st);
    return n;
}

void FSAINoteCache::clear()
{
    if (mDb)
    {
        exec("DELETE FROM cards;");
        exec("VACUUM;");
    }
}
