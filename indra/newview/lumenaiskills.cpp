/**
 * @file lumenaiskills.cpp
 * @brief Skills: a routine taught once and carried out on request, step by checked step.
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

#include "lumenaiskills.h"

#include "llagent.h"
#include "llapp.h"
#include "llappearancemgr.h"
#include "llcoros.h"
#include "lldir.h"
#include "lleventcoro.h"
#include "llfile.h"
#include "llinventoryfunctions.h"
#include "llinventorymodel.h"
#include "llinventorymodelbackgroundfetch.h"
#include "llinventoryobserver.h"
#include "llnotifications.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "lltimer.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "lumenaichat.h"
#include "lumenaictl.h"
#include "lumenfolders.h"

#include <boost/json.hpp>

#include <algorithm>
#include <cctype>
#include <ctime>
#include <regex>
#include <set>

namespace
{
    // The runner's own calls skip the viewer's per-act questions: the skill as
    // a whole is what the person approved. Set only around one synchronous
    // call on the main thread, so nothing else can slip in under it.
    bool gStepRunning = false;

    const char* const CARD_HEADER = "Lumen skill 1";
    const char* const TOOL_PREFIX = "skill_";
    const char* const SKILLS_FOLDER = "Skills";
    const char* const LAST_GOOD_FILE = "lumen_skills_last_good.json";
    const char* const TRUSTED_FILE   = "lumen_skills_trusted.json";

    // A finished run is handed back again for this long, so a caller that
    // asks twice gets the same answer instead of a second run.
    const F64 RESULT_KEEPS = 300.0;
    // How long one wait may last when a step does not say.
    const F64 DEFAULT_WAIT = 30.0;
    const F64 LONGEST_WAIT = 300.0;

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
        return s;
    }

    std::string trim(const std::string& s)
    {
        const size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) return std::string();
        const size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }

    std::string toJson(const LLSD& v)
    {
        return boost::json::serialize(LlsdToJson(v));
    }

    bool fromJson(const std::string& text, LLSD& out, std::string& error)
    {
        boost::system::error_code ec;
        boost::json::value jv = boost::json::parse(text, ec);
        if (ec)
        {
            error = ec.message();
            return false;
        }
        out = LlsdFromJson(jv);
        return true;
    }

    /** Words, or a pattern with * and ?, matched without regard to case. */
    bool globMatch(const char* p, const char* s)
    {
        if (!*p) return !*s;
        if (*p == '*') return globMatch(p + 1, s) || (*s && globMatch(p, s + 1));
        if (*s && (*p == '?' || *p == *s)) return globMatch(p + 1, s + 1);
        return false;
    }
    bool nameMatches(const std::string& name, const std::string& pattern)
    {
        const std::string n = lower(name), p = lower(trim(pattern));
        if (p.empty()) return false;
        if (p.find_first_of("*?") != std::string::npos) return globMatch(p.c_str(), n.c_str());
        // Every word of the pattern somewhere in the name, in any order -- the
        // way inventory search reads "una skirt".
        size_t at = 0;
        while (at < p.size())
        {
            const size_t end = p.find(' ', at);
            const std::string w = p.substr(at, end == std::string::npos ? std::string::npos : end - at);
            if (!w.empty() && n.find(w) == std::string::npos) return false;
            if (end == std::string::npos) break;
            at = end + 1;
        }
        return true;
    }
    /** The words a fuzzy search is given for a pattern: its letters without the wildcards. */
    std::string patternWords(const std::string& pattern)
    {
        std::string w;
        for (char c : pattern) w += (c == '*' || c == '?') ? ' ' : c;
        return trim(w);
    }

    // ---- values: {sickness}, {recipe.potion}, {herb1.item_id} ---------------

    /** {sickness} and {recipe.herbs.0} are values; {4} and {1,2} in a pattern are not. */
    bool isRef(const std::string& inner)
    {
        const std::string t = trim(inner);
        if (t.empty() || !(std::isalpha((unsigned char)t[0]) || t[0] == '_')) return false;
        return std::all_of(t.begin(), t.end(), [](unsigned char c)
                           { return std::isalnum(c) || c == '_' || c == '.'; });
    }

    bool lookupPath(const LLSD& vars, const std::string& path, LLSD& out)
    {
        LLSD cur = vars;
        size_t at = 0;
        while (true)
        {
            const size_t dot = path.find('.', at);
            const std::string part = trim(path.substr(at, dot == std::string::npos ? std::string::npos : dot - at));
            if (part.empty()) return false;
            if (cur.isMap() && cur.has(part)) cur = cur[part];
            else if (cur.isArray() && std::all_of(part.begin(), part.end(), ::isdigit)
                     && (size_t)std::stoi(part) < (size_t)cur.size())
                cur = cur[std::stoi(part)];
            else return false;
            if (dot == std::string::npos) break;
            at = dot + 1;
        }
        out = cur;
        return true;
    }

    /** The names a value refers to, for checking a card before it is used. */
    void namesIn(const LLSD& v, std::set<std::string>& out)
    {
        if (v.isString())
        {
            const std::string s = v.asString();
            size_t at = 0;
            while ((at = s.find('{', at)) != std::string::npos)
            {
                const size_t end = s.find('}', at);
                if (end == std::string::npos) break;
                if (!isRef(s.substr(at + 1, end - at - 1))) { at = end + 1; continue; }
                std::string name = trim(s.substr(at + 1, end - at - 1));
                const size_t dot = name.find('.');
                if (dot != std::string::npos) name = name.substr(0, dot);
                if (!name.empty()) out.insert(name);
                at = end + 1;
            }
        }
        else if (v.isArray())
        {
            for (LLSD::array_const_iterator it = v.beginArray(); it != v.endArray(); ++it) namesIn(*it, out);
        }
        else if (v.isMap())
        {
            for (LLSD::map_const_iterator it = v.beginMap(); it != v.endMap(); ++it) namesIn(it->second, out);
        }
    }

    /**
     * Fill in {name} from what is known. A string that is nothing but one
     * {name} becomes that value itself (a map stays a map); anything else is
     * text with the values written in. `missing` names the first one unknown.
     */
    LLSD resolve(const LLSD& v, const LLSD& vars, std::string& missing)
    {
        if (v.isString())
        {
            const std::string s = v.asString();
            if (s.size() > 2 && s.front() == '{' && s.back() == '}'
                && s.find('{', 1) == std::string::npos && isRef(s.substr(1, s.size() - 2)))
            {
                LLSD out;
                if (lookupPath(vars, s.substr(1, s.size() - 2), out)) return out;
                if (missing.empty()) missing = s;
                return LLSD();
            }
            std::string res;
            size_t at = 0;
            while (true)
            {
                const size_t open = s.find('{', at);
                if (open == std::string::npos) { res += s.substr(at); break; }
                const size_t close = s.find('}', open);
                if (close == std::string::npos) { res += s.substr(at); break; }
                res += s.substr(at, open - at);
                if (!isRef(s.substr(open + 1, close - open - 1)))
                {
                    res += s.substr(open, close - open + 1);   // literal, as in \d{4}
                    at = close + 1;
                    continue;
                }
                LLSD val;
                if (lookupPath(vars, s.substr(open + 1, close - open - 1), val))
                {
                    res += val.isMap() && val.has("name") ? val["name"].asString() : val.asString();
                }
                else if (missing.empty())
                {
                    missing = s.substr(open, close - open + 1);
                }
                at = close + 1;
            }
            return res;
        }
        if (v.isArray())
        {
            LLSD out = LLSD::emptyArray();
            for (LLSD::array_const_iterator it = v.beginArray(); it != v.endArray(); ++it)
                out.append(resolve(*it, vars, missing));
            return out;
        }
        if (v.isMap())
        {
            LLSD out = LLSD::emptyMap();
            for (LLSD::map_const_iterator it = v.beginMap(); it != v.endMap(); ++it)
                out[it->first] = resolve(it->second, vars, missing);
            return out;
        }
        return v;
    }

    // ---- dates in names: "Mint (exp 2026-10-12)" -----------------------------

    /** Today as yyyymmdd, on Second Life's clock (Pacific) unless told UTC. */
    S32 today(const std::string& timezone)
    {
        const time_t now = time_corrected();
        struct tm* t = lower(timezone) == "utc" ? gmtime(&now)
                                                 : utc_to_pacific_time(now, is_daylight_savings());
        if (!t) return 0;
        return (t->tm_year + 1900) * 10000 + (t->tm_mon + 1) * 100 + t->tm_mday;
    }

    /** "YYYY-MM-DD", "DD/MM/YYYY", "MM.DD.YY" -- the order of the parts, and a pattern for them. */
    bool dateFormat(const std::string& format, std::string& pattern, std::vector<char>& order, std::string& error)
    {
        pattern.clear();
        order.clear();
        const std::string f = format.empty() ? std::string("YYYY-MM-DD") : format;
        for (size_t i = 0; i < f.size(); )
        {
            if (f.compare(i, 4, "YYYY") == 0) { pattern += "(\\d{4})"; order.push_back('Y'); i += 4; }
            else if (f.compare(i, 2, "YY") == 0) { pattern += "(\\d{2})"; order.push_back('y'); i += 2; }
            else if (f.compare(i, 2, "MM") == 0) { pattern += "(\\d{1,2})"; order.push_back('M'); i += 2; }
            else if (f.compare(i, 2, "DD") == 0) { pattern += "(\\d{1,2})"; order.push_back('D'); i += 2; }
            else
            {
                const char c = f[i++];
                if (std::isalnum((unsigned char)c)) pattern += c;
                else { pattern += '\\'; pattern += c; }
            }
        }
        if (std::find(order.begin(), order.end(), 'M') == order.end()
            || std::find(order.begin(), order.end(), 'D') == order.end()
            || (std::find(order.begin(), order.end(), 'Y') == order.end()
                && std::find(order.begin(), order.end(), 'y') == order.end()))
        {
            error = "a date format needs a year, a month and a day, like YYYY-MM-DD";
            return false;
        }
        return true;
    }

    /** The date written in a name, as yyyymmdd; 0 when there is none. */
    S32 dateIn(const std::string& name, const LLSD& spec)
    {
        std::string pattern, error;
        std::vector<char> order;
        if (!dateFormat(spec["format"].asString(), pattern, order, error)) return 0;
        try
        {
            std::string text = name;
            if (spec.has("pattern") && !spec["pattern"].asString().empty())
            {
                // The card's own pattern finds where the date is; its first
                // group is the date, read by the format.
                std::smatch m;
                const std::regex around(spec["pattern"].asString(), std::regex::icase);
                if (!std::regex_search(name, m, around) || m.size() < 2) return 0;
                text = m[1].str();
            }
            std::smatch m;
            if (!std::regex_search(text, m, std::regex(pattern))) return 0;
            S32 y = 0, mo = 0, d = 0;
            for (size_t i = 0; i < order.size() && i + 1 < m.size(); ++i)
            {
                const S32 n = std::stoi(m[i + 1].str());
                switch (order[i])
                {
                    case 'Y': y = n; break;
                    case 'y': y = 2000 + n; break;
                    case 'M': mo = n; break;
                    case 'D': d = n; break;
                }
            }
            if (mo < 1 || mo > 12 || d < 1 || d > 31) return 0;
            return y * 10000 + mo * 100 + d;
        }
        catch (const std::exception&)
        {
            return 0;
        }
    }

    // ---- the card --------------------------------------------------------------

    struct BlockRule
    {
        const char* name;
        std::vector<const char*> required;
        const char* plain;   // for the summary when a step says nothing of itself
    };
    const std::vector<BlockRule>& blockRules()
    {
        static const std::vector<BlockRule> rules = {
            { "lookup",          { "table", "key", "as" }, "look up {key} in the {table} list" },
            { "find_item",       { "name", "as" },         "find {name}" },
            { "touch",           { "object" },             "click {object}" },
            { "dialog",          { "text", "press" },      "when the menu saying \"{text}\" comes up, press \"{press}\"" },
            { "rez",             { "item", "as" },         "rez {item}" },
            { "wait_for_object", { "name", "as" },         "wait for \"{name}\" to appear" },
            { "take",            { "object" },             "pick up {object}" },
            { "verify",          { "item" },               "check that {item} is in the inventory" },
            { "accept_offer",    { "from" },               "accept what {from} offers" },
            { "say",             { "text" },               "say: {text}" },
        };
        return rules;
    }
    const BlockRule* blockRule(const std::string& name)
    {
        for (const BlockRule& r : blockRules())
            if (name == r.name) return &r;
        return nullptr;
    }

    std::string slugFor(const std::string& name)
    {
        std::string s;
        for (unsigned char c : lower(name))
        {
            if (std::isalnum(c)) s += (char)c;
            else if (!s.empty() && s.back() != '_') s += '_';
        }
        while (!s.empty() && s.back() == '_') s.pop_back();
        // Short: hosts allow a tool name 64 characters, and Claude Code puts
        // "mcp__second_life__" in front of it.
        if (s.size() > 32) s.resize(32);
        while (!s.empty() && s.back() == '_') s.pop_back();
        return s.empty() ? std::string("unnamed") : s;
    }

    /** How a step is put to the person: its own words if it has some. */
    std::string stepWords(const LLSD& step)
    {
        if (step.has("about") && !step["about"].asString().empty()) return step["about"].asString();
        const BlockRule* rule = blockRule(step["do"].asString());
        if (!rule) return step["do"].asString();
        std::string out = rule->plain;
        for (const char* key : { "key", "table", "name", "text", "press", "item", "from" })
        {
            const std::string token = std::string("{") + key + "}";
            const size_t at = out.find(token);
            if (at != std::string::npos) out.replace(at, token.size(), step[key].asString());
        }
        const size_t at = out.find("{object}");
        if (at != std::string::npos)
        {
            const LLSD& o = step["object"];
            std::string what = o.isMap()
                ? (o.has("worn") ? "the worn " + o["worn"].asString()
                   : o.has("near") ? "\"" + o["near"].asString() + "\" nearby"
                   : o["id"].asString())
                : o.asString();
            out.replace(at, 8, what);
        }
        return out;
    }

    // ---- a call through the viewer's own front door -----------------------------

    /**
     * One of the endpoint's tools, as a person's assistant would call it -- so
     * every guard, the replay of a repeated request and the action log all
     * apply -- but with the viewer's per-act questions skipped. A reply that is
     * waiting on the region is asked for again, a quarter second apart.
     */
    LLSD callTool(const std::string& tool, LLSD args, const std::string& request_id, std::string& error)
    {
        static S32 next_id = 1;
        if (!request_id.empty()) args["request_id"] = request_id;
        LLSD req;
        req["jsonrpc"] = "2.0";
        req["id"] = next_id++;
        req["method"] = "tools/call";
        req["params"]["name"] = tool;
        req["params"]["arguments"] = args;
        const std::string body = toJson(req);

        const F64 give_up = LLTimer::getTotalSeconds() + 15.0;
        while (true)
        {
            if (!LumenAIControl::instanceExists())
            {
                error = "the viewer is closing";
                return LLSD();
            }
            gStepRunning = true;
            const std::string reply = LumenAIControl::instance().handleRequest(body);
            gStepRunning = false;

            LLSD r;
            std::string parse_error;
            if (!fromJson(reply, r, parse_error))
            {
                error = "the viewer gave an answer that could not be read";
                return LLSD();
            }
            if (r.has("error"))
            {
                error = r["error"]["message"].asString();
                return LLSD();
            }
            const LLSD result = r["result"];
            if (result["isError"].asBoolean())
            {
                error = result["content"][0]["text"].asString();
                if (error.empty()) error = "it failed";
                return LLSD();
            }
            const LLSD sc = result["structuredContent"];
            if (sc["settling"].asBoolean() && LLTimer::getTotalSeconds() < give_up)
            {
                llcoro::suspendUntilTimeout(0.25f);
                continue;
            }
            if (sc["waiting_for_user"].asBoolean())
            {
                error = "the viewer wanted to ask the user first, which a skill does not do";
                return LLSD();
            }
            return sc;
        }
    }

    void progress(const std::string& note)
    {
        if (!LumenAIChatFloater::postFromViewer(std::string(), note))
        {
            LL_INFOS("AISkills") << note << LL_ENDL;
        }
    }
}

