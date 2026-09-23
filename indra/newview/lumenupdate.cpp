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
#include "llsdjson.h"

#include <boost/json.hpp>

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
                // <Lumen> stoi throws on a run of digits past INT_MAX, and
                // this runs inside a coroutine. A tag that long is not a
                // version we understand; say so rather than throw.
                if (cur.empty() || cur.size() > 9) return {};
                out.push_back(std::stoi(cur));
                cur.clear();
                continue;
            }
            // A suffix such as "-beta" ends the number and is ignored; any
            // other character means we do not understand this tag at all.
            break;
        }
        if (cur.size() > 9) return {};   // <Lumen>
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
    if (!gSavedSettings.getBOOL("LumenUpdateCheck"))
    {
        LL_INFOS("LumenUpdate") << "update check is switched off (LumenUpdateCheck)" << LL_ENDL;
        return;
    }
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
              // An exception here unwinds into LLCoros and is swallowed, which
              // is indistinguishable from a check that ran and had nothing to
              // say. Never again: say so, once, and still never interrupt.
              try
              {
                // Our own policy class and a timeout, both copied from the
                // caller in lumenaictl.cpp that works. DEFAULT_POLICY_ID with
                // no timeout is what left this suspended for ever.
                static const LLCore::HttpRequest::policy_t update_policy =
                    LLCore::HttpRequest::createPolicyClass();

                LLCoreHttpUtil::HttpCoroutineAdapter adapter("LumenUpdate", update_policy);
                LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
                LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
                // GitHub asks for both; without a User-Agent it refuses.
                headers->append("Accept", "application/vnd.github+json");
                headers->append("User-Agent", "Lumen");

                // <Lumen> This used to pass a NULL HttpOptions, and that is why
                // the update check never once worked: the coroutine was entered
                // -- proven with probes -- and never came back out of
                // getAndSuspend, so not one of the outcome branches below could
                // report anything. Every symptom was an absence.
                //
                // The caller that DOES work was twenty lines away in another
                // file the whole time: lumenaictl.cpp builds real options before
                // getRawAndSuspend. Third time this month that reading the
                // working neighbour would have been quicker than reasoning.
                LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
                options->setTimeout(20);             // WITHOUT THIS IT WAITS FOR EVER
                options->setFollowRedirects(true);   // the releases API can 301
                options->setRetries(0);              // one attempt; this is not urgent

                // getAndSuspend() parses the body as LLSD **XML**. GitHub
                // answers JSON, so the parse failed on every call this check
                // has ever made, the reply came back without `tag_name`, and
                // the branch below returned without a word. That is why this
                // has never once told anybody about a release.
                //
                // Raw body, then JSON, which is what lumenaichat.cpp has always
                // done. The body arrives under one of two keys: a success puts
                // it in HTTP_RESULTS_RAW, an error in HTTP_RESULTS_CONTENT.
                LLSD raw = adapter.getRawAndSuspend(request, RELEASES_API, options, headers);

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

                LLSD reply;
                try
                {
                    if (!body_text.empty()) reply = LlsdFromJson(boost::json::parse(body_text));
                }
                catch (...)
                {
                    LL_WARNS("LumenUpdate") << "the releases API answered something that is "
                                               "not JSON" << LL_ENDL;
                    return;
                }

                const LLSD status = raw[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
                if (!LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(status))
                {
                    // Offline, rate-limited, GitHub down, repository private.
                    // None of these gets a dialogue: an update check that
                    // interrupts you to say it could not check is worse than
                    // one that says nothing, because nothing is what the user
                    // asked for.
                    //
                    // But it is INFO rather than DEBUG, and that is the whole
                    // point of this line. At DEBUG a dead update check and a
                    // working one with nothing to report are the same silence,
                    // and the check ran for a release and a half without
                    // anybody being able to tell which they had. One line per
                    // session in a log nobody reads until something is wrong
                    // is the right price for being able to answer it.
                    LL_INFOS("LumenUpdate") << "no answer from the releases API: "
                                            << RELEASES_API << LL_ENDL;
                    return;
                }

                const std::string tag  = reply.has("tag_name") ? reply["tag_name"].asString() : "";
                const std::string page = reply.has("html_url") ? reply["html_url"].asString() : "";
                if (tag.empty() || page.empty())
                {
                    // The exit that hid all of this. Never silent again.
                    LL_WARNS("LumenUpdate") << "the releases API answered without a tag_name "
                                               "or html_url" << LL_ENDL;
                    return;
                }

                if (!LumenUpdate::isNewer(tag, LUMEN_VERSION))
                {
                    // Also INFO, for the same reason: this is the branch that
                    // says the check worked and there was nothing to say, and
                    // it is the only thing that distinguishes that from the
                    // check never having run at all.
                    LL_INFOS("LumenUpdate") << "up to date: running " << LUMEN_VERSION
                                            << ", newest released " << tag << LL_ENDL;
                    return;
                }

                LLSD args;
                args["VERSION"] = tag;
                args["URL"]     = page;
                LLNotificationsUtil::add("LumenUpdateAvailable", args);
                LL_INFOS("LumenUpdate") << "newer release: " << tag << LL_ENDL;
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
            return false;
        });
}
