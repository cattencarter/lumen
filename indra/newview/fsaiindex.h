/**
 * @file fsaiindex.h
 * @brief A searchable picture of the inventory, so the best match wins.
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
#ifndef FS_AIINDEX_H
#define FS_AIINDEX_H

#include "llsingleton.h"
#include "lluuid.h"
#include "llassettype.h"

#include <string>
#include <vector>

/**
 * Every item, flattened and lowercased once, so a search can afford to look at
 * all of them and then pick the best.
 *
 * **Why this exists, and why it is not a database.** `collectDescendentsIf`
 * asks `exceedsLimit()` as it walks and stops the moment the cap is reached
 * (Findings 21). That is the right thing for a bounded walk and the wrong thing
 * for a search: with 65,621 items it returns the first hundred in inventory
 * order, not the hundred anyone wanted. "Wear my black skirt" found whichever
 * hundred items happened to sit early in the tree.
 *
 * Ranking needs every candidate scored, which means touching every item -- and
 * doing *that* per search, dereferencing 65,621 `LLViewerInventoryItem`s and
 * lowercasing their names each time, is exactly the frame-loop cost Findings 21
 * warns about. So the names are lowercased once into a flat array and scanned
 * from there. About five megabytes for this inventory.
 *
 * **No SQLite, no file, no daemon, no Node.** The inventory is already in the
 * viewer's memory; this is a second, cheaper shape for it. Decisions 2 says the
 * product is the viewer and nothing beside it, and a search index that outlives
 * the process would need invalidating against a world that changed while it was
 * not running.
 */
class FSAIIndex : public LLSingleton<FSAIIndex>
{
    LLSINGLETON(FSAIIndex);
    ~FSAIIndex();

public:
    struct Hit
    {
        LLUUID id;
        S32    score = 0;
        time_t acquired = 0;
        S32    copies   = 1;   // how many items share this exact name
    };

    /**
     * The best `limit` items matching every word of `query`, in any order.
     *
     * Unlike the walk it replaces, this looks at everything before choosing,
     * so the answer is the best matches rather than the first found.
     * `total_matches` reports how many matched altogether, which the old path
     * could not know without walking twice.
     */
    enum Order
    {
        BY_BEST,    // how well the name fits -- the default
        BY_NEWEST,  // most recently acquired first
        BY_OLDEST
    };

    std::vector<Hit> search(const std::string& query,
                            LLAssetType::EType  kind,
                            const LLUUID&       creator_id,
                            size_t              limit,
                            size_t&             total_matches,
                            Order               order = BY_BEST);

    /** Drop the index; it rebuilds on the next search. */
    void invalidate() { mBuilt = false; }

    /** How many items are indexed, building first if needed. */
    size_t size();

private:
    struct Entry
    {
        LLUUID             id;
        std::string        lname;     // lowercased once, which is the whole point
        LLAssetType::EType type = LLAssetType::AT_NONE;
        LLUUID             creator;
        time_t             acquired = 0;
    };

    std::vector<Entry> mEntries;
    bool               mBuilt = false;

    void build();

    /**
     * How well a name answers a query.
     *
     * Deliberately simple and explainable, because a ranking nobody can predict
     * is its own kind of wrong: an exact name beats a prefix, a prefix beats
     * whole-word matches, whole words beat matches inside longer words, and
     * among equals the shorter name wins. That last rule is what makes "Tapi
     * Skirt" beat "Tapi Skirt Fatpack Unpacker Box v3".
     */
    static S32 score(const std::string& lname,
                     const std::vector<std::string>& words,
                     const std::string& whole);

    class Watcher;
    Watcher* mWatcher = nullptr;
};

#endif // FS_AIINDEX_H
