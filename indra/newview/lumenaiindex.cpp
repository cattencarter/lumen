/**
 * @file lumenaiindex.cpp
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

#include "lumenaiindex.h"
#include "lumenainotecache.h"   // <Lumen> landmark destinations

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

    /**
     * Everything, including the trash: an item there is still findable.
     *
     * But not LINKS. A link takes its name and type from the item it points
     * at and keeps its own date, so the worn skirt's link in Current Outfit
     * (or in a saved outfit) collapsed into the real skirt as a "copy", and
     * lent it today's date. The real item is always indexed beside it.
     */
    class EverythingFunctor : public LLInventoryCollectFunctor
    {
    public:
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            return item != nullptr && !item->getIsLinkType();
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
class LumenAIIndex::Watcher : public LLInventoryObserver
{
public:
    void changed(U32 mask) override
    {
        // LABEL, ADD, REMOVE and STRUCTURE all alter what a search should find.
        // The rest (calling cards, sort order, UI rebuilds) do not.
        if (mask & (LLInventoryObserver::LABEL | LLInventoryObserver::ADD
                    | LLInventoryObserver::REMOVE | LLInventoryObserver::STRUCTURE))
        {
            LumenAIIndex::instance().invalidate();
        }
    }
};

LumenAIIndex::LumenAIIndex()
{
    mWatcher = new Watcher();
    gInventory.addObserver(mWatcher);
}

LumenAIIndex::~LumenAIIndex()
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

void LumenAIIndex::build()
{
    LLTimer timer;

    mEntries.clear();

    LLInventoryModel::cat_array_t  cats;
    LLInventoryModel::item_array_t items;
    EverythingFunctor everything;

    // No cap: this is the one walk that is supposed to see all of it.
    //
    // From the user's own root, not from null. Null holds the Linden Library
    // as well, so its free gestures, landmarks and clothes were presented as
    // things the user owns -- and a delete of one reported "moved to Trash"
    // about an item nobody can move. Every other walk starts here too.
    gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items, true, everything);

    // Whether a folder is in the Trash, asked once per folder rather than
    // walking up the tree for each of seventy thousand items.
    const LLUUID trash = gInventory.findCategoryUUIDForType(LLFolderType::FT_TRASH);
    std::map<LLUUID, bool> trash_cache;

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
        e.parent  = it->getParentUUID();   // <Lumen>
        e.type    = it->getType();
        // <Lumen> A landmark is also found by where it GOES: "my landmarks in
        // Rio Solimoes" is a question about destinations, which the name and
        // folder often do not mention. Read in the background after login.
        if (e.type == LLAssetType::AT_LANDMARK)
        {
            const LumenAINoteCache::Destination* d =
                LumenAINoteCache::instance().landmark(it->getAssetUUID());
            if (d && !d->leadsNowhere())
            {
                e.lfolder += " > " + lowered(d->region);
            }
        }
        e.creator  = it->getPermissions().getCreator();
        e.acquired = it->getCreationDate();
        const LLUUID parent = it->getParentUUID();
        std::map<LLUUID, bool>::const_iterator tc = trash_cache.find(parent);
        if (tc == trash_cache.end())
        {
            const bool under = trash.notNull()
                && (parent == trash || gInventory.isObjectDescendentOf(parent, trash));
            tc = trash_cache.insert(std::make_pair(parent, under)).first;
        }
        e.in_trash = tc->second;
        mEntries.push_back(std::move(e));
    }

    // The vocabulary, for correcting a word that matches nothing. Built here
    // because it is the one walk that already has every name in hand.
    {
        std::set<std::string> distinct;
        mWords.clear();
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
                        if (word.size() >= 3) mWords.insert(word);
                        word.clear();
                    }
                }
                if (word.size() >= 4) distinct.insert(word);
                if (word.size() >= 3) mWords.insert(word);
            }
        }
        mTokens.assign(distinct.begin(), distinct.end());
    }

    mBuilt = true;

    LL_INFOS("LumenAIIndex") << "Indexed " << mEntries.size() << " items in "
                          << (S32)(timer.getElapsedTimeF32() * 1000.f) << " ms" << LL_ENDL;
}

