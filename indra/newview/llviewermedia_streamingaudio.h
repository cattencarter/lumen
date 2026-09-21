/**
 * @file llviewermedia_streamingaudio.h
 * @author Tofu Linden
 * @brief Definition of LLStreamingAudio_MediaPlugins implementation - an implementation of the streaming audio interface which is implemented as a client of the media plugins API.
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

#ifndef LL_VIEWERMEDIA_STREAMINGAUDIO_H
#define LL_VIEWERMEDIA_STREAMINGAUDIO_H


#include "stdtypes.h" // from llcommon

#include "llstreamingaudio.h"
#include "llframetimer.h"

class LLPluginClassMedia;

class LLStreamingAudio_MediaPlugins : public LLStreamingAudioInterface
{
 public:
    LLStreamingAudio_MediaPlugins();
    /*virtual*/ ~LLStreamingAudio_MediaPlugins();

    /*virtual*/ void start(const std::string& url);
    /*virtual*/ void stop();
    /*virtual*/ void pause(int pause);
    /*virtual*/ void update();
    /*virtual*/ int isPlaying();
    /*virtual*/ void setGain(F32 vol);
    /*virtual*/ F32 getGain();
    /*virtual*/ std::string getURL();

    // <FS:ND> For FS metadata extraction
    LLSD getCurrentMetadata() const noexcept { return mMetadata; }
    // </FS:ND>

    // <Lumen> Why the stream is not playing, in words a person can act on.
    // Empty when there is nothing worth saying.  See the .cpp for the reasons.
    std::string getStreamNote() const;
    // </Lumen>

private:
    LLPluginClassMedia* initializeMedia(const std::string& media_type);

    // <Lumen> a stream that fails must say so, and a wrong scheme must not be fatal
    void playURL(const std::string& url, bool fresh_plugin);
    void checkStreamHealth();
    void scheduleRetry(const std::string& url, F32 delay_seconds);
    void resetStreamState();
    // </Lumen>

    LLPluginClassMedia *mMediaPlugin;

    std::string mURL;

    // <Lumen>
    std::string  mActiveURL;        // what we actually handed the plugin
    std::string  mRetryURL;         // what the pending retry will ask for
    std::string  mFailureReason;    // empty unless we have given up
    bool         mRetryPending;
    bool         mTriedPlainHttp;   // this URL has already been downgraded once
    bool         mDowngraded;       // ...and the downgrade is what is playing
    bool         mEverPlayed;       // this URL played at least once
    S32          mReconnectsLeft;
    LLFrameTimer mRetryTimer;
    LLFrameTimer mPlayingFor;       // since this URL was last handed to the plugin
    // </Lumen>

    // <FS:ND> stream metadata from plugin
    void updateMetadata() noexcept;

    std::string mArtist;
    std::string mTitle;
    LLSD mMetadata;
    // </FS:ND>

    F32 mGain;
};


#endif //LL_VIEWERMEDIA_STREAMINGAUDIO_H