// =============================================================================

struct LumenAISkills::Run
{
    enum State { RUNNING, DONE, FAILED, STOPPED };

    std::string key;
    Skill       skill;
    LLSD        inputs, vars;
    S32         step = 0;
    State       state = RUNNING;
    std::string failure;
    std::vector<std::string> said;
    F64         started = 0.0, finished = 0.0;
    bool        stop = false;
    bool        collected = false;   //< its ending has been handed back; the same request runs again
    std::set<LLUUID> roots_before, dialogues_before, answered;
    std::vector<std::pair<LLUUID, std::string>> arrived;   // items that came into the inventory

    bool stopped() const
    {
        return stop || !LumenAIControl::instanceExists()
            || LLStartUp::getStartupState() < STATE_STARTED || LLApp::isExiting();
    }
    /** Wait for `seconds`, or less if asked to stop. False when stopped. */
    bool pause(F64 seconds)
    {
        const F64 until = LLTimer::getTotalSeconds() + seconds;
        while (LLTimer::getTotalSeconds() < until)
        {
            if (stopped()) return false;
            llcoro::suspendUntilTimeout((F32)llmin(0.25, until - (F64)LLTimer::getTotalSeconds() + 0.01));
        }
        return !stopped();
    }
};

class LumenSkillsObserver : public LLInventoryObserver
{
public:
    void changed(U32 mask) override
    {
        if (!LumenAISkills::instanceExists()) return;
        LumenAISkills& self = LumenAISkills::instance();

        // What arrives while a skill runs is how `verify` knows it worked.
        if ((mask & LLInventoryObserver::ADD) && self.mRun && self.mRun->state == LumenAISkills::Run::RUNNING)
        {
            for (const LLUUID& id : gInventory.getAddedIDs())
            {
                if (LLViewerInventoryItem* item = gInventory.getItem(id))
                {
                    self.mRun->arrived.emplace_back(id, item->getName());
                }
            }
        }

        // While teaching: what came into the inventory, for the model to see.
        if ((mask & LLInventoryObserver::ADD) && self.mTeaching)
        {
            for (const LLUUID& id : gInventory.getAddedIDs())
            {
                if (LLViewerInventoryItem* item = gInventory.getItem(id))
                {
                    LLSD e;
                    e["kind"] = "item_arrived";
                    e["item_id"] = id;
                    e["name"] = item->getName();
                    if (const LLViewerInventoryCategory* c = gInventory.getCategory(item->getParentUUID()))
                        e["folder"] = c->getName();
                    self.noteTeachEvent(e);
                }
            }
        }

        // A card in the skills folder added, changed, renamed or taken away.
        const LLUUID folder = self.skillsFolder();
        bool ours = false;
        for (const LLUUID& id : gInventory.getChangedIDs())
        {
            if (id == folder) { ours = true; break; }
            if (LLViewerInventoryItem* item = gInventory.getItem(id))
            {
                if (folder.notNull() && item->getParentUUID() == folder) { ours = true; break; }
            }
            if (LLViewerInventoryCategory* cat = gInventory.getCategory(id))
            {
                if (cat->getName() == SKILLS_FOLDER) { ours = true; break; }
            }
        }
        // A card moved out leaves no trace in the folder; any skill whose card
        // is no longer in it is a reason to read again too.
        if (!ours)
        {
            for (const LumenAISkills::Skill& s : self.mSkills)
            {
                LLViewerInventoryItem* item = gInventory.getItem(s.item_id);
                if (!item || item->getParentUUID() != folder || item->getAssetUUID() != s.asset_id)
                {
                    ours = true;
                    break;
                }
            }
        }
        if (ours) self.scheduleReload();
    }
};

// =============================================================================

LumenAISkills::LumenAISkills() {}

LumenAISkills::~LumenAISkills()
{
    // The inventory model deletes its observers itself at logout, before
    // singletons go; one still registered is ours to remove.
    if (mObserver && gInventory.containsObserver(mObserver))
    {
        gInventory.removeObserver(mObserver);
        delete mObserver;
    }
    mObserver = nullptr;
}

// static
bool LumenAISkills::stepRunning()
{
    return gStepRunning;
}

// static
bool LumenAISkills::isSkillTool(const std::string& name)
{
    return name.compare(0, strlen(TOOL_PREFIX), TOOL_PREFIX) == 0;
}

const LumenAISkills::Skill* LumenAISkills::find(const std::string& tool) const
{
    for (const Skill& s : mSkills)
        if (s.tool == tool) return &s;
    return nullptr;
}