size_t LumenAIIndex::size()
{
    if (!mBuilt)
    {
        build();
    }
    return mEntries.size();
}

/**
 * The body fits worth knowing about.
 *
 * **One entry is a prefix of another and that is the trap.** "lara" is a prefix
 * of "larax", and Maitreya Lara and Maitreya LaraX are *different bodies* --
 * clothes cut for one do not fit the other. Measured while building this: a
 * plain substring scan reported "lara" 59 times and "larax" 59 times across
 * 100 skirts, identical counts, because every "lara" hit was really a "larax".
 * The two bodies had been silently merged and nothing anywhere said so.
 *
 * What separates them is the word-boundary test in fitInName, not the order of
 * this list: "lara" inside "larax" is followed by a letter and so is not a
 * word. The order here is therefore free, and must stay free -- fitInName
 * picks by position in the NAME, deliberately (see its comment).
 *
 * This is not a complete list of every body ever sold and does not need to be:
 * it only has to recognise the fit the wearer is actually in. An unknown fit
 * yields "", the preference never engages, and ranking is exactly as it was.
 */
static const char* const BODY_FITS[] = {
    "hourglass", "physique", "maitreya", "gianni", "legacy", "reborn", "kupra",
    "belleza", "slink", "ebody", "erika", "waifu", "peach", "juicy", "freya",
    "isis", "venus", "larax", "lara", "jake",
    // <Lumen> Small avatars, which wear clothes made for them just as a mesh
    // body does -- Whisper's dinkie wears "Medieval Maiden Dinkie Long Skirt",
    // her ferret wears things filed under "av tinies clothes". Without these,
    // a dinkie searching "dress" got human dresses and material swatches.
    // Spelled several ways; canonicalFit() folds them into one body.
    "dinkie", "dinkies", "dinky", "tiny", "tinies"
};

/** <Lumen> One name per body: "dinkies" and "dinky" are the dinkie. */
static std::string canonicalFit(const std::string& fit)
{
    if (fit == "dinkies" || fit == "dinky") return "dinkie";
    if (fit == "tinies")                    return "tiny";
    return fit;
}

/**
 * The lowercased path of a category, memoised.
 *
 * Walks up until it meets a folder already in the cache, then fills in what it
 * passed on the way back down, so building 65,000 paths costs each folder once
 * rather than once per item inside it.
 */
