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

#include <map>
#include <set>
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

    /**
     * @param worn  Items currently on the avatar, which outrank everything
     *              else that matches. The index cannot know this -- worn
     *              state changes constantly and the index is built once --
     *              so the caller supplies it. Empty is fine.
     */
    std::vector<Hit> search(const std::string& query,
                            LLAssetType::EType  kind,
                            const LLUUID&       creator_id,
                            size_t              limit,
                            size_t&             total_matches,
                            Order               order = BY_BEST,
                            const std::set<LLUUID>* worn = NULL,
                            const std::string&  prefer_fit = std::string(),
                            std::vector<std::pair<std::string, std::string> >* corrections = NULL);

    /**
     * The body-fit token in a name -- "larax", "maitreya", "legacy" -- or "".
     *
     * Public because the caller works out which fit the avatar is wearing by
     * asking this about each worn item, and the vocabulary should live in one
     * place rather than two that drift.
     *
     * @param lname  an already-lowercased name.
     */
    static std::string fitInName(const std::string& lname);

    /**
     * What a query word probably should have been, or "" to leave it alone.
     *
     * Runs ONLY for a word that appears nowhere in any name or folder, so a
     * correctly spelled search is never touched and cannot be made worse: the
     * alternative at that point is an empty result. Allows one edit for a
     * short word and two for a long one, which catches "tantacio" for
     * "tentacio" without pretending "skirt" might have meant "shirt" -- those
     * are one edit apart and both real words, which is why a word that DID
     * match is never reconsidered.
     */
    std::string correctWord(const std::string& word);

    /** Whether @a w appears anywhere in any name or folder. */
    bool known(const std::string& w) const;

    /**
     * Split a run-together word into two that are both known, or false.
     *
     * "friendlist" is not a typo for "friends list" -- it is four edits away,
     * so correctWord() cannot reach it however generous the allowance. A
     * missing space is its own kind of mistake and needs its own rule.
     */
    bool splitWord(const std::string& w, std::string& a, std::string& b) const;

private:
    /** Lowercased path of a category, memoised while building. */
    std::string folderPathLower(const LLUUID& cat_id,
                                std::map<LLUUID, std::string>& cache);
public:

    /** Drop the index; it rebuilds on the next search. */
    void invalidate() { mBuilt = false; }

    /** How many items are indexed, building first if needed. */
    size_t size();

private:
    struct Entry
    {
        LLUUID             id;
        std::string        lname;     // lowercased once, which is the whole point
        /**
         * The folder path, lowercased, e.g. clothing > *Tentacio* Alba skirt > larax.
         *
         * Searchable because in Second Life the brand and the product are
         * routinely in the FOLDER and not in the item: the garment inside is
         * called "alba skirt white", while only the box carries the brand and
         * product in its own name. Matching names alone therefore finds the
         * box and can never find the garment.
         */
        std::string        lfolder;
        LLAssetType::EType type = LLAssetType::AT_NONE;
        LLUUID             creator;
        time_t             acquired = 0;
    };

    std::vector<Entry> mEntries;
    /**
     * Every distinct word appearing in any name or folder, sorted.
     *
     * Only ever consulted for a query word that matched nothing, to find what
     * the user probably meant. See correctWord().
     */
    std::vector<std::string> mTokens;
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
                     const std::string& whole,
                     const std::string& lfolder);

    class Watcher;
    Watcher* mWatcher = nullptr;
};

#endif // FS_AIINDEX_H