std::vector<std::string> LumenAISkills::toolNames() const
{
    std::vector<std::string> out;
    for (const Skill& s : mSkills) out.push_back(s.tool);
    return out;
}

std::string LumenAISkills::toolForTrigger(const std::string& typed) const
{
    const std::string t = lower(trim(typed));
    if (t.empty()) return std::string();
    for (const Skill& s : mSkills)
        if (!s.trigger.empty() && lower(trim(s.trigger)) == t) return s.tool;
    return std::string();
}

// ---- reading a card -----------------------------------------------------------

// static
bool LumenAISkills::parse(const std::string& text_in, Skill& out, std::string& error)
{
    std::string text = text_in;
    // The header first, on its own line; the JSON after it.
    const size_t first_end = text.find('\n');
    const std::string first = trim(text.substr(0, first_end));
    if (first != CARD_HEADER)
    {
        error = std::string("the first line must be \"") + CARD_HEADER + "\"";
        return false;
    }
    const std::string body = first_end == std::string::npos ? std::string() : text.substr(first_end + 1);
    LLSD card;
    std::string json_error;
    if (!fromJson(body, card, json_error) || !card.isMap())
    {
        error = "what follows the first line is not a readable skill (" +
                (json_error.empty() ? std::string("not a { ... } block") : json_error) +
                ") -- a hand edit may have broken it";
        return false;
    }

    Skill s;
    s.name = trim(card["name"].asString());
    s.about = trim(card["about"].asString());
    s.summary = trim(card["summary"].asString());
    s.trigger = trim(card["trigger"].asString());
    s.ask_first = !card.has("ask_first") || card["ask_first"].asBoolean();
    if (s.name.empty() || s.name.size() > 60)
    {
        error = "it needs a name of 1 to 60 characters";
        return false;
    }
    if (s.about.empty() || s.about.size() > 600)
    {
        error = "it needs an `about` saying when to use it, at most 600 characters";
        return false;
    }
    if (card.has("examples"))
    {
        if (!card["examples"].isArray()) { error = "`examples` must be a list of phrases"; return false; }
        for (LLSD::array_const_iterator it = card["examples"].beginArray(); it != card["examples"].endArray(); ++it)
        {
            if (s.examples.size() < 8 && !trim(it->asString()).empty()) s.examples.push_back(trim(it->asString()));
        }
    }

    std::set<std::string> known = { "today" };
    if (card.has("inputs"))
    {
        if (!card["inputs"].isArray()) { error = "`inputs` must be a list"; return false; }
        for (LLSD::array_const_iterator it = card["inputs"].beginArray(); it != card["inputs"].endArray(); ++it)
        {
            Input in;
            in.name = trim((*it)["name"].asString());
            in.about = trim((*it)["about"].asString());
            in.required = !it->has("required") || (*it)["required"].asBoolean();
            if (in.name.empty() || !std::all_of(in.name.begin(), in.name.end(),
                    [](unsigned char c) { return std::islower(c) || std::isdigit(c) || c == '_'; })
                || std::isdigit((unsigned char)in.name[0]))
            {
                error = "an input's name must be lowercase letters, digits and _, like \"sickness\"";
                return false;
            }
            if ((*it)["choices"].isArray())
            {
                for (LLSD::array_const_iterator c = (*it)["choices"].beginArray(); c != (*it)["choices"].endArray(); ++c)
                    if (!trim(c->asString()).empty()) in.choices.push_back(trim(c->asString()));
            }
            known.insert(in.name);
            s.inputs.push_back(in);
        }
    }
    if (card.has("tables"))
    {
        if (!card["tables"].isMap()) { error = "`tables` must be a set of named lists"; return false; }
        s.tables = card["tables"];
    }

    if (!card["steps"].isArray() || card["steps"].size() == 0)
    {
        error = "it has no steps";
        return false;
    }
    if (card["steps"].size() > 60)
    {
        error = "it has more than 60 steps";
        return false;
    }
    S32 n = 0;
    for (LLSD::array_const_iterator it = card["steps"].beginArray(); it != card["steps"].endArray(); ++it)
    {
        ++n;
        const LLSD& step = *it;
        const std::string what = step["do"].asString();
        if (what == "web_call")
        {
            error = llformat("step %d calls a web address, which Lumen cannot do yet", n);
            return false;
        }
        if (what == "give" || what == "pay" || what == "delete" || what == "remove")
        {
            error = llformat("step %d would %s, and a skill may never give, pay or delete", n, what.c_str());
            return false;
        }
        const BlockRule* rule = blockRule(what);
        if (!rule)
        {
            error = llformat("step %d does \"%s\", which is not something a skill can do", n, what.c_str());
            return false;
        }
        for (const char* key : rule->required)
        {
            if (!step.has(key) || (step[key].isString() && trim(step[key].asString()).empty()))
            {
                error = llformat("step %d (%s) needs `%s`", n, what.c_str(), key);
                return false;
            }
        }
        if (what == "lookup" && !s.tables.has(step["table"].asString()))
        {
            error = llformat("step %d looks in a table called \"%s\", and there is none", n,
                             step["table"].asString().c_str());
            return false;
        }
        if (what == "find_item" && step.has("date"))
        {
            std::string pattern, ferr;
            std::vector<char> order;
            if (!dateFormat(step["date"]["format"].asString(), pattern, order, ferr))
            {
                error = llformat("step %d: %s", n, ferr.c_str());
                return false;
            }
            if (step["date"].has("pattern"))
            {
                try { std::regex r(step["date"]["pattern"].asString()); }
                catch (const std::exception&)
                {
                    error = llformat("step %d's date pattern cannot be read", n);
                    return false;
                }
            }
        }
        // Nothing may be used before something gives it a value.
        std::set<std::string> used;
        LLSD without_as = step;
        without_as.erase("as");
        without_as.erase("about");
        namesIn(without_as, used);
        for (const std::string& u : used)
        {
            if (!known.count(u))
            {
                error = llformat("step %d uses {%s} before anything gives it a value", n, u.c_str());
                return false;
            }
        }
        if (step.has("as")) known.insert(trim(step["as"].asString()));
    }
    s.steps = card["steps"];
    s.card = card;
    s.tool = std::string(TOOL_PREFIX) + slugFor(s.name);
    if (s.summary.empty()) s.summary = plainSummary(s);
    out = s;
    return true;
}

// static
std::string LumenAISkills::plainSummary(const Skill& skill)
{
    std::string out;
    S32 n = 0;
    for (LLSD::array_const_iterator it = skill.steps.beginArray(); it != skill.steps.endArray(); ++it)
    {
        out += llformat("%d. ", ++n) + stepWords(*it) + "\n";
    }
    return trim(out);
}

// ---- to the model ---------------------------------------------------------------

LLSD LumenAISkills::toolDescriptors() const
{
    LLSD tools = LLSD::emptyArray();
    for (const Skill& s : mSkills)
    {
        std::string d = s.about;
        if (!s.examples.empty())
        {
            d += "\nSaid like: ";
            for (size_t i = 0; i < s.examples.size(); ++i)
                d += (i ? "; " : "") + std::string("\"") + s.examples[i] + "\"";
            d += ".";
        }
        d += "\nA skill the user taught Lumen (\"" + s.name + "\"): the viewer carries out its "
             "steps itself, in order, and says how it went. Use it only when the user asks for "
             "this to be DONE -- a question about it (\"what herbs do I have?\") is not a reason "
             "to run it. If an input is missing, ask the user for it rather than guessing.";
        LLSD props = LLSD::emptyMap();
        LLSD required = LLSD::emptyArray();
        for (const Input& in : s.inputs)
        {
            LLSD p;
            p["type"] = "string";
            p["description"] = in.about.empty() ? in.name : in.about;
            if (!in.choices.empty())
            {
                LLSD e = LLSD::emptyArray();
                for (const std::string& c : in.choices) e.append(c);
                p["enum"] = e;
            }
            props[in.name] = p;
            if (in.required) required.append(in.name);
        }
        LLSD schema;
        schema["type"] = "object";
        schema["properties"] = props;
        if (required.size() > 0) schema["required"] = required;
        LLSD tool;
        tool["name"] = s.tool;
        tool["description"] = d;
        tool["inputSchema"] = schema;
        tools.append(tool);
    }
    return tools;
}

LLSD LumenAISkills::describe() const
{
    LLSD out;
    LLSD list = LLSD::emptyArray();
    for (const Skill& s : mSkills)
    {
        LLSD one;
        one["tool"] = s.tool;
        one["name"] = s.name;
        one["card"] = s.card_name;
        one["asks_first"] = s.ask_first;
        if (!s.trigger.empty()) one["trigger"] = s.trigger;
        one["summary"] = s.summary;
        one["card"] = s.card;   // to change it: change this and save it again
        if (!s.problem.empty()) one["card_problem"] = s.problem;
        list.append(one);
    }
    out["skills"] = list;
    if (mProblems.size() > 0) out["cards_refused"] = mProblems;
    out["loaded"] = mLoaded;
    return out;
}

std::string LumenAISkills::checkInputs(const Skill& skill, const LLSD& inputs) const
{
    for (const Input& in : skill.inputs)
    {
        const std::string v = trim(inputs[in.name].asString());
        if (v.empty())
        {
            if (!in.required) continue;
            std::string ask = "This skill needs `" + in.name + "`" +
                              (in.about.empty() ? std::string() : " (" + in.about + ")") + ".";
            if (!in.choices.empty())
            {
                ask += " It can be: ";
                for (size_t i = 0; i < in.choices.size(); ++i) ask += (i ? ", " : "") + in.choices[i];
                ask += ".";
            }
            return ask + " Ask the user which -- do not guess -- then call it again with it.";
        }
        if (!in.choices.empty())
        {
            bool fits = false;
            for (const std::string& c : in.choices) fits = fits || lower(c) == lower(v);
            if (!fits)
            {
                std::string ask = "\"" + v + "\" is not one this skill knows for `" + in.name + "`. It can be: ";
                for (size_t i = 0; i < in.choices.size(); ++i) ask += (i ? ", " : "") + in.choices[i];
                return ask + ". Ask the user which they mean.";
            }
        }
    }
    return std::string();
}

bool LumenAISkills::needsTrust(const Skill& skill) const
{
    return skill.creator_id.notNull() && skill.creator_id != gAgent.getID()
        && !mTrusted.has(skill.asset_id.asString());
}

