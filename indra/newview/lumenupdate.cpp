/**
 * @file lumenupdate.cpp
 * @brief See lumenupdate.h.
 *
 * Copyright (C) 2026 Catten Carter
 * Licensed under the GNU Lesser General Public License, version 2.1.
 */

#include "llviewerprecompiledheaders.h"
#include "lumenupdate.h"

#include "indra_constants.h"        // LUMEN_VERSION
#include "llagent.h"
#include "llcorehttputil.h"
#include "lleventcoro.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llviewercontrol.h"

#include <vector>

namespace
{
    // The releases API excludes drafts and pre-releases from /latest, which is
    // exactly the behaviour wanted: a draft sitting in the repository must not
    // tell anybody to go and download something that is not there.
    const std::string RELEASES_API =
        "https://api.github.com/repos/cattencarter/lumen/releases/latest";

    bool sChecked = false;

    /** "v0.1.0" or "0.1.0" -> {0,1,0}. Anything unparseable gives an empty
     *  vector, which isNewer() then treats as "not newer". */
    std::vector<int> parts(const std::string& in)
    {
        std::vector<int> out;
        std::string s = in;
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(0, 1);

        std::string cur;
        for (char c : s)
        {
            if (c >= '0' && c <= '9') { cur += c; continue; }
            if (c == '.')
            {
                if (cur.empty()) return {};
                out.push_back(std::stoi(cur));
                cur.clear();
                continue;
            }
            // A suffix such as "-beta" ends the number and is ignored; any
            // other character means we do not understand this tag at all.
            break;
        }
        if (!cur.empty()) out.push_back(std::stoi(cur));
        return out;
    }
}

bool LumenUpdate::isNewer(const std::string& candidate, const std::string& current)
{
    const std::vector<int> a = parts(candidate);
    const std::vector<int> b = parts(current);
    if (a.empty() || b.empty()) return false;   // never nag on a tag we cannot read

    for (size_t i = 0; i < a.size() || i < b.size(); ++i)
    {
        const int x = (i < a.size()) ? a[i] : 0;
        const int y = (i < b.size()) ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void LumenUpdate::checkWhenLoggedIn()
{
    if (sChecked) return;
    if (!gSavedSettings.getBOOL("LumenUpdateCheck")) return;
    sChecked = true;

    // Same shape as the login notice: wait for a logged-in viewer rather than
    // the login screen, because that is where somebody is ready to read
    // anything, then take the listener back down.
    LLEventPumps::instance().obtain("mainloop").listen("LumenUpdateCheck",
        [](const LLSD&)
        {
            if (LLStartUp::getStartupState() < STATE_STARTED) return false;

            LLEventPumps::instance().obtain("mainloop").stopListening("LumenUpdateCheck");

            LLCoros::instance().launch("LumenUpdateCheck", []()
            {
                LLCoreHttpUtil::HttpCoroutineAdapter adapter(
                    "LumenUpdate", LLCore::HttpRequest::DEFAULT_POLICY_ID);
                LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
                LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
                // GitHub asks for both; without a User-Agent it refuses.
                headers->append("Accept", "application/vnd.github+json");
                headers->append("User-Agent", "Lumen");

                LLSD reply = adapter.getAndSuspend(request, RELEASES_API,
                                                   LLCore::HttpOptions::ptr_t(), headers);

                const LLSD status = reply[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
                if (!LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(status))
                {
                    // Offline, rate-limited, GitHub down, repository private.
                    // All of these are silence: an update check that produces
                    // an error dialogue is worse than one that produces
                    // nothing, because nothing is what the user asked for.
                    LL_DEBUGS("LumenUpdate") << "no answer from the releases API" << LL_ENDL;
                    return;
                }

                const std::string tag  = reply.has("tag_name") ? reply["tag_name"].asString() : "";
                const std::string page = reply.has("html_url") ? reply["html_url"].asString() : "";
                if (tag.empty() || page.empty()) return;

                if (!LumenUpdate::isNewer(tag, LUMEN_VERSION))
                {
                    LL_DEBUGS("LumenUpdate") << "current: " << LUMEN_VERSION
                                             << ", newest: " << tag << LL_ENDL;
                    return;
                }

                LLSD args;
                args["VERSION"] = tag;
                args["URL"]     = page;
                LLNotificationsUtil::add("LumenUpdateAvailable", args);
                LL_INFOS("LumenUpdate") << "newer release: " << tag << LL_ENDL;
            });
            return false;
        });
}
