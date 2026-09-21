/**
 * @file llviewermedia_streamingaudio.h
 * @author Tofu Linden, Sam Kolb
 * @brief LLStreamingAudio_MediaPlugins implementation - an implementation of the streaming audio interface which is implemented as a client of the media plugin API.
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
#include "linden_common.h"
#include "llpluginclassmedia.h"
#include "llpluginclassmediaowner.h"
#include "llviewermedia.h"

#include "llviewermedia_streamingaudio.h"

#include "llmimetypes.h"
#include "lldir.h"

LLStreamingAudio_MediaPlugins::LLStreamingAudio_MediaPlugins() :
    mMediaPlugin(NULL),
    // <Lumen>
    mRetryPending(false),
    mTriedPlainHttp(false),
    mDowngraded(false),
    mEverPlayed(false),
    mReconnectsLeft(0),
    // </Lumen>
    mGain(1.0)
{
    // nothing interesting to do?
    // we will lazily create a media plugin at play-time, if none exists.
}

LLStreamingAudio_MediaPlugins::~LLStreamingAudio_MediaPlugins()
{
    delete mMediaPlugin;
    mMediaPlugin = NULL;
}

void LLStreamingAudio_MediaPlugins::start(const std::string& url)
{
    if (!mMediaPlugin) // lazy-init the underlying media plugin
    {
        mMediaPlugin = initializeMedia("audio/mpeg"); // assumes that whatever media implementation supports mp3 also supports vorbis.
        LL_INFOS() << "streaming audio mMediaPlugin is now " << mMediaPlugin << LL_ENDL;
    }

    if(!mMediaPlugin)
        return;

    if (!url.empty())
    {
        LL_INFOS() << "Starting internet stream: " << url << LL_ENDL;

        mURL = url; // keep original url here for comparison purposes
        std::string snt_url = url;
        LLStringUtil::trim(snt_url);
        size_t pos = snt_url.find(' ');
        if (pos != std::string::npos)
        {
            // fmod permited having names after the url and people were using it.
            // People label their streams this way, ignore the 'label'.
            snt_url = snt_url.substr(0, pos);
        }
        // <Lumen> a fresh request, so forget what the last one learned
        resetStreamState();
        playURL(snt_url, false);
        // </Lumen>
    }
    else
    {
        LL_INFOS() << "setting stream to NULL"<< LL_ENDL;
        mURL.clear();
        resetStreamState(); // <Lumen>
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }
}

void LLStreamingAudio_MediaPlugins::stop()
{
    LL_INFOS() << "Stopping internet stream." << LL_ENDL;
    if(mMediaPlugin)
    {
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }

    mURL.clear();
    resetStreamState(); // <Lumen>

    // <FS:Ansariel> Stream meta data display
    updateMetadata();
}

void LLStreamingAudio_MediaPlugins::pause(int pause)
{
    if(!mMediaPlugin)
        return;

    if(pause)
    {
        LL_INFOS() << "Pausing internet stream." << LL_ENDL;
        mMediaPlugin->pause();
    }
    else
    {
        LL_INFOS() << "Unpausing internet stream." << LL_ENDL;
        mMediaPlugin->start();
    }
}

void LLStreamingAudio_MediaPlugins::update()
{
    if (mMediaPlugin)
        mMediaPlugin->idle();

    checkStreamHealth(); // <Lumen>

    // <FS:Ansariel> Stream meta data display
    updateMetadata();
}

int LLStreamingAudio_MediaPlugins::isPlaying()
{
    if (!mMediaPlugin)
        return 0; // stopped

    LLPluginClassMediaOwner::EMediaStatus status =
        mMediaPlugin->getStatus();

    switch (status)
    {
    case LLPluginClassMediaOwner::MEDIA_LOADING: // but not MEDIA_LOADED
    case LLPluginClassMediaOwner::MEDIA_PLAYING:
        return 1; // Active and playing
    case LLPluginClassMediaOwner::MEDIA_PAUSED:
        return 2; // paused
    default:
        return 0; // stopped
    }
}

void LLStreamingAudio_MediaPlugins::setGain(F32 vol)
{
    mGain = vol;

    if(!mMediaPlugin)
        return;

    vol = llclamp(vol, 0.f, 1.f);
    mMediaPlugin->setVolume(vol);
}

F32 LLStreamingAudio_MediaPlugins::getGain()
{
    return mGain;
}

std::string LLStreamingAudio_MediaPlugins::getURL()
{
    return mURL;
}

LLPluginClassMedia* LLStreamingAudio_MediaPlugins::initializeMedia(const std::string& media_type)
{
    LLPluginClassMediaOwner* owner = NULL;
    S32 default_size = 1; // audio-only - be minimal, doesn't matter
    F64 default_zoom = 1.0;
    LLPluginClassMedia* media_source = LLViewerMediaImpl::newSourceFromMediaType(media_type, owner, default_size, default_size, default_zoom);

    if (media_source)
    {
        media_source->setLoop(false); // audio streams are not expected to loop
    }

    return media_source;
}

// <FS:ND> stream metadata from plugin
void LLStreamingAudio_MediaPlugins::updateMetadata() noexcept
{
    if (!mMediaPlugin)
    {
        return;
    }

    if (mTitle != mMediaPlugin->getTitle() || mArtist != mMediaPlugin->getArtist())
    {
        mArtist = mMediaPlugin->getArtist();
        mTitle = mMediaPlugin->getTitle();
        mMetadata.clear();
        mMetadata["ARTIST"] = mArtist;
        mMetadata["TITLE"] = mTitle;

        mMetadataUpdateSignal(mMetadata);
    }
}
// </FS:ND>

// <Lumen>
// The media-plugin path is what Lumen, Megapahit and Linden Lab's own viewer
// all use for streaming music; only the viewers that license FMOD take a
// different road.  VLC is the stricter of the two, and until now the viewer
// threw away everything it learned when a stream would not open: isPlaying()
// folded MEDIA_ERROR into "stopped", so the play button flicked back and
// nobody was told why.  These four functions keep that knowledge.

namespace
{
    const S32 LUMEN_RECONNECT_ATTEMPTS = 3;
    const F32 LUMEN_RECONNECT_DELAY    = 3.0f;
    const F32 LUMEN_DOWNGRADE_DELAY    = 0.25f;

    bool lumen_is_https(const std::string& url)
    {
        if (url.size() <= 8)
        {
            return false;
        }
        std::string head = url.substr(0, 8);
        LLStringUtil::toLower(head);
        return head == "https://";
    }
}

void LLStreamingAudio_MediaPlugins::resetStreamState()
{
    mActiveURL.clear();
    mRetryURL.clear();
    mFailureReason.clear();
    mRetryPending   = false;
    mTriedPlainHttp = false;
    mDowngraded     = false;
    mEverPlayed     = false;
    mReconnectsLeft = LUMEN_RECONNECT_ATTEMPTS;
}

void LLStreamingAudio_MediaPlugins::playURL(const std::string& url, bool fresh_plugin)
{
    // A plugin that has reported an error will not play again, so a retry
    // needs a new one.  The first attempt reuses whatever start() lazily made.
    if (fresh_plugin && mMediaPlugin)
    {
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }

    if (!mMediaPlugin)
    {
        mMediaPlugin = initializeMedia("audio/mpeg");
        if (!mMediaPlugin)
        {
            mFailureReason = "the viewer could not start its media plugin";
            LL_WARNS() << mFailureReason << LL_ENDL;
            return;
        }
        mMediaPlugin->setVolume(llclamp(mGain, 0.f, 1.f));
    }

    mActiveURL = url;
    mPlayingFor.reset();
    mMediaPlugin->loadURI(url);
    mMediaPlugin->start();
    LL_INFOS() << "Playing stream..." << LL_ENDL;
}

void LLStreamingAudio_MediaPlugins::scheduleRetry(const std::string& url, F32 delay_seconds)
{
    mRetryURL     = url;
    mRetryPending = true;
    mRetryTimer.setTimerExpirySec(delay_seconds);
}

void LLStreamingAudio_MediaPlugins::checkStreamHealth()
{
    if (mActiveURL.empty())
    {
        return;
    }

    if (mRetryPending)
    {
        if (mRetryTimer.hasExpired())
        {
            mRetryPending = false;
            const std::string next = mRetryURL;
            mRetryURL.clear();
            playURL(next, true);
        }
        return;
    }

    if (!mMediaPlugin)
    {
        return;
    }

    const LLPluginClassMediaOwner::EMediaStatus status = mMediaPlugin->getStatus();

    if (status == LLPluginClassMediaOwner::MEDIA_PLAYING)
    {
        mEverPlayed = true;
        mFailureReason.clear();
        // Only a stream that has held for a while earns its reconnects back.
        // Resetting on every PLAYING made a station that plays for a second
        // and drops reconnect every three seconds for ever.
        if (mPlayingFor.getElapsedTimeF32() > 60.f)
        {
            mReconnectsLeft = LUMEN_RECONNECT_ATTEMPTS;
        }
        return;
    }

    if (status != LLPluginClassMediaOwner::MEDIA_ERROR
     && status != LLPluginClassMediaOwner::MEDIA_DONE)
    {
        return; // still opening, or nothing has happened yet
    }

    // It stopped without being asked to.

    // The commonest cause in Second Life is a parcel whose music URL says
    // https for a SHOUTcast server that has never spoken TLS.  FMOD connects
    // regardless of the scheme, so those parcels play in Firestorm and are
    // silent here.  Try once without TLS -- and say so in the log, because a
    // silent downgrade from TLS is not something to do quietly.
    if (!mTriedPlainHttp && !mEverPlayed && lumen_is_https(mActiveURL))
    {
        mTriedPlainHttp = true;
        const std::string plain = "http://" + mActiveURL.substr(8);
        LL_WARNS() << "Stream would not open over https: " << mActiveURL
                   << " -- retrying WITHOUT TLS as " << plain << LL_ENDL;
        scheduleRetry(plain, LUMEN_DOWNGRADE_DELAY);
        mDowngraded = true;
        return;
    }

    if (mEverPlayed && mReconnectsLeft > 0)
    {
        --mReconnectsLeft;
        LL_WARNS() << "Stream stopped: " << mActiveURL << " -- reconnecting, "
                   << mReconnectsLeft << " attempt(s) left after this" << LL_ENDL;
        scheduleRetry(mActiveURL, LUMEN_RECONNECT_DELAY);
        return;
    }

    if (mFailureReason.empty())
    {
        if (mEverPlayed)
        {
            mFailureReason = "the stream stopped and would not start again";
        }
        else if (mTriedPlainHttp)
        {
            mFailureReason = "that stream would not play over https or http, "
                             "so the station is probably off the air";
        }
        else
        {
            mFailureReason = "that stream would not play, so the station is "
                             "probably off the air or the address is wrong";
        }
        mDowngraded = false;
        LL_WARNS() << "Giving up on " << mActiveURL << ": " << mFailureReason << LL_ENDL;
    }
}

std::string LLStreamingAudio_MediaPlugins::getStreamNote() const
{
    if (!mFailureReason.empty())
    {
        return mFailureReason;
    }

    if (mDowngraded && mEverPlayed)
    {
        return "that server does not support https, so the viewer connected "
               "over plain http instead";
    }

    return std::string();
}
// </Lumen>