void LumenAISkills::trust(const Skill& skill)
{
    mTrusted[skill.asset_id.asString()] = true;
    writeTrusted();
}

// ---- the folder and the files beside it ---------------------------------------------

LLUUID LumenAISkills::skillsFolder() const
{
    if (!gInventory.isInventoryUsable()) return LLUUID::null;
    // Asked on every inventory change, so the answer is kept while it holds.
    if (mFolder.notNull())
    {
        const LLViewerInventoryCategory* cat = gInventory.getCategory(mFolder);
        const LLViewerInventoryCategory* up = cat ? gInventory.getCategory(cat->getParentUUID()) : nullptr;
        if (cat && cat->getName() == SKILLS_FOLDER && up && up->getName() == LumenFolders::LUMEN_FOLDER)
            return mFolder;
        mFolder.setNull();
    }
    const LLUUID lumen = gInventory.findCategoryByName(LumenFolders::LUMEN_FOLDER);
    if (lumen.isNull()) return LLUUID::null;
    LLInventoryModel::cat_array_t* cats = nullptr;
    LLInventoryModel::item_array_t* items = nullptr;
    gInventory.getDirectDescendentsOf(lumen, cats, items);
    if (!cats) return LLUUID::null;
    for (const LLPointer<LLViewerInventoryCategory>& c : *cats)
        if (c && c->getName() == SKILLS_FOLDER) return mFolder = c->getUUID();
    return LLUUID::null;
}

void LumenAISkills::readLastGood()
{
    mLastGood = LLSD::emptyMap();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, LAST_GOOD_FILE);
    llifstream in(path.c_str());
    if (!in.is_open()) return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLSD v;
    std::string error;
    if (fromJson(text, v, error) && v.isMap()) mLastGood = v;
}

void LumenAISkills::writeLastGood() const
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, LAST_GOOD_FILE);
    llofstream out(path.c_str());
    if (out.is_open()) out << toJson(mLastGood);
}

void LumenAISkills::readTrusted()
{
    mTrusted = LLSD::emptyMap();
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, TRUSTED_FILE);
    llifstream in(path.c_str());
    if (!in.is_open()) return;
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    LLSD v;
    std::string error;
    if (fromJson(text, v, error) && v.isMap()) mTrusted = v;
}

void LumenAISkills::writeTrusted() const
{
    const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, TRUSTED_FILE);
    llofstream out(path.c_str());
    if (out.is_open()) out << toJson(mTrusted);
}

void LumenAISkills::startLoading()
{
    readLastGood();
    readTrusted();
    watchFolder();
    scheduleReload();
}

void LumenAISkills::watchFolder()
{
    if (mObserver && gInventory.containsObserver(mObserver)) return;
    mObserver = new LumenSkillsObserver();
    gInventory.addObserver(mObserver);
}

void LumenAISkills::scheduleReload()
{
    mReloadWanted = true;
    if (mLoading) return;   // the run in progress reads again when it ends
    mLoading = true;
    LLCoros::instance().launch("LumenAISkillsLoad", []()
    {
        // A moment's grace, so a card being saved is read once, not halfway.
        llcoro::suspendUntilTimeout(2.0f);
        while (LumenAISkills::instanceExists())
        {
            LumenAISkills& self = LumenAISkills::instance();
            if (!self.mReloadWanted) break;
            self.mReloadWanted = false;
            self.loadNow();
        }
        if (LumenAISkills::instanceExists()) LumenAISkills::instance().mLoading = false;
    });
}

/** Runs inside the loading coroutine: may wait for the region. */
void LumenAISkills::loadNow()
{
    // Inventory first; a fresh login may still be filling it.
    for (S32 i = 0; i < 120 && !gInventory.isInventoryUsable(); ++i) llcoro::suspendUntilTimeout(0.5f);
    if (!gInventory.isInventoryUsable() || LLApp::isExiting()) return;

    const LLUUID folder = skillsFolder();
    std::vector<Skill> loaded;
    LLSD problems = LLSD::emptyMap();
    if (folder.notNull())
    {
        if (!gInventory.isCategoryComplete(folder))
        {
            LLInventoryModelBackgroundFetch::instance().start(folder);
            for (S32 i = 0; i < 40 && !gInventory.isCategoryComplete(folder); ++i)
                llcoro::suspendUntilTimeout(0.5f);
        }
        LLInventoryModel::cat_array_t* cats = nullptr;
        LLInventoryModel::item_array_t* items = nullptr;
        gInventory.getDirectDescendentsOf(folder, cats, items);
        std::vector<LLPointer<LLViewerInventoryItem>> cards;
        if (items)
        {
            for (const LLPointer<LLViewerInventoryItem>& it : *items)
                if (it && it->getType() == LLAssetType::AT_NOTECARD && !it->getIsLinkType()) cards.push_back(it);
        }
        for (const LLPointer<LLViewerInventoryItem>& item : cards)
        {
            if (!LumenAISkills::instanceExists() || LLApp::isExiting()) return;
            const std::string key = item->getUUID().asString();
            std::string text;
            std::string why;
            if (item->getAssetUUID().isNull())
            {
                why = "the card is empty";
            }
            else if (mLastGood.has(key) && mLastGood[key]["asset"].asUUID() == item->getAssetUUID())
            {
                text = mLastGood[key]["text"].asString();   // unchanged since it last worked
            }
            else
            {
                // Read the way read_notecard reads any card, waiting for the region.
                LLSD args;
                args["action"] = "read_notecard";
                args["item_id"] = item->getUUID();
                const F64 give_up = LLTimer::getTotalSeconds() + 30.0;
                while (true)
                {
                    std::string error;
                    const LLSD r = callTool("inventory", args, std::string(), error);
                    if (!error.empty()) { why = error; break; }
                    if (r["status"].asString() == "ready") { text = r["text"].asString(); break; }
                    if (LLTimer::getTotalSeconds() > give_up) { why = "its text did not arrive"; break; }
                    llcoro::suspendUntilTimeout(0.5f);
                }
            }

            Skill s;
            std::string error;
            if (why.empty() && parse(text, s, error))
            {
                LLSD good;
                good["asset"] = item->getAssetUUID();
                good["text"] = text;
                mLastGood[key] = good;
            }
            else
            {
                const std::string problem = why.empty() ? error : why;
                problems[item->getName()] = problem;
                LL_WARNS("AISkills") << "skill card \"" << item->getName() << "\" refused: " << problem << LL_ENDL;
                // The last version of this card that worked, if there is one.
                if (!mLastGood.has(key) || !parse(mLastGood[key]["text"].asString(), s, error)) continue;
                s.problem = problem;
            }
            s.item_id = item->getUUID();
            s.asset_id = mLastGood[key]["asset"].asUUID();
            s.creator_id = item->getCreatorUUID();
            s.card_name = item->getName();
            // Two cards for one name: the second gets a number, so both stay reachable.
            std::string tool = s.tool;
            for (S32 k = 2; std::any_of(loaded.begin(), loaded.end(),
                                        [&tool](const Skill& o) { return o.tool == tool; }); ++k)
                tool = s.tool + llformat("_%d", k);
            s.tool = tool;
            loaded.push_back(s);
        }
    }
    if (!LumenAISkills::instanceExists()) return;
    // Forget what was kept for cards that are gone.
    LLSD kept = LLSD::emptyMap();
    for (const Skill& s : loaded) kept[s.item_id.asString()] = mLastGood[s.item_id.asString()];
    mLastGood = kept;
    writeLastGood();

    mSkills = loaded;
    mProblems = problems;
    mLoaded = true;
    LL_INFOS("AISkills") << mSkills.size() << " skill(s) ready, " << problems.size()
                         << " card(s) refused" << (folder.isNull() ? " (no #Lumen/Skills folder)" : "") << LL_ENDL;
}

// ---- writing a card ------------------------------------------------------------

namespace
{
    std::string jsonString(const std::string& s)
    {
        return boost::json::serialize(boost::json::value(boost::json::string(s)));
    }

    /** The keys of a map in the order a person reads them, the rest after. */
    std::vector<std::string> keyOrder(const LLSD& map, const std::vector<std::string>& first,
                                      const std::vector<std::string>& last)
    {
        std::vector<std::string> out;
        for (const std::string& k : first) if (map.has(k)) out.push_back(k);
        for (LLSD::map_const_iterator it = map.beginMap(); it != map.endMap(); ++it)
        {
            if (std::find(first.begin(), first.end(), it->first) == first.end()
                && std::find(last.begin(), last.end(), it->first) == last.end())
                out.push_back(it->first);
        }
        for (const std::string& k : last) if (map.has(k)) out.push_back(k);
        return out;
    }

    bool isFlat(const LLSD& v)
    {
        if (v.isArray())
        {
            for (LLSD::array_const_iterator it = v.beginArray(); it != v.endArray(); ++it)
                if (it->isMap() || it->isArray()) return false;
            return true;
        }
        return !v.isMap();
    }

