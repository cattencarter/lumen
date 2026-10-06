/**
 * @file lumenaisight.h
 * @brief A picture of what is around the user, rendered for the assistant: no avatars in it,
 *        and a number on each object so the model can say exactly which one it means.
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
 *
 * The author, 2026-10-05: building and furnishing come first; describing the
 * world does not matter, because people can see it themselves. So this is a
 * tool for the MODEL -- to see a build and point at a part of it -- not a
 * camera for the person.
 *
 * Every avatar is left out, the user's own too, and so are animated objects,
 * particles and the lights people wear. That is the rule that makes a picture
 * of the world acceptable at all: the outfit pictures were given up because of
 * what avatars may look like, and nothing here may bring that back.
 *
 * The way it renders is the viewer's own 360 capture and reflection-probe
 * code: hide the avatars by render type, point the viewer's camera somewhere
 * else, draw into a buffer that is never put on screen, and put everything
 * back before the next frame. The user's view does not move or flicker, and
 * nothing reaches the server -- the field of view is set without the message
 * the viewer would otherwise send, and the camera is back where it was before
 * the next agent update reads it.
 */
#ifndef LUMEN_AISIGHT_H
#define LUMEN_AISIGHT_H

#include "llimage.h"
#include "llpointer.h"
#include "v3math.h"

#include <string>

class LLViewerObject;

namespace LumenAISight
{
    /** Where the picture is taken from, in agent coordinates. */
    struct View
    {
        LLVector3 origin;
        LLVector3 at { 1.f, 0.f, 0.f };   // where it looks, unit length
        LLVector3 up { 0.f, 0.f, 1.f };   // perpendicular to `at`, unit length
        F32       fov    = 1.0472f;        // vertical, radians (60 degrees)
        S32       width  = 640;            // multiples of 4, so rows need no padding
        S32       height = 480;
    };

    /** Point `view` at `target`, keeping "up" as close to `up_hint` as it allows. */
    void aim(View& view, const LLVector3& target, const LLVector3& up_hint);

    /**
     * Avatars, animated objects, particles and worn lights are left out while
     * one of these lives -- of the picture AND of a raycast, which honours the
     * same switches. Everything is put back as it was when it goes.
     */
    class WithoutAvatars
    {
    public:
        WithoutAvatars();
        ~WithoutAvatars();
        WithoutAvatars(const WithoutAvatars&) = delete;
        WithoutAvatars& operator=(const WithoutAvatars&) = delete;
    private:
        bool mAttachedLights = false;
    };

    /**
     * Render the world from `view` into a new RGB picture. Call it with a
     * WithoutAvatars alive. The viewer's camera is put back exactly as it was.
     * Rows run bottom-up, as glReadPixels gives them; the image encoders
     * expect that.
     */
    bool render(const View& view, LLPointer<LLImageRaw>& out, std::string& why);

    /** Where a point lands in the picture, counted from the TOP-left. False when it is outside it or behind. */
    bool project(const View& view, const LLVector3& point, F32& px, F32& py);

    /**
     * What a ray from `from` towards `to` hits first, ignoring avatars and
     * their name tags. Returns the object and where; null when nothing.
     * `beyond` carries the ray that far past `to`, so a ray aimed at the
     * middle of a table still finds the table. Hits on `ignore`'s linked set
     * are stepped past too, for a ray that starts inside an object.
     * `normal` and `face`, when given, get which way the surface faces there
     * (unit length, toward where the ray came from) and the face of the prim
     * it hit. `leaving`, when given, says the surface was met from behind:
     * the ray started inside that thing and was on its way out.
     */
    LLViewerObject* firstHit(const LLVector3& from, const LLVector3& to, F32 beyond, LLVector3& where,
                             const LLViewerObject* ignore = nullptr,
                             LLVector3* normal = nullptr, S32* face = nullptr,
                             bool* leaving = nullptr);

    /** The direction from the camera through a pixel of the picture, counted from the top-left. */
    LLVector3 rayThrough(const View& view, F32 px, F32 py);

    /** The axis-aligned box of a whole linked set, in agent coordinates. */
    void linksetBox(const LLViewerObject* root, LLVector3& min, LLVector3& max);

    /**
     * Draw `number` on a small dark tag centred at x, y (from the top-left).
     * `rect` is where the tag would go -- left, top, right, bottom -- so the
     * caller can keep tags from covering each other; with `draw` false only
     * that is worked out.
     */
    void label(LLImageRaw* raw, S32 x, S32 y, S32 number, S32 scale, bool draw, S32 rect[4]);

    /**
     * A dark scene made readable: when the picture's middle brightness is low,
     * it is lifted -- a gain set by its bright end, then a curve that opens the
     * shadows -- so the model can see what is in a dim room. Nothing in the
     * world or on the user's screen changes; only this picture. Returns true
     * when it changed anything. The author's cabin, 2026-10-05: the scene's own
     * light is what the picture had.
     */
    bool brighten(LLImageRaw* raw);
}

#endif // LUMEN_AISIGHT_H
