/**
 * @file lumenaisight.cpp
 * @brief A picture of what is around the user, rendered for the assistant, without avatars.
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

#include "lumenaisight.h"

#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerwindow.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace LumenAISight
{

void aim(View& view, const LLVector3& target, const LLVector3& up_hint)
{
    LLVector3 at = target - view.origin;
    if (at.magVec() < 0.001f)
    {
        at.setVec(1.f, 0.f, 0.f);
    }
    at.normVec();

    LLVector3 hint = up_hint;
    hint.normVec();
    // Looking straight along the hint leaves no way to tell up from sideways;
    // any other direction will do, and north is the one people expect at the
    // top of a picture taken from overhead.
    if (fabsf(at * hint) > 0.999f)
    {
        hint = fabsf(at.mV[VY]) < 0.9f ? LLVector3(0.f, 1.f, 0.f) : LLVector3(1.f, 0.f, 0.f);
    }
    // The camera's own frame (LLCoordFrame::lookDir): left = up x at, up = at x left.
    LLVector3 left = hint % at;
    left.normVec();
    LLVector3 up = at % left;
    up.normVec();

    view.at = at;
    view.up = up;
}

WithoutAvatars::WithoutAvatars()
{
    // As LLViewerWindow::cubeSnapshot hides "dynamic" things from a reflection
    // probe: inside a pushed mask, so popping it restores exactly what was on.
    gPipeline.pushRenderTypeMask();
    static const U32 hidden[] = {
        LLPipeline::RENDER_TYPE_AVATAR,       // every avatar and what they wear
        LLPipeline::RENDER_TYPE_CONTROL_AV,   // animated objects: pets, and avatars in all but name
        LLPipeline::RENDER_TYPE_PARTICLES
    };
    for (U32 type : hidden)
    {
        if (gPipeline.hasRenderType(type))
        {
            LLPipeline::toggleRenderType(type);
        }
    }
    // As the 360 capture does: a light somebody wears still lights the room
    // with the avatar gone, and shows where they stand.
    mAttachedLights = LLPipeline::sRenderAttachedLights;
    LLPipeline::sRenderAttachedLights = false;
}

WithoutAvatars::~WithoutAvatars()
{
    gPipeline.popRenderTypeMask();
    LLPipeline::sRenderAttachedLights = mAttachedLights;
}

bool render(const View& view, LLPointer<LLImageRaw>& out, std::string& why)
{
    if (!gViewerWindow || !LLViewerCamera::instanceExists())
    {
        why = "The viewer is not drawing anything yet.";
        return false;
    }
    LLViewerCamera* camera = LLViewerCamera::getInstance();

    // Saved and restored whole, as cubeSnapshot does, so nothing about the
    // user's own view survives as a side effect.
    LLViewerCamera saved_camera = *camera;
    const glm::mat4 saved_proj = get_current_projection();
    const glm::mat4 saved_mod  = get_current_modelview();
    const S32 old_occlusion = LLPipeline::sUseOcclusion;
    // Occlusion is worked out from the last frames the USER saw, so with it on
    // a view from anywhere else misses whatever the user's own view hid.
    LLPipeline::sUseOcclusion = 0;

    // The aspect first: the field of view is clamped against it.
    camera->setAspect((F32)view.width / (F32)view.height);
    // NoBroadcast: setView would tell the simulator the user's field of view
    // had changed, and nothing about this picture may reach the server.
    camera->setViewNoBroadcast(view.fov);
    camera->setOrigin(view.origin);
    camera->lookDir(view.at, view.up);

    out = new LLImageRaw(view.width, view.height, 3);
    // Several passes, as the 360 capture's own comment explains: with one, much
    // of what the region has sent is missing from the picture.
    const S32 passes = llclamp((S32)gSavedSettings.getU32("360CaptureNumRenderPasses"), 1, 5);
    const bool ok = gViewerWindow->simpleSnapshot(out, view.width, view.height, passes);

    *camera = saved_camera;
    set_current_modelview(saved_mod);
    set_current_projection(saved_proj);
    gViewerWindow->setup3DViewport();
    LLPipeline::sUseOcclusion = old_occlusion;

    if (!ok)
    {
        why = "The picture could not be drawn.";
        out = nullptr;
        return false;
    }
    return true;
}

bool project(const View& view, const LLVector3& point, F32& px, F32& py)
{
    const LLVector3 d = point - view.origin;
    const F32 depth = d * view.at;
    if (depth < 0.05f)
    {
        return false;   // behind the camera, or in its face
    }
    const LLVector3 left = view.up % view.at;
    const F32 half_v = tanf(view.fov * 0.5f);
    const F32 aspect = (F32)view.width / (F32)view.height;
    const F32 nx = -(d * left) / (depth * half_v * aspect);
    const F32 ny =  (d * view.up) / (depth * half_v);
    px = (nx + 1.f) * 0.5f * (F32)view.width;
    py = (1.f - ny) * 0.5f * (F32)view.height;
    return nx >= -1.f && nx <= 1.f && ny >= -1.f && ny <= 1.f;
}

LLViewerObject* firstHit(const LLVector3& from, const LLVector3& to, F32 beyond, LLVector3& where,
                         const LLViewerObject* ignore)
{
    LLVector3 dir = to - from;
    const F32 len = dir.magVec();
    if (len < 0.001f)
    {
        return nullptr;
    }
    dir /= len;
    LLVector3 start = from;
    const LLVector3 end = to + dir * llmax(beyond, 0.f);

    // The raycast honours the render-type switches, so with WithoutAvatars
    // alive it passes through avatars and what they wear -- but it tests every
    // avatar's NAME TAG regardless (pipeline.cpp, "silly, isn't it?"). A tag
    // is not in the picture, so a hit on one is stepped past.
    for (S32 tries = 0; tries < 8; ++tries)
    {
        LLVector4a s, e, hit;
        s.load3(start.mV);
        e.load3(end.mV);
        LLViewerObject* o = gPipeline.lineSegmentIntersectInWorld(
            s, e,
            false,   // pick_transparent: an invisible prim hides nothing
            false,   // pick_rigged
            true,    // pick_unselectable: it is still in the way
            false,   // pick_reflection_probe
            nullptr, nullptr, nullptr, &hit, nullptr, nullptr, nullptr);
        if (!o)
        {
            return nullptr;
        }
        where.set(hit.getF32ptr());
        if (!o->isAvatar() && !o->isAttachment() && !(ignore && o->getRootEdit() == ignore))
        {
            return o;
        }
        start = where + dir * 0.02f;
        if ((start - from).magVec() >= (end - from).magVec())
        {
            return nullptr;
        }
    }
    return nullptr;
}

void linksetBox(const LLViewerObject* root, LLVector3& min, LLVector3& max)
{
    min.setVec( 1.e9f,  1.e9f,  1.e9f);
    max.setVec(-1.e9f, -1.e9f, -1.e9f);
    auto addPrim = [&](const LLViewerObject* p)
    {
        const LLVector3 half = p->getScale() * 0.5f;
        const LLQuaternion rot = p->getRenderRotation();
        const LLVector3 centre = p->getPositionAgent();
        for (S32 i = 0; i < 8; ++i)
        {
            const LLVector3 corner((i & 1) ? half.mV[VX] : -half.mV[VX],
                                   (i & 2) ? half.mV[VY] : -half.mV[VY],
                                   (i & 4) ? half.mV[VZ] : -half.mV[VZ]);
            const LLVector3 c = centre + corner * rot;
            for (S32 a = 0; a < 3; ++a)
            {
                min.mV[a] = llmin(min.mV[a], c.mV[a]);
                max.mV[a] = llmax(max.mV[a], c.mV[a]);
            }
        }
    };
    addPrim(root);
    for (const LLViewerObject* child : root->getChildren())
    {
        if (child && !child->isAvatar())   // a seated avatar is a child of its seat
        {
            addPrim(child);
        }
    }
}

namespace
{
    // 5 x 7 digits, the high bit of each row's five the leftmost column.
    const U8 DIGITS[10][7] = {
        { 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E },   // 0
        { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },   // 1
        { 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F },   // 2
        { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },   // 3
        { 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 },   // 4
        { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },   // 5
        { 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E },   // 6
        { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },   // 7
        { 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E },   // 8
        { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },   // 9
    };

    void putPixel(LLImageRaw* raw, S32 x, S32 y, U8 r, U8 g, U8 b)
    {
        const S32 w = raw->getWidth(), h = raw->getHeight();
        if (x < 0 || y < 0 || x >= w || y >= h) return;
        // The picture's rows run bottom-up; x, y here count from the top.
        U8* p = raw->getData() + 3 * ((h - 1 - y) * w + x);
        p[0] = r; p[1] = g; p[2] = b;
    }
}

void label(LLImageRaw* raw, S32 x, S32 y, S32 number, S32 scale, bool draw, S32 rect[4])
{
    const std::string text = std::to_string(llmax(number, 0));
    const S32 s   = llmax(scale, 1);
    const S32 len = (S32)text.size();
    const S32 tw  = len * 5 * s + (len - 1) * s;
    const S32 th  = 7 * s;
    const S32 pad = s + 1;
    rect[0] = x - tw / 2 - pad;
    rect[1] = y - th / 2 - pad;
    rect[2] = rect[0] + tw + 2 * pad;
    rect[3] = rect[1] + th + 2 * pad;
    if (!draw || !raw)
    {
        return;
    }
    // A dark tag with a yellow edge: readable on a white wall and on a night sky.
    for (S32 py = rect[1] - 1; py <= rect[3]; ++py)
    {
        for (S32 px = rect[0] - 1; px <= rect[2]; ++px)
        {
            const bool edge = py == rect[1] - 1 || py == rect[3] || px == rect[0] - 1 || px == rect[2];
            if (edge) putPixel(raw, px, py, 255, 210, 0);
            else      putPixel(raw, px, py, 0, 0, 0);
        }
    }
    S32 cx = rect[0] + pad;
    const S32 cy = rect[1] + pad;
    for (char ch : text)
    {
        const U8* rows = DIGITS[ch - '0'];
        for (S32 row = 0; row < 7; ++row)
        {
            for (S32 col = 0; col < 5; ++col)
            {
                if (rows[row] & (0x10 >> col))
                {
                    for (S32 dy = 0; dy < s; ++dy)
                        for (S32 dx = 0; dx < s; ++dx)
                            putPixel(raw, cx + col * s + dx, cy + row * s + dy, 255, 255, 255);
                }
            }
        }
        cx += 6 * s;
    }
}

} // namespace LumenAISight