    void writeJson(const LLSD& v, const std::string& pad, std::string& out, bool is_step = false, bool top = false)
    {
        if (v.isMap())
        {
            const std::vector<std::string> keys = top
                ? keyOrder(v, { "name", "about", "examples", "trigger", "ask_first", "inputs", "tables", "summary" }, { "steps" })
                : is_step ? keyOrder(v, { "do", "about" }, { "as" })
                          : keyOrder(v, { "name", "about" }, {});
            if (keys.empty()) { out += "{}"; return; }
            // A short map of plain values fits on one line.
            bool one_line = !top && !is_step && keys.size() <= 4;
            for (const std::string& k : keys) one_line = one_line && isFlat(v[k]) && !v[k].isArray();
            if (is_step)
            {
                one_line = true;
                for (const std::string& k : keys) one_line = one_line && (isFlat(v[k]) || (v[k].isMap() && v[k].size() <= 4));
            }
            if (one_line)
            {
                out += "{ ";
                for (size_t i = 0; i < keys.size(); ++i)
                {
                    out += (i ? ", " : "") + jsonString(keys[i]) + ": ";
                    writeJson(v[keys[i]], pad, out);
                }
                out += " }";
                return;
            }
            out += "{\n";
            for (size_t i = 0; i < keys.size(); ++i)
            {
                out += pad + "  " + jsonString(keys[i]) + ": ";
                const bool steps = top && keys[i] == "steps";
                if (steps && v[keys[i]].isArray())
                {
                    out += "[\n";
                    const LLSD& list = v[keys[i]];
                    for (S32 j = 0; j < (S32)list.size(); ++j)
                    {
                        out += pad + "    ";
                        writeJson(list[j], pad + "    ", out, true);
                        out += (j + 1 < (S32)list.size() ? ",\n" : "\n");
                    }
                    out += pad + "  ]";
                }
                else
                {
                    writeJson(v[keys[i]], pad + "  ", out);
                }
                out += (i + 1 < keys.size() ? ",\n" : "\n");
            }
            out += pad + "}";
            return;
        }
        if (v.isArray())
        {
            if (isFlat(v))
            {
                out += "[";
                for (S32 i = 0; i < (S32)v.size(); ++i)
                {
                    out += (i ? ", " : "");
                    writeJson(v[i], pad, out);
                }
                out += "]";
                return;
            }
            out += "[\n";
            for (S32 i = 0; i < (S32)v.size(); ++i)
            {
                out += pad + "  ";
                writeJson(v[i], pad + "  ", out);
                out += (i + 1 < (S32)v.size() ? ",\n" : "\n");
            }
            out += pad + "]";
            return;
        }
        if (v.isBoolean()) { out += v.asBoolean() ? "true" : "false"; return; }
        if (v.isInteger()) { out += llformat("%d", v.asInteger()); return; }
        if (v.isReal())
        {
            const F64 r = v.asReal();
            out += (r == (F64)(S64)r && fabs(r) < 1e9) ? llformat("%lld", (long long)r) : llformat("%g", r);
            return;
        }
        if (v.isUndefined()) { out += "null"; return; }
        out += jsonString(v.asString());
    }
}

// static
std::string LumenAISkills::cardText(const LLSD& card)
{
    std::string out = std::string(CARD_HEADER) + "\n";
    writeJson(card, std::string(), out, false, true);
    return out + "\n";
}

LLUUID LumenAISkills::cardFor(const std::string& name) const
{
    for (const Skill& s : mSkills)
        if (lower(s.name) == lower(trim(name))) return s.item_id;
    return LLUUID::null;
}

// ---- teaching ----------------------------------------------------------------------

namespace
{
    /** The name a person would know a thing by: a worn one by its item, else as the viewer has it. */
    std::string thingName(LLViewerObject* root)
    {
        if (!root) return std::string();
        if (root->isAttachment())
        {
            if (LLViewerInventoryItem* item = gInventory.getItem(root->getAttachmentItemID()))
                return item->getName();
        }
        return LumenAIControl::instanceExists() ? LumenAIControl::instance().objectNameFor(root->getID())
                                                : std::string();
    }

    S32 linkNumber(LLViewerObject* prim)
    {
        LLViewerObject* root = prim ? prim->getRootEdit() : nullptr;
        if (!root || root == prim) return 1;
        S32 n = 2;
        for (const LLViewerObject* child : root->getChildren())
        {
            if (child == prim) return n;
            if (child && !child->isAvatar()) ++n;
        }
        return 1;
    }
}

void LumenAISkills::noteTeachEvent(LLSD event)
{
    event["t"] = ll_round((F32)(LLTimer::getTotalSeconds() - mTeachStarted), 0.1f);
    mTeachEvents.append(event);
    // A routine is minutes, not hours; a recording left on is cut, not grown.
    if (mTeachEvents.size() > 400)
    {
        LLSD kept = LLSD::emptyArray();
        for (S32 i = (S32)mTeachEvents.size() - 400; i < (S32)mTeachEvents.size(); ++i) kept.append(mTeachEvents[i]);
        mTeachEvents = kept;
    }
}

// static
void LumenAISkills::noteTouch(LLViewerObject* object, const LLVector2& st, const LLVector2& uv, S32 face)
{
    if (!object || gStepRunning || !LumenAISkills::instanceExists()) return;
    LumenAISkills& self = LumenAISkills::instance();
    if (!self.mTeaching) return;
    LLViewerObject* root = object->getRootEdit();
    LLSD e;
    e["kind"] = "touch";
    e["object_id"] = root->getID();
    e["link"] = linkNumber(object);
    if (object != root) e["prim_id"] = object->getID();
    e["face"] = face;
    LLSD spot; spot.append(ll_round(st.mV[VX], 0.001f)); spot.append(ll_round(st.mV[VY], 0.001f));
    e["spot"] = spot;
    if (root->isHUDAttachment()) e["hud"] = true;
    else if (root->isAttachment()) e["worn"] = true;
    const std::string name = thingName(root);
    if (!name.empty()) e["name"] = name;
    (void)uv;
    self.noteTeachEvent(e);
}

void LumenAISkills::startTeaching()
{
    mTeaching = true;
    mTeachStarted = LLTimer::getTotalSeconds();
    mTeachEvents = LLSD::emptyArray();
    mTeachMenus.clear();
    mTeachMenuPtrs.clear();
    mTeachRoots.clear();
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (o && !o->isDead() && o->getRootEdit() == o) mTeachRoots.insert(o->getID());
    }
    watchFolder();   // the same watcher notes what comes into the inventory

    // Menus: as they come up, and which button the user pressed.
    if (LLNotificationChannelPtr visible = LLNotifications::instance().getChannel("Visible"))
    {
        mTeachMenuListener = visible->connectChanged([](const LLSD& payload) -> bool
        {
            if (!LumenAISkills::instanceExists()) return false;
            LumenAISkills& self = LumenAISkills::instance();
            if (!self.mTeaching || gStepRunning) return false;
            const std::string sig = payload["sigtype"].asString();
            const LLUUID id = payload["id"].asUUID();
            if (sig == "add")
            {
                LLNotificationPtr n = LLNotifications::instance().find(id);
                if (!n) return false;
                const std::string kind = n->getName();
                if (kind.compare(0, 12, "ScriptDialog") != 0 && kind != "ScriptTextBox"
                    && kind.compare(0, 14, "ScriptQuestion") != 0 && kind.compare(0, 13, "ObjectGiveItem") != 0)
                    return false;
                LLSD e;
                e["kind"] = kind.compare(0, 14, "ScriptQuestion") == 0 ? "permission_request"
                          : kind.compare(0, 13, "ObjectGiveItem") == 0 ? "item_offered" : "menu";
                e["text"] = n->getMessage();
                const LLSD subs = n->getSubstitutions();
                if (subs.has("TITLE")) e["from"] = subs["TITLE"];
                else if (subs.has("OBJECTNAME")) e["from"] = subs["OBJECTNAME"];
                LLSD buttons = LLSD::emptyArray();
                if (LLNotificationFormPtr form = n->getForm())
                {
                    for (S32 i = 0; i < form->getNumElements(); ++i)
                    {
                        const LLSD el = form->getElement(i);
                        if (el["type"].asString() == "button")
                            buttons.append(el.has("text") ? el["text"] : el["name"]);
                    }
                }
                if (buttons.size() > 0) e["buttons"] = buttons;
                self.noteTeachEvent(e);
                self.mTeachMenus[id] = (S32)self.mTeachEvents.size() - 1;
                self.mTeachMenuPtrs[id] = n;
            }
            else if (sig == "delete")
            {
                std::map<LLUUID, S32>::iterator m = self.mTeachMenus.find(id);
                std::map<LLUUID, LLNotificationPtr>::iterator p = self.mTeachMenuPtrs.find(id);
                if (m != self.mTeachMenus.end() && p != self.mTeachMenuPtrs.end() && p->second
                    && m->second < (S32)self.mTeachEvents.size())
                {
                    const std::string pressed = LLNotification::getSelectedOptionName(p->second->getResponse());
                    if (!pressed.empty()) self.mTeachEvents[m->second]["pressed"] = pressed;
                    else if (p->second->getResponse().has("message"))
                        self.mTeachEvents[m->second]["typed"] = p->second->getResponse()["message"];
                    self.mTeachEvents[m->second]["answered_after"] =
                        ll_round((F32)(LLTimer::getTotalSeconds() - self.mTeachStarted), 0.1f);
                }
                if (m != self.mTeachMenus.end()) self.mTeachMenus.erase(m);
                if (p != self.mTeachMenuPtrs.end()) self.mTeachMenuPtrs.erase(p);
            }
            return false;
        });
    }

    // Objects that come and go, a few times a second.
    LLEventPumps::instance().obtain("mainloop").stopListening("LumenAISkillsTeach");
    LLEventPumps::instance().obtain("mainloop").listen("LumenAISkillsTeach", [](const LLSD&)
    {
        static F64 next = 0.0;
        const F64 now = LLTimer::getTotalSeconds();
        if (now < next) return false;
        next = now + 0.5;
        if (LumenAISkills::instanceExists()) LumenAISkills::instance().pollTeaching();
        return false;
    });
    LL_INFOS("AISkills") << "teaching: watching what the user does" << LL_ENDL;
}

void LumenAISkills::pollTeaching()
{
    if (!mTeaching) return;
    // Ten minutes is a long routine; a recording forgotten is stopped.
    if (LLTimer::getTotalSeconds() - mTeachStarted > 900.0)
    {
        stopTeaching();
        return;
    }
    const LLVector3d me = gAgent.getPositionGlobal();
    std::set<LLUUID> now;
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->getRootEdit() != o || o->isAvatar() || o->isAttachment()) continue;
        if (o->getPCode() != LL_PCODE_VOLUME) continue;
        now.insert(o->getID());
        if (mTeachRoots.count(o->getID())) continue;
        mTeachRoots.insert(o->getID());
        const F32 d = (F32)(o->getPositionGlobal() - me).magVec();
        if (d > 40.f) continue;   // far off is somebody else's business
        LLSD e;
        e["kind"] = "object_appeared";
        e["object_id"] = o->getID();
        e["owner"] = o->permYouOwner() ? "you" : "someone else";
        e["distance"] = ll_round(d, 0.1f);
        const std::string name = thingName(o);
        if (!name.empty()) e["name"] = name;
        noteTeachEvent(e);
    }
}

