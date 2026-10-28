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
#include "llcorehttputil.h"
#include "llcoros.h"
#include "llnotificationsutil.h"
#include "llsdjson.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"
#include "llweb.h"
#include "llwindow.h"

#include <boost/json.hpp>

#include <ctime>
#include <vector>

namespace
{
    // Where Download goes when the file names no https page of its own.
    const std::string DOWNLOAD_PAGE = "https://needfulsl.com/lumen/";

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
                // stoi throws on a run of digits past INT_MAX, and this runs
                // inside a coroutine. A version that long is not one we
                // understand; say so rather than throw.
                if (cur.empty() || cur.size() > 9) return {};
                out.push_back(std::stoi(cur));
                cur.clear();
                continue;
            }
            // A suffix such as "-beta" ends the number and is ignored; any
            // other character means we do not understand this version at all.
            break;
        }
        if (cur.size() > 9) return {};
        if (!cur.empty()) out.push_back(std::stoi(cur));
        return out;
    }

    /** Today on the computer's own calendar, "YYYY-MM-DD": told again on
     *  another day, not after 24 hours. */
    std::string today()
    {
        const time_t now = time(nullptr);
        struct tm local {};
#if LL_WINDOWS
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char day[16] = {};
        strftime(day, sizeof(day), "%Y-%m-%d", &local);
        return day;
    }

    void checkCoro(const std::string& url)
    {
        LL_INFOS("LumenUpdate") << "asking " << url << " for the newest version" << LL_ENDL;

        // Our own policy class and a timeout. DEFAULT_POLICY_ID with no
        // timeout is what once left this suspended for ever.
        static const LLCore::HttpRequest::policy_t update_policy =
            LLCore::HttpRequest::createPolicyClass();
        LLCoreHttpUtil::HttpCoroutineAdapter adapter("LumenUpdate", update_policy);
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        options->setTimeout(20);             // WITHOUT THIS IT WAITS FOR EVER
        options->setTransferTimeout(20);
        options->setFollowRedirects(true);
        options->setRetries(0);              // one request; the next session asks again
        LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
        headers->append("Accept", "application/json");
        headers->append("User-Agent", "Lumen");

        // Raw body, then JSON: getAndSuspend() would parse it as LLSD XML and
        // fail on every answer. A success puts the body in HTTP_RESULTS_RAW,
        // an error in HTTP_RESULTS_CONTENT.
        LLSD raw = adapter.getRawAndSuspend(request, url, options, headers);
        const LLSD status = raw[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        if (!LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(status))
        {
            // Offline, no file yet, the site down: no dialogue -- a check that
            // interrupts to say it could not check is worse than silence. But
            // INFO, so a dead check and an up-to-date one can be told apart.
            LL_INFOS("LumenUpdate") << "no answer from " << url << ": "
                                    << LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(status).toString()
                                    << LL_ENDL;
            return;
        }

        std::string body_text;
        for (const std::string& key : { LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW,
                                        LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_CONTENT })
        {
            if (raw.has(key) && raw[key].isBinary())
            {
                const LLSD::Binary& bytes = raw[key].asBinary();
                if (!bytes.empty()) { body_text.assign(bytes.begin(), bytes.end()); break; }
            }
        }

        boost::system::error_code ec;
        const boost::json::value json = boost::json::parse(body_text, ec);
        const LLSD newest = ec ? LLSD() : LlsdFromJson(json);
        const std::string version = newest.has("version") ? newest["version"].asString() : std::string();
        if (version.empty())
        {
            LL_WARNS("LumenUpdate") << "the answer names no version" << LL_ENDL;
            return;
        }
        if (!LumenUpdate::isNewer(version, LUMEN_VERSION))
        {
            LL_INFOS("LumenUpdate") << "up to date: running " << LUMEN_VERSION
                                    << ", the newest " << version << LL_ENDL;
            return;
        }

        // Once a day while this one is older, whichever button -- Download
        // too, so the message still comes the next day to a viewer that was
        // not updated; a newer version at once.
        if (gSavedSettings.getString("LumenUpdateSeen") == version
            && gSavedSettings.getString("LumenUpdateToldDay") == today())
        {
            LL_INFOS("LumenUpdate") << "Lumen " << version << " is out; already told today" << LL_ENDL;
            return;
        }

        // Only a web page of https, in the browser, when Download is clicked.
        std::string link = newest["url"].asString();
        if (link.rfind("https://", 0) != 0)
        {
            link = DOWNLOAD_PAGE;
        }
        std::string notes = newest["notes"].asString();
        LLStringUtil::trim(notes);
        if (notes.size() > 600)
        {
            notes = utf8str_truncate(notes, 600) + "...";
        }
        std::string message = "Lumen " + version + " is out (this is " + LUMEN_VERSION + ").";
        if (!notes.empty())
        {
            message += "\n\n" + notes;
        }

        LL_INFOS("LumenUpdate") << "Lumen " << version << " is out: telling" << LL_ENDL;
        gSavedSettings.setString("LumenUpdateSeen", version);
        gSavedSettings.setString("LumenUpdateToldDay", today());
        LLNotificationsUtil::add("LumenUpdateAvailable", LLSD().with("MESSAGE", message), LLSD(),
            [link](const LLSD& notification, const LLSD& response)
            {
                // Download: the click is the consent, so no second question
                // as LLWeb::loadURLExternal would ask.
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 0 && gViewerWindow
                    && !gSavedSettings.getBOOL("DisableExternalBrowser"))
                {
                    gViewerWindow->getWindow()->spawnWebBrowser(LLWeb::escapeURL(link), true);
                }
                return false;
            });
    }
}

bool LumenUpdate::isNewer(const std::string& candidate, const std::string& current)
{
    const std::vector<int> a = parts(candidate);
    const std::vector<int> b = parts(current);
    if (a.empty() || b.empty()) return false;   // never nag on a version we cannot read

    for (size_t i = 0; i < a.size() || i < b.size(); ++i)
    {
        const int x = (i < a.size()) ? a[i] : 0;
        const int y = (i < b.size()) ? b[i] : 0;
        if (x != y) return x > y;
    }
    return false;
}

void LumenUpdate::check()
{
    static bool asked = false;
    if (asked) return;
    asked = true;
    if (!gSavedSettings.getBOOL("LumenUpdateCheck"))
    {
        LL_INFOS("LumenUpdate") << "update check is switched off (LumenUpdateCheck)" << LL_ENDL;
        return;
    }
    const std::string url = gSavedSettings.getString("LumenUpdateURL");
    if (url.rfind("https://", 0) != 0)
    {
        LL_WARNS("LumenUpdate") << "LumenUpdateURL is not https, so nothing is asked: " << url << LL_ENDL;
        return;
    }
    LLCoros::instance().launch("LumenUpdateCheck", [url]()
    {
        // An exception unwinding into LLCoros is swallowed, which looks the
        // same as a check with nothing to say. Say so, and still never
        // interrupt.
        try
        {
            checkCoro(url);
        }
        catch (const std::exception& e)
        {
            LL_WARNS("LumenUpdate") << "update check threw: " << e.what() << LL_ENDL;
        }
        catch (...)
        {
            LL_WARNS("LumenUpdate") << "update check threw something that is not "
                                       "a std::exception" << LL_ENDL;
        }
    });
}
