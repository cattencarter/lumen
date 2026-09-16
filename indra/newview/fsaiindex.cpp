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
#include <cstring>
#include <cctype>

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
        // The inventory model OWNS every observer registered with it:
        // cleanupInventory() pops the list and deletes each one. It runs at
        // llappviewer.cpp:2236, and singletons are torn down at :2586 -- so by
        // the time we get here our watcher has already been deleted, and
        // removing and deleting it again was a double free. It crashed on
        // quit, which is the one moment nobody is watching the screen.
        //
        // containsObserver only compares pointer values inside a set and never
        // dereferences, so it is safe to ask with a stale pointer: false means
        // the model has already destroyed it and there is nothing left to do.
        // True means we are being torn down early, while the model is still
        // alive, and then the observer really is ours to remove.
        if (gInventory.containsObserver(mWatcher))
        {
            gInventory.removeObserver(mWatcher);
            delete mWatcher;
        }
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

    std::map<LLUUID, std::string> path_cache;
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
        e.lfolder = folderPathLower(it->getParentUUID(), path_cache);
        e.type    = it->getType();
        e.creator  = it->getPermissions().getCreator();
        e.acquired = it->getCreationDate();
        mEntries.push_back(std::move(e));
    }

    // The vocabulary, for correcting a word that matches nothing. Built here
    // because it is the one walk that already has every name in hand.
    {
        std::set<std::string> distinct;
        for (const Entry& e : mEntries)
        {
            for (const std::string* src : { &e.lname, &e.lfolder })
            {
                std::string word;
                for (char c : *src)
                {
                    if (isalnum((unsigned char)c))
                    {
                        word += c;
                    }
                    else
                    {
                        if (word.size() >= 4) distinct.insert(word);
                        word.clear();
                    }
                }
                if (word.size() >= 4) distinct.insert(word);
            }
        }
        mTokens.assign(distinct.begin(), distinct.end());
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

/**
 * The body fits worth knowing about, longest first so "larax" is found before
 * "lara" -- they are different bodies and the shorter one is a prefix of the
 * other.
 *
 * This is not a complete list of every body ever sold and does not need to be:
 * it only has to recognise the fit the wearer is actually in. An unknown fit
 * yields "", the preference never engages, and ranking is exactly as it was.
 */
static const char* const BODY_FITS[] = {
    "hourglass", "physique", "maitreya", "gianni", "legacy", "reborn", "kupra",
    "belleza", "slink", "ebody", "erika", "waifu", "peach", "juicy", "freya",
    "isis", "venus", "larax", "lara", "jake"
};

/**
 * The lowercased path of a category, memoised.
 *
 * Walks up until it meets a folder already in the cache, then fills in what it
 * passed on the way back down, so building 65,000 paths costs each folder once
 * rather than once per item inside it.
 */
std::string FSAIIndex::folderPathLower(const LLUUID& cat_id,
                                       std::map<LLUUID, std::string>& cache)
{
    std::vector<LLUUID> chain;
    std::string path;
    LLUUID cur = cat_id;

    while (cur.notNull())
    {
        const std::map<LLUUID, std::string>::const_iterator f = cache.find(cur);
        if (f != cache.end())
        {
            path = f->second;
            break;
        }
        LLViewerInventoryCategory* cat = gInventory.getCategory(cur);
        if (!cat)
        {
            break;              // the root, or a folder not loaded
        }
        chain.push_back(cur);
        cur = cat->getParentUUID();
    }

    for (std::vector<LLUUID>::const_reverse_iterator it = chain.rbegin();
         it != chain.rend(); ++it)
    {
        LLViewerInventoryCategory* cat = gInventory.getCategory(*it);
        if (!cat)
        {
            continue;
        }
        if (!path.empty())
        {
            path += "/";
        }
        path += lowered(cat->getName());
        cache[*it] = path;
    }
    return path;
}

/** Levenshtein distance, abandoned as soon as it exceeds @a cap. */
static S32 editDistance(const std::string& a, const std::string& b, S32 cap)
{
    const size_t n = a.size(), m = b.size();
    if ((S32)(n > m ? n - m : m - n) > cap)
    {
        return cap + 1;                 // length alone rules it out
    }

    std::vector<S32> prev(m + 1), cur(m + 1);
    for (size_t j = 0; j <= m; ++j) prev[j] = (S32)j;

    for (size_t i = 1; i <= n; ++i)
    {
        cur[0] = (S32)i;
        S32 row_best = cur[0];
        for (size_t j = 1; j <= m; ++j)
        {
            const S32 cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
            cur[j] = std::min(std::min(cur[j - 1] + 1, prev[j] + 1), prev[j - 1] + cost);
            row_best = std::min(row_best, cur[j]);
        }
        if (row_best > cap)
        {
            return cap + 1;             // no cell in this row can recover
        }
        prev.swap(cur);
    }
    return prev[m];
}

bool FSAIIndex::known(const std::string& w) const
{
    for (const std::string& t : mTokens)
    {
        if (t.find(w) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

bool FSAIIndex::splitWord(const std::string& w, std::string& a, std::string& b) const
{
    if (w.size() < 6)
    {
        return false;                   // nothing useful to split
    }

    // Both halves at least three characters, so "friendlist" can become
    // "friend list" but "skirts" cannot become "ski rts".
    for (size_t at = 3; at + 3 <= w.size(); ++at)
    {
        const std::string left  = w.substr(0, at);
        const std::string right = w.substr(at);
        if (known(left) && known(right))
        {
            a = left; b = right;
            return true;
        }
    }
    return false;
}

std::string FSAIIndex::correctWord(const std::string& word)
{
    if (word.size() < 4)
    {
        return std::string();           // too short to correct safely
    }

    // Already present somewhere? Then it is not a typo, whatever it looks
    // like, and must be left exactly alone.
    for (const std::string& t : mTokens)
    {
        if (t.find(word) != std::string::npos)
        {
            return std::string();
        }
    }

    const S32 cap = (word.size() <= 5) ? 1 : 2;
    S32 best = cap + 1;
    std::string best_token;
    bool ambiguous = false;

    for (const std::string& t : mTokens)
    {
        const S32 d = editDistance(word, t, cap);
        if (d > cap)
        {
            continue;
        }
        if (d < best)
        {
            best = d; best_token = t; ambiguous = false;
        }
        else if (d == best && t != best_token)
        {
            ambiguous = true;           // two equally good guesses is no guess
        }
    }

    return ambiguous ? std::string() : best_token;
}

std::string FSAIIndex::fitInName(const std::string& lname)
{
    for (const char* fit : BODY_FITS)
    {
        const size_t at = lname.find(fit);
        if (at != std::string::npos && isWordEdge(lname, at, strlen(fit)))
        {
            return fit;
        }
    }
    return std::string();
}

S32 FSAIIndex::score(const std::string& lname,
                     const std::vector<std::string>& words,
                     const std::string& whole,
                     const std::string& lfolder)
{
    S32 s = 0;

    // A word found only in the folder counts, but for less than one in the
    // item's own name: the folder says which product this came from, the name
    // says what it is. Scored before the name rules so a name match always
    // wins a tie against a folder match.
    for (const std::string& w : words)
    {
        if (lname.find(w) == std::string::npos)
        {
            const size_t at = lfolder.find(w);
            if (at != std::string::npos)
            {
                s += isWordEdge(lfolder, at, w.size()) ? 250 : 80;
            }
        }
    }

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
                                              Order              order,
                                              const std::set<LLUUID>* worn,
                                              const std::string& prefer_fit,
                                              std::vector<std::pair<std::string, std::string> >* corrections)
{
    if (!mBuilt)
    {
        build();
    }

    total_matches = 0;

    const std::string whole = lowered(query);
    std::vector<std::string> words = wordsOf(whole);

    // Correct only what matched nothing at all.
    //
    // "tantacio" is not a substring of "tentacio", so the search returned zero
    // and the assistant reported -- correctly and uselessly -- that there were
    // no such skirts. Nothing about that is the model's doing: it passed what
    // it was given and said what it was told.
    //
    // This runs after exact matching has already failed for a word, so a
    // correctly spelled query is never altered and the worst case it replaces
    // is an empty result.
    {
        std::vector<std::string> rebuilt;
        rebuilt.reserve(words.size() + 1);
        for (const std::string& w : words)
        {
            if (known(w))
            {
                rebuilt.push_back(w);   // matched something: never touched
                continue;
            }

            // A missing space first, because it is the more likely mistake and
            // the more certain one: both halves have to be real.
            std::string a, b;
            if (splitWord(w, a, b))
            {
                if (corrections)
                {
                    corrections->push_back(std::make_pair(w, a + " " + b));
                }
                rebuilt.push_back(a);
                rebuilt.push_back(b);
                continue;
            }

            const std::string fixed = correctWord(w);
            if (!fixed.empty())
            {
                if (corrections)
                {
                    corrections->push_back(std::make_pair(w, fixed));
                }
                rebuilt.push_back(fixed);
                continue;
            }

            rebuilt.push_back(w);       // leave it alone and return nothing
        }
        words.swap(rebuilt);
    }

    // The phrase bonuses compare against the whole query, so they have to see
    // the corrected spelling too -- otherwise a fixed word still loses every
    // exact and prefix bonus and ranks as if it had barely matched.
    std::string corrected_whole = whole;
    if (corrections && !corrections->empty())
    {
        corrected_whole.clear();
        for (const std::string& w : words)
        {
            if (!corrected_whole.empty()) corrected_whole += " ";
            corrected_whole += w;
        }
    }

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
        // The folder counts as part of the name, because in Second Life it
        // routinely IS the name: "*Tentacio* Alba skirt/larax/alba skirt
        // white". Without this, "tentacio skirt" matches only the box.
        bool all = true;
        for (const std::string& w : words)
        {
            if (e.lname.find(w) == std::string::npos &&
                e.lfolder.find(w) == std::string::npos)
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

        S32 sc = score(e.lname, words, corrected_whole, e.lfolder);

        // What the avatar is already wearing outranks everything else that
        // matches, and the bonus is larger than the exact-name bonus on
        // purpose.
        //
        // "skirt" returned an unrelated object literally named SKIRT, because
        // an exact name is worth 10000 and no amount of being the actual
        // garment on the avatar was worth anything at all. A stronger model
        // avoided this by passing worn:true, which the description asks for;
        // a smaller one read the same description and did not. Ranking that
        // only works when the caller reads carefully is ranking that does not
        // work -- the same argument as the demo and HUD penalties above.
        //
        // This can only reorder items that already matched every word of the
        // query, so "blue skirt" is not dragged to a red one that is worn.
        if (worn && worn->count(e.id))
        {
            sc += 12000;
        }

        // The body fit the avatar is already in.
        //
        // "wear one of the tentacio skirts" attached the *box* rather than the
        // garment, and the two scored 969 against 968 -- the winner was
        // whichever sat earlier in inventory. Nothing separated them, because
        // neither "box" nor "boxed" is in the box's name; it is simply called
        // "*Tentacio* Marla skirt black".
        //
        // Asset type cannot separate them either: a rigged mesh garment IS an
        // object, exactly like a box, so preferring clothing over objects
        // would hide most modern clothing in Second Life.
        //
        // What does separate them is that a rigged garment names the body it
        // is cut for and a box does not. Preferring the fit already being worn
        // therefore picks the garment over the box and the right body over the
        // wrong one, with one rule.
        //
        // Below the exact-name bonus on purpose: this is a preference, not an
        // override. It also cannot rescue a demo or a HUD, whose penalties are
        // the same size and cancel it out.
        if (!prefer_fit.empty())
        {
            // The fit is as often a folder ("...\/larax\/skirt") as part of
            // the item's name, and either one means the same thing.
            const size_t at_n = e.lname.find(prefer_fit);
            const size_t at_f = e.lfolder.find(prefer_fit);
            if ((at_n != std::string::npos &&
                 isWordEdge(e.lname, at_n, prefer_fit.size())) ||
                (at_f != std::string::npos &&
                 isWordEdge(e.lfolder, at_f, prefer_fit.size())))
            {
                sc += 3000;
            }
        }

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