LLSD LumenAISkills::stopTeaching()
{
    LLSD out;
    if (!mTeaching && mTeachEvents.size() == 0)
    {
        out["recorded"] = false;
        return out;
    }
    mTeaching = false;
    mTeachMenuListener.disconnect();
    LLEventPumps::instance().obtain("mainloop").stopListening("LumenAISkillsTeach");

    // Names that had not arrived when the thing happened.
    for (S32 i = 0; i < (S32)mTeachEvents.size(); ++i)
    {
        LLSD& e = mTeachEvents[i];
        if (!e.has("object_id") || e.has("name")) continue;
        LLViewerObject* o = gObjectList.findObject(e["object_id"].asUUID());
        const std::string name = o ? thingName(o) : std::string();
        if (!name.empty()) e["name"] = name;
        if (!o && e["kind"].asString() == "object_appeared") e["gone_since"] = true;
    }
    out["recorded"] = true;
    out["seconds"] = ll_round((F32)(LLTimer::getTotalSeconds() - mTeachStarted), 0.1f);
    out["events"] = mTeachEvents;
    LL_INFOS("AISkills") << "teaching: stopped, " << mTeachEvents.size() << " things noted" << LL_ENDL;
    mTeachMenus.clear();
    mTeachMenuPtrs.clear();
    return out;
}

// ---- running ---------------------------------------------------------------------

namespace
{
    std::string runKey(const std::string& tool, const LLSD& inputs)
    {
        return tool + "|" + toJson(inputs);
    }

    /** Ids of every root object in view, so a step can tell what is new. */
    std::set<LLUUID> rootsNow()
    {
        std::set<LLUUID> out;
        const S32 count = gObjectList.getNumObjects();
        for (S32 i = 0; i < count; ++i)
        {
            LLViewerObject* o = gObjectList.getObject(i);
            if (o && !o->isDead() && o->getRootEdit() == o) out.insert(o->getID());
        }
        return out;
    }

    std::set<LLUUID> dialoguesNow()
    {
        std::set<LLUUID> out;
        std::string error;
        LLSD args; args["action"] = "read_dialogues"; args["limit"] = 50;
        const LLSD r = callTool("viewer", args, std::string(), error);
        for (LLSD::array_const_iterator it = r["dialogues"].beginArray(); it != r["dialogues"].endArray(); ++it)
            out.insert((*it)["id"].asUUID());
        return out;
    }

    /** Something worn, by the name of its item -- a HUD, most often. */
    LLViewerObject* wornByName(const std::string& pattern)
    {
        if (!isAgentAvatarValid()) return nullptr;
        LLInventoryModel::cat_array_t cats;
        LLInventoryModel::item_array_t items;
        gInventory.collectDescendents(LLAppearanceMgr::instance().getCOF(), cats, items, LLInventoryModel::EXCLUDE_TRASH);
        for (const LLPointer<LLViewerInventoryItem>& link : items)
        {
            if (!link) continue;
            const LLViewerInventoryItem* item = link->getLinkedItem() ? link->getLinkedItem() : link.get();
            if (!item || item->getType() != LLAssetType::AT_OBJECT) continue;
            if (!nameMatches(item->getName(), pattern)) continue;
            if (LLViewerObject* o = gAgentAvatarp->getWornAttachment(item->getUUID())) return o;
        }
        return nullptr;
    }
}

bool LumenAISkills::hasRun(const Skill& skill, const LLSD& inputs) const
{
    return mRun && mRun->key == runKey(skill.tool, inputs)
        && (mRun->state == Run::RUNNING
            || (!mRun->collected && LLTimer::getTotalSeconds() - mRun->finished < RESULT_KEEPS));
}

void LumenAISkills::stopAll()
{
    if (mRun && mRun->state == Run::RUNNING) mRun->stop = true;
}

namespace
{
    /** An object a step names: an id, something worn, or something nearby by name. */
    LLUUID objectFor(LumenAISkills::Run& run, const LLSD& ref_in, std::string& error,
                     const std::set<LLUUID>* skip = nullptr);

    bool runStep(LumenAISkills::Run& run, const LLSD& step_in, S32 index, std::string& error, bool& may_retry);
}

LLSD LumenAISkills::run(const Skill& skill, const LLSD& inputs)
{
    const std::string key = runKey(skill.tool, inputs);
    const F64 now = LLTimer::getTotalSeconds();

    auto report = [this, &skill]() -> LLSD
    {
        Run& r = *mRun;
        if (r.state != Run::RUNNING) r.collected = true;
        LLSD out;
        out["skill"] = skill.name;
        out["steps"] = (S32)r.skill.steps.size();
        LLSD said = LLSD::emptyArray();
        for (const std::string& s : r.said) said.append(s);
        if (said.size() > 0) out["said"] = said;
        if (r.state == Run::RUNNING)
        {
            out["status"] = "running";
            out["on_step"] = r.step + 1;
            out["doing"] = stepWords(r.skill.steps[r.step]);
            out["settling"] = true;
            out["settle_for"] = 600;
            out["note"] = "The skill is still running; its steps show in the conversation as they "
                          "happen. Call this again with exactly the same arguments to wait for how "
                          "it ends -- it will not start a second time.";
            return out;
        }
        if (r.state == Run::DONE)
        {
            out["status"] = "done";
            out["note"] = "Every step was done and checked. Tell the user in a sentence; the steps "
                          "already showed in the conversation, so do not list them again.";
            return out;
        }
        LLSD e;
        e["code"] = -32000;
        e["message"] = r.state == Run::STOPPED
            ? llformat("The skill \"%s\" was stopped at step %d of %d. Nothing after that was done.",
                       skill.name.c_str(), r.step + 1, (S32)r.skill.steps.size())
            : llformat("The skill \"%s\" stopped at step %d of %d (%s): %s. The steps before it "
                       "were done; nothing after it was. Tell the user plainly which step and why. "
                       "Do not try to finish it another way unless they ask.",
                       skill.name.c_str(), r.step + 1, (S32)r.skill.steps.size(),
                       stepWords(r.skill.steps[r.step]).c_str(), r.failure.c_str());
        LLSD w; w["__error"] = e;
        return w;
    };

    if (mRun && mRun->key == key
        && (mRun->state == Run::RUNNING || (!mRun->collected && now - mRun->finished < RESULT_KEEPS)))
    {
        return report();
    }
    if (mRun && mRun->state == Run::RUNNING)
    {
        LLSD e; e["code"] = -32000;
        e["message"] = "Another skill (\"" + mRun->skill.name + "\") is running. Wait for it to "
                       "finish, or the user can stop it with Clear in the Assistant window.";
        LLSD w; w["__error"] = e; return w;
    }

    std::shared_ptr<Run> r = std::make_shared<Run>();
    r->key = key;
    r->skill = skill;
    r->inputs = inputs;
    r->started = now;
    r->vars = LLSD::emptyMap();
    for (const Input& in : skill.inputs)
    {
        // The choice as the card spells it, so a table keyed "Fever" is found from "fever".
        std::string v = trim(inputs[in.name].asString());
        for (const std::string& c : in.choices) if (lower(c) == lower(v)) v = c;
        r->vars[in.name] = v;
    }
    {
        const S32 t = today("slt");
        r->vars["today"] = llformat("%04d-%02d-%02d", t / 10000, (t / 100) % 100, t % 100);
    }
    r->roots_before = rootsNow();
    mRun = r;
    LL_INFOS("AISkills") << "running \"" << skill.name << "\" (" << skill.steps.size() << " steps)" << LL_ENDL;

    LLCoros::instance().launch("LumenAISkillRun", [r]()
    {
        r->dialogues_before = dialoguesNow();
        progress(r->skill.name + ": starting.");
        const S32 count = (S32)r->skill.steps.size();
        for (r->step = 0; r->step < count; ++r->step)
        {
            if (r->stopped()) { r->state = LumenAISkills::Run::STOPPED; break; }
            const LLSD& step = r->skill.steps[r->step];
            if (step["do"].asString() != "say")
            {
                // In the words of this run: "find Mint", not "find {recipe.herbs.0}".
                std::string unknown;
                const LLSD shown = resolve(step, r->vars, unknown);
                progress(llformat("%s: %d/%d -- %s", r->skill.name.c_str(), r->step + 1, count,
                                  stepWords(unknown.empty() ? shown : step).c_str()));
            }
            std::string error;
            bool may_retry = false;
            bool ok = runStep(*r, step, r->step, error, may_retry);
            if (!ok && may_retry && !r->stopped())
            {
                // Once, and only for a wait that ran out -- lag, not a wrong guess.
                LL_INFOS("AISkills") << "step " << r->step + 1 << " failed (" << error << "), trying once more" << LL_ENDL;
                error.clear();
                ok = runStep(*r, step, r->step, error, may_retry);
            }
            if (!ok)
            {
                if (r->stopped()) { r->state = LumenAISkills::Run::STOPPED; break; }
                if (step["optional"].asBoolean())
                {
                    progress(r->skill.name + ": skipped, as it may be -- " + error + ".");
                    continue;
                }
                r->failure = step.has("fail") ? step["fail"].asString() + " (" + error + ")" : error;
                r->state = LumenAISkills::Run::FAILED;
                break;
            }
        }
        if (r->state == LumenAISkills::Run::RUNNING)
        {
            r->state = LumenAISkills::Run::DONE;
            r->step = count - 1;
        }
        r->finished = LLTimer::getTotalSeconds();
        if (r->state == LumenAISkills::Run::DONE)
            progress(r->skill.name + ": done.");
        else if (r->state == LumenAISkills::Run::STOPPED)
            progress(r->skill.name + ": stopped.");
        else
            progress(r->skill.name + ": stopped at step " + llformat("%d", r->step + 1) + " -- " + r->failure + ".");
        LL_INFOS("AISkills") << "\"" << r->skill.name << "\" ended "
                             << (r->state == LumenAISkills::Run::DONE ? "done" :
                                 r->state == LumenAISkills::Run::STOPPED ? "stopped" : "failed")
                             << " at step " << r->step + 1 << (r->failure.empty() ? "" : ": " + r->failure) << LL_ENDL;
    });
    return report();
}

namespace
{
    F64 waitFor(const LLSD& step, F64 fallback = DEFAULT_WAIT)
    {
        const F64 w = step.has("wait") ? step["wait"].asReal() : fallback;
        return llclamp(w, 1.0, LONGEST_WAIT);
    }

    std::string stepRequestId(const LumenAISkills::Run& run, S32 index, const char* what)
    {
        return llformat("skill:%s:%.0f:%d:%s", run.skill.tool.c_str(), run.started * 1000.0, index, what);
    }

