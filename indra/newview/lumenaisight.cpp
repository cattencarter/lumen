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

#include "llagent.h"
#include "lldrawable.h"
#include "llface.h"
#include "llframetimer.h"
#include "llrender.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerwindow.h"
#include "llvovolume.h"
#include "pipeline.h"

#include <algorithm>
#include <cmath>
#include <string>

namespace LumenAISight
{
namespace
{
U32 sHiddenFrame = (U32)-1;   // the frame hiddenVolumes() last gathered in; -1 to gather again
}


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
    sHiddenFrame = (U32)-1;   // what is drawn changed with the picture: gather again

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

namespace
{
// The viewer's own raycast (LLOctreeIntersect::check) passes over anything the
// user's view did not draw in the last frame -- out of it, or not drawn yet
// since login. A coffee table beside the sofa, or the floor under a basket,
// was simply not there for place and set unless the user had looked at it
// (2026-10-06, the beta grid: a rug placed under a table said nothing was in
// the way). Those are met here on their own shapes: the volumes near the user
// that the last frame did not draw, gathered once a frame.
struct Hidden
{
    LLPointer<LLViewerObject> o;
    LLVector3 c;
    F32 r;
};
std::vector<Hidden> sHidden;

const std::vector<Hidden>& hiddenVolumes()
{
    const U32 frame = LLFrameTimer::getFrameCount();
    if (frame == sHiddenFrame)
    {
        return sHidden;
    }
    sHiddenFrame = frame;
    sHidden.clear();
    const LLVector3 me = gAgent.getPositionAgent();
    const S32 count = gObjectList.getNumObjects();
    for (S32 i = 0; i < count; ++i)
    {
        LLViewerObject* o = gObjectList.getObject(i);
        if (!o || o->isDead() || o->getPCode() != LL_PCODE_VOLUME || o->isAvatar()
            || o->isAttachment() || !o->getRegion())
        {
            continue;
        }
        LLDrawable* d = o->mDrawable.get();
        if (!d || d->isDead() || d->isVisible() || d->isState(LLDrawable::RIGGED)
            || !gPipeline.hasRenderType(d->getRenderType()))
        {
            continue;   // drawn: the viewer's raycast meets it already
        }
        const LLVector3 c = o->getPositionAgent();
        if ((c - me).magVecSquared() > 128.f * 128.f)
        {
            continue;
        }
        sHidden.push_back({ o, c, o->getScale().magVec() * 0.5f });
    }
    return sHidden;
}

// The nearest face of an undrawn volume along from -> to, as lumenShapeHit
// meets one: front faces only, nothing fully transparent or see-through.
LLViewerObject* hiddenHit(const LLVector3& from, const LLVector3& to, const LLViewerObject* ignore,
                          F32& dist, LLVector3& where, LLVector3& normal, S32& face)
{
    LLVector3 d = to - from;
    const F32 len = d.magVec();
    if (len < 0.001f)
    {
        return nullptr;
    }
    d /= len;
    LLViewerObject* best = nullptr;
    for (const Hidden& h : hiddenVolumes())
    {
        LLViewerObject* o = h.o.get();
        if (!o || o->isDead() || (ignore && o->getRootEdit() == ignore))
        {
            continue;
        }
        const LLVector3 pc = h.c - from;
        const F32 t = llclamp(pc * d, 0.f, len);
        if ((pc - d * t).magVecSquared() > h.r * h.r)
        {
            continue;   // its bounding sphere is nowhere near the line
        }
        LLVOVolume* v = dynamic_cast<LLVOVolume*>(o);
        LLVolume* vol = v ? v->getVolume() : nullptr;
        if (!vol || vol->getNumVolumeFaces() <= 0 || (v->isMesh() && !vol->isMeshAssetLoaded())
            || v->mDrawable.isNull() || v->mDrawable->isDead())
        {
            continue;
        }
        const LLVector3 a = v->agentPositionToVolume(from), b = v->agentPositionToVolume(to);
        LLVector4a s, e;
        s.load3(a.mV);
        e.load3(b.mV);
        const LLDrawable* dr = v->mDrawable.get();
        for (S32 i = 0; i < vol->getNumVolumeFaces(); ++i)
        {
            const LLTextureEntry* te = v->getTE(i);
            if (te && te->getColor().mV[VALPHA] < 0.05f) continue;
            const LLFace* f = (dr && i < dr->getNumFaces()) ? dr->getFace(i) : nullptr;
            if (f && f->isInAlphaPool()) continue;
            LLVector4a hit, n;
            n.clear();
            if (vol->lineSegmentIntersect(s, e, i, &hit, nullptr, &n) < 0) continue;
            const LLVector3 w = v->volumePositionToAgent(LLVector3(hit.getF32ptr()));
            const F32 dd = (w - from).magVec();
            if (!best || dd < dist)
            {
                best = o;
                dist = dd;
                where = w;
                normal = v->volumeDirectionToAgent(LLVector3(n.getF32ptr()));
                face = i;
            }
        }
    }
    return best;
}
} // namespace

LLViewerObject* firstHit(const LLVector3& from, const LLVector3& to, F32 beyond, LLVector3& where,
                         const LLViewerObject* ignore, LLVector3* normal, S32* face, bool* leaving,
                         bool* gave_up)
{
    if (leaving) *leaving = false;
    if (gave_up) *gave_up = false;
    LLVector3 dir = to - from;
    const F32 len = dir.magVec();
    if (len < 0.001f)
    {
        return nullptr;
    }
    dir /= len;
    LLVector3 start = from;
    LLVector3 end = to + dir * llmax(beyond, 0.f);
    // Second Life's land test walks a ray by its HORIZONTAL length
    // (LLVOSurfacePatch::lineSegmentIntersect), so a ray straight down never
    // meets the land at all -- a box floating over bare ground had "nothing
    // under it". Leaning it 5 cm over its whole length is enough, and is less
    // than a millimetre where it meets something a metre away.
    if (fabsf(end.mV[VX] - from.mV[VX]) + fabsf(end.mV[VY] - from.mV[VY]) < 0.05f)
    {
        end.mV[VX] = from.mV[VX] + 0.05f;
        end.mV[VY] = from.mV[VY];
        dir = end - from;
        dir.normVec();
    }

    // The raycast honours the render-type switches, so with WithoutAvatars
    // alive it passes through avatars and what they wear -- but it tests every
    // avatar's NAME TAG regardless (pipeline.cpp, "silly, isn't it?"). A tag
    // is not in the picture, so a hit on one is stepped past.
    // Two budgets. Tags and worn things: a few, then it gives up. `ignore`'s
    // own faces: as many as the ray meets on its way out of it -- a bookshelf
    // of thirty linked books or a sofa of cushions has more than eight, and
    // when one shared budget of eight ran out there it answered "nothing", so
    // the space check stopped short of what lay beyond (the review,
    // 2026-10-06). Each step moves on 2 cm, so the cap on those is only a
    // guard; running into either is said through `gave_up`.
    // What the user's view has not drawn, met on its shape (hiddenHit above).
    F32 hid_dist = 0.f;
    LLVector3 hid_where, hid_n;
    S32 hid_face = -1;
    LLViewerObject* hid = hiddenHit(from, end, ignore, hid_dist, hid_where, hid_n, hid_face);
    auto useHidden = [&]() -> LLViewerObject*
    {
        where = hid_where;
        const F32 len_n = hid_n.magVec();
        if (leaving) *leaving = len_n > 0.001f && (hid_n * dir) > 0.5f * len_n;
        if (normal)
        {
            *normal = hid_n;
            if (normal->magVec() > 0.001f) normal->normVec();
            if (*normal * dir > 0.f) *normal = -*normal;
        }
        if (face) *face = hid_face;
        return hid;
    };
    S32 tags = 0, own = 0;
    while (tags < 8 && own < 1024)
    {
        LLVector4a s, e, hit, n;
        s.load3(start.mV);
        e.load3(end.mV);
        n.clear();
        S32 f = -1;
        LLViewerObject* o = gPipeline.lineSegmentIntersectInWorld(
            s, e,
            false,   // pick_transparent: an invisible prim hides nothing
            false,   // pick_rigged
            true,    // pick_unselectable: it is still in the way
            false,   // pick_reflection_probe
            &f, nullptr, nullptr, &hit, nullptr, &n, nullptr);
        if (!o)
        {
            return hid ? useHidden() : nullptr;
        }
        where.set(hit.getF32ptr());
        const bool mine = ignore && o->getRootEdit() == ignore;
        if (hid && hid_dist < (where - from).magVec())
        {
            return useHidden();   // an undrawn thing comes first
        }
        if (!o->isAvatar() && !o->isAttachment() && !mine)
        {
            if (leaving)
            {
                // The raycast meets a triangle from its front only
                // (LLTriangleRayIntersect), so by the triangle's own normal no
                // face is ever met from behind -- and that normal is not handed
                // back. What is handed back is the smoothed normal the maker
                // gave the mesh. It points along the ray where the two
                // disagree: a mesh turned inside out, whose normals still say
                // which side is out -- there the ray really is leaving it --
                // or a ray grazing a smooth-shaded curve, where the smoothing
                // leans a few degrees past the edge. Only a normal well along
                // the ray counts, so the second is not read as the first (the
                // review, 2026-10-06).
                const LLVector3 raw(n.getF32ptr());
                const F32 len_n = raw.magVec();
                *leaving = len_n > 0.001f && (raw * dir) > 0.5f * len_n;
            }
            if (normal)
            {
                normal->set(n.getF32ptr());
                if (normal->magVec() > 0.001f) normal->normVec();
                // A surface is seen from the side the ray came from.
                if (*normal * dir > 0.f) *normal = -*normal;
            }
            if (face) *face = f;
            return o;
        }
        if (mine) ++own; else ++tags;
        start = where + dir * 0.02f;
        if ((start - from).magVec() >= (end - from).magVec())
        {
            return hid ? useHidden() : nullptr;
        }
    }
    if (gave_up) *gave_up = true;
    return nullptr;
}

LLVector3 rayThrough(const View& view, F32 px, F32 py)
{
    const LLVector3 left = view.up % view.at;
    const F32 half_v = tanf(view.fov * 0.5f);
    const F32 aspect = (F32)view.width / (F32)view.height;
    const F32 nx = 2.f * px / (F32)view.width - 1.f;
    const F32 ny = 1.f - 2.f * py / (F32)view.height;
    LLVector3 d = view.at - left * (nx * half_v * aspect) + view.up * (ny * half_v);
    d.normVec();
    return d;
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

bool brighten(LLImageRaw* raw)
{
    if (!raw || raw->getComponents() < 3 || raw->getWidth() <= 0 || raw->getHeight() <= 0) return false;
    U8* d = raw->getData();
    const S32 comps = raw->getComponents();
    const S32 n = raw->getWidth() * raw->getHeight();

    // How bright it is: the luminance histogram, its mean and its 99th percentile.
    S32 hist[256] = { 0 };
    F64 sum = 0.0;
    for (S32 i = 0; i < n; ++i)
    {
        const U8* p = d + i * comps;
        const S32 y = (S32)(0.299f * p[0] + 0.587f * p[1] + 0.114f * p[2] + 0.5f);
        ++hist[llclamp(y, 0, 255)];
        sum += y;
    }
    const F32 mean = (F32)(sum / n);
    if (mean >= 85.f) return false;   // light enough already: leave it exactly as drawn
    S32 seen = 0, p99 = 255;
    for (S32 v = 0; v < 256; ++v)
    {
        seen += hist[v];
        if (seen >= n * 0.99) { p99 = v; break; }
    }
    // A gain that brings its bright end near white, at most four times, then a
    // curve that lifts the middle towards a readable grey.
    const F32 gain = llclamp(235.f / (F32)llmax(p99, 1), 1.f, 4.f);
    const F32 lifted = llclamp(mean * gain / 255.f, 0.01f, 0.99f);
    const F32 gamma = llclamp(logf(105.f / 255.f) / logf(lifted), 0.45f, 1.f);
    if (gain < 1.05f && gamma > 0.97f) return false;
    U8 lut[256];
    for (S32 v = 0; v < 256; ++v)
    {
        const F32 x = llclamp(v * gain / 255.f, 0.f, 1.f);
        lut[v] = (U8)llclamp((S32)(powf(x, gamma) * 255.f + 0.5f), 0, 255);
    }
    for (S32 i = 0; i < n; ++i)
    {
        U8* p = d + i * comps;
        p[0] = lut[p[0]];
        p[1] = lut[p[1]];
        p[2] = lut[p[2]];
    }
    return true;
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
