/**
 * @file fsaiindex.cpp
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

#include "llviewerprecompiledheaders.h"

#include "fsaiindex.h"

#include "llinventorymodel.h"
#include "llinventoryobserver.h"
#include "llinventoryfunctions.h"
#include "lltimer.h"
#include "llviewerinventory.h"

#include <algorithm>
#include <unordered_map>
#include <sstream>

namespace
{
    std::string lowered(const std::string& in)
    {
        std::string out(in);
        std::transform(out.begin(), out.end(), out.begin(),
                       [](unsigned char c) { return (char)std::tolower(c); });
        return out;
    }

    std::vector<std::string> wordsOf(const std::string& s)
    {
        std::vector<std::string> out;
        std::istringstream ss(s);
        std::string w;
        while (ss >> w)
        {
            out.push_back(w);
        }
        return out;
    }

    bool isWordEdge(const std::string& s, size_t pos, size_t len)
    {
        const bool left  = (pos == 0) || !isalnum((unsigned char)s[pos - 1]);
        const size_t end = pos + len;
        const bool right = (end >= s.size()) || !isalnum((unsigned char)s[end]);
        return left && right;
    }

    /** Everything, including the trash: an item there is still findable. */
    class EverythingFunctor : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            return item != nullptr;
        }
    };
}

/**
 * Marks the index stale when inventory changes.
 *
 * Stale rather than rebuilt: wearing one item fires a change, and rebuilding
 * 65,621 entries on each would be worse than the problem being solved. The cost
 * is paid once, on the next search that actually needs it.
 */
class FSAIIndex::Watcher : public LLInventoryObserver
{
public:
    void changed(U32 mask) override
    {
        // LABEL, ADD, REMOVE and STRUCTURE all alter what a search should find.
        // The rest (calling cards, sort order, UI rebuilds) do not.
        if (mask & (LLInventoryObserver::LABEL | LLInventoryObserver::ADD
                    | LLInventoryObserver::REMOVE | LLInventoryObserver::STRUCTURE))
        {
            FSAIIndex::instance().invalidate();
        }
    }
};

FSAIIndex::FSAIIndex()
{
    mWatcher = new Watcher();
    gInventory.addObserver(mWatcher);
}

FSAIIndex::~FSAIIndex()
{
    if (mWatcher)
    {
        gInventory.removeObserver(mWatcher);
        delete mWatcher;
        mWatcher = nullptr;
    }
}

void FSAIIndex::build()
{
    LLTimer timer;

    mEntries.clear();

    LLInventoryModel::cat_array_t  cats;
    LLInventoryModel::item_array_t items;
    EverythingFunctor everything;

    // No cap: this is the one walk that is supposed to see all of it.
    gInventory.collectDescendentsIf(LLUUID::null, cats, items, true, everything);

    mEntries.reserve(items.size());
    for (const auto& it : items)
    {
        if (!it)
        {
            continue;
        }
        Entry e;
        e.id      = it->getUUID();
        e.lname   = lowered(it->getName());
        e.type    = it->getType();
        e.creator  = it->getPermissions().getCreator();
        e.acquired = it->getCreationDate();
        mEntries.push_back(std::move(e));
    }

    mBuilt = true;

    LL_INFOS("FSAIIndex") << "Indexed " << mEntries.size() << " items in "
                          << (S32)(timer.getElapsedTimeF32() * 1000.f) << " ms" << LL_ENDL;
}

size_t FSAIIndex::size()
{
    if (!mBuilt)
    {
        build();
    }
    return mEntries.size();
}