    LLUUID objectFor(LumenAISkills::Run& run, const LLSD& ref_in, std::string& error,
                     const std::set<LLUUID>* skip)
    {
        std::string missing;
        const LLSD ref = resolve(ref_in, run.vars, missing);
        if (!missing.empty()) { error = "nothing has given " + missing + " a value yet"; return LLUUID::null; }

        auto byId = [&error](const std::string& s) -> LLUUID
        {
            LLUUID id;
            if (!LLUUID::validate(s) || !id.set(s) || id.isNull()) { error = "\"" + s + "\" is not an object"; return LLUUID::null; }
            if (!gObjectList.findObject(id)) { error = "that object is not in view any more"; return LLUUID::null; }
            return id;
        };
        if (ref.isString()) return byId(ref.asString());
        if (ref.isMap() && ref.has("object_id")) return byId(ref["object_id"].asString());
        if (ref.isMap() && ref.has("id")) return byId(ref["id"].asString());
        if (ref.isMap() && ref.has("worn"))
        {
            if (LLViewerObject* o = wornByName(ref["worn"].asString())) return o->getID();
            error = "nothing worn is called \"" + ref["worn"].asString() + "\" -- is the HUD on?";
            return LLUUID::null;
        }
        if (ref.isMap() && ref.has("near"))
        {
            const std::string pattern = ref["near"].asString();
            const bool mine = lower(ref["owner"].asString()) == "me";
            const F32 within = ref.has("within") ? (F32)ref["within"].asReal() : 20.f;
            for (S32 tries = 0; tries < 8; ++tries)
            {
                LLSD args;
                args["action"] = "look_nearby";
                args["find"] = patternWords(pattern);
                args["radius"] = within;
                const LLSD r = callTool("movement", args, std::string(), error);
                if (!error.empty()) return LLUUID::null;
                LLUUID best;
                F32 best_d = 1.e9f;
                for (LLSD::array_const_iterator it = r["found"].beginArray(); it != r["found"].endArray(); ++it)
                {
                    if (!nameMatches((*it)["name"].asString(), pattern)) continue;
                    std::vector<LLUUID> ids;
                    if ((*it)["object_ids"].isArray())
                        for (LLSD::array_const_iterator i = (*it)["object_ids"].beginArray(); i != (*it)["object_ids"].endArray(); ++i)
                            ids.push_back(i->asUUID());
                    else ids.push_back((*it)["object_id"].asUUID());
                    for (const LLUUID& id : ids)
                    {
                        LLViewerObject* o = gObjectList.findObject(id);
                        if (!o || (mine && !o->permYouOwner())) continue;
                        if (skip && skip->count(id)) continue;
                        const F32 d = (F32)(o->getPositionGlobal() - gAgent.getPositionGlobal()).magVec();
                        if (d < best_d) { best_d = d; best = id; }
                    }
                }
                if (best.notNull()) return best;
                if (!r["pending"].asBoolean() && tries >= 1) break;   // names were all in, and none fit
                if (!run.pause(1.0)) { error = "stopped"; return LLUUID::null; }
            }
            error = "nothing called \"" + pattern + "\"" + (mine ? " of the user's own" : "") +
                    llformat(" is within %.0f m", within);
            return LLUUID::null;
        }
        error = "the step does not say which object";
        return LLUUID::null;
    }

    struct FoundItem { LLUUID id; std::string name; std::string where; S32 date = 0; };

    class NameCollector : public LLInventoryCollectFunctor
    {
    public:
        NameCollector(const std::string& pattern, const std::string& kind, const std::string& folder)
            : mPattern(pattern), mKind(lower(kind)), mFolder(lower(folder)) {}
        bool operator()(LLInventoryCategory*, LLInventoryItem* item) override
        {
            if (!item) return false;
            const LLViewerInventoryItem* vi = dynamic_cast<const LLViewerInventoryItem*>(item);
            if (vi && vi->getIsLinkType()) return false;   // the item itself, not a link to it
            if (!nameMatches(item->getName(), mPattern)) return false;
            if (!mKind.empty() && lower(LLAssetType::lookup(item->getType())) != mKind) return false;
            if (!mFolder.empty())
            {
                const LLViewerInventoryCategory* parent = gInventory.getCategory(item->getParentUUID());
                bool in = false;
                for (S32 depth = 0; parent && depth < 30 && !in; ++depth)
                {
                    in = lower(parent->getName()).find(mFolder) != std::string::npos;
                    parent = gInventory.getCategory(parent->getParentUUID());
                }
                if (!in) return false;
            }
            return true;
        }
    private:
        std::string mPattern, mKind, mFolder;
    };

    bool findItem(LumenAISkills::Run& run, const LLSD& step, std::string& error)
    {
        const std::string pattern = step["name"].asString();
        const std::string as = trim(step["as"].asString());
        std::vector<FoundItem> found;

        // Where to look, in order: "inventory" and/or objects ("in": [...]).
        LLSD places = step.has("in") ? step["in"] : LLSD("inventory");
        if (!places.isArray()) { LLSD one = LLSD::emptyArray(); one.append(places); places = one; }
        for (LLSD::array_const_iterator p = places.beginArray(); p != places.endArray() && found.empty(); ++p)
        {
            if (p->isString() && lower(p->asString()) == "inventory")
            {
                LLInventoryModel::cat_array_t cats;
                LLInventoryModel::item_array_t items;
                NameCollector want(pattern, step["kind"].asString(), step["folder"].asString());
                gInventory.collectDescendentsIf(gInventory.getRootFolderID(), cats, items,
                                                LLInventoryModel::EXCLUDE_TRASH, want);
                for (const LLPointer<LLViewerInventoryItem>& it : items)
                {
                    if (!it) continue;
                    FoundItem f;
                    f.id = it->getUUID();
                    f.name = it->getName();
                    f.where = "inventory";
                    found.push_back(f);
                }
                continue;
            }
            // Inside an object: what list_contents shows.
            std::string why;
            const LLUUID obj = objectFor(run, *p, why);
            if (obj.isNull()) continue;
            LLSD args;
            args["action"] = "list_contents";
            args["object_id"] = obj;
            for (S32 tries = 0; tries < 20; ++tries)
            {
                std::string cerr;
                const LLSD r = callTool("build", args, std::string(), cerr);
                if (!cerr.empty()) break;
                if (r["pending"].asBoolean() || r["status"].asString() == "loading")
                {
                    if (!run.pause(0.5)) { error = "stopped"; return false; }
                    continue;
                }
                const LLSD& list = r.has("items") ? r["items"] : r["contents"];
                for (LLSD::array_const_iterator it = list.beginArray(); it != list.endArray(); ++it)
                {
                    if (!nameMatches((*it)["name"].asString(), pattern)) continue;
                    FoundItem f;
                    f.id = (*it)["item_id"].asUUID();
                    f.name = (*it)["name"].asString();
                    f.where = obj.asString();
                    found.push_back(f);
                }
                break;
            }
        }

        // A date in each name: drop what has expired, soonest first.
        if (step.has("date"))
        {
            const LLSD& spec = step["date"];
            const std::string keep = lower(spec.has("keep") ? spec["keep"].asString() : std::string("unexpired"));
            const S32 now = today(spec["timezone"].asString());
            S32 expired = 0, undated = 0;
            std::vector<FoundItem> kept;
            for (FoundItem& f : found)
            {
                f.date = dateIn(f.name, spec);
                if (!f.date) { ++undated; continue; }
                if (keep == "unexpired" && f.date < now) { ++expired; continue; }
                kept.push_back(f);
            }
            if (kept.empty() && !found.empty())
            {
                error = llformat("%d found, but %s", (S32)found.size(),
                                 expired && undated ? llformat("%d have expired and %d carry no date", expired, undated).c_str()
                                 : expired ? llformat("all %d have expired", expired).c_str()
                                 : "none carries a date in the form the skill expects");
                return false;
            }
            found = kept;
            const std::string order = lower(step.has("order") ? step["order"].asString() : std::string("soonest"));
            std::stable_sort(found.begin(), found.end(), [&order](const FoundItem& a, const FoundItem& b)
                             { return order == "latest" ? a.date > b.date : a.date < b.date; });
        }
        else if (lower(step["order"].asString()) == "name")
        {
            std::stable_sort(found.begin(), found.end(), [](const FoundItem& a, const FoundItem& b) { return a.name < b.name; });
        }

        if (found.empty())
        {
            error = "nothing called \"" + pattern + "\" was found";
            return false;
        }
        const FoundItem& f = found.front();
        LLSD v;
        v["item_id"] = f.id;
        v["name"] = f.name;
        v["where"] = f.where;
        v["count"] = (S32)found.size();
        if (f.date) v["date"] = llformat("%04d-%02d-%02d", f.date / 10000, (f.date / 100) % 100, f.date % 100);
        run.vars[as] = v;
        return true;
    }