std::string LumenAIIndex::folderPathLower(const LLUUID& cat_id,
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

// static
S32 LumenAIIndex::editDistance(const std::string& a, const std::string& b, S32 cap)
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

bool LumenAIIndex::known(const std::string& w) const
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

bool LumenAIIndex::splitWord(const std::string& w, std::string& a, std::string& b) const
{
    if (w.size() < 6)
    {
        return false;                   // nothing useful to split
    }

    // Both halves at least three characters, and both WHOLE words -- "friend"
    // and "list" -- not merely letters found inside one. <Lumen> This used
    // known(), and in 72,000 names nearly any three letters occur somewhere:
    // "scarves" became "sca rves", "knives" became "kni ves", and each then
    // found nothing.
    for (size_t at = 3; at + 3 <= w.size(); ++at)
    {
        const std::string left  = w.substr(0, at);
        const std::string right = w.substr(at);
        if (isWord(left) && isWord(right))
        {
            a = left; b = right;
            return true;
        }
    }
    return false;
}

bool LumenAIIndex::isWord(const std::string& w) const
{
    if (mWords.count(w) || mWords.count(w + "s"))
    {
        return true;
    }
    return w.size() > 3 && w.back() == 's' && mWords.count(w.substr(0, w.size() - 1));
}

std::string LumenAIIndex::singularOf(const std::string& w) const
{
    std::vector<std::string> tries;
    if (w.size() >= 5 && w.compare(w.size() - 3, 3, "ves") == 0)
    {
        const std::string stem = w.substr(0, w.size() - 3);
        tries.push_back(stem + "fe");   // knives, wives
        tries.push_back(stem + "f");    // scarves, wolves, shelves
    }
    if (w.size() >= 5 && w.compare(w.size() - 3, 3, "ies") == 0)
    {
        tries.push_back(w.substr(0, w.size() - 3) + "y");  // berries, puppies
    }
    for (const std::string& t : tries)
    {
        if (mWords.count(t))
        {
            return t;
        }
    }
    return std::string();
}

std::string LumenAIIndex::correctWord(const std::string& word)
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

/**
 * Does @a text name this fit, as a word rather than as letters inside one?
 *
 * Every occurrence is tried, not just the first. Stopping at one that is not
 * on a word boundary would give up on "Bellezafied Belleza Freya" -- the same
 * shape as the NOT_THE_THING scan, and wrong for the same reason.
 */
static bool namesFitWord(const std::string& text, const std::string& fit);

// <Lumen> Any spelling of that body: a dinkie's "dinky summer dress" is hers.
static bool namesFit(const std::string& text, const std::string& fit)
{
    for (const char* f : BODY_FITS)
    {
        if (canonicalFit(f) == fit && namesFitWord(text, f)) return true;
    }
    return false;
}

static bool namesFitWord(const std::string& text, const std::string& fit)
{
    size_t at = text.find(fit);
    while (at != std::string::npos)
    {
        if (isWordEdge(text, at, fit.size()))
        {
            return true;
        }
        at = text.find(fit, at + 1);
    }
    return false;
}

/**
 * The last two segments of a folder path.
 *
 * **Measured, and it overturned the obvious design.** Asking whether the fit
 * appears anywhere in the path looks right and is useless here, because this
 * inventory files everything under
 *
 *     clothing/amazon new system larax/clothes/skirts/...
 *
 * so "larax" is in the path of every garment in it -- the Legacy one, the
 * Reborn one, all of them. A rule that is true of everything separates nothing,
 * and it would have shipped looking like it worked: plausible items in a
 * confident order, with the wrong body quietly still on top.
 *
 * The distinction that survives contact with a real inventory is **how close
 * the folder is to the item**. An immediate parent called "larax", as in
 * "*Tentacio* Alba skirt/larax/alba skirt white", is the product's own label
 * for what is inside it. A folder four levels up called "amazon new system
 * larax" is where this person keeps things. Two segments, because a fatpack
 * commonly puts the fit one above the colour.
 */
static std::string nearFolder(const std::string& path)
{
    const size_t last = path.rfind('/');
    if (last == std::string::npos || last == 0)
    {
        return path;            // one segment, or a leading slash: all of it
    }
    const size_t prev = path.rfind('/', last - 1);
    return path.substr(prev == std::string::npos ? 0 : prev + 1);
}

/**
 * Which body a name is for. **The LAST fit mentioned wins, not the first.**
 *
 * Second Life names put the maker in front and the body after it, so the word
 * that answers "which body" is the rightmost one: *Maitreya* LaraX, *Belleza*
 * Freya, *Legacy* Perky, *Slink* Hourglass. Taking the first match instead
 * reads the brand and calls it the fit.
 *
 * Found by counting the author's own worn items, which is the one place this
 * has to be right: "Tapi Skirt - Maitreya LaraX" voted *maitreya*, so a LaraX
 * wardrobe came to 2 votes for larax against 1 for maitreya and 1 for belleza
 * -- a majority of two that one detached garment would have tied away, and a
 * tie means no preference at all. Reading it the other way round makes the
 * same evidence 3 to 1.
 *
 * Returns "" when no body is named, which is not a fault: a third of this
 * inventory names none.
 */
std::string LumenAIIndex::fitInName(const std::string& lname)
{
    std::string best;
    size_t      best_at = 0;

    for (const char* fit : BODY_FITS)
    {
        const size_t len = strlen(fit);

        // The rightmost word-edge occurrence of this one. Every occurrence is
        // tried, not just the first: stopping at one that is not on a word
        // boundary would give up on "Bellezafied Belleza Freya", the same
        // shape as the NOT_THE_THING scan and wrong for the same reason.
        size_t at = lname.find(fit);
        size_t last = std::string::npos;
        while (at != std::string::npos)
        {
            if (isWordEdge(lname, at, len))
            {
                last = at;
            }
            at = lname.find(fit, at + 1);
        }

        if (last != std::string::npos && (best.empty() || last > best_at))
        {
            best_at = last;
            best    = fit;
        }
    }
    return canonicalFit(best);   // <Lumen>
}


S32 LumenAIIndex::score(const std::string& lname,
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

std::vector<LumenAIIndex::Hit> LumenAIIndex::search(const std::string& query,
                                              LLAssetType::EType kind,
                                              const LLUUID&      creator_id,
                                              size_t             limit,
                                              size_t&            total_matches,
                                              Order              order,
                                              const std::set<LLUUID>* worn,
                                              const std::string& prefer_fit,
                                              std::vector<std::pair<std::string, std::string> >* corrections,
                                              std::string*       fit_used)
{
    if (!mBuilt)
    {
        build();
    }

    total_matches = 0;

    const std::string whole = lowered(query);
    std::vector<std::string> words = repairedWords(wordsOf(whole), corrections);   // <Lumen>

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

    // If the query names a body itself, stop guessing.
    //
    // "a legacy skirt" from somebody wearing LaraX is not a mistake to be
    // corrected; it is the one case where the person has said outright which
    // body they mean, and a preference inferred from their clothes must not
    // argue with it. The word filter above already keeps only items naming
    // that fit, so nothing here has to replace the preference -- it only has
    // to get out of the way.
    //
    // Same principle as NOT_THE_THING skipping a penalty the query asked for:
    // an inferred rule yields to a stated one.
    std::string wanted_fit = prefer_fit;
    if (!fitInName(corrected_whole).empty())
    {
        wanted_fit.clear();
    }
    if (fit_used)
    {
        *fit_used = wanted_fit;
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
        // A garment labelled for another body is worse than one labelled for
        // none, so the two cannot score alike.
        //
        // Measured across 100 of this account's skirts: 52 name a fit, 14 name
        // one only in the folder, and **34 name none at all**. So a filter is
        // out of the question -- it would hide a third of the wardrobe, and
        // hide it silently, which is this project's worst failure shape. Three
        // outcomes instead, and the middle one is the point:
        //
        //   names the worn fit    +3000   this one fits
        //   names a different fit -3000   this one is known NOT to fit
        //   names no fit at all       0   unknown, and unknown is not bad
        //
        // Symmetric with the boost on purpose, and the same size as the demo
        // and HUD penalties, so a wrong-fit demo cannot climb back.
        //
        // The worn fit is looked for first and a different one only if it is
        // absent, which is what keeps a fatpack safe: "Skirt - Lara & LaraX"
        // names two bodies, and asking "which fit is this?" would answer with
        // whichever comes first in BODY_FITS and demote a garment that fits
        // perfectly well.
        // **The item's own name is asked first and, if it answers, alone.**
        //
        // The fit is as often in a folder ("...\/larax\/skirt") as in the
        // item's name, so both are consulted -- but not equally, and not at
        // the same time. "Jungsu skirt legacy leo" sits inside a folder whose
        // path contains "larax"; taking either as good enough scored it
        // exactly like "Jungsu skirt larax leo" sitting beside it. When a
        // garment names a body in its own name, that IS the answer, and the
        // folder cannot overrule it.
        if (!wanted_fit.empty())
        {
            const std::string nearby = nearFolder(e.lfolder);

            if (namesFit(e.lname, wanted_fit))          sc += 3000;
            else if (!fitInName(e.lname).empty())       sc -= 3000;
            else if (namesFit(nearby, wanted_fit))        sc += 3000;
            else if (!fitInName(nearby).empty())          sc -= 3000;
            // else: nothing anywhere names a body, and that is not a fault.
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
        //
        // **The copy kept is kept whole: id, score and date together.** The
        // date used to be taken from the newest copy while the id stayed with
        // the first, so a list sorted by date was sorted by one copy and
        // showed another.
        auto found = seen.find(e.lname);
        if (found != seen.end())
        {
            Hit& existing = hits[found->second];
            ++existing.copies;

            bool better;
            if (e.in_trash != existing.in_trash)
            {
                // A copy in the Trash is never kept over one that is not:
                // "wear my X" would be handed an item the viewer refuses.
                better = !e.in_trash;
            }
            else if (order == BY_NEWEST && e.acquired != existing.acquired)
            {
                better = e.acquired > existing.acquired;
            }
            else if (order == BY_OLDEST && e.acquired != existing.acquired)
            {
                better = e.acquired < existing.acquired;
            }
            else if (sc != existing.score)
            {
                better = sc > existing.score;
            }
            else
            {
                // Same name and as good a match: keep the newest, so "wear my
                // X" reaches for the copy most recently acquired.
                better = e.acquired > existing.acquired;
            }
            if (better)
            {
                existing.id       = e.id;
                existing.score    = sc;
                existing.acquired = e.acquired;
                existing.in_trash = e.in_trash;
            }
            continue;
        }

        Hit h;
        h.id       = e.id;
        h.score    = sc;
        h.acquired = e.acquired;
        h.in_trash = e.in_trash;
        seen[e.lname] = hits.size();
        hits.push_back(h);
    }

    // Distinct names, not items: copies were collapsed above, so counting
    // them made "showing the best 20 of 30" appear with all 20 on the list.
    total_matches = hits.size();

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

// <Lumen> Whether a word occurs, exactly as typed, in some name or folder --
// the test matching itself uses. The vocabulary is runs of letters and digits,
// so a word with an apostrophe or a hyphen ("whisper's", "men's", "t-shirt")
// was never "known", and was "corrected" to a nearby word that then dropped
// the very item named, with a note saying they mistyped.
bool LumenAIIndex::occurs(const std::string& w) const
{
    for (const Entry& e : mEntries)
    {
        if (e.lname.find(w) != std::string::npos
            || e.lfolder.find(w) != std::string::npos)
        {
            return true;
        }
    }
    return false;
}

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
std::vector<std::string> LumenAIIndex::repairedWords(
    const std::vector<std::string>& words,
    std::vector<std::pair<std::string, std::string> >* corrections)
{
    std::vector<std::string> rebuilt;
    rebuilt.reserve(words.size() + 1);
    for (const std::string& w : words)
    {
        if (known(w) || occurs(w))
        {
            rebuilt.push_back(w);   // matched something: never touched
            continue;
        }

        // <Lumen> A plural the edit allowance cannot reach, before anything
        // else: "scarves" is three edits from "scarf". Reported like any
        // other correction, because the search did change the word.
        const std::string single = singularOf(w);
        if (!single.empty())
        {
            if (corrections)
            {
                corrections->push_back(std::make_pair(w, single));
            }
            rebuilt.push_back(single);
            continue;
        }

        // Then a missing space, before the edit pass, because it is the
        // more likely mistake and the more certain one: both halves have to
        // be real words.
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
    return rebuilt;
}

// <Lumen> See the header. Items, not names: nothing is collapsed here.
std::vector<LumenAIIndex::Match> LumenAIIndex::matchAll(
    const std::string& query,
    LLAssetType::EType kind,
    std::vector<std::pair<std::string, std::string> >* corrections,
    const std::set<LLUUID>* only)
{
    if (!mBuilt)
    {
        build();
    }

    std::vector<std::pair<std::string, std::string> > mine;
    std::vector<std::pair<std::string, std::string> >* fixes = corrections ? corrections : &mine;
    const size_t fixes_before = fixes->size();

    const std::string whole = lowered(query);
    const std::vector<std::string> words = repairedWords(wordsOf(whole), fixes);

    // The phrase bonuses see the corrected spelling, as in search().
    std::string phrase = whole;
    if (fixes->size() > fixes_before)
    {
        phrase.clear();
        for (const std::string& w : words)
        {
            if (!phrase.empty()) phrase += " ";
            phrase += w;
        }
    }

    std::vector<Match> out;
    for (const Entry& e : mEntries)
    {
        if (e.in_trash)
        {
            continue;
        }
        if (kind != LLAssetType::AT_NONE && e.type != kind)
        {
            continue;
        }
        if (only && !only->count(e.id))
        {
            continue;
        }
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
        Match m;
        m.id      = e.id;
        m.parent  = e.parent;
        m.type    = e.type;
        m.creator = e.creator;
        m.score   = words.empty() ? 0 : score(e.lname, words, phrase, e.lfolder);
        out.push_back(m);
    }

    if (!words.empty())
    {
        std::stable_sort(out.begin(), out.end(),
                         [](const Match& a, const Match& b) { return a.score > b.score; });
    }
    return out;
}

// <Lumen> See the header.
namespace
{
    /**
     * @a w as a whole word of @a lname, or with a plural ending on it:
     * "pumpkin" in "pumpkins" and "boxes", never in "pumpkinhead".
     */
    bool wholeWordIn(const std::string& lname, const std::string& w)
    {
        if (w.empty())
        {
            return true;
        }
        for (size_t at = lname.find(w); at != std::string::npos; at = lname.find(w, at + 1))
        {
            if (at > 0 && isalnum((unsigned char)lname[at - 1]))
            {
                continue;
            }
            const size_t end = at + w.size();
            if (isWordEdge(lname, at, w.size()))
            {
                return true;
            }
            for (const char* tail : { "s", "es" })
            {
                const size_t len = strlen(tail);
                if (lname.compare(end, len, tail) == 0 && isWordEdge(lname, at, w.size() + len))
                {
                    return true;
                }
            }
        }
        return false;
    }
}

std::vector<LumenAIIndex::Match> LumenAIIndex::matchWords(
    const std::string& query,
    LLAssetType::EType kind,
    std::vector<Match>* loose,
    std::vector<std::pair<std::string, std::string> >* corrections)
{
    if (!mBuilt)
    {
        build();
    }

    const std::string whole = lowered(query);
    const std::vector<std::string> typed = wordsOf(whole);

    // The wider match, as matchAll() makes it -- only when somebody asks.
    std::vector<std::string> wide;
    std::string phrase = whole;
    if (loose)
    {
        std::vector<std::pair<std::string, std::string> > mine;
        std::vector<std::pair<std::string, std::string> >* fixes = corrections ? corrections : &mine;
        const size_t fixes_before = fixes->size();
        wide = repairedWords(typed, fixes);
        if (fixes->size() > fixes_before)
        {
            phrase.clear();
            for (const std::string& w : wide)
            {
                if (!phrase.empty()) phrase += " ";
                phrase += w;
            }
        }
    }

    std::vector<Match> out;
    for (const Entry& e : mEntries)
    {
        if (e.in_trash)
        {
            continue;
        }
        if (kind != LLAssetType::AT_NONE && e.type != kind)
        {
            continue;
        }
        bool named = true;
        for (const std::string& w : typed)
        {
            if (!wholeWordIn(e.lname, w))
            {
                named = false;
                break;
            }
        }
        // A whole word of the name is in it as typed, so it occurs, so it is
        // never repaired: every named item is in the wider match too.
        if (!named)
        {
            if (!loose)
            {
                continue;
            }
            bool all = true;
            for (const std::string& w : wide)
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
        }
        Match m;
        m.id      = e.id;
        m.parent  = e.parent;
        m.type    = e.type;
        m.creator = e.creator;
        if (named)
        {
            m.score = typed.empty() ? 0 : score(e.lname, typed, whole, e.lfolder);
            out.push_back(m);
        }
        else
        {
            m.score = wide.empty() ? 0 : score(e.lname, wide, phrase, e.lfolder);
            loose->push_back(m);
        }
    }

    auto best = [](const Match& a, const Match& b) { return a.score > b.score; };
    if (!typed.empty())
    {
        std::stable_sort(out.begin(), out.end(), best);
        if (loose)
        {
            std::stable_sort(loose->begin(), loose->end(), best);
        }
    }
    return out;
}
// </Lumen>