S32 FSAIIndex::score(const std::string& lname,
                     const std::vector<std::string>& words,
                     const std::string& whole)
{
    S32 s = 0;

    if (lname == whole)
    {
        s += 10000;                     // the exact thing asked for
    }
    else if (lname.rfind(whole, 0) == 0)
    {
        s += 4000;                      // "tapi skirt" -> "Tapi Skirt - Maitreya"
    }
    else if (lname.find(whole) != std::string::npos)
    {
        s += 2000;                      // the words together, somewhere inside
    }

    for (const std::string& w : words)
    {
        const size_t at = lname.find(w);
        if (at == std::string::npos)
        {
            continue;                   // cannot happen: matching ran first
        }
        s += isWordEdge(lname, at, w.size()) ? 400 : 120;

        if (at == 0)
        {
            s += 200;                   // a word the name opens with
        }
    }

    // Among otherwise equal names the shorter one is usually the thing itself.
    // Capped so it can never outweigh a real match.
    s += (S32)std::max<size_t>(0, 200 - std::min<size_t>(200, lname.size()));

    // Things that are not the garment, however well the name matches.
    //
    // Twice now this list was short by one and the wrong item was worn: the
    // demo, then "Tapi Skirt HUD (wear me)", which took all eight top places
    // for "tapi skirt" because it is *shorter* than "Tapi Skirt - Maitreya
    // LaraX" and the length rule therefore preferred it. The pattern is that
    // an accessory is named after the garment and is usually the tidier name,
    // so plain name matching ranks it above the thing itself, every time.
    //
    // "una skirt" put "UNA. Prya Skirt Larax DEMO" above "UNA. Prya Skirt
    // LaraX Teal" -- the two names are the same length, so the shorter-name
    // rule could not separate them and the winner was whichever came first in
    // inventory. A demo is the one item nobody means when they say "wear my
    // skirt": it is the trial copy, usually with a watermark or a timer.
    //
    // Penalised rather than hidden, and only when the query does not ask for
    // it -- "wear the demo" must still work -- and only on a word boundary, so
    // a product genuinely called something ending in those letters is safe.
    static const struct { const char* word; S32 cost; } NOT_THE_THING[] = {
        { "demo",     3000 },
        { "hud",      3000 },   // the fitting interface, not the garment
        { "wear me",  2500 },   // and neither is the thing that says so
        { "add me",   2500 },
        { "unpacker", 2500 },
        { "unpack",   2500 },
        { "box",      1500 },   // the packaging, not the contents
        { "boxed",    1500 },
        { "resizer",  1500 },
        { "resize",   1200 },
    };

    for (const auto& n : NOT_THE_THING)
    {
        const std::string w(n.word);

        // Asked for explicitly? Then it is the thing.
        if (whole.find(w) != std::string::npos)
        {
            continue;
        }

        size_t at = lname.find(w);
        while (at != std::string::npos)
        {
            if (isWordEdge(lname, at, w.size()))
            {
                s -= n.cost;
                break;
            }
            at = lname.find(w, at + 1);
        }
    }

    return s;
}

std::vector<FSAIIndex::Hit> FSAIIndex::search(const std::string& query,
                                              LLAssetType::EType kind,
                                              const LLUUID&      creator_id,
                                              size_t             limit,
                                              size_t&            total_matches,
                                              Order              order)
{
    if (!mBuilt)
    {
        build();
    }

    total_matches = 0;

    const std::string whole = lowered(query);
    const std::vector<std::string> words = wordsOf(whole);

    std::vector<Hit> hits;
    hits.reserve(std::min<size_t>(limit * 4, 512));
    std::unordered_map<std::string, size_t> seen;

    for (const Entry& e : mEntries)
    {
        if (kind != LLAssetType::AT_NONE && e.type != kind)
        {
            continue;
        }
        if (creator_id.notNull() && e.creator != creator_id)
        {
            continue;
        }

        // Every word, anywhere, in any order. Findings 59: matching the query
        // as one run of characters missed almost everything, because Second
        // Life names are brand, punctuation, product and body fit.
        bool all = true;
        for (const std::string& w : words)
        {
            if (e.lname.find(w) == std::string::npos)
            {
                all = false;
                break;
            }
        }
        if (!all)
        {
            continue;
        }

        ++total_matches;

        const S32 sc = score(e.lname, words, whole);

        // Collapse items sharing a name as we go, rather than afterwards.
        //
        // Six copies of "Tapi Skirt - Legacy (all)" took six of the eight
        // places a search returns, and the LaraX fit the author was actually
        // wearing never appeared. Duplicates are truthful and useless -- the
        // same garment unpacked twice -- and each one costs a slot that could
        // have shown a different body fit.
        //
        // Done here because the obvious version (group the hits afterwards)
        // needs a name lookup per hit, and with 1,658 matches over 72,431
        // entries that is a quadratic walk on the frame loop. One hash lookup
        // per match instead.
        auto found = seen.find(e.lname);
        if (found != seen.end())
        {
            Hit& existing = hits[found->second];
            ++existing.copies;
            if (sc > existing.score)
            {
                existing.id       = e.id;
                existing.score    = sc;
                existing.acquired = e.acquired;
            }
            else if (e.acquired > existing.acquired)
            {
                // Same name and no better match: keep the newest, so "wear my
                // X" reaches for the copy most recently acquired.
                existing.acquired = e.acquired;
            }
            continue;
        }

        Hit h;
        h.id       = e.id;
        h.score    = sc;
        h.acquired = e.acquired;
        seen[e.lname] = hits.size();
        hits.push_back(h);
    }

    // Best first, and only then cut. This is the whole difference: the walk
    // this replaces cut first and never saw the rest.
    // Sorting by date still ranks first, so "the newest UNA skirt" means the
    // newest of the things that actually matched rather than the newest item
    // that happens to contain the letters.
    if (order == BY_NEWEST)
    {
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b)
                  { return a.acquired != b.acquired ? a.acquired > b.acquired
                                                    : a.score > b.score; });
    }
    else if (order == BY_OLDEST)
    {
        std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b)
                  { return a.acquired != b.acquired ? a.acquired < b.acquired
                                                    : a.score > b.score; });
    }
    else
    {
        std::sort(hits.begin(), hits.end(),
                  [](const Hit& a, const Hit& b) { return a.score > b.score; });
    }

    if (hits.size() > limit)
    {
        hits.resize(limit);
    }
    return hits;
}
