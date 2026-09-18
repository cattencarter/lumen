/**
 * @file llviewerhelp.cpp
 * @brief Utility functions for the Help system
 * @author Tofu Linden
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "llcommandhandler.h"
#include "llfloaterhelpbrowser.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llviewercontrol.h"
#include "llappviewer.h"
#include "lllogininstance.h"

#include "llviewerhelputil.h"
#include "llviewerhelp.h"

#include "llnotificationsutil.h"
#include "llweb.h" // <FS:Beq/> Support for opening help in external browser

// support for secondlife:///app/help/{TOPIC} SLapps
class LLHelpHandler : public LLCommandHandler
{
public:
    // requests will be throttled from a non-trusted browser
    LLHelpHandler() : LLCommandHandler("help", UNTRUSTED_CLICK_ONLY) {}

    bool handle(const LLSD& params, const LLSD& query_map, const std::string& grid, LLMediaCtrl* web) override
    {
        LLViewerHelp* vhelp = LLViewerHelp::getInstance();
        if (! vhelp)
        {
            return false;
        }

        // get the requested help topic name, or use the fallback if none
        std::string help_topic = vhelp->defaultTopic();
        if (params.size() >= 1)
        {
            help_topic = params[0].asString();
        }

        vhelp->showTopic(help_topic);
        return true;
    }
};
LLHelpHandler gHelpHandler;

//////////////////////////////
// implement LLHelp interface

std::string LLViewerHelp::getURL(const std::string &topic)
{
    // if the help topic is empty, use the default topic
    std::string help_topic = topic;
    if (help_topic.empty())
    {
        help_topic = defaultTopic();
    }

    // f1 help topic means: if the user is not logged in yet, show
    // the pre-login topic instead of the default fallback topic,
    // otherwise show help for the focused item
    if (help_topic == f1HelpTopic())
    {
        help_topic = getTopicFromFocus();
        if (help_topic == defaultTopic() && ! LLLoginInstance::getInstance()->authSuccess())
        {
            help_topic = preLoginTopic();
        }
    }

    return LLViewerHelpUtil::buildHelpURL( help_topic );
}

void LLViewerHelp::showTopic(const std::string& topic)
{
    // <FS:Beq> allow external browser for help topics
    auto url = getURL(topic);

    // <FS:AICtl> Never send this viewer's users to Firestorm's wiki.
    //
    // The author found the ? button doing exactly that FROM THE AI TAB, whose
    // topic slug is one Firestorm never had, so it offered their page about a
    // Preferences > Advanced tab instead. Renaming the viewer was supposed to
    // have removed every path to their volunteers and this one was missed.
    //
    // The test is on the URL rather than on `HelpURLFormat`, because the login
    // response can overwrite that setting (llstartup.cpp, `help_url_format`) --
    // so a clean default is not a guarantee and this is.
    //
    // Nothing replaces it: there is no Lumen wiki, and pointing at a page that
    // does not exist is the same fault on our own domain. The Assistant answers
    // this better anyway, by reading the menus rather than remembering them, so
    // the notice says so.
    if (url.empty() || url.find("firestormviewer.org") != std::string::npos)
    {
        LLNotificationsUtil::add("LumenNoHelpPages");
        return;
    }
    // </FS:AICtl>
    if ( LLWeb::useExternalBrowser(url) )
    {
        LLWeb::loadURLExternal(url);
    }
    else
    // </FS:Beq>
    LLFloaterReg::showInstance("help_browser", topic);
}

std::string LLViewerHelp::defaultTopic()
{
    // *hack: to be done properly
    return "this_is_fallbacktopic";
}

std::string LLViewerHelp::preLoginTopic()
{
    // *hack: to be done properly
    return "start";
}

std::string LLViewerHelp::f1HelpTopic()
{
    // *hack: to be done properly
    return "f1_help";
}

// <FS:TS> FIRE-7377: Add grid manager help topic to login screen
std::string LLViewerHelp::gridMgrHelpTopic()
{
    // *hack: to be done properly
    return "fs_grid_manager";
}
// </FS:TS>

//////////////////////////////
// our own interfaces

std::string LLViewerHelp::getTopicFromFocus()
{
    // use UI element with viewer's keyboard focus as basis for searching
    LLUICtrl* focused = dynamic_cast<LLUICtrl*>(gFocusMgr.getKeyboardFocus());

    if (focused)
    {
        std::string topic;
        if (focused->findHelpTopic(topic))
        {
            return topic;
        }
    }

    // didn't find a help topic in the UI hierarchy for focused
    // element, return the fallback topic name instead.
    return defaultTopic();
}