    bool runStep(LumenAISkills::Run& run, const LLSD& step_in, S32 index, std::string& error, bool& may_retry)
    {
        may_retry = false;
        const std::string what = step_in["do"].asString();
        std::string missing;
        // `as` names a result and `about` is words for people; neither is filled in.
        LLSD step = step_in;
        const LLSD as = step_in["as"];
        step.erase("as");
        step = resolve(step, run.vars, missing);
        if (as.isDefined()) step["as"] = as;
        if (!missing.empty())
        {
            error = "nothing has given " + missing + " a value";
            return false;
        }

        if (what == "lookup")
        {
            const LLSD& table = run.skill.tables[step["table"].asString()];
            const std::string key = step["key"].asString();
            for (LLSD::map_const_iterator it = table.beginMap(); it != table.endMap(); ++it)
            {
                if (lower(it->first) == lower(key))
                {
                    run.vars[trim(as.asString())] = it->second;
                    return true;
                }
            }
            error = "\"" + key + "\" is not in the " + step["table"].asString() + " list";
            return false;
        }

        if (what == "find_item")
        {
            return findItem(run, step, error);
        }

        if (what == "touch")
        {
            const LLUUID obj = objectFor(run, step["object"], error);
            if (obj.isNull()) return false;
            LLSD args;
            args["action"] = "touch";
            args["object_id"] = obj;
            if (step.has("link")) args["link"] = step["link"];
            if (step.has("face")) args["face"] = step["face"];
            if (step.has("spot")) args["spot"] = step["spot"];
            callTool("movement", args, stepRequestId(run, index, "touch"), error);
            return error.empty();
        }

        if (what == "dialog")
        {
            const std::string text = lower(step["text"].asString());
            const std::string press = step["press"].asString();
            const F64 until = LLTimer::getTotalSeconds() + waitFor(step);
            while (true)
            {
                std::string rerr;
                LLSD args; args["action"] = "read_dialogues"; args["limit"] = 50;
                const LLSD r = callTool("viewer", args, std::string(), rerr);
                for (LLSD::array_const_iterator it = r["dialogues"].beginArray(); it != r["dialogues"].endArray(); ++it)
                {
                    const LLUUID id = (*it)["id"].asUUID();
                    if (run.dialogues_before.count(id) || run.answered.count(id)) continue;
                    if (lower((*it)["text"].asString()).find(text) == std::string::npos) continue;
                    if ((*it).has("assistant_may_answer") && !(*it)["assistant_may_answer"].asBoolean())
                    {
                        error = "the window that came up is one only the user answers -- " +
                                (*it)["kind"].asString();
                        return false;
                    }
                    // The button by its label, as the person sees it.
                    std::string labels;
                    for (LLSD::array_const_iterator c = (*it)["choices"].beginArray(); c != (*it)["choices"].endArray(); ++c)
                    {
                        const std::string label = (*c)["label"].asString();
                        labels += (labels.empty() ? "" : ", ") + label;
                        if (lower(label) == lower(press) || lower((*c)["name"].asString()) == lower(press))
                        {
                            LLSD a; a["action"] = "answer_dialogue"; a["id"] = id; a["choice"] = (*c)["name"];
                            callTool("viewer", a, stepRequestId(run, index, "answer"), error);
                            run.answered.insert(id);
                            return error.empty();
                        }
                    }
                    error = "the menu came up, but has no \"" + press + "\" -- it offers " + labels;
                    return false;
                }
                if (LLTimer::getTotalSeconds() > until) break;
                if (!run.pause(0.5)) { error = "stopped"; return false; }
            }
            error = "no menu saying \"" + step["text"].asString() + llformat("\" came up within %.0f seconds", waitFor(step));
            may_retry = false;   // pressing the HUD again is the step before, not this one
            return false;
        }

        if (what == "rez")
        {
            const LLSD item = step["item"];
            const std::string item_id = item.isMap() ? item["item_id"].asString() : item.asString();
            const std::string item_name = item.isMap() ? item["name"].asString() : std::string();
            if (item.isMap() && item.has("where") && item["where"].asString() != "inventory")
            {
                error = "it is inside an object, and a skill cannot take things out of one yet";
                return false;
            }
            LLSD args;
            args["action"] = "rez";
            if (LLUUID::validate(item_id)) args["item_id"] = item_id;
            else args["item"] = item.asString();
            if (step.has("near"))
            {
                const LLUUID beside = objectFor(run, step["near"], error);
                if (beside.isNull()) return false;
                args["near"] = beside;
            }
            const std::set<LLUUID> before = rootsNow();
            const F64 sent = LLTimer::getTotalSeconds();
            callTool("build", args, stepRequestId(run, index, "rez"), error);
            if (!error.empty()) return false;

            // What came of it: a new object of the user's own, of that name.
            std::string name = item_name;
            if (name.empty())
                if (LLViewerInventoryItem* inv = gInventory.getItem(LLUUID(item_id))) name = inv->getName();
            const F64 until = sent + waitFor(step, 20.0);
            while (LLTimer::getTotalSeconds() < until)
            {
                const S32 count = gObjectList.getNumObjects();
                for (S32 i = 0; i < count; ++i)
                {
                    LLViewerObject* o = gObjectList.getObject(i);
                    if (!o || o->isDead() || o->getRootEdit() != o || before.count(o->getID())) continue;
                    if (!o->permYouOwner() || o->isAttachment()) continue;
                    if ((o->getPositionGlobal() - gAgent.getPositionGlobal()).magVec() > 15.0) continue;
                    run.roots_before.insert(o->getID());   // ours now, never "new" again
                    LLSD v; v["object_id"] = o->getID(); v["name"] = name;
                    if (as.isDefined()) run.vars[trim(as.asString())] = v;
                    return true;
                }
                if (!run.pause(0.25)) { error = "stopped"; return false; }
            }
            error = "nothing appeared after rezzing it -- the land may not allow it, or the region is slow";
            may_retry = false;   // a second rez could leave two
            return false;
        }

        if (what == "wait_for_object")
        {
            const std::string pattern = step["name"].asString();
            LLSD ref;
            ref["near"] = pattern;
            ref["owner"] = step.has("owner") ? step["owner"] : LLSD("anyone");
            ref["within"] = step.has("within") ? step["within"] : LLSD(20.0);
            const F64 until = LLTimer::getTotalSeconds() + waitFor(step, 60.0);
            while (LLTimer::getTotalSeconds() < until)
            {
                std::string ferr;
                const LLUUID id = objectFor(run, ref, ferr, &run.roots_before);
                if (id.notNull())
                {
                    // Some scripts rename an object a moment after it is rezzed,
                    // so the name is read again before it counts.
                    const F64 settle = step.has("settle") ? llclamp(step["settle"].asReal(), 0.0, 10.0) : 2.0;
                    if (settle > 0.0)
                    {
                        if (!run.pause(settle)) { error = "stopped"; return false; }
                        LLSD look; look["action"] = "inspect_object"; look["object_id"] = id;
                        std::string ierr;
                        const LLSD seen = callTool("viewer", look, std::string(), ierr);
                        const std::string now_called = seen["name"].asString();
                        if (!now_called.empty() && !nameMatches(now_called, pattern))
                        {
                            run.roots_before.insert(id);   // it was something else after all
                            continue;
                        }
                    }
                    LLSD v; v["object_id"] = id;
                    if (LLViewerObject* o = gObjectList.findObject(id)) v["distance"] =
                        (F32)(o->getPositionGlobal() - gAgent.getPositionGlobal()).magVec();
                    v["name"] = pattern;
                    run.vars[trim(as.asString())] = v;
                    return true;
                }
                if (!run.pause(1.0)) { error = "stopped"; return false; }
            }
            error = "\"" + pattern + llformat("\" did not appear within %.0f seconds", waitFor(step, 60.0));
            may_retry = true;   // waiting longer is safe
            return false;
        }

        if (what == "take")
        {
            const LLUUID obj = objectFor(run, step["object"], error);
            if (obj.isNull()) return false;
            const std::string how = lower(step.has("how") ? step["how"].asString() : std::string("take"));
            if (how == "touch")
            {
                LLSD args; args["action"] = "touch"; args["object_id"] = obj;
                callTool("movement", args, stepRequestId(run, index, "take-touch"), error);
                return error.empty();
            }
            if (how != "take")
            {
                error = "\"" + how + "\" is not a way a skill can take something yet (take or touch)";
                return false;
            }
            LLSD args; args["action"] = "take"; args["object_id"] = obj;
            callTool("build", args, stepRequestId(run, index, "take"), error);
            return error.empty();
        }

        if (what == "verify")
        {
            const std::string pattern = step["item"].isMap() ? step["item"]["name"].asString() : step["item"].asString();
            const F64 until = LLTimer::getTotalSeconds() + waitFor(step);
            while (true)
            {
                for (const auto& a : run.arrived)
                {
                    if (nameMatches(a.second, pattern))
                    {
                        if (as.isDefined())
                        {
                            LLSD v; v["item_id"] = a.first; v["name"] = a.second;
                            run.vars[trim(as.asString())] = v;
                        }
                        return true;
                    }
                }
                if (LLTimer::getTotalSeconds() > until) break;
                if (!run.pause(0.5)) { error = "stopped"; return false; }
            }
            error = "\"" + pattern + "\" has not come into the inventory";
            may_retry = true;
            return false;
        }

        if (what == "accept_offer")
        {
            // An inventory offer from an object or a person: Keep, as the user
            // would press it -- or nothing to press, when the viewer took it by
            // itself and it has simply arrived.
            const std::string from = lower(trim(step["from"].asString()));
            const std::string item = step["item"].isMap() ? step["item"]["name"].asString() : step["item"].asString();
            const F64 until = LLTimer::getTotalSeconds() + waitFor(step, 60.0);
            bool kept = false;
            while (true)
            {
                if (!item.empty())
                {
                    for (const auto& a : run.arrived)
                    {
                        if (!nameMatches(a.second, item)) continue;
                        if (as.isDefined())
                        {
                            LLSD v; v["item_id"] = a.first; v["name"] = a.second;
                            run.vars[trim(as.asString())] = v;
                        }
                        return true;
                    }
                }
                if (!kept)
                {
                    std::string rerr;
                    LLSD args; args["action"] = "read_dialogues"; args["limit"] = 50;
                    const LLSD r = callTool("viewer", args, std::string(), rerr);
                    for (LLSD::array_const_iterator it = r["dialogues"].beginArray(); it != r["dialogues"].endArray(); ++it)
                    {
                        const LLUUID id = (*it)["id"].asUUID();
                        const std::string kind = (*it)["kind"].asString();
                        if (run.dialogues_before.count(id) || run.answered.count(id)) continue;
                        if (kind != "ObjectGiveItem" && kind.compare(0, 12, "UserGiveItem") != 0) continue;
                        const std::string text = lower((*it)["text"].asString());
                        if (text.find(from) == std::string::npos) continue;
                        if (!item.empty() && !nameMatches(text, item)) continue;
                        LLSD a; a["action"] = "answer_dialogue"; a["id"] = id; a["choice"] = "Keep";
                        callTool("viewer", a, stepRequestId(run, index, "keep"), error);
                        run.answered.insert(id);
                        if (!error.empty()) return false;
                        kept = true;
                        if (item.empty()) return true;   // nothing named to wait for
                        break;
                    }
                }
                if (LLTimer::getTotalSeconds() > until) break;
                if (!run.pause(0.5)) { error = "stopped"; return false; }
            }
            error = kept ? "\"" + item + "\" was kept but has not come into the inventory"
                         : "no offer from \"" + step["from"].asString() + "\" came";
            may_retry = !kept;
            return false;
        }

        if (what == "say")
        {
            const std::string text = trim(step["text"].asString());
            run.said.push_back(text);
            progress(run.skill.name + ": " + text);
            return true;
        }

        error = "\"" + what + "\" is not something a skill can do";
        return false;
    }
}
