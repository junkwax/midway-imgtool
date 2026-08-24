/*************************************************************
 * platform/ui_canvas.cpp
 * Canvas/World-View helpers declared in ui_canvas.h.
 *************************************************************/
#include <imgui_internal.h>
#include "ui_internal.h"
#include "image_ops.h"
#include "img_io.h"
#include "mk2_fatality.h"
#include "mk2_hitbox.h"
#include "load2_verify.h"
#include "ui_canvas.h"
#include "palette_math.h"
#include "ui_palette.h"
#include "ui_reactions.h"  /* React canvas tab */

#include "anipoint.h"       /* secondary_anipoint_in_use */
#include "paint_tools.h"     /* blur / smudge / content-aware erase */
#include "anipoint_edit.h"  /* set_primary_anipoint_with_sequence */
#include "img_format.h"     /* get_img */
#include "img_util.h"       /* img_name_string */
#include "shim_vid.h"       /* g_palette */
#include "ui_timeline.h"    /* ClampTimelineHold */
#include "world_render.h"   /* doc_get_img */
#include "bdd_bg.h"         /* World View reference background */
#include "sprite_resize_ops.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static SDL_Texture *s_world_onion_tex = NULL;
static int s_world_onion_tex_w = 0;
static int s_world_onion_tex_h = 0;
static int s_world_onion_idx = -1;

static const int kWorldFrameMirrorX = 1;
static const int kWorldFrameMirrorY = 2;

/* Magnitude past which a Y anipoint cannot be describing a 512x254 playfield
   and is therefore a corrupt word rather than an aggressive placement. */
static const int kWorldAnipointSaneLimit = 0x4000;

/* A Y anchor worth warning about.

   Deliberately NOT "aniy is negative". The renderer places art at
   anchor - aniy, so a negative aniy is simply art that hangs *below* its
   anchor, and stock Midway art leans on that constantly -- 79 of the 100
   frames in UGMO8.IMG carry a negative aniy, UGSTANCE1 (the idle stance)
   among them. The original check flagged every one of those, plus -1 a
   second time via the 0xFFFF test. What is genuinely broken is a word that
   cannot be an anipoint at all. */
static bool WorldBadYAnchor(const IMG *img, int effective_ay)
{
    if (!img) return false;
    /* Kept from the original check. 0x4000 appears nowhere else in this
       codebase's format handling, so treat it as a suspicious literal rather
       than claim it is a documented sentinel. */
    if ((int)img->aniy == 0x4000) return true;
    return effective_ay <= -kWorldAnipointSaneLimit ||
           effective_ay >= kWorldAnipointSaneLimit;
}

static int  g_selection_add_mask_w = 0;
static int  g_selection_add_mask_h = 0;
static std::vector<bool> g_selection_add_mask;


WorldViewState &WorldView(void)
{
    static WorldViewState state;
    return state;
}

BddBackground &WorldBackground(void)
{
    static BddBackground bg;
    return bg;
}

AnipointLinkState &AnipointLink(void)
{
    static AnipointLinkState state;
    return state;
}

float ZoomFitScaleForAvailable(const ImVec2 &avail)
{
    if (g_img_tex_w <= 0 || g_img_tex_h <= 0) return 1.0f;
    float fitscale = (float)(int)(avail.x / (float)g_img_tex_w);
    if (fitscale < 1.0f) fitscale = 1.0f;
    float fith = (float)g_img_tex_h * fitscale;
    if (fith > avail.y) fitscale = (float)(int)(avail.y / (float)g_img_tex_h);
    if (fitscale < 1.0f) fitscale = 1.0f;
    return fitscale;
}

float ZoomDisplayScaleForAvailable(const ImVec2 &avail)
{
    if (g_zoom_fit) return ZoomFitScaleForAvailable(avail);
    if (g_zoom < 1.0f) return 1.0f;
    if (g_zoom > ZOOM_MAX) return ZOOM_MAX;
    return g_zoom;
}

void ZoomClampPanForScale(const ImVec2 &avail, float scale)
{
    if (g_img_tex_w <= 0 || g_img_tex_h <= 0 || avail.x <= 0.0f || avail.y <= 0.0f) {
        g_pan_x = 0.0f;
        g_pan_y = 0.0f;
        return;
    }

    float tw = (float)g_img_tex_w * scale;
    float th = (float)g_img_tex_h * scale;
    float max_x = (tw - avail.x) * 0.5f;
    float max_y = (th - avail.y) * 0.5f;

    if (max_x <= 0.0f) g_pan_x = 0.0f;
    else if (g_pan_x < -max_x) g_pan_x = -max_x;
    else if (g_pan_x > max_x) g_pan_x = max_x;

    if (max_y <= 0.0f) g_pan_y = 0.0f;
    else if (g_pan_y < -max_y) g_pan_y = -max_y;
    else if (g_pan_y > max_y) g_pan_y = max_y;
}

void ZoomClampPanForAvailable(const ImVec2 &avail)
{
    ZoomClampPanForScale(avail, ZoomDisplayScaleForAvailable(avail));
}

void ZoomPanBy(const ImVec2 &avail, float dx, float dy)
{
    g_pan_x += dx;
    g_pan_y += dy;
    ZoomClampPanForAvailable(avail);
}

void ZoomImageRectForAvailable(const ImVec2 &avail, const ImVec2 &origin,
                               ImVec2 *pos, ImVec2 *size, float *scale_out)
{
    float scale = ZoomDisplayScaleForAvailable(avail);
    g_zoom_effective = scale;
    float tw = (float)g_img_tex_w * scale;
    float th = (float)g_img_tex_h * scale;
    if (pos) {
        pos->x = origin.x + (avail.x - tw) * 0.5f + g_pan_x;
        pos->y = origin.y + (avail.y - th) * 0.5f + g_pan_y;
    }
    if (size) *size = ImVec2(tw, th);
    if (scale_out) *scale_out = scale;
}

float ZoomNextLevel(float current, int dir)
{
    static const float levels[] = {
        1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 8.0f, 10.0f,
        12.0f, 16.0f, 24.0f, 32.0f, 48.0f, 64.0f, 96.0f,
        128.0f
    };
    if (dir > 0) {
        if (current >= ZOOM_MAX) return current;
        if (current < 1.0f) current = 1.0f;
        for (float level : levels)
            if (level > current + 0.001f) return level;
        return ZOOM_MAX;
    }
    if (dir < 0) {
        if (current > ZOOM_MAX) return ZOOM_MAX;
        if (current < 1.0f) current = 1.0f;
        for (int i = (int)(sizeof(levels) / sizeof(levels[0])) - 1; i >= 0; i--)
            if (levels[i] < current - 0.001f) return levels[i];
        return 1.0f;
    }
    return current;
}

void ResetZoomToFit(void)
{
    g_zoom = 1.0f;
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;
    g_zoom_fit = true;
    g_zoom_reset = false;
    g_zoom_wheel_accum = 0.0f;
    g_zoom_user_pref = 0.0f;   /* explicit fit drops the sticky zoom */
}

void ResetZoomToHalfFit(const ImVec2 &avail)
{
    float fit = ZoomFitScaleForAvailable(avail);
    float scale = floorf(fit * 0.5f);
    if (scale < 1.0f) scale = 1.0f;
    /* Most sprites are small; half-fit can drop to 100% for tall/medium ones,
       which opens them too small to work on. Prefer a readable initial
       magnification (300%), but never exceed the fit scale so the whole
       sprite stays visible in the canvas. */
    float min_initial = 3.0f;
    if (min_initial > fit) min_initial = fit;
    if (scale < min_initial) scale = min_initial;
    if (scale > ZOOM_MAX) scale = ZOOM_MAX;
    g_zoom = scale;
    g_zoom_effective = scale;
    g_pan_x = 0.0f;
    g_pan_y = 0.0f;
    g_zoom_fit = false;
    g_zoom_reset = false;
    g_zoom_wheel_accum = 0.0f;
}

/* Applied when a selection change (sprite/palette nav, undo, etc.) requests a
   zoom reset. If the user has pinned a zoom level, keep it and just recenter on
   the new content; otherwise fall back to the half-fit default. */
void ResetZoomForSelection(const ImVec2 &avail)
{
    if (g_zoom_user_pref > 0.0f) {
        float scale = g_zoom_user_pref;
        if (scale < 1.0f) scale = 1.0f;
        if (scale > ZOOM_MAX) scale = ZOOM_MAX;
        g_zoom = scale;
        g_zoom_effective = scale;
        g_pan_x = 0.0f;
        g_pan_y = 0.0f;
        g_zoom_fit = false;
        g_zoom_reset = false;
        g_zoom_wheel_accum = 0.0f;
        return;
    }
    ResetZoomToHalfFit(avail);
}

void QueueZoomStep(int dir)
{
    if (dir > 0) g_zoom_pending_steps++;
    else if (dir < 0) g_zoom_pending_steps--;
}

void QueueZoomFit(void)
{
    g_zoom_pending_fit = true;
    g_zoom_pending_steps = 0;
}

bool ApplyZoomScale(float old_scale, float new_scale,
                    const ImVec2 &anchor, const ImVec2 &old_pos,
                    const ImVec2 &old_size, const ImVec2 &avail)
{
    if (old_scale <= 0.0f) old_scale = 1.0f;
    if (new_scale < 1.0f) new_scale = 1.0f;
    if (new_scale > ZOOM_MAX) new_scale = ZOOM_MAX;
    if (fabsf(new_scale - old_scale) < 0.001f) return false;

    float old_cx = old_pos.x + old_size.x * 0.5f;
    float old_cy = old_pos.y + old_size.y * 0.5f;
    float view_cx = old_cx - g_pan_x;
    float view_cy = old_cy - g_pan_y;
    float ratio = new_scale / old_scale;
    g_pan_x = (anchor.x + (old_cx - anchor.x) * ratio) - view_cx;
    g_pan_y = (anchor.y + (old_cy - anchor.y) * ratio) - view_cy;
    g_zoom = new_scale;
    g_zoom_fit = false;
    g_zoom_reset = false;
    g_zoom_user_pref = new_scale;   /* remember the user's chosen zoom */
    ZoomClampPanForScale(avail, new_scale);
    return true;
}

bool CanvasPointInRect(ImVec2 p, ImVec2 mn, ImVec2 mx)
{
    return p.x >= mn.x && p.x < mx.x && p.y >= mn.y && p.y < mx.y;
}

CanvasTransform2D CanvasMakeTransform(ImVec2 img_pos, float sx, float sy,
                                      int rect_x, int rect_y,
                                      int rect_w, int rect_h,
                                      float angle_deg)
{
    const float pi = 3.14159265358979323846f;
    float angle_rad = angle_deg * pi / 180.0f;

    CanvasTransform2D xf;
    xf.img_pos = img_pos;
    xf.sx = sx;
    xf.sy = sy;
    xf.cx_img = (float)rect_x + (float)rect_w * 0.5f;
    xf.cy_img = (float)rect_y + (float)rect_h * 0.5f;
    xf.ca = cosf(angle_rad);
    xf.sa = sinf(angle_rad);
    return xf;
}

ImVec2 CanvasTransformPointScreen(const CanvasTransform2D &xf,
                                  float image_x, float image_y)
{
    float dxp = image_x - xf.cx_img;
    float dyp = image_y - xf.cy_img;
    float rxp = xf.cx_img + dxp * xf.ca - dyp * xf.sa;
    float ryp = xf.cy_img + dxp * xf.sa + dyp * xf.ca;
    return ImVec2(xf.img_pos.x + rxp * xf.sx,
                  xf.img_pos.y + ryp * xf.sy);
}

ImVec2 CanvasTransformPointImage(const CanvasTransform2D &xf,
                                 float image_x, float image_y)
{
    float dxp = image_x - xf.cx_img;
    float dyp = image_y - xf.cy_img;
    return ImVec2(xf.cx_img + dxp * xf.ca - dyp * xf.sa,
                  xf.cy_img + dxp * xf.sa + dyp * xf.ca);
}

void CanvasTransformRectCorners(const CanvasTransform2D &xf,
                                int rect_x, int rect_y,
                                int rect_w, int rect_h,
                                ImVec2 corners[4])
{
    if (!corners)
        return;

    corners[0] = CanvasTransformPointScreen(xf, (float)rect_x,
                                            (float)rect_y);
    corners[1] = CanvasTransformPointScreen(xf, (float)rect_x + rect_w,
                                            (float)rect_y);
    corners[2] = CanvasTransformPointScreen(xf, (float)rect_x + rect_w,
                                            (float)rect_y + rect_h);
    corners[3] = CanvasTransformPointScreen(xf, (float)rect_x,
                                            (float)rect_y + rect_h);
}

void CanvasQuadBounds(const ImVec2 corners[4], ImVec2 *out_min,
                      ImVec2 *out_max)
{
    if (!corners || !out_min || !out_max)
        return;

    ImVec2 mn = corners[0];
    ImVec2 mx = corners[0];
    for (int i = 1; i < 4; i++) {
        if (corners[i].x < mn.x) mn.x = corners[i].x;
        if (corners[i].y < mn.y) mn.y = corners[i].y;
        if (corners[i].x > mx.x) mx.x = corners[i].x;
        if (corners[i].y > mx.y) mx.y = corners[i].y;
    }
    *out_min = mn;
    *out_max = mx;
}

void DrawCanvasPasteBorder(ImDrawList *dl, const ImVec2 corners[4],
                           bool transform_active, bool hovering)
{
    if (!dl || !corners)
        return;

    ImU32 border_col = transform_active ? IM_COL32(0, 220, 255, 255)
                         : (hovering ? IM_COL32(255, 200, 0, 255)
                                     : IM_COL32(255, 255, 0, 255));
    dl->AddPolyline(corners, 4, border_col, ImDrawFlags_Closed, 2.0f);
}

void DrawCanvasPasteSnapGuides(ImDrawList *dl, ImVec2 img_pos,
                               float sx, float sy,
                               int tex_w, int tex_h,
                               bool dragging,
                               bool hit_x, int guide_x,
                               bool hit_y, int guide_y)
{
    if (!dl || !dragging)
        return;

    if (hit_x) {
        float gx = img_pos.x + guide_x * sx;
        dl->AddLine(ImVec2(gx, img_pos.y),
                    ImVec2(gx, img_pos.y + tex_h * sy),
                    IM_COL32(255, 0, 255, 220), 1.5f);
    }
    if (hit_y) {
        float gy = img_pos.y + guide_y * sy;
        dl->AddLine(ImVec2(img_pos.x, gy),
                    ImVec2(img_pos.x + tex_w * sx, gy),
                    IM_COL32(255, 0, 255, 220), 1.5f);
    }
}

void DrawCanvasPasteHint(ImDrawList *dl, ImVec2 img_pos,
                         const char *hint, ImU32 color)
{
    if (!dl || !hint || !hint[0])
        return;

    dl->AddText(ImVec2(img_pos.x + 6.0f, img_pos.y + 6.0f),
                color, hint);
}

CanvasPasteHint CanvasPasteHintForState(bool transform_active,
                                        TransformHandle handle,
                                        bool paste_dragging)
{
    CanvasPasteHint hint;
    if (transform_active) {
        hint.text = (handle != TransformHandle::None)
            ? (handle == TransformHandle::Rotate ? "Rotating..."
               : (handle == TransformHandle::Move ? "Moving..." : "Scaling..."))
            : "Drag inside to move | handles scale | top dot rotates | Enter commits";
        hint.color = IM_COL32(0, 220, 255, 255);
    } else if (paste_dragging) {
        hint.text = "Moving...";
        hint.color = IM_COL32(255, 200, 0, 255);
    } else {
        hint.text = "Drag to move | H/V flip | L to layer | Ctrl+T transform | Click outside to place | Esc cancel";
        hint.color = IM_COL32(255, 255, 0, 255);
    }
    return hint;
}

CanvasPasteControlsLayout CanvasPasteControlsLayoutFor(ImVec2 canvas_origin,
                                                       ImVec2 mouse)
{
    CanvasPasteControlsLayout layout;
    layout.min = ImVec2(canvas_origin.x + 10.0f,
                        canvas_origin.y + 10.0f);
    layout.max = ImVec2(layout.min.x + 276.0f,
                        layout.min.y + 89.0f);
    layout.blend_label_pos = ImVec2(layout.min.x + 8.0f,
                                    layout.min.y + 7.0f);
    layout.blend_control_pos = ImVec2(layout.min.x + 76.0f,
                                      layout.min.y + 5.0f);
    layout.opacity_label_pos = ImVec2(layout.min.x + 8.0f,
                                      layout.min.y + 34.0f);
    layout.opacity_control_pos = ImVec2(layout.min.x + 76.0f,
                                        layout.min.y + 32.0f);
    layout.smooth_control_pos = ImVec2(layout.min.x + 8.0f,
                                       layout.min.y + 61.0f);
    layout.item_width = layout.max.x - layout.min.x - 86.0f;
    layout.blocks_mouse = CanvasPointInRect(mouse, layout.min, layout.max);
    return layout;
}

CanvasPasteHitTest CanvasPasteHitTestFor(const ImVec2 corners[4],
                                         ImVec2 img_pos,
                                         ImVec2 img_sz,
                                         ImVec2 mouse)
{
    CanvasPasteHitTest hit;
    CanvasQuadBounds(corners, &hit.bounds_min, &hit.bounds_max);
    hit.hovering = CanvasPointInRect(mouse, hit.bounds_min, hit.bounds_max);
    hit.over_sprite = CanvasPointInRect(
        mouse, img_pos, ImVec2(img_pos.x + img_sz.x, img_pos.y + img_sz.y));
    return hit;
}

CanvasPastePreviewCell CanvasPastePreviewCellForPixel(
    const CanvasTransform2D &xf,
    int paste_x, int paste_y,
    int paste_w, int paste_h,
    int clip_w, int clip_h,
    int pixel_x, int pixel_y)
{
    CanvasPastePreviewCell cell;
    float ix0 = (float)paste_x + ((float)pixel_x * (float)paste_w /
                                  (float)clip_w);
    float iy0 = (float)paste_y + ((float)pixel_y * (float)paste_h /
                                  (float)clip_h);
    float ix1 = (float)paste_x + ((float)(pixel_x + 1) * (float)paste_w /
                                  (float)clip_w);
    float iy1 = (float)paste_y + ((float)(pixel_y + 1) * (float)paste_h /
                                  (float)clip_h);
    ImVec2 mid = CanvasTransformPointImage(
        xf, (ix0 + ix1) * 0.5f, (iy0 + iy1) * 0.5f);
    cell.target_x = (int)floorf(mid.x);
    cell.target_y = (int)floorf(mid.y);
    cell.quad[0] = CanvasTransformPointScreen(xf, ix0, iy0);
    cell.quad[1] = CanvasTransformPointScreen(xf, ix1, iy0);
    cell.quad[2] = CanvasTransformPointScreen(xf, ix1, iy1);
    cell.quad[3] = CanvasTransformPointScreen(xf, ix0, iy1);
    return cell;
}

CanvasPasteGeometry CanvasPasteGeometryForState(
    ImVec2 img_pos,
    float sx, float sy,
    ImVec2 img_sz,
    ImVec2 mouse,
    bool transform_active,
    int transform_x, int transform_y,
    int transform_w, int transform_h,
    float transform_angle_deg,
    int paste_x, int paste_y,
    int paste_w, int paste_h)
{
    CanvasPasteGeometry geom;
    if (transform_active) {
        geom.x = transform_x;
        geom.y = transform_y;
        geom.w = transform_w;
        geom.h = transform_h;
        geom.angle_deg = transform_angle_deg;
    } else {
        geom.x = paste_x;
        geom.y = paste_y;
        geom.w = paste_w;
        geom.h = paste_h;
        geom.angle_deg = 0.0f;
    }

    geom.transform = CanvasMakeTransform(img_pos, sx, sy,
                                         geom.x, geom.y,
                                         geom.w, geom.h,
                                         geom.angle_deg);
    CanvasTransformRectCorners(geom.transform,
                               geom.x, geom.y, geom.w, geom.h,
                               geom.corners);
    geom.hit = CanvasPasteHitTestFor(geom.corners, img_pos, img_sz, mouse);
    return geom;
}

CanvasTransformHandleOverlay DrawCanvasTransformHandles(
    ImDrawList *dl, const ImVec2 corners[4], ImVec2 mouse,
    TransformHandle active_handle, bool aspect_locked)
{
    CanvasTransformHandleOverlay out;
    if (!dl || !corners)
        return out;

    const float hsz = 5.0f;
    struct HandleSpec { TransformHandle h; float cx, cy; };
    HandleSpec specs[8] = {
        { TransformHandle::TL, corners[0].x, corners[0].y },
        { TransformHandle::T,  (corners[0].x + corners[1].x) * 0.5f,
                               (corners[0].y + corners[1].y) * 0.5f },
        { TransformHandle::TR, corners[1].x, corners[1].y },
        { TransformHandle::L,  (corners[0].x + corners[3].x) * 0.5f,
                               (corners[0].y + corners[3].y) * 0.5f },
        { TransformHandle::R,  (corners[1].x + corners[2].x) * 0.5f,
                               (corners[1].y + corners[2].y) * 0.5f },
        { TransformHandle::BL, corners[3].x, corners[3].y },
        { TransformHandle::B,  (corners[3].x + corners[2].x) * 0.5f,
                               (corners[3].y + corners[2].y) * 0.5f },
        { TransformHandle::BR, corners[2].x, corners[2].y },
    };

    for (int i = 0; i < 8; i++) {
        const HandleSpec &s = specs[i];
        bool hov = mouse.x >= s.cx - hsz && mouse.x <= s.cx + hsz &&
                   mouse.y >= s.cy - hsz && mouse.y <= s.cy + hsz;
        if (hov && active_handle == TransformHandle::None)
            out.hover = s.h;
        ImU32 fill = (hov || active_handle == s.h)
            ? IM_COL32(255, 255, 255, 255)
            : IM_COL32(0, 220, 255, 255);
        dl->AddRectFilled(ImVec2(s.cx - hsz, s.cy - hsz),
                          ImVec2(s.cx + hsz, s.cy + hsz),
                          fill);
        dl->AddRect(ImVec2(s.cx - hsz, s.cy - hsz),
                    ImVec2(s.cx + hsz, s.cy + hsz),
                    IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
    }

    out.center.x = (corners[0].x + corners[2].x) * 0.5f;
    out.center.y = (corners[0].y + corners[2].y) * 0.5f;
    float top_mid_x = (corners[0].x + corners[1].x) * 0.5f;
    float top_mid_y = (corners[0].y + corners[1].y) * 0.5f;
    float vx = top_mid_x - out.center.x;
    float vy = top_mid_y - out.center.y;
    float vlen = sqrtf(vx * vx + vy * vy);
    if (vlen < 0.001f) { vx = 0.0f; vy = -1.0f; vlen = 1.0f; }
    vx /= vlen;
    vy /= vlen;
    ImVec2 rot_handle(top_mid_x + vx * 26.0f, top_mid_y + vy * 26.0f);
    dl->AddLine(ImVec2(top_mid_x, top_mid_y), rot_handle,
                IM_COL32(0, 220, 255, 190), 1.5f);
    float rdist = (mouse.x - rot_handle.x) * (mouse.x - rot_handle.x) +
                  (mouse.y - rot_handle.y) * (mouse.y - rot_handle.y);
    out.rotate_hover = rdist <= 9.0f * 9.0f;
    if (out.rotate_hover && active_handle == TransformHandle::None)
        out.hover = TransformHandle::Rotate;
    dl->AddCircleFilled(rot_handle, 7.0f,
                        (out.rotate_hover || active_handle == TransformHandle::Rotate)
                            ? IM_COL32(255, 255, 255, 255)
                            : IM_COL32(0, 220, 255, 255));
    dl->AddCircle(rot_handle, 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.0f);

    float chain_cx = corners[1].x + 14.0f;
    float chain_cy = corners[1].y - 14.0f;
    float chain_hs = 8.0f;
    out.chain_min = ImVec2(chain_cx - chain_hs, chain_cy - chain_hs);
    out.chain_max = ImVec2(chain_cx + chain_hs, chain_cy + chain_hs);
    out.chain_hover = mouse.x >= out.chain_min.x && mouse.x <= out.chain_max.x &&
                      mouse.y >= out.chain_min.y && mouse.y <= out.chain_max.y;
    ImU32 chain_bg = out.chain_hover ? IM_COL32(255, 255, 255, 200)
                                     : IM_COL32(40, 40, 40, 200);
    ImU32 chain_fg = aspect_locked ? IM_COL32(0, 220, 255, 255)
                                   : IM_COL32(180, 180, 180, 255);
    dl->AddRectFilled(out.chain_min, out.chain_max, chain_bg, 2.0f);
    dl->AddRect(out.chain_min, out.chain_max, IM_COL32(0, 0, 0, 255),
                2.0f, 0, 1.0f);
    if (aspect_locked) {
        dl->AddCircle(ImVec2(chain_cx - 3.0f, chain_cy), 3.5f,
                      chain_fg, 0, 1.5f);
        dl->AddCircle(ImVec2(chain_cx + 3.0f, chain_cy), 3.5f,
                      chain_fg, 0, 1.5f);
    } else {
        dl->AddCircle(ImVec2(chain_cx - 4.0f, chain_cy - 2.0f), 3.0f,
                      chain_fg, 0, 1.5f);
        dl->AddCircle(ImVec2(chain_cx + 4.0f, chain_cy + 2.0f), 3.0f,
                      chain_fg, 0, 1.5f);
    }

    return out;
}

void CanvasResizeTransformRect(TransformHandle handle,
                               int drag_x, int drag_y,
                               int drag_w, int drag_h,
                               float ref_aspect,
                               int dx, int dy,
                               bool lock_aspect,
                               int *out_x, int *out_y,
                               int *out_w, int *out_h)
{
    if (!out_x || !out_y || !out_w || !out_h)
        return;

    int rx = drag_x;
    int ry = drag_y;
    int rw = drag_w;
    int rh = drag_h;

    bool affects_left = (handle == TransformHandle::TL ||
                         handle == TransformHandle::L ||
                         handle == TransformHandle::BL);
    bool affects_right = (handle == TransformHandle::TR ||
                          handle == TransformHandle::R ||
                          handle == TransformHandle::BR);
    bool affects_top = (handle == TransformHandle::TL ||
                        handle == TransformHandle::T ||
                        handle == TransformHandle::TR);
    bool affects_bottom = (handle == TransformHandle::BL ||
                           handle == TransformHandle::B ||
                           handle == TransformHandle::BR);

    if (affects_left)   { rx += dx; rw -= dx; }
    if (affects_right)  {           rw += dx; }
    if (affects_top)    { ry += dy; rh -= dy; }
    if (affects_bottom) {           rh += dy; }

    bool is_corner = (handle == TransformHandle::TL ||
                      handle == TransformHandle::TR ||
                      handle == TransformHandle::BL ||
                      handle == TransformHandle::BR);
    if (lock_aspect && ref_aspect > 0.0f) {
        if (is_corner) {
            float scale_w = (float)rw / (float)drag_w;
            float scale_h = (float)rh / (float)drag_h;
            float scale = (fabsf(scale_w - 1.0f) > fabsf(scale_h - 1.0f))
                ? scale_w : scale_h;
            int new_w = (int)(drag_w * scale + 0.5f);
            int new_h = (int)(new_w / ref_aspect + 0.5f);
            if (new_w < 1) new_w = 1;
            if (new_h < 1) new_h = 1;
            if (affects_left) rx = (drag_x + drag_w) - new_w;
            if (affects_top)  ry = (drag_y + drag_h) - new_h;
            rw = new_w;
            rh = new_h;
        } else {
            if (handle == TransformHandle::T || handle == TransformHandle::B) {
                int new_w = (int)(rh * ref_aspect + 0.5f);
                if (new_w < 1) new_w = 1;
                int cx_old = drag_x + drag_w / 2;
                rx = cx_old - new_w / 2;
                rw = new_w;
            } else {
                int new_h = (int)(rw / ref_aspect + 0.5f);
                if (new_h < 1) new_h = 1;
                int cy_old = drag_y + drag_h / 2;
                ry = cy_old - new_h / 2;
                rh = new_h;
            }
        }
    }

    if (rw < 1) rw = 1;
    if (rh < 1) rh = 1;
    *out_x = rx;
    *out_y = ry;
    *out_w = rw;
    *out_h = rh;
}

float CanvasRotateTransformAngle(float start_angle_deg,
                                 ImVec2 drag_mouse,
                                 ImVec2 mouse,
                                 ImVec2 center,
                                 bool snap_15_deg)
{
    const float pi = 3.14159265358979323846f;
    float a0 = atan2f(drag_mouse.y - center.y,
                      drag_mouse.x - center.x);
    float a1 = atan2f(mouse.y - center.y,
                      mouse.x - center.x);
    float new_angle = start_angle_deg + (a1 - a0) * 180.0f / pi;
    while (new_angle <= -180.0f) new_angle += 360.0f;
    while (new_angle >   180.0f) new_angle -= 360.0f;
    if (snap_15_deg)
        new_angle = roundf(new_angle / 15.0f) * 15.0f;
    return new_angle;
}

CanvasTransformDragResult CanvasResolveTransformDrag(
    const CanvasTransformDragStart &start,
    ImVec2 mouse,
    float sx, float sy,
    ImVec2 center,
    bool aspect_locked,
    bool shift_down)
{
    CanvasTransformDragResult result;
    result.x = start.x;
    result.y = start.y;
    result.w = start.w;
    result.h = start.h;
    result.angle_deg = start.angle_deg;

    if (start.handle == TransformHandle::None)
        return result;

    if (start.handle == TransformHandle::Move) {
        int dx = 0;
        int dy = 0;
        CanvasDragDeltaPixels(start.mouse, mouse, sx, sy, &dx, &dy);
        result.x = start.x + dx;
        result.y = start.y + dy;
        return result;
    }

    if (start.handle == TransformHandle::Rotate) {
        result.angle_deg = CanvasRotateTransformAngle(
            start.angle_deg, start.mouse, mouse, center, shift_down);
        return result;
    }

    int dx = 0;
    int dy = 0;
    CanvasDragDeltaPixels(start.mouse, mouse, sx, sy, &dx, &dy);
    bool lock_now = aspect_locked ^ shift_down;
    CanvasResizeTransformRect(start.handle,
                              start.x, start.y, start.w, start.h,
                              start.ref_aspect,
                              dx, dy, lock_now,
                              &result.x, &result.y,
                              &result.w, &result.h);
    return result;
}

void CanvasDragDeltaPixels(ImVec2 drag_start_mouse, ImVec2 mouse,
                           float sx, float sy,
                           int *dx, int *dy)
{
    if (dx)
        *dx = (int)((mouse.x - drag_start_mouse.x) / sx);
    if (dy)
        *dy = (int)((mouse.y - drag_start_mouse.y) / sy);
}

CanvasContentBounds CanvasFindOpaqueBounds(const IMG *img)
{
    CanvasContentBounds bounds;
    if (!img || !img->data_p)
        return bounds;

    int min_x = img->w;
    int min_y = img->h;
    int max_x = 0;
    int max_y = 0;
    unsigned short stride = (img->w + 3) & ~3;
    const unsigned char *dp = (const unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            if (dp[y * stride + x] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                bounds.valid = true;
            }
        }
    }

    if (bounds.valid) {
        bounds.min_x = min_x;
        bounds.min_y = min_y;
        bounds.max_x = max_x;
        bounds.max_y = max_y;
    }
    return bounds;
}

static int CanvasAbsInt(int v)
{
    return v < 0 ? -v : v;
}

CanvasPasteSnapResult CanvasSnapPasteToContent(int x, int y, int w, int h,
                                               int target_w, int target_h,
                                               const CanvasContentBounds &bounds,
                                               float view_sx, float view_sy)
{
    CanvasPasteSnapResult result;
    result.x = x;
    result.y = y;
    if (!bounds.valid)
        return result;

    int tx = (int)(6.0f / view_sx);
    int ty = (int)(6.0f / view_sy);
    if (tx < 1) tx = 1;
    if (ty < 1) ty = 1;

    int sx_min = bounds.min_x;
    int sy_min = bounds.min_y;
    int sx_max = bounds.max_x + 1;
    int sy_max = bounds.max_y + 1;

    if (CanvasAbsInt(result.x - sx_min) < tx) {
        result.x = sx_min;
        result.hit_x = true;
        result.guide_x = sx_min;
    } else if (CanvasAbsInt((result.x + w) - sx_max) < tx) {
        result.x = sx_max - w;
        result.hit_x = true;
        result.guide_x = sx_max;
    } else if (CanvasAbsInt(result.x - sx_max) < tx) {
        result.x = sx_max;
        result.hit_x = true;
        result.guide_x = sx_max;
    } else if (CanvasAbsInt((result.x + w) - sx_min) < tx) {
        result.x = sx_min - w;
        result.hit_x = true;
        result.guide_x = sx_min;
    }

    if (CanvasAbsInt(result.y - sy_min) < ty) {
        result.y = sy_min;
        result.hit_y = true;
        result.guide_y = sy_min;
    } else if (CanvasAbsInt((result.y + h) - sy_max) < ty) {
        result.y = sy_max - h;
        result.hit_y = true;
        result.guide_y = sy_max;
    } else if (CanvasAbsInt(result.y - sy_max) < ty) {
        result.y = sy_max;
        result.hit_y = true;
        result.guide_y = sy_max;
    } else if (CanvasAbsInt((result.y + h) - sy_min) < ty) {
        result.y = sy_min - h;
        result.hit_y = true;
        result.guide_y = sy_min;
    }

    if (target_w > 0 && !result.hit_x) {
        int sprite_cx = target_w / 2;
        int paste_cx = result.x + w / 2;
        if (CanvasAbsInt(paste_cx - sprite_cx) < tx) {
            result.x = sprite_cx - w / 2;
            result.hit_x = true;
            result.guide_x = sprite_cx;
        }
    }
    if (target_h > 0 && !result.hit_y) {
        int sprite_cy = target_h / 2;
        int paste_cy = result.y + h / 2;
        if (CanvasAbsInt(paste_cy - sprite_cy) < ty) {
            result.y = sprite_cy - h / 2;
            result.hit_y = true;
            result.guide_y = sprite_cy;
        }
    }

    return result;
}

CanvasPasteSnapResult CanvasPasteCenterGuide(int x, int y, int w, int h,
                                             int target_w, int target_h,
                                             bool hit_x, bool hit_y,
                                             int guide_x, int guide_y)
{
    CanvasPasteSnapResult result;
    result.x = x;
    result.y = y;
    result.hit_x = hit_x;
    result.hit_y = hit_y;
    result.guide_x = guide_x;
    result.guide_y = guide_y;

    if (target_w > 0 && !result.hit_x) {
        int sprite_cx = target_w / 2;
        if (result.x + w / 2 == sprite_cx) {
            result.hit_x = true;
            result.guide_x = sprite_cx;
        }
    }
    if (target_h > 0 && !result.hit_y) {
        int sprite_cy = target_h / 2;
        if (result.y + h / 2 == sprite_cy) {
            result.hit_y = true;
            result.guide_y = sprite_cy;
        }
    }

    return result;
}

void CanvasClampPasteRect(int canvas_w, int canvas_h,
                          int rect_w, int rect_h,
                          int *x, int *y)
{
    if (!x || !y)
        return;

    if (*x < 0) *x = 0;
    if (*y < 0) *y = 0;
    if (*x + rect_w > canvas_w) *x = canvas_w - rect_w;
    if (*y + rect_h > canvas_h) *y = canvas_h - rect_h;
}

CanvasPasteDragResult CanvasResolvePasteDrag(ImVec2 drag_start_mouse,
                                             ImVec2 mouse,
                                             float sx, float sy,
                                             int start_x, int start_y,
                                             int rect_w, int rect_h,
                                             int canvas_w, int canvas_h,
                                             int target_w, int target_h,
                                             bool snap_to_content,
                                             const CanvasContentBounds &bounds,
                                             bool show_center_guides)
{
    int dx = 0;
    int dy = 0;
    CanvasDragDeltaPixels(drag_start_mouse, mouse, sx, sy, &dx, &dy);

    CanvasPasteDragResult result;
    result.x = start_x + dx;
    result.y = start_y + dy;

    if (snap_to_content && bounds.valid) {
        CanvasPasteSnapResult snap =
            CanvasSnapPasteToContent(result.x, result.y, rect_w, rect_h,
                                     target_w, target_h, bounds, sx, sy);
        result.x = snap.x;
        result.y = snap.y;
        result.hit_x = snap.hit_x;
        result.hit_y = snap.hit_y;
        result.guide_x = snap.guide_x;
        result.guide_y = snap.guide_y;
    }

    if (show_center_guides) {
        CanvasPasteSnapResult guide =
            CanvasPasteCenterGuide(result.x, result.y, rect_w, rect_h,
                                   target_w, target_h,
                                   result.hit_x, result.hit_y,
                                   result.guide_x, result.guide_y);
        result.x = guide.x;
        result.y = guide.y;
        result.hit_x = guide.hit_x;
        result.hit_y = guide.hit_y;
        result.guide_x = guide.guide_x;
        result.guide_y = guide.guide_y;
    }

    CanvasClampPasteRect(canvas_w, canvas_h, rect_w, rect_h,
                         &result.x, &result.y);
    return result;
}

void CanvasRotateButtonRects(ImVec2 img_pos, ImVec2 img_sz,
                             ImVec2 canvas_pos, ImVec2 canvas_sz,
                             ImVec2 mins[2], ImVec2 maxs[2])
{
    const float size = 24.0f;
    const float margin = 6.0f;
    float left = canvas_pos.x;
    float top = canvas_pos.y;
    float right = canvas_pos.x + canvas_sz.x;
    float bottom = canvas_pos.y + canvas_sz.y;

    float x0 = img_pos.x + img_sz.x + margin;
    float y0 = img_pos.y;
    if (x0 + size > right) x0 = right - size - margin;
    if (x0 < left) x0 = left;
    if (y0 < top) y0 = top;
    if (y0 + size > bottom) y0 = bottom - size;
    if (y0 < top) y0 = top;

    mins[0] = ImVec2(x0, y0);
    maxs[0] = ImVec2(x0 + size, y0 + size);
    mins[1] = maxs[1] = ImVec2(0, 0);
}

static void DrawRotateArrow(ImDrawList *dl, ImVec2 center, ImU32 col)
{
    const float r = 6.8f;
    float a0 = -2.35f;
    float a1 =  3.55f;
    dl->PathArcTo(center, r, a0, a1, 24);
    dl->PathStroke(col, false, 1.8f);

    float tip_a = a1;
    ImVec2 tip(center.x + cosf(tip_a) * r, center.y + sinf(tip_a) * r);
    ImVec2 dir(-sinf(tip_a), cosf(tip_a));
    ImVec2 n(-dir.y, dir.x);
    ImVec2 p1(tip.x - dir.x * 5.0f + n.x * 3.0f,
              tip.y - dir.y * 5.0f + n.y * 3.0f);
    ImVec2 p2(tip.x - dir.x * 5.0f - n.x * 3.0f,
              tip.y - dir.y * 5.0f - n.y * 3.0f);
    dl->AddTriangleFilled(tip, p1, p2, col);
}

void DrawCanvasRotateButtons(ImDrawList *dl, const ImVec2 mins[2],
                             const ImVec2 maxs[2], int hover_idx)
{
    for (int i = 0; i < 1; i++) {
        bool hover = (i == hover_idx);
        ImU32 bg = hover ? IM_COL32(45, 45, 45, 230) : IM_COL32(12, 12, 12, 175);
        ImU32 border = hover ? IM_COL32(255, 220, 90, 255) : IM_COL32(235, 235, 235, 180);
        ImU32 icon = hover ? IM_COL32(255, 235, 130, 255) : IM_COL32(245, 245, 245, 230);
        dl->AddRectFilled(mins[i], maxs[i], bg, 4.0f);
        dl->AddRect(mins[i], maxs[i], border, 4.0f, 0, hover ? 1.5f : 1.0f);
        ImVec2 c((mins[i].x + maxs[i].x) * 0.5f, (mins[i].y + maxs[i].y) * 0.5f);
        DrawRotateArrow(dl, c, icon);
    }
}

void DrawCanvasCheckerboard(ImDrawList *dl, ImVec2 img_pos, ImVec2 img_sz,
                            float scale)
{
    if (!dl || img_sz.x <= 0.0f || img_sz.y <= 0.0f) return;

    float cs = 8.0f * scale;
    if (cs < 8.0f) cs = 8.0f;
    for (float cy = img_pos.y; cy < img_pos.y + img_sz.y; cy += cs) {
        for (float cx = img_pos.x; cx < img_pos.x + img_sz.x; cx += cs) {
            int row = (int)((cy - img_pos.y) / cs);
            int col = (int)((cx - img_pos.x) / cs);
            ImU32 col32 = ((row + col) & 1) ? IM_COL32(160, 160, 160, 255)
                                            : IM_COL32(100, 100, 100, 255);
            float x2 = cx + cs;
            if (x2 > img_pos.x + img_sz.x) x2 = img_pos.x + img_sz.x;
            float y2 = cy + cs;
            if (y2 > img_pos.y + img_sz.y) y2 = img_pos.y + img_sz.y;
            dl->AddRectFilled(ImVec2(cx, cy), ImVec2(x2, y2), col32);
        }
    }
}

void DrawCanvasPixelGrid(ImDrawList *dl, ImVec2 img_pos, ImVec2 img_sz,
                         int tex_w, int tex_h, float scale)
{
    if (!dl || scale < 4.0f || tex_w <= 0 || tex_h <= 0)
        return;

    float sx = img_sz.x / (float)tex_w;
    float sy = img_sz.y / (float)tex_h;
    ImU32 gc = IM_COL32(60, 60, 60, 100);
    for (int x = 0; x <= tex_w; x++)
        dl->AddLine(ImVec2(img_pos.x + x * sx, img_pos.y),
                    ImVec2(img_pos.x + x * sx, img_pos.y + img_sz.y),
                    gc, 0.5f);
    for (int y = 0; y <= tex_h; y++)
        dl->AddLine(ImVec2(img_pos.x, img_pos.y + y * sy),
                    ImVec2(img_pos.x + img_sz.x, img_pos.y + y * sy),
                    gc, 0.5f);
}

/* The zoom readout now rides the right end of the document tab strip — it is
   drawn inline beside the filename in DrawMainLayout (ui_main.cpp). Drawn here
   at the canvas window's top-left corner it landed underneath the "Image" view
   tab, where a dim grey percentage was unreadable against the tab it sat on. */

void DrawCanvasDmaCompressionOverlay(ImDrawList *dl, IMG *img,
                                     ImVec2 img_pos, float sx, float sy)
{
    if (!dl || !img || !img->data_p)
        return;

    unsigned short stride = (img->w + 3) & ~3;
    unsigned char *pixels = (unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        int leading = 0;
        while (leading < img->w && pixels[y * stride + leading] == 0)
            leading++;

        if (leading == img->w) {
            ImVec2 p_min(img_pos.x, img_pos.y + y * sy);
            ImVec2 p_max(img_pos.x + img->w * sx, img_pos.y + (y + 1) * sy);
            dl->AddRectFilled(p_min, p_max, IM_COL32(255, 0, 255, 100));
        } else {
            if (leading > 0) {
                ImVec2 p_min(img_pos.x, img_pos.y + y * sy);
                ImVec2 p_max(img_pos.x + leading * sx, img_pos.y + (y + 1) * sy);
                dl->AddRectFilled(p_min, p_max, IM_COL32(255, 0, 255, 100));
            }

            int trailing = 0;
            while (trailing < img->w &&
                   pixels[y * stride + (img->w - 1 - trailing)] == 0)
                trailing++;
            if (trailing > 0) {
                ImVec2 p_min(img_pos.x + (img->w - trailing) * sx,
                             img_pos.y + y * sy);
                ImVec2 p_max(img_pos.x + img->w * sx,
                             img_pos.y + (y + 1) * sy);
                dl->AddRectFilled(p_min, p_max, IM_COL32(0, 255, 255, 100));
            }
        }
    }
}

void DrawCanvasColorIsolationOverlay(ImDrawList *dl, IMG *img,
                                     const bool kept[256],
                                     ImVec2 img_pos, float sx, float sy)
{
    if (!dl || !img || !img->data_p || !kept)
        return;

    int iw = img->w;
    int ih = img->h;
    int stride = (iw + 3) & ~3;
    unsigned char *idp = (unsigned char *)img->data_p;
    ImU32 dim = IM_COL32(40, 30, 80, 180);
    for (int y = 0; y < ih; y++) {
        int x = 0;
        while (x < iw) {
            if (kept[idp[y * stride + x]]) {
                x++;
                continue;
            }
            int x0 = x;
            while (x < iw && !kept[idp[y * stride + x]])
                x++;
            ImVec2 a(img_pos.x + x0 * sx, img_pos.y + y * sy);
            ImVec2 b(img_pos.x + x * sx,  img_pos.y + (y + 1) * sy);
            dl->AddRectFilled(a, b, dim);
        }
    }
}

void DrawCanvasPixelHoverHighlight(ImDrawList *dl, ImVec2 mouse,
                                   ImVec2 img_pos, ImVec2 img_sz,
                                   float sx, float sy,
                                   bool suppress)
{
    if (!dl || suppress || img_sz.x <= 0.0f || img_sz.y <= 0.0f)
        return;
    if (((sx + sy) * 0.5f) < 4.0f)
        return;
    if (mouse.x < img_pos.x || mouse.x >= img_pos.x + img_sz.x ||
        mouse.y < img_pos.y || mouse.y >= img_pos.y + img_sz.y)
        return;

    int hx = (int)((mouse.x - img_pos.x) / sx);
    int hy = (int)((mouse.y - img_pos.y) / sy);
    dl->AddRect(ImVec2(img_pos.x + hx * sx, img_pos.y + hy * sy),
                ImVec2(img_pos.x + (hx + 1) * sx,
                       img_pos.y + (hy + 1) * sy),
                IM_COL32(255, 255, 0, 180), 0.0f, 0, 1.5f);
}

void DrawCanvasPencilCursor(ImDrawList *dl, ImVec2 img_pos,
                            float sx, float sy,
                            int pixel_x, int pixel_y,
                            int brush, ImU32 color)
{
    if (!dl)
        return;

    ImVec2 cc(img_pos.x + (pixel_x + 0.5f) * sx,
              img_pos.y + (pixel_y + 0.5f) * sy);
    if (brush > 1) {
        float rr = (sx + sy) * 0.5f * (brush - 1);
        dl->AddCircle(cc, rr, IM_COL32(0, 0, 0, 200), 0, 3.0f);
        dl->AddCircle(cc, rr, color, 0, 1.5f);
        return;
    }

    float pix = (sx + sy) * 0.5f;
    float gap = pix * 0.5f;
    if (gap > 4.0f) gap = 4.0f;
    if (gap < 1.0f) gap = 1.0f;
    float len = 8.0f;
    ImU32 halo = IM_COL32(0, 0, 0, 200);
    auto arm = [&](ImVec2 a, ImVec2 b) {
        dl->AddLine(a, b, halo, 3.0f);
        dl->AddLine(a, b, color, 1.5f);
    };
    arm(ImVec2(cc.x - gap - len, cc.y), ImVec2(cc.x - gap, cc.y));
    arm(ImVec2(cc.x + gap, cc.y), ImVec2(cc.x + gap + len, cc.y));
    arm(ImVec2(cc.x, cc.y - gap - len), ImVec2(cc.x, cc.y - gap));
    arm(ImVec2(cc.x, cc.y + gap), ImVec2(cc.x, cc.y + gap + len));
}

void DrawCanvasCloneStampAids(ImDrawList *dl, ImVec2 img_pos,
                              float sx, float sy,
                              int source_x, int source_y,
                              bool show_dest_brush,
                              int dest_x, int dest_y,
                              int brush)
{
    if (!dl)
        return;

    ImVec2 sc(img_pos.x + (source_x + 0.5f) * sx,
              img_pos.y + (source_y + 0.5f) * sy);
    ImU32 src_col = IM_COL32(0, 255, 255, 230);
    dl->AddLine(ImVec2(sc.x - 8, sc.y), ImVec2(sc.x + 8, sc.y),
                src_col, 1.5f);
    dl->AddLine(ImVec2(sc.x, sc.y - 8), ImVec2(sc.x, sc.y + 8),
                src_col, 1.5f);
    dl->AddCircle(sc, 4.0f, src_col, 0, 1.0f);

    if (show_dest_brush && brush > 1) {
        int r = brush - 1;
        ImVec2 cc(img_pos.x + (dest_x + 0.5f) * sx,
                  img_pos.y + (dest_y + 0.5f) * sy);
        float rr = (sx + sy) * 0.5f * r;
        dl->AddCircle(cc, rr, IM_COL32(255, 255, 255, 200), 0, 1.0f);
    }
}

void DrawCanvasLassoPath(ImDrawList *dl, ImVec2 img_pos,
                         float sx, float sy,
                         const std::vector<std::pair<int, int>> &points)
{
    if (!dl || points.size() < 2)
        return;

    std::vector<ImVec2> screen_pts;
    screen_pts.reserve(points.size() + 1);
    for (const auto &p : points) {
        screen_pts.push_back(ImVec2(img_pos.x + (p.first + 0.5f) * sx,
                                    img_pos.y + (p.second + 0.5f) * sy));
    }
    dl->AddPolyline(screen_pts.data(), (int)screen_pts.size(),
                    IM_COL32(255, 0, 255, 220), 0, 1.5f);
    if (screen_pts.size() >= 2) {
        dl->AddLine(screen_pts.back(), screen_pts.front(),
                    IM_COL32(255, 0, 255, 110), 1.0f);
    }
}

void DrawCanvasSelectionOverlay(ImDrawList *dl, ImVec2 img_pos,
                                float sx, float sy,
                                int x1, int y1, int x2, int y2,
                                bool is_mask, int mask_w,
                                const std::vector<bool> *pixel_mask)
{
    if (!dl)
        return;

    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    if (is_mask) {
        if (!pixel_mask || mask_w <= 0)
            return;
        for (int y = y1; y <= y2; y++) {
            for (int x = x1; x <= x2; x++) {
                if ((*pixel_mask)[y * mask_w + x]) {
                    ImVec2 r1(img_pos.x + x * sx, img_pos.y + y * sy);
                    ImVec2 r2(img_pos.x + (x + 1) * sx,
                              img_pos.y + (y + 1) * sy);
                    dl->AddRectFilled(r1, r2,
                                      IM_COL32(255, 0, 255, 80), 0.0f);
                }
            }
        }
        ImVec2 br1(img_pos.x + x1 * sx, img_pos.y + y1 * sy);
        ImVec2 br2(img_pos.x + (x2 + 1) * sx,
                   img_pos.y + (y2 + 1) * sy);
        dl->AddRect(br1, br2, IM_COL32(255, 0, 255, 255),
                    0.0f, 0, 1.0f);
    } else {
        ImVec2 r1(img_pos.x + x1 * sx, img_pos.y + y1 * sy);
        ImVec2 r2(img_pos.x + (x2 + 1) * sx,
                  img_pos.y + (y2 + 1) * sy);
        dl->AddRect(r1, r2, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
        dl->AddRectFilled(r1, r2, IM_COL32(0, 255, 0, 30), 0.0f);
    }
}

void DrawCanvasAnipointCrosshair(ImDrawList *dl, ImVec2 p, ImU32 col,
                                 float len, float thick)
{
    if (!dl) return;

    dl->AddLine(ImVec2(p.x - len, p.y), ImVec2(p.x + len, p.y), col, thick);
    dl->AddLine(ImVec2(p.x, p.y - len), ImVec2(p.x, p.y + len), col, thick);
    dl->AddCircleFilled(p, 1.5f, col);
}

bool CanvasAnipointHitTest(const IMG *img, ImVec2 img_pos,
                           float sx, float sy, ImVec2 mouse,
                           bool *primary_hover, bool *secondary_hover)
{
    bool h1 = false;
    bool h2 = false;
    if (img) {
        ImVec2 a1(img_pos.x + (short)img->anix * sx,
                  img_pos.y + (short)img->aniy * sy);
        float dx1 = mouse.x - a1.x;
        float dy1 = mouse.y - a1.y;
        h1 = (dx1 * dx1 + dy1 * dy1) < 10.0f * 10.0f;

        if (secondary_anipoint_in_use(img)) {
            ImVec2 a2(img_pos.x + (short)img->anix2 * sx,
                      img_pos.y + (short)img->aniy2 * sy);
            float dx2 = mouse.x - a2.x;
            float dy2 = mouse.y - a2.y;
            h2 = (dx2 * dx2 + dy2 * dy2) < 10.0f * 10.0f;
        }
    }
    if (primary_hover) *primary_hover = h1;
    if (secondary_hover) *secondary_hover = h2;
    return h1 || h2;
}

void DrawCanvasAnipointOverlay(ImDrawList *dl, const IMG *img,
                               const IMG *prev_img,
                               ImVec2 img_pos, float sx, float sy,
                               ImVec2 mouse,
                               bool *primary_hover,
                               bool *secondary_hover)
{
    bool h1 = false;
    bool h2 = false;
    CanvasAnipointHitTest(img, img_pos, sx, sy, mouse, &h1, &h2);
    if (primary_hover) *primary_hover = h1;
    if (secondary_hover) *secondary_hover = h2;
    if (!dl || !img)
        return;

    if (prev_img) {
        ImVec2 sp(img_pos.x + (short)prev_img->anix * sx,
                  img_pos.y + (short)prev_img->aniy * sy);
        DrawCanvasAnipointCrosshair(dl, sp,
                                    IM_COL32(160, 160, 160, 180),
                                    12.0f, 1.0f);
        if (secondary_anipoint_in_use(prev_img)) {
            ImVec2 sp2(img_pos.x + (short)prev_img->anix2 * sx,
                       img_pos.y + (short)prev_img->aniy2 * sy);
            DrawCanvasAnipointCrosshair(dl, sp2,
                                        IM_COL32(160, 160, 160, 140),
                                        9.0f, 1.0f);
        }
    }

    ImVec2 s1(img_pos.x + (short)img->anix * sx,
              img_pos.y + (short)img->aniy * sy);
    ImU32 col1 = h1 ? IM_COL32(255, 220, 60, 255)
                    : IM_COL32(255, 255, 255, 255);
    DrawCanvasAnipointCrosshair(dl, s1, col1, 14.0f, h1 ? 2.0f : 1.5f);

    if (secondary_anipoint_in_use(img)) {
        ImVec2 s2(img_pos.x + (short)img->anix2 * sx,
                  img_pos.y + (short)img->aniy2 * sy);
        ImU32 col2 = h2 ? IM_COL32(120, 255, 255, 255)
                        : IM_COL32(60, 200, 220, 255);
        DrawCanvasAnipointCrosshair(dl, s2, col2, 10.0f, h2 ? 2.0f : 1.5f);
        dl->AddLine(s1, s2, IM_COL32(255, 255, 0, 140), 1.0f);
    }
}

void WorldDrawReferenceFigure(ImDrawList *dl, const WorldCanvasLayout &layout,
                              const WorldViewState &state)
{
    if (!dl || !state.show_reference) return;
    int rw = state.ref_w < 4 ? 4 : state.ref_w;
    int rh = state.ref_h < 8 ? 8 : state.ref_h;
    float s = layout.scale;

    /* Feet centre sits at the anchor plus the user's offset; the body grows
       upward from there. */
    float feet_x = layout.origin_x + state.ref_dx * s;
    float feet_y = layout.origin_y + state.ref_dy * s;
    float top_y  = feet_y - rh * s;
    float left_x = feet_x - (rw * 0.5f) * s;

    /* Normalized-to-screen, honoring the mirror toggle so both facings are
       checkable without moving anything. */
    auto P = [&](float nx, float ny) {
        float fx = state.ref_mirror ? (1.0f - nx) : nx;
        return ImVec2(left_x + fx * rw * s, top_y + ny * rh * s);
    };

    const ImU32 fill = IM_COL32(120, 180, 255, 46);
    const ImU32 line = IM_COL32(140, 200, 255, 170);

    /* Head */
    ImVec2 head = P(0.50f, 0.10f);
    float head_r = 0.085f * rh * s;
    dl->AddCircleFilled(head, head_r, fill, 20);
    dl->AddCircle(head, head_r, line, 20, 1.0f);

    /* Torso */
    dl->AddRectFilled(P(0.34f, 0.19f), P(0.66f, 0.56f), fill);
    dl->AddRect(P(0.34f, 0.19f), P(0.66f, 0.56f), line, 0.0f, 0, 1.0f);

    /* Arms + legs as thick strokes; a fighting-stance outline reads better
       than a plain box when an effect overlaps it. */
    float limb = 0.055f * rh * s;
    if (limb < 1.5f) limb = 1.5f;
    dl->AddLine(P(0.36f, 0.22f), P(0.10f, 0.48f), line, limb);
    dl->AddLine(P(0.64f, 0.22f), P(0.90f, 0.48f), line, limb);
    dl->AddLine(P(0.42f, 0.56f), P(0.30f, 1.00f), line, limb);
    dl->AddLine(P(0.58f, 0.56f), P(0.72f, 1.00f), line, limb);

    /* Footprint + centre line: the two things you actually measure against. */
    dl->AddLine(ImVec2(left_x, feet_y), ImVec2(left_x + rw * s, feet_y),
                IM_COL32(140, 200, 255, 200), 1.0f);
    dl->AddRect(ImVec2(left_x, top_y), ImVec2(left_x + rw * s, feet_y),
                IM_COL32(140, 200, 255, 70), 0.0f, 0, 1.0f);
}

/* Park a stage module in the playfield: its horizontal centre at the
   playfield's centre, its bottom edge on the floor line. A module is one
   parallax plane, so this is the closest thing to "where the game would put
   it" that the BDD/BDB can honestly support. */
void WorldBgSnapToModule(WorldViewState &state, const BddBackground &bg,
                         int module_idx)
{
    if (module_idx < 0 || module_idx >= (int)bg.modules.size()) return;
    const BddBgModule &mod = bg.modules[(size_t)module_idx];
    /* Align the painted edges, not the declared rect: a module's rect is a
       packing region and its art rarely fills it, so using the rect leaves a
       dead band between the stage and the floor line. */
    int x1 = mod.has_content ? mod.cx1 : mod.x1;
    int x2 = mod.has_content ? mod.cx2 : mod.x2;
    int y2 = mod.has_content ? mod.cy2 : mod.y2;
    state.bg_x = (x1 + x2) / 2 - state.w / 2;
    state.bg_y = y2 - state.floor_y;
    state.bg_module = module_idx;
}

void WorldDrawReferenceBackground(ImDrawList *dl, const WorldCanvasLayout &layout,
                                  const WorldViewState &state)
{
    if (!dl || !state.bg_enabled) return;
    BddBackground &bg = WorldBackground();
    if (!bg.loaded) return;
    SDL_Texture *tex = BddBgTexture(&bg);
    if (!tex) return;

    /* bg_x/bg_y are the world coordinates shown at the canvas's top-left, so
       the stage slides under a fixed playfield rather than the reverse. The
       composite was cropped to its art, hence the content_x/content_y term. */
    float s = layout.scale;
    ImVec2 p0(layout.pos.x + (float)(bg.content_x - state.bg_x) * s,
              layout.pos.y + (float)(bg.content_y - state.bg_y) * s);
    ImVec2 p1(p0.x + (float)bg.w * s, p0.y + (float)bg.h * s);

    ImVec2 clip_min = layout.pos;
    ImVec2 clip_max(layout.pos.x + layout.width, layout.pos.y + layout.height);
    if (p1.x <= clip_min.x || p0.x >= clip_max.x ||
        p1.y <= clip_min.y || p0.y >= clip_max.y)
        return;   /* panned entirely off the canvas */

    int alpha = state.bg_alpha;
    if (alpha < 0) alpha = 0;
    if (alpha > 255) alpha = 255;

    dl->PushClipRect(clip_min, clip_max, true);
    dl->AddImage((ImTextureID)(intptr_t)tex, p0, p1, ImVec2(0, 0), ImVec2(1, 1),
                 IM_COL32(255, 255, 255, alpha));
    dl->PopClipRect();
}

int CanvasFlipPreviewOffsetPx(const IMG *img)
{
    if (!img || img->w == 0) return 0;
    int ax = (int)(short)img->anix;
    int eff = anipoint_effective(ax, (int)img->w, true, g_mirror_convention);
    /* Both placements put the anchor at the same screen X, so the mirrored
       sprite's left edge shifts by (anix - anix_eff). */
    return ax - eff;
}

void DrawCanvasFlipPreview(ImDrawList *dl, const IMG *img,
                           SDL_Texture *img_texture,
                           ImVec2 img_pos, ImVec2 img_sz,
                           float sx, float sy, bool ghost)
{
    if (!dl || !img || img->w == 0 || !img_texture) return;
    (void)sy;

    int off_px = CanvasFlipPreviewOffsetPx(img);
    ImVec2 fpos(img_pos.x + off_px * sx, img_pos.y);
    ImVec2 fmax(fpos.x + img_sz.x, fpos.y + img_sz.y);

    /* Mirrored UVs draw the art itself flipped; the offset above places it. */
    ImU32 tint = ghost ? IM_COL32(255, 190, 120, 110)
                       : IM_COL32(255, 255, 255, 255);
    dl->AddImage((ImTextureID)(intptr_t)img_texture, fpos, fmax,
                 ImVec2(1, 0), ImVec2(0, 1), tint);
    dl->AddRect(fpos, fmax, IM_COL32(255, 170, 60, 200), 0.0f, 0, 1.0f);

    /* The anipoint is the one point both placements share — draw it as the
       fixed pivot so the two rects visibly hinge about it. */
    float anchor_x = img_pos.x + (int)(short)img->anix * sx;
    dl->AddLine(ImVec2(anchor_x, ImMin(img_pos.y, fpos.y) - 6.0f),
                ImVec2(anchor_x, ImMax(img_pos.y + img_sz.y, fmax.y) + 6.0f),
                IM_COL32(255, 170, 60, 150), 1.0f);

    char tag[64];
    snprintf(tag, sizeof(tag), "flip %+d px", off_px);
    ImVec2 tag_sz = ImGui::CalcTextSize(tag);
    ImVec2 tag_pos(fpos.x, fpos.y - tag_sz.y - 2.0f);
    dl->AddRectFilled(ImVec2(tag_pos.x - 2.0f, tag_pos.y - 1.0f),
                      ImVec2(tag_pos.x + tag_sz.x + 2.0f, tag_pos.y + tag_sz.y + 1.0f),
                      IM_COL32(0, 0, 0, 180));
    dl->AddText(tag_pos, IM_COL32(255, 200, 120, 255), tag);
}

void DrawCanvasHitboxOverlay(ImDrawList *dl, ImVec2 img_pos,
                             float sx, float sy,
                             int x, int y, int w, int h,
                             ImVec2 mouse, bool hovering[4])
{
    if (!dl) {
        if (hovering) {
            for (int c = 0; c < 4; c++) hovering[c] = false;
        }
        return;
    }

    ImVec2 tl(img_pos.x + x * sx, img_pos.y + y * sy);
    ImVec2 br(img_pos.x + (x + w) * sx, img_pos.y + (y + h) * sy);
    ImVec2 tr(br.x, tl.y);
    ImVec2 bl(tl.x, br.y);
    dl->AddRect(tl, br, IM_COL32(0, 255, 255, 255), 0, 0, 2.0f);

    ImVec2 corners[4] = {tl, tr, br, bl};
    float hr = 12.0f * 12.0f;
    for (int c = 0; c < 4; c++) {
        ImVec2 d(mouse.x - corners[c].x, mouse.y - corners[c].y);
        bool hover = (d.x * d.x + d.y * d.y < hr);
        if (hovering) hovering[c] = hover;
        ImU32 col = hover ? IM_COL32(255, 255, 0, 255)
                          : IM_COL32(0, 255, 255, 255);
        dl->AddCircleFilled(corners[c], 5.0f, col);
    }
}

void DrawCanvasStrikeBoxOverlay(ImDrawList *dl, ImVec2 img_pos,
                                float sx, float sy,
                                int x, int y, int w, int h,
                                const char *label,
                                ImVec2 mouse, bool enable_hover,
                                bool hovering[4])
{
    if (!dl) {
        if (hovering) {
            for (int c = 0; c < 4; c++) hovering[c] = false;
        }
        return;
    }

    ImVec2 tl(img_pos.x + x * sx, img_pos.y + y * sy);
    ImVec2 br(img_pos.x + (x + w) * sx, img_pos.y + (y + h) * sy);
    ImVec2 tr(br.x, tl.y);
    ImVec2 bl(tl.x, br.y);
    ImU32 col_line = IM_COL32(255, 80, 220, 230);
    ImU32 col_fill = IM_COL32(255, 80, 220, 40);
    ImU32 col_hover = IM_COL32(255, 255, 0, 255);

    dl->AddRectFilled(tl, br, col_fill);
    dl->AddRect(tl, br, col_line, 0, 0, 2.0f);
    if (label && label[0])
        dl->AddText(ImVec2(tl.x, tl.y - 16.0f), col_line, label);

    ImVec2 corners[4] = {tl, tr, br, bl};
    float hr = 12.0f * 12.0f;
    for (int c = 0; c < 4; c++) {
        bool hover = false;
        if (enable_hover) {
            ImVec2 d(mouse.x - corners[c].x, mouse.y - corners[c].y);
            hover = (d.x * d.x + d.y * d.y < hr);
        }
        if (hovering) hovering[c] = hover;
        dl->AddCircleFilled(corners[c], 5.0f, hover ? col_hover : col_line);
    }
}

void CanvasResizeRectFromCorner(int corner, int mouse_x, int mouse_y,
                                int *x, int *y, int *w, int *h)
{
    if (!x || !y || !w || !h)
        return;

    int nx = *x;
    int ny = *y;
    int nw = *w;
    int nh = *h;
    if (corner == 0) {
        nw += nx - mouse_x;
        nh += ny - mouse_y;
        nx = mouse_x;
        ny = mouse_y;
    } else if (corner == 1) {
        nw = mouse_x - nx;
        nh += ny - mouse_y;
        ny = mouse_y;
    } else if (corner == 2) {
        nw = mouse_x - nx;
        nh = mouse_y - ny;
    } else if (corner == 3) {
        nw += nx - mouse_x;
        nx = mouse_x;
        nh = mouse_y - ny;
    }
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;

    *x = nx;
    *y = ny;
    *w = nw;
    *h = nh;
}

WorldCanvasLayout ComputeWorldCanvasLayout(ImVec2 avail, ImVec2 img_pos,
                                           int world_w, int world_h,
                                           int world_origin_x,
                                           int world_origin_y)
{
    WorldCanvasLayout layout;
    int guide_w = world_w;
    int guide_h = world_h;
    if (guide_w < 512) guide_w = 512;
    if (guide_h < 254) guide_h = 254;

    float fit_x = avail.x / (float)guide_w;
    float fit_y = avail.y / (float)guide_h;
    layout.scale = (fit_x < fit_y) ? fit_x : fit_y;
    if (layout.scale < 2.0f) layout.scale = 2.0f;
    layout.scale = (float)(int)layout.scale;
    if (layout.scale < 2.0f) layout.scale = 2.0f;

    layout.width = (float)guide_w * layout.scale;
    layout.height = (float)guide_h * layout.scale;
    layout.pos = ImVec2(img_pos.x + (avail.x - layout.width) * 0.5f,
                        img_pos.y + (avail.y - layout.height) * 0.5f);
    layout.origin_x = layout.pos.x + world_origin_x * layout.scale;
    layout.origin_y = layout.pos.y + world_origin_y * layout.scale;
    return layout;
}

/* One frame thumbnail, and the breathing room the strip needs around them.
   The ctrl+click highlight is drawn outside each thumbnail, so the strip has
   to carry padding or the highlight is clipped against the child's own edge.
   Anything reserving vertical space for a row reads these rather than a
   number that was measured once. */
static const float kWorldLaneThumbPx = 34.0f;
static const float kWorldLaneStripPad = 4.0f;

WorldMarkedPanelLayout ComputeWorldMarkedPanelLayout(ImVec2 avail,
                                                     ImVec2 img_pos,
                                                     int lane_count,
                                                     int hidden_lane_count)
{
    WorldMarkedPanelLayout layout;
    layout.width = avail.x - 16.0f;
    if (layout.width < 240.0f) layout.width = 240.0f;
    if (hidden_lane_count < 0) hidden_lane_count = 0;
    if (hidden_lane_count > lane_count) hidden_lane_count = lane_count;
    int drawn = lane_count - hidden_lane_count;
    /* A hidden row is its own header line and nothing else, so it should not
       reserve what a row with controls and a thumbnail strip needs.

       104 was that figure before the strip gained padding for the ctrl+click
       highlight; it grows by the same amount rather than being re-measured,
       so the two stay in step. */
    const float row_h = 104.0f + kWorldLaneStripPad * 2.0f;
    /* 54 covered a one-line header. The strip wraps to a second row now --
       timing on the first, the tools and menus on the second -- so the
       allowance follows the font rather than staying a number that was right
       for the old layout. */
    const float header_h = 54.0f + ImGui::GetFrameHeightWithSpacing();
    layout.height = header_h + (float)drawn * row_h +
                    (float)hidden_lane_count * 26.0f;
    float max_h = avail.y - 24.0f;
    if (max_h > 380.0f) max_h = 380.0f;
    if (layout.height > max_h) layout.height = max_h;
    float min_h = 96.0f + ImGui::GetFrameHeightWithSpacing();
    if (layout.height < min_h) layout.height = min_h;
    layout.pos = ImVec2(img_pos.x + 8.0f,
                        img_pos.y + avail.y - layout.height - 8.0f);
    if (layout.pos.y < img_pos.y + 8.0f)
        layout.pos.y = img_pos.y + 8.0f;
    return layout;
}

WorldMarkedSequenceState &WorldMarkedState(void)
{
    static WorldMarkedSequenceState state;
    return state;
}

/* ---- Row -> document binding ----
   Every one of these resolves from the stored uid on the spot. Nothing here
   may cache a Document* or an index into the state: that caching is exactly
   what let a closed tab leave a dangling pointer behind. */

int WorldMarkedRowDocIndex(const WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return -1;
    return document_index_of_uid(state.sequence_doc_uid[slot]);
}

Document *WorldMarkedRowDoc(const WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return NULL;
    return document_from_uid(state.sequence_doc_uid[slot]);
}

void WorldMarkedSetRowDoc(WorldMarkedSequenceState &state, int slot, int doc_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.sequence_doc_uid[slot] = document_uid(doc_idx);  /* 0 when out of range */
}

void WorldMarkedClearRowDoc(WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.sequence_doc_uid[slot] = 0;
}

bool *WorldMarkedMirrorFlag(WorldMarkedSequenceState &state, int slot)
{
    if (slot == 0) return &state.mirror_active;
    if (slot == 1) return &state.mirror_other;
    if (slot >= 2 && slot < kWorldMarkedMaxTabs) return &state.mirror_extra[slot - 2];
    return nullptr;
}

static const int kWorldDummyDecapOrder[] = {
    1, 2, 3,
    4, 3, 4, 3, 4, 3,
    4, 5, 6, 7
};

static const int kWorldDummyDecapDefaultDelays[] = {
    48, 6, 6,
    10, 10, 10, 10, 10, 10,
    6, 6, 6, 6
};

static bool WorldReadDecapFrameNo(const std::string &upper, size_t pos,
                                  int *frame_no, size_t *end_pos)
{
    if (pos >= upper.size() || !std::isdigit((unsigned char)upper[pos])) return false;

    int val = 0;
    size_t p = pos;
    while (p < upper.size() && std::isdigit((unsigned char)upper[p])) {
        val = val * 10 + (upper[p] - '0');
        p++;
    }
    if (val < 1 || val > 7) return false;
    if (frame_no) *frame_no = val;
    if (end_pos) *end_pos = p;
    return true;
}

static std::string WorldUpperName(const std::string &name)
{
    std::string upper;
    upper.reserve(name.size());
    for (char c : name)
        upper.push_back((char)std::toupper((unsigned char)c));
    return upper;
}

bool WorldDecapBodyFrameNo(const std::string &name, int *frame_no, std::string *prefix)
{
    std::string upper = WorldUpperName(name);
    if (upper.find("DECAPHEAD") != std::string::npos ||
        upper.find("DECAPLEG") != std::string::npos ||
        upper.find("DECAPTORSO") != std::string::npos)
        return false;

    size_t pos = upper.rfind("DECAP");
    if (pos == std::string::npos) return false;
    size_t p = pos + 5;
    int val = 0;
    if (!WorldReadDecapFrameNo(upper, p, &val, &p)) return false;
    if (p != upper.size()) return false;

    if (frame_no) *frame_no = val;
    if (prefix) *prefix = upper.substr(0, pos);
    return true;
}

bool WorldDecapBodyPieceInfo(const std::string &name, int *frame_no,
                             std::string *prefix, int *kind)
{
    std::string upper = WorldUpperName(name);
    size_t pos = upper.rfind("DECAPLEG");
    int piece_kind = 0; /* leg before torso */
    size_t token_len = 8;
    if (pos == std::string::npos) {
        pos = upper.rfind("DECAPTORSO");
        piece_kind = 1;
        token_len = 10;
    }
    if (pos == std::string::npos) return false;

    size_t p = pos + token_len;
    int val = 0;
    if (!WorldReadDecapFrameNo(upper, p, &val, &p)) return false;

    if (frame_no) *frame_no = val;
    if (prefix) *prefix = upper.substr(0, pos);
    if (kind) *kind = piece_kind;
    return true;
}

bool WorldDecapPrefixFromName(const std::string &name, std::string *prefix)
{
    int frame_no = 0;
    if (WorldDecapBodyFrameNo(name, &frame_no, prefix)) return true;
    int kind = 0;
    if (WorldDecapBodyPieceInfo(name, &frame_no, prefix, &kind)) return true;
    return false;
}

int WorldDummyDecapFrameCount(void)
{
    return (int)(sizeof(kWorldDummyDecapOrder) / sizeof(kWorldDummyDecapOrder[0]));
}

int WorldDummyDecapFrameNo(int index)
{
    int n = WorldDummyDecapFrameCount();
    if (index < 0 || index >= n) return 1;
    return kWorldDummyDecapOrder[index];
}

void WorldResetDummyDecapDelays(WorldMarkedSequenceState &state, int frame_count)
{
    if (frame_count < 0) frame_count = 0;
    std::vector<int> &delays = state.frame_delays[kWorldDummyDecapSlot];
    delays.assign((size_t)frame_count, 1);
    state.local_dx[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.local_dy[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.visible_from[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.visible_until[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.motion_dx[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.motion_dy[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.motion_cap_x[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    state.motion_cap_y[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
    int n = (int)(sizeof(kWorldDummyDecapDefaultDelays) /
                  sizeof(kWorldDummyDecapDefaultDelays[0]));
    if (frame_count < n) n = frame_count;
    for (int i = 0; i < n; i++)
        delays[i] = ClampTimelineHold(kWorldDummyDecapDefaultDelays[i]);
    state.dummy_decap_reset = false;
}

bool WorldAssignSelectedDummyDecap(WorldMarkedSequenceState &state,
                                   IMG *selected_img,
                                   int active_doc_idx)
{
    std::string prefix;
    if (!selected_img || !WorldDecapPrefixFromName(img_name_string(selected_img), &prefix))
        return false;

    state.dummy_decap_body = true;
    state.dummy_decap_manual = true;
    state.dummy_decap_doc_idx = active_doc_idx;
    state.dummy_decap_prefix = prefix;
    state.dummy_decap_reset = true;
    state.hold_end[kWorldDummyDecapSlot] = true;

    /* Default the dummy to face the player. Anipoints still pin to the shared
       origin, so this only flips facing relative to lane 0. */
    bool *player_mirror = WorldMarkedMirrorFlag(state, 0);
    bool *dummy_mirror = WorldMarkedMirrorFlag(state, kWorldDummyDecapSlot);
    if (dummy_mirror) *dummy_mirror = player_mirror ? !*player_mirror : true;

    WorldMarkedRestart(state);
    return true;
}

/* Index of the drawable sprite named `name` in `doc`, or -1. */
static int WorldFindFrameByName(Document *doc, const std::string &name)
{
    if (!doc || name.empty()) return -1;
    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        if (ascii_iequals(trim_sprite_name(img_name_string(img)), name))
            return idx;
    }
    return -1;
}

/* World View shows parent frames, never subframes.

   A chopped piece is authored through its parent: on the Image tab you pick
   the parent and reach its pieces from there. The parent's own bitmap already
   holds the whole drawing, and the two draw identically -- a child's anchor is
   parent - piece_offset and its art sits at that same offset inside the parent
   (the relation img_io.cpp's Bulk Restore depends on) -- so a marked piece is
   previewed here as its parent. That also keeps World View's editing gestures
   pointed at the frame that owns the placement, so a move propagates to the
   pieces instead of splitting the composite.

   A piece whose parent is not in this file has nothing to stand in for it and
   is left alone, so an orphaned chop stays previewable. */
static int WorldResolveMarkedFrame(Document *doc, int idx)
{
    IMG *img = doc_get_img(doc, idx);
    if (!img) return idx;
    std::string parent = InferSubframeParentName(img_name_string(img).c_str());
    if (parent.empty()) return idx;
    int parent_idx = WorldFindFrameByName(doc, parent);
    return parent_idx >= 0 ? parent_idx : idx;
}

void WorldCollectMarkedFrames(Document *doc, std::vector<int> &out)
{
    out.clear();
    if (!doc) return;
    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0))
            continue;
        int frame = WorldResolveMarkedFrame(doc, idx);
        /* Marking a parent and two of its pieces still has to yield one frame,
           and so does marking three pieces of the same parent. */
        bool dup = false;
        for (size_t i = 0; i < out.size(); i++)
            if (out[i] == frame) { dup = true; break; }
        if (!dup) out.push_back(frame);
    }
}

/* Marked sprites, grouped so that pieces of one chopped frame become one lane
   entry instead of N.

   A name like BGBIGFIST1A/1B/1C/1D describes four slices of a single drawing;
   marking them all and getting four separate animation frames is never what
   was meant. Each piece carries its own anipoint, so drawing them together at
   the same tick reassembles the original picture — which is exactly what a
   multi-piece lane entry already does.

   `out_frames` holds the first piece of each group (the entry's nominal
   sprite); `out_pieces` holds every piece in it. A sprite with no siblings
   yields a one-piece group, i.e. the previous behaviour. */
void WorldCollectMarkedFrameGroups(Document *doc, std::vector<int> &out_frames,
                                   std::vector<std::vector<int>> &out_pieces)
{
    out_frames.clear();
    out_pieces.clear();
    if (!doc) return;

    std::vector<int> marked;
    WorldCollectMarkedFrames(doc, marked);
    if (marked.empty()) return;

    /* Group key: the inferred parent name, or the sprite's own name when it is
       not a piece of anything. Groups keep the order of their first member so
       the lane still plays in image-list order. */
    std::vector<std::string> keys;
    for (int idx : marked) {
        IMG *img = doc_get_img(doc, idx);
        std::string nm = img ? img_name_string(img) : std::string();
        std::string parent = InferSubframeParentName(nm.c_str());
        keys.push_back(parent.empty() ? nm : parent);
    }

    for (size_t i = 0; i < marked.size(); i++) {
        if (keys[i].empty()) {           /* unnamed: never group */
            out_frames.push_back(marked[i]);
            out_pieces.push_back(std::vector<int>(1, marked[i]));
            continue;
        }
        bool merged = false;
        for (size_t g = 0; g < out_frames.size(); g++) {
            IMG *head = doc_get_img(doc, out_frames[g]);
            std::string head_nm = head ? img_name_string(head) : std::string();
            std::string head_parent = InferSubframeParentName(head_nm.c_str());
            std::string head_key = head_parent.empty() ? head_nm : head_parent;
            if (head_key == keys[i]) {
                out_pieces[g].push_back(marked[i]);
                merged = true;
                break;
            }
        }
        if (!merged) {
            out_frames.push_back(marked[i]);
            out_pieces.push_back(std::vector<int>(1, marked[i]));
        }
    }
}

struct WorldDecapCandidate {
    Document *doc;
    int doc_idx;
    std::string prefix;
    int frame_idx[8];
    std::vector<int> pieces[8];
    bool has_leg[8];
    bool has_torso[8];
    int wrapper_count;
    int piece_frame_count;
};

static bool WorldDecapCandidateComplete(const WorldDecapCandidate &cand)
{
    if (cand.wrapper_count >= 7) return true;
    for (int frame_no = 1; frame_no <= 7; frame_no++) {
        if (!cand.has_leg[frame_no] || !cand.has_torso[frame_no])
            return false;
    }
    return true;
}

static int WorldDecapCandidateScore(const WorldDecapCandidate &cand)
{
    int piece_frames = 0;
    for (int frame_no = 1; frame_no <= 7; frame_no++)
        if (cand.has_leg[frame_no] && cand.has_torso[frame_no])
            piece_frames++;
    return cand.wrapper_count * 10 + piece_frames;
}

WorldMarkedLane WorldBuildDummyDecapLane(WorldMarkedSequenceState &state,
                                         int active_doc_idx)
{
    std::vector<WorldDecapCandidate> candidates;
    for (int doc_idx = 0; doc_idx < document_tab_count(); doc_idx++) {
        Document *doc = document_get(doc_idx);
        if (!doc) continue;
        for (unsigned int img_idx = 0; img_idx < doc->imgcnt; img_idx++) {
            IMG *img = doc_get_img(doc, (int)img_idx);
            int frame_no = 0;
            std::string prefix;
            int piece_kind = -1;
            bool is_wrapper = WorldDecapBodyFrameNo(img_name_string(img),
                                                    &frame_no, &prefix);
            bool is_piece = !is_wrapper &&
                WorldDecapBodyPieceInfo(img_name_string(img),
                                        &frame_no, &prefix, &piece_kind);
            if (!is_wrapper && !is_piece)
                continue;

            int cand_idx = -1;
            for (int i = 0; i < (int)candidates.size(); i++) {
                if (candidates[i].doc_idx == doc_idx &&
                    candidates[i].prefix == prefix) {
                    cand_idx = i;
                    break;
                }
            }
            if (cand_idx < 0) {
                WorldDecapCandidate cand = {};
                cand.doc = doc;
                cand.doc_idx = doc_idx;
                cand.prefix = prefix;
                for (int i = 0; i < 8; i++) cand.frame_idx[i] = -1;
                for (int i = 0; i < 8; i++) {
                    cand.has_leg[i] = false;
                    cand.has_torso[i] = false;
                }
                cand.wrapper_count = 0;
                cand.piece_frame_count = 0;
                candidates.push_back(cand);
                cand_idx = (int)candidates.size() - 1;
            }
            WorldDecapCandidate &cand = candidates[cand_idx];
            if (is_wrapper && cand.frame_idx[frame_no] < 0) {
                candidates[cand_idx].frame_idx[frame_no] = (int)img_idx;
                candidates[cand_idx].wrapper_count++;
            } else if (is_piece) {
                bool had_frame = !cand.pieces[frame_no].empty();
                cand.pieces[frame_no].push_back((int)img_idx);
                if (piece_kind == 0) cand.has_leg[frame_no] = true;
                else cand.has_torso[frame_no] = true;
                if (!had_frame) cand.piece_frame_count++;
            }
        }
    }

    int best = -1;
    for (int i = 0; i < (int)candidates.size(); i++) {
        if (!WorldDecapCandidateComplete(candidates[i])) continue;
        if (state.dummy_decap_manual) {
            if (candidates[i].doc_idx != state.dummy_decap_doc_idx ||
                candidates[i].prefix != state.dummy_decap_prefix)
                continue;
            best = i;
            break;
        }
        if (best < 0)
            best = i;
        else if (candidates[i].doc_idx == active_doc_idx &&
                 candidates[best].doc_idx != active_doc_idx)
            best = i;
        else if (candidates[i].doc_idx == candidates[best].doc_idx &&
                 WorldDecapCandidateScore(candidates[i]) >
                 WorldDecapCandidateScore(candidates[best]))
            best = i;
    }

    WorldMarkedLane lane = {};
    lane.delay_slot = kWorldDummyDecapSlot;
    lane.frame_pos = 0;
    lane.img = NULL;
    lane.dummy_decap = true;
    lane.label = "Dummy Decap Body";
    lane.asm_label_part = "dummy_decap_body";
    if (best < 0) {
        lane.doc = NULL;
        lane.doc_idx = -1;
        return lane;
    }

    WorldDecapCandidate &cand = candidates[best];
    lane.doc = cand.doc;
    lane.doc_idx = cand.doc_idx;
    lane.asm_label_part = "dummy_decap_body_" +
                          WorldMarkedAsmLabelPart(cand.prefix.c_str(),
                                                  kWorldDummyDecapSlot);
    char label_buf[96];
    snprintf(label_buf, sizeof(label_buf), "Dummy Decap Body [%s]",
             cand.prefix.c_str());
    lane.label = label_buf;

    int n = WorldDummyDecapFrameCount();
    lane.frames.reserve((size_t)n);
    lane.frame_pieces.reserve((size_t)n);
    lane.frame_labels.reserve((size_t)n);
    for (int i = 0; i < n; i++) {
        int frame_no = WorldDummyDecapFrameNo(i);
        char frame_label[96];
        snprintf(frame_label, sizeof(frame_label), "%sDECAP%d",
                 cand.prefix.c_str(), frame_no);
        lane.frame_labels.push_back(frame_label);

        std::vector<int> pieces;
        if (cand.frame_idx[frame_no] >= 0) {
            pieces.push_back(cand.frame_idx[frame_no]);
        } else {
            pieces = cand.pieces[frame_no];
            std::sort(pieces.begin(), pieces.end(), [&](int a, int b) {
                IMG *ia = doc_get_img(cand.doc, a);
                IMG *ib = doc_get_img(cand.doc, b);
                int fa = 0, fb = 0, ka = 0, kb = 0;
                std::string pa, pb;
                WorldDecapBodyPieceInfo(img_name_string(ia), &fa, &pa, &ka);
                WorldDecapBodyPieceInfo(img_name_string(ib), &fb, &pb, &kb);
                if (ka != kb) return ka < kb;
                return img_name_string(ia) < img_name_string(ib);
            });
        }
        int representative = pieces.empty() ? -1 : pieces[0];
        lane.frames.push_back(representative);
        lane.frame_pieces.push_back(pieces);
    }

    if (state.dummy_decap_reset ||
        state.dummy_decap_doc_idx != cand.doc_idx ||
        state.dummy_decap_prefix != cand.prefix ||
        (int)state.frame_delays[kWorldDummyDecapSlot].size() !=
        (int)lane.frames.size()) {
        state.dummy_decap_doc_idx = cand.doc_idx;
        state.dummy_decap_prefix = cand.prefix;
        WorldResetDummyDecapDelays(state, (int)lane.frames.size());
    }
    EnsureWorldMarkedFrameDelays(state, kWorldDummyDecapSlot,
                                 (int)lane.frames.size());
    return lane;
}

static bool WorldMarkedSlotReservedForSplit(const WorldMarkedSequenceState &state,
                                            int slot)
{
    for (const WorldMarkedSplitLane &split : state.split_lanes)
        if (split.slot == slot)
            return true;
    return false;
}

static int WorldMarkedFindBaseSourceSlot(WorldMarkedSequenceState &state,
                                         Document *doc, int doc_idx,
                                         const bool used_source_slots[kWorldMarkedSourceTabs])
{
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (used_source_slots[slot] || WorldMarkedSlotReservedForSplit(state, slot))
            continue;
        if (WorldMarkedRowDoc(state, slot) == doc &&
            WorldMarkedRowDocIndex(state, slot) == doc_idx)
            return slot;
    }
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (used_source_slots[slot] || WorldMarkedSlotReservedForSplit(state, slot))
            continue;
        /* A slot the user drained to zero frames (dragged its last frame
           elsewhere) is still "claimed" by its own doc and must stay put for
           it — checking default_frames rather than sequence_frames tells a
           genuinely fresh/never-built slot (both empty) apart from one that
           was built and then deliberately emptied (default_frames remains
           the last marked-set snapshot). */
        if (WorldMarkedRowDocIndex(state, slot) < 0 || state.default_frames[slot].empty())
            return slot;
    }
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (!used_source_slots[slot] && !WorldMarkedSlotReservedForSplit(state, slot))
            return slot;
    }
    return -1;
}

/* Whether a lane is divisible at all, ignoring which frame is selected.
   Kept in one place so the toolbar button and the Row... menu item cannot
   drift into disagreeing about when a split is legal — the split itself
   re-checks, so this only governs whether the control is greyed out. */
static bool WorldMarkedLaneCanSplit(const WorldMarkedLane &lane,
                                    const std::vector<WorldMarkedLane> &lanes);

static int WorldMarkedFindFreeSplitSlot(const std::vector<WorldMarkedLane> &lanes)
{
    bool used_source_slots[kWorldMarkedSourceTabs] = {};
    for (const WorldMarkedLane &lane : lanes) {
        if (lane.delay_slot >= 0 && lane.delay_slot < kWorldMarkedSourceTabs)
            used_source_slots[lane.delay_slot] = true;
    }
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++)
        if (!used_source_slots[slot])
            return slot;
    return -1;
}

static void WorldMarkedPruneSplitLanes(WorldMarkedSequenceState &state)
{
    bool seen_slot[kWorldMarkedSourceTabs] = {};
    std::vector<WorldMarkedSplitLane> kept;
    kept.reserve(state.split_lanes.size());
    for (const WorldMarkedSplitLane &split : state.split_lanes) {
        if (split.slot < 0 || split.slot >= kWorldMarkedSourceTabs)
            continue;
        if (seen_slot[split.slot])
            continue;
        if (!document_get(split.doc_idx))
            continue;
        if (state.sequence_frames[split.slot].empty())
            continue;
        seen_slot[split.slot] = true;
        kept.push_back(split);
    }
    state.split_lanes = kept;
}

bool WorldAppendMarkedSourceLane(WorldMarkedSequenceState &state, int doc_idx,
                                 std::vector<WorldMarkedLane> &lanes,
                                 bool used_source_slots[kWorldMarkedSourceTabs])
{
    Document *doc = document_get(doc_idx);
    if (!doc) return false;

    std::vector<int> marked_frames;
    std::vector<std::vector<int>> marked_groups;
    if (g_world_state.group_subframes)
        WorldCollectMarkedFrameGroups(doc, marked_frames, marked_groups);
    else
        WorldCollectMarkedFrames(doc, marked_frames);
    if (marked_frames.empty()) return false;

    int source_slot = WorldMarkedFindBaseSourceSlot(state, doc, doc_idx,
                                                    used_source_slots);
    if (source_slot < 0) return false;

    WorldMarkedLane lane = {};
    lane.doc = doc;
    lane.doc_idx = doc_idx;
    lane.delay_slot = source_slot;
    lane.frame_pos = 0;
    lane.img = NULL;
    lane.dummy_decap = false;
    lane.frames = marked_frames;

    /* A per-slot piece override the user built by hand wins over the automatic
       grouping; otherwise the chopped-piece groups become the frame pieces. */
    const std::vector<std::vector<int>> *pieces =
        !state.entry_pieces[source_slot].empty() ? &state.entry_pieces[source_slot]
        : (!marked_groups.empty() ? &marked_groups : NULL);
    WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                    lane.frame_pieces, lane.frame_labels,
                                    pieces);
    WorldMarkedSyncSequenceOverride(state, source_slot, doc, doc_idx,
                                    lane.frames, lane.frame_pieces,
                                    lane.frame_labels);
    lane.frame_docs = WorldMarkedResolveFrameDocs(doc, state.frame_doc[source_slot]);
    used_source_slots[source_slot] = true;
    lanes.push_back(lane);
    return true;
}

static bool WorldAppendMarkedSplitLanes(WorldMarkedSequenceState &state,
                                        std::vector<WorldMarkedLane> &lanes,
                                        bool used_source_slots[kWorldMarkedSourceTabs])
{
    bool appended = false;
    WorldMarkedPruneSplitLanes(state);
    for (const WorldMarkedSplitLane &split : state.split_lanes) {
        if ((int)lanes.size() >= kWorldMarkedSourceTabs)
            break;
        if (split.slot < 0 || split.slot >= kWorldMarkedSourceTabs)
            continue;
        if (used_source_slots[split.slot])
            continue;
        Document *doc = document_get(split.doc_idx);
        if (!doc)
            continue;

        WorldMarkedLane lane = {};
        lane.doc = doc;
        lane.doc_idx = split.doc_idx;
        lane.delay_slot = split.slot;
        lane.frame_pos = 0;
        lane.img = NULL;
        lane.dummy_decap = false;
        lane.frames = state.sequence_frames[split.slot];
        if (lane.frames.empty())
            continue;

        const char *doc_name = doc->fname_s[0] ? doc->fname_s : "Untitled";
        char label[128];
        snprintf(label, sizeof(label), "%s Row %d", doc_name, split.slot + 1);
        lane.label = label;
        lane.asm_label_part = WorldMarkedAsmLabelPart(label, split.slot);

        EnsureWorldMarkedFrameDelays(state, split.slot, (int)lane.frames.size());
        WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                        lane.frame_pieces, lane.frame_labels,
                                        &state.entry_pieces[split.slot],
                                        &state.frame_doc[split.slot]);
        lane.frame_docs = WorldMarkedResolveFrameDocs(doc, state.frame_doc[split.slot]);
        used_source_slots[split.slot] = true;
        lanes.push_back(lane);
        appended = true;
    }
    return appended;
}

bool WorldAppendMarkedDocumentLanes(WorldMarkedSequenceState &state,
                                    int active_doc_idx,
                                    std::vector<WorldMarkedLane> &lanes,
                                    bool *dummy_decap_missing)
{
    if (dummy_decap_missing) *dummy_decap_missing = false;
    bool appended = false;
    bool used_source_slots[kWorldMarkedSourceTabs] = {};
    WorldMarkedPruneSplitLanes(state);

    for (int i = 0; i < document_tab_count(); i++) {
        if (WorldAppendMarkedSourceLane(state, i, lanes, used_source_slots))
            appended = true;
        if ((int)lanes.size() >= kWorldMarkedSourceTabs) break;
    }

    if (WorldAppendMarkedSplitLanes(state, lanes, used_source_slots))
        appended = true;

    if (state.dummy_decap_body) {
        WorldMarkedLane dummy = WorldBuildDummyDecapLane(state, active_doc_idx);
        if (dummy.doc && !dummy.frames.empty()) {
            lanes.push_back(dummy);
            appended = true;
        } else if (dummy_decap_missing) {
            *dummy_decap_missing = true;
        }
    }

    return appended;
}

bool WorldAppendAsmLane(WorldMarkedSequenceState &state, const char *name,
                        const std::vector<WorldAsmLaneFrame> &frames,
                        Document *doc, int doc_idx, int slot_id,
                        std::vector<WorldMarkedLane> &lanes)
{
    if (!doc || frames.empty()) return false;

    WorldMarkedLane lane = {};
    lane.doc = doc;
    lane.doc_idx = doc_idx;
    lane.delay_slot = slot_id;
    lane.frame_pos = 0;
    lane.dummy_decap = false;
    lane.label = name ? name : "";

    Document *rep_doc = NULL;     /* first doc that actually resolved a piece */
    for (const WorldAsmLaneFrame &fr : frames) {
        std::vector<int> pcs;
        std::vector<Document*> pcs_docs;
        const std::vector<int> *piece_img = fr.piece_img;
        const std::vector<unsigned int> *piece_uid = fr.piece_doc_uid;
        if (piece_img) {
            for (size_t p = 0; p < piece_img->size(); p++) {
                int ri = (*piece_img)[p];
                if (ri < 0) continue;
                /* Resolved here, once per frame, and only into the per-frame
                   lane below — never stored back into the animation. */
                Document *pdoc = (piece_uid && p < piece_uid->size())
                               ? document_from_uid((*piece_uid)[p]) : NULL;
                if (!pdoc) pdoc = doc;
                pcs.push_back(ri);
                pcs_docs.push_back(pdoc);
                if (!rep_doc) rep_doc = pdoc;
            }
        }
        lane.frames.push_back(pcs.empty() ? -1 : pcs[0]);
        lane.frame_pieces.push_back(pcs);
        lane.frame_piece_docs.push_back(pcs_docs);
        lane.frame_docs.push_back(pcs_docs.empty() ? doc : pcs_docs[0]);
        lane.frame_labels.push_back(name ? name : "");
    }

    /* Anchor the lane on a doc that actually contains a piece so the shared
       render path resolves even when pieces live outside the active IMG. */
    if (rep_doc) lane.doc = rep_doc;

    int n = (int)lane.frames.size();
    EnsureWorldMarkedFrameDelays(state, slot_id, n);

    /* WHERE THE IMPORTED LANE'S TIMING COMES FROM, and it is not always the
       lane.

       An EXPANDED lane carries its hold by repeating the frame label - four
       identical rows is a hold of four - so those rows really are one tick
       each and applying a hold on top would multiply it. That was the only
       case this used to handle, and it pinned every import to 1.

       Most MK2 lanes are not written that way. They list one row per POSE and
       the hold lives in the CALLER: mframew takes it in a0, animate_a9 takes
       [sleep|index] with the sleep in the high word, init_anirate takes ticks
       per frame. a_sahb_splat is five rows run at sleep 4; a_guts, STAB,
       SMGUSH and the rest of MKBLOOD are the same shape. Nothing in the table
       itself says 4, so importing one and pinning it to 1 produced a row
       running at 54.7 fps - a rate no MK2 animation plays at - with no hint
       that timing had been discarded.

       Tell them apart by looking for a repeat. A lane with two consecutive
       identical frames was expanded and is self-timing; one whose every row is
       a distinct pose carries no timing at all, so the honest default is the
       row's own hold rather than a made-up 1. */
    bool expanded = false;
    for (int k = 1; k < n && !expanded; k++) {
        const std::vector<int> *a = frames[k - 1].piece_img;
        const std::vector<int> *b = frames[k].piece_img;
        if (a && b && *a == *b) expanded = true;
    }
    int imported_hold = expanded ? 1 : WorldMarkedSlotHold(state, slot_id);

    for (int k = 0; k < n; k++) {
        const WorldAsmLaneFrame &fr = frames[k];
        state.frame_delays[slot_id][k] = ClampTimelineHold(imported_hold);
        /* fr.dx/dy is an ASM POSITION offset -- the running total of the
           animation's own ani_adjustxy rows, or a *_local_anipts row, which
           the exporter writes in that same sign. local_dx/dy is an ANIPOINT
           offset. Converting between them is the exporter's rule read
           backwards, and it is flip-dependent: X always negates (the engine's
           own b_fliph negation cancels the preview's X mirror), Y negates
           only when the frame is not V-flipped. Copied straight across, every
           hand-written ani_adjustxy lane drew mirrored about the anchor in
           this row while the ASM Animations window drew the same numbers
           correctly. */
        state.local_dx[slot_id][k] = ClampWorldMarkedAniptDelta(
            -fr.dx + state.lane_base_dx[slot_id]);
        state.local_dy[slot_id][k] = ClampWorldMarkedAniptDelta(
            (fr.mirror_v ? fr.dy : -fr.dy) + state.lane_base_dy[slot_id]);
        state.visible_from[slot_id][k] = 0;
        state.visible_until[slot_id][k] = 0;
        state.motion_dx[slot_id][k] = 0;
        state.motion_dy[slot_id][k] = 0;
        state.motion_cap_x[slot_id][k] = 0;
        state.motion_cap_y[slot_id][k] = 0;
        state.frame_mirror[slot_id][k] =
            (fr.mirror ? kWorldFrameMirrorX : 0) |
            (fr.mirror_v ? kWorldFrameMirrorY : 0);
    }

    lanes.push_back(lane);
    return true;
}

static bool WorldSeqScrEntryValues(const SeqScrRecordView &rec,
                                   const SeqScrLayoutInfo &li,
                                   int entry_idx,
                                   int *target_idx,
                                   int *ticks,
                                   int *dx,
                                   int *dy)
{
    if (!g_doc || !g_doc->scrseqmem_p || rec.truncated ||
        entry_idx < 0 || entry_idx >= rec.num)
        return false;
    unsigned char *blob = (unsigned char *)g_doc->scrseqmem_p;
    unsigned char *entry = blob + rec.entries_offset +
                           (size_t)entry_idx * (size_t)li.entry_size;
    if (target_idx) *target_idx = (int)SeqScrReadI16(entry + li.entry_index_off);
    if (ticks) *ticks = (int)entry[li.entry_ticks_off];
    if (dx) *dx = (int)SeqScrReadI16(entry + li.entry_dx_off);
    if (dy) *dy = (int)SeqScrReadI16(entry + li.entry_dy_off);
    return true;
}

static int WorldSeqScrFirstImageInSequence(const SeqScrRecordView &seq,
                                           const SeqScrLayoutInfo &li)
{
    if (seq.script || seq.truncated) return -1;
    for (int i = seq.num - 1; i >= 0; i--) {
        int img_idx = -1;
        if (!WorldSeqScrEntryValues(seq, li, i, &img_idx, NULL, NULL, NULL))
            continue;
        if (img_idx >= 0 && doc_get_img(g_doc, img_idx))
            return img_idx;
    }
    return -1;
}

static bool WorldDecodeSeqScrRecord(int record_index,
                                    std::vector<int> &frames,
                                    std::vector<int> &delays,
                                    std::vector<int> &dxs,
                                    std::vector<int> &dys,
                                    std::vector<int> &targets,
                                    std::vector<std::string> &labels,
                                    bool *is_script,
                                    std::string *name)
{
    frames.clear();
    delays.clear();
    dxs.clear();
    dys.clear();
    targets.clear();
    labels.clear();
    if (is_script) *is_script = false;
    if (name) name->clear();
    if (!g_doc || !g_doc->scrseqmem_p || g_doc->scrseqbytes == 0)
        return false;

    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    if (!SeqScrBuildRecords(records, &truncated) ||
        record_index < 0 || record_index >= (int)records.size())
        return false;

    const SeqScrRecordView &rec = records[(size_t)record_index];
    if (rec.truncated) return false;
    if (is_script) *is_script = rec.script;
    if (name) *name = rec.name;

    SeqScrLayoutInfo li = SeqScrLayout();
    frames.reserve((size_t)rec.num);
    delays.reserve((size_t)rec.num);
    dxs.reserve((size_t)rec.num);
    dys.reserve((size_t)rec.num);
    targets.reserve((size_t)rec.num);
    labels.reserve((size_t)rec.num);

    for (int display_e = 0; display_e < rec.num; display_e++) {
        int e = rec.script ? display_e : (rec.num - 1 - display_e);
        int target = -1;
        int ticks = 1;
        int dx = 0;
        int dy = 0;
        if (!WorldSeqScrEntryValues(rec, li, e, &target, &ticks, &dx, &dy))
            continue;

        if (rec.script) {
            std::string label;
            int preview_img = -1;
            if (target >= 0 && target < (int)g_doc->seqcnt &&
                target < (int)records.size() && !records[(size_t)target].script) {
                const SeqScrRecordView &seq = records[(size_t)target];
                preview_img = WorldSeqScrFirstImageInSequence(seq, li);
                char buf[80];
                IMG *preview = doc_get_img(g_doc, preview_img);
                if (preview) {
                    snprintf(buf, sizeof(buf), "%02d %s -> %s", e,
                             seq.name[0] ? seq.name : "sequence",
                             img_name_string(preview).c_str());
                } else {
                    snprintf(buf, sizeof(buf), "%02d %s", e,
                             seq.name[0] ? seq.name : "sequence");
                }
                label = buf;
            } else {
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "%02d seq[%d]?", e, target);
                label = fallback;
            }
            frames.push_back(preview_img);
            targets.push_back(target);
            labels.push_back(label);
        } else {
            frames.push_back(target);
            targets.push_back(target);
            IMG *img = doc_get_img(g_doc, target);
            if (img) {
                labels.push_back(img_name_string(img));
            } else {
                char fallback[32];
                snprintf(fallback, sizeof(fallback), "img[%d]?", target);
                labels.push_back(fallback);
            }
        }

        delays.push_back(ClampTimelineHold(ticks > 0 ? ticks : 1));
        dxs.push_back(ClampWorldMarkedAniptDelta(dx));
        dys.push_back(ClampWorldMarkedAniptDelta(dy));
    }

    return !frames.empty();
}

static std::string WorldSeqScrRecordAsmLabel(const SeqScrRecordView &rec)
{
    int local_idx = rec.script ? rec.index - (int)g_doc->seqcnt : rec.index;
    char fallback[32];
    snprintf(fallback, sizeof(fallback), "%s%d",
             rec.script ? "script" : "seq", local_idx);
    std::string token = WorldMarkedAsmToken(rec.name, fallback);
    char label[96];
    snprintf(label, sizeof(label), "%s%02d_%s",
             rec.script ? "scr" : "seq", local_idx, token.c_str());
    return label;
}

/* ani_flip and ani_flip_v are toggle opcodes, while World View stores an
   absolute orientation for each entry.  Emit only the toggles needed to
   reach the next entry's orientation so exported ASM has the same image
   state as the preview (not merely a descriptive flipX/flipY comment). */
static void WorldAppendFrameFlipOps(std::string &out, int *emitted_mirror,
                                    int wanted_mirror)
{
    if (!emitted_mirror) return;
    wanted_mirror &= kWorldFrameMirrorX | kWorldFrameMirrorY;
    if (((*emitted_mirror) ^ wanted_mirror) & kWorldFrameMirrorX)
        out += "\t.long\tani_flip\n";
    if (((*emitted_mirror) ^ wanted_mirror) & kWorldFrameMirrorY)
        out += "\t.long\tani_flip_v\n";
    *emitted_mirror = wanted_mirror;
}

static bool WorldSeqScrExportUsesLiveState(const SeqScrRecordView &rec)
{
    const WorldMarkedSequenceState &state = g_world_marked_state;
    return state.embedded_active &&
           state.embedded_record_index == rec.index &&
           state.embedded_doc_idx == document_active_index() &&
           !state.sequence_frames[kWorldEmbeddedSeqScrSlot].empty();
}

static void WorldAppendSeqScrSequenceAsm(std::string &out,
                                         const SeqScrRecordView &rec,
                                         const SeqScrLayoutInfo &li)
{
    std::string label = WorldSeqScrRecordAsmLabel(rec);
    int local_idx = rec.index;
    char line[256];
    snprintf(line, sizeof(line),
             "; Sequence %d %.16s: frame table + matching local anipts\n",
             local_idx, rec.name);
    out += line;
    bool live = WorldSeqScrExportUsesLiveState(rec);
    const int live_slot = kWorldEmbeddedSeqScrSlot;
    int entry_count = live
                    ? (int)g_world_marked_state.sequence_frames[live_slot].size()
                    : rec.num;
    snprintf(line, sizeof(line), "%s_count\t.word\t%d\n",
             label.c_str(), entry_count);
    out += line;

    out += label;
    out += "_frames\n";
    std::string anipts = label + "_anipts\n";
    int expanded_ticks = 0;
    int emitted_mirror = 0;
    for (int display_e = 0; display_e < entry_count; display_e++) {
        int e = display_e < rec.num ? rec.num - 1 - display_e : -1;
        int target = -1;
        int ticks = 1;
        int dx = 0;
        int dy = 0;
        int frame_mirror = 0;
        if (live) {
            const WorldMarkedSequenceState &state = g_world_marked_state;
            target = state.sequence_frames[live_slot][(size_t)display_e];
            ticks = display_e < (int)state.frame_delays[live_slot].size()
                  ? state.frame_delays[live_slot][(size_t)display_e] : 1;
            dx = display_e < (int)state.local_dx[live_slot].size()
               ? state.local_dx[live_slot][(size_t)display_e] : 0;
            dy = display_e < (int)state.local_dy[live_slot].size()
               ? state.local_dy[live_slot][(size_t)display_e] : 0;
            frame_mirror =
                display_e < (int)state.frame_mirror[live_slot].size()
              ? state.frame_mirror[live_slot][(size_t)display_e] : 0;
        } else {
            if (!WorldSeqScrEntryValues(rec, li, e, &target, &ticks, &dx, &dy))
                continue;
        }
        int hold = ClampTimelineHold(ticks > 0 ? ticks : 1);
        IMG *img = doc_get_img(g_doc, target);
        char fallback[32];
        snprintf(fallback, sizeof(fallback), "img%d", target);
        std::string sprite = WorldMarkedAsmToken(img ? img_name_string(img) : "",
                                                 fallback);
        /* Emitted in MK2's position sign, not the editor's anipoint sign.
           Not a constant negation: X always flips (multi_adjust_xy's own
           b_fliph negation cancels the preview's X mirror), Y flips only for
           frames that are not V-flipped. See the contract at the head of this
           export. */
        int out_dx = -dx;
        int out_dy = (frame_mirror & kWorldFrameMirrorY) ? dy : -dy;
        for (int t = 0; t < hold; t++) {
            WorldAppendFrameFlipOps(out, &emitted_mirror, frame_mirror);
            snprintf(line, sizeof(line),
                     "\t.long\t%s\t; entry %d visual %d tick %d/%d img=%d dX=%d dY=%d%s%s\n",
                     sprite.c_str(), e, display_e, t + 1, hold, target,
                     out_dx, out_dy,
                     (frame_mirror & kWorldFrameMirrorX) ? " flipX" : "",
                     (frame_mirror & kWorldFrameMirrorY) ? " flipY" : "");
            out += line;
            snprintf(line, sizeof(line),
                     "\t.word\t%d,%d\t; entry %d visual %d tick %d\n",
                     out_dx, out_dy, e, display_e, expanded_ticks);
            anipts += line;
            expanded_ticks++;
        }
    }
    snprintf(line, sizeof(line), "%s_ticks\t.word\t%d\n\n",
             label.c_str(), expanded_ticks);
    out += line;
    out += anipts;
    out += "\n";
}

static void WorldAppendSeqScrScriptAsm(std::string &out,
                                       const SeqScrRecordView &rec,
                                       const SeqScrLayoutInfo &li,
                                       const std::vector<SeqScrRecordView> &records)
{
    std::string label = WorldSeqScrRecordAsmLabel(rec);
    char line[256];
    snprintf(line, sizeof(line),
             "; Script %d %.16s: higher-level sequence calls\n",
             rec.index - (int)g_doc->seqcnt, rec.name);
    out += line;
    snprintf(line, sizeof(line), "%s_count\t.word\t%d\n",
             label.c_str(), rec.num);
    out += line;

    out += label;
    out += "\n";
    std::string anipts = label + "_anipts\n";
    bool live = WorldSeqScrExportUsesLiveState(rec);
    for (int e = 0; e < rec.num; e++) {
        int target = -1;
        int ticks = 1;
        int dx = 0;
        int dy = 0;
        if (!WorldSeqScrEntryValues(rec, li, e, &target, &ticks, &dx, &dy))
            continue;
        if (live) {
            const WorldMarkedSequenceState &state = g_world_marked_state;
            int slot = kWorldEmbeddedSeqScrSlot;
            ticks = e < (int)state.frame_delays[slot].size()
                  ? state.frame_delays[slot][(size_t)e] : ticks;
            dx = e < (int)state.local_dx[slot].size()
               ? state.local_dx[slot][(size_t)e] : dx;
            dy = e < (int)state.local_dy[slot].size()
               ? state.local_dy[slot][(size_t)e] : dy;
        }

        std::string seq_label;
        const char *seq_name = "missing";
        if (target >= 0 && target < (int)g_doc->seqcnt &&
            target < (int)records.size() && !records[(size_t)target].script) {
            const SeqScrRecordView &seq = records[(size_t)target];
            seq_label = WorldSeqScrRecordAsmLabel(seq);
            seq_name = seq.name;
        } else {
            char fallback[32];
            snprintf(fallback, sizeof(fallback), "missing_seq_%d", target);
            seq_label = fallback;
        }

        snprintf(line, sizeof(line),
                 "\t.long\t%s_frames,%s_anipts\t; entry %d seq=%d %.16s ticks=%d\n",
                 seq_label.c_str(), seq_label.c_str(), e, target, seq_name,
                 ticks);
        out += line;
        /* Same anipoint->position conversion as the sequence table. A script
           entry carries no flip of its own, so the V-flip comes from the
           live lane when there is one; a stored script falls back to the
           unflipped rule, which is all its ENTRY words can tell us. */
        bool entry_flipv = false;
        if (live) {
            const WorldMarkedSequenceState &state = g_world_marked_state;
            int slot = kWorldEmbeddedSeqScrSlot;
            entry_flipv = e < (int)state.frame_mirror[slot].size() &&
                          (state.frame_mirror[slot][(size_t)e] &
                           kWorldFrameMirrorY) != 0;
        }
        snprintf(line, sizeof(line),
                 "\t.word\t%d,%d,%d\t; ticks,dX,dY for script entry %d\n",
                 ticks, -dx, entry_flipv ? dy : -dy, e);
        anipts += line;
    }
    out += "\n";
    out += anipts;
    out += "\n";
}

std::string WorldBuildSeqScrAsmExport(int record_index)
{
    std::string out;
    out.reserve(8192);
    out += "; IMGTOOL embedded WIMP SEQSCR export\n";
    out += "; Image records: export marked sprites with File > Export > Write TBL.\n";
    out += "; Sequences below emit *_frames tables and matching *_anipts tables.\n";
    out += "; Scripts below emit higher-level tables that point at sequence tables.\n";
    out += "; dX/dY in the *_anipts tables are MK2 POSITION offsets, the same sign\n";
    out += ";   and space as ani_adjustxy: +dY moves the object DOWN the screen\n";
    out += ";   (multi_adjust_xy does oypos += dY), +dX moves it forward in FACING\n";
    out += ";   space (oxpos += dX, negated by the engine when flipped). Use them\n";
    out += ";   as printed; do not negate them again when transcribing.\n";
    out += ";   The editor holds the same placement as an ANIPOINT offset, drawing\n";
    out += ";   at anchor - anieff(ani + offset). Converting is NOT a flat sign\n";
    out += ";   flip: X always inverts, but Y inverts only for frames that are not\n";
    out += ";   V-flipped, because anieff already mirrors Y on the ones that are\n";
    out += ";   and the engine never mirrors dY itself. That is why a lane of\n";
    out += ";   V-flipped frames reads through unchanged and an unflipped one does\n";
    out += ";   not, and why a constant rule inverts one or the other.\n";
    out += "; They are ABSOLUTE offsets from the anchor. ani_adjustxy is cumulative,\n";
    out += ";   so it wants the difference between consecutive rows, not the rows.\n\n";

    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    if (!SeqScrBuildRecords(records, &truncated) || records.empty()) {
        out += "; No SEQSCR/ENTRY data found.\n";
        return out;
    }

    SeqScrLayoutInfo li = SeqScrLayout();
    std::vector<char> include(records.size(), 0);
    if (record_index >= 0 && record_index < (int)records.size()) {
        include[(size_t)record_index] = 1;
        const SeqScrRecordView &focus = records[(size_t)record_index];
        if (focus.script && !focus.truncated) {
            for (int e = 0; e < focus.num; e++) {
                int target = -1;
                if (!WorldSeqScrEntryValues(focus, li, e, &target, NULL, NULL, NULL))
                    continue;
                if (target >= 0 && target < (int)g_doc->seqcnt &&
                    target < (int)records.size())
                    include[(size_t)target] = 1;
            }
        }
    } else {
        for (size_t i = 0; i < records.size(); i++)
            include[i] = 1;
    }

    for (const SeqScrRecordView &rec : records) {
        if (!include[(size_t)rec.index] || rec.script || rec.truncated) continue;
        WorldAppendSeqScrSequenceAsm(out, rec, li);
    }
    for (const SeqScrRecordView &rec : records) {
        if (!include[(size_t)rec.index] || !rec.script || rec.truncated) continue;
        WorldAppendSeqScrScriptAsm(out, rec, li, records);
    }

    if (truncated)
        out += "; Warning: source SEQSCR blob was truncated; skipped truncated records.\n";
    return out;
}

int WorldGamePlacementOriginY(const IMG *img)
{
    if (!img || img->h <= 0) return g_world_state.origin_y;
    /* Renderer: top = origin_y - anipoint_effective(aniy, h), bottom = top + h.
       Solve bottom == floor_y. Mirroring only affects X, so the unflipped
       effective value is the right one to use here. */
    int eff = anipoint_effective((int)(short)img->aniy, (int)img->h, false,
                                 g_mirror_convention);
    return g_world_state.floor_y + eff - (int)img->h;
}

void WorldApplyGamePlacement(const WorldMarkedSequenceState &state)
{
    if (!g_world_state.game_placement) return;
    if (!state.embedded_active) return;
    const int slot = kWorldEmbeddedSeqScrSlot;
    Document *doc = document_get(state.embedded_doc_idx);
    if (!doc) return;

    /* First frame that actually has pixels: a sequence can open on an empty
       or missing entry, and anchoring to that would put the whole animation
       in the wrong place. */
    for (int i = 0; i < (int)state.sequence_frames[slot].size(); i++) {
        int doc_idx = i < (int)state.frame_doc[slot].size()
                    ? state.frame_doc[slot][(size_t)i] : -1;
        Document *fdoc = WorldMarkedResolveEntryDoc(doc, doc_idx);
        IMG *img = doc_get_img(fdoc, state.sequence_frames[slot][(size_t)i]);
        if (!img || img->h <= 0 || !img->data_p) continue;
        g_world_state.origin_y = WorldGamePlacementOriginY(img);
        return;
    }
}

bool WorldLoadSeqScrRecord(int record_index)
{
    std::vector<int> frames;
    std::vector<int> delays;
    std::vector<int> dxs;
    std::vector<int> dys;
    std::vector<int> targets;
    std::vector<std::string> labels;
    bool is_script = false;
    std::string name;
    if (!WorldDecodeSeqScrRecord(record_index, frames, delays, dxs, dys,
                                 targets, labels, &is_script, &name))
        return false;

    WorldMarkedSequenceState &state = g_world_marked_state;
    int slot = kWorldEmbeddedSeqScrSlot;
    WorldMarkedClearSequenceState(state, slot);
    state.default_frames[slot] = frames;
    state.sequence_frames[slot] = frames;
    WorldMarkedSetRowDoc(state, slot, document_active_index());
    state.embedded_active = true;
    state.embedded_is_script = is_script;
    /* The Anim workspace shows this record and nothing else. Marked rows, ASM
       lanes, the dummy body, and the reaction companion are World View
       staging concepts and stay there. */
    state.embedded_show_companions = false;
    state.embedded_record_index = record_index;
    state.embedded_doc_idx = document_active_index();
    state.embedded_name = name;
    state.embedded_frame_labels = labels;
    state.embedded_targets = targets;
    state.lane_visible[slot] = true;
    state.hold_end[slot] = false;

    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());
    for (int i = 0; i < (int)frames.size(); i++) {
        state.frame_delays[slot][i] = i < (int)delays.size()
                                    ? delays[i] : state.default_hold;
        state.local_dx[slot][i] = i < (int)dxs.size() ? dxs[i] : 0;
        state.local_dy[slot][i] = i < (int)dys.size() ? dys[i] : 0;
        state.visible_from[slot][i] = 0;
        state.visible_until[slot][i] = 0;
        state.motion_dx[slot][i] = 0;
        state.motion_dy[slot][i] = 0;
        state.motion_cap_x[slot][i] = 0;
        state.motion_cap_y[slot][i] = 0;
        state.frame_mirror[slot][i] = 0;
        state.frame_z[slot][i] = 0;
        state.dual_on[slot][i] = 0;
        state.dual_dx[slot][i] = 0;
        state.dual_dy[slot][i] = 0;
        state.dual_z[slot][i] = 0;
    }

    /* Loading a record opens the Sequence/Script workspace rather than World
       View: the two modes are mutually exclusive canvas tabs. */
    g_seqscr_workspace = true;
    g_world_state.enabled = false;
    AnipointLink().enabled = false;
    /* Stand the animation on the floor rather than hanging it off the stock
       (200, 20) anchor, so what you see is where the game draws it. */
    WorldApplyGamePlacement(state);
    state.marked_play = true;
    WorldMarkedRestart(state);
    state.paused = true;
    state.timer = 0.0f;
    return true;
}

static int WorldMarkedEffectiveTickForSlot(WorldMarkedSequenceState &state,
                                           int slot, int frame_count,
                                           int tick)
{
    if (tick < 0) tick = 0;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0)
        return tick;

    int stop_tick = ClampWorldMarkedVisibleFrom(state.stop_tick[slot]);
    state.stop_tick[slot] = stop_tick;
    if (stop_tick > 0 && tick >= stop_tick)
        return stop_tick;

    return tick;
}

static bool WorldAppendEmbeddedSeqScrLane(WorldMarkedSequenceState &state,
                                          std::vector<WorldMarkedLane> &lanes)
{
    if (!state.embedded_active ||
        state.embedded_record_index < 0 ||
        state.embedded_doc_idx < 0)
        return false;

    Document *doc = document_get(state.embedded_doc_idx);
    if (!doc || state.sequence_frames[kWorldEmbeddedSeqScrSlot].empty())
        return false;

    WorldMarkedLane lane = {};
    lane.doc = doc;
    lane.doc_idx = state.embedded_doc_idx;
    lane.delay_slot = kWorldEmbeddedSeqScrSlot;
    lane.frame_pos = 0;
    lane.dummy_decap = false;
    lane.frames = state.sequence_frames[kWorldEmbeddedSeqScrSlot];
    lane.label = state.embedded_is_script ? "Script" : "Sequence";
    if (!state.embedded_name.empty()) {
        lane.label += ": ";
        lane.label += state.embedded_name;
    }
    lane.asm_label_part = WorldMarkedAsmLabelPart(state.embedded_name.c_str(),
                                                  kWorldEmbeddedSeqScrSlot);
    EnsureWorldMarkedFrameDelays(state, kWorldEmbeddedSeqScrSlot,
                                 (int)lane.frames.size());
    WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                    lane.frame_pieces, lane.frame_labels,
                                    &state.entry_pieces[kWorldEmbeddedSeqScrSlot],
                                    &state.frame_doc[kWorldEmbeddedSeqScrSlot]);
    lane.frame_docs = WorldMarkedResolveFrameDocs(doc, state.frame_doc[kWorldEmbeddedSeqScrSlot]);
    for (int i = 0; i < (int)lane.frame_labels.size() &&
                    i < (int)state.embedded_frame_labels.size(); i++) {
        if (!state.embedded_frame_labels[(size_t)i].empty())
            lane.frame_labels[(size_t)i] = state.embedded_frame_labels[(size_t)i];
    }
    lane.tick =
        WorldMarkedEffectiveTickForSlot(state, kWorldEmbeddedSeqScrSlot,
                                        (int)lane.frames.size(), state.frame);
    lane.frame_pos =
        WorldMarkedFrameForTick(state, kWorldEmbeddedSeqScrSlot,
                                (int)lane.frames.size(), lane.tick,
                                state.hold_end[kWorldEmbeddedSeqScrSlot]);
    if (lane.frame_pos >= 0 && lane.frame_pos < (int)lane.frames.size())
        lane.img = doc_get_img(doc, lane.frames[(size_t)lane.frame_pos]);
    lanes.push_back(lane);
    return true;
}

WorldMarkedTabsResult WorldDrawMarkedTabs(WorldMarkedSequenceState &state,
                                          const WorldViewState &world,
                                          ImVec2 avail,
                                          ImVec2 img_pos,
                                          float delta_time,
                                          int active_doc_idx,
                                          IMG *selected_img,
                                          const std::vector<WorldMarkedAsmLaneInput> &asm_lanes,
                                          bool draw_panel)
{
    WorldMarkedTabsResult result = {};
    if (!state.marked_play)
        return result;

    std::vector<WorldMarkedLane> lanes;
    lanes.reserve(kWorldMarkedMaxTabs);

    /* SEQSCR records deliberately do not appear here any more: they animate in
       the Sequence/Script workspace ("Anim" canvas mode), which owns its own
       lane, inspector, and entry table. World View is marked rows + ASM lanes. */
    bool dummy_decap_missing = false;
    WorldAppendMarkedDocumentLanes(state, active_doc_idx, lanes,
                                   &dummy_decap_missing);

    for (const WorldMarkedAsmLaneInput &input : asm_lanes) {
        if (!input.enabled) continue;
        WorldAppendAsmLane(state, input.name, input.frames, input.doc,
                           input.doc_idx, input.slot_id, lanes);
    }

    WorldMarkedApplyLaneOrder(state, lanes);

    /* One marked row is enough.

       This used to require a second row (or an ASM lane) before it would draw
       anything, on the assumption that the panel exists to play two animations
       against each other. But everything the panel carries -- splitting a lane,
       per-frame delays and offsets, motion, visibility, mirroring, the ASM
       export -- is per-row work that is just as necessary when staging a single
       move. Requiring a second row meant marking a throwaway tab to unlock the
       tools for the row you actually cared about. */
    if (lanes.empty()) return result;
    if (!WorldUpdateMarkedLanePlayback(state, lanes, delta_time))
        return result;

    WorldMarkedSceneResult scene =
        WorldDrawMarkedScene(state, world, lanes, avail, img_pos);

    ImGui::SetCursorScreenPos(img_pos);
    ImGui::Dummy(ImVec2(avail.x, avail.y));

    if (draw_panel) {
        result.panel =
            WorldDrawMarkedPanel(state, lanes, scene.panel_layout,
                                 dummy_decap_missing, selected_img,
                                 active_doc_idx);
    }
    result.drew = true;
    return result;
}

/* Ticks after which the whole preview repeats, or 0 when it should keep
   counting. Individual lanes already wrap on their own length, but the global
   tick fed to Show@/Hide@/Stop@ used to climb forever, so a scene that had
   visibly finished kept running and those absolute-tick fields drifted out of
   reach. The loop point is the longest visible lane, extended past any entry
   scheduled beyond it so nothing is cut off mid-schedule.

   Zero — keep counting — whenever something deliberately parks the preview on
   a tick: a lane holding its last entry, a tick stop, or a ping-pong chain
   whose reversal is computed from the running tick. */
/* How long the scene runs, and whether it repeats.

   Returns the tick to wrap at, or 0 when NOTHING in the scene loops. Either
   way `out_end_tick` receives the tick by which every visible lane has
   finished.

   This used to bail out with 0 the moment it met one held or stopped lane,
   which meant "do not wrap" -- and nothing else ever stopped the clock, so the
   tick counter ran away for as long as the window was open with the scene
   sitting finished underneath it. A lane that holds is a lane that has
   finished, not a reason to count forever. */
static int WorldMarkedPreviewLoopTicks(WorldMarkedSequenceState &state,
                                       const std::vector<WorldMarkedLane> &lanes,
                                       int *out_end_tick)
{
    int longest = 0;
    bool any_loops = false;
    for (size_t li = 0; li < lanes.size(); li++) {
        const WorldMarkedLane &lane = lanes[li];
        int slot = lane.delay_slot;
        int n = (int)lane.frames.size();
        if (n <= 0 || slot < 0 || slot >= kWorldMarkedMaxTabs) continue;
        if (!state.lane_visible[slot]) continue;
        /* Holds its last frame, freezes at a tick, or reverses -- none of
           these come back round on their own. */
        bool holds = state.hold_end[slot] ||
                     state.stop_tick[slot] > 0 ||
                     state.chain_pingpong[slot];
        if (!holds) any_loops = true;

        int span = WorldMarkedSequenceTicks(state, slot, n);
        for (int i = 0; i < n; i++) {
            if (i < (int)state.visible_from[slot].size() &&
                state.visible_from[slot][i] + 1 > span)
                span = state.visible_from[slot][i] + 1;
            if (i < (int)state.visible_until[slot].size() &&
                state.visible_until[slot][i] > span)
                span = state.visible_until[slot][i];
        }
        if (state.stop_tick[slot] > 0 && state.stop_tick[slot] + 1 > span)
            span = state.stop_tick[slot] + 1;
        if (span > longest) longest = span;
    }
    if (out_end_tick) *out_end_tick = longest;
    /* One looping lane is enough to make the whole scene repeat: the held
       ones replay alongside it, which is what a scene loop means. */
    return any_loops ? longest : 0;
}

bool WorldUpdateMarkedLanePlayback(WorldMarkedSequenceState &state,
                                   std::vector<WorldMarkedLane> &lanes,
                                   float delta_time)
{
    if (state.embedded_active && state.embedded_is_script) {
        state.paused = true;
        state.timer = 0.0f;
    }
    if (!state.paused)
        state.timer += delta_time;
    float step = 1.0f / kMk2TickHz;
    while (state.timer >= step) {
        state.timer -= step;
        state.frame++;
    }
    int end_tick = 0;
    int loop_ticks = WorldMarkedPreviewLoopTicks(state, lanes, &end_tick);
    state.preview_end_tick = end_tick;
    if (loop_ticks > 0) {
        if (state.frame >= loop_ticks)
            state.frame %= loop_ticks;
    } else if (end_tick > 0 && state.frame >= end_tick) {
        /* Nothing here loops and everything has played. Park the clock on the
           last tick instead of counting into empty space -- the scene on
           screen stopped changing a long time before the number did. */
        state.frame = end_tick;
        state.paused = true;
    }

    bool have_image = false;
    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        WorldMarkedLane &lane = lanes[slot];
        int n = (int)lane.frames.size();
        if (n <= 0) continue;
        lane.tick = WorldMarkedEffectiveTickForSlot(state, lane.delay_slot, n,
                                                    state.frame);
        lane.frame_pos = WorldMarkedFrameForTick(state, lane.delay_slot, n,
                                                 lane.tick,
                                                 state.hold_end[lane.delay_slot]);
        Document *fdoc = (lane.frame_pos < (int)lane.frame_docs.size() &&
                          lane.frame_docs[lane.frame_pos])
                       ? lane.frame_docs[lane.frame_pos] : lane.doc;
        lane.img = doc_get_img(fdoc, lane.frames[lane.frame_pos]);
        if (lane.img) have_image = true;
        if (state.embedded_active && state.embedded_is_script &&
            lane.delay_slot == kWorldEmbeddedSeqScrSlot)
            have_image = true;
    }
    return have_image;
}

static unsigned char WorldMarkedLaneAlpha(int slot)
{
    static const unsigned char kAlpha[] =
        {255, 185, 170, 155, 205, 235, 190, 220, 175, 210};
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return 255;
    return kAlpha[slot % (int)(sizeof(kAlpha) / sizeof(kAlpha[0]))];
}

ImU32 WorldMarkedLaneOutlineColor(int slot)
{
    static const ImU32 kOutline[] = {
        IM_COL32(120, 190, 255, 230),
        IM_COL32(255, 190, 90, 230),
        IM_COL32(120, 230, 150, 230),
        IM_COL32(230, 130, 230, 230),
        IM_COL32(240, 80, 80, 230),
        IM_COL32(180, 170, 255, 230),
        IM_COL32(110, 220, 220, 230),
        IM_COL32(255, 130, 160, 230),
        IM_COL32(210, 220, 95, 230),
        IM_COL32(160, 205, 120, 230)
    };
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return IM_COL32(220, 220, 220, 230);
    return kOutline[slot % (int)(sizeof(kOutline) / sizeof(kOutline[0]))];
}

static bool WorldMarkedEntryVisibleAtTick(WorldMarkedSequenceState &state,
                                          int slot, int frame_idx, int tick)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_idx < 0)
        return false;
    int visible_from = 0;
    int visible_until = 0;
    if (frame_idx < (int)state.visible_from[slot].size())
        visible_from = state.visible_from[slot][frame_idx];
    if (frame_idx < (int)state.visible_until[slot].size())
        visible_until = state.visible_until[slot][frame_idx];
    return tick >= visible_from &&
           (visible_until <= 0 || tick < visible_until);
}

static bool WorldMarkedEntryHasTimedHold(WorldMarkedSequenceState &state,
                                         int slot, int frame_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_idx < 0)
        return false;
    int visible_from = 0;
    int visible_until = 0;
    if (frame_idx < (int)state.visible_from[slot].size())
        visible_from = state.visible_from[slot][frame_idx];
    if (frame_idx < (int)state.visible_until[slot].size())
        visible_until = state.visible_until[slot][frame_idx];
    return visible_from > 0 || visible_until > 0;
}

static int WorldMarkedEntryMotionStartTick(WorldMarkedSequenceState &state,
                                           int slot, int frame_count,
                                           int frame_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_idx < 0)
        return 0;
    int visible_from = 0;
    int visible_until = 0;
    if (frame_idx < (int)state.visible_from[slot].size())
        visible_from = state.visible_from[slot][frame_idx];
    if (frame_idx < (int)state.visible_until[slot].size())
        visible_until = state.visible_until[slot][frame_idx];
    if (visible_from > 0 || visible_until > 0)
        return visible_from;
    return WorldMarkedTickForFrame(state, slot, frame_count, frame_idx);
}

static int WorldMarkedEntryMotionElapsed(WorldMarkedSequenceState &state,
                                         int slot, int frame_count,
                                         int frame_idx, int tick)
{
    int start_tick =
        WorldMarkedEntryMotionStartTick(state, slot, frame_count, frame_idx);
    return tick > start_tick ? tick - start_tick : 0;
}

static int WorldMarkedClampedMotion(int velocity, int elapsed, int cap)
{
    int motion = velocity * elapsed;
    if (cap > 0) {
        if (motion > cap) motion = cap;
        if (motion < -cap) motion = -cap;
    }
    return motion;
}

static void WorldMarkedEffectiveLocalDelta(WorldMarkedSequenceState &state,
                                           int slot, int frame_count,
                                           int frame_idx, bool dual,
                                           int tick, int *out_dx, int *out_dy)
{
    int dx = dual ? state.dual_dx[slot][frame_idx]
                  : state.local_dx[slot][frame_idx];
    int dy = dual ? state.dual_dy[slot][frame_idx]
                  : state.local_dy[slot][frame_idx];
    int elapsed =
        WorldMarkedEntryMotionElapsed(state, slot, frame_count, frame_idx, tick);
    int vx = (frame_idx < (int)state.motion_dx[slot].size())
           ? state.motion_dx[slot][frame_idx] : 0;
    int vy = (frame_idx < (int)state.motion_dy[slot].size())
           ? state.motion_dy[slot][frame_idx] : 0;
    int cap_x = (frame_idx < (int)state.motion_cap_x[slot].size())
              ? state.motion_cap_x[slot][frame_idx] : 0;
    int cap_y = (frame_idx < (int)state.motion_cap_y[slot].size())
              ? state.motion_cap_y[slot][frame_idx] : 0;

    /* Motion is expressed visually in world pixels. The renderer positions by
       subtracting anipoints, so visual +X/+Y becomes negative local delta. */
    dx = ClampWorldMarkedAniptDelta(
        dx - WorldMarkedClampedMotion(vx, elapsed, cap_x));
    dy = ClampWorldMarkedAniptDelta(
        dy - WorldMarkedClampedMotion(vy, elapsed, cap_y));
    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
}

/* One sprite instance to paint: a lane frame, optionally its dual copy.
   Collected first, then sorted, so both the on-screen draw and the PNG export
   composite the scene in exactly the same order. */
struct WorldLaneDrawJob {
    int slot;
    int frame_idx;
    int z;
    int order;
    bool dual;
    bool mirror_x;
    bool mirror_y;
};

/* Walk every visible lane and gather its paint jobs back-to-front by per-entry
   Z. Equal Z keeps the legacy order: slot N-1 painted first (back), slot 0 last
   (top); a dual copy paints right after its primary. When `render_info` is
   given, the current frame's resolved mirror flags are recorded into it. */
static void WorldCollectMarkedDrawJobs(WorldMarkedSequenceState &state,
                                       const std::vector<WorldMarkedLane> &lanes,
                                       std::vector<WorldLaneDrawJob> &jobs,
                                       WorldMarkedLaneRenderInfo *render_info)
{
    int n = (int)lanes.size();
    jobs.clear();
    jobs.reserve((size_t)n * 2);

    auto add_frame_jobs = [&](int slot, int frame_idx, int order,
                              bool mirror_x, bool mirror_y) {
        const WorldMarkedLane &lane = lanes[slot];
        int state_slot = lane.delay_slot;
        jobs.push_back({slot, frame_idx,
                        state.frame_z[state_slot][frame_idx],
                        order, false, mirror_x, mirror_y});
        if (state.dual_on[state_slot][frame_idx])
            jobs.push_back({slot, frame_idx,
                            state.dual_z[state_slot][frame_idx],
                            order, true, mirror_x, mirror_y});
    };

    for (int slot = 0; slot < n; slot++) {
        const WorldMarkedLane &lane = lanes[slot];
        int state_slot = lane.delay_slot;
        EnsureWorldMarkedFrameDelays(state, state_slot, (int)lane.frames.size());
        if (!state.lane_visible[state_slot])
            continue;
        if (lane.frame_pos < 0 ||
            lane.frame_pos >= (int)lane.frames.size())
            continue;

        bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
        int order = (slot == 0) ? n - 1 : n - 1 - slot;
        bool base_mirror_x = mirror_flag ? *mirror_flag : false;

        for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
            if (fi == lane.frame_pos)
                continue;
            if (!WorldMarkedEntryHasTimedHold(state, state_slot, fi) ||
                !WorldMarkedEntryVisibleAtTick(state, state_slot, fi, lane.tick))
                continue;
            int mirror_bits = fi < (int)state.frame_mirror[state_slot].size()
                            ? state.frame_mirror[state_slot][fi] : 0;
            bool mirror_x =
                base_mirror_x ^ ((mirror_bits & kWorldFrameMirrorX) != 0);
            bool mirror_y = (mirror_bits & kWorldFrameMirrorY) != 0;
            add_frame_jobs(slot, fi, order, mirror_x, mirror_y);
        }

        int current_mirror_bits =
            lane.frame_pos < (int)state.frame_mirror[state_slot].size()
          ? state.frame_mirror[state_slot][lane.frame_pos] : 0;
        bool current_mirror_x =
            base_mirror_x ^ ((current_mirror_bits & kWorldFrameMirrorX) != 0);
        bool current_mirror_y =
            (current_mirror_bits & kWorldFrameMirrorY) != 0;
        if (render_info) {
            render_info->lane_mirror_x[slot] = current_mirror_x;
            render_info->lane_mirror_y[slot] = current_mirror_y;
        }
        if (WorldMarkedEntryVisibleAtTick(state, state_slot, lane.frame_pos,
                                          lane.tick))
            add_frame_jobs(slot, lane.frame_pos, order,
                           current_mirror_x, current_mirror_y);
    }

    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const WorldLaneDrawJob &a, const WorldLaneDrawJob &b) {
                         if (a.z != b.z) return a.z < b.z;
                         return a.order < b.order;
                     });
}

void WorldDrawMarkedLaneSprites(ImDrawList *dl, WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldCanvasLayout &layout,
                                WorldMarkedLaneRenderInfo &render_info)
{
    if (!dl) return;

    auto draw_instance = [&](int slot, int frame_idx, bool dual,
                             bool mirror_x, bool mirror_y) {
        const WorldMarkedLane &lane = lanes[slot];
        int state_slot = lane.delay_slot;
        if (frame_idx < 0 || frame_idx >= (int)lane.frames.size())
            return;

        const std::vector<int> *pieces = NULL;
        const std::vector<Document*> *piece_docs = NULL;
        if (frame_idx < (int)lane.frame_pieces.size())
            pieces = &lane.frame_pieces[frame_idx];
        if (frame_idx < (int)lane.frame_piece_docs.size())
            piece_docs = &lane.frame_piece_docs[frame_idx];

        std::vector<int> fallback_piece;
        if (!pieces || pieces->empty()) {
            if (!lane.img) return;
            fallback_piece.push_back(lane.frames[frame_idx]);
            pieces = &fallback_piece;
            piece_docs = NULL;
        }

        int local_dx = 0;
        int local_dy = 0;
        WorldMarkedEffectiveLocalDelta(state, state_slot,
                                       (int)lane.frames.size(), frame_idx,
                                       dual, lane.tick,
                                       &local_dx, &local_dy);
        bool *rect_valid = dual ? &render_info.dual_rect_valid[slot]
                                : &render_info.lane_rect_valid[slot];
        ImVec2 *rect_min = dual ? &render_info.dual_rect_min[slot]
                                : &render_info.lane_rect_min[slot];
        ImVec2 *rect_max = dual ? &render_info.dual_rect_max[slot]
                                : &render_info.lane_rect_max[slot];
        bool *bad_y_anchor = dual ? &render_info.dual_bad_y_anchor[slot]
                                  : &render_info.lane_bad_y_anchor[slot];

        Document *frame_doc = (frame_idx < (int)lane.frame_docs.size() &&
                              lane.frame_docs[frame_idx])
                            ? lane.frame_docs[frame_idx] : lane.doc;
        for (size_t pi = 0; pi < pieces->size(); pi++) {
            int piece_idx = (*pieces)[pi];
            Document *pdoc = (piece_docs && pi < piece_docs->size() && (*piece_docs)[pi])
                           ? (*piece_docs)[pi] : frame_doc;
            IMG *img = doc_get_img(pdoc, piece_idx);
            if (!img) continue;
            SDL_Texture *tex = BuildWorldSpriteTexture(pdoc, img,
                                                       WorldMarkedLaneAlpha(slot));
            if (!tex) continue;

            int ax = (int)(short)img->anix + local_dx;
            int ay = (int)(short)img->aniy + local_dy;
            if (WorldBadYAnchor(img, ay))
                *bad_y_anchor = true;
            float spw = img->w * layout.scale;
            float sph = img->h * layout.scale;
            float left = layout.origin_x -
                anipoint_effective(ax, (int)img->w, mirror_x,
                                   g_mirror_convention) * layout.scale;
            float top = layout.origin_y -
                anipoint_effective(ay, (int)img->h, mirror_y,
                                   g_mirror_convention) * layout.scale;
            ImVec2 spos(left, top);
            ImVec2 uv0(mirror_x ? 1.0f : 0.0f,
                       mirror_y ? 1.0f : 0.0f);
            ImVec2 uv1(mirror_x ? 0.0f : 1.0f,
                       mirror_y ? 0.0f : 1.0f);
            dl->AddImage((ImTextureID)(intptr_t)tex,
                         spos, ImVec2(spos.x + spw, spos.y + sph), uv0, uv1);
            if (state.draw_sprite_borders)
                dl->AddRect(spos, ImVec2(spos.x + spw, spos.y + sph),
                            WorldMarkedLaneOutlineColor(slot), 0.0f, 0, 1.0f);

            ImVec2 rmax(spos.x + spw, spos.y + sph);
            if (!*rect_valid) {
                *rect_valid = true;
                *rect_min = spos;
                *rect_max = rmax;
            } else {
                if (spos.x < rect_min->x) rect_min->x = spos.x;
                if (spos.y < rect_min->y) rect_min->y = spos.y;
                if (rmax.x > rect_max->x) rect_max->x = rmax.x;
                if (rmax.y > rect_max->y) rect_max->y = rmax.y;
            }
        }
    };

    std::vector<WorldLaneDrawJob> jobs;
    WorldCollectMarkedDrawJobs(state, lanes, jobs, &render_info);
    for (const WorldLaneDrawJob &job : jobs)
        draw_instance(job.slot, job.frame_idx, job.dual,
                      job.mirror_x, job.mirror_y);
}

/* Composite the same scene WorldDrawMarkedLaneSprites paints, but into a
   world-pixel RGBA buffer instead of a draw list: 1:1 scale, no borders, no
   overlays, index 0 transparent. `out` is resized to world_w * world_h * 4 and
   fully cleared first. Returns the number of sprite instances that landed at
   least one pixel inside the world rect. */
int WorldComposeMarkedSceneRgba(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                int world_w, int world_h,
                                int origin_x, int origin_y,
                                bool use_lane_alpha,
                                std::vector<unsigned char> &out)
{
    out.clear();
    if (world_w <= 0 || world_h <= 0) return 0;
    out.assign((size_t)world_w * (size_t)world_h * 4u, 0);

    std::vector<WorldLaneDrawJob> jobs;
    WorldCollectMarkedDrawJobs(state, lanes, jobs, NULL);

    int drawn = 0;
    for (const WorldLaneDrawJob &job : jobs) {
        const WorldMarkedLane &lane = lanes[job.slot];
        int state_slot = lane.delay_slot;
        int frame_idx = job.frame_idx;
        if (frame_idx < 0 || frame_idx >= (int)lane.frames.size())
            continue;

        const std::vector<int> *pieces = NULL;
        const std::vector<Document*> *piece_docs = NULL;
        if (frame_idx < (int)lane.frame_pieces.size())
            pieces = &lane.frame_pieces[frame_idx];
        if (frame_idx < (int)lane.frame_piece_docs.size())
            piece_docs = &lane.frame_piece_docs[frame_idx];

        std::vector<int> fallback_piece;
        if (!pieces || pieces->empty()) {
            if (!lane.img) continue;
            fallback_piece.push_back(lane.frames[frame_idx]);
            pieces = &fallback_piece;
            piece_docs = NULL;
        }

        int local_dx = 0;
        int local_dy = 0;
        WorldMarkedEffectiveLocalDelta(state, state_slot,
                                       (int)lane.frames.size(), frame_idx,
                                       job.dual, lane.tick,
                                       &local_dx, &local_dy);

        Document *frame_doc = (frame_idx < (int)lane.frame_docs.size() &&
                              lane.frame_docs[frame_idx])
                            ? lane.frame_docs[frame_idx] : lane.doc;
        for (size_t pi = 0; pi < pieces->size(); pi++) {
            int piece_idx = (*pieces)[pi];
            Document *pdoc = (piece_docs && pi < piece_docs->size() && (*piece_docs)[pi])
                           ? (*piece_docs)[pi] : frame_doc;
            IMG *img = doc_get_img(pdoc, piece_idx);
            if (!img) continue;

            /* Same anchor math as the screen draw at scale 1. */
            int ax = (int)(short)img->anix + local_dx;
            int ay = (int)(short)img->aniy + local_dy;
            int left = origin_x - anipoint_effective(ax, (int)img->w,
                                                    job.mirror_x,
                                                    g_mirror_convention);
            int top  = origin_y - anipoint_effective(ay, (int)img->h,
                                                    job.mirror_y,
                                                    g_mirror_convention);
            /* Lane alpha exists to keep overlapping lanes readable while
               editing. An export defaults to opaque so the PNG matches what
               the hardware would actually draw. */
            unsigned char alpha = use_lane_alpha ? WorldMarkedLaneAlpha(job.slot) : 255;
            if (WorldBlitSpriteRgba(pdoc, img, alpha,
                                    job.mirror_x, job.mirror_y, left, top,
                                    out.data(), world_w, world_h) > 0)
                drawn++;
        }
    }
    return drawn;
}

enum WorldBoundaryClass {
    WorldBoundary_Green = 0,
    WorldBoundary_Yellow,
    WorldBoundary_Red
};

struct WorldBoundaryRect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

static ImVec2 WorldBoundaryPointToScreen(const WorldCanvasLayout &layout,
                                         int x, int y)
{
    return ImVec2(layout.pos.x + (float)x * layout.scale,
                  layout.pos.y + (float)y * layout.scale);
}

static WorldBoundaryRect WorldBoundaryRectFromScreen(ImVec2 rect_min,
                                                     ImVec2 rect_max,
                                                     const WorldCanvasLayout &layout)
{
    float scale = layout.scale > 0.0f ? layout.scale : 1.0f;
    WorldBoundaryRect r;
    r.left = (int)std::floor((rect_min.x - layout.pos.x) / scale + 0.0001f);
    r.top = (int)std::floor((rect_min.y - layout.pos.y) / scale + 0.0001f);
    r.right = (int)std::ceil((rect_max.x - layout.pos.x) / scale - 0.0001f) - 1;
    r.bottom = (int)std::ceil((rect_max.y - layout.pos.y) / scale - 0.0001f) - 1;
    return r;
}

static WorldBoundaryClass WorldBoundaryClassify(const WorldBoundaryRect &r,
                                                bool bad_y_anchor)
{
    /* v3.14.0 shipped this as "lanes and dual instances flag a bad Y anchor",
       but the parameter arrived stubbed out with (void) and stayed that way,
       so the badge has never once reflected it. An anipoint word that cannot
       be an anipoint outranks art merely hanging off the playfield edge. */
    if (bad_y_anchor)
        return WorldBoundary_Red;

    if (r.top < 0 || r.bottom > 253 || r.right > 511)
        return WorldBoundary_Red;

    if (r.left >= 0 && r.top >= 0 && r.right <= 399 && r.bottom <= 253)
        return WorldBoundary_Green;

    if (r.right <= 511 && r.bottom <= 253)
        return WorldBoundary_Yellow;

    return WorldBoundary_Red;
}

static ImU32 WorldBoundaryColor(WorldBoundaryClass cls, int alpha)
{
    if (cls == WorldBoundary_Red)
        return IM_COL32(255, 70, 70, alpha);
    if (cls == WorldBoundary_Yellow)
        return IM_COL32(255, 210, 60, alpha);
    return IM_COL32(80, 235, 120, alpha);
}

static const char *WorldBoundaryName(WorldBoundaryClass cls)
{
    if (cls == WorldBoundary_Red) return "RED";
    if (cls == WorldBoundary_Yellow) return "YELLOW";
    return "GREEN";
}

static void WorldDrawBoundaryGuides(ImDrawList *dl,
                                    const WorldCanvasLayout &layout)
{
    if (!dl) return;

    ImVec2 green_min = WorldBoundaryPointToScreen(layout, 0, 0);
    ImVec2 green_max = WorldBoundaryPointToScreen(layout, 400, 254);
    ImVec2 yellow_min = WorldBoundaryPointToScreen(layout, 0, 0);
    ImVec2 yellow_max = WorldBoundaryPointToScreen(layout, 512, 254);
    const float red_pad = 7.0f;
    ImVec2 red_min(yellow_min.x - red_pad, yellow_min.y - red_pad);
    ImVec2 red_max(yellow_max.x + red_pad, yellow_max.y + red_pad);

    dl->AddRect(red_min, red_max,
                WorldBoundaryColor(WorldBoundary_Red, 145),
                0.0f, 0, 1.2f);
    dl->AddRect(yellow_min, yellow_max,
                WorldBoundaryColor(WorldBoundary_Yellow, 190),
                0.0f, 0, 1.4f);
    dl->AddRect(green_min, green_max,
                WorldBoundaryColor(WorldBoundary_Green, 230),
                0.0f, 0, 1.8f);
}

static bool WorldBoundaryInfoForRect(const WorldMarkedLaneRenderInfo &render_info,
                                     const WorldCanvasLayout &layout,
                                     int slot, bool dual,
                                     WorldBoundaryRect *out_rect,
                                     WorldBoundaryClass *out_class)
{
    bool valid = dual ? render_info.dual_rect_valid[slot]
                      : render_info.lane_rect_valid[slot];
    if (!valid) return false;

    ImVec2 mn = dual ? render_info.dual_rect_min[slot]
                     : render_info.lane_rect_min[slot];
    ImVec2 mx = dual ? render_info.dual_rect_max[slot]
                     : render_info.lane_rect_max[slot];
    bool bad_y = dual ? render_info.dual_bad_y_anchor[slot]
                      : render_info.lane_bad_y_anchor[slot];
    WorldBoundaryRect rect = WorldBoundaryRectFromScreen(mn, mx, layout);
    WorldBoundaryClass cls = WorldBoundaryClassify(rect, bad_y);
    if (out_rect) *out_rect = rect;
    if (out_class) *out_class = cls;
    return true;
}

static void WorldDrawBoundaryStatus(ImDrawList *dl,
                                    const WorldMarkedSequenceState &state,
                                    const std::vector<WorldMarkedLane> &lanes,
                                    const WorldMarkedLaneRenderInfo &render_info,
                                    const WorldCanvasLayout &layout,
                                    ImVec2 world_pos, float world_width)
{
    if (!dl || !state.show_boundary_overlay) return;

    int green_count = 0;
    int yellow_count = 0;
    int red_count = 0;
    int first_issue_slot = -1;
    bool first_issue_dual = false;
    WorldBoundaryRect first_issue_rect = {};
    WorldBoundaryClass first_issue_class = WorldBoundary_Green;

    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        for (int pass = 0; pass < 2; pass++) {
            bool dual = pass != 0;
            WorldBoundaryRect rect;
            WorldBoundaryClass cls;
            if (!WorldBoundaryInfoForRect(render_info, layout, slot, dual,
                                          &rect, &cls))
                continue;
            if (cls == WorldBoundary_Red) red_count++;
            else if (cls == WorldBoundary_Yellow) yellow_count++;
            else green_count++;

            if (cls != WorldBoundary_Green &&
                (first_issue_slot < 0 || cls > first_issue_class)) {
                first_issue_slot = slot;
                first_issue_dual = dual;
                first_issue_rect = rect;
                first_issue_class = cls;
            }
        }
    }

    char label[256];
    if (first_issue_slot >= 0) {
        snprintf(label, sizeof(label),
                 "Bounds: %s row %d%s L%d T%d R%d B%d   ok=%d warn=%d bad=%d",
                 WorldBoundaryName(first_issue_class),
                 first_issue_slot + 1,
                 first_issue_dual ? " dual" : "",
                 first_issue_rect.left, first_issue_rect.top,
                 first_issue_rect.right, first_issue_rect.bottom,
                 green_count, yellow_count, red_count);
    } else {
        snprintf(label, sizeof(label),
                 "Bounds: GREEN visible 0..399 x 0..253   DMA X clip max 511");
    }

    ImU32 col = first_issue_slot >= 0
              ? WorldBoundaryColor(first_issue_class, 255)
              : WorldBoundaryColor(WorldBoundary_Green, 255);
    ImVec2 label_pos(world_pos.x, world_pos.y + 20.0f);
    ImVec2 label_sz = ImGui::CalcTextSize(label);
    float label_w = label_sz.x + 8.0f;
    if (label_w > world_width) label_w = world_width;
    dl->AddRectFilled(label_pos,
                      ImVec2(label_pos.x + label_w, label_pos.y + 18.0f),
                      IM_COL32(0, 0, 0, 190));
    dl->PushClipRect(label_pos,
                     ImVec2(label_pos.x + label_w, label_pos.y + 18.0f), true);
    dl->AddText(ImVec2(label_pos.x + 4.0f, label_pos.y + 2.0f),
                col, label);
    dl->PopClipRect();
}

static void WorldDrawMarkedBoundaryOverlay(ImDrawList *dl,
                                           const WorldMarkedSequenceState &state,
                                           const std::vector<WorldMarkedLane> &lanes,
                                           const WorldMarkedLaneRenderInfo &render_info,
                                           const WorldCanvasLayout &layout)
{
    if (!dl || !state.show_boundary_overlay) return;

    WorldDrawBoundaryGuides(dl, layout);
    if (!state.draw_sprite_borders) return;

    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        for (int pass = 0; pass < 2; pass++) {
            bool dual = pass != 0;
            WorldBoundaryRect rect;
            WorldBoundaryClass cls;
            if (!WorldBoundaryInfoForRect(render_info, layout, slot, dual,
                                          &rect, &cls))
                continue;
            if (cls == WorldBoundary_Green)
                continue;

            ImVec2 mn = dual ? render_info.dual_rect_min[slot]
                             : render_info.lane_rect_min[slot];
            ImVec2 mx = dual ? render_info.dual_rect_max[slot]
                             : render_info.lane_rect_max[slot];
            float thick = cls == WorldBoundary_Red ? 3.0f :
                          cls == WorldBoundary_Yellow ? 2.4f : 1.6f;
            dl->AddRect(ImVec2(mn.x - 1.0f, mn.y - 1.0f),
                        ImVec2(mx.x + 1.0f, mx.y + 1.0f),
                        IM_COL32(0, 0, 0, 220), 0.0f, 0, thick + 1.2f);
            dl->AddRect(mn, mx, WorldBoundaryColor(cls, 255),
                        0.0f, 0, thick);

            if (cls != WorldBoundary_Green) {
                char tag[64];
                snprintf(tag, sizeof(tag), "%s R%d%s",
                         WorldBoundaryName(cls), slot + 1,
                         dual ? "D" : "");
                ImVec2 tag_sz = ImGui::CalcTextSize(tag);
                ImVec2 tag_pos(mn.x, mx.y + 2.0f);
                dl->AddRectFilled(ImVec2(tag_pos.x - 2.0f, tag_pos.y - 1.0f),
                                  ImVec2(tag_pos.x + tag_sz.x + 2.0f,
                                         tag_pos.y + tag_sz.y + 1.0f),
                                  IM_COL32(0, 0, 0, 190));
                dl->AddText(tag_pos, WorldBoundaryColor(cls, 255), tag);
            }
        }
    }
}

void WorldDrawMarkedLaneTags(ImDrawList *dl,
                             const std::vector<WorldMarkedLane> &lanes,
                             const WorldMarkedLaneRenderInfo &render_info,
                             ImVec2 world_pos)
{
    if (!dl) return;
    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        if (!render_info.lane_rect_valid[slot]) continue;
        const WorldMarkedLane &lane = lanes[slot];
        const char *doc_name = !lane.label.empty()
                             ? lane.label.c_str()
                             : (lane.doc && lane.doc->fname_s[0]
                                ? lane.doc->fname_s : "Untitled");
        std::string frame_name = (lane.frame_pos >= 0 &&
                                  lane.frame_pos < (int)lane.frame_labels.size() &&
                                  !lane.frame_labels[lane.frame_pos].empty())
                               ? lane.frame_labels[lane.frame_pos]
                               : (lane.img ? img_name_string(lane.img) : std::string());
        char tag[256];
        snprintf(tag, sizeof(tag), "%s:%s", doc_name, frame_name.c_str());
        ImVec2 tag_sz = ImGui::CalcTextSize(tag);
        ImVec2 tag_pos(render_info.lane_rect_min[slot].x,
                       render_info.lane_rect_min[slot].y - tag_sz.y - 3.0f);
        if (tag_pos.y < world_pos.y + 1.0f) tag_pos.y = world_pos.y + 1.0f;
        dl->AddRectFilled(ImVec2(tag_pos.x - 2.0f, tag_pos.y - 1.0f),
                          ImVec2(tag_pos.x + tag_sz.x + 2.0f,
                                 tag_pos.y + tag_sz.y + 1.0f),
                          IM_COL32(0, 0, 0, 190));
        dl->AddText(tag_pos, WorldMarkedLaneOutlineColor(slot), tag);
    }
}

void WorldDrawMarkedLaneStatus(ImDrawList *dl, WorldMarkedSequenceState &state,
                               const std::vector<WorldMarkedLane> &lanes,
                               const WorldMarkedLaneRenderInfo &render_info,
                               const WorldCanvasLayout &layout,
                               ImVec2 world_pos, float world_width)
{
    if (!dl) return;

    std::string label = "Marked rows: ";
    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        const WorldMarkedLane &lane = lanes[slot];
        if (!lane.img) continue;
        const char *doc_name = !lane.label.empty()
                             ? lane.label.c_str()
                             : (lane.doc->fname_s[0] ? lane.doc->fname_s : "Untitled");
        bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
        char y_buf[48] = "";
        if (render_info.lane_rect_valid[slot]) {
            WorldBoundaryRect rect =
                WorldBoundaryRectFromScreen(render_info.lane_rect_min[slot],
                                            render_info.lane_rect_max[slot],
                                            layout);
            snprintf(y_buf, sizeof(y_buf), " y=%d..%d",
                     rect.top, rect.bottom);
        }
        char stop_buf[32] = "";
        int stop_tick = (lane.delay_slot >= 0 &&
                         lane.delay_slot < kWorldMarkedMaxTabs)
                      ? state.stop_tick[lane.delay_slot] : 0;
        if (stop_tick > 0) {
            snprintf(stop_buf, sizeof(stop_buf), " stop@%d%s",
                     stop_tick, lane.tick >= stop_tick ? "*" : "");
        }
        char part[320];
        snprintf(part, sizeof(part), "%s[%d] %s:%s %d/%d tick=%d%s%s%s%s",
                 slot == 0 ? "" : " + ",
                 lane.doc_idx, doc_name, img_name_string(lane.img).c_str(),
                 lane.frame_pos + 1, (int)lane.frames.size(), lane.tick,
                 y_buf,
                 (mirror_flag && *mirror_flag) ? " mirror" : "",
                 state.hold_end[lane.delay_slot] ? " hold" : "",
                 stop_buf);
        label += part;
    }
    char fps_buf[32];
    snprintf(fps_buf, sizeof(fps_buf), "   %.2f Hz", kMk2TickHz);
    label += fps_buf;

    ImVec2 label_sz = ImGui::CalcTextSize(label.c_str());
    float label_w = label_sz.x + 8.0f;
    if (label_w > world_width) label_w = world_width;
    dl->AddRectFilled(world_pos,
                      ImVec2(world_pos.x + label_w, world_pos.y + 18),
                      IM_COL32(0, 0, 0, 180));
    dl->PushClipRect(world_pos,
                     ImVec2(world_pos.x + label_w, world_pos.y + 18), true);
    dl->AddText(ImVec2(world_pos.x + 4, world_pos.y + 2),
                IM_COL32(220, 220, 220, 255), label.c_str());
    dl->PopClipRect();
}

WorldMarkedSceneResult WorldDrawMarkedScene(WorldMarkedSequenceState &state,
                                            const WorldViewState &world,
                                            const std::vector<WorldMarkedLane> &lanes,
                                            ImVec2 avail,
                                            ImVec2 img_pos)
{
    WorldMarkedSceneResult result = {};
    result.layout =
        ComputeWorldCanvasLayout(avail, img_pos, world.w, world.h,
                                 world.origin_x, world.origin_y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 world_pos = result.layout.pos;
    float world_width = result.layout.width;
    float world_height = result.layout.height;
    float origin_x = result.layout.origin_x;
    float origin_y = result.layout.origin_y;

    dl->AddRectFilled(world_pos,
                      ImVec2(world_pos.x + world_width,
                             world_pos.y + world_height),
                      IM_COL32(0, 0, 0, 255));
    /* Straight onto the cleared canvas, so the stage replaces the flat black
       and every guide, figure and sprite below still draws over it. */
    WorldDrawReferenceBackground(dl, result.layout, world);
    dl->AddLine(ImVec2(origin_x - 8, origin_y),
                ImVec2(origin_x + 8, origin_y),
                IM_COL32(120, 120, 120, 255));
    dl->AddLine(ImVec2(origin_x, origin_y - 8),
                ImVec2(origin_x, origin_y + 8),
                IM_COL32(120, 120, 120, 255));

    /* Behind the lanes so an effect that lands on the figure reads as landing
       on it, not behind it. */
    WorldDrawReferenceFigure(dl, result.layout, world);

    WorldDrawMarkedLaneSprites(dl, state, lanes, result.layout,
                               result.render_info);
    dl->AddCircle(ImVec2(origin_x, origin_y), 4.0f,
                  IM_COL32(255, 200, 0, 255), 0, 1.5f);
    WorldDrawMarkedBoundaryOverlay(dl, state, lanes, result.render_info,
                                   result.layout);
    if (state.draw_sprite_borders) {
        WorldDrawMarkedLaneTags(dl, lanes, result.render_info, world_pos);
        WorldDrawMarkedLaneStatus(dl, state, lanes, result.render_info,
                                  result.layout, world_pos, world_width);
    }
    WorldDrawBoundaryStatus(dl, state, lanes, result.render_info,
                            result.layout, world_pos, world_width);

    int hidden_lanes = 0;
    for (size_t i = 0; i < lanes.size(); i++) {
        int s = lanes[i].delay_slot;
        if (s >= 0 && s < kWorldMarkedMaxTabs && !state.lane_visible[s])
            hidden_lanes++;
    }
    result.panel_layout =
        ComputeWorldMarkedPanelLayout(avail, img_pos, (int)lanes.size(),
                                      hidden_lanes);
    if (g_world_marked_panel_docked) {
        result.panel_layout.pos = ImVec2(-10000.0f, -10000.0f);
        result.panel_layout.width = 0.0f;
        result.panel_layout.height = 0.0f;
    }
    WorldHandleMarkedLaneDrag(dl, state, lanes, result.render_info,
                              result.layout, result.panel_layout,
                              img_pos, ImVec2(img_pos.x + avail.x,
                                              img_pos.y + avail.y));
    return result;
}

/* Off by default. The Subframes/Tick/Now/Swap strip sits directly above the
   thumbnails and only appears when the selected sprite happens to have child
   subframes, so it turns up unbidden in the middle of an unrelated edit and
   pushes the strip down. It is a real tool, just a rare one -- View > Show
   Subframe Swap Tool turns it back on. */
bool g_world_show_subframe_tool = false;

/* ---- Ctrl+click frame selection ----------------------------------------
   A set of (row, entry) pairs picked out of the thumbnail strips, across any
   number of rows, so a run can be gathered and dropped into the row being
   built. Drag-and-drop already MOVES one frame between rows; this is the
   copy, and the multiple.

   Held as a plain vector because it is tiny and needs a stable order: entries
   are copied in row-then-index order, not click order, so picking the same
   frames in a different sequence still yields the same run. */
struct WorldFrameSel {
    int slot;
    int frame_idx;
};
static std::vector<WorldFrameSel> g_world_frame_sel;

/* Defined further down, next to the move it is modelled on. */
bool WorldMarkedCopySelectionToSlot(WorldMarkedSequenceState &state,
                                    int dst_slot, int *out_copied,
                                    int *out_skipped);

static bool WorldFrameSelContains(int slot, int frame_idx)
{
    for (const WorldFrameSel &e : g_world_frame_sel)
        if (e.slot == slot && e.frame_idx == frame_idx) return true;
    return false;
}

static void WorldFrameSelToggle(int slot, int frame_idx)
{
    for (size_t i = 0; i < g_world_frame_sel.size(); i++) {
        if (g_world_frame_sel[i].slot == slot &&
            g_world_frame_sel[i].frame_idx == frame_idx) {
            g_world_frame_sel.erase(g_world_frame_sel.begin() + (long)i);
            return;
        }
    }
    WorldFrameSel e;
    e.slot = slot;
    e.frame_idx = frame_idx;
    g_world_frame_sel.push_back(e);
}

/* Every entry a row loses or gains shifts the indices the selection is holding,
   so anything that rebuilds a row drops it rather than letting it point at
   whatever moved into place. */
static void WorldFrameSelClear(void)
{
    g_world_frame_sel.clear();
}

/* The tick rate, as a fact rather than a control. MK2 refreshes at
   54.7068 Hz and there is no version of the machine that does not, so the
   only thing this has to do is stop people looking for a speed knob here
   and reach for Ticks/frame instead. */
static void WorldDrawTickRateReadout(void)
{
    ImGui::TextDisabled("%.2f Hz", kMk2TickHz);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "MK2's hardware tick rate, and not adjustable: one tick is one\n"
            "display refresh, measured from MAME's mk2 driver at\n"
            "%.4f Hz (18.279 ms).\n\n"
            "This used to be a 1..60 slider, which is how lanes got authored\n"
            "against a made-up rate and shipped several times too slow. Set\n"
            "speed with Ticks/frame -- that is the number the export writes.",
            kMk2TickHz);
}

/* An InputInt that hands its value over only when the edit FINISHES -- Enter,
   Tab, or clicking away -- instead of on every keystroke.

   Committing per keystroke is what made the hold boxes fight back. Deleting
   the "2" from "12" hands the commit a 1; ClampTimelineHold accepts it, the
   row is retimed to 1, its scheduled windows are re-laid, and the tick clock
   is rewound. The box is then rewritten from that new value underneath the
   caret, so the "7" meant for "17" lands somewhere else and the playhead has
   moved as well. Half-typed numbers are not edits, and nothing outside the
   widget should see them.

   Deliberately does not consult IsItemActive: an InputInt with step buttons is
   a GROUP, and a group's LastItemData.ID is 0, so IsItemActive is always false
   for one. IsItemDeactivatedAfterEdit is forwarded by EndGroup and is true for
   both shapes -- and it only fires when something really was edited, so a
   click in and straight back out commits nothing.

   One pending value is enough: ImGui has at most one active item. */
static bool WorldDeferredIntInput(const char *id, int shown, float width,
                                  int step, int *out_value)
{
    static int s_pending = 0;
    ImGui::SetNextItemWidth(width);
    int edited = shown;
    bool changed = ImGui::InputInt(id, &edited, step, step);
    if (ImGui::IsItemActivated()) s_pending = shown;
    if (changed) s_pending = edited;
    if (!ImGui::IsItemDeactivatedAfterEdit()) return false;
    if (out_value) *out_value = s_pending;
    return true;
}

/* Ticks per frame, sitting next to the transport in both preview headers.
   This is the control that actually sets playback speed: the tick rate is
   hardware (54.7 Hz) and stays put, while the hold is authoring, and the value
   here is literally what goes into the ASM — a hold of 4 is `.word 4` in a
   script entry, or the frame label repeated four times in an animation. The
   resulting frame rate is spelled out so the number can be judged by eye. */
static void WorldDrawTickHoldControl(WorldMarkedSequenceState &state,
                                     const char *id_suffix)
{
    char id[64];
    snprintf(id, sizeof(id), "##%s_hold", id_suffix);
    ImGui::TextDisabled("Ticks/frame");
    ImGui::SameLine(0.0f, 4.0f);
    int hold = 0;
    if (WorldDeferredIntInput(id, state.default_hold, 76.0f, 1, &hold)) {
        state.default_hold = ClampTimelineHold(hold);
        WorldMarkedApplyUniformHold(state, state.default_hold);
    }
    if (ImGui::IsItemHovered()) {
        int shown = ClampTimelineHold(state.default_hold);
        ImGui::SetTooltip(
            "Hold every frame this many ticks, on every VISIBLE row. This is\n"
            "the sleep value the animation runner is given -- MKUTIL.ASM\n"
            "animate_a9 holds one .long row for this many ticks -- not a\n"
            "preview-only speed.\n\n"
            "MK2 runs %.4f ticks a second, so a hold of %d is\n"
            "%.4f / %d = %.2f rows a second.\n\n"
            "Rows follow this until one is given its own T/f, which then keeps\n"
            "it; \"All\" overrides that. Hidden rows and the dummy body are left\n"
            "alone -- show a row to retime it.",
            kMk2TickHz, shown, kMk2TickHz, shown, kMk2TickHz / (float)shown);
    }

    /* The same number from the other end. Ticks are what the game holds and
       what the export writes, but nobody authors in 54.7ths of a second, and
       guessing the rate is how a lane authored "at 12 fps" shipped running at
       5 game ticks a frame -- 10.9 fps -- with the mismatch baked in. Type the
       rate, get the nearest whole tick count, and see what it actually is. */
    int hold_now = ClampTimelineHold(state.default_hold);
    float derived = kMk2TickHz / (float)hold_now;
    ImGui::SameLine(0.0f, 8.0f);
    ImGui::TextDisabled("fps");
    ImGui::SameLine(0.0f, 4.0f);
    char fps_id[64];
    snprintf(fps_id, sizeof(fps_id), "##%s_animfps", id_suffix);
    ImGui::SetNextItemWidth(58.0f);
    float want_fps = derived;
    if (ImGui::InputFloat(fps_id, &want_fps, 0.0f, 0.0f, "%.1f",
                          ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (want_fps > 0.05f) {
            int want_hold = (int)(kMk2TickHz / want_fps + 0.5f);
            state.default_hold = ClampTimelineHold(want_hold);
            WorldMarkedApplyUniformHold(state, state.default_hold);
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Frame rate, derived: tick rate / ticks-per-frame. Type one and the\n"
            "hold snaps to the nearest whole tick, because a frame cannot last\n"
            "part of a tick -- at %.1f Hz the reachable rates are %.1f, %.1f,\n"
            "%.1f, %.1f, %.1f, %.1f... Asking for 12 gives a hold of 4, which is\n"
            "%.1f fps. That is the number the game will run.",
            kMk2TickHz, kMk2TickHz, kMk2TickHz / 2.0f, kMk2TickHz / 3.0f,
            kMk2TickHz / 4.0f, kMk2TickHz / 5.0f, kMk2TickHz / 6.0f,
            kMk2TickHz / 4.0f);

    /* Which rows this box does NOT reach, and the one action that pulls the
       pinned ones back. Without the count, a global that visibly does nothing
       to part of the scene reads as broken rather than as rows that were
       deliberately pinned or parked.

       Pinned and hidden are counted apart because only one of them is
       something "All" can undo. */
    int own_rows = 0, hidden_rows = 0;
    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++) {
        if (slot == kWorldDummyDecapSlot) continue;
        if (state.sequence_frames[slot].empty()) continue;
        if (!state.lane_visible[slot]) hidden_rows++;
        else if (state.slot_hold_custom[slot]) own_rows++;
    }
    if (own_rows > 0 || hidden_rows > 0) {
        ImGui::SameLine(0.0f, 8.0f);
        if (own_rows > 0) {
            char all_id[64];
            snprintf(all_id, sizeof(all_id), "All##%s_hold_all", id_suffix);
            if (ImGui::SmallButton(all_id))
                WorldMarkedApplyUniformHold(state, state.default_hold, true);
        } else {
            ImGui::TextDisabled("(%d hidden)", hidden_rows);
        }
        if (ImGui::IsItemHovered()) {
            char note[320];
            int n = 0;
            n += snprintf(note + n, sizeof(note) - n,
                          "This hold reaches every visible row that is not pinned.\n\n");
            if (own_rows > 0)
                n += snprintf(note + n, sizeof(note) - n,
                              "%d pinned with its own T/f. Click to apply %d to\n"
                              "every visible row and put them back on the global.\n",
                              own_rows, ClampTimelineHold(state.default_hold));
            if (hidden_rows > 0 && n < (int)sizeof(note))
                snprintf(note + n, sizeof(note) - n,
                         "%d hidden, and left alone -- a hidden row shows no T/f,\n"
                         "so retiming it would change timing you cannot see. Show\n"
                         "the row to retime it. \"All\" does not override this.\n",
                         hidden_rows);
            ImGui::SetTooltip("%s", note);
        }
    }
}

/* One grouped popup, opened from a SmallButton. The header used to run two
   dozen widgets down a single SameLine chain -- transport, five overlays,
   four ASM buttons, four file buttons, the dummy body -- and picking anything
   out of it meant reading the whole line. Playback stays on the strip because
   it is touched every few seconds; everything else groups by what it acts
   on. */
static bool WorldHeaderMenu(const char *label, const char *popup_id,
                            const char *tooltip)
{
    if (ImGui::SmallButton(label))
        ImGui::OpenPopup(popup_id);
    if (tooltip && ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", tooltip);
    return ImGui::BeginPopup(popup_id);
}

/* Vertical rule between header groups. */
static void WorldHeaderDivider(void)
{
    ImGui::SameLine();
    ImGui::TextDisabled("|");
    ImGui::SameLine();
}

/* The reference figure's placement fields. Lives in its own menu now; it used
   to be a right-click popup on the checkbox, which nothing advertised. */
static void WorldDrawReferenceConfig(void)
{
    ImGui::TextDisabled("Reference figure (proportioned outline, not art)");
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Width##ref_w", &g_world_state.ref_w);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Height##ref_h", &g_world_state.ref_h);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Feet dX##ref_dx", &g_world_state.ref_dx);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Feet dY##ref_dy", &g_world_state.ref_dy);
    ImGui::Checkbox("Mirror##ref_mirror", &g_world_state.ref_mirror);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Flip the figure so both facings are checkable\n"
                          "against the same effect placement.");
    if (ImGui::Button("Feet to Floor##ref_floor")) {
        g_world_state.ref_dx = 0;
        g_world_state.ref_dy = g_world_state.h - g_world_state.origin_y;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stand the figure on the bottom of the playfield.\n"
                          "Use after moving the World origin.");
    if (g_world_state.ref_w < 4) g_world_state.ref_w = 4;
    if (g_world_state.ref_h < 8) g_world_state.ref_h = 8;
    if (g_world_state.ref_w > 400) g_world_state.ref_w = 400;
    if (g_world_state.ref_h > 254) g_world_state.ref_h = 254;
    ImGui::TextDisabled("Feet dX/dY place the figure's feet centre relative\n"
                        "to the shared anchor, in world pixels.");
}

/* Stage-background placement. `action` carries the load request back out: the
   file dialog must not open while this popup still owns the ID stack. */
static void WorldDrawBackgroundConfig(WorldMarkedPanelAction &action)
{
    BddBackground &bg = WorldBackground();
    WorldViewState &world = g_world_state;

    ImGui::TextDisabled("Reference background (view only -- never saved)");
    if (ImGui::Button("Load BDD...##world_bg_load")) {
        action.request_load_bg = true;
        /* Dismiss this popup before the file dialog opens. Leaving it up means
           a modal is opened while a popup still owns the ID stack, which is how
           a modal ends up open-but-never-drawn -- invisible and blocking. From
           a submenu this closes the whole chain. */
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Pick a stage .BDD. Its .BDB sibling is loaded with it --\n"
                          "the BDD holds the pixels, the BDB every placement.");
    if (bg.loaded) {
        ImGui::SameLine();
        if (ImGui::Button("Clear##world_bg_clear")) {
            BddBgFree(&bg);
            world.bg_enabled = false;
            world.bg_module = -1;
        }

        ImGui::Separator();
        ImGui::Text("%s  %dx%d world, %d objects",
                    bg.stage_name.c_str(), bg.world_w, bg.world_h,
                    bg.object_count);
        if (bg.skipped_objects > 0)
            ImGui::TextDisabled("%d placement(s) skipped -- missing image or palette",
                                bg.skipped_objects);

        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("X##world_bg_x", &world.bg_x);
        ImGui::SetNextItemWidth(110.0f);
        ImGui::InputInt("Y##world_bg_y", &world.bg_y);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("World coordinates drawn at the canvas's top-left.\n"
                              "The stage is far bigger than the playfield, so this\n"
                              "is a camera onto it.");
        ImGui::SetNextItemWidth(110.0f);
        ImGui::SliderInt("Alpha##world_bg_alpha", &world.bg_alpha, 32, 255);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Fade the stage back when it competes with the sprites.");

        /* Planes, not layers: each BDB module is one parallax plane, and
           jumping between them is how you find the band you want behind the
           animation without hunting with the X/Y fields. */
        if (!bg.modules.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("Planes (BDB modules)");
            for (size_t m = 0; m < bg.modules.size(); m++) {
                const BddBgModule &mod = bg.modules[m];
                char label[128];
                snprintf(label, sizeof(label), "%s  (%d obj)##world_bg_mod%d",
                         mod.name.c_str(), mod.object_count, (int)m);
                bool selected = (world.bg_module == (int)m);
                if (ImGui::RadioButton(label, selected))
                    WorldBgSnapToModule(world, bg, (int)m);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Centre this plane in the playfield with its\n"
                                      "bottom edge on the floor line (y=%d).\n"
                                      "World rect %d,%d .. %d,%d",
                                      world.floor_y, mod.x1, mod.y1,
                                      mod.x2, mod.y2);
            }
        }

        ImGui::Separator();
        ImGui::TextDisabled(
            "Placement is manual on purpose: real parallax rates,\n"
            "per-plane offsets and draw order live in BGND.ASM, not\n"
            "in the BDD/BDB, so this cannot reproduce the game camera.\n"
            "A stage's floor is a runtime layer and is not in these\n"
            "files either -- expect bare canvas under the scenery.");
    } else if (!bg.error.empty()) {
        ImGui::Separator();
        ImGui::TextWrapped("%s", bg.error.c_str());
    }
}

WorldMarkedPanelAction WorldDrawMarkedPanelHeader(WorldMarkedSequenceState &state,
                                                  const std::vector<WorldMarkedLane> &lanes,
                                                  bool dummy_decap_missing,
                                                  IMG *selected_img,
                                                  int active_doc_idx)
{
    WorldMarkedPanelAction action = {};

    /* ---- Transport: the only controls that stay on the strip ---- */
    ImGui::Text("Frame Sequence");
    ImGui::SameLine();
    if (ImGui::SmallButton(state.paused
            ? (g_icon_font_loaded ? ICON_PLAY "##world_marked_pause"
                                  : ICON_PLAY_TXT "##world_marked_pause")
            : (g_icon_font_loaded ? ICON_PAUSE "##world_marked_pause"
                                  : ICON_PAUSE_TXT "##world_marked_pause"))) {
        /* A scene where nothing loops parks itself on its last tick. Resuming
           from there would sit still and re-pause on the next frame, so Play
           starts it over instead of being a button that does nothing. */
        bool ended = state.paused && state.preview_end_tick > 0 &&
                     state.frame >= state.preview_end_tick;
        state.paused = !state.paused;
        if (ended) WorldMarkedRestart(state);
    }
    if (ImGui::IsItemHovered() && state.preview_end_tick > 0 &&
        state.frame >= state.preview_end_tick)
        ImGui::SetTooltip("Every row has finished at tick %d and nothing here\n"
                          "loops. Play starts the scene over.",
                          state.preview_end_tick);
    ImGui::SameLine();
    if (ImGui::SmallButton(g_icon_font_loaded ? ICON_REFRESH "##world_marked_restart"
                                              : ICON_REFRESH_TXT "##world_marked_restart"))
        WorldMarkedRestart(state);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restart: every marked row plays again from frame 1.");
    ImGui::SameLine();
    ImGui::TextDisabled("Tick");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    int goto_tick = state.frame;
    if (ImGui::InputInt("##world_marked_goto_tick", &goto_tick, 0, 0))
        WorldMarkedSetTick(state, goto_tick);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Jump straight to this exact tick (pauses playback).");
    ImGui::SameLine();
    WorldDrawTickHoldControl(state, "world_marked_panel");
    ImGui::SameLine();
    /* Was a 1..60 "Tick Hz" slider, and before that one labelled "FPS" --
       which is how a 12 typed here came to mean "12 frames a second" to
       everyone who used it, when it was setting the tick clock. The rate is
       hardware, so it is a readout now and Ticks/frame is the only knob. */
    WorldDrawTickRateReadout();

    /* ---- Second row -------------------------------------------------------
       The strip had grown to transport, timing, a selection group, four menus
       and the project name on ONE line, which runs off the side of a docked
       panel long before the menus are reached. Everything that acts on the
       scene starts again at the left here, under the clock it reads. No
       SameLine above: the line break IS the separator, which is why the
       divider that used to sit before Overlays is gone. */

    /* Only on screen while frames are picked. The count is the point: a
       selection spread over rows that are scrolled apart is otherwise
       invisible, and a Copy button with no idea how much would be a button
       nobody trusts. */
    if (!g_world_frame_sel.empty()) {
        int picked = (int)g_world_frame_sel.size();
        int dst = state.active_slot;
        bool dst_ok = dst >= 0 && dst < kWorldMarkedMaxTabs &&
                      dst != kWorldDummyDecapSlot;
        ImGui::TextColored(ImVec4(0.35f, 0.86f, 1.0f, 1.0f), "%d picked", picked);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%d frame%s ctrl+clicked. They copy in row order,\n"
                              "not the order you clicked them.", picked,
                              picked == 1 ? "" : "s");
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::BeginDisabled(!dst_ok);
        if (ImGui::SmallButton("Copy to Active##world_sel_copy")) {
            int copied = 0, skipped = 0;
            if (WorldMarkedCopySelectionToSlot(state, dst, &copied, &skipped)) {
                char tail[96];
                tail[0] = 0;
                if (skipped > 0)
                    snprintf(tail, sizeof(tail),
                             " %d skipped: composite entries cannot cross files.",
                             skipped);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Copied %d frame%s to row %d.%s", copied,
                         copied == 1 ? "" : "s", dst + 1, tail);
                g_restore_msg_timer = 4.0f;
                WorldFrameSelClear();
            }
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(dst_ok
                ? "Append the picked frames to the row holding [KEYS] (row %d),\n"
                  "carrying their offsets, flips, Z and hold. The originals stay\n"
                  "put -- drag a frame instead to move it."
                : "No row holds [KEYS]. Click a row's frame first to make it\n"
                  "the active row.", dst + 1);
        ImGui::SameLine(0.0f, 4.0f);
        if (ImGui::SmallButton("X##world_sel_clear"))
            WorldFrameSelClear();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drop the picked frames.");
        /* Keep the menus on this same second row rather than starting a
           third one that only exists while something is picked. */
        WorldHeaderDivider();
    }

    /* ---- File: the scene in and out, and pixels out ----
       First on the row because it is the one people arrive looking for.
       Load Project is how a session starts, and hunting for it behind two
       menus about drawing and codegen was backwards.

       Was called "Export...", which described half of what is in here -- Load
       Project reads a file rather than writing one, and nobody looks for that
       under Export. */
    if (WorldHeaderMenu(g_icon_font_loaded ? ICON_FOLDER "##world_menu_export"
                                           : "File##world_menu_export",
                        "##world_export_popup",
                        "FILE\n\n"
                        "PNG stills and sequences, and saving or loading the\n"
                        "World View project file.")) {
        if (ImGui::MenuItem("Save PNG...##world_marked_save_png"))
            action.request_save_png = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Write the composited scene at this tick -- every visible lane,\n"
                              "in draw order -- to a PNG.");
        if (ImGui::MenuItem("Save PNG Sequence...##world_marked_save_png_seq"))
            action.request_save_png_seq = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Write one PNG per tick across the whole sequence,\n"
                              "numbered <name>_0000.PNG onward.");
        ImGui::Separator();
        if (ImGui::MenuItem("Save Project...##world_marked_save_project"))
            action.request_save_project = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the World View layout, slots, timing, offsets, marks,\n"
                              "and source file links.");
        if (ImGui::MenuItem("Load Project...##world_marked_load_project"))
            action.request_load_project = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restore a World View project exactly as it was saved.\n"
                              "Closes what is open first -- a project is a whole\n"
                              "workspace, not an overlay.");
        if (ImGui::MenuItem("Append Project...##world_marked_append_project"))
            action.request_append_project = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Add another project's rows to the scene already open,\n"
                              "into whatever rows are free. Its IMGs are opened\n"
                              "alongside the current tabs, and one already open is\n"
                              "reused rather than opened twice.\n\n"
                              "Only the rows come across: the origin, canvas size,\n"
                              "tick clock and global hold stay as they are here.\n"
                              "Two halves of a fight saved separately can be\n"
                              "watched together this way.");
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    /* ---- Overlays: everything in the scene that is not a marked row ---- */
    bool lanes_marked = state.dummy_decap_body || !state.split_lanes.empty();
    if (WorldHeaderMenu(g_icon_font_loaded
                            ? (lanes_marked ? ICON_LAYERS "*##world_menu_overlays"
                                            : ICON_LAYERS "##world_menu_overlays")
                            : (lanes_marked ? "Overlays *##world_menu_overlays"
                                            : "Overlays##world_menu_overlays"),
                        "##world_overlays_popup",
                        "OVERLAYS\n\n"
                        "Sprite borders, TV-safe bounds, the reference figure,\n"
                        "the stage background, anchor linking, the dummy\n"
                        "fatality body, and clearing split rows.\n\n"
                        "* means the dummy body or a split row is in the scene.\n"
                        "Row order, timing and deletion live on each row.")) {
        ImGui::Checkbox("Borders##world_marked_borders", &state.draw_sprite_borders);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw colored sprite bounds in World View.");
        ImGui::Checkbox("Bounds##world_marked_bounds", &state.show_boundary_overlay);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw TV-safe World View guides: green is 0..399 x 0..253,\n"
                              "yellow extends to DMA X 511 while vertically safe,\n"
                              "red is outside those limits.");

        ImGui::Separator();
        ImGui::Checkbox("Reference##world_reference", &g_world_state.show_reference);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw a standing-fighter reference figure at the shared\n"
                              "anchor, so \"will this effect land on the victim?\" is a\n"
                              "look instead of a calculation.");
        if (ImGui::BeginMenu("Reference figure...##world_reference_cfg")) {
            WorldDrawReferenceConfig();
            ImGui::EndMenu();
        }

        /* Deliberately never disabled: the submenu is the only door to the
           load dialog, and a disabled item takes no clicks at all. */
        BddBackground &bg = WorldBackground();
        if (ImGui::Checkbox("BG##world_bg_enable", &g_world_state.bg_enabled) &&
            g_world_state.bg_enabled && !bg.loaded)
            action.request_load_bg = true;   /* ticked with nothing to show */
        if (ImGui::IsItemHovered()) {
            if (bg.loaded)
                ImGui::SetTooltip("Draw %s behind the playfield as an alignment\n"
                                  "reference.", bg.stage_name.c_str());
            else
                ImGui::SetTooltip("Draw an MK2 stage behind the playfield as an\n"
                                  "alignment reference. Nothing is loaded yet.");
        }
        if (ImGui::BeginMenu("Background...##world_bg_cfg")) {
            WorldDrawBackgroundConfig(action);
            ImGui::EndMenu();
        }

        ImGui::Separator();
        if (ImGui::Checkbox("Link Anchors##world_anchor_link", &state.anchor_link_mode))
            state.anchor_link_active = false;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Drag from a feature on one World View sprite to its matching\n"
                              "feature on another. On release, the target sprite's anipoint\n"
                              "is moved so the two points meet.");
        ImGui::Separator();
        /* The dummy body and the split rows moved here from a "Lanes..." menu
           of their own. Overlays already means "things in the scene that are
           not your marked rows" -- the reference figure and the stage
           background live here -- and one fewer menu on the strip is worth
           more than the category being exact. */
        if (ImGui::Checkbox("Dummy Body##world_dummy_decap_body",
                            &state.dummy_decap_body)) {
            state.dummy_decap_reset = true;
            state.hold_end[kWorldDummyDecapSlot] = true;
            WorldMarkedRestart(state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds the stock fatality decap body as its own sync lane\n"
                              "using *DECAP1-7 frames from open tabs.");
        if (ImGui::MenuItem("Use Selected as Body##world_dummy_assign")) {
            if (WorldAssignSelectedDummyDecap(state, selected_img, active_doc_idx))
                action.dummy_assigned = true;
            else
                action.dummy_assign_failed = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Assign the dummy body from the selected *DECAP frame,\n"
                              "*DECAPLEG piece, or *DECAPTORSO piece.");
        if (state.dummy_decap_manual) {
            ImGui::TextDisabled("Body: [%d] %sDECAP",
                                state.dummy_decap_doc_idx,
                                state.dummy_decap_prefix.c_str());
            if (ImGui::MenuItem("Back to Automatic Body##world_dummy_auto")) {
                state.dummy_decap_manual = false;
                state.dummy_decap_reset = true;
                WorldMarkedRestart(state);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Return to automatic dummy body selection.");
        }
        if (ImGui::MenuItem("Clear Split Rows##world_marked_clear_splits", NULL, false,
                            !state.split_lanes.empty()))
            WorldMarkedClearSplitLanes(state);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Remove split rows and restore each source row to its\n"
                              "marked-frame sequence.");
        ImGui::EndPopup();
    }

    ImGui::SameLine();

    /* ---- ASM: the generated animation tables ---- */
    if (WorldHeaderMenu(g_icon_font_loaded ? ICON_CODE "##world_menu_asm"
                                           : "ASM##world_menu_asm",
                        "##world_asm_popup",
                        "ASM\n\n"
                        "Copy, preview, save or load animation-table source\n"
                        "for the WHOLE scene. Each row has its own copy button.")) {
        if (ImGui::MenuItem("Copy ASM to Clipboard##world_marked_copy_asm")) {
            state.generated_asm = WorldBuildMarkedAsm(state, lanes);
            ImGui::SetClipboardText(state.generated_asm.c_str());
            action.copied_asm = true;
            action.copied_lane_count = (int)lanes.size();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copies one animation table per marked tab, plus aligned\n"
                              "local-anipoint tables.");
        if (ImGui::MenuItem("View ASM##world_marked_view_asm")) {
            state.generated_asm = WorldBuildMarkedAsm(state, lanes);
            state.show_asm = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Preview the generated animation-table source.");
        if (ImGui::MenuItem("Save ASM...##world_marked_save_asm")) {
            state.generated_asm = WorldBuildMarkedAsm(state, lanes);
            action.request_save_asm = true;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Save the generated animation tables to a .ASM file.");
        if (ImGui::MenuItem("Load ASM...##world_marked_load_asm"))
            action.request_load_asm = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Load a saved/character .ASM into the ASM Animations viewer.\n"
                              "The sprite IMGs it references are opened automatically.");
        ImGui::EndPopup();
    }

    /* Which .wax this is. A lane gets iterated as barakadown3, barakadoneout,
       barakcleandone... and the scene on screen cannot tell you which of them
       you are looking at -- nor which one a generator reading the file will
       pick up. Name it on the strip, and click to put the full path on the
       clipboard, since the next step is usually pasting it somewhere. */
    {
        const char *proj = WorldLastProjectPath();
        ImGui::SameLine();
        if (proj && proj[0]) {
            const char *slash = strrchr(proj, '\\');
            const char *fwd = strrchr(proj, '/');
            if (fwd && (!slash || fwd > slash)) slash = fwd;
            const char *base = slash ? slash + 1 : proj;
            ImGui::TextDisabled("| %s", base);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("World View project last opened or saved:\n%s\n"
                                  "Click to copy the full path.", proj);
            }
            if (ImGui::IsItemClicked()) {
                ImGui::SetClipboardText(proj);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Copied project path: %s", proj);
                g_restore_msg_timer = 4.0f;
            }
        } else {
            ImGui::TextDisabled("| no project");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("No World View project has been opened or saved\n"
                                  "this session. File... > Save Project names one.");
        }
    }

    /* The one status that stays on the strip: it only appears when the scene
       is asking for a body it cannot find, which is a broken preview rather
       than a setting. */
    if (state.dummy_decap_body && dummy_decap_missing) {
        ImGui::SameLine();
        ImGui::TextDisabled("No assigned *DECAP body found");
    }

    return action;
}

static void WorldSyncEditorSelectionToSprite(Document *doc, int doc_idx,
                                             int img_idx);

static void WorldDrawEmbeddedScriptTable(WorldMarkedSequenceState &state,
                                         WorldMarkedLane &lane)
{
    int slot = lane.delay_slot;
    int n = (int)lane.frames.size();
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || n <= 0)
        return;

    EnsureWorldMarkedFrameDelays(state, slot, n);
    ImGui::Separator();
    ImGui::Text("Script Table  %s  entries=%d",
                state.embedded_name.empty() ? "(unnamed)" : state.embedded_name.c_str(),
                n);
    ImGui::TextDisabled("World View shows the first drawable frame from each target sequence. Load Seq opens the full target sequence.");
    ImGui::SameLine();
    int stop_tick = state.stop_tick[slot];
    ImGui::TextDisabled("Stop@");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    if (ImGui::InputInt("##script_stop_tick", &stop_tick, 0, 0))
        state.stop_tick[slot] = ClampWorldMarkedVisibleFrom(stop_tick);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Freeze this script lane at the given global preview tick. 0 disables the tick stop.");

    ImGuiTableFlags flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_Resizable |
        ImGuiTableFlags_SizingStretchProp;
    int load_target = -1;
    if (ImGui::BeginTable("##world_embedded_script_table", 6, flags)) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Ticks", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("dX", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("dY", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableHeadersRow();

        for (int fi = 0; fi < n; fi++) {
            bool current = fi == lane.frame_pos;
            ImGui::PushID(fi);
            ImGui::TableNextRow();
            if (current) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(38, 86, 135, 180));
            }

            ImGui::TableNextColumn();
            char row_label[16];
            snprintf(row_label, sizeof(row_label), "%d", fi);
            if (ImGui::Selectable(row_label, current)) {
                state.paused = true;
                state.timer = 0.0f;
                state.frame = WorldMarkedTickForFrame(state, slot, n, fi);
                lane.frame_pos = fi;
                if (fi < (int)lane.frames.size())
                    WorldSyncEditorSelectionToSprite(lane.doc, lane.doc_idx,
                                                     lane.frames[(size_t)fi]);
            }

            ImGui::TableNextColumn();
            int target = (fi < (int)state.embedded_targets.size())
                       ? state.embedded_targets[(size_t)fi] : -1;
            const char *target_name =
                (fi < (int)state.embedded_frame_labels.size() &&
                 !state.embedded_frame_labels[(size_t)fi].empty())
                    ? state.embedded_frame_labels[(size_t)fi].c_str()
                    : "sequence";
            ImGui::Text("%d  %s", target, target_name);

            ImGui::TableNextColumn();
            int ticks = state.frame_delays[slot][fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##script_ticks", &ticks, 0, 0)) {
                state.frame_delays[slot][fi] = ClampTimelineHold(ticks);
                state.paused = true;
            }

            ImGui::TableNextColumn();
            int dx = state.local_dx[slot][fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##script_dx", &dx, 0, 0)) {
                state.local_dx[slot][fi] = ClampWorldMarkedAniptDelta(dx);
                state.paused = true;
            }

            ImGui::TableNextColumn();
            int dy = state.local_dy[slot][fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##script_dy", &dy, 0, 0)) {
                state.local_dy[slot][fi] = ClampWorldMarkedAniptDelta(dy);
                state.paused = true;
            }

            ImGui::TableNextColumn();
            bool can_load = target >= 0 && g_doc && target < (int)g_doc->seqcnt;
            ImGui::BeginDisabled(!can_load);
            if (ImGui::SmallButton("Load Seq##script_load_target"))
                load_target = target;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (load_target >= 0)
        WorldLoadSeqScrRecord(load_target);
}

static void WorldEmbeddedSequenceRefreshMetadata(WorldMarkedSequenceState &state,
                                                 WorldMarkedLane &lane)
{
    int slot = kWorldEmbeddedSeqScrSlot;
    if (lane.delay_slot != slot)
        slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return;

    lane.frames = state.sequence_frames[slot];
    int n = (int)lane.frames.size();
    state.embedded_targets.resize((size_t)n);
    state.embedded_frame_labels.resize((size_t)n);
    EnsureWorldMarkedFrameDelays(state, slot, n);
    WorldMarkedBuildSingleFrameLane(lane.doc, lane.frames,
                                    lane.frame_pieces, lane.frame_labels,
                                    &state.entry_pieces[slot],
                                    &state.frame_doc[slot]);
    lane.frame_docs = WorldMarkedResolveFrameDocs(lane.doc, state.frame_doc[slot]);
    for (int fi = 0; fi < n; fi++) {
        int target = lane.frames[(size_t)fi];
        state.embedded_targets[(size_t)fi] = target;
        Document *entry_doc = (fi < (int)lane.frame_docs.size() && lane.frame_docs[(size_t)fi])
                             ? lane.frame_docs[(size_t)fi] : lane.doc;
        IMG *img = doc_get_img(entry_doc, target);
        std::string label;
        if (img) {
            label = img_name_string(img);
        } else {
            char fallback[32];
            snprintf(fallback, sizeof(fallback), "img[%d]?", target);
            label = fallback;
        }
        state.embedded_frame_labels[(size_t)fi] = label;
        if (fi < (int)lane.frame_labels.size())
            lane.frame_labels[(size_t)fi] = label;
    }

    lane.tick = WorldMarkedEffectiveTickForSlot(state, slot, n, state.frame);
    lane.frame_pos = WorldMarkedFrameForTick(state, slot, n, lane.tick,
                                             state.hold_end[slot]);
    if (lane.frame_pos < 0) lane.frame_pos = 0;
    if (lane.frame_pos >= n) lane.frame_pos = n - 1;
    Document *cur_doc = (lane.frame_pos >= 0 &&
                         lane.frame_pos < (int)lane.frame_docs.size() &&
                         lane.frame_docs[(size_t)lane.frame_pos])
                      ? lane.frame_docs[(size_t)lane.frame_pos] : lane.doc;
    lane.img = (lane.frame_pos >= 0 && lane.frame_pos < n)
             ? doc_get_img(cur_doc, lane.frames[(size_t)lane.frame_pos])
             : NULL;
}

static void WorldSyncEditorSelectionToSprite(Document *doc, int doc_idx,
                                             int img_idx)
{
    if (!doc || img_idx < 0 || !doc_get_img(doc, img_idx))
        return;

    if (doc_idx >= 0 && doc_idx != document_active_index()) {
        document_set_active(doc_idx);
        /* Selecting a frame in a World View lane commonly crosses IMG tabs.
           Preserve the active multi-document workspace while doing so. */
        if (!(g_world_state.enabled && g_world_marked_state.marked_play))
            ResetPerDocumentUiState(false);
        g_doc_tab_select_request = doc_idx;
    }

    if (!g_doc || !doc_get_img(g_doc, img_idx))
        return;
    g_doc->ilselected = img_idx;
    g_img_tex_idx = -2;
    g_zoom_reset = true;
}

/* Defined with the frame browser below; picking a sequence entry hands the
   corner box back to the playhead. */
static void SeqScrClearBrowseSelection(void);

static void WorldEmbeddedSequenceSelectEntry(WorldMarkedSequenceState &state,
                                             WorldMarkedLane &lane,
                                             int frame_idx)
{
    int slot = lane.delay_slot;
    int n = (int)lane.frames.size();
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || n <= 0)
        return;
    SeqScrClearBrowseSelection();
    if (frame_idx < 0) frame_idx = 0;
    if (frame_idx >= n) frame_idx = n - 1;
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, n, frame_idx);
    lane.frame_pos = frame_idx;
    Document *entry_doc = (frame_idx < (int)lane.frame_docs.size() && lane.frame_docs[(size_t)frame_idx])
                        ? lane.frame_docs[(size_t)frame_idx] : lane.doc;
    lane.img = doc_get_img(entry_doc, lane.frames[(size_t)frame_idx]);
    WorldSyncEditorSelectionToSprite(entry_doc, lane.doc_idx,
                                     lane.frames[(size_t)frame_idx]);
}

bool WorldEmbeddedSeqScrActive(const WorldMarkedSequenceState &state)
{
    return state.embedded_active &&
           state.embedded_doc_idx >= 0 &&
           !state.sequence_frames[kWorldEmbeddedSeqScrSlot].empty();
}

void WorldExitEmbeddedSeqScr(WorldMarkedSequenceState &state)
{
    const int slot = kWorldEmbeddedSeqScrSlot;
    WorldMarkedClearSequenceState(state, slot);
    state.default_frames[slot].clear();
    WorldMarkedClearRowDoc(state, slot);
    state.embedded_active = false;
    state.embedded_is_script = false;
    state.embedded_show_companions = false;
    state.embedded_record_index = -1;
    state.embedded_doc_idx = -1;
    state.embedded_name.clear();
    state.embedded_frame_labels.clear();
    state.embedded_targets.clear();
    state.paused = true;
    state.timer = 0.0f;
    state.frame = 0;
    WorldMarkedRestart(state);
}

bool WorldMarkedAttachSpriteToFrame(WorldMarkedSequenceState &state,
                                    int slot, int frame_idx,
                                    Document *doc, int doc_idx,
                                    int sprite_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || !doc ||
        sprite_idx < 0 || !doc_get_img(doc, sprite_idx))
        return false;
    std::vector<int> &frames = state.sequence_frames[slot];
    if (frame_idx < 0 || frame_idx >= (int)frames.size())
        return false;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    /* A composite frame currently has one owning document.  Refuse a
       cross-tab mix rather than silently resolving the extra piece against
       the wrong IMG library. */
    int owner_doc_idx = WorldMarkedRowDocIndex(state, slot);
    if (frame_idx < (int)state.frame_doc[slot].size() &&
        state.frame_doc[slot][frame_idx] >= 0)
        owner_doc_idx = state.frame_doc[slot][frame_idx];
    if (owner_doc_idx >= 0 && owner_doc_idx != doc_idx)
        return false;

    std::vector<int> &pieces = state.entry_pieces[slot][frame_idx];
    if (pieces.empty())
        pieces.push_back(frames[frame_idx]);
    if (std::find(pieces.begin(), pieces.end(), sprite_idx) != pieces.end())
        return false;
    pieces.push_back(sprite_idx);
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), frame_idx);
    return true;
}

void StepWorldEmbeddedSeqScrEntry(WorldMarkedSequenceState &state, int delta)
{
    if (!WorldEmbeddedSeqScrActive(state))
        return;
    const int slot = kWorldEmbeddedSeqScrSlot;
    int n = (int)state.sequence_frames[slot].size();
    if (n <= 0) return;

    int cur = WorldMarkedFrameForTick(state, slot, n, state.frame,
                                      state.hold_end[slot]);
    if (cur < 0) cur = 0;
    if (cur >= n) cur = n - 1;

    int next = (cur + delta) % n;
    if (next < 0) next += n;

    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, n, next);

    Document *doc = document_get(state.embedded_doc_idx);
    WorldSyncEditorSelectionToSprite(doc, state.embedded_doc_idx,
                                     state.sequence_frames[slot][(size_t)next]);
}

static void WorldMarkedApplyAutoYChain(WorldMarkedSequenceState &state,
                                       const WorldMarkedLane &lane,
                                       int start_frame, int show_step,
                                       int fallback_life, int visual_vx,
                                       int visual_vy, int breach_y);
static void WorldMarkedApplyYLinkChain(WorldMarkedSequenceState &state,
                                       const WorldMarkedLane &lane,
                                       int start_frame, int link_count,
                                       int gap_px, int delay_ticks,
                                       int visual_vy, bool ping_pong,
                                       int ping_pong_delay_ticks);
static int WorldMarkedChainReverseStartTick(WorldMarkedSequenceState &state,
                                            const WorldMarkedLane &lane,
                                            int start_frame, int link_count,
                                            int gap_px, int delay_ticks,
                                            int visual_vy,
                                            int ping_pong_delay_ticks);
static void WorldMarkedClearMotionFrom(WorldMarkedSequenceState &state,
                                       int slot, int start_frame);
static bool WorldDrawSubframeSwapTool(WorldMarkedSequenceState &state,
                                      WorldMarkedLane &lane,
                                      int edit_fi,
                                      bool embedded);
static bool WorldMarkedDuplicateSlot(WorldMarkedSequenceState &state,
                                     const WorldMarkedLane &lane,
                                     const std::vector<WorldMarkedLane> &lanes);
static int ClampWorldMarkedFrameMirror(int value);

static int WorldMarkedSpriteHeightForChain(const WorldMarkedLane &lane,
                                           int frame_idx)
{
    if (frame_idx < 0 || frame_idx >= (int)lane.frames.size())
        return 20;
    Document *entry_doc = (frame_idx < (int)lane.frame_docs.size() && lane.frame_docs[(size_t)frame_idx])
                        ? lane.frame_docs[(size_t)frame_idx] : lane.doc;
    IMG *img = doc_get_img(entry_doc, lane.frames[(size_t)frame_idx]);
    if (!img || img->h <= 0)
        return 20;
    return img->h > 9999 ? 9999 : img->h;
}

static void WorldMarkedClampAutoChainSettings(WorldMarkedSequenceState &state,
                                              int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return;
    if (state.auto_step[slot] < 0) state.auto_step[slot] = 0;
    if (state.auto_step[slot] > 999) state.auto_step[slot] = 999;
    if (state.auto_life[slot] < 1) state.auto_life[slot] = 1;
    if (state.auto_life[slot] > 9999) state.auto_life[slot] = 9999;
    state.auto_vx[slot] = ClampWorldMarkedMotion(state.auto_vx[slot]);
    state.auto_vy[slot] = ClampWorldMarkedMotion(state.auto_vy[slot]);
    if (state.auto_y[slot] < -9999) state.auto_y[slot] = -9999;
    if (state.auto_y[slot] >  9999) state.auto_y[slot] =  9999;
    if (state.chain_count[slot] < 1) state.chain_count[slot] = 1;
    if (state.chain_count[slot] > 32) state.chain_count[slot] = 32;
    if (state.chain_gap[slot] < 1) state.chain_gap[slot] = 1;
    if (state.chain_gap[slot] > 9999) state.chain_gap[slot] = 9999;
    if (state.chain_delay[slot] < 0) state.chain_delay[slot] = 0;
    if (state.chain_delay[slot] > 9999) state.chain_delay[slot] = 9999;
    state.chain_vy[slot] = ClampWorldMarkedMotion(state.chain_vy[slot]);
    if (state.chain_vy[slot] == 0) state.chain_vy[slot] = 1;
    state.pingpong_delay[slot] =
        ClampWorldMarkedVisibleFrom(state.pingpong_delay[slot]);
    state.stop_tick[slot] = ClampWorldMarkedVisibleFrom(state.stop_tick[slot]);
    state.subframe_swap_tick[slot] =
        ClampWorldMarkedVisibleFrom(state.subframe_swap_tick[slot]);
}

/* ---- SEQSCR entry tables -------------------------------------------
 * A SEQSCR ENTRY holds four editable values: the target index, the tick hold,
 * and dX/dY. That is the whole vocabulary, so that is all these tables offer.
 * World View's per-entry show/hide ticks, motion vectors, Z order, dual
 * instances, flip bits, and its auto-chain / waterline generators are preview
 * state with nowhere to live in the IMG; offering them here would invite
 * edits that silently evaporate on save. The three spare words per entry stay
 * with the raw-data editor, which is explicit about being raw.
 *
 * Edits go into the lane, and SeqScrSyncLaneToBlob() reconciles the lane into
 * the record once per frame — so dragging a sprite in the viewport persists
 * exactly like typing a dX does. */

/* Entries the blob can represent: everything sourced from the record's own
   IMG. Frames pulled in from a sibling IMG are skipped — a SEQSCR index cannot
   name a sprite outside its own file.

   Returned in STORAGE order, which for a sequence is the reverse of what the
   table shows: Midway's sequence ENTRY arrays run back to front, which is why
   WorldDecodeSeqScrRecord and the ASM exporter both walk them as
   `num - 1 - i`. Script arrays run forward. Writing display order straight
   back would silently flip every sequence in the file. */
static std::vector<SeqScrEntryValues> SeqScrLaneOwnEntries(
    const WorldMarkedSequenceState &state)
{
    std::vector<SeqScrEntryValues> out;
    const int slot = kWorldEmbeddedSeqScrSlot;
    const std::vector<int> &frames = state.sequence_frames[slot];
    const std::vector<int> &fdoc = state.frame_doc[slot];
    for (int i = 0; i < (int)frames.size(); i++) {
        int doc_idx = i < (int)fdoc.size() ? fdoc[(size_t)i] : -1;
        if (doc_idx >= 0 && doc_idx != state.embedded_doc_idx) continue;
        SeqScrEntryValues v;
        v.index = frames[(size_t)i];
        v.ticks = i < (int)state.frame_delays[slot].size()
                ? state.frame_delays[slot][(size_t)i] : 1;
        if (v.ticks < 0) v.ticks = 0;
        if (v.ticks > 255) v.ticks = 255;
        v.dx = i < (int)state.local_dx[slot].size()
             ? state.local_dx[slot][(size_t)i] : 0;
        v.dy = i < (int)state.local_dy[slot].size()
             ? state.local_dy[slot][(size_t)i] : 0;
        out.push_back(v);
    }
    if (!state.embedded_is_script)
        std::reverse(out.begin(), out.end());
    return out;
}

/* What the lane held right after it was loaded. The decoder normalises as it
   reads (a 0-tick entry becomes a 1-tick hold, deltas get clamped), so
   comparing the lane against the blob would rewrite untouched records and
   dirty documents nobody edited. Comparing against this baseline instead means
   only a real edit writes. */
static std::vector<SeqScrEntryValues> s_seqscr_sync_baseline;
static int s_seqscr_sync_record = -1;
static int s_seqscr_sync_doc = -1;

static bool SeqScrEntriesEqual(const std::vector<SeqScrEntryValues> &a,
                               const std::vector<SeqScrEntryValues> &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].index != b[i].index || a[i].ticks != b[i].ticks ||
            a[i].dx != b[i].dx || a[i].dy != b[i].dy)
            return false;
    }
    return true;
}

/* Write the lane back into the record, if and only if it differs from what was
   loaded. */
static void SeqScrSyncLaneToBlob(WorldMarkedSequenceState &state)
{
    if (!state.embedded_active || state.embedded_record_index < 0) return;
    if (!document_get(state.embedded_doc_idx)) return;
    /* A viewport drag moves dX/dY every frame. Writing mid-drag would push one
       undo step per frame; wait for the release and record the whole drag as
       a single edit. */
    if (state.drag_slot >= 0) return;

    std::vector<SeqScrEntryValues> now = SeqScrLaneOwnEntries(state);
    if (s_seqscr_sync_record != state.embedded_record_index ||
        s_seqscr_sync_doc != state.embedded_doc_idx) {
        /* First sight of this record: adopt it as the baseline, write nothing. */
        s_seqscr_sync_record = state.embedded_record_index;
        s_seqscr_sync_doc = state.embedded_doc_idx;
        s_seqscr_sync_baseline = now;
        return;
    }
    if (SeqScrEntriesEqual(now, s_seqscr_sync_baseline)) return;

    /* The blob helpers work through g_doc, so the record's own document has to
       be active while it is rewritten. */
    int restore = document_active_index();
    bool switched = state.embedded_doc_idx != restore;
    if (switched) document_set_active(state.embedded_doc_idx);
    bool ok = SeqScrReplaceEntries(state.embedded_record_index, now);
    if (switched) document_set_active(restore);
    if (ok) s_seqscr_sync_baseline = now;
}

/* Sequence entries: one IMG sprite per row. */
static void SeqScrDrawEntryTable(WorldMarkedSequenceState &state,
                                 WorldMarkedLane &lane, float table_h)
{
    int slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;

    EnsureWorldMarkedFrameDelays(state, slot, (int)lane.frames.size());
    WorldEmbeddedSequenceRefreshMetadata(state, lane);
    int n = (int)lane.frames.size();
    if (n <= 0) {
        ImGui::TextDisabled("This sequence has no entries yet. Mark frames in the Animation panel and use \"Add Marked to Sequence\".");
        return;
    }
    int edit_fi = lane.frame_pos;
    if (edit_fi < 0) edit_fi = 0;
    if (edit_fi >= n) edit_fi = n - 1;

    ImGui::Text("Sequence  %s  entries=%d",
                state.embedded_name.empty() ? "(unnamed)"
                                            : state.embedded_name.c_str(), n);
    ImGui::SameLine();
    ImGui::TextDisabled("Entry %d/%d", edit_fi + 1, n);
    ImGui::SameLine();
    ImGui::TextDisabled("| index, ticks, dX and dY are the record's own fields and save with the IMG.");

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingFixedFit;
    int pending_move_fi = -1, pending_move_dir = 0;
    int pending_delete_fi = -1, pending_dup_fi = -1;

    if (ImGui::BeginTable("##seqscr_entry_table", 7, flags,
                          ImVec2(0.0f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 96.0f);
        ImGui::TableSetupColumn("Sprite", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Index", ImGuiTableColumnFlags_WidthFixed, 68.0f);
        ImGui::TableSetupColumn("Ticks", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("dX", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("dY", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableHeadersRow();

        for (int fi = 0; fi < n; fi++) {
            bool current = fi == lane.frame_pos;
            int entry_doc = fi < (int)state.frame_doc[slot].size()
                          ? state.frame_doc[slot][(size_t)fi] : -1;
            bool foreign = entry_doc >= 0 && entry_doc != state.embedded_doc_idx;

            ImGui::PushID(fi);
            ImGui::TableNextRow();
            if (current)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(38, 86, 135, 180));

            ImGui::TableNextColumn();
            char row_label[16];
            snprintf(row_label, sizeof(row_label), "%d", fi);
            /* Deliberately not SpanAllColumns: a spanning Selectable sits over
               the row-op buttons and value fields to its right and eats their
               clicks. The row number alone selects the entry. */
            if (ImGui::Selectable(row_label, current))
                WorldEmbeddedSequenceSelectEntry(state, lane, fi);

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(fi <= 0);
            if (ImGui::SmallButton("^")) { pending_move_fi = fi; pending_move_dir = -1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(fi + 1 >= n);
            if (ImGui::SmallButton("v")) { pending_move_fi = fi; pending_move_dir = 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("+")) pending_dup_fi = fi;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate this entry.");
            ImGui::SameLine();
            ImGui::BeginDisabled(n <= 1);
            if (ImGui::SmallButton("x")) pending_delete_fi = fi;
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            const char *sprite_name =
                (fi < (int)state.embedded_frame_labels.size() &&
                 !state.embedded_frame_labels[(size_t)fi].empty())
                    ? state.embedded_frame_labels[(size_t)fi].c_str()
                    : "(missing)";
            if (foreign) {
                Document *fdoc = document_get(entry_doc);
                ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "%s", sprite_name);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("From %s. Preview and ASM export only:\na SEQSCR entry cannot name a sprite outside its own file.",
                                      (fdoc && fdoc->fname_s[0]) ? fdoc->fname_s
                                                                 : "another IMG");
                ImGui::SameLine();
                ImGui::TextDisabled("(%s)",
                    (fdoc && fdoc->fname_s[0]) ? fdoc->fname_s : "other IMG");
            } else {
                ImGui::TextUnformatted(sprite_name);
            }

            ImGui::TableNextColumn();
            int target = state.sequence_frames[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##seq_index", &target, 0, 0)) {
                Document *edoc = WorldMarkedResolveEntryDoc(lane.doc, entry_doc);
                int limit = edoc ? (int)edoc->imgcnt : 0;
                if (target < 0) target = 0;
                if (limit > 0 && target >= limit) target = limit - 1;
                state.sequence_frames[slot][(size_t)fi] = target;
                WorldEmbeddedSequenceSelectEntry(state, lane, fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Sprite index inside %s.",
                                  foreign ? "the source IMG" : "this IMG");

            ImGui::TableNextColumn();
            int ticks = state.frame_delays[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##seq_ticks", &ticks, 0, 0)) {
                /* ENTRY.ticks is one byte. */
                if (ticks < 0) ticks = 0;
                if (ticks > 255) ticks = 255;
                state.frame_delays[slot][(size_t)fi] = ticks;
                state.paused = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Frame hold, 0-255 (one byte in the ENTRY record).");

            ImGui::TableNextColumn();
            int dx = state.local_dx[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##seq_dx", &dx, 0, 0)) {
                state.local_dx[slot][(size_t)fi] = ClampWorldMarkedAniptDelta(dx);
                state.paused = true;
            }

            ImGui::TableNextColumn();
            int dy = state.local_dy[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##seq_dy", &dy, 0, 0)) {
                state.local_dy[slot][(size_t)fi] = ClampWorldMarkedAniptDelta(dy);
                state.paused = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (pending_move_fi >= 0) {
        WorldMarkedMoveSequenceEntry(state, slot, pending_move_fi, pending_move_dir);
        WorldEmbeddedSequenceRefreshMetadata(state, lane);
    } else if (pending_dup_fi >= 0) {
        WorldMarkedDuplicateSequenceEntry(state, slot, pending_dup_fi);
        WorldEmbeddedSequenceRefreshMetadata(state, lane);
    } else if (pending_delete_fi >= 0) {
        WorldMarkedDeleteSequenceEntry(state, slot, pending_delete_fi);
        WorldEmbeddedSequenceRefreshMetadata(state, lane);
    }
}

/* Script entries: one sequence call per row. Same four editable fields. */
static void SeqScrDrawScriptTable(WorldMarkedSequenceState &state,
                                  WorldMarkedLane &lane, float table_h)
{
    int slot = lane.delay_slot;
    int n = (int)lane.frames.size();
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    if (n <= 0) {
        ImGui::TextDisabled("This script has no entries yet. Use the raw-data editor to add sequence calls.");
        return;
    }

    EnsureWorldMarkedFrameDelays(state, slot, n);
    ImGui::Text("Script  %s  entries=%d",
                state.embedded_name.empty() ? "(unnamed)"
                                            : state.embedded_name.c_str(), n);
    ImGui::SameLine();
    ImGui::TextDisabled("| each entry calls a sequence; the viewport shows that sequence's first drawable frame.");

    ImGuiTableFlags flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                            ImGuiTableFlags_Resizable |
                            ImGuiTableFlags_ScrollY |
                            ImGuiTableFlags_SizingFixedFit;
    int load_target = -1;
    int pending_move_fi = -1, pending_move_dir = 0, pending_delete_fi = -1;

    if (ImGui::BeginTable("##seqscr_script_table", 7, flags,
                          ImVec2(0.0f, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 74.0f);
        ImGui::TableSetupColumn("Target", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Ticks", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("dX", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("dY", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 78.0f);
        ImGui::TableHeadersRow();

        for (int fi = 0; fi < n; fi++) {
            bool current = fi == lane.frame_pos;
            ImGui::PushID(fi);
            ImGui::TableNextRow();
            if (current)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(38, 86, 135, 180));

            ImGui::TableNextColumn();
            char row_label[16];
            snprintf(row_label, sizeof(row_label), "%d", fi);
            if (ImGui::Selectable(row_label, current)) {
                state.paused = true;
                state.timer = 0.0f;
                state.frame = WorldMarkedTickForFrame(state, slot, n, fi);
                lane.frame_pos = fi;
            }

            ImGui::TableNextColumn();
            ImGui::BeginDisabled(fi <= 0);
            if (ImGui::SmallButton("^")) { pending_move_fi = fi; pending_move_dir = -1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(fi + 1 >= n);
            if (ImGui::SmallButton("v")) { pending_move_fi = fi; pending_move_dir = 1; }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(n <= 1);
            if (ImGui::SmallButton("x")) pending_delete_fi = fi;
            ImGui::EndDisabled();

            ImGui::TableNextColumn();
            int target = (fi < (int)state.embedded_targets.size())
                       ? state.embedded_targets[(size_t)fi] : -1;
            const char *target_name =
                (fi < (int)state.embedded_frame_labels.size() &&
                 !state.embedded_frame_labels[(size_t)fi].empty())
                    ? state.embedded_frame_labels[(size_t)fi].c_str()
                    : "sequence";
            ImGui::Text("%d  %s", target, target_name);

            ImGui::TableNextColumn();
            int ticks = state.frame_delays[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##script_ticks", &ticks, 0, 0)) {
                if (ticks < 0) ticks = 0;
                if (ticks > 255) ticks = 255;
                state.frame_delays[slot][(size_t)fi] = ticks;
                state.paused = true;
            }

            ImGui::TableNextColumn();
            int dx = state.local_dx[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##script_dx", &dx, 0, 0)) {
                state.local_dx[slot][(size_t)fi] = ClampWorldMarkedAniptDelta(dx);
                state.paused = true;
            }

            ImGui::TableNextColumn();
            int dy = state.local_dy[slot][(size_t)fi];
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputInt("##script_dy", &dy, 0, 0)) {
                state.local_dy[slot][(size_t)fi] = ClampWorldMarkedAniptDelta(dy);
                state.paused = true;
            }

            ImGui::TableNextColumn();
            bool can_load = target >= 0 && g_doc && target < (int)g_doc->seqcnt;
            ImGui::BeginDisabled(!can_load);
            if (ImGui::SmallButton("Load Seq")) load_target = target;
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (pending_move_fi >= 0)
        WorldMarkedMoveSequenceEntry(state, slot, pending_move_fi, pending_move_dir);
    else if (pending_delete_fi >= 0)
        WorldMarkedDeleteSequenceEntry(state, slot, pending_delete_fi);
    else if (load_target >= 0)
        WorldLoadSeqScrRecord(load_target);
}


WorldMarkedPanelResult WorldDrawMarkedPanel(WorldMarkedSequenceState &state,
                                            std::vector<WorldMarkedLane> &lanes,
                                            const WorldMarkedPanelLayout &layout,
                                            bool dummy_decap_missing,
                                            IMG *selected_img,
                                            int active_doc_idx)
{
    WorldMarkedPanelResult result = {};

    ImGui::SetCursorScreenPos(layout.pos);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.03f, 0.03f, 0.035f, 0.90f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    /* AlwaysHorizontalScrollbar, not HorizontalScrollbar. A row's content
       width changes as playback moves the current entry, and at a window
       width near that content width the scrollbar was appearing and
       disappearing frame to frame -- each toggle takes ~12px off the child's
       height, which shoves every row up and down. Reserving the bar costs one
       strip of pixels and makes the panel hold still. The widths below are
       padded for the same reason: nothing on a row may change size as the
       tick advances. */
    if (ImGui::BeginChild("##world_marked_sequence",
                          ImVec2(layout.width, layout.height), true,
                          ImGuiWindowFlags_HorizontalScrollbar |
                          ImGuiWindowFlags_AlwaysHorizontalScrollbar)) {
        result.header =
            WorldDrawMarkedPanelHeader(state, lanes, dummy_decap_missing,
                                       selected_img, active_doc_idx);

        /* The header stays whatever the scene holds; only the rows below it
           are missing, so say so here rather than in place of the strip. */
        if (lanes.empty()) {
            ImGui::Separator();
            ImGui::TextDisabled("No rows. Mark sprites in an IMG tab, turn on ASM");
            ImGui::TextDisabled("lanes, or load a project from File... above.");
        }

        /* Slot 1 owns the keys and the per-frame tools until a row is clicked.

           The old fallback chose whichever row happened to contain the
           editor's selected sprite, resolved lazily on the first Left/Right.
           That moves as you click around the image list, so the tools drifted
           off the row you were actually animating -- and until you pressed a
           key, no row was marked at all. The same pass re-homes the keys when
           the active row stops being displayed, which otherwise left [KEYS]
           pointing at a lane that was no longer there. */
        if (!lanes.empty()) {
            int active = state.active_slot;
            bool usable = active >= 0 && active < kWorldMarkedMaxTabs &&
                          !state.sequence_frames[active].empty();
            bool displayed = false;
            for (size_t i = 0; i < lanes.size() && !displayed; i++)
                if (lanes[i].delay_slot == active) displayed = true;
            if (!usable || !displayed)
                state.active_slot = lanes[0].delay_slot;
        }

        for (int slot = 0; slot < (int)lanes.size(); slot++) {
            WorldMarkedLane &lane = lanes[slot];
            ImGui::PushID(slot);
            if (WorldDrawMarkedLaneControls(state, lane, lanes, slot)) {
                /* The row is gone and `lanes` still describes the old set;
                   rebuild happens next frame. */
                ImGui::PopID();
                break;
            }

            /* Hidden rows never reach the thumbnail strip: see the early
               return in WorldDrawMarkedLaneControls for why that matters. */
            if (state.lane_visible[lane.delay_slot]) {
                WorldMarkedLaneThumbClick thumb_click =
                    WorldDrawMarkedLaneThumbnails(state, lane, lanes);
                if (thumb_click.clicked) {
                    state.active_slot = lane.delay_slot;
                    result.thumb_click = thumb_click;
                }
            }
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    result.copied_popup_asm = WorldDrawMarkedAsmPopup(state);
    return result;
}

static bool WorldMarkedImageWorldYBounds(Document *doc, int img_idx, int local_dy,
                                         bool mirror_y,
                                         int *out_top, int *out_bottom)
{
    IMG *img = doc_get_img(doc, img_idx);
    if (!img) return false;

    int ay = (int)(short)img->aniy + local_dy;
    int top = g_world_state.origin_y -
              anipoint_effective(ay, (int)img->h, mirror_y, g_mirror_convention);
    int bottom = top + (int)img->h;
    if (out_top) *out_top = top;
    if (out_bottom) *out_bottom = bottom;
    return true;
}

static bool WorldMarkedFrameWorldYBounds(const WorldMarkedLane &lane,
                                         int frame_idx, int local_dy,
                                         bool mirror_y,
                                         int *out_top, int *out_bottom)
{
    if (frame_idx < 0 || frame_idx >= (int)lane.frames.size())
        return false;

    const std::vector<int> *pieces = NULL;
    const std::vector<Document*> *piece_docs = NULL;
    if (frame_idx < (int)lane.frame_pieces.size())
        pieces = &lane.frame_pieces[frame_idx];
    if (frame_idx < (int)lane.frame_piece_docs.size())
        piece_docs = &lane.frame_piece_docs[frame_idx];

    std::vector<int> fallback_piece;
    if (!pieces || pieces->empty()) {
        fallback_piece.push_back(lane.frames[frame_idx]);
        pieces = &fallback_piece;
        piece_docs = NULL;
    }

    Document *frame_doc = (frame_idx < (int)lane.frame_docs.size() &&
                          lane.frame_docs[frame_idx])
                        ? lane.frame_docs[frame_idx] : lane.doc;
    bool valid = false;
    int top = 0;
    int bottom = 0;
    for (size_t pi = 0; pi < pieces->size(); pi++) {
        Document *pdoc = (piece_docs && pi < piece_docs->size() && (*piece_docs)[pi])
                       ? (*piece_docs)[pi] : frame_doc;
        int piece_top = 0;
        int piece_bottom = 0;
        if (!WorldMarkedImageWorldYBounds(pdoc, (*pieces)[pi], local_dy, mirror_y,
                                          &piece_top, &piece_bottom))
            continue;
        if (!valid) {
            top = piece_top;
            bottom = piece_bottom;
            valid = true;
        } else {
            if (piece_top < top) top = piece_top;
            if (piece_bottom > bottom) bottom = piece_bottom;
        }
    }

    if (!valid) return false;
    if (out_top) *out_top = top;
    if (out_bottom) *out_bottom = bottom;
    return true;
}

static int WorldMarkedTicksUntilYBreach(const WorldMarkedLane &lane,
                                        int frame_idx, int local_dy,
                                        bool mirror_y,
                                        int visual_vy, int breach_y,
                                        int fallback_life)
{
    int life = fallback_life < 1 ? 1 : fallback_life;
    int top = 0;
    int bottom = 0;
    if (!WorldMarkedFrameWorldYBounds(lane, frame_idx, local_dy, mirror_y,
                                      &top, &bottom))
        return life;

    if (visual_vy > 0) {
        int dist = breach_y - bottom + 1; /* bottom moves past the line */
        if (dist <= 0) return 1;
        int ticks = (dist + visual_vy - 1) / visual_vy;
        return ticks < 1 ? 1 : ticks;
    }
    if (visual_vy < 0) {
        int speed = -visual_vy;
        int dist = top - breach_y + 1;    /* top moves past the line */
        if (dist <= 0) return 1;
        int ticks = (dist + speed - 1) / speed;
        return ticks < 1 ? 1 : ticks;
    }
    return life;
}

/* Same dist/ticks formula as WorldMarkedTicksUntilYBreach, but for one
   specific image rather than a frame's whole piece group — used so each
   fine subframe can hide individually as its own bottom crosses the
   waterline, instead of the group hiding all at once. */
static int WorldMarkedPieceTicksUntilYBreach(Document *doc, int img_idx,
                                             int local_dy, bool mirror_y,
                                             int visual_vy, int breach_y,
                                             int fallback_life)
{
    int life = fallback_life < 1 ? 1 : fallback_life;
    int top = 0;
    int bottom = 0;
    if (!WorldMarkedImageWorldYBounds(doc, img_idx, local_dy, mirror_y,
                                      &top, &bottom))
        return life;

    if (visual_vy > 0) {
        int dist = breach_y - bottom + 1;
        if (dist <= 0) return 1;
        int ticks = (dist + visual_vy - 1) / visual_vy;
        return ticks < 1 ? 1 : ticks;
    }
    if (visual_vy < 0) {
        int speed = -visual_vy;
        int dist = top - breach_y + 1;
        if (dist <= 0) return 1;
        int ticks = (dist + speed - 1) / speed;
        return ticks < 1 ? 1 : ticks;
    }
    return life;
}

static void WorldMarkedApplyAutoYChain(WorldMarkedSequenceState &state,
                                       const WorldMarkedLane &lane,
                                       int start_frame, int show_step,
                                       int fallback_life, int visual_vx,
                                       int visual_vy, int breach_y)
{
    int slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return;
    int n = (int)lane.frames.size();
    if (n <= 0) return;
    if (start_frame < 0) start_frame = 0;
    if (start_frame >= n) start_frame = n - 1;

    EnsureWorldMarkedFrameDelays(state, slot, n);
    int step = show_step < 0 ? 0 : show_step;
    int life = fallback_life < 1 ? 1 : fallback_life;
    int base_show = state.visible_from[slot][start_frame];
    if (base_show <= 0)
        base_show = WorldMarkedTickForFrame(state, slot, n, start_frame);

    visual_vx = ClampWorldMarkedMotion(visual_vx);
    visual_vy = ClampWorldMarkedMotion(visual_vy);
    for (int fi = start_frame; fi < n; fi++) {
        int show_tick = base_show + (fi - start_frame) * step;
        bool mirror_y = (state.frame_mirror[slot][fi] & kWorldFrameMirrorY) != 0;
        int entry_life =
            WorldMarkedTicksUntilYBreach(lane, fi,
                                         state.local_dy[slot][fi],
                                         mirror_y, visual_vy, breach_y, life);
        state.visible_from[slot][fi] = ClampWorldMarkedVisibleFrom(show_tick);
        state.visible_until[slot][fi] =
            ClampWorldMarkedVisibleUntil(show_tick + entry_life);
        state.motion_dx[slot][fi] = visual_vx;
        state.motion_dy[slot][fi] = visual_vy;
        state.motion_cap_x[slot][fi] = 0;
        state.motion_cap_y[slot][fi] = 0;
        if (step > 0)
            state.frame_delays[slot][fi] = ClampTimelineHold(step);
    }
    state.paused = true;
    state.timer = 0.0f;
    state.frame = base_show;
}

static void WorldMarkedClearMotionFrom(WorldMarkedSequenceState &state,
                                       int slot, int start_frame)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return;
    int n = (int)state.sequence_frames[slot].size();
    if (n <= 0) n = (int)state.motion_dx[slot].size();
    if (start_frame < 0) start_frame = 0;
    EnsureWorldMarkedFrameDelays(state, slot, n);
    for (int fi = start_frame; fi < n; fi++) {
        state.motion_dx[slot][fi] = 0;
        state.motion_dy[slot][fi] = 0;
        state.motion_cap_x[slot][fi] = 0;
        state.motion_cap_y[slot][fi] = 0;
    }
}

static int WorldGroundAlignLaneAnipoints(const WorldMarkedLane &lane)
{
    /* Undo storage is document-local. Require this slot's IMG tab to be active
       so the whole correction remains one clean, reversible operation. */
    if (!g_doc || lane.doc != g_doc || lane.frames.empty())
        return -1;

    struct GroundAnchor { IMG *img; int img_idx; int x; int y; };
    std::vector<GroundAnchor> anchors;
    anchors.reserve(lane.frames.size());

    int previous_x = 0;
    bool have_previous_x = false;
    for (size_t fi = 0; fi < lane.frames.size(); fi++) {
        Document *entry_doc = (fi < lane.frame_docs.size() && lane.frame_docs[fi])
                            ? lane.frame_docs[fi] : lane.doc;
        if (entry_doc != lane.doc)
            return -1; /* mixed-document ASM lanes need cross-document undo */

        int img_idx = lane.frames[fi];
        IMG *img = doc_get_img(entry_doc, img_idx);
        if (!img || img->w == 0 || img->h == 0)
            continue;

        bool duplicate = false;
        for (const GroundAnchor &anchor : anchors) {
            if (anchor.img == img) { duplicate = true; break; }
        }
        if (duplicate)
            continue;

        CanvasContentBounds bounds = CanvasFindOpaqueBounds(img);
        int left = bounds.valid ? bounds.min_x : 0;
        int right = bounds.valid ? bounds.max_x + 1 : (int)img->w;
        int bottom = bounds.valid ? bounds.max_y + 1 : (int)img->h;

        int x = (int)(short)img->anix;
        if (have_previous_x) {
            /* Reuse the preceding frame's axis while it crosses this sprite;
               otherwise settle on the nearest opaque edge. */
            x = previous_x;
            if (x < left) x = left;
            if (x > right) x = right;
        }
        previous_x = x;
        have_previous_x = true;
        anchors.push_back({img, img_idx, x, bottom});
    }

    /* These are preview-only offsets. Leaving them behind would make correctly
       aligned hard IMG anipoints still appear inconsistent in World View. */
    int slot = lane.delay_slot;
    bool preview_offsets_changed = false;
    if (slot >= 0 && slot < kWorldMarkedMaxTabs) {
        for (int value : g_world_marked_state.local_dx[slot])
            if (value != 0) preview_offsets_changed = true;
        for (int value : g_world_marked_state.local_dy[slot])
            if (value != 0) preview_offsets_changed = true;
    }

    int changed = 0;
    for (const GroundAnchor &anchor : anchors)
        if ((int)(short)anchor.img->anix != anchor.x ||
            (int)(short)anchor.img->aniy != anchor.y)
            changed++;
    if (changed == 0 && !preview_offsets_changed)
        return 0;
    if (changed > 0 && !doc_undo_push())
        return 0;

    for (const GroundAnchor &anchor : anchors) {
        anchor.img->anix = signed_to_img_word(anchor.x);
        anchor.img->aniy = signed_to_img_word(anchor.y);
        InvalidateThumb(anchor.img_idx);
    }
    if (slot >= 0 && slot < kWorldMarkedMaxTabs) {
        std::fill(g_world_marked_state.local_dx[slot].begin(),
                  g_world_marked_state.local_dx[slot].end(), 0);
        std::fill(g_world_marked_state.local_dy[slot].begin(),
                  g_world_marked_state.local_dy[slot].end(), 0);
    }
    g_img_tex_idx = -2;
    mark_dirty();
    return changed > 0 ? changed : (preview_offsets_changed ? 1 : 0);
}

/* The row's removal confirm, factored out because a hidden row returns early
   and still has to be deletable. Opened at the caller's own ID-stack level:
   an OpenPopup issued from inside the Row... menu would register against that
   popup's stack and never draw. Returns true when the row went away. */
/* Defined further down, next to the promote itself. */
static std::string WorldMarkedSequenceNameForLane(const WorldMarkedLane &lane);

/* Where a promoted row lands, asked before anything is written.

   Promote used to fire the instant the button was hit: it named the record
   from the row's first sprite and dropped it into whichever tab was active,
   with no way to say otherwise. Both halves of that were wrong often enough
   to matter -- the derived name collides as soon as two rows come off the
   same sprite stem, and the destination is frequently NOT the tab in front,
   because the row was staged from sprites in several files.

   Held across frames because a modal spans them; keyed to the slot it was
   opened for so a stray Del elsewhere cannot retarget it. */
static char s_promote_name[32] = "";
static int  s_promote_doc = -1;
static int  s_promote_slot = -1;

static void WorldOpenPromoteSeqDialog(const WorldMarkedLane &lane)
{
    std::string seed = WorldMarkedSequenceNameForLane(lane);
    snprintf(s_promote_name, sizeof(s_promote_name), "%s", seed.c_str());
    /* Default to the row's own file when it has one -- that is the answer
       most of the time, and it keeps the old behaviour one Enter away. */
    s_promote_doc = -1;
    for (int i = 0; i < document_tab_count(); i++) {
        if (document_get(i) == lane.doc) { s_promote_doc = i; break; }
    }
    if (s_promote_doc < 0) s_promote_doc = document_active_index();
    s_promote_slot = lane.delay_slot;
    ImGui::OpenPopup("##world_promote_seq");
}

static void WorldDrawPromoteSeqDialog(WorldMarkedSequenceState &state,
                                      const WorldMarkedLane &lane)
{
    ImGui::SetNextWindowSize(ImVec2(430.0f, 0.0f), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("##world_promote_seq", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    /* The dialog outlives a rebuild of `lanes`; if the row it was opened for
       is gone, close rather than write into whatever took its slot. */
    if (s_promote_slot != lane.delay_slot) {
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::TextUnformatted("Promote row to a SEQSCR sequence");
    ImGui::Separator();

    ImGui::TextDisabled("Name");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    /* Land in the name box: it is the field that always gets changed. */
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::InputText("##world_promote_name", s_promote_name,
                     sizeof(s_promote_name));
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Name of the new record, as it appears in the Anim tab.\n"
                          "Seeded from the row's first sprite; up to %d characters.",
                          (int)sizeof(s_promote_name) - 1);

    ImGui::Spacing();
    ImGui::TextDisabled("Write into");
    int tabs = document_tab_count();
    if (s_promote_doc < 0 || s_promote_doc >= tabs)
        s_promote_doc = document_active_index();
    if (ImGui::BeginListBox("##world_promote_doc", ImVec2(-1.0f, 132.0f))) {
        for (int i = 0; i < tabs; i++) {
            Document *d = document_get(i);
            if (!d) continue;
            char label[160];
            snprintf(label, sizeof(label), "%s%s  (%u sprite%s, %u seq)##promote_doc_%d",
                     d->fname_s[0] ? d->fname_s : "Untitled",
                     d == lane.doc ? "  <- row's file" : "",
                     d->imgcnt, d->imgcnt == 1 ? "" : "s", d->seqcnt, i);
            if (ImGui::Selectable(label, s_promote_doc == i))
                s_promote_doc = i;
        }
        ImGui::EndListBox();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The open tab that will hold the record. Frames from any\n"
                          "other file are imported into it -- reused when a sprite\n"
                          "of that name is already there, copied in with its\n"
                          "palette when not.");

    ImGui::Separator();
    bool named = s_promote_name[0] != 0;
    ImGui::BeginDisabled(!named);
    if (ImGui::Button("Promote##world_promote_go", ImVec2(110, 0))) {
        std::string msg;
        WorldMarkedPromoteLaneToSequence(state, lane, s_promote_name,
                                         s_promote_doc, &msg);
        if (!msg.empty()) {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", msg.c_str());
            g_restore_msg_timer = 6.0f;
        }
        s_promote_slot = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    if (!named && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Give the sequence a name first.");
    ImGui::SameLine();
    if (ImGui::Button("Cancel##world_promote_no", ImVec2(110, 0))) {
        s_promote_slot = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}


static bool WorldDrawRowDeleteConfirm(WorldMarkedSequenceState &state,
                                      const WorldMarkedLane &lane,
                                      int display_slot, bool want_open)
{
    if (want_open)
        ImGui::OpenPopup("##world_row_delete_confirm");
    if (!ImGui::BeginPopup("##world_row_delete_confirm"))
        return false;

    ImGui::Text("Remove row %d: %s", display_slot + 1,
                !lane.label.empty() ? lane.label.c_str()
                : (lane.doc && lane.doc->fname_s[0] ? lane.doc->fname_s
                                                    : "Untitled"));
    ImGui::Separator();
    ImGui::TextUnformatted(WorldMarkedRemoveLaneDescription(state, lane).c_str());
    ImGui::Separator();
    if (ImGui::Button("Delete##world_row_delete_go", ImVec2(90, 0))) {
        std::string msg;
        bool removed = WorldMarkedRemoveLane(state, lane, &msg);
        if (removed) {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", msg.c_str());
            g_restore_msg_timer = 4.0f;
        }
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return removed;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel##world_row_delete_no", ImVec2(90, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
    return false;
}

bool WorldDrawMarkedLaneControls(WorldMarkedSequenceState &state,
                                 WorldMarkedLane &lane,
                                 const std::vector<WorldMarkedLane> &lanes,
                                 int display_slot)
{
    /* Set by the strip's Del button and by Row... > Delete Row; the confirm
       popup is opened at the bottom of this function, where it is at the same
       ID-stack level as the OpenPopup call. Opening it from inside the Row
       menu would register it against that popup's stack instead, and it would
       never draw. */
    bool want_delete_popup = false;

    ImGui::Separator();
    const char *doc_name = !lane.label.empty()
                         ? lane.label.c_str()
                         : (lane.doc && lane.doc->fname_s[0] ? lane.doc->fname_s : "Untitled");

    /* This row's own animation table, ahead of the eye so it reads as
       something the row produces rather than something done to it. The
       header's ASM... menu emits the WHOLE scene as one draft, which is the
       wrong unit when a lane is being iterated on its own -- a fatality is
       built one actor at a time, and pasting five tables to get at one is how
       the wrong lane ends up in a character file. */
    {
        std::vector<WorldMarkedLane> one;
        one.push_back(lane);
        if (ImGui::SmallButton(g_icon_font_loaded ? ICON_CODE "##world_lane_asm"
                                                  : "ASM##world_lane_asm")) {
            state.generated_asm = WorldBuildMarkedAsm(state, one);
            ImGui::SetClipboardText(state.generated_asm.c_str());
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Copied %s's animation table (%d entr%s) to the clipboard.",
                     doc_name, (int)lane.frames.size(),
                     lane.frames.size() == 1 ? "y" : "ies");
            g_restore_msg_timer = 4.0f;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Copy JUST this row's animation table, with its\n"
                              "local-anipoint table, to the clipboard.\n\n"
                              "ASM... on the strip above does the whole scene,\n"
                              "and can also view, save or load one.");
        ImGui::SameLine();
    }

    bool row_visible = state.lane_visible[lane.delay_slot];
    const char *eye = g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT;
    if (!row_visible)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.38f);
    if (ImGui::SmallButton(eye)) {
        state.lane_visible[lane.delay_slot] = !state.lane_visible[lane.delay_slot];
    }
    if (!row_visible)
        ImGui::PopStyleVar();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(row_visible
            ? "Hide this World View row while testing other animations."
            : "Show this World View row.");

    /* Order and removal. Rows are rebuilt every frame from tab order plus the
       split list, so "move up" swaps the rank the slot carries rather than
       anything in the vector -- and that rank is display order, draw order
       for equal Z, and ASM export order all at once. */
    ImGui::SameLine();
    ImGui::BeginDisabled(display_slot <= 0);
    if (ImGui::SmallButton(g_icon_font_loaded ? ICON_UP "##world_lane_up"
                                              : ICON_UP_TXT "##world_lane_up"))
        WorldMarkedMoveLane(state, lanes, display_slot, -1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move this row up. Row order is the draw order for lanes\n"
                          "sharing a Z, and the order the ASM tables come out in.");
    ImGui::SameLine();
    ImGui::BeginDisabled(display_slot >= (int)lanes.size() - 1);
    if (ImGui::SmallButton(g_icon_font_loaded ? ICON_DOWN "##world_lane_down"
                                              : ICON_DOWN_TXT "##world_lane_down"))
        WorldMarkedMoveLane(state, lanes, display_slot, +1);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Move this row down. Row order is the draw order for\n"
                          "lanes sharing a Z, and the ASM table order.");
    ImGui::SameLine();
    if (ImGui::SmallButton(g_icon_font_loaded ? ICON_CLOSE "##world_lane_del"
                                              : ICON_CLOSE_TXT "##world_lane_del"))
        want_delete_popup = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Remove this row. Asks first and spells out what it will do --\n"
                          "a marked row leaves by unmarking its frames in that IMG.");

    ImGui::SameLine();
    ImGui::Text("Slot %d  [%d] %s", display_slot + 1, lane.doc_idx, doc_name);

    /* A hidden row stops here. Everything below this point -- three lines of
       controls and a thumbnail strip -- was still being submitted every frame
       for a row that draws nothing, and the strip is the expensive half:
       BuildWorldSpriteTexture creates an SDL texture and converts every pixel
       through the palette, per frame, per thumbnail. A hidden 20-frame row was
       paying for 20 texture builds a frame to render a strip nobody is looking
       at. The eye, the order buttons and Del stay live so it can be brought
       back, moved or dropped while collapsed. */
    if (!row_visible) {
        int n = (int)lane.frames.size();
        ImGui::SameLine();
        ImGui::TextDisabled("hidden -- %d frame%s, not drawn", n, n == 1 ? "" : "s");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Collapsed while hidden: no controls, no thumbnails,\n"
                              "no sprite textures built, and nothing drawn in the\n"
                              "world. Click the eye to bring it back.\n\n"
                              "Its timing is frozen too -- the global Ticks/frame\n"
                              "skips hidden rows, so this row keeps the hold it\n"
                              "had when you hid it.");
        return WorldDrawRowDeleteConfirm(state, lane, display_slot,
                                         want_delete_popup);
    }

    if (state.active_slot == lane.delay_slot) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.2f, 1.0f), "[KEYS]");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Left/Right controls this slot and loads each frame's real IMG anipoints.");
    }
    /* This row's speed, and whether the global still owns it. Both live here
       rather than at the end of the strip: this used to be the last thing
       submitted after Stop@, PongDelay and the rest, so on any panel that was
       not very wide it was clipped off the right edge and nobody knew a
       per-row hold existed at all.

       "Global" checked means the row inherits -- change the global Ticks/frame
       and this row moves with it, which is what every row does until it is
       deliberately taken off. Typing a value here takes it off, because that
       is the only reason to type one. */
    if (!lane.dummy_decap) {
        int slot = lane.delay_slot;
        bool own = state.slot_hold_custom[slot];
        int shown = WorldMarkedSlotHold(state, slot);
        ImGui::SameLine();
        ImGui::TextDisabled("T/f");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Ticks per frame for this row: %d tick%s, so %.4f / %d = %.2f\n"
                "rows a second. This is the sleep the ASM export writes.\n\n"
                "%s",
                shown, shown == 1 ? "" : "s", kMk2TickHz, shown,
                kMk2TickHz / (float)shown,
                own ? "Taken off the global -- the scene's Ticks/frame no longer"
                      " reaches it."
                    : "Following the global Ticks/frame.");
        ImGui::SameLine(0.0f, 4.0f);
        int want = 0;
        if (WorldDeferredIntInput("##world_lane_hold", shown, 48.0f, 0, &want))
            WorldMarkedSetSlotHold(state, slot, want, true);
        ImGui::SameLine(0.0f, 4.0f);
        bool follow = !own;
        if (ImGui::Checkbox("Global##world_lane_hold_global", &follow))
            WorldMarkedSetSlotHold(state, slot,
                                   follow ? state.default_hold : shown, !follow);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(follow
                ? "Following the scene's global Ticks/frame (%d). Change the\n"
                  "global and this row changes with it. Untick, or type a\n"
                  "value in the box, to give the row a speed of its own."
                : "Off the global: this row stays at %d no matter what the\n"
                  "scene's Ticks/frame is set to. Tick this to hand it back.",
                follow ? ClampTimelineHold(state.default_hold) : shown);
    }

    ImGui::SameLine();
    ImGui::Checkbox("Stop##world_lane_stop", &state.hold_end[lane.delay_slot]);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Hold this slot on its final entry instead of looping.");
    ImGui::SameLine();
    int stop_tick = state.stop_tick[lane.delay_slot];
    ImGui::TextDisabled("Stop@");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(54.0f);
    if (ImGui::InputInt("##world_lane_stop_tick", &stop_tick, 0, 0))
        state.stop_tick[lane.delay_slot] = ClampWorldMarkedVisibleFrom(stop_tick);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Freeze this slot at the given global preview tick. 0 disables the tick stop.");
    ImGui::SameLine();
    int pingpong_delay = state.pingpong_delay[lane.delay_slot];
    ImGui::TextDisabled("PongDelay");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(54.0f);
    if (ImGui::InputInt("##world_lane_pingpong_delay", &pingpong_delay, 0, 0))
        state.pingpong_delay[lane.delay_slot] =
            ClampWorldMarkedVisibleFrom(pingpong_delay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Extra ticks chain Ping Pong waits before it reverses for this slot.");
    if (state.chain_pingpong[lane.delay_slot]) {
        int anchor_fi = lane.frame_pos;
        if (anchor_fi < 0) anchor_fi = 0;
        if (anchor_fi >= (int)lane.frames.size()) anchor_fi = (int)lane.frames.size() - 1;
        ImGui::SameLine();
        ImGui::TextDisabled("reverses @ tick %-4d",
            WorldMarkedChainReverseStartTick(state, lane, anchor_fi,
                                             state.chain_count[lane.delay_slot],
                                             state.chain_gap[lane.delay_slot],
                                             state.chain_delay[lane.delay_slot],
                                             state.chain_vy[lane.delay_slot],
                                             state.pingpong_delay[lane.delay_slot]));
    }
    if (lane.dummy_decap) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Body##world_dummy_decap_reset")) {
            WorldResetDummyDecapDelays(state, (int)lane.frames.size());
            WorldMarkedRestart(state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restore stock decap timing: 48, 6/6, 10-tick wobble, 6-tick fall.");
    }
    ImGui::SameLine();
    ImGui::Checkbox("Together##world_lane_rigid", &state.lane_rigid[lane.delay_slot]);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Drag any frame of this row in the world and the whole run\n"
                          "moves with it, keeping the frames' relative positions.\n"
                          "Off, a drag moves only the frame on screen at that tick --\n"
                          "and the others are not visible to show they stayed behind.\n"
                          "Ctrl-drag does this on any row; blood rows start with it on.");
    bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
    if (mirror_flag) {
        ImGui::SameLine();
        ImGui::Checkbox("Mirror##world_lane_mirror", mirror_flag);
    }
    /* Z is usually a property of the row: draw priority is "which lane is in
       front", and a lane whose Z changes halfway pops through the one it
       overlaps. So this box sets the whole row at once and is the one to
       reach for.

       It is no longer the ONLY way in, though -- the frame editor has a Z for
       the selected entry. The renderer has always sorted per entry and the
       export has always annotated per entry; the array simply had no editor. */
    {
        int slot_z = 0;
        const std::vector<int> &zs = state.frame_z[lane.delay_slot];
        if (!zs.empty()) slot_z = zs[0];
        ImGui::SameLine();
        ImGui::TextDisabled("Z");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(42.0f);
        if (ImGui::InputInt("##world_lane_z", &slot_z, 0, 0)) {
            int z = ClampWorldMarkedZ(slot_z);
            for (int &entry_z : state.frame_z[lane.delay_slot])
                entry_z = z;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw priority for this whole row: higher Z draws on top\n"
                              "of other rows. Equal Z keeps the normal row order\n"
                              "(row 1 on top).\n\n"
                              "Writes every entry at once, and shows entry 1's value --\n"
                              "so a row whose Z changes partway reads as its first\n"
                              "entry here. Set a single frame's Z with the Z box in\n"
                              "the frame editor below.");
    }
    if (!lane.dummy_decap) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reverse##world_lane_reverse")) {
            if (WorldMarkedReverseSlot(state, lane)) {
                int refresh_fi = lane.frame_pos;
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, refresh_fi);
            }
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Play this row backward: plain entries reverse order, "
                              "while chain/composite/waterline entries mirror their "
                              "show/hide ticks and motion so they retrace in reverse.");
        ImGui::SameLine();
        bool can_ground_align = lane.doc == g_doc && !lane.frames.empty();
        ImGui::BeginDisabled(!can_ground_align);
        if (ImGui::SmallButton("Ground Align Anipts##world_lane_ground_align")) {
            state.active_slot = lane.delay_slot;
            int changed = WorldGroundAlignLaneAnipoints(lane);
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     changed > 0
                         ? "Ground-aligned %d anipoint%s in this slot."
                         : "This slot's anipoints are already ground-aligned.",
                     changed, changed == 1 ? "" : "s");
            g_restore_msg_timer = 4.0f;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(can_ground_align
                ? "Put every frame's primary anipoint at its lowest opaque pixel (feet/ground).\nThe first frame keeps its X axis; later frames reuse it when possible."
                : "Select a frame from this slot first so its IMG tab is active.");
    }

    EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());
    int edit_fi = lane.frame_pos;
    if (edit_fi < 0) edit_fi = 0;
    if (edit_fi >= (int)lane.frames.size()) edit_fi = (int)lane.frames.size() - 1;

    if (edit_fi >= 0) {
        ImGui::AlignTextToFramePadding();
        /* Padded: the font is fixed-advance, so %2d keeps this label one width
           from entry 1 to entry 99 and the buttons after it stop sliding
           sideways every tick. */
        ImGui::TextDisabled("Entry %2d/%2d", edit_fi + 1, (int)lane.frames.size());
        if (!lane.dummy_decap) {
            ImGui::SameLine();
            ImGui::TextDisabled("Order");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("< > move this frame earlier/later, + duplicates it, - removes it.");
            ImGui::SameLine();
            ImGui::BeginDisabled(edit_fi <= 0);
            if (ImGui::SmallButton("<##world_seq_left")) {
                WorldMarkedMoveSequenceEntry(state, lane.delay_slot, edit_fi, -1);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move this entry earlier in the animation order.");
            ImGui::SameLine();
            ImGui::BeginDisabled(edit_fi >= (int)lane.frames.size() - 1);
            if (ImGui::SmallButton(">##world_seq_right")) {
                WorldMarkedMoveSequenceEntry(state, lane.delay_slot, edit_fi, +1);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move this entry later in the animation order.");
            ImGui::SameLine();
            if (ImGui::SmallButton("+##world_seq_dup")) {
                WorldMarkedDuplicateSequenceEntry(state, lane.delay_slot, edit_fi);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Duplicate this sequence entry so the same sprite can use different local anipoints later.");
            ImGui::SameLine();
            if (ImGui::SmallButton("-##world_seq_del")) {
                WorldMarkedDeleteSequenceEntry(state, lane.delay_slot, edit_fi);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove this sequence entry. The sprite itself is not deleted.");
            /* Split is back out on the strip. v3.21.0 folded it into the
               Row... menu with the other structural edits, but it is not like
               the others: dividing a run at the frame you are looking at is
               part of laying out an animation, not a once-in-a-while rebuild,
               and it reads directly off the entry the cursor is already on.
               The destructive operations stay behind the menu. */
            bool can_split = WorldMarkedLaneCanSplit(lane, lanes) && edit_fi > 0;
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_split);
            if (ImGui::SmallButton("Split##world_seq_split")) {
                if (WorldMarkedSplitLaneAtFrame(state, lane, lanes, edit_fi))
                    WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                if (lane.delay_slot == kWorldEmbeddedSeqScrSlot)
                    ImGui::SetTooltip(
                        "Move this entry and all later entries into a new row.\n\n"
                        "This lane is showing an embedded sequence. Splitting it\n"
                        "detaches the preview from that record — the sequence\n"
                        "saved in the IMG is left exactly as it is.");
                else
                    ImGui::SetTooltip("Move this entry and all later entries into a new row from the same IMG.");
            }

            /* Structural edits — the ones that add, remove or rebuild whole
               rows — are grouped away from the per-entry nudges. They are
               used once in a while and are the most destructive things here,
               so they should not sit a stray click away from "move entry
               later". */
            /* Promote lives on the strip rather than in the menu because it
               is the end of the job: the row has been staged, and this is
               what turns it into something the IMG can hold. */
            /* No longer gated on the row's file being the active tab -- the
               dialog asks which tab to write into, and switches to it. */
            bool can_promote = !lane.frames.empty() && document_tab_count() > 0;
            ImGui::SameLine();
            ImGui::BeginDisabled(!can_promote);
            if (ImGui::SmallButton("To Seq...##world_seq_promote"))
                WorldOpenPromoteSeqDialog(lane);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip(can_promote
                    ? "Write this row into an IMG as a new SEQSCR sequence:\n"
                      "one entry per row entry, carrying sprite, ticks, dX and dY.\n\n"
                      "Asks for a name and which open tab to write into first.\n"
                      "Frames living in other tabs are brought into that IMG so the\n"
                      "entry can name them -- reused if a sprite of that name is\n"
                      "already there, copied in with its palette if not.\n\n"
                      "Flips, Z, motion, Show@/Hide@ and dual are preview-only and\n"
                      "cannot be stored in an entry -- the toast names what was left.\n"
                      "Open it afterwards from the Anim tab."
                    : "This row has no frames to promote.");

            ImGui::SameLine();
            if (ImGui::SmallButton("Row...##world_seq_row_menu"))
                ImGui::OpenPopup("##world_seq_row_popup");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Split, duplicate, delete or rebuild this row.");
            if (ImGui::BeginPopup("##world_seq_row_popup")) {
                if (ImGui::MenuItem("Split Row Here", NULL, false, can_split)) {
                    if (WorldMarkedSplitLaneAtFrame(state, lane, lanes, edit_fi))
                        WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Move this entry and all later entries into a new row from the same IMG.");

                bool can_duplicate = lane.delay_slot >= 0 &&
                                     lane.delay_slot < kWorldMarkedSourceTabs &&
                                     WorldMarkedFindFreeSplitSlot(lanes) >= 0;
                if (ImGui::MenuItem("Duplicate Row", NULL, false, can_duplicate)) {
                    if (WorldMarkedDuplicateSlot(state, lane, lanes))
                        WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
                }
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    ImGui::SetTooltip("Copy this whole row into a new editable World View slot.");

                ImGui::Separator();
                if (ImGui::MenuItem("Reset Row to Marked")) {
                    WorldMarkedResetSequenceToDefaults(state, lane.delay_slot);
                    WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Rebuild this lane from the currently marked sprites and clear local sequence offsets.");

                /* Every row type can go now, not just split ones -- the
                   confirm below names what removing this particular row
                   means before anything happens. */
                if (ImGui::MenuItem("Delete Row..."))
                    want_delete_popup = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove this row from World View and the ASM export.");
                ImGui::EndPopup();
            }
        }

        int delay = state.frame_delays[lane.delay_slot][edit_fi];
        int local_dx = state.local_dx[lane.delay_slot][edit_fi];
        int local_dy = state.local_dy[lane.delay_slot][edit_fi];
        int show_at = state.visible_from[lane.delay_slot][edit_fi];
        int hide_at = state.visible_until[lane.delay_slot][edit_fi];
        int motion_dx = state.motion_dx[lane.delay_slot][edit_fi];
        int motion_dy = state.motion_dy[lane.delay_slot][edit_fi];
        int motion_cap_x = state.motion_cap_x[lane.delay_slot][edit_fi];
        int motion_cap_y = state.motion_cap_y[lane.delay_slot][edit_fi];
        /* DEFERRED, and this box above all the others on this row.
           ClampTimelineHold floors at 1, so a per-keystroke commit does not
           merely show a wrong number for an instant - clearing the field to
           retype it commits the empty value as 1, WorldMarkedSetSlotHold-style
           retiming runs, and the box is rewritten from that 1 under the caret
           so the digits meant for it land somewhere else. The visible result is
           a row that will not hold anything but 1 no matter how often you type
           into it, and it is intermittent: overtyping a single digit survives,
           clear-then-type does not. Every blood row in data/sliceblood.WAX came
           out at 1 tick this way while the dive lane authored beside it kept
           its 3s and 4s. */
        ImGui::SameLine();
        ImGui::TextDisabled("Delay");
        ImGui::SameLine();
        if (WorldDeferredIntInput("##world_edit_delay", delay, 38.0f, 0, &delay))
            state.frame_delays[lane.delay_slot][edit_fi] = ClampTimelineHold(delay);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Repeat count for this sequence entry.");
        ImGui::SameLine();
        ImGui::TextDisabled("dAX");
        ImGui::SameLine();
        if (WorldDeferredIntInput("##world_edit_dax", local_dx, 46.0f, 0, &local_dx))
            state.local_dx[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(local_dx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Local anipoint X delta for this entry. You can also drag the sprite in the world canvas.");
        ImGui::SameLine();
        ImGui::TextDisabled("dAY");
        ImGui::SameLine();
        if (WorldDeferredIntInput("##world_edit_day", local_dy, 46.0f, 0, &local_dy))
            state.local_dy[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(local_dy);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Local anipoint Y delta for this entry. Positive values move the effective anipoint down.");
        /* This ENTRY's draw priority. The row-level Z next to Mirror writes
           every entry at once, which is the right default -- draw priority is
           usually "which lane is in front" and a lane that changes Z halfway
           pops through whatever it overlaps. But a hand passing behind a torso
           for three frames and in front for the rest is a real pose, the
           renderer has always sorted per entry, and the ASM export already
           annotates each one, so the array was the only thing without a way to
           edit it. */
        ImGui::SameLine();
        ImGui::TextDisabled("Z");
        ImGui::SameLine();
        int entry_z = state.frame_z[lane.delay_slot][edit_fi];
        if (WorldDeferredIntInput("##world_edit_z", entry_z, 42.0f, 0, &entry_z))
            state.frame_z[lane.delay_slot][edit_fi] = ClampWorldMarkedZ(entry_z);
        if (ImGui::IsItemHovered()) {
            bool uniform = true;
            const std::vector<int> &zs = state.frame_z[lane.delay_slot];
            for (size_t zi = 1; zi < zs.size() && uniform; zi++)
                if (zs[zi] != zs[0]) uniform = false;
            ImGui::SetTooltip(
                "Draw priority for THIS entry only: higher draws on top.\n"
                "Equal Z falls back to row order (row 1 on top).\n\n"
                "%s\n\n"
                "The Z box on the row above sets every entry at once.",
                uniform ? "Every entry in this row currently shares one Z."
                        : "This row's entries do NOT all share a Z -- it changes\n"
                          "partway through, which the preview and the export\n"
                          "both honour.");
        }
        int mirror_bits = state.frame_mirror[lane.delay_slot][edit_fi];
        bool flip_x = (mirror_bits & kWorldFrameMirrorX) != 0;
        bool flip_y = (mirror_bits & kWorldFrameMirrorY) != 0;
        ImGui::SameLine();
        if (ImGui::Checkbox("Flip X##world_edit_flip_x", &flip_x)) {
            if (flip_x) mirror_bits |= kWorldFrameMirrorX;
            else mirror_bits &= ~kWorldFrameMirrorX;
            state.frame_mirror[lane.delay_slot][edit_fi] =
                ClampWorldMarkedFrameMirror(mirror_bits);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mirror this entry horizontally. Combines with the slot-level Mirror toggle.");
        ImGui::SameLine();
        if (ImGui::Checkbox("Flip Y##world_edit_flip_y", &flip_y)) {
            if (flip_y) mirror_bits |= kWorldFrameMirrorY;
            else mirror_bits &= ~kWorldFrameMirrorY;
            state.frame_mirror[lane.delay_slot][edit_fi] =
                ClampWorldMarkedFrameMirror(mirror_bits);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Mirror this entry vertically around its anipoint.");
        /* Scheduling, motion and the second instance are per-entry but rarely
           touched, and inline they tripled the length of this row. They live
           behind one button now; the button says when the entry is using any
           of them, so nothing goes quietly missing. */
        bool dual = state.dual_on[lane.delay_slot][edit_fi] != 0;
        bool has_extras = show_at || hide_at || motion_dx || motion_dy ||
                          motion_cap_x || motion_cap_y || dual;
        ImGui::SameLine();
        /* The marker is a colour, not an extra character: a label that grew by
           a "*" as playback stepped onto an entry with extras moved every
           control to its right. */
        if (has_extras)
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.78f, 0.20f, 1.0f));
        bool more_clicked = ImGui::SmallButton("Timing/FX##world_edit_more");
        if (has_extras)
            ImGui::PopStyleColor();
        if (more_clicked)
            ImGui::OpenPopup("##world_edit_more_popup");
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(has_extras
                ? "Show/Hide ticks, per-tick motion, and the second sprite copy.\nThis entry is using some of them."
                : "Show/Hide ticks, per-tick motion, and the second sprite copy.");
        if (ImGui::BeginPopup("##world_edit_more_popup")) {
            ImGui::TextDisabled("Entry %d/%d", edit_fi + 1, (int)lane.frames.size());
            ImGui::Separator();

            ImGui::TextDisabled("Schedule (global preview ticks)");
            if (WorldDeferredIntInput("Show@##world_edit_show", show_at, 90.0f, 1, &show_at))
                state.visible_from[lane.delay_slot][edit_fi] = ClampWorldMarkedVisibleFrom(show_at);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hide this entry until the global preview tick reaches this value.");
            if (WorldDeferredIntInput("Hide@##world_edit_hide", hide_at, 90.0f, 1, &hide_at))
                state.visible_until[lane.delay_slot][edit_fi] = ClampWorldMarkedVisibleUntil(hide_at);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Hide this entry once the global preview tick reaches this value. 0 disables the cutoff.");

            ImGui::Separator();
            ImGui::TextDisabled("Motion (pixels per tick)");
            if (WorldDeferredIntInput("vX##world_edit_vx", motion_dx, 90.0f, 1, &motion_dx))
                state.motion_dx[lane.delay_slot][edit_fi] = ClampWorldMarkedMotion(motion_dx);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Visual X motion in pixels per tick. Positive moves this entry right.");
            if (WorldDeferredIntInput("vY##world_edit_vy", motion_dy, 90.0f, 1, &motion_dy))
                state.motion_dy[lane.delay_slot][edit_fi] = ClampWorldMarkedMotion(motion_dy);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Visual Y motion in pixels per tick. Positive moves this entry down.");
            ImGui::BeginDisabled(!(motion_dx || motion_dy || motion_cap_x || motion_cap_y));
            if (WorldDeferredIntInput("StopY##world_edit_stop_y", motion_cap_y, 90.0f, 1, &motion_cap_y))
                state.motion_cap_y[lane.delay_slot][edit_fi] =
                    ClampWorldMarkedMotionCap(motion_cap_y);
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Maximum visual Y travel before vY stops. 0 keeps moving.");

            ImGui::Separator();
            if (ImGui::Checkbox("Dual##world_edit_dual", &dual))
                state.dual_on[lane.delay_slot][edit_fi] = dual ? 1 : 0;
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draw this entry's sprite a second time in the same frame,\n"
                                  "at its own local offset and Z. Drag either copy in the world canvas.");
            if (dual) {
                int dual_dx = state.dual_dx[lane.delay_slot][edit_fi];
                int dual_dy = state.dual_dy[lane.delay_slot][edit_fi];
                int dual_z = state.dual_z[lane.delay_slot][edit_fi];
                if (WorldDeferredIntInput("dAX2##world_edit_dax2", dual_dx, 90.0f, 1, &dual_dx))
                    state.dual_dx[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(dual_dx);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Local anipoint X delta for the second copy of this sprite.");
                if (WorldDeferredIntInput("dAY2##world_edit_day2", dual_dy, 90.0f, 1, &dual_dy))
                    state.dual_dy[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(dual_dy);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Local anipoint Y delta for the second copy of this sprite.");
                if (WorldDeferredIntInput("Z2##world_edit_z2", dual_z, 90.0f, 1, &dual_z))
                    state.dual_z[lane.delay_slot][edit_fi] = ClampWorldMarkedZ(dual_z);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Draw priority for the second copy. Lower than the slot Z puts\n"
                                      "it behind the first copy (and behind lanes it sorts under).");
            }
            ImGui::EndPopup();
        }

        if (!lane.dummy_decap &&
            lane.delay_slot >= 0 && lane.delay_slot < kWorldMarkedMaxTabs) {
            int slot = lane.delay_slot;
            if (state.chain_gap[slot] <= 0)
                state.chain_gap[slot] = WorldMarkedSpriteHeightForChain(lane, edit_fi);
            WorldMarkedClampAutoChainSettings(state, slot);
            int &auto_step = state.auto_step[slot];
            int &auto_life = state.auto_life[slot];
            int &auto_vx = state.auto_vx[slot];
            int &auto_vy = state.auto_vy[slot];
            int &auto_y = state.auto_y[slot];
            int &chain_count = state.chain_count[slot];
            int &chain_gap = state.chain_gap[slot];
            int &chain_delay = state.chain_delay[slot];
            int &chain_vy = state.chain_vy[slot];
            bool &chain_pingpong = state.chain_pingpong[slot];
            int &pingpong_delay = state.pingpong_delay[slot];

            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Auto Y");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Apply motion from the selected entry onward. Downward motion hides when the sprite bottom passes Y; upward motion hides when the top passes Y.");
            ImGui::SameLine();
            ImGui::TextDisabled("Step");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(38.0f);
            if (ImGui::InputInt("##world_auto_step", &auto_step, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ticks between each subframe's Show@ value.");
            ImGui::SameLine();
            ImGui::TextDisabled("Life");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42.0f);
            if (ImGui::InputInt("##world_auto_life", &auto_life, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Fallback visible ticks when vY is 0 or no Y breach can be calculated.");
            ImGui::SameLine();
            ImGui::TextDisabled("vX");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42.0f);
            if (ImGui::InputInt("##world_auto_vx", &auto_vx, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Visual X motion in pixels per tick.");
            ImGui::SameLine();
            ImGui::TextDisabled("vY");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42.0f);
            if (ImGui::InputInt("##world_auto_vy", &auto_vy, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Visual Y motion in pixels per tick. Positive moves down, negative moves up.");
            ImGui::SameLine();
            ImGui::TextDisabled("Y");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.0f);
            if (ImGui::InputInt("##world_auto_y", &auto_y, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("World Y breach line.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Apply From Entry##world_auto_apply")) {
                WorldMarkedApplyAutoYChain(state, lane, edit_fi,
                                           auto_step, auto_life,
                                           auto_vx, auto_vy, auto_y);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Set Show@, Hide@, and vX/vY from this entry through the end of the row.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clear v##world_auto_clear")) {
                WorldMarkedClearMotionFrom(state, lane.delay_slot, edit_fi);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Clear vX/vY from this entry through the end of the row.");

            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled("Chain");
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Stack the selected sprite into a falling chain. New copies spawn above and feed down; it holds once Count copies exist (or ping-pongs back if enabled).");
            ImGui::SameLine();
            ImGui::TextDisabled("Count");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(38.0f);
            if (ImGui::InputInt("##world_chain_count", &chain_count, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Number of copies in the chain, including the anchored first link.");
            ImGui::SameLine();
            ImGui::TextDisabled("Gap");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42.0f);
            if (ImGui::InputInt("##world_chain_gap", &chain_gap, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Pixels between each stacked copy. Set this to the sprite's pixel length for a seamless, gap-free stack.");
            ImGui::SameLine();
            ImGui::TextDisabled("Delay");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42.0f);
            if (ImGui::InputInt("##world_chain_delay", &chain_delay, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Ticks between linked copies. 0 uses Gap/vY so a new top copy starts as the prior one clears one Gap.");
            ImGui::SameLine();
            ImGui::TextDisabled("vY");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(42.0f);
            if (ImGui::InputInt("##world_chain_vy", &chain_vy, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Visual Y speed for moving links. Positive moves down.");
            ImGui::SameLine();
            ImGui::Checkbox("Ping Pong##world_chain_pingpong",
                            &chain_pingpong);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Append a delayed reverse pass so linked copies retract in the opposite order.");
            ImGui::SameLine();
            ImGui::TextDisabled("PongDelay");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(48.0f);
            if (ImGui::InputInt("##world_chain_pingpong_delay",
                                &pingpong_delay, 0, 0))
                WorldMarkedClampAutoChainSettings(state, slot);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Extra ticks to hold the fully extended chain before the reverse pass starts.");
            if (chain_pingpong) {
                ImGui::SameLine();
                ImGui::TextDisabled("reverses @ tick %-4d",
                    WorldMarkedChainReverseStartTick(state, lane, edit_fi, chain_count,
                                                     chain_gap, chain_delay, chain_vy,
                                                     pingpong_delay));
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Build Chain##world_chain_apply")) {
                WorldMarkedApplyYLinkChain(state, lane, edit_fi,
                                           chain_count, chain_gap,
                                           chain_delay, chain_vy,
                                           chain_pingpong,
                                           pingpong_delay);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Replace the same-sprite run starting here with Count linked copies.");

            if (WorldDrawSubframeSwapTool(state, lane, edit_fi, false))
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
        }
    }

    WorldDrawPromoteSeqDialog(state, lane);
    return WorldDrawRowDeleteConfirm(state, lane, display_slot, want_delete_popup);
}

struct WorldLaneFramePayload {
    int slot;
    int frame_idx;
};

static const char *kWorldLaneFramePayloadType = "WORLD_LANE_FRAME";

static bool WorldNameLooksBloody(const std::string &upper)
{
    static const char *kWords[] = {
        "BLOOD", "SPILL", "SPURT", "DRIP", "SPRAY", "SPLAT", "GUTS", "GORE"
    };
    for (size_t i = 0; i < sizeof(kWords) / sizeof(kWords[0]); i++)
        if (upper.find(kWords[i]) != std::string::npos) return true;
    return false;
}

/* A file name, not a sprite name: BLOOD.IMG and MK1BLOOD.IMG qualify every
   sprite they hold, however it is named. */
static bool WorldFileLooksBloody(const char *fname)
{
    if (!fname) return false;
    std::string upper(fname);
    for (size_t i = 0; i < upper.size(); i++)
        upper[i] = (char)toupper((unsigned char)upper[i]);
    return upper.find("BLOOD") != std::string::npos;
}

/* Uppercased, trimmed names of every sprite in every open tab, sorted. A
   subframe is then told from a lone sprite by whether its inferred parent is
   really open -- across tabs, because a chop split over two files still has
   all its pieces -- with a binary search instead of a walk of every image
   list per sprite. */
static void WorldCollectOpenSpriteNames(std::vector<std::string> &out)
{
    out.clear();
    for (int di = 0; di < document_tab_count(); di++) {
        Document *doc = document_get(di);
        if (!doc) continue;
        for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p) {
            std::string name = trim_sprite_name(img_name_string(img));
            if (name.empty()) continue;
            for (size_t i = 0; i < name.size(); i++)
                name[i] = (char)toupper((unsigned char)name[i]);
            out.push_back(name);
        }
    }
    std::sort(out.begin(), out.end());
}

void WorldCollectBloodRuns(std::vector<WorldBloodRun> &out)
{
    out.clear();
    std::vector<std::string> open_names;
    WorldCollectOpenSpriteNames(open_names);

    for (int di = 0; di < document_tab_count(); di++) {
        Document *doc = document_get(di);
        if (!doc) continue;
        const char *doc_name = doc->fname_s[0] ? doc->fname_s : "Untitled";
        bool doc_is_blood = WorldFileLooksBloody(doc->fname_s);

        int idx = 0;
        for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
            if (!img->data_p || img->w <= 0 || img->h <= 0) continue;
            std::string name = trim_sprite_name(img_name_string(img));
            if (name.empty()) continue;
            std::string upper = name;
            for (size_t i = 0; i < upper.size(); i++)
                upper[i] = (char)toupper((unsigned char)upper[i]);
            if (!doc_is_blood && !WorldNameLooksBloody(upper)) continue;

            /* No subframes, ever. SPILL1A/SPILL1B are pieces of SPILL1, not
               steps of the spray: imported as frames they double the run's
               length and play its halves as separate images. The parent is
               the frame; a row that wants the pieces gets them from the
               parent, the way every other World View row does. */
            std::string parent = InferSubframeParentName(upper.c_str());
            if (!parent.empty() &&
                std::binary_search(open_names.begin(), open_names.end(), parent))
                continue;

            /* SPILL1..SPILL13 is one run; a lone SPLAT is a run of one. */
            std::string stem;
            if (!strip_trailing_sequence_digits(upper, &stem) || stem.empty())
                stem = upper;

            WorldBloodRun *run = NULL;
            for (size_t r = 0; r < out.size(); r++) {
                if (out[r].doc_idx == di && out[r].stem == stem) {
                    run = &out[r];
                    break;
                }
            }
            if (!run) {
                WorldBloodRun fresh;
                fresh.doc_idx = di;
                fresh.doc_name = doc_name;
                fresh.stem = stem;
                out.push_back(fresh);
                run = &out.back();
            }
            run->frames.push_back(idx);
        }
    }
}

/* ---- Finding the blood art on disk -------------------------------------
   The picker can only offer what is open, and nobody opens BLOOD.IMG before
   they need it -- the request is always "start blood here", never "go and
   open the blood file first". So look for it: the IMGs already open say which
   folders this project keeps its art in, and a file whose name says BLOOD is
   the art. */
/* Collapse "a\b\..\c" to "a\c" and drop "." segments. The scan builds
   candidate folders by appending "data" and "..\data" to each open tab's
   path, so one folder arrives spelled three ways -- src\..\data,
   data\..\data, src\..\data\..\data. A raw string compare calls those
   distinct, and BLOOD.IMG got opened once per spelling: six tabs for two
   files. Normalise before deduping. */
static std::string WorldNormalizeDir(const std::string &dir)
{
    std::vector<std::string> parts;
    std::string seg;
    bool unc = dir.size() >= 2 && (dir[0] == '\\' || dir[0] == '/') &&
                                  (dir[1] == '\\' || dir[1] == '/');
    for (size_t i = 0; i <= dir.size(); i++) {
        char c = (i < dir.size()) ? dir[i] : '\\';
        if (c != '\\' && c != '/') { seg += c; continue; }
        if (seg.empty() || seg == ".") { seg.clear(); continue; }
        if (seg == ".." && !parts.empty() && parts.back() != ".." &&
            !(parts.size() == 1 && parts[0].size() >= 2 && parts[0][1] == ':')) {
            parts.pop_back();
            seg.clear();
            continue;
        }
        parts.push_back(seg);
        seg.clear();
    }
    std::string out;
    if (unc) out = "\\\\";
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += "\\";
        out += parts[i];
    }
    return out.empty() ? dir : out;
}

static void WorldAddBloodScanDir(std::vector<std::string> &dirs,
                                 const std::string &raw_dir)
{
    std::string dir = WorldNormalizeDir(raw_dir);
    if (dir.empty()) return;
    std::string low = dir;
    for (size_t i = 0; i < low.size(); i++)
        low[i] = (char)tolower((unsigned char)low[i]);
    for (size_t i = 0; i < dirs.size(); i++) {
        std::string ex = dirs[i];
        for (size_t c = 0; c < ex.size(); c++)
            ex[c] = (char)tolower((unsigned char)ex[c]);
        if (ex == low) return;
    }
    dirs.push_back(dir);
}

/* Shared with the SEQSCR frame browser further down: both ask "is this file
   already open?", and both have to answer it the way Windows would. */
static bool WorldPathsEqual(const std::string &a, const std::string &b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = (char)tolower((unsigned char)a[i]);
        char cb = (char)tolower((unsigned char)b[i]);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
    }
    return true;
}

static std::string WorldDocFullPath(const Document *doc)
{
    if (!doc || !doc->fname_s[0]) return std::string();
    return PathCombine(doc->fpath_s, doc->fname_s);
}

void WorldFindBloodImgFiles(std::vector<WorldBloodFile> &out, bool force_rescan)
{
    /* The scan hits the filesystem, and the picker asks for this list every
       frame it is open. Cache it, and re-run only when the set of open tabs
       changed -- which is also the only thing that can change where we look. */
    static std::vector<WorldBloodFile> s_found;
    static int s_scanned_tabs = -1;
    static std::string s_scanned_active;

    std::string active_dir = g_doc ? std::string(g_doc->fpath_s) : std::string();
    if (force_rescan || s_scanned_tabs != document_tab_count() ||
        s_scanned_active != active_dir) {
        s_scanned_tabs = document_tab_count();
        s_scanned_active = active_dir;
        s_found.clear();

        std::vector<std::string> dirs;
        for (int t = 0; t < document_tab_count(); t++) {
            Document *doc = document_get(t);
            if (doc && doc->fpath_s[0]) WorldAddBloodScanDir(dirs, doc->fpath_s);
        }
        if (g_doc && g_doc->fpath_s[0]) WorldAddBloodScanDir(dirs, g_doc->fpath_s);
        const char *imgdir = getenv("IMGDIR");
        if (imgdir && imgdir[0]) WorldAddBloodScanDir(dirs, imgdir);
        /* MK2's tree keeps sprites in data/ beside the sources, so a tab
           opened from src/ still finds the blood next door. */
        size_t roots = dirs.size();
        for (size_t i = 0; i < roots; i++) {
            std::string root = dirs[i];
            WorldAddBloodScanDir(dirs, PathCombine(root, "data"));
            WorldAddBloodScanDir(dirs, PathCombine(PathCombine(root, ".."), "data"));
        }

        for (size_t i = 0; i < dirs.size(); i++) {
            std::vector<FileEntry> entries;
            GetDirectoryFiles(dirs[i], entries, "IMG");
            for (size_t e = 0; e < entries.size(); e++) {
                if (entries[e].is_dir) continue;
                if (!WorldFileLooksBloody(entries[e].name.c_str())) continue;
                WorldBloodFile f;
                f.path = PathCombine(dirs[i], entries[e].name);
                f.name = entries[e].name;
                bool dup = false;
                for (size_t k = 0; k < s_found.size() && !dup; k++)
                    dup = WorldPathsEqual(s_found[k].path, f.path);
                if (!dup) s_found.push_back(f);
            }
        }
    }

    /* Which of them are open changes without the scan needing to: recompute
       it every call rather than caching a flag that goes stale on a tab. */
    for (size_t i = 0; i < s_found.size(); i++) {
        s_found[i].open = false;
        for (int t = 0; t < document_tab_count() && !s_found[i].open; t++) {
            std::string open_path = WorldDocFullPath(document_get(t));
            if (!open_path.empty() && WorldPathsEqual(open_path, s_found[i].path))
                s_found[i].open = true;
        }
    }
    out = s_found;
}

int WorldOpenBloodImgFiles(void)
{
    std::vector<WorldBloodFile> files;
    WorldFindBloodImgFiles(files, true);

    /* Opening a tab makes it active. The user asked for blood on the row they
       were looking at, not for the blood file to take over the editor, so put
       the tab back afterwards. Identify it by uid: indices shift. */
    unsigned int was_active = document_uid(document_active_index());

    /* A data folder can hold more blood IMGs than anyone wants tabs for. */
    const int kMaxOpen = 8;
    int opened = 0;
    std::string last;
    for (size_t i = 0; i < files.size() && opened < kMaxOpen; i++) {
        if (files[i].open) continue;
        OpenImgFile(files[i].path);
        last = files[i].name;
        opened++;
    }
    if (opened > 0) {
        /* document_set_active rather than ActivateDocumentTab: going back is
           not navigation, and the reset that rides along with a real tab
           switch would throw away the World View staging we came from. */
        int back = document_index_of_uid(was_active);
        if (back >= 0) {
            document_set_active(back);
            g_doc_tab_select_request = back;
        }
        WorldFindBloodImgFiles(files, true);   /* refresh the cache we invalidated */
        if (opened == 1)
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Opened %s for its blood runs.", last.c_str());
        else
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Opened %d blood IMGs for their runs.", opened);
        g_restore_msg_timer = 4.0f;
    }
    return opened;
}

bool WorldMarkedCreateBloodLane(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldMarkedLane &src_lane,
                                int src_entry,
                                const WorldBloodRun &run,
                                std::string *out_msg)
{
    char msg[256];
    int src_slot = src_lane.delay_slot;
    if (src_slot < 0 || src_slot >= kWorldMarkedMaxTabs || run.frames.empty() ||
        src_entry < 0 || src_entry >= (int)src_lane.frames.size()) {
        if (out_msg) *out_msg = "Could not add a blood row here.";
        return false;
    }
    if (!document_get(run.doc_idx)) {
        if (out_msg) *out_msg = "That blood IMG is no longer open.";
        return false;
    }

    int slot = WorldMarkedFindFreeSplitSlot(lanes);
    if (slot < 0) {
        snprintf(msg, sizeof(msg),
                 "No free World View row -- all %d are in use.",
                 kWorldMarkedSourceTabs);
        if (out_msg) *out_msg = msg;
        return false;
    }

    /* When the spray starts, and where. The tick is the one clicking that
       thumbnail would jump to; the offset is the entry's effective local
       delta at that tick, so the blood sits on the same anipoint the frame
       is drawn from -- motion and all. */
    int n_src = (int)src_lane.frames.size();
    int start_tick = WorldMarkedTickForFrame(state, src_slot, n_src, src_entry);
    int anchor_dx = 0, anchor_dy = 0;
    WorldMarkedEffectiveLocalDelta(state, src_slot, n_src, src_entry, false,
                                   start_tick, &anchor_dx, &anchor_dy);

    WorldMarkedClearSequenceState(state, slot);
    state.sequence_frames[slot] = run.frames;
    state.default_frames[slot] = run.frames;
    state.entry_pieces[slot].clear();
    WorldMarkedSetRowDoc(state, slot, run.doc_idx);
    state.lane_visible[slot] = true;
    state.hold_end[slot] = true;      /* a spray plays once */
    state.stop_tick[slot] = 0;
    /* Every blood row starts on the anipoint of the frame that spawned it, so
       two sprays added from the same hit land exactly on top of each other and
       have to be dragged apart. Rigid by default: grab any frame and the whole
       spray travels with it. */
    state.lane_rigid[slot] = true;

    int n = (int)run.frames.size();
    EnsureWorldMarkedFrameDelays(state, slot, n);

    /* Scheduled, not looped: consecutive Show@/Hide@ windows are how this
       panel already draws a timed subframe (it is what Build Chain emits), so
       the run fires once at start_tick and then stops, whatever the rest of
       the scene is doing on its own clocks.

       The row comes in at one tick per frame rather than at MK2's own blood
       speed (ani speed 5 in MKBLOOD.ASM). That speed is what the finished
       effect plays at, not what it is authored at: imported at 5 the spray
       only lands on every fifth tick and can no longer be walked onto the
       exact tick of the hit that caused it. Set the speed afterwards with the
       row's Ticks/frame, which is one edit; un-quantising it is not. */
    int hold = ClampTimelineHold(kWorldBloodTicksPerFrame);
    /* Imported at 1 tick, but NOT pinned: the global Ticks/frame still owns
       this row. Pinning it here meant a spray silently ignored the scene's
       speed control forever after -- and because the row is scheduled, the
       only thing that ever retimed it was code that also re-lays its
       Show@/Hide@ windows. Pin it from the row's own T/f if a spray really
       does need to run at a different rate from the hit it came from. */
    state.slot_hold[slot] = hold;
    state.slot_hold_custom[slot] = false;
    int t = start_tick;
    for (int i = 0; i < n; i++) {
        state.frame_delays[slot][i] = hold;
        state.local_dx[slot][i] = ClampWorldMarkedAniptDelta(anchor_dx);
        state.local_dy[slot][i] = ClampWorldMarkedAniptDelta(anchor_dy);
        state.visible_from[slot][i] = ClampWorldMarkedVisibleFrom(t);
        state.visible_until[slot][i] = ClampWorldMarkedVisibleUntil(t + hold);
        t += hold;
    }

    state.split_lanes.push_back(WorldMarkedSplitLane{slot, run.doc_idx});

    /* Sit directly under the row that spawned it rather than at the bottom of
       the panel -- the two are read together. */
    int src_rank = state.lane_order[src_slot];
    if (src_rank >= 0) {
        for (int s = 0; s < kWorldMarkedMaxTabs; s++)
            if (s != slot && state.lane_order[s] > src_rank) state.lane_order[s]++;
        state.lane_order[slot] = src_rank + 1;
    } else {
        state.lane_order[slot] = -1;
    }

    state.paused = true;
    WorldMarkedSetTick(state, start_tick);

    snprintf(msg, sizeof(msg),
             "Blood row: %s x%d from %s, starting at tick %d on this frame's "
             "anipoint at %d tick%s per frame. Drag any frame to move the "
             "whole spray.",
             run.stem.c_str(), n, run.doc_name.c_str(), start_tick,
             hold, hold == 1 ? "" : "s");
    if (out_msg) *out_msg = msg;
    return true;
}

/* ---- The blood picker ---------------------------------------------------
   BLOOD.IMG is a couple of hundred sprites in twenty-odd runs, and once the
   file name alone qualifies a file, MK1BLOOD.IMG stacks on top of that. A
   flat menu of every run is taller than the screen and unusable, so the runs
   live in a scrolling list with a filter box: type "spill", or scroll to the
   thumbnail you recognise. */
static bool WorldBloodRunMatchesFilter(const WorldBloodRun &run, const char *filter)
{
    if (!filter || !filter[0]) return true;
    std::string needle(filter);
    for (size_t i = 0; i < needle.size(); i++)
        needle[i] = (char)tolower((unsigned char)needle[i]);
    std::string hay = run.stem + " " + run.doc_name;
    for (size_t i = 0; i < hay.size(); i++)
        hay[i] = (char)tolower((unsigned char)hay[i]);
    return hay.find(needle) != std::string::npos;
}

static void WorldDrawBloodRunPicker(WorldMarkedSequenceState &state,
                                    const std::vector<WorldMarkedLane> &lanes,
                                    const WorldMarkedLane &lane, int entry)
{
    static char s_filter[48] = "";

    std::vector<WorldBloodRun> runs;
    WorldCollectBloodRuns(runs);

    std::vector<WorldBloodFile> files;
    WorldFindBloodImgFiles(files, false);
    int unopened = 0;
    for (size_t i = 0; i < files.size(); i++)
        if (!files[i].open) unopened++;

    /* Auto-populate. Nobody opens BLOOD.IMG before they need it -- the request
       is always "start blood here", never "go and open the blood file first"
       -- so when the list would be empty and the art is sitting next to the
       IMGs already open, fetch it instead of explaining how to. Only on the
       frame the menu opens, and only when there is nothing to show: past that
       point opening tabs is the user's call, on the button below. */
    if (runs.empty() && unopened > 0 && ImGui::IsWindowAppearing()) {
        if (WorldOpenBloodImgFiles() > 0) {
            WorldCollectBloodRuns(runs);
            WorldFindBloodImgFiles(files, false);
            unopened = 0;
            for (size_t i = 0; i < files.size(); i++)
                if (!files[i].open) unopened++;
        }
    }

    int start_tick = WorldMarkedTickForFrame(state, lane.delay_slot,
                                             (int)lane.frames.size(), entry);
    const int hold = ClampTimelineHold(kWorldBloodTicksPerFrame);
    ImGui::TextDisabled("Starts at tick %d, %d tick%s per frame, plays once.",
                        start_tick, hold, hold == 1 ? "" : "s");

    ImGui::SetNextItemWidth(220.0f);
    ImGui::InputTextWithHint("##blood_filter", "filter (spill, guts, ...)",
                             s_filter, sizeof(s_filter));
    ImGui::SameLine();
    if (ImGui::SmallButton("Clear##blood_filter_clear")) s_filter[0] = 0;

    const float row_h = 34.0f;
    const float box = row_h - 8.0f;
    ImGui::BeginChild("##blood_runs", ImVec2(300.0f, 232.0f), true,
                      ImGuiWindowFlags_AlwaysVerticalScrollbar);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImU32 col_text = ImGui::GetColorU32(ImGuiCol_Text);
    ImU32 col_dim  = ImGui::GetColorU32(ImGuiCol_TextDisabled);
    int shown = 0;
    for (size_t r = 0; r < runs.size(); r++) {
        const WorldBloodRun &run = runs[r];
        if (!WorldBloodRunMatchesFilter(run, s_filter)) continue;
        shown++;

        ImGui::PushID((int)r);
        ImVec2 pos = ImGui::GetCursorScreenPos();
        /* Only the rows on screen pay for a texture: BuildWorldSpriteTexture
           converts every pixel through the palette on each call, and a list
           of two hundred runs would do that two hundred times a frame. */
        bool row_on_screen = ImGui::IsRectVisible(ImVec2(280.0f, row_h));
        bool pick = ImGui::Selectable("##blood_pick", false,
                                      ImGuiSelectableFlags_None,
                                      ImVec2(0.0f, row_h));

        /* The middle frame, not the first: a spray opens with a couple of
           near-empty frames, and a thumbnail of nothing identifies nothing. */
        Document *run_doc = document_get(run.doc_idx);
        IMG *thumb = (run_doc && !run.frames.empty())
                   ? doc_get_img(run_doc, run.frames[run.frames.size() / 2])
                   : NULL;
        SDL_Texture *tex = row_on_screen
                         ? BuildWorldSpriteTexture(run_doc, thumb, 255) : NULL;
        if (tex && thumb->w > 0 && thumb->h > 0) {
            float longest = (float)(thumb->w > thumb->h ? thumb->w : thumb->h);
            float scale = box / longest;
            float w = thumb->w * scale, h = thumb->h * scale;
            ImVec2 a(pos.x + 4.0f + (box - w) * 0.5f,
                     pos.y + 4.0f + (box - h) * 0.5f);
            dl->AddImage((ImTextureID)(intptr_t)tex, a, ImVec2(a.x + w, a.y + h));
        }
        if (row_on_screen) {
            char head[96];
            snprintf(head, sizeof(head), "%s  x%d", run.stem.c_str(),
                     (int)run.frames.size());
            float tx = pos.x + box + 12.0f;
            dl->AddText(ImVec2(tx, pos.y + 3.0f), col_text, head);
            dl->AddText(ImVec2(tx, pos.y + 17.0f), col_dim, run.doc_name.c_str());
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%s x%d from %s\nStarts at tick %d, %d tick%s per frame.",
                              run.stem.c_str(), (int)run.frames.size(),
                              run.doc_name.c_str(), start_tick,
                              hold, hold == 1 ? "" : "s");
        ImGui::PopID();

        if (pick) {
            std::string msg;
            WorldMarkedCreateBloodLane(state, lanes, lane, entry, run, &msg);
            if (!msg.empty()) {
                snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", msg.c_str());
                g_restore_msg_timer = 5.0f;
            }
            /* A Selectable inside a child window does not close the popup it
               sits in the way a MenuItem would, so say so: this closes the
               submenu and the frame's context menu with it. */
            ImGui::CloseCurrentPopup();
            break;
        }
    }
    if (shown == 0) {
        if (!runs.empty())
            ImGui::TextDisabled("Nothing matches \"%s\".", s_filter);
        else if (unopened > 0)
            ImGui::TextDisabled("Blood IMGs found -- open them below.");
        else {
            ImGui::TextDisabled("No blood sprites in the open tabs, and no");
            ImGui::TextDisabled("BLOOD IMG beside them on disk. Open BLOOD.IMG,");
            ImGui::TextDisabled("or any IMG whose sprites read as BLOOD/SPURT/");
            ImGui::TextDisabled("DRIP/SPRAY/SPLAT/GUTS/GORE.");
        }
    }
    ImGui::EndChild();

    if (unopened > 0) {
        char label[96];
        snprintf(label, sizeof(label), "Open %d blood IMG%s found on disk##blood_open",
                 unopened, unopened == 1 ? "" : "s");
        /* A button, not a MenuItem: a MenuItem closes the menu on click, and
           the point of the button is to watch the list fill and pick from it. */
        if (ImGui::Button(label)) WorldOpenBloodImgFiles();
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::TextDisabled("Opens them as tabs; the tab you are on stays active.");
            for (size_t i = 0; i < files.size(); i++)
                if (!files[i].open) ImGui::TextUnformatted(files[i].path.c_str());
            ImGui::EndTooltip();
        }
    }
}

WorldMarkedLaneThumbClick WorldDrawMarkedLaneThumbnails(WorldMarkedSequenceState &state,
                                                        WorldMarkedLane &lane,
                                                        const std::vector<WorldMarkedLane> &lanes)
{
    WorldMarkedLaneThumbClick action = {};
    bool draggable = !lane.dummy_decap;
    const ImGuiPayload *active_drag = ImGui::GetDragDropPayload();
    bool dragging_frame = active_drag && active_drag->IsDataType(kWorldLaneFramePayloadType);
    /* Snapshot the dragged frame's identity as plain ints up front rather than
       holding onto active_drag->Data: a drop delivered to an earlier item in
       this same row's loop below calls ImGui's ClearDragDrop(), which zeroes
       the payload's backing buffer mid-loop. Re-dereferencing active_drag
       after that for a later item in the row read a null Data pointer and
       crashed (0xC0000005). Plain ints aren't affected by that reset. */
    int dragging_slot = -1, dragging_frame_idx = -1;
    if (dragging_frame) {
        const WorldLaneFramePayload *dd = (const WorldLaneFramePayload *)active_drag->Data;
        dragging_slot = dd->slot;
        dragging_frame_idx = dd->frame_idx;
    }

    /* ImGui's built-in drag preview tooltip can end up hidden (it defers to
       the hovered target's own preview) or stacked behind the hover-info
       tooltip, so draw our own ghost of the dragged thumbnail at the cursor
       instead — always on top via the foreground draw list, guaranteed
       visible regardless of tooltip state. Only the row that owns the
       dragged frame draws it, so it happens exactly once per frame. */
    if (dragging_frame && lane.delay_slot == dragging_slot &&
        dragging_frame_idx >= 0 &&
        dragging_frame_idx < (int)state.sequence_frames[dragging_slot].size()) {
        int drag_img_idx = state.sequence_frames[dragging_slot][dragging_frame_idx];
        int drag_doc_idx = (dragging_frame_idx < (int)state.frame_doc[dragging_slot].size())
                         ? state.frame_doc[dragging_slot][dragging_frame_idx] : -1;
        Document *drag_doc = WorldMarkedResolveEntryDoc(WorldMarkedRowDoc(state, dragging_slot),
                                                        drag_doc_idx);
        IMG *drag_img = doc_get_img(drag_doc, drag_img_idx);
        SDL_Texture *drag_tex = BuildWorldSpriteTexture(drag_doc, drag_img, 230);
        if (drag_tex) {
            ImVec2 mouse = ImGui::GetMousePos();
            ImVec2 p0(mouse.x + 14.0f, mouse.y + 14.0f);
            ImVec2 p1(p0.x + 34.0f, p0.y + 34.0f);
            ImDrawList *fg = ImGui::GetForegroundDrawList();
            fg->AddRectFilled(ImVec2(p0.x - 2.0f, p0.y - 2.0f),
                              ImVec2(p1.x + 2.0f, p1.y + 2.0f),
                              IM_COL32(15, 15, 18, 210), 3.0f);
            fg->AddImage((ImTextureID)(intptr_t)drag_tex, p0, p1);
            fg->AddRect(p0, p1, IM_COL32(255, 205, 80, 255), 3.0f, 0, 1.5f);
        }
    }

    /* A borderless BeginChild gets ZERO window padding in ImGui, whatever
       WindowPadding is pushed around it -- so content sits flush against the
       child's clip rect. The ctrl+click highlight is drawn 2px OUTSIDE the
       thumbnail, which put it exactly on that edge and shaved the top and
       left off it. AlwaysUseWindowPadding opts back in, and the height grows
       to match so the bottom edge is not clipped instead.

       kWorldLaneStripPad is the space the highlight needs on every side; it
       and the 104px per-row reserve in ComputeWorldMarkedPanelLayout move
       together. */
    const float pad = kWorldLaneStripPad;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(pad, pad));
    ImGui::BeginChild("##world_lane_frames",
                      ImVec2(0.0f, kWorldLaneThumbPx +
                                   ImGui::GetStyle().FramePadding.y * 2.0f +
                                   pad * 2.0f),
                      false,
                      ImGuiWindowFlags_HorizontalScrollbar |
                      ImGuiWindowFlags_NoBackground |
                      ImGuiWindowFlags_AlwaysUseWindowPadding);
    for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
        /* 4px of gap plus 2px of highlight on each neighbour: enough that two
           picked frames side by side still read as two. */
        if (fi > 0) ImGui::SameLine(0.0f, 8.0f);
        ImGui::PushID(fi);
        ImGui::BeginGroup();
        int img_idx = lane.frames[fi];
        Document *thumb_doc = (fi < (int)lane.frame_docs.size() && lane.frame_docs[fi])
                            ? lane.frame_docs[fi] : lane.doc;
        IMG *thumb_img = doc_get_img(thumb_doc, img_idx);
        SDL_Texture *thumb_tex = BuildWorldSpriteTexture(thumb_doc, thumb_img, 255);
        bool current = (fi == lane.frame_pos);
        bool is_being_dragged = dragging_frame &&
            dragging_slot == lane.delay_slot && dragging_frame_idx == fi;
        if (current)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.46f, 0.72f, 1.0f));
        if (is_being_dragged)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * 0.35f);
        bool clicked = false;
        if (thumb_tex) {
            bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
            bool mirror_x = mirror_flag ? *mirror_flag : false;
            bool mirror_y = false;
            if (fi < (int)state.frame_mirror[lane.delay_slot].size()) {
                int mirror_bits = state.frame_mirror[lane.delay_slot][fi];
                mirror_x = mirror_x ^ ((mirror_bits & kWorldFrameMirrorX) != 0);
                mirror_y = (mirror_bits & kWorldFrameMirrorY) != 0;
            }
            clicked = ImGui::ImageButton("##world_thumb",
                                         (ImTextureID)(intptr_t)thumb_tex,
                                         ImVec2(kWorldLaneThumbPx, kWorldLaneThumbPx),
                                         ImVec2(mirror_x ? 1.0f : 0.0f,
                                                mirror_y ? 1.0f : 0.0f),
                                         ImVec2(mirror_x ? 0.0f : 1.0f,
                                                mirror_y ? 0.0f : 1.0f),
                                         ImVec4(0, 0, 0, 0),
                                         ImVec4(1, 1, 1, 1));
        } else {
            char fallback[16];
            snprintf(fallback, sizeof(fallback), "%d", img_idx);
            clicked = ImGui::Button(fallback, ImVec2(kWorldLaneThumbPx, kWorldLaneThumbPx));
        }
        ImVec2 item_min = ImGui::GetItemRectMin();
        ImVec2 item_max = ImGui::GetItemRectMax();
        if (current)
            ImGui::PopStyleColor();
        if (is_being_dragged)
            ImGui::PopStyleVar();
        bool picked = WorldFrameSelContains(lane.delay_slot, fi);
        if (clicked && ImGui::GetIO().KeyCtrl) {
            /* Ctrl+click gathers; it deliberately does NOT move the playhead
               or change the editor selection, because picking six frames out
               of a row would otherwise drag the whole scene around six
               times. */
            WorldFrameSelToggle(lane.delay_slot, fi);
            picked = !picked;
        } else if (clicked) {
            /* A plain click is the old behaviour, and it clears the pick --
               a selection you can no longer see the edges of is worse than
               no selection. */
            WorldFrameSelClear();
            state.paused = true;
            state.timer = 0.0f;
            state.frame = WorldMarkedTickForFrame(state, lane.delay_slot,
                                                  (int)lane.frames.size(), fi);
            lane.frame_pos = fi;
            action.clicked = true;
            action.doc_idx = lane.doc_idx;
            action.img_idx = img_idx;
        }
        if (picked) {
            /* Drawn over the thumbnail rather than as a style colour: the
               current-entry highlight already owns the button colour, and a
               frame can be both. */
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(item_min.x - 2.0f, item_min.y - 2.0f),
                ImVec2(item_max.x + 2.0f, item_max.y + 2.0f),
                IM_COL32(90, 220, 255, 255), 3.0f, 0, 2.0f);
        }
        /* Right-click a frame: the spray has to start somewhere, and "this
           frame, where it is" is the only anchor an authoring tool can offer
           that matches what create_blood_proc takes. */
        if (ImGui::BeginPopupContextItem("##world_thumb_ctx")) {
            ImGui::TextDisabled("Entry %d, tick %d", fi + 1,
                                WorldMarkedTickForFrame(state, lane.delay_slot,
                                                        (int)lane.frames.size(), fi));
            ImGui::Separator();
            if (ImGui::BeginMenu("Start Blood Here")) {
                WorldDrawBloodRunPicker(state, lanes, lane, fi);
                ImGui::EndMenu();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                ImGui::SetTooltip("Add a row playing a blood run once, from this frame's\n"
                                  "tick and on its anipoint -- the authoring twin of\n"
                                  "create_blood_proc's [y,x] offset from the victim.");
            ImGui::EndPopup();
        }
        if (!dragging_frame && ImGui::IsItemHovered()) {
            std::string sprite_name = (fi < (int)lane.frame_labels.size() &&
                                       !lane.frame_labels[fi].empty())
                                    ? lane.frame_labels[fi]
                                    : (thumb_img ? img_name_string(thumb_img) : std::string());
            int mirror_bits = fi < (int)state.frame_mirror[lane.delay_slot].size()
                            ? state.frame_mirror[lane.delay_slot][fi] : 0;
            ImGui::SetTooltip("[%d] %s%s%s%s", img_idx, sprite_name.c_str(),
                              (mirror_bits & kWorldFrameMirrorX) ? " flipX" : "",
                              (mirror_bits & kWorldFrameMirrorY) ? " flipY" : "",
                              draggable ? "\nDrag to reorder, or drop onto another row to move it there."
                                          "\nCtrl+click to pick several, then Copy on the strip above." : "");
        }
        if (draggable && ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID |
                                                    ImGuiDragDropFlags_SourceNoPreviewTooltip)) {
            WorldLaneFramePayload payload_data = { lane.delay_slot, fi };
            ImGui::SetDragDropPayload(kWorldLaneFramePayloadType, &payload_data,
                                      sizeof(payload_data));
            ImGui::EndDragDropSource();
        }
        if (draggable && ImGui::BeginDragDropTarget()) {
            bool insert_before = true;
            if (dragging_frame) {
                float mouse_x = ImGui::GetMousePos().x;
                insert_before = mouse_x < (item_min.x + item_max.x) * 0.5f;
                float line_x = insert_before ? item_min.x - 4.0f : item_max.x + 4.0f;
                ImGui::GetForegroundDrawList()->AddLine(
                    ImVec2(line_x, item_min.y - 3.0f), ImVec2(line_x, item_max.y + 3.0f),
                    IM_COL32(255, 205, 80, 255), 3.0f);
            }
            if (const ImGuiPayload *payload =
                    ImGui::AcceptDragDropPayload(kWorldLaneFramePayloadType,
                                                 ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                WorldLaneFramePayload src = *(const WorldLaneFramePayload *)payload->Data;
                int dst_idx = insert_before ? fi : fi + 1;
                if (WorldMarkedMoveEntryBetweenSlots(state, src.slot, src.frame_idx,
                                                     lane.delay_slot, dst_idx)) {
                    int refresh_fi = dst_idx;
                    WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, refresh_fi);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    if (draggable) {
        if (!lane.frames.empty()) ImGui::SameLine(0.0f, 6.0f);
        float drop_w = ImGui::GetContentRegionAvail().x;
        if (drop_w < 24.0f) drop_w = 24.0f;
        ImGui::InvisibleButton("##world_lane_drop_end", ImVec2(drop_w, 34.0f));
        ImVec2 end_min = ImGui::GetItemRectMin();
        ImVec2 end_max = ImGui::GetItemRectMax();
        if (ImGui::BeginDragDropTarget()) {
            if (dragging_frame) {
                ImGui::GetForegroundDrawList()->AddLine(
                    ImVec2(end_min.x + 2.0f, end_min.y - 3.0f),
                    ImVec2(end_min.x + 2.0f, end_max.y + 3.0f),
                    IM_COL32(255, 205, 80, 255), 3.0f);
            }
            if (const ImGuiPayload *payload =
                    ImGui::AcceptDragDropPayload(kWorldLaneFramePayloadType,
                                                 ImGuiDragDropFlags_AcceptNoDrawDefaultRect)) {
                WorldLaneFramePayload src = *(const WorldLaneFramePayload *)payload->Data;
                int append_at = (int)lane.frames.size();
                if (WorldMarkedMoveEntryBetweenSlots(state, src.slot, src.frame_idx,
                                                     lane.delay_slot, append_at)) {
                    int refresh_fi = (int)lane.frames.size();
                    WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, refresh_fi);
                }
            }
            ImGui::EndDragDropTarget();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();   /* the strip's WindowPadding */

    return action;
}

std::string WorldBuildMarkedAsm(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes)
{
    std::string out;
    out.reserve(4096);
    /* Not "fatality": nothing in here is fatality-specific and it never was.
       Walks, reactions, projectiles and props all come out of the same
       generator, and a header claiming otherwise sends people looking for a
       different tool. */
    out += "; IMGTOOL World View animation draft\n";
    out += "; One lane is one actor/object animation table.\n";
    /* The tables are emitted in MK2's OWN sign, not World View's, and the
       conversion is flip-dependent rather than a flat negation.

       World View authors an ANIPOINT offset, drawing at
       `anchor - anieff(ani + dA, size, flip)`. MK2's ani_adjustxy carries a
       POSITION offset -- multi_adjust_xy does `oxpos += dX` / `oypos += dY`,
       negating dX alone under b_fliph. Equate the two and:

         dX = -dAX                     always: the engine's own b_fliph
                                       negation cancels the preview's X mirror
         dY = flipv ? +dAY : -dAY      the engine never mirrors dY, so anieff's
                                       Y mirror is ours to resolve

       Emitting a constant negation inverted every V-flipped frame, which is
       most of a spin lane: a descent came out climbing off the top of the
       screen. Emitting the raw authoring sign inverts the unflipped ones
       instead. Neither is a fix; the flip bit is. */
    out += "; Export contract: dAX/dAY below are MK2 POSITION offsets, the same\n";
    out += ";   sign and space as ani_adjustxy -- +dAY moves the object DOWN the\n";
    out += ";   screen (multi_adjust_xy: oypos += dY), +dAX moves it forward in\n";
    out += ";   FACING space (oxpos += dX, negated by the engine when flipped).\n";
    out += ";   Use them as printed; do not negate them again when transcribing.\n";
    out += ";   World View holds the same placement as an ANIPOINT offset, drawing\n";
    out += ";   at anchor - anieff(ani + dA, size, flip). Converting is NOT a flat\n";
    out += ";   sign flip: X always inverts, Y only for frames that are not\n";
    out += ";   V-flipped, since anieff already mirrors Y on the ones that are and\n";
    out += ";   the engine never mirrors dY itself.\n";
    char conv_line[160];
    snprintf(conv_line, sizeof(conv_line),
             ";   anieff = %s, %s.\n",
             mirror_convention_label(g_mirror_convention),
             mirror_convention_source(g_mirror_convention));
    out += conv_line;
    out += ";   dAX is in FACING space, not screen space, the same way MK2's own\n";
    out += ";   blood and prop spawn offsets mirror with facing -- so one set of\n";
    out += ";   values serves both sides and needs no pre-mirroring. Applying dAX\n";
    out += ";   as a screen offset after mirroring lands every flipped entry\n";
    out += ";   2*dAX away.\n";
    out += "; The rows are ABSOLUTE offsets from the anchor; ani_adjustxy is\n";
    out += ";   cumulative. Each row that changes prints the ani_adjustxy operand\n";
    out += ";   it would take, so the lane can be transcribed without differencing\n";
    out += ";   it by hand. A held pose prints none: it needs no second adjust.\n";
    out += "; The runtime must substitute its own object for the shared anchor.\n";
    char world_meta[192];
    snprintf(world_meta, sizeof(world_meta),
             "; World View: W=%d H=%d Origin=(%d,%d) TickHz=%.4f\n",
             g_world_state.w, g_world_state.h,
             g_world_state.origin_x, g_world_state.origin_y, kMk2TickHz);
    out += world_meta;
    out += "; TIMING. One .long row is one ANIMATION STEP, not one tick.\n";
    out += ";   MKUTIL.ASM animate_a9 takes a9 = [sleep,ani_offset] and holds each\n";
    out += ";   row for `sleep` ticks -- \"a0 = sleep time between each frame\".\n";
    out += ";   So a lane exported as N repeated rows and then run at the customary\n";
    out += ";   sleep of 5 plays N*5 game ticks per authored frame, which is where a\n";
    out += ";   preview that looked right ends up several times too slow in game.\n";
    out += ";   Each lane below prints the sleep its rows were built for. Pass that\n";
    out += ";   value; do not substitute a house default.\n";
    out += "; Show@/Hide@ entries act as timed held subframes in preview, and\n";
    out += "; export emits 0 outside that tick window. A lane whose entries are\n";
    out += "; scheduled starts its rows at its FIRST scheduled tick and prints\n";
    out += "; the tick to spawn it at -- a table carries no spawn time of its own.\n";
    out += "; vX/vY motion is baked into the per-tick local anipoint rows;\n";
    out += "; StopX/StopY caps clamp that baked preview motion.\n";
    out += "; Each *_local_anipts table is aligned 1:1 with the .long rows.\n";
    out += "; Flip X/Y IS emitted as code: ani_flip / ani_flip_v toggles, only where\n";
    out += "; the orientation changes, so the table's image state matches the preview.\n";
    out += "; Z is NOT code and cannot be -- MK2 has no per-frame draw-priority opcode.\n";
    out += "; Entries with z= / dual annotations need routine code: z orders the\n";
    out += "; object's draw priority (set it on the object, once, from the lane's Z),\n";
    out += "; dual draws the same sprite a second time.\n";
    out += "; Lanes with dual entries also emit a *_dual_anipts table aligned\n";
    out += "; 1:1 with the rows; -32768,-32768 means no second copy that tick.\n";
    out += "; Entries built from Use Subframe are a single composite of several\n";
    out += "; sprites (e.g. head/body/legs) drawn together every tick. Each extra\n";
    out += "; piece beyond the primary gets its own *_pieceN_sprites table (.long\n";
    out += "; label-or-0, aligned 1:1 with the rows); all pieces share the\n";
    out += "; primary's *_local_anipts offset and rely on their own art anipoint\n";
    out += "; for relative placement, same as the World View preview.\n";
    out += "; Per-entry Flip X/Y controls emit ani_flip/ani_flip_v toggles.\n";
    out += "; Run each lane at the sleep IT prints above its rows -- lanes with\n";
    out += "; different holds do not share one. The preview runs at MK2's own\n";
    out += "; 54.7068 Hz and cannot be set to anything else, so a row here is a\n";
    out += "; game tick.\n\n";

    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        const WorldMarkedLane &lane = lanes[slot];
        if (lane.delay_slot >= 0 && lane.delay_slot < kWorldMarkedMaxTabs &&
            !state.lane_visible[lane.delay_slot])
            continue;
        const char *doc_name = lane.doc && lane.doc->fname_s[0]
                             ? lane.doc->fname_s : "Untitled";
        if (!lane.label.empty())
            doc_name = lane.label.c_str();
        std::string doc_part = !lane.asm_label_part.empty()
                             ? lane.asm_label_part
                             : WorldMarkedAsmLabelPart(doc_name, slot);
        char label_buf[96];
        if (lane.dummy_decap)
            snprintf(label_buf, sizeof(label_buf), "a_imgtool_%s", doc_part.c_str());
        else
            snprintf(label_buf, sizeof(label_buf), "a_imgtool_slot%d_%s",
                     slot + 1, doc_part.c_str());
        std::string anim_label = label_buf;

        char comment[192];
        snprintf(comment, sizeof(comment),
                 "; Slot %d  [%d] %s  %d frame%s%s\n",
                 slot + 1, lane.doc_idx, doc_name,
                 (int)lane.frames.size(), lane.frames.size() == 1 ? "" : "s",
                 state.hold_end[lane.delay_slot] ? "  stop-on-final" : "  looping");
        out += comment;
        int slot_stop_tick = (lane.delay_slot >= 0 &&
                              lane.delay_slot < kWorldMarkedMaxTabs)
                           ? ClampWorldMarkedVisibleFrom(
                                 state.stop_tick[lane.delay_slot])
                           : 0;
        int slot_pong_delay = (lane.delay_slot >= 0 &&
                               lane.delay_slot < kWorldMarkedMaxTabs)
                            ? ClampWorldMarkedVisibleFrom(
                                  state.pingpong_delay[lane.delay_slot])
                            : 0;
        if (slot_stop_tick > 0) {
            out += "; Preview Stop@ freezes this lane at tick ";
            out += std::to_string(slot_stop_tick);
            out += ".\n";
        }
        if (slot_pong_delay > 0) {
            out += "; Chain Ping Pong delay before reverse: ";
            out += std::to_string(slot_pong_delay);
            out += " ticks.\n";
        }
        if (lane.dummy_decap)
            out += "; Stock decap body timing: stand, fall-to-knees, wobble, fall-to-ground.\n";

        bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
        if (lane.dummy_decap)
            out += "; Anchor role: shared victim/body anchor.\n";
        else
            out += "; Anchor role: local offset from the shared victim/body anchor.\n";
        out += "; Local table below contains effective per-tick offsets after vX/vY motion.\n";
        if (mirror_flag && *mirror_flag)
            out += "; Mirror mode: DRAW_ONLY; spawn/draw this object mirrored in routine code.\n";
        else
            out += "; Mirror mode: OFF; spawn/draw this object without mirroring.\n";

        EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());

        /* Rows are animation steps, so the hold has to leave the table and
           become the runner's sleep. The gcd of the lane's holds is the
           largest sleep that still expresses every entry exactly: a uniform
           lane collapses to one row per frame (the idiomatic MK2 encoding),
           and a mixed one keeps whole-number repeats on top of a smaller
           sleep instead of one row per tick. */
        int lane_sleep = 0;
        for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
            int h = ClampTimelineHold(state.frame_delays[lane.delay_slot][fi]);
            int a = lane_sleep, b = h;
            while (b) { int t = a % b; a = b; b = t; }
            lane_sleep = a;
        }
        if (lane_sleep < 1) lane_sleep = 1;

        /* A row is lane_sleep ticks, full stop. The preview runs at the
           hardware rate and cannot be set to anything else, so there is no
           rate conversion left to do here -- this used to scale by
           kMk2TickHz/preview_hz and warn when the slider had been moved to
           something that was not a whole divisor of 54.7. */
        int game_sleep = lane_sleep;

        char sleep_line[288];
        snprintf(sleep_line, sizeof(sleep_line),
                 "; Run this lane with a9 = [%d,ani_offset]: %d game tick%s per row,\n"
                 ";   i.e. %.1f fps at MK2's %.4f Hz. Rows below are steps, not ticks.\n",
                 game_sleep, game_sleep, game_sleep == 1 ? "" : "s",
                 kMk2TickHz / (float)game_sleep, kMk2TickHz);
        out += sleep_line;
        {
            bool lane_has_motion = false;
            for (int fi = 0; fi < (int)lane.frames.size() && !lane_has_motion; fi++)
                lane_has_motion = state.motion_dx[lane.delay_slot][fi] ||
                                  state.motion_dy[lane.delay_slot][fi];
            if (lane_has_motion && lane_sleep > 1)
                out += "; NOTE: this lane uses per-tick motion, but a row only lands "
                       "every\n;   sleep ticks -- the offsets below are sampled at each "
                       "row's first\n;   tick, so the motion steps in jumps of "
                       "sleep*v rather than smoothly.\n";
        }

        out += anim_label;
        out += "\n";
        std::string local_table;
        local_table += anim_label;
        local_table += "_local_anipts\n";
        int emitted_mirror = 0;
        bool lane_has_dual = false;
        for (int fi = 0; fi < (int)lane.frames.size(); fi++)
            if (state.dual_on[lane.delay_slot][fi]) { lane_has_dual = true; break; }
        std::string dual_table;
        if (lane_has_dual) {
            dual_table += anim_label;
            dual_table += "_dual_anipts\n";
        }
        /* Code generation emits the pieces, never the parent.

           World View poses the parent -- one thing to drag, one anipoint to
           own the placement -- but the record the hardware draws is the chop,
           which is the entire reason the art was chopped: the pieces skip the
           empty space a parent's bitmap still has to store. Emitting the
           parent would throw that away. So the parent/piece split is resolved
           here, at the boundary between the editing model and the code. */
        std::vector<std::vector<int> > emit_pieces((size_t)lane.frames.size());
        std::vector<std::vector<Document*> > emit_piece_docs((size_t)lane.frames.size());
        std::vector<bool> emit_expanded((size_t)lane.frames.size(), false);
        for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
            Document *fdoc = (fi < (int)lane.frame_docs.size() && lane.frame_docs[fi])
                           ? lane.frame_docs[fi] : lane.doc;
            IMG *fimg = doc_get_img(fdoc, lane.frames[fi]);

            std::vector<int> kids;
            if (fimg) collect_subframe_indices(fdoc, fimg, &kids);
            if (!kids.empty()) {
                emit_pieces[(size_t)fi] = kids;
                emit_piece_docs[(size_t)fi].assign(kids.size(), fdoc);
                emit_expanded[(size_t)fi] = true;
                continue;
            }

            if (fi < (int)lane.frame_pieces.size() && !lane.frame_pieces[fi].empty()) {
                emit_pieces[(size_t)fi] = lane.frame_pieces[fi];
                if (fi < (int)lane.frame_piece_docs.size())
                    emit_piece_docs[(size_t)fi] = lane.frame_piece_docs[fi];
            } else {
                emit_pieces[(size_t)fi].push_back(lane.frames[fi]);
            }
            emit_piece_docs[(size_t)fi].resize(emit_pieces[(size_t)fi].size(), fdoc);
        }

        bool any_expanded = false;
        for (size_t i = 0; i < emit_expanded.size(); i++)
            if (emit_expanded[i]) { any_expanded = true; break; }
        if (any_expanded) {
            out += "; Frames with subframes are emitted as their pieces, not as the\n";
            out += "; parent frame World View poses. Piece order below is queue order.\n";
        }

        int extra_piece_cols = 0;
        for (size_t fi = 0; fi < emit_pieces.size(); fi++) {
            int extra = (int)emit_pieces[fi].size() - 1;
            if (extra > extra_piece_cols) extra_piece_cols = extra;
        }
        std::vector<std::string> piece_tables((size_t)extra_piece_cols);
        for (int c = 0; c < extra_piece_cols; c++) {
            piece_tables[(size_t)c] += anim_label;
            piece_tables[(size_t)c] += "_piece";
            piece_tables[(size_t)c] += std::to_string(c + 2);
            piece_tables[(size_t)c] += "_sprites\n";
        }
        /* Where this lane's clock starts.

           visible_from/visible_until are ABSOLUTE preview ticks -- a blood run
           spawned on the tick of a hit at 40 has visible_from[0] = 40. The
           export used to start its own counter at 0 and count the lane's rows,
           so every row of a scheduled lane failed `tick < visible_from` and
           emitted `.long 0`: the whole table came out hidden and terminated on
           row 0, which is not something you notice until you paste it.

           A table does not carry its own spawn time -- the routine that starts
           it does -- so the lane's clock begins at its first scheduled tick and
           the spawn tick is reported as a comment instead. Unscheduled lanes
           have these all at 0 and are unaffected. This also fixes Stop@, which
           is an absolute preview tick and was being compared against a
           lane-local count. */
        int lane_tick_base = 0;
        {
            int earliest = -1;
            for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
                int vf = state.visible_from[lane.delay_slot][fi];
                int vu = state.visible_until[lane.delay_slot][fi];
                if (vf <= 0 && vu <= 0) continue;   /* entry is not scheduled */
                if (earliest < 0 || vf < earliest) earliest = vf;
            }
            if (earliest > 0) lane_tick_base = earliest;
        }
        if (lane_tick_base > 0) {
            char spawn_line[224];
            snprintf(spawn_line, sizeof(spawn_line),
                     "; Scheduled lane: spawn it at preview tick %d. The rows below\n"
                     ";   start there, and the ; tick N comments are absolute preview\n"
                     ";   ticks so they line up with the World View transport.\n",
                     lane_tick_base);
            out += spawn_line;
        }
        int tick = lane_tick_base;
        bool reached_stop_tick = false;
        bool has_wide_local = false;
        int first_wide_tick = -1;
        /* Last offset written to the local table, for the ani_adjustxy delta
           annotation. Starts at 0,0: the object enters the lane sitting on
           the anchor with nothing accumulated. */
        int prev_out_dx = 0;
        int prev_out_dy = 0;
        /* Same running total, but for the ani_adjustxy rows emitted INLINE
           in the lane below. Separate from prev_out_* because the table is
           written per tick and the lane is written per row. */
        int prev_lane_dx = 0;
        int prev_lane_dy = 0;
        for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
            if (slot_stop_tick > 0 && tick > slot_stop_tick) {
                reached_stop_tick = true;
                break;
            }
            Document *frame_doc = (fi < (int)lane.frame_docs.size() && lane.frame_docs[fi])
                                ? lane.frame_docs[fi] : lane.doc;
            IMG *frame_img = doc_get_img(frame_doc, lane.frames[fi]);
            char fallback[32];
            snprintf(fallback, sizeof(fallback), "slot%d_frame%d", slot + 1, fi + 1);
            /* An expanded frame is named by its first piece; the parent's
               own label describes something the code never draws. */
            std::string raw_label;
            if (emit_expanded[(size_t)fi]) {
                IMG *first_piece = doc_get_img(emit_piece_docs[(size_t)fi][0],
                                               emit_pieces[(size_t)fi][0]);
                raw_label = img_name_string(first_piece);
            } else {
                raw_label = (fi < (int)lane.frame_labels.size() &&
                             !lane.frame_labels[fi].empty())
                          ? lane.frame_labels[fi]
                          : img_name_string(frame_img);
            }
            std::string sprite = WorldMarkedAsmToken(raw_label, fallback);
            int delay = ClampTimelineHold(state.frame_delays[lane.delay_slot][fi]);
            int local_dx = state.local_dx[lane.delay_slot][fi];
            int local_dy = state.local_dy[lane.delay_slot][fi];
            int visible_from = state.visible_from[lane.delay_slot][fi];
            int visible_until = state.visible_until[lane.delay_slot][fi];
            int motion_dx = state.motion_dx[lane.delay_slot][fi];
            int motion_dy = state.motion_dy[lane.delay_slot][fi];
            int motion_cap_x = state.motion_cap_x[lane.delay_slot][fi];
            int motion_cap_y = state.motion_cap_y[lane.delay_slot][fi];
            int frame_mirror = state.frame_mirror[lane.delay_slot][fi];
            int frame_z = state.frame_z[lane.delay_slot][fi];
            bool dual = state.dual_on[lane.delay_slot][fi] != 0;
            int dual_dx = state.dual_dx[lane.delay_slot][fi];
            int dual_dy = state.dual_dy[lane.delay_slot][fi];
            int dual_z = state.dual_z[lane.delay_slot][fi];
            std::vector<std::string> extra_sprites((size_t)extra_piece_cols, "0");
            {
                const std::vector<int> &pieces = emit_pieces[(size_t)fi];
                const std::vector<Document*> &piece_docs =
                    emit_piece_docs[(size_t)fi];
                for (int c = 0; c < extra_piece_cols; c++) {
                    size_t pi = (size_t)c + 1;
                    if (pi >= pieces.size()) continue;
                    Document *pdoc = (pi < piece_docs.size() && piece_docs[pi])
                                   ? piece_docs[pi] : lane.doc;
                    IMG *piece_img = doc_get_img(pdoc, pieces[pi]);
                    char piece_fallback[40];
                    snprintf(piece_fallback, sizeof(piece_fallback),
                             "slot%d_frame%d_piece%d", slot + 1, fi + 1, c + 2);
                    extra_sprites[(size_t)c] = WorldMarkedAsmToken(
                        img_name_string(piece_img), piece_fallback);
                }
            }
            /* delay is a multiple of lane_sleep by construction (gcd), so a
               uniform lane emits exactly one row per authored frame. */
            int rows = delay / lane_sleep;
            if (rows < 1) rows = 1;
            for (int repeat = 0; repeat < rows; repeat++) {
                if (slot_stop_tick > 0 && tick > slot_stop_tick) {
                    reached_stop_tick = true;
                    break;
                }
                bool hidden = tick < visible_from ||
                              (visible_until > 0 && tick >= visible_until);
                int eff_dx = 0;
                int eff_dy = 0;
                WorldMarkedEffectiveLocalDelta(state, lane.delay_slot,
                                               (int)lane.frames.size(), fi,
                                               false, tick, &eff_dx, &eff_dy);
                int eff_dual_dx = 0;
                int eff_dual_dy = 0;
                if (lane_has_dual)
                    WorldMarkedEffectiveLocalDelta(state, lane.delay_slot,
                                                   (int)lane.frames.size(), fi,
                                                   true, tick,
                                                   &eff_dual_dx, &eff_dual_dy);
                if (eff_dx < -128 || eff_dx > 127 || eff_dy < -128 || eff_dy > 127) {
                    has_wide_local = true;
                    if (first_wide_tick < 0) first_wide_tick = tick;
                }
                /* ani_adjustxy, INLINE and already in MK2's sign, so the
                   lane assembles as-is and nobody has to difference the
                   *_local_anipts table by hand. Same conversion as that
                   table: X always negates, Y only when the frame is not
                   V-flipped.

                   ORDER MATTERS. multi_adjust_xy negates dx against whatever
                   b_fliph is set at that moment, and dAX is in FACING space -
                   mirrored by the character's facing alone, never by a
                   per-entry flipX. So drop X back to base first, apply the
                   offset there, and let the flip ops below put the entry's
                   own mirror on. Emitting it while a per-entry flipX was
                   still set lands that entry 2*dAX away. */
                {
                    bool row_flipv = (frame_mirror & kWorldFrameMirrorY) != 0;
                    int row_dx = -eff_dx;
                    int row_dy = row_flipv ? eff_dy : -eff_dy;
                    if (row_dx != prev_lane_dx || row_dy != prev_lane_dy) {
                        WorldAppendFrameFlipOps(out, &emitted_mirror,
                                                emitted_mirror &
                                                    ~kWorldFrameMirrorX);
                        char adj[64];
                        snprintf(adj, sizeof(adj),
                                 "\t.long\tani_adjustxy\n\t.word\t%d,%d\n",
                                 row_dx - prev_lane_dx, row_dy - prev_lane_dy);
                        out += adj;
                        prev_lane_dx = row_dx;
                        prev_lane_dy = row_dy;
                    }
                }
                WorldAppendFrameFlipOps(out, &emitted_mirror, frame_mirror);
                out += "\t.long\t";
                out += hidden ? "0" : sprite;
                if (repeat == 0) {
                    out += "\t; ";
                    out += sprite;
                    out += " hold ";
                    out += std::to_string(delay);
                    out += "t = ";
                    out += std::to_string(rows);
                    out += " row";
                    out += rows == 1 ? "" : "s";
                    out += " x ";
                    out += std::to_string(lane_sleep);
                    out += "t";
                    if (local_dx || local_dy) {
                        out += " dAX=";
                        out += std::to_string(local_dx);
                        out += " dAY=";
                        out += std::to_string(local_dy);
                    }
                    if (visible_from > 0) {
                        out += " show>=";
                        out += std::to_string(visible_from);
                    }
                    if (visible_until > 0) {
                        out += " hide>=";
                        out += std::to_string(visible_until);
                    }
                    if (motion_dx || motion_dy) {
                        out += " vX=";
                        out += std::to_string(motion_dx);
                        out += " vY=";
                        out += std::to_string(motion_dy);
                    }
                    if (motion_cap_x > 0) {
                        out += " stopX=";
                        out += std::to_string(motion_cap_x);
                    }
                    if (motion_cap_y > 0) {
                        out += " stopY=";
                        out += std::to_string(motion_cap_y);
                    }
                    if (frame_mirror & kWorldFrameMirrorX)
                        out += " flipX";
                    if (frame_mirror & kWorldFrameMirrorY)
                        out += " flipY";
                    if (frame_z) {
                        out += " z=";
                        out += std::to_string(frame_z);
                    }
                    if (dual) {
                        out += " dual dAX2=";
                        out += std::to_string(dual_dx);
                        out += " dAY2=";
                        out += std::to_string(dual_dy);
                        if (dual_z) {
                            out += " z2=";
                            out += std::to_string(dual_z);
                        }
                    }
                } else if (hidden) {
                    out += "\t; hidden";
                }
                out += "\n";

                /* Into MK2's position sign. NOT a constant negation -- that
                   is the trap this export fell into, and it inverts a whole
                   lane's descent.

                   The preview draws at anchor - anieff(ani + dA, size, flip),
                   so the drawn offset from the anchor is -dA unflipped and
                   +dA flipped: anieff mirrors about size, and the size term
                   cancels in the difference, which is why this holds for both
                   mirror conventions.

                   X: multi_adjust_xy negates dX itself under b_fliph, and
                      that exactly cancels the preview's own X mirror. So X is
                      an unconditional negation, flipped or not.
                   Y: the engine never touches dY -- multi_adjust_xy mirrors
                      a0 only -- so the V-flip stays ours to resolve here. A
                      V-flipped frame already agrees with MK2 and passes
                      through; an unflipped one negates. */
                bool frame_flipv = (frame_mirror & kWorldFrameMirrorY) != 0;
                int out_dx = -eff_dx;
                int out_dy = frame_flipv ? eff_dy : -eff_dy;
                local_table += "\t.word\t";
                local_table += std::to_string(out_dx);
                local_table += ",";
                local_table += std::to_string(out_dy);
                local_table += "\t; tick ";
                local_table += std::to_string(tick);
                local_table += hidden ? " hidden " : " ";
                local_table += sprite;
                /* These rows are ABSOLUTE offsets from the anchor, but
                   ani_adjustxy is cumulative -- it adds to oxpos/oypos and
                   leaves them there. So print the difference each row would
                   need as an ani_adjustxy operand. MKSA.ASM's heli lane was
                   converted by hand ("ani_adjustxy wants deltas, so they are
                   differenced"), which is a subtraction nobody should be
                   doing on paper. A repeat of the same offset prints nothing,
                   because a held pose needs no second adjust. */
                if (out_dx != prev_out_dx || out_dy != prev_out_dy) {
                    local_table += "  ani_adjustxy ";
                    local_table += std::to_string(out_dx - prev_out_dx);
                    local_table += ",";
                    local_table += std::to_string(out_dy - prev_out_dy);
                }
                local_table += "\n";
                prev_out_dx = out_dx;
                prev_out_dy = out_dy;

                if (lane_has_dual) {
                    dual_table += "\t.word\t";
                    if (dual && !hidden) {
                        dual_table += std::to_string(-eff_dual_dx);
                        dual_table += ",";
                        dual_table += std::to_string(frame_flipv ? eff_dual_dy
                                                                : -eff_dual_dy);
                        dual_table += "\t; tick ";
                        dual_table += std::to_string(tick);
                        dual_table += " second ";
                        dual_table += sprite;
                    } else {
                        dual_table += "-32768,-32768\t; tick ";
                        dual_table += std::to_string(tick);
                        dual_table += " no second copy";
                    }
                    dual_table += "\n";
                }

                for (int c = 0; c < extra_piece_cols; c++) {
                    piece_tables[(size_t)c] += "\t.long\t";
                    piece_tables[(size_t)c] += hidden ? "0" : extra_sprites[(size_t)c];
                    piece_tables[(size_t)c] += "\t; tick ";
                    piece_tables[(size_t)c] += std::to_string(tick);
                    piece_tables[(size_t)c] += "\n";
                }
                tick += lane_sleep;
            }
            if (reached_stop_tick)
                break;
        }
        if (slot_stop_tick > 0 && tick > slot_stop_tick)
            reached_stop_tick = true;
        if (has_wide_local) {
            out += "; WARNING: local anipoint exceeds signed 8-bit range at/after tick ";
            out += std::to_string(first_wide_tick);
            out += "; preserve .word data and verify target-runtime encoding.\n";
        }
        if (reached_stop_tick) {
            out += "\t.long\t0\t; stop at preview tick ";
            out += std::to_string(slot_stop_tick);
            out += "\n\n";
        } else if (state.hold_end[lane.delay_slot]) {
            out += "\t.long\t0\t; stop on final frame\n\n";
        } else {
            out += "\t.long\tani_jump,";
            out += anim_label;
            out += "\t; loop\n\n";
        }
        out += local_table;
        out += "\n";
        if (lane_has_dual) {
            out += dual_table;
            out += "\n";
        }
        for (int c = 0; c < extra_piece_cols; c++) {
            out += piece_tables[(size_t)c];
            out += "\n";
        }
    }
    return out;
}

bool WorldDrawMarkedAsmPopup(WorldMarkedSequenceState &state)
{
    bool copied = false;
    if (state.show_asm)
        ImGui::OpenPopup("World View ASM");
    if (ImGui::BeginPopupModal("World View ASM", &state.show_asm,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextDisabled("Generated from the current marked-row World View sequence.");
        ImGui::BeginChild("##world_marked_asm_text", ImVec2(720.0f, 420.0f), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::TextUnformatted(state.generated_asm.c_str());
        ImGui::EndChild();
        if (ImGui::Button("Copy to Clipboard")) {
            ImGui::SetClipboardText(state.generated_asm.c_str());
            copied = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Close")) {
            state.show_asm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
    return copied;
}

void WorldHandleMarkedLaneDrag(ImDrawList *dl, WorldMarkedSequenceState &state,
                               const std::vector<WorldMarkedLane> &lanes,
                               const WorldMarkedLaneRenderInfo &render_info,
                               const WorldCanvasLayout &world_layout,
                               const WorldMarkedPanelLayout &panel_layout,
                               ImVec2 canvas_min, ImVec2 canvas_max)
{
    ImVec2 mouse = ImGui::GetMousePos();
    bool over_panel =
        mouse.x >= panel_layout.pos.x &&
        mouse.x <= panel_layout.pos.x + panel_layout.width &&
        mouse.y >= panel_layout.pos.y &&
        mouse.y <= panel_layout.pos.y + panel_layout.height;
    /* The grab region is the canvas, not the playfield rect. Sprites are drawn
       unclipped, so one anchored past the world edge is visible out there and
       has to be grabbable — it is usually the frame whose anipoint most needs
       fixing. The floating lane panel still wins wherever it overlaps. */
    bool over_canvas =
        mouse.x >= canvas_min.x && mouse.x <= canvas_max.x &&
        mouse.y >= canvas_min.y && mouse.y <= canvas_max.y;

    int hover_slot = -1;
    bool hover_dual = false;
    if (over_canvas && !over_panel) {
        /* Dual copies paint after their primary, so test them first; both
           rects exist whenever the entry draws its sprite twice. */
        for (int slot = 0; slot < (int)lanes.size() && hover_slot < 0; slot++) {
            if (!render_info.dual_rect_valid[slot]) continue;
            if (mouse.x >= render_info.dual_rect_min[slot].x &&
                mouse.x <= render_info.dual_rect_max[slot].x &&
                mouse.y >= render_info.dual_rect_min[slot].y &&
                mouse.y <= render_info.dual_rect_max[slot].y) {
                hover_slot = slot;
                hover_dual = true;
            }
        }
        for (int slot = 0; slot < (int)lanes.size() && hover_slot < 0; slot++) {
            if (!render_info.lane_rect_valid[slot]) continue;
            if (mouse.x >= render_info.lane_rect_min[slot].x &&
                mouse.x <= render_info.lane_rect_max[slot].x &&
                mouse.y >= render_info.lane_rect_min[slot].y &&
                mouse.y <= render_info.lane_rect_max[slot].y) {
                hover_slot = slot;
            }
        }
    }

    if (hover_slot >= 0) {
        if (dl) {
            dl->AddRect(hover_dual ? render_info.dual_rect_min[hover_slot]
                                   : render_info.lane_rect_min[hover_slot],
                        hover_dual ? render_info.dual_rect_max[hover_slot]
                                   : render_info.lane_rect_max[hover_slot],
                        IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    /* Link mode is deliberately separate from ordinary lane dragging. It
       operates on the actual individual pieces of a composite frame, rather
       than the lane's union rectangle. */
    struct AnchorHit {
        IMG *img = NULL;
        Document *doc = NULL;   /* the document that actually owns img */
        int idx = -1;           /* its index within that document */
        bool mirror = false;
    };
    auto piece_at = [&](ImVec2 p) -> AnchorHit {
        AnchorHit hit;
        if (!over_canvas || over_panel) return hit;
        for (int li = (int)lanes.size() - 1; li >= 0; li--) {
            const WorldMarkedLane &lane = lanes[li];
            int fi = lane.frame_pos;
            if (fi < 0 || fi >= (int)lane.frames.size()) continue;
            int ss = lane.delay_slot;
            if (!state.lane_visible[ss]) continue;
            /* render_info is keyed by row position, like every other
               reader of it (hover_slot indexes lanes[] too) -- not by
               delay_slot. */
            bool mirror = render_info.lane_mirror_x[li] ||
                          render_info.lane_mirror_y[li];
            const std::vector<int> *pieces =
                fi < (int)lane.frame_pieces.size() ? &lane.frame_pieces[fi] : NULL;
            std::vector<int> fallback;
            if (!pieces || pieces->empty()) { fallback.push_back(lane.frames[fi]); pieces = &fallback; }
            const std::vector<Document*> *piece_docs =
                fi < (int)lane.frame_piece_docs.size() ? &lane.frame_piece_docs[fi]
                                                       : NULL;
            Document *fdoc = (fi < (int)lane.frame_docs.size() && lane.frame_docs[fi])
                           ? lane.frame_docs[fi] : lane.doc;
            int dx = 0, dy = 0;
            WorldMarkedEffectiveLocalDelta(state, ss, (int)lane.frames.size(), fi,
                                           false, lane.tick, &dx, &dy);
            for (int pi = (int)pieces->size() - 1; pi >= 0; pi--) {
                /* frame_docs[] records only the *first* piece's document, so a
                   composite whose pieces span files resolved every later piece
                   against the wrong image list -- and link mode then grabbed
                   whatever sprite happened to sit at that index. Resolve per
                   piece, exactly as the draw path does. */
                Document *pdoc = (piece_docs && (size_t)pi < piece_docs->size() &&
                                  (*piece_docs)[(size_t)pi])
                               ? (*piece_docs)[(size_t)pi] : fdoc;
                IMG *img = doc_get_img(pdoc, (*pieces)[(size_t)pi]);
                if (!img) continue;
                float left = world_layout.origin_x - ((int)(short)img->anix + dx) * world_layout.scale;
                float top = world_layout.origin_y - ((int)(short)img->aniy + dy) * world_layout.scale;
                float right = left + (float)img->w * world_layout.scale;
                float bottom = top + (float)img->h * world_layout.scale;
                if (p.x >= left && p.x <= right && p.y >= top && p.y <= bottom) {
                    hit.img = img;
                    hit.doc = pdoc;
                    hit.idx = (*pieces)[(size_t)pi];
                    hit.mirror = mirror;
                    return hit;
                }
            }
        }
        return hit;
    };

    if (state.anchor_link_mode) {
        if (!state.anchor_link_active && over_canvas && !over_panel &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            AnchorHit source = piece_at(mouse);
            if (source.img && !source.mirror) {
                state.anchor_link_active = true;
                state.anchor_link_source_img = source.img;
                state.anchor_link_source = mouse;
                state.paused = true;
            }
        }
        if (state.anchor_link_active && dl) {
            dl->AddLine(state.anchor_link_source, mouse, IM_COL32(90, 235, 255, 255), 2.0f);
            dl->AddCircleFilled(state.anchor_link_source, 4.0f, IM_COL32(90, 235, 255, 255));
        }
        if (state.anchor_link_active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            AnchorHit target = piece_at(mouse);
            if (target.img && target.img != state.anchor_link_source_img && !target.mirror) {
                int dx = (int)lroundf((mouse.x - state.anchor_link_source.x) / world_layout.scale);
                int dy = (int)lroundf((mouse.y - state.anchor_link_source.y) / world_layout.scale);
                Document *tdoc = target.doc ? target.doc : g_doc;
                /* Every other anipoint edit in the tool is undoable; this one
                   wrote straight through. The snapshot only ever covers the
                   active document, so take it when that is what changes. */
                if (tdoc == g_doc) doc_undo_push();
                target.img->anix = (unsigned short)ClampWorldMarkedAniptDelta((int)(short)target.img->anix + dx);
                target.img->aniy = (unsigned short)ClampWorldMarkedAniptDelta((int)(short)target.img->aniy + dy);
                /* A lane can be fed from any open file, but mark_dirty() only
                   ever flags g_doc. Linking a piece that lived in another tab
                   left that document clean: no asterisk, no save, and no close
                   prompt -- the edit was simply lost. Flag the document that
                   actually changed. */
                if (tdoc == g_doc) {
                    mark_dirty();
                    InvalidateThumb(target.idx);
                    g_img_tex_idx = -2;
                } else {
                    tdoc->dirty = true;
                }
                /* Auto-Chop/Body-Split children hold their anchor as
                   parent - piece_offset, so moving a parent alone splits the
                   composite. Carry its subframes by the same delta. */
                int kids = shift_subframe_anipoints(tdoc, target.img, dx, dy);
                /* Anipoints are subtracted from the anchor, so the sprite
                   travels the opposite way from the delta stored in the file.
                   Report both rather than letting one stand for the other. */
                if (kids > 0)
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Linked anchors: target moved %d, %d px on screen "
                             "(anipoint %+d, %+d), with %d subframe%s.",
                             -dx, -dy, dx, dy, kids, kids == 1 ? "" : "s");
                else
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Linked anchors: target moved %d, %d px on screen "
                             "(anipoint %+d, %+d).", -dx, -dy, dx, dy);
                g_restore_msg_timer = 4.0f;
            }
            state.anchor_link_active = false;
            state.anchor_link_source_img = NULL;
        }
        return;
    }

    if (hover_slot >= 0 && ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const WorldMarkedLane &lane = lanes[hover_slot];
        int state_slot = lane.delay_slot;
        EnsureWorldMarkedFrameDelays(state, state_slot, (int)lane.frames.size());
        if (lane.frame_pos >= 0 && lane.frame_pos < (int)lane.frames.size()) {
            state.paused = true;
            state.active_slot = state_slot;
            state.drag_slot = state_slot;
            state.drag_frame = lane.frame_pos;
            state.drag_dual = hover_dual;
            state.drag_mouse = mouse;
            state.drag_dx = hover_dual ? state.dual_dx[state_slot][lane.frame_pos]
                                       : state.local_dx[state_slot][lane.frame_pos];
            state.drag_dy = hover_dual ? state.dual_dy[state_slot][lane.frame_pos]
                                       : state.local_dy[state_slot][lane.frame_pos];
            state.drag_mirror_x = render_info.lane_mirror_x[hover_slot];
            state.drag_mirror_y = render_info.lane_mirror_y[hover_slot];
            state.drag_all_dx = hover_dual ? state.dual_dx[state_slot]
                                           : state.local_dx[state_slot];
            state.drag_all_dy = hover_dual ? state.dual_dy[state_slot]
                                           : state.local_dy[state_slot];
        }
    }

    if (state.drag_slot >= 0) {
        int state_slot = state.drag_slot;
        int frame_idx = state.drag_frame;
        std::vector<int> *dst_dx = state.drag_dual ? state.dual_dx : state.local_dx;
        std::vector<int> *dst_dy = state.drag_dual ? state.dual_dy : state.local_dy;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
            state_slot < 0 || state_slot >= kWorldMarkedMaxTabs ||
            frame_idx < 0 ||
            frame_idx >= (int)dst_dx[state_slot].size()) {
            state.drag_slot = -1;
            state.drag_frame = -1;
            state.drag_dual = false;
            state.drag_mirror_x = false;
            state.drag_mirror_y = false;
            state.drag_all_dx.clear();
            state.drag_all_dy.clear();
        } else {
            int px = (int)((mouse.x - state.drag_mouse.x) / world_layout.scale);
            int py = (int)((mouse.y - state.drag_mouse.y) / world_layout.scale);
            int next_dx =
                ClampWorldMarkedAniptDelta(state.drag_dx +
                                           (state.drag_mirror_x ? px : -px));
            int next_dy =
                ClampWorldMarkedAniptDelta(state.drag_dy +
                                           (state.drag_mirror_y ? py : -py));
            /* Move the whole run, not just the frame under the cursor.
               This used to also require frame_idx == 0, which made it a trap:
               park the row on any other frame, Ctrl-drag, and only that one
               frame moved -- silently, because the rest are not on screen at
               that tick to show they stayed behind. Every frame is a valid
               handle on the run now. */
            bool move_together = !state.drag_dual &&
                                 (state.lane_rigid[state_slot] ||
                                  ImGui::GetIO().KeyCtrl);
            if (move_together) {
                int n = (int)dst_dx[state_slot].size();
                for (int fi = 0; fi < n && fi < (int)dst_dy[state_slot].size(); fi++) {
                    int base_dx = fi < (int)state.drag_all_dx.size()
                                ? state.drag_all_dx[fi]
                                : dst_dx[state_slot][fi];
                    int base_dy = fi < (int)state.drag_all_dy.size()
                                ? state.drag_all_dy[fi]
                                : dst_dy[state_slot][fi];
                    dst_dx[state_slot][fi] =
                        ClampWorldMarkedAniptDelta(base_dx + next_dx - state.drag_dx);
                    dst_dy[state_slot][fi] =
                        ClampWorldMarkedAniptDelta(base_dy + next_dy - state.drag_dy);
                }
            } else {
                dst_dx[state_slot][frame_idx] = next_dx;
                dst_dy[state_slot][frame_idx] = next_dy;
            }
        }
    }
}

std::string WorldMarkedAsmToken(const std::string &raw, const char *fallback)
{
    std::string out;
    out.reserve(raw.size() + 8);
    for (char c : raw) {
        unsigned char uc = (unsigned char)c;
        if (std::isalnum(uc) || c == '_' || c == '+' || c == '-' || c == '*' ||
            c == '(' || c == ')')
            out.push_back(c);
    }
    if (out.empty() && fallback) out = fallback;
    if (!out.empty() && std::isdigit((unsigned char)out[0]))
        out.insert(out.begin(), '_');
    return out;
}

std::string WorldMarkedAsmLabelPart(const char *raw, int slot)
{
    std::string out;
    if (raw) {
        for (size_t i = 0; raw[i] && i < 64; i++) {
            char c = raw[i];
            if (c == '.') break;
            unsigned char uc = (unsigned char)c;
            if (std::isalnum(uc))
                out.push_back((char)std::tolower(uc));
            else if (c == '_')
                out.push_back('_');
        }
    }
    if (out.empty()) {
        char fallback[24];
        snprintf(fallback, sizeof(fallback), "tab%d", slot + 1);
        out = fallback;
    }
    if (std::isdigit((unsigned char)out[0]))
        out.insert(out.begin(), '_');
    return out;
}

int ClampWorldMarkedAniptDelta(int value)
{
    if (value < -32768) return -32768;
    if (value >  32767) return  32767;
    return value;
}

int ClampWorldMarkedVisibleFrom(int value)
{
    if (value < 0) return 0;
    if (value > 99999) return 99999;
    return value;
}

int ClampWorldMarkedVisibleUntil(int value)
{
    if (value < 0) return 0;
    if (value > 99999) return 99999;
    return value;
}

int ClampWorldMarkedZ(int value)
{
    if (value < -99) return -99;
    if (value >  99) return  99;
    return value;
}

int ClampWorldMarkedMotion(int value)
{
    if (value < -128) return -128;
    if (value >  128) return  128;
    return value;
}

int ClampWorldMarkedMotionCap(int value)
{
    if (value < 0) return 0;
    if (value > 99999) return 99999;
    return value;
}

static int ClampWorldMarkedFrameMirror(int value)
{
    return value & (kWorldFrameMirrorX | kWorldFrameMirrorY);
}

/* Every per-entry array a slot's sequence carries, with the value a fresh
   entry gets. Sequence edits (duplicate/move/delete/reconcile) must touch all
   of them together or the entries drift out of alignment. */
struct WorldSeqArrayRef {
    std::vector<int> *vec;
    int fresh;
};

static std::vector<WorldSeqArrayRef> WorldMarkedSeqArrays(
    WorldMarkedSequenceState &state, int slot)
{
    return {
        { &state.frame_delays[slot], 1 },
        { &state.local_dx[slot],     0 },
        { &state.local_dy[slot],     0 },
        { &state.visible_from[slot], 0 },
        { &state.visible_until[slot], 0 },
        { &state.motion_dx[slot],    0 },
        { &state.motion_dy[slot],    0 },
        { &state.motion_cap_x[slot], 0 },
        { &state.motion_cap_y[slot], 0 },
        { &state.frame_mirror[slot], 0 },
        { &state.frame_z[slot],      0 },
        { &state.dual_on[slot],      0 },
        { &state.dual_dx[slot],      0 },
        { &state.dual_dy[slot],      0 },
        { &state.dual_z[slot],       0 },
    };
}

void WorldMarkedRestart(WorldMarkedSequenceState &state)
{
    state.timer = 0.0f;
    state.frame = 0;
}

void StepWorldMarkedSequence(WorldMarkedSequenceState &state, int delta)
{
    state.paused = true;
    state.timer = 0.0f;

    int slot = state.active_slot;
    auto usable_slot = [&](int candidate) {
        return candidate >= 0 && candidate < kWorldMarkedMaxTabs &&
               !state.sequence_frames[candidate].empty();
    };

    /* If no row has been explicitly chosen yet, prefer the row containing the
       editor's selected real IMG, then any row belonging to the active tab. */
    if (!usable_slot(slot)) {
        slot = -1;
        int active_doc = document_active_index();
        int selected_img = g_doc ? g_doc->ilselected : -1;
        for (int s = 0; s < kWorldMarkedMaxTabs && slot < 0; s++) {
            if (!usable_slot(s)) continue;
            for (size_t fi = 0; fi < state.sequence_frames[s].size(); fi++) {
                int doc_idx = (fi < state.frame_doc[s].size() && state.frame_doc[s][fi] >= 0)
                            ? state.frame_doc[s][fi] : WorldMarkedRowDocIndex(state, s);
                if (doc_idx == active_doc && state.sequence_frames[s][fi] == selected_img) {
                    slot = s;
                    break;
                }
            }
        }
        if (!usable_slot(slot)) {
            for (int s = 0; s < kWorldMarkedMaxTabs; s++) {
                if (usable_slot(s) && WorldMarkedRowDocIndex(state, s) == active_doc) {
                    slot = s;
                    break;
                }
            }
        }
    }

    if (!usable_slot(slot)) {
        state.frame += delta;
        if (state.frame < 0) state.frame = 0;
        return;
    }

    state.active_slot = slot;
    int n = (int)state.sequence_frames[slot].size();
    int current = WorldMarkedFrameForTick(state, slot, n, state.frame,
                                           state.hold_end[slot]);
    int next = (current + delta) % n;
    if (next < 0) next += n;
    state.frame = WorldMarkedTickForFrame(state, slot, n, next);

    int doc_idx = (next < (int)state.frame_doc[slot].size() &&
                   state.frame_doc[slot][next] >= 0)
                ? state.frame_doc[slot][next] : WorldMarkedRowDocIndex(state, slot);
    Document *doc = document_get(doc_idx);
    WorldSyncEditorSelectionToSprite(doc, doc_idx,
                                     state.sequence_frames[slot][next]);
}

void WorldMarkedSetTick(WorldMarkedSequenceState &state, int tick)
{
    state.paused = true;
    state.timer = 0.0f;
    state.frame = ClampWorldMarkedVisibleFrom(tick);
}

int WorldMarkedSlotHold(const WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return ClampTimelineHold(state.default_hold);
    /* slot_hold is the row's REAL hold, pinned or not -- it is what the T/f
       box shows and what a grown row extends at. It used to report the global
       for any row that was not pinned, which meant a blood row sitting at 1
       tick displayed the scene's 4 and read as if it had already inherited
       when it had not. Zero is the never-set case (the array is value
       initialised), and only then does the global stand in. */
    int hold = state.slot_hold[slot];
    if (hold <= 0) hold = state.default_hold;
    return ClampTimelineHold(hold);
}

/* Re-lay a scheduled row's Show@/Hide@ windows for a new hold.

   A blood spray is scheduled, not looped: WorldMarkedCreateBloodLane writes
   visible_from[i] = base + i*hold and visible_until[i] = that + hold, so the
   run fires once at the tick of the hit. Retiming such a row by rewriting
   frame_delays alone changed how long the runner held each frame while the
   windows still said "show frame 3 for tick 13 only" -- the row kept its old
   cadence on screen and looked like the speed control had missed it. That is
   the bug this exists to close.

   Only a run that is exactly uniform at old_hold is touched. Windows authored
   by Build Chain or typed into Show@/Hide@ by hand encode gaps and launch
   delays that are not derived from the hold, and rescaling those would be
   inventing timing the user did not ask for. */
static void WorldMarkedRetimeSchedule(WorldMarkedSequenceState &state, int slot,
                                      int old_hold, int new_hold)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    if (old_hold == new_hold || old_hold < 1 || new_hold < 1) return;

    std::vector<int> &from = state.visible_from[slot];
    std::vector<int> &until = state.visible_until[slot];
    int n = (int)from.size();
    if (n < 1 || (int)until.size() < n) return;

    /* An unscheduled row leaves these at 0 and is driven by the tick clock
       alone; there is nothing to re-lay. */
    int base = from[0];
    if (base <= 0 && until[0] <= 0) return;

    for (int i = 0; i < n; i++) {
        if (from[i] != base + i * old_hold) return;
        if (until[i] != base + (i + 1) * old_hold) return;
    }
    for (int i = 0; i < n; i++) {
        from[i] = ClampWorldMarkedVisibleFrom(base + i * new_hold);
        until[i] = ClampWorldMarkedVisibleUntil(base + (i + 1) * new_hold);
    }
}

void WorldMarkedSetSlotHold(WorldMarkedSequenceState &state, int slot,
                            int ticks, bool custom)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    /* The dummy body runs canned stock timing and is not anyone's to retime
       from here -- the same reason the global skips it. */
    if (slot == kWorldDummyDecapSlot) return;
    ticks = ClampTimelineHold(ticks);
    int old_hold = WorldMarkedSlotHold(state, slot);
    state.slot_hold[slot] = ticks;
    state.slot_hold_custom[slot] = custom;
    for (int &delay : state.frame_delays[slot])
        delay = ticks;
    WorldMarkedRetimeSchedule(state, slot, old_hold, ticks);
    state.frame = 0;
    state.timer = 0.0f;
}

void WorldMarkedApplyUniformHold(WorldMarkedSequenceState &state, int ticks,
                                 bool include_custom)
{
    ticks = ClampTimelineHold(ticks);
    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++) {
        if (slot == kWorldDummyDecapSlot) continue;
        /* A hidden row is parked, not part of the scene being timed. It shows
           no T/f of its own while hidden -- the row collapses to one line
           before those controls are reached -- so retiming it here would edit
           a row whose timing the user cannot see, and they would find it
           changed whenever they brought it back. Unhide it to retime it.
           This holds for `include_custom` too: "All" overrides PINNING, and
           hidden is not pinned. */
        if (!state.lane_visible[slot]) continue;
        /* A row that was pinned with its own T/f keeps it. Pinning is opt-in
           and per row, so the global still reaches everything else -- which
           is the whole point of it being called the global. */
        if (state.slot_hold_custom[slot] && !include_custom) continue;
        if (include_custom) state.slot_hold_custom[slot] = false;
        int old_hold = WorldMarkedSlotHold(state, slot);
        state.slot_hold[slot] = ticks;
        for (int &delay : state.frame_delays[slot])
            delay = ticks;
        /* Scheduled rows -- blood sprays above all -- have to have their
           Show@/Hide@ run re-laid at the new hold or they keep the old
           cadence and appear to ignore the global entirely. */
        WorldMarkedRetimeSchedule(state, slot, old_hold, ticks);
    }
    /* Rewind the tick clock rather than the sequences: the frame you were
       looking at keeps its place in the list, it just holds longer now. */
    state.frame = 0;
    state.timer = 0.0f;
}


void EnsureWorldMarkedFrameDelays(WorldMarkedSequenceState &state, int slot, int frame_count)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    if (frame_count < 0) frame_count = 0;

    std::vector<int> &delays = state.frame_delays[slot];
    /* Frames arriving with no authored timing take the row's hold, not one
       tick. A 1-tick hold is 54.7 fps, which is not a speed any MK2 animation
       plays at, so it made a freshly marked set unwatchable. The row's hold,
       not the global one: a row that owns its timing has to keep it when it
       grows, or extending a 1-tick blood row appends frames at 4. */
    if ((int)delays.size() < frame_count)
        delays.resize((size_t)frame_count, WorldMarkedSlotHold(state, slot));
    else if ((int)delays.size() > frame_count)
        delays.resize((size_t)frame_count);
    for (int &delay : delays)
        delay = ClampTimelineHold(delay);

    std::vector<int> &local_dx = state.local_dx[slot];
    std::vector<int> &local_dy = state.local_dy[slot];
    std::vector<int> &visible_from = state.visible_from[slot];
    std::vector<int> &visible_until = state.visible_until[slot];
    std::vector<int> &motion_dx = state.motion_dx[slot];
    std::vector<int> &motion_dy = state.motion_dy[slot];
    std::vector<int> &motion_cap_x = state.motion_cap_x[slot];
    std::vector<int> &motion_cap_y = state.motion_cap_y[slot];
    if ((int)local_dx.size() < frame_count)
        local_dx.resize((size_t)frame_count, 0);
    else if ((int)local_dx.size() > frame_count)
        local_dx.resize((size_t)frame_count);
    if ((int)local_dy.size() < frame_count)
        local_dy.resize((size_t)frame_count, 0);
    else if ((int)local_dy.size() > frame_count)
        local_dy.resize((size_t)frame_count);
    if ((int)visible_from.size() < frame_count)
        visible_from.resize((size_t)frame_count, 0);
    else if ((int)visible_from.size() > frame_count)
        visible_from.resize((size_t)frame_count);
    if ((int)visible_until.size() < frame_count)
        visible_until.resize((size_t)frame_count, 0);
    else if ((int)visible_until.size() > frame_count)
        visible_until.resize((size_t)frame_count);
    if ((int)motion_dx.size() < frame_count)
        motion_dx.resize((size_t)frame_count, 0);
    else if ((int)motion_dx.size() > frame_count)
        motion_dx.resize((size_t)frame_count);
    if ((int)motion_dy.size() < frame_count)
        motion_dy.resize((size_t)frame_count, 0);
    else if ((int)motion_dy.size() > frame_count)
        motion_dy.resize((size_t)frame_count);
    if ((int)motion_cap_x.size() < frame_count)
        motion_cap_x.resize((size_t)frame_count, 0);
    else if ((int)motion_cap_x.size() > frame_count)
        motion_cap_x.resize((size_t)frame_count);
    if ((int)motion_cap_y.size() < frame_count)
        motion_cap_y.resize((size_t)frame_count, 0);
    else if ((int)motion_cap_y.size() > frame_count)
        motion_cap_y.resize((size_t)frame_count);
    for (int &dx : local_dx)
        dx = ClampWorldMarkedAniptDelta(dx);
    for (int &dy : local_dy)
        dy = ClampWorldMarkedAniptDelta(dy);
    for (int &show_tick : visible_from)
        show_tick = ClampWorldMarkedVisibleFrom(show_tick);
    for (int &hide_tick : visible_until)
        hide_tick = ClampWorldMarkedVisibleUntil(hide_tick);
    for (int &dx : motion_dx)
        dx = ClampWorldMarkedMotion(dx);
    for (int &dy : motion_dy)
        dy = ClampWorldMarkedMotion(dy);
    for (int &cap : motion_cap_x)
        cap = ClampWorldMarkedMotionCap(cap);
    for (int &cap : motion_cap_y)
        cap = ClampWorldMarkedMotionCap(cap);

    std::vector<int> &fmir = state.frame_mirror[slot];
    if ((int)fmir.size() < frame_count)
        fmir.resize((size_t)frame_count, 0);
    else if ((int)fmir.size() > frame_count)
        fmir.resize((size_t)frame_count);
    for (int &mirror : fmir)
        mirror = ClampWorldMarkedFrameMirror(mirror);

    std::vector<int> &fz = state.frame_z[slot];
    std::vector<int> &don = state.dual_on[slot];
    std::vector<int> &ddx = state.dual_dx[slot];
    std::vector<int> &ddy = state.dual_dy[slot];
    std::vector<int> &dz = state.dual_z[slot];
    fz.resize((size_t)frame_count, 0);
    don.resize((size_t)frame_count, 0);
    ddx.resize((size_t)frame_count, 0);
    ddy.resize((size_t)frame_count, 0);
    dz.resize((size_t)frame_count, 0);
    for (int &z : fz)
        z = ClampWorldMarkedZ(z);
    for (int &on : don)
        on = on ? 1 : 0;
    for (int &dx : ddx)
        dx = ClampWorldMarkedAniptDelta(dx);
    for (int &dy : ddy)
        dy = ClampWorldMarkedAniptDelta(dy);
    for (int &z : dz)
        z = ClampWorldMarkedZ(z);

    /* End-resize only; callers that splice frames in the middle insert/erase
       matching entries here explicitly, same as they do for the other
       per-fi arrays. */
    state.entry_pieces[slot].resize((size_t)frame_count);

    std::vector<int> &fdoc = state.frame_doc[slot];
    size_t old_fdoc_size = fdoc.size();
    fdoc.resize((size_t)frame_count);
    for (size_t i = old_fdoc_size; i < fdoc.size(); i++)
        fdoc[i] = -1; /* defer to this row's own sequence_doc_uid[slot] */
}

int WorldMarkedTickForFrame(WorldMarkedSequenceState &state, int slot,
                            int frame_count, int frame_idx)
{
    EnsureWorldMarkedFrameDelays(state, slot, frame_count);
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0) return 0;
    if (frame_idx < 0) frame_idx = 0;
    if (frame_idx >= frame_count) frame_idx = frame_count - 1;
    int tick = 0;
    for (int i = 0; i < frame_idx; i++)
        tick += ClampTimelineHold(state.frame_delays[slot][i]);
    return tick;
}

int WorldMarkedSequenceTicks(WorldMarkedSequenceState &state, int slot, int frame_count)
{
    EnsureWorldMarkedFrameDelays(state, slot, frame_count);
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0) return 1;
    int ticks = 0;
    for (int i = 0; i < frame_count; i++)
        ticks += ClampTimelineHold(state.frame_delays[slot][i]);
    return ticks > 0 ? ticks : 1;
}

int WorldMarkedFrameForTick(WorldMarkedSequenceState &state, int slot,
                            int frame_count, int tick, bool hold_final)
{
    EnsureWorldMarkedFrameDelays(state, slot, frame_count);
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || frame_count <= 0) return 0;
    int cycle_ticks = WorldMarkedSequenceTicks(state, slot, frame_count);
    if (hold_final && tick >= cycle_ticks) return frame_count - 1;

    int t = tick % cycle_ticks;
    if (t < 0) t += cycle_ticks;
    for (int i = 0; i < frame_count; i++) {
        int delay = ClampTimelineHold(state.frame_delays[slot][i]);
        if (t < delay) return i;
        t -= delay;
    }
    return frame_count - 1;
}

static int WorldMarkedAbsMotion(int value)
{
    return value < 0 ? -value : value;
}

/* Read-only preview of the tick WorldMarkedApplyYLinkChain's reverse pass
   would start at, given the current Count/Gap/Delay/vY/PongDelay fields —
   mirrors that function's stop_tick/reverse_start math without touching
   any state, so the UI can show "reverses at tick N" as the fields change. */
static int WorldMarkedChainReverseStartTick(WorldMarkedSequenceState &state,
                                            const WorldMarkedLane &lane,
                                            int start_frame, int link_count,
                                            int gap_px, int delay_ticks,
                                            int visual_vy,
                                            int ping_pong_delay_ticks)
{
    int slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return 0;

    const std::vector<int> &frames = state.sequence_frames[slot].empty()
                                    ? lane.frames : state.sequence_frames[slot];
    int n = (int)frames.size();
    if (n <= 0)
        return 0;
    if (start_frame < 0) start_frame = 0;
    if (start_frame >= n) start_frame = n - 1;

    if (link_count < 1) link_count = 1;
    if (link_count > 32) link_count = 32;
    if (gap_px < 1) gap_px = 1;
    if (gap_px > 9999) gap_px = 9999;
    if (delay_ticks < 0) delay_ticks = 0;
    if (delay_ticks > 9999) delay_ticks = 9999;
    ping_pong_delay_ticks = ClampWorldMarkedVisibleFrom(ping_pong_delay_ticks);
    visual_vy = ClampWorldMarkedMotion(visual_vy);
    if (visual_vy == 0) visual_vy = 1;

    EnsureWorldMarkedFrameDelays(state, slot, n);
    int base_show = state.visible_from[slot][start_frame];
    if (base_show <= 0 && state.visible_until[slot][start_frame] <= 0)
        base_show = WorldMarkedTickForFrame(state, slot, n, start_frame);
    base_show = ClampWorldMarkedVisibleFrom(base_show);

    int speed = WorldMarkedAbsMotion(visual_vy);
    if (speed < 1) speed = 1;
    int phase_ticks = (gap_px + speed - 1) / speed;
    if (phase_ticks < 1) phase_ticks = 1;
    int launch_delay = delay_ticks > 0 ? delay_ticks : phase_ticks;
    if (launch_delay < 1) launch_delay = 1;

    int last_spawn_offset = (link_count - 1) <= 1 ? 0 : (link_count - 2) * launch_delay;
    int stop_tick = base_show + last_spawn_offset;
    return stop_tick + ping_pong_delay_ticks;
}

static void WorldMarkedApplyYLinkChain(WorldMarkedSequenceState &state,
                                       const WorldMarkedLane &lane,
                                       int start_frame, int link_count,
                                       int gap_px, int delay_ticks,
                                       int visual_vy, bool ping_pong,
                                       int ping_pong_delay_ticks)
{
    int slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return;

    std::vector<int> &frames = state.sequence_frames[slot];
    if (frames.empty())
        frames = lane.frames;
    int n = (int)frames.size();
    if (n <= 0)
        return;
    if (start_frame < 0) start_frame = 0;
    if (start_frame >= n) start_frame = n - 1;

    if (link_count < 1) link_count = 1;
    if (link_count > 32) link_count = 32;
    if (gap_px < 1) gap_px = 1;
    if (gap_px > 9999) gap_px = 9999;
    if (delay_ticks < 0) delay_ticks = 0;
    if (delay_ticks > 9999) delay_ticks = 9999;
    ping_pong_delay_ticks = ClampWorldMarkedVisibleFrom(ping_pong_delay_ticks);
    visual_vy = ClampWorldMarkedMotion(visual_vy);
    if (visual_vy == 0) visual_vy = 1;

    EnsureWorldMarkedFrameDelays(state, slot, n);
    const int base_frame = frames[(size_t)start_frame];
    const int base_dx = state.local_dx[slot][start_frame];
    const int base_dy = state.local_dy[slot][start_frame];
    const int base_mirror = state.frame_mirror[slot][start_frame];
    const int base_z = state.frame_z[slot][start_frame];

    int base_show = state.visible_from[slot][start_frame];
    if (base_show <= 0 && state.visible_until[slot][start_frame] <= 0)
        base_show = WorldMarkedTickForFrame(state, slot, n, start_frame);
    base_show = ClampWorldMarkedVisibleFrom(base_show);

    int run_len = 1;
    while (start_frame + run_len < n &&
           frames[(size_t)(start_frame + run_len)] == base_frame) {
        run_len++;
    }

    std::vector<WorldSeqArrayRef> refs = WorldMarkedSeqArrays(state, slot);
    int speed = WorldMarkedAbsMotion(visual_vy);
    if (speed < 1) speed = 1;
    int phase_ticks = (gap_px + speed - 1) / speed;
    if (phase_ticks < 1) phase_ticks = 1;
    int launch_delay = delay_ticks > 0 ? delay_ticks : phase_ticks;
    if (launch_delay < 1) launch_delay = 1;
    int delay = ClampTimelineHold(launch_delay);
    int hide_at = ClampWorldMarkedVisibleUntil(99999);
    /* Every link gets a forward (falling) entry; ping-pong adds one
       reverse (rising) entry per link, all starting the same tick. */
    int entry_count = ping_pong ? (link_count * 2) : link_count;

    std::vector<std::vector<int>> &entry_pieces = state.entry_pieces[slot];
    if (run_len < entry_count) {
        int insert_at = start_frame + run_len;
        int add = entry_count - run_len;
        frames.insert(frames.begin() + insert_at, (size_t)add, base_frame);
        for (const WorldSeqArrayRef &ref : refs) {
            int seed = (start_frame < (int)ref.vec->size())
                     ? (*ref.vec)[(size_t)start_frame] : ref.fresh;
            ref.vec->insert(ref.vec->begin() + insert_at, (size_t)add, seed);
        }
        entry_pieces.insert(entry_pieces.begin() + insert_at, (size_t)add,
                            std::vector<int>());
    } else if (run_len > entry_count) {
        int erase_first = start_frame + entry_count;
        int erase_last = start_frame + run_len;
        frames.erase(frames.begin() + erase_first, frames.begin() + erase_last);
        for (const WorldSeqArrayRef &ref : refs)
            ref.vec->erase(ref.vec->begin() + erase_first,
                           ref.vec->begin() + erase_last);
        entry_pieces.erase(entry_pieces.begin() + erase_first,
                           entry_pieces.begin() + erase_last);
    }

    n = (int)frames.size();
    EnsureWorldMarkedFrameDelays(state, slot, n);
    /* Chain links are always single-image; clear any composite a prior
       Use Subframe build may have left at these indices. */
    for (int i = 0; i < entry_count; i++)
        entry_pieces[(size_t)(start_frame + i)].clear();

    /* Link 0 (the anchor) and link 1 (stacked one sprite-length above it,
       at -gap_px) both spawn immediately. Every later link spawns one
       travel-phase after the previous one, always at the same -gap_px
       slot, so the chain reads as a continuous feed rather than copies
       sliding out from underneath the anchor. Once the last link spawns
       (stop_tick) every link freezes in place — that's the held, fully
       built formation. Ping Pong holds that freeze for ping_pong_delay_
       ticks, then reverses all links together from their frozen spots. */
    auto spawn_offset = [&](int i) {
        return (i <= 1) ? 0 : (i - 1) * launch_delay;
    };
    int last_spawn_offset = spawn_offset(link_count - 1);
    int stop_tick = base_show + last_spawn_offset;
    int reverse_start = stop_tick + (ping_pong ? ping_pong_delay_ticks : 0);

    for (int i = 0; i < link_count; i++) {
        int fi = start_frame + i;
        int spawn_tick = base_show + spawn_offset(i);
        int stack_y = (i == 0) ? 0 : gap_px;
        int elapsed_at_stop = stop_tick - spawn_tick;
        if (elapsed_at_stop < 0) elapsed_at_stop = 0;
        int forward_cap = speed * elapsed_at_stop;

        frames[(size_t)fi] = base_frame;
        state.frame_delays[slot][fi] = delay;
        state.local_dx[slot][fi] = base_dx;
        state.local_dy[slot][fi] = ClampWorldMarkedAniptDelta(base_dy + stack_y);
        state.visible_from[slot][fi] = ClampWorldMarkedVisibleFrom(spawn_tick);
        state.visible_until[slot][fi] = ping_pong
            ? ClampWorldMarkedVisibleUntil(reverse_start)
            : hide_at;
        state.motion_dx[slot][fi] = 0;
        /* A cap of 0 reads as "uncapped" to the renderer, so a link that
           is already at its frozen spot the instant it spawns (forward_cap
           == 0, always true for the last-spawned link) must get zero
           velocity instead of relying on the cap to hold it still. */
        state.motion_dy[slot][fi] = forward_cap > 0 ? visual_vy : 0;
        state.motion_cap_x[slot][fi] = 0;
        state.motion_cap_y[slot][fi] = ClampWorldMarkedMotionCap(forward_cap);
        state.frame_mirror[slot][fi] = base_mirror;
        state.frame_z[slot][fi] = base_z;
        state.dual_on[slot][fi] = 0;
        state.dual_dx[slot][fi] = 0;
        state.dual_dy[slot][fi] = 0;
        state.dual_z[slot][fi] = 0;
    }

    /* Reverse pass: every link flips direction together at reverse_start
       (from its frozen, fully-built position) and rises until its
       trailing edge clears the source sprite's top (Y=0), i.e. until it
       reaches the same -gap_px slot it once spawned from, at which point
       it is hidden. */
    if (ping_pong) {
        for (int i = 0; i < link_count; i++) {
            int fi = start_frame + link_count + i;
            int spawn_tick = base_show + spawn_offset(i);
            int stack_y = (i == 0) ? 0 : gap_px;
            int elapsed_at_stop = stop_tick - spawn_tick;
            if (elapsed_at_stop < 0) elapsed_at_stop = 0;
            int pos_at_stop = stack_y - visual_vy * elapsed_at_stop;
            int travel = gap_px - pos_at_stop;
            if (travel < 0) travel = 0;
            int reverse_ticks = (travel + speed - 1) / speed;
            if (reverse_ticks < 1) reverse_ticks = 1;

            frames[(size_t)fi] = base_frame;
            state.frame_delays[slot][fi] = delay;
            state.local_dx[slot][fi] = base_dx;
            state.local_dy[slot][fi] =
                ClampWorldMarkedAniptDelta(base_dy + pos_at_stop);
            state.visible_from[slot][fi] =
                ClampWorldMarkedVisibleFrom(reverse_start);
            state.visible_until[slot][fi] =
                ClampWorldMarkedVisibleUntil(reverse_start + reverse_ticks);
            state.motion_dx[slot][fi] = 0;
            /* Same uncapped-at-zero pitfall as the forward pass: the
               last-spawned link is already at the hide threshold the
               instant it starts reversing (travel == 0), so it needs zero
               velocity rather than a 0 cap. */
            state.motion_dy[slot][fi] = travel > 0 ? -visual_vy : 0;
            state.motion_cap_x[slot][fi] = 0;
            state.motion_cap_y[slot][fi] = ClampWorldMarkedMotionCap(travel);
            state.frame_mirror[slot][fi] = base_mirror;
            state.frame_z[slot][fi] = base_z;
            state.dual_on[slot][fi] = 0;
            state.dual_dx[slot][fi] = 0;
            state.dual_dy[slot][fi] = 0;
            state.dual_z[slot][fi] = 0;
        }
    }

    state.hold_end[slot] = true;
    state.paused = true;
    state.timer = 0.0f;
    state.frame = base_show;
}

static std::string WorldSpriteSourceGroup(const IMG *img)
{
    if (!img || !img->src_filename[0])
        return std::string("Workspace");
    return std::string(img->src_filename);
}

static bool WorldSpriteNameBelongsToParent(const std::string &child_name,
                                           const std::string &parent_name)
{
    if (child_name.empty() || parent_name.empty())
        return false;

    if (InferSubframeParentName(child_name.c_str()) == parent_name)
        return true;

    std::string numbered_parent;
    return strip_trailing_sequence_digits(child_name, &numbered_parent) &&
           numbered_parent == parent_name;
}

static int WorldCollectSubframesForParent(Document *doc,
                                          int parent_idx,
                                          std::vector<int> &out)
{
    out.clear();
    IMG *parent = doc_get_img(doc, parent_idx);
    if (!doc || !parent)
        return 0;

    std::string parent_name = img_name_string(parent);
    if (parent_name.empty())
        return 0;
    std::string parent_src = WorldSpriteSourceGroup(parent);

    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (idx == parent_idx || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        if (WorldSpriteSourceGroup(img) != parent_src)
            continue;
        if (WorldSpriteNameBelongsToParent(img_name_string(img), parent_name))
            out.push_back(idx);
    }
    return (int)out.size();
}

static bool WorldMarkedSequenceSlotEditableForSubframes(int slot)
{
    return (slot >= 0 && slot < kWorldMarkedSourceTabs) ||
           slot == kWorldEmbeddedSeqScrSlot;
}

static bool WorldMarkedSubframeRunContains(const std::vector<int> &subframes,
                                           int img_idx)
{
    return std::find(subframes.begin(), subframes.end(), img_idx) != subframes.end();
}

static bool WorldMarkedSwapEntryWithSubframesAtTick(
    WorldMarkedSequenceState &state,
    WorldMarkedLane &lane,
    int parent_frame_idx,
    int swap_tick,
    int *subframe_count_out)
{
    if (subframe_count_out) *subframe_count_out = 0;

    int slot = lane.delay_slot;
    if (!WorldMarkedSequenceSlotEditableForSubframes(slot) || !lane.doc)
        return false;

    std::vector<int> &frames = state.sequence_frames[slot];
    if (frames.empty())
        frames = lane.frames;
    int n = (int)frames.size();
    if (n <= 0)
        return false;
    if (parent_frame_idx < 0)
        parent_frame_idx = 0;
    if (parent_frame_idx >= n)
        parent_frame_idx = n - 1;

    EnsureWorldMarkedFrameDelays(state, slot, n);

    int parent_idx = frames[(size_t)parent_frame_idx];
    std::vector<int> subframes;
    if (WorldCollectSubframesForParent(lane.doc, parent_idx, subframes) <= 0)
        return false;

    swap_tick = ClampWorldMarkedVisibleFrom(swap_tick);

    int parent_start_tick =
        WorldMarkedTickForFrame(state, slot, n, parent_frame_idx);
    int parent_hold = swap_tick - parent_start_tick;
    if (parent_hold > 0)
        state.frame_delays[slot][parent_frame_idx] =
            ClampTimelineHold(parent_hold);

    int sub_dx = 0;
    int sub_dy = 0;
    WorldMarkedEffectiveLocalDelta(state, slot, n, parent_frame_idx,
                                   false, swap_tick, &sub_dx, &sub_dy);

    int parent_vx = state.motion_dx[slot][parent_frame_idx];
    int parent_vy = state.motion_dy[slot][parent_frame_idx];
    int sub_vx = parent_vx;
    int sub_vy = parent_vy;
    int sub_cap_x = state.motion_cap_x[slot][parent_frame_idx];
    int sub_cap_y = state.motion_cap_y[slot][parent_frame_idx];
    int elapsed = WorldMarkedEntryMotionElapsed(state, slot, n,
                                                parent_frame_idx,
                                                swap_tick);
    if (sub_cap_x > 0) {
        int moved_x = WorldMarkedAbsMotion(
            WorldMarkedClampedMotion(parent_vx, elapsed, sub_cap_x));
        sub_cap_x -= moved_x;
        if (sub_cap_x <= 0) {
            sub_cap_x = 0;
            sub_vx = 0;
        }
    }
    if (sub_cap_y > 0) {
        int moved_y = WorldMarkedAbsMotion(
            WorldMarkedClampedMotion(parent_vy, elapsed, sub_cap_y));
        sub_cap_y -= moved_y;
        if (sub_cap_y <= 0) {
            sub_cap_y = 0;
            sub_vy = 0;
        }
    }

    int parent_mirror = state.frame_mirror[slot][parent_frame_idx];
    int parent_z = state.frame_z[slot][parent_frame_idx];

    /* Any earlier swap at this entry inserted either a run of separate
       subframe entries (the old behavior) or a single composite entry (this
       function's own prior output) right after the parent. Either way, drop
       it before inserting the new composite so repeated swaps don't pile up. */
    int erase_first = parent_frame_idx + 1;
    int erase_last = erase_first;
    while (erase_last < (int)frames.size() &&
           (WorldMarkedSubframeRunContains(subframes, frames[(size_t)erase_last]) ||
            !state.entry_pieces[slot][(size_t)erase_last].empty())) {
        erase_last++;
    }

    std::vector<WorldSeqArrayRef> refs = WorldMarkedSeqArrays(state, slot);
    std::vector<std::vector<int>> &entry_pieces = state.entry_pieces[slot];
    if (erase_last > erase_first) {
        frames.erase(frames.begin() + erase_first, frames.begin() + erase_last);
        for (const WorldSeqArrayRef &ref : refs)
            ref.vec->erase(ref.vec->begin() + erase_first,
                           ref.vec->begin() + erase_last);
        entry_pieces.erase(entry_pieces.begin() + erase_first,
                           entry_pieces.begin() + erase_last);
    }

    /* One composite entry replaces the parent from swap_tick onward — it
       draws every subframe together as a single whole frame instead of
       stepping through them as a sequence. */
    int insert_at = parent_frame_idx + 1;
    frames.insert(frames.begin() + insert_at, subframes[0]);
    for (const WorldSeqArrayRef &ref : refs)
        ref.vec->insert(ref.vec->begin() + insert_at, ref.fresh);
    entry_pieces.insert(entry_pieces.begin() + insert_at, std::vector<int>());

    n = (int)frames.size();
    EnsureWorldMarkedFrameDelays(state, slot, n);

    if (swap_tick <= 0) {
        state.visible_from[slot][parent_frame_idx] =
            ClampWorldMarkedVisibleFrom(99999);
        state.visible_until[slot][parent_frame_idx] = 1;
    } else {
        if (state.visible_from[slot][parent_frame_idx] >= swap_tick)
            state.visible_from[slot][parent_frame_idx] = 0;
        state.visible_until[slot][parent_frame_idx] =
            ClampWorldMarkedVisibleUntil(swap_tick);
    }

    state.frame_delays[slot][insert_at] = 1;
    state.local_dx[slot][insert_at] = sub_dx;
    state.local_dy[slot][insert_at] = sub_dy;
    state.visible_from[slot][insert_at] = swap_tick;
    state.visible_until[slot][insert_at] = 0;
    state.motion_dx[slot][insert_at] = sub_vx;
    state.motion_dy[slot][insert_at] = sub_vy;
    state.motion_cap_x[slot][insert_at] = sub_cap_x;
    state.motion_cap_y[slot][insert_at] = sub_cap_y;
    state.frame_mirror[slot][insert_at] = parent_mirror;
    state.frame_z[slot][insert_at] = parent_z;
    state.dual_on[slot][insert_at] = 0;
    state.dual_dx[slot][insert_at] = 0;
    state.dual_dy[slot][insert_at] = 0;
    state.dual_z[slot][insert_at] = 0;
    entry_pieces[insert_at] = subframes;

    WorldMarkedSetRowDoc(state, slot, lane.doc_idx);
    state.paused = true;
    state.timer = 0.0f;
    state.frame = swap_tick;
    if (subframe_count_out)
        *subframe_count_out = (int)subframes.size();
    return true;
}

/* Hands a (possibly composite) entry off to a finer set of subframes once
   its bottom crosses a configured waterline Y, then hides each fine piece
   individually as ITS OWN bottom crosses that same line — so a body sinking
   into water eats away piece by piece instead of clipping as one block. */
static bool WorldMarkedChopEntryAtWaterline(WorldMarkedSequenceState &state,
                                            WorldMarkedLane &lane,
                                            int parent_frame_idx,
                                            int *fine_count_out)
{
    if (fine_count_out) *fine_count_out = 0;

    int slot = lane.delay_slot;
    if (!WorldMarkedSequenceSlotEditableForSubframes(slot) || !lane.doc)
        return false;

    int fine_source = state.subframe_fine_source[slot];
    int waterline_y = state.subframe_waterline_y[slot];
    if (fine_source < 0 || waterline_y <= 0)
        return false;

    std::vector<int> &frames = state.sequence_frames[slot];
    if (frames.empty())
        frames = lane.frames;
    int n = (int)frames.size();
    if (n <= 0)
        return false;
    if (parent_frame_idx < 0)
        parent_frame_idx = 0;
    if (parent_frame_idx >= n)
        parent_frame_idx = n - 1;

    EnsureWorldMarkedFrameDelays(state, slot, n);

    std::vector<int> fine_subframes;
    if (WorldCollectSubframesForParent(lane.doc, fine_source, fine_subframes) <= 0)
        return false;

    int parent_local_dy = state.local_dy[slot][parent_frame_idx];
    int parent_vy = state.motion_dy[slot][parent_frame_idx];
    int parent_mirror = state.frame_mirror[slot][parent_frame_idx];
    bool mirror_y = (parent_mirror & kWorldFrameMirrorY) != 0;
    int parent_z = state.frame_z[slot][parent_frame_idx];

    int motion_start = WorldMarkedEntryMotionStartTick(state, slot, n, parent_frame_idx);
    int ticks_to_chop = WorldMarkedTicksUntilYBreach(lane, parent_frame_idx,
                                                      parent_local_dy, mirror_y,
                                                      parent_vy, waterline_y, 9999);
    int chop_tick = ClampWorldMarkedVisibleFrom(motion_start + ticks_to_chop);

    if (state.visible_from[slot][parent_frame_idx] >= chop_tick)
        state.visible_from[slot][parent_frame_idx] = 0;
    state.visible_until[slot][parent_frame_idx] =
        ClampWorldMarkedVisibleUntil(chop_tick);

    int sub_dx = 0;
    int sub_dy = 0;
    WorldMarkedEffectiveLocalDelta(state, slot, n, parent_frame_idx,
                                   false, chop_tick, &sub_dx, &sub_dy);

    int parent_vx = state.motion_dx[slot][parent_frame_idx];
    int sub_vx = parent_vx;
    int sub_vy = parent_vy;
    int sub_cap_x = state.motion_cap_x[slot][parent_frame_idx];
    int sub_cap_y = state.motion_cap_y[slot][parent_frame_idx];
    int elapsed = WorldMarkedEntryMotionElapsed(state, slot, n,
                                                parent_frame_idx, chop_tick);
    if (sub_cap_x > 0) {
        int moved_x = WorldMarkedAbsMotion(
            WorldMarkedClampedMotion(parent_vx, elapsed, sub_cap_x));
        sub_cap_x -= moved_x;
        if (sub_cap_x <= 0) {
            sub_cap_x = 0;
            sub_vx = 0;
        }
    }
    if (sub_cap_y > 0) {
        int moved_y = WorldMarkedAbsMotion(
            WorldMarkedClampedMotion(parent_vy, elapsed, sub_cap_y));
        sub_cap_y -= moved_y;
        if (sub_cap_y <= 0) {
            sub_cap_y = 0;
            sub_vy = 0;
        }
    }

    /* Drop any fine-piece run a previous chop at this entry already left
       behind before inserting the new one. */
    int erase_first = parent_frame_idx + 1;
    int erase_last = erase_first;
    while (erase_last < (int)frames.size() &&
           WorldMarkedSubframeRunContains(fine_subframes, frames[(size_t)erase_last])) {
        erase_last++;
    }

    std::vector<WorldSeqArrayRef> refs = WorldMarkedSeqArrays(state, slot);
    std::vector<std::vector<int>> &entry_pieces = state.entry_pieces[slot];
    if (erase_last > erase_first) {
        frames.erase(frames.begin() + erase_first, frames.begin() + erase_last);
        for (const WorldSeqArrayRef &ref : refs)
            ref.vec->erase(ref.vec->begin() + erase_first,
                           ref.vec->begin() + erase_last);
        entry_pieces.erase(entry_pieces.begin() + erase_first,
                           entry_pieces.begin() + erase_last);
    }

    int insert_at = parent_frame_idx + 1;
    frames.insert(frames.begin() + insert_at,
                  fine_subframes.begin(), fine_subframes.end());
    for (const WorldSeqArrayRef &ref : refs)
        ref.vec->insert(ref.vec->begin() + insert_at,
                        fine_subframes.size(), ref.fresh);
    entry_pieces.insert(entry_pieces.begin() + insert_at,
                        fine_subframes.size(), std::vector<int>());

    n = (int)frames.size();
    EnsureWorldMarkedFrameDelays(state, slot, n);

    for (size_t i = 0; i < fine_subframes.size(); i++) {
        int fi = insert_at + (int)i;
        int piece_ticks = WorldMarkedPieceTicksUntilYBreach(
            lane.doc, fine_subframes[i], sub_dy, mirror_y, sub_vy, waterline_y, 9999);
        state.frame_delays[slot][fi] = 1;
        state.local_dx[slot][fi] = sub_dx;
        state.local_dy[slot][fi] = sub_dy;
        state.visible_from[slot][fi] = ClampWorldMarkedVisibleFrom(chop_tick);
        state.visible_until[slot][fi] =
            ClampWorldMarkedVisibleUntil(chop_tick + piece_ticks);
        state.motion_dx[slot][fi] = sub_vx;
        state.motion_dy[slot][fi] = sub_vy;
        state.motion_cap_x[slot][fi] = sub_cap_x;
        state.motion_cap_y[slot][fi] = sub_cap_y;
        state.frame_mirror[slot][fi] = parent_mirror;
        state.frame_z[slot][fi] = parent_z;
        state.dual_on[slot][fi] = 0;
        state.dual_dx[slot][fi] = 0;
        state.dual_dy[slot][fi] = 0;
        state.dual_z[slot][fi] = 0;
    }

    WorldMarkedSetRowDoc(state, slot, lane.doc_idx);
    state.paused = true;
    state.timer = 0.0f;
    state.frame = chop_tick;
    if (fine_count_out)
        *fine_count_out = (int)fine_subframes.size();
    return true;
}

static bool WorldDrawSubframeSwapTool(WorldMarkedSequenceState &state,
                                      WorldMarkedLane &lane,
                                      int edit_fi,
                                      bool embedded)
{
    int slot = lane.delay_slot;
    /* Gated here rather than at the call sites so the embedded workspace and
       the lane strip cannot drift apart. */
    if (!g_world_show_subframe_tool) return false;
    if (lane.dummy_decap || !WorldMarkedSequenceSlotEditableForSubframes(slot) ||
        !lane.doc)
        return false;

    const std::vector<int> &frames = state.sequence_frames[slot].empty()
                                   ? lane.frames
                                   : state.sequence_frames[slot];
    if (edit_fi < 0 || edit_fi >= (int)frames.size())
        return false;

    std::vector<int> subframes;
    if (WorldCollectSubframesForParent(lane.doc, frames[(size_t)edit_fi],
                                       subframes) <= 0)
        return false;

    WorldMarkedClampAutoChainSettings(state, slot);
    int &swap_tick = state.subframe_swap_tick[slot];
    const char *kind = embedded ? "embed" : "lane";
    bool changed = false;

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Subframes");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Replace the selected sprite with one composite of its child subframes, drawn together as a single whole frame from a tick onward (not stepped through as a sequence).");
    ImGui::SameLine();
    ImGui::TextDisabled("%d", (int)subframes.size());
    ImGui::SameLine();
    ImGui::TextDisabled("Tick");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    char tick_id[64];
    snprintf(tick_id, sizeof(tick_id), "##world_%s_subframe_tick", kind);
    if (ImGui::InputInt(tick_id, &swap_tick, 0, 0))
        swap_tick = ClampWorldMarkedVisibleFrom(swap_tick);
    ImGui::SameLine();
    char now_id[64];
    snprintf(now_id, sizeof(now_id), "Now##world_%s_subframe_now", kind);
    if (ImGui::SmallButton(now_id))
        swap_tick = ClampWorldMarkedVisibleFrom(state.frame);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Use the current World View tick.");
    ImGui::SameLine();
    char swap_id[80];
    snprintf(swap_id, sizeof(swap_id), "Swap with Subframes##world_%s_subframe_swap", kind);
    if (ImGui::SmallButton(swap_id)) {
        int subframe_count = 0;
        if (WorldMarkedSwapEntryWithSubframesAtTick(state, lane, edit_fi,
                                                     swap_tick,
                                                     &subframe_count)) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "World View: composited parent into %d subframe%s at tick %d.",
                     subframe_count, subframe_count == 1 ? "" : "s", swap_tick);
            g_restore_msg_timer = 4.0f;
            changed = true;
        }
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Waterline Y");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Absolute world Y. The composite hands off to the fine subframes once its bottom crosses this line, and each fine piece hides individually as its own bottom crosses it.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(56.0f);
    char waterline_id[64];
    snprintf(waterline_id, sizeof(waterline_id), "##world_%s_waterline_y", kind);
    int &waterline_y = state.subframe_waterline_y[slot];
    if (ImGui::InputInt(waterline_id, &waterline_y, 0, 0)) {
        if (waterline_y < 0) waterline_y = 0;
        if (waterline_y > 9999) waterline_y = 9999;
    }
    ImGui::SameLine();
    int fine_source = state.subframe_fine_source[slot];
    IMG *fine_img = fine_source >= 0 ? doc_get_img(lane.doc, fine_source) : NULL;
    std::string fine_label = fine_img ? img_name_string(fine_img) : std::string("none");
    ImGui::TextDisabled("Fine: %s", fine_label.c_str());
    ImGui::SameLine();
    char pick_id[64];
    snprintf(pick_id, sizeof(pick_id), "Pick##world_%s_fine_pick", kind);
    if (ImGui::SmallButton(pick_id))
        state.subframe_fine_source[slot] = g_doc ? g_doc->ilselected : -1;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Capture the sprite currently selected in the editor as the fine-subframe source (its children are found the same way the Subframes above are).");
    ImGui::SameLine();
    char chop_id[64];
    snprintf(chop_id, sizeof(chop_id), "Chop at Waterline##world_%s_chop", kind);
    bool chop_ready = fine_source >= 0 && waterline_y > 0;
    ImGui::BeginDisabled(!chop_ready);
    if (ImGui::SmallButton(chop_id)) {
        int fine_count = 0;
        if (WorldMarkedChopEntryAtWaterline(state, lane, edit_fi, &fine_count)) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "World View: chopped into %d fine subframe%s at waterline Y=%d.",
                     fine_count, fine_count == 1 ? "" : "s", waterline_y);
            g_restore_msg_timer = 4.0f;
            changed = true;
        }
    }
    ImGui::EndDisabled();
    if (!chop_ready && ImGui::IsItemHovered())
        ImGui::SetTooltip("Set a Fine source and a Waterline Y above 0 first.");

    return changed;
}

void WorldMarkedClearSequenceState(WorldMarkedSequenceState &state, int slot)
{
    WorldFrameSelClear();   /* see WorldMarkedDuplicateSequenceEntry */
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->clear();
    state.entry_pieces[slot].clear();
    state.frame_doc[slot].clear();
    state.pingpong_delay[slot] = 0;
    state.stop_tick[slot] = 0;
    state.lane_rigid[slot] = false;
    state.slot_hold[slot] = ClampTimelineHold(state.default_hold);
    state.slot_hold_custom[slot] = false;
}

/* frame_doc[slot] stores a doc TAB INDEX per entry, not a Document* — the
   document list can reshuffle or replace its backing storage on reorder/
   close, so a pointer cached across frames can dangle. -1 means "this row's
   own doc". Always re-derive the pointer via document_get() the same frame
   it's used; never store the result anywhere longer-lived than a lane. */
Document *WorldMarkedResolveEntryDoc(Document *row_doc, int doc_idx_override)
{
    if (doc_idx_override < 0) return row_doc;
    Document *resolved = document_get(doc_idx_override);
    return resolved ? resolved : row_doc;
}

std::vector<Document*> WorldMarkedResolveFrameDocs(Document *row_doc,
                                                   const std::vector<int> &doc_idx_overrides)
{
    std::vector<Document*> out(doc_idx_overrides.size());
    for (size_t i = 0; i < doc_idx_overrides.size(); i++)
        out[i] = WorldMarkedResolveEntryDoc(row_doc, doc_idx_overrides[i]);
    return out;
}

void WorldMarkedBuildSingleFrameLane(Document *doc, const std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels,
                                     const std::vector<std::vector<int>> *piece_overrides,
                                     const std::vector<int> *doc_idx_overrides)
{
    frame_pieces.clear();
    frame_labels.clear();
    frame_pieces.reserve(frames.size());
    frame_labels.reserve(frames.size());
    for (size_t i = 0; i < frames.size(); i++) {
        int idx = frames[i];
        if (piece_overrides && i < piece_overrides->size() &&
            !(*piece_overrides)[i].empty())
            frame_pieces.push_back((*piece_overrides)[i]);
        else
            frame_pieces.push_back(std::vector<int>(1, idx));
        int doc_idx_override = (doc_idx_overrides && i < doc_idx_overrides->size())
                             ? (*doc_idx_overrides)[i] : -1;
        Document *entry_doc = WorldMarkedResolveEntryDoc(doc, doc_idx_override);
        frame_labels.push_back(img_name_string(doc_get_img(entry_doc, idx)));
    }
}

void WorldRefreshMarkedLaneAfterSequenceEdit(WorldMarkedSequenceState &state,
                                             WorldMarkedLane &lane,
                                             int &edit_frame)
{
    if (!lane.dummy_decap) {
        lane.frames = state.sequence_frames[lane.delay_slot];
        EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());
        WorldMarkedBuildSingleFrameLane(lane.doc, lane.frames,
                                        lane.frame_pieces, lane.frame_labels,
                                        &state.entry_pieces[lane.delay_slot],
                                        &state.frame_doc[lane.delay_slot]);
        lane.frame_docs = WorldMarkedResolveFrameDocs(lane.doc, state.frame_doc[lane.delay_slot]);
    }

    EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());
    lane.tick = WorldMarkedEffectiveTickForSlot(state, lane.delay_slot,
                                                (int)lane.frames.size(),
                                                state.frame);
    lane.frame_pos = WorldMarkedFrameForTick(state, lane.delay_slot,
                                             (int)lane.frames.size(),
                                             lane.tick,
                                             state.hold_end[lane.delay_slot]);
    if (lane.frame_pos < 0) lane.frame_pos = 0;
    if (lane.frame_pos >= (int)lane.frames.size())
        lane.frame_pos = (int)lane.frames.size() - 1;
    Document *cur_doc = (lane.frame_pos >= 0 &&
                         lane.frame_pos < (int)lane.frame_docs.size() &&
                         lane.frame_docs[lane.frame_pos])
                      ? lane.frame_docs[lane.frame_pos] : lane.doc;
    lane.img = (lane.frame_pos >= 0 && lane.frame_pos < (int)lane.frames.size())
             ? doc_get_img(cur_doc, lane.frames[lane.frame_pos])
             : NULL;
    edit_frame = lane.frame_pos;
}

void WorldMarkedSyncSequenceOverride(WorldMarkedSequenceState &state, int slot,
                                     Document *doc, int doc_idx,
                                     std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs || !doc)
        return;

    const std::vector<int> defaults = frames;
    bool doc_changed = WorldMarkedRowDoc(state, slot) != doc ||
                       WorldMarkedRowDocIndex(state, slot) != doc_idx;
    /* Whether this slot has ever been seeded for the current doc, NOT
       whether it currently holds any frames. A row the user drained to zero
       (e.g. by dragging its last frame into another row) must stay empty —
       checking default_frames instead of sequence_frames here keeps it from
       being mistaken for "nothing built yet" and silently reseeded from the
       marked set, which would undo the drag. */
    bool initialized = !state.default_frames[slot].empty();

    /* Does the persisted sequence still reference only frames the doc has?
       Entries dragged in from another row's document carry their own doc in
       frame_doc[slot], so check against that rather than assuming every
       entry belongs to this row's bound doc. */
    bool stale_entry = false;
    if (!doc_changed && initialized) {
        const std::vector<int> &cur_seq = state.sequence_frames[slot];
        const std::vector<int> &cur_fdoc = state.frame_doc[slot];
        for (size_t i = 0; i < cur_seq.size(); i++) {
            Document *entry_doc = WorldMarkedResolveEntryDoc(doc, i < cur_fdoc.size() ? cur_fdoc[i] : -1);
            if (!doc_get_img(entry_doc, cur_seq[i])) { stale_entry = true; break; }
        }
    }
    bool defaults_changed = state.default_frames[slot] != defaults;

    if (doc_changed || !initialized) {
        /* A different sprite/tab now occupies this slot (or there is nothing
           built yet): seed the sequence straight from the marked frames. */
        WorldMarkedSetRowDoc(state, slot, doc_idx);
        state.default_frames[slot] = defaults;
        state.sequence_frames[slot] = defaults;
        WorldMarkedClearSequenceState(state, slot);
        EnsureWorldMarkedFrameDelays(state, slot, (int)defaults.size());
    } else if (defaults_changed || stale_entry) {
        /* Same sprite, but the marked SET changed (the user marked another
           frame/sprite) or a referenced frame was deleted. Reconcile in place
           so the hand-built sequence survives: keep every still-marked entry
           in its current position with its per-entry edits, drop entries whose
           frame is gone or was unmarked, and append only the newly marked
           frames at the end. */
        EnsureWorldMarkedFrameDelays(state, slot,
            (int)state.sequence_frames[slot].size());

        std::vector<int> prev_defaults = state.default_frames[slot];
        std::vector<int> old_seq = state.sequence_frames[slot];
        std::vector<WorldSeqArrayRef> refs = WorldMarkedSeqArrays(state, slot);
        std::vector<std::vector<int>> old_vals;
        old_vals.reserve(refs.size());
        for (const WorldSeqArrayRef &ref : refs)
            old_vals.push_back(*ref.vec);
        std::vector<std::vector<int>> old_pieces = state.entry_pieces[slot];
        std::vector<int> old_fdoc = state.frame_doc[slot];

        std::vector<int> new_seq;
        std::vector<std::vector<int>> new_vals(refs.size());
        std::vector<std::vector<int>> new_pieces;
        std::vector<int> new_fdoc;
        new_seq.reserve(old_seq.size() + defaults.size());
        for (size_t i = 0; i < old_seq.size(); i++) {
            int idx = old_seq[i];
            int entry_doc_idx = i < old_fdoc.size() ? old_fdoc[i] : -1;
            Document *entry_doc = WorldMarkedResolveEntryDoc(doc, entry_doc_idx);
            if (!doc_get_img(entry_doc, idx)) continue;   /* frame deleted from its doc */
            /* Only drop an entry here if it WAS a directly-marked sprite of
               THIS row's own doc and got unmarked. Entries dragged in from
               another row's document were never in this row's marked set,
               so they're never auto-dropped here (the user can still delete
               them explicitly). Entries built from a marked sprite's
               children (Use Subframe composites, Chop at Waterline fine
               pieces) were also never themselves in the marked set, so they
               must not be dropped just because marking some other,
               unrelated sprite changed the set. */
            bool own_doc = entry_doc == doc;
            bool was_marked = own_doc &&
                            std::find(prev_defaults.begin(), prev_defaults.end(), idx)
                            != prev_defaults.end();
            bool still_marked = own_doc &&
                              std::find(defaults.begin(), defaults.end(), idx)
                              != defaults.end();
            if (was_marked && !still_marked)
                continue;                            /* sprite was unmarked */
            new_seq.push_back(idx);
            for (size_t a = 0; a < refs.size(); a++)
                new_vals[a].push_back(old_vals[a][i]);
            new_pieces.push_back(i < old_pieces.size() ? old_pieces[i]
                                                       : std::vector<int>());
            new_fdoc.push_back(entry_doc_idx);
        }
        for (int idx : defaults) {
            /* Only frames newly added to the marked set get appended; frames
               the user deliberately removed from the sequence (still marked)
               stay removed. */
            if (std::find(prev_defaults.begin(), prev_defaults.end(), idx) != prev_defaults.end())
                continue;
            if (std::find(new_seq.begin(), new_seq.end(), idx) != new_seq.end())
                continue;
            new_seq.push_back(idx);
            for (size_t a = 0; a < refs.size(); a++)
                new_vals[a].push_back(refs[a].fresh);
            new_pieces.push_back(std::vector<int>());
            new_fdoc.push_back(-1);
        }

        state.default_frames[slot] = defaults;
        state.sequence_frames[slot] = new_seq;
        for (size_t a = 0; a < refs.size(); a++)
            *refs[a].vec = new_vals[a];
        state.entry_pieces[slot] = new_pieces;
        state.frame_doc[slot] = new_fdoc;
        EnsureWorldMarkedFrameDelays(state, slot, (int)new_seq.size());
    }

    frames = state.sequence_frames[slot];
    WorldMarkedBuildSingleFrameLane(doc, frames, frame_pieces, frame_labels,
                                    &state.entry_pieces[slot], &state.frame_doc[slot]);
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());
}

/* Every IMG an entry draws: a composite entry lists its pieces, a plain one is
   just the sequence frame. Returns false when the entry resolves to nothing. */
static bool WorldMarkedEntryImages(WorldMarkedSequenceState &state,
                                   int slot, int entry,
                                   Document **out_doc,
                                   std::vector<IMG*> *out_imgs)
{
    out_imgs->clear();
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return false;
    if (entry < 0 || entry >= (int)state.sequence_frames[slot].size()) return false;

    int doc_override = entry < (int)state.frame_doc[slot].size()
                     ? state.frame_doc[slot][entry] : -1;
    Document *doc = WorldMarkedResolveEntryDoc(WorldMarkedRowDoc(state, slot), doc_override);
    if (!doc) return false;
    *out_doc = doc;

    const std::vector<int> *pieces = NULL;
    if (entry < (int)state.entry_pieces[slot].size() &&
        !state.entry_pieces[slot][entry].empty())
        pieces = &state.entry_pieces[slot][entry];

    std::vector<int> single;
    if (!pieces) {
        single.push_back(state.sequence_frames[slot][entry]);
        pieces = &single;
    }
    for (size_t i = 0; i < pieces->size(); i++) {
        IMG *img = doc_get_img(doc, (*pieces)[i]);
        if (img) out_imgs->push_back(img);
    }
    return !out_imgs->empty();
}

bool WorldMarkedFindEntryForImage(WorldMarkedSequenceState &state,
                                  int doc_idx, int img_idx,
                                  int *out_slot, int *out_entry)
{
    Document *want = document_get(doc_idx);
    if (!want || img_idx < 0) return false;
    IMG *want_img = doc_get_img(want, img_idx);
    if (!want_img) return false;

    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++) {
        int n = (int)state.sequence_frames[slot].size();
        for (int e = 0; e < n; e++) {
            Document *doc = NULL;
            std::vector<IMG*> imgs;
            if (!WorldMarkedEntryImages(state, slot, e, &doc, &imgs)) continue;
            for (size_t i = 0; i < imgs.size(); i++) {
                if (imgs[i] != want_img) continue;
                if (out_slot) *out_slot = slot;
                if (out_entry) *out_entry = e;
                return true;
            }
        }
    }
    return false;
}

int WorldMarkedBakeEntryOffsets(WorldMarkedSequenceState &state,
                                int slot, int entry, int *out_conflicts)
{
    if (out_conflicts) *out_conflicts = 0;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return 0;

    int first = entry, last = entry;
    if (entry < 0) { first = 0; last = (int)state.sequence_frames[slot].size() - 1; }

    /* One IMG can appear in several entries — duplicating an entry so the same
       sprite sits at two offsets is a supported move. Baking both would apply
       two deltas to one anipoint, so the first wins and the rest are reported
       rather than silently compounded. */
    std::vector<IMG*> baked;
    int changed = 0;
    for (int e = first; e <= last; e++) {
        if (e < 0 || e >= (int)state.local_dx[slot].size()) continue;
        int dx = state.local_dx[slot][e];
        int dy = state.local_dy[slot][e];

        Document *doc = NULL;
        std::vector<IMG*> imgs;
        if (!WorldMarkedEntryImages(state, slot, e, &doc, &imgs)) continue;

        bool seen = false;
        for (size_t i = 0; i < imgs.size(); i++)
            for (size_t b = 0; b < baked.size(); b++)
                if (baked[b] == imgs[i]) seen = true;
        if (seen) {
            if (dx || dy) { if (out_conflicts) (*out_conflicts)++; }
            continue;
        }
        for (size_t i = 0; i < imgs.size(); i++)
            baked.push_back(imgs[i]);

        if (!dx && !dy) continue;
        for (size_t i = 0; i < imgs.size(); i++) {
            IMG *img = imgs[i];
            img->anix = signed_to_img_word((int)(short)img->anix + dx);
            img->aniy = signed_to_img_word((int)(short)img->aniy + dy);
            changed++;
            /* Baking a parent has to bake its chopped children too, or the
               placement you lined up only becomes real for half the sprite.
               They join `baked` so a later entry that names a child directly
               is reported as a conflict instead of compounding the delta. */
            for (IMG *kid = (IMG *)doc->img_p; kid; kid = (IMG *)kid->nxt_p) {
                std::string owner =
                    InferSubframeParentName(img_name_string(kid).c_str());
                if (owner.empty() ||
                    !ascii_iequals(owner, trim_sprite_name(img_name_string(img))))
                    continue;
                baked.push_back(kid);
            }
            changed += shift_subframe_anipoints(doc, img, dx, dy);
        }
        state.local_dx[slot][e] = 0;
        state.local_dy[slot][e] = 0;
        doc->dirty = true;
        if (doc == g_doc) g_img_tex_idx = -2;
    }
    return changed;
}

void WorldMarkedResetSequenceToDefaults(WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.sequence_frames[slot] = state.default_frames[slot];
    WorldMarkedClearSequenceState(state, slot);
    EnsureWorldMarkedFrameDelays(state, slot, (int)state.sequence_frames[slot].size());
    WorldMarkedRestart(state);
}

static bool WorldMarkedLaneCanSplit(const WorldMarkedLane &lane,
                                    const std::vector<WorldMarkedLane> &lanes)
{
    if (lane.dummy_decap) return false;
    int slot = lane.delay_slot;
    bool splittable_source = (slot >= 0 && slot < kWorldMarkedSourceTabs) ||
                             slot == kWorldEmbeddedSeqScrSlot;
    if (!splittable_source) return false;
    /* The tail has to land somewhere: splits always target a free marked row,
       including when the source is the embedded lane. */
    return WorldMarkedFindFreeSplitSlot(lanes) >= 0;
}

bool WorldMarkedSplitLaneAtFrame(WorldMarkedSequenceState &state,
                                 const WorldMarkedLane &lane,
                                 const std::vector<WorldMarkedLane> &lanes,
                                 int frame_idx)
{
    int src_slot = lane.delay_slot;
    /* Marked rows split freely. The embedded SEQSCR lane is allowed too --
       splitting the single unmarked view is the whole point of this path --
       but it needs the detach below first. Everything else (dummy body, ASM
       lane) has no frame list of its own to divide. */
    bool embedded_src = (src_slot == kWorldEmbeddedSeqScrSlot);
    if (lane.dummy_decap ||
        (!embedded_src && (src_slot < 0 || src_slot >= kWorldMarkedSourceTabs)))
        return false;

    if (embedded_src) {
        /* SeqScrSyncLaneToBlob writes this lane back into the IMG's SEQSCR
           record whenever it stops matching what was loaded. A split truncates
           the source frame list, so leaving the lane attached would quietly
           delete the tail entries from the record on the next frame -- the
           user asked to divide a preview, not to destroy half a sequence.
           Detaching turns it into an ordinary preview lane, exactly like the
           marked rows, and the record on disk is left alone. */
        state.embedded_active = false;
    }

    std::vector<int> &src_frames = state.sequence_frames[src_slot];
    if (src_frames.empty())
        src_frames = lane.frames;
    EnsureWorldMarkedFrameDelays(state, src_slot, (int)src_frames.size());

    int n = (int)src_frames.size();
    if (frame_idx <= 0 || frame_idx >= n)
        return false;

    int dst_slot = WorldMarkedFindFreeSplitSlot(lanes);
    if (dst_slot < 0 || dst_slot >= kWorldMarkedSourceTabs ||
        dst_slot == src_slot)
        return false;

    std::vector<int> tail(src_frames.begin() + frame_idx, src_frames.end());
    src_frames.erase(src_frames.begin() + frame_idx, src_frames.end());

    std::vector<WorldSeqArrayRef> src_refs =
        WorldMarkedSeqArrays(state, src_slot);
    std::vector<WorldSeqArrayRef> dst_refs =
        WorldMarkedSeqArrays(state, dst_slot);
    for (size_t i = 0; i < src_refs.size(); i++) {
        std::vector<int> &src = *src_refs[i].vec;
        std::vector<int> &dst = *dst_refs[i].vec;
        dst.assign(src.begin() + frame_idx, src.end());
        src.erase(src.begin() + frame_idx, src.end());
    }
    std::vector<std::vector<int>> &src_pieces = state.entry_pieces[src_slot];
    std::vector<std::vector<int>> &dst_pieces = state.entry_pieces[dst_slot];
    dst_pieces.assign(src_pieces.begin() + frame_idx, src_pieces.end());
    src_pieces.erase(src_pieces.begin() + frame_idx, src_pieces.end());

    std::vector<int> &src_fdoc = state.frame_doc[src_slot];
    std::vector<int> &dst_fdoc = state.frame_doc[dst_slot];
    dst_fdoc.assign(src_fdoc.begin() + frame_idx, src_fdoc.end());
    src_fdoc.erase(src_fdoc.begin() + frame_idx, src_fdoc.end());

    WorldMarkedSetRowDoc(state, dst_slot, lane.doc_idx);
    state.default_frames[dst_slot] = tail;
    state.sequence_frames[dst_slot] = tail;
    state.lane_visible[dst_slot] = state.lane_visible[src_slot];
    state.hold_end[dst_slot] = state.hold_end[src_slot];
    state.pingpong_delay[dst_slot] = state.pingpong_delay[src_slot];
    state.stop_tick[dst_slot] = state.stop_tick[src_slot];
    bool *src_mirror = WorldMarkedMirrorFlag(state, src_slot);
    bool *dst_mirror = WorldMarkedMirrorFlag(state, dst_slot);
    if (src_mirror && dst_mirror)
        *dst_mirror = *src_mirror;

    WorldMarkedSplitLane split = {};
    split.slot = dst_slot;
    split.doc_idx = lane.doc_idx;
    state.split_lanes.push_back(split);
    WorldMarkedPruneSplitLanes(state);

    EnsureWorldMarkedFrameDelays(state, src_slot, (int)src_frames.size());
    EnsureWorldMarkedFrameDelays(state, dst_slot, (int)tail.size());
    state.paused = true;
    WorldMarkedRestart(state);
    return true;
}

/* ---- Merging one project's rows into the scene already open -------------
   Two lanes of a fight live in two .WAX files as often as one, and until now
   the only way to see them together was to rebuild one of them by hand inside
   the other. Load replaces the workspace; these let a second project be laid
   on top of it.

   The row is copied field for field rather than by assigning the whole state,
   because everything outside the per-slot arrays -- the origin, the tick
   clock, the global hold, the ASM lanes -- belongs to the scene that is
   already open and must survive the merge. */

/* The first marked-row slot with nothing in it, or -1 when the scene is full.
   WorldMarkedFindFreeSplitSlot answers the same question from a lane list;
   this one asks the state directly, because a project being merged in has no
   lanes built yet. */
int WorldMarkedFirstFreeSourceSlot(const WorldMarkedSequenceState &state)
{
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (slot == kWorldDummyDecapSlot) continue;
        if (state.sequence_frames[slot].empty() &&
            state.default_frames[slot].empty())
            return slot;
    }
    return -1;
}

/* Copy one row out of `src` into `dst_slot` of `state`. `doc_idx` is the
   destination document index for the row -- the caller resolves it, since only
   it knows how the merged project's documents map onto the open tabs. */
bool WorldMarkedCopySlotFrom(WorldMarkedSequenceState &state, int dst_slot,
                             WorldMarkedSequenceState &src, int src_slot,
                             int doc_idx)
{
    if (dst_slot < 0 || dst_slot >= kWorldMarkedSourceTabs) return false;
    if (src_slot < 0 || src_slot >= kWorldMarkedMaxTabs) return false;
    if (dst_slot == kWorldDummyDecapSlot) return false;
    if (src.sequence_frames[src_slot].empty() &&
        src.default_frames[src_slot].empty())
        return false;

    std::vector<int> frames = src.sequence_frames[src_slot].empty()
                            ? src.default_frames[src_slot]
                            : src.sequence_frames[src_slot];
    if (frames.empty()) return false;

    EnsureWorldMarkedFrameDelays(src, src_slot, (int)frames.size());

    WorldMarkedSetRowDoc(state, dst_slot, doc_idx);
    state.default_frames[dst_slot] = src.default_frames[src_slot];
    state.sequence_frames[dst_slot] = frames;

    std::vector<WorldSeqArrayRef> src_refs = WorldMarkedSeqArrays(src, src_slot);
    std::vector<WorldSeqArrayRef> dst_refs = WorldMarkedSeqArrays(state, dst_slot);
    for (size_t i = 0; i < src_refs.size() && i < dst_refs.size(); i++)
        *dst_refs[i].vec = *src_refs[i].vec;
    state.entry_pieces[dst_slot] = src.entry_pieces[src_slot];
    state.frame_doc[dst_slot] = src.frame_doc[src_slot];

    state.lane_visible[dst_slot] = src.lane_visible[src_slot];
    state.hold_end[dst_slot] = src.hold_end[src_slot];
    state.lane_rigid[dst_slot] = src.lane_rigid[src_slot];
    state.pingpong_delay[dst_slot] = src.pingpong_delay[src_slot];
    state.stop_tick[dst_slot] = src.stop_tick[src_slot];
    state.slot_hold[dst_slot] = src.slot_hold[src_slot];
    state.slot_hold_custom[dst_slot] = src.slot_hold_custom[src_slot];
    bool *src_mirror = WorldMarkedMirrorFlag(src, src_slot);
    bool *dst_mirror = WorldMarkedMirrorFlag(state, dst_slot);
    if (src_mirror && dst_mirror) *dst_mirror = *src_mirror;

    state.auto_step[dst_slot] = src.auto_step[src_slot];
    state.auto_life[dst_slot] = src.auto_life[src_slot];
    state.auto_vx[dst_slot] = src.auto_vx[src_slot];
    state.auto_vy[dst_slot] = src.auto_vy[src_slot];
    state.auto_y[dst_slot] = src.auto_y[src_slot];
    state.chain_count[dst_slot] = src.chain_count[src_slot];
    state.chain_gap[dst_slot] = src.chain_gap[src_slot];
    state.chain_delay[dst_slot] = src.chain_delay[src_slot];
    state.chain_vy[dst_slot] = src.chain_vy[src_slot];
    state.chain_pingpong[dst_slot] = src.chain_pingpong[src_slot];
    state.subframe_swap_tick[dst_slot] = src.subframe_swap_tick[src_slot];
    state.subframe_waterline_y[dst_slot] = src.subframe_waterline_y[src_slot];
    state.subframe_fine_source[dst_slot] = src.subframe_fine_source[src_slot];

    /* Merged rows go to the back of the draw/display order rather than
       fighting the open scene for its ranks. */
    state.lane_order[dst_slot] = kWorldMarkedMaxTabs + dst_slot;

    EnsureWorldMarkedFrameDelays(state, dst_slot,
                                 (int)state.sequence_frames[dst_slot].size());
    WorldMarkedClampAutoChainSettings(state, dst_slot);
    return true;
}

static bool WorldMarkedDuplicateSlot(WorldMarkedSequenceState &state,
                                     const WorldMarkedLane &lane,
                                     const std::vector<WorldMarkedLane> &lanes)
{
    int src_slot = lane.delay_slot;
    if (lane.dummy_decap ||
        src_slot < 0 || src_slot >= kWorldMarkedSourceTabs ||
        !lane.doc)
        return false;

    std::vector<int> &src_frames = state.sequence_frames[src_slot];
    if (src_frames.empty())
        src_frames = lane.frames;
    EnsureWorldMarkedFrameDelays(state, src_slot, (int)src_frames.size());
    if (src_frames.empty())
        return false;

    int dst_slot = WorldMarkedFindFreeSplitSlot(lanes);
    if (dst_slot < 0 || dst_slot >= kWorldMarkedSourceTabs ||
        dst_slot == src_slot)
        return false;

    WorldMarkedSetRowDoc(state, dst_slot, lane.doc_idx);
    state.default_frames[dst_slot] = src_frames;
    state.sequence_frames[dst_slot] = src_frames;

    std::vector<WorldSeqArrayRef> src_refs =
        WorldMarkedSeqArrays(state, src_slot);
    std::vector<WorldSeqArrayRef> dst_refs =
        WorldMarkedSeqArrays(state, dst_slot);
    for (size_t i = 0; i < src_refs.size(); i++)
        *dst_refs[i].vec = *src_refs[i].vec;
    state.entry_pieces[dst_slot] = state.entry_pieces[src_slot];
    state.frame_doc[dst_slot] = state.frame_doc[src_slot];

    state.lane_visible[dst_slot] = state.lane_visible[src_slot];
    state.hold_end[dst_slot] = state.hold_end[src_slot];
    state.pingpong_delay[dst_slot] = state.pingpong_delay[src_slot];
    state.stop_tick[dst_slot] = state.stop_tick[src_slot];
    bool *src_mirror = WorldMarkedMirrorFlag(state, src_slot);
    bool *dst_mirror = WorldMarkedMirrorFlag(state, dst_slot);
    if (src_mirror && dst_mirror)
        *dst_mirror = *src_mirror;

    state.auto_step[dst_slot] = state.auto_step[src_slot];
    state.auto_life[dst_slot] = state.auto_life[src_slot];
    state.auto_vx[dst_slot] = state.auto_vx[src_slot];
    state.auto_vy[dst_slot] = state.auto_vy[src_slot];
    state.auto_y[dst_slot] = state.auto_y[src_slot];
    state.chain_count[dst_slot] = state.chain_count[src_slot];
    state.chain_gap[dst_slot] = state.chain_gap[src_slot];
    state.chain_delay[dst_slot] = state.chain_delay[src_slot];
    state.chain_vy[dst_slot] = state.chain_vy[src_slot];
    state.chain_pingpong[dst_slot] = state.chain_pingpong[src_slot];
    state.subframe_swap_tick[dst_slot] = state.subframe_swap_tick[src_slot];
    state.subframe_waterline_y[dst_slot] = state.subframe_waterline_y[src_slot];
    state.subframe_fine_source[dst_slot] = state.subframe_fine_source[src_slot];

    WorldMarkedSplitLane split = {};
    split.slot = dst_slot;
    split.doc_idx = lane.doc_idx;
    state.split_lanes.push_back(split);
    WorldMarkedPruneSplitLanes(state);

    EnsureWorldMarkedFrameDelays(state, dst_slot,
                                 (int)state.sequence_frames[dst_slot].size());
    WorldMarkedClampAutoChainSettings(state, dst_slot);
    state.paused = true;
    WorldMarkedRestart(state);
    return true;
}

bool WorldMarkedReverseSlot(WorldMarkedSequenceState &state, WorldMarkedLane &lane)
{
    int slot = lane.delay_slot;
    if (lane.dummy_decap || slot < 0 || slot >= kWorldMarkedMaxTabs)
        return false;

    std::vector<int> &frames = state.sequence_frames[slot];
    if (frames.empty())
        frames = lane.frames;
    int n = (int)frames.size();
    if (n <= 0)
        return false;
    EnsureWorldMarkedFrameDelays(state, slot, n);

    /* Total span to mirror tick-driven entries (chain/composite/waterline)
       around: the lane's natural flipbook length, or further if some
       entry's explicit Hide@ already runs past that. */
    int total_ticks = WorldMarkedSequenceTicks(state, slot, n);
    for (int fi = 0; fi < n; fi++) {
        int vu = state.visible_until[slot][fi];
        if (vu > total_ticks) total_ticks = vu;
    }
    if (total_ticks < 1) total_ticks = 1;

    for (int fi = 0; fi < n; fi++) {
        int old_vf = state.visible_from[slot][fi];
        int old_vu = state.visible_until[slot][fi];
        bool timed = old_vf > 0 || old_vu > 0 ||
                    state.motion_dx[slot][fi] != 0 ||
                    state.motion_dy[slot][fi] != 0;
        if (!timed)
            continue;

        /* Tick-driven entries don't reverse by reordering the array (their
           visibility comes from absolute ticks, not array position) — mirror
           their show/hide window around total_ticks, negate their motion,
           and rebase them to start from where they used to end up. */
        int end_tick = old_vu > 0 ? old_vu : total_ticks;
        int end_dx = 0, end_dy = 0;
        WorldMarkedEffectiveLocalDelta(state, slot, n, fi, false, end_tick,
                                      &end_dx, &end_dy);
        bool has_dual = state.dual_on[slot][fi] != 0;
        int end_dual_dx = 0, end_dual_dy = 0;
        if (has_dual)
            WorldMarkedEffectiveLocalDelta(state, slot, n, fi, true, end_tick,
                                           &end_dual_dx, &end_dual_dy);

        int new_vf = old_vu > 0 ? (total_ticks - old_vu) : 0;
        int new_vu = old_vf > 0 ? (total_ticks - old_vf) : 0;
        if (new_vf < 0) new_vf = 0;
        if (new_vu < 0) new_vu = 0;

        state.visible_from[slot][fi] = ClampWorldMarkedVisibleFrom(new_vf);
        state.visible_until[slot][fi] = ClampWorldMarkedVisibleUntil(new_vu);
        state.local_dx[slot][fi] = ClampWorldMarkedAniptDelta(end_dx);
        state.local_dy[slot][fi] = ClampWorldMarkedAniptDelta(end_dy);
        if (has_dual) {
            state.dual_dx[slot][fi] = ClampWorldMarkedAniptDelta(end_dual_dx);
            state.dual_dy[slot][fi] = ClampWorldMarkedAniptDelta(end_dual_dy);
        }
        state.motion_dx[slot][fi] = ClampWorldMarkedMotion(-state.motion_dx[slot][fi]);
        state.motion_dy[slot][fi] = ClampWorldMarkedMotion(-state.motion_dy[slot][fi]);
    }

    /* Plain entries have no absolute anchor of their own — their only
       timing is their position in the array, so reversing the array order
       (every per-entry field travels with it as one unit) is what reverses
       their playback. */
    std::vector<WorldSeqArrayRef> refs = WorldMarkedSeqArrays(state, slot);
    std::reverse(frames.begin(), frames.end());
    for (const WorldSeqArrayRef &ref : refs)
        std::reverse(ref.vec->begin(), ref.vec->end());
    std::reverse(state.entry_pieces[slot].begin(), state.entry_pieces[slot].end());
    std::reverse(state.frame_doc[slot].begin(), state.frame_doc[slot].end());

    state.paused = true;
    WorldMarkedRestart(state);
    return true;
}

/* Rank lookup that tolerates the odd out-of-range slot by sorting it last. */
static int WorldMarkedLaneRank(const WorldMarkedSequenceState &state,
                               const WorldMarkedLane &lane)
{
    int slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return 1 << 20;
    return state.lane_order[slot];
}

void WorldMarkedApplyLaneOrder(WorldMarkedSequenceState &state,
                               std::vector<WorldMarkedLane> &lanes)
{
    int next = 0;
    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++)
        if (state.lane_order[slot] >= next) next = state.lane_order[slot] + 1;

    /* Anything appearing for the first time goes on the end, in build order.
       A slot keeps its rank while it is away, so a row that comes back -- a
       tab reopened, the dummy body re-ticked -- lands where it was left. */
    for (size_t i = 0; i < lanes.size(); i++) {
        int slot = lanes[i].delay_slot;
        if (slot < 0 || slot >= kWorldMarkedMaxTabs) continue;
        if (state.lane_order[slot] < 0) state.lane_order[slot] = next++;
    }

    std::stable_sort(lanes.begin(), lanes.end(),
                     [&state](const WorldMarkedLane &a, const WorldMarkedLane &b) {
                         return WorldMarkedLaneRank(state, a) <
                                WorldMarkedLaneRank(state, b);
                     });
}

bool WorldMarkedMoveLane(WorldMarkedSequenceState &state,
                         const std::vector<WorldMarkedLane> &lanes,
                         int display_index, int dir)
{
    int other = display_index + dir;
    if (display_index < 0 || display_index >= (int)lanes.size()) return false;
    if (other < 0 || other >= (int)lanes.size()) return false;

    int a = lanes[(size_t)display_index].delay_slot;
    int b = lanes[(size_t)other].delay_slot;
    if (a < 0 || a >= kWorldMarkedMaxTabs) return false;
    if (b < 0 || b >= kWorldMarkedMaxTabs) return false;

    int tmp = state.lane_order[a];
    state.lane_order[a] = state.lane_order[b];
    state.lane_order[b] = tmp;
    return true;
}

/* Clear every mark in a document. This is what created a base row, so it is
   what has to go for the row to leave; the sprites themselves are untouched
   and re-marking brings the row straight back. */
static int WorldUnmarkAllFrames(Document *doc)
{
    int n = 0;
    if (!doc) return 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (!(img->flags & 1)) continue;
        img->flags &= ~1u;
        n++;
    }
    if (n > 0) doc->dirty = 1;
    return n;
}

static int WorldMarkedCountMarkedFrames(Document *doc)
{
    int n = 0;
    if (!doc) return 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p)
        if (img->flags & 1) n++;
    return n;
}

std::string WorldMarkedRemoveLaneDescription(const WorldMarkedSequenceState &state,
                                             const WorldMarkedLane &lane)
{
    char buf[256];
    int slot = lane.delay_slot;
    if (lane.dummy_decap)
        return "Turn off the dummy fatality body lane.";
    if (slot == kWorldAsmSlot)
        return "Switch off the ASM player lane. The .ASM file is untouched.";
    if (slot == kWorldAsmOpponentSlot)
        return "Switch off the ASM opponent lane. The .ASM file is untouched.";
    if (slot == kWorldEmbeddedSeqScrSlot)
        return "Close the embedded sequence/script lane. The record in\n"
               "the IMG is left exactly as it is.";
    if (WorldMarkedSlotReservedForSplit(state, slot)) {
        snprintf(buf, sizeof(buf),
                 "Delete this split row and its hand-edited sequence.\n"
                 "There is no undo for it.");
        return buf;
    }

    Document *doc = lane.doc;
    const char *name = (doc && doc->fname_s[0]) ? doc->fname_s : "this IMG";
    int marked = WorldMarkedCountMarkedFrames(doc);
    snprintf(buf, sizeof(buf),
             "Unmark %d frame%s in %s.\n\n"
             "That is what puts this row in World View, so the row goes with\n"
             "them. The sprites are not touched, and re-marking brings the row\n"
             "back -- but this row's timing and offsets are cleared.",
             marked, marked == 1 ? "" : "s", name);
    return buf;
}

bool WorldMarkedRemoveLane(WorldMarkedSequenceState &state,
                           const WorldMarkedLane &lane,
                           std::string *out_msg)
{
    char msg[256];
    int slot = lane.delay_slot;

    if (lane.dummy_decap) {
        state.dummy_decap_body = false;
        WorldMarkedRestart(state);
        snprintf(msg, sizeof(msg), "Removed the dummy body row.");
    } else if (slot == kWorldAsmSlot) {
        g_asm_lane_enabled = false;
        snprintf(msg, sizeof(msg),
                 "Removed the ASM player lane. Re-enable it in ASM Animations.");
    } else if (slot == kWorldAsmOpponentSlot) {
        g_asm_opp_enabled = false;
        snprintf(msg, sizeof(msg),
                 "Removed the ASM opponent lane. Re-enable it in ASM Animations.");
    } else if (slot == kWorldEmbeddedSeqScrSlot) {
        WorldExitEmbeddedSeqScr(state);
        snprintf(msg, sizeof(msg), "Closed the embedded sequence/script lane.");
    } else if (WorldMarkedSlotReservedForSplit(state, slot)) {
        if (!WorldMarkedDeleteSplitSlot(state, slot)) return false;
        snprintf(msg, sizeof(msg), "Deleted the split row.");
    } else {
        Document *doc = lane.doc;
        if (!doc || slot < 0 || slot >= kWorldMarkedMaxTabs) return false;
        const char *name = doc->fname_s[0] ? doc->fname_s : "Untitled";
        int n = WorldUnmarkAllFrames(doc);

        /* Same clear-down a deleted split row gets: leaving the sequence
           override behind would resurrect the old frame list the moment
           anything in that file is marked again. */
        state.sequence_frames[slot].clear();
        state.default_frames[slot].clear();
        state.entry_pieces[slot].clear();
        WorldMarkedClearRowDoc(state, slot);
        state.lane_visible[slot] = true;
        state.hold_end[slot] = false;
        bool *mirror = WorldMarkedMirrorFlag(state, slot);
        if (mirror) *mirror = false;
        WorldMarkedClearSequenceState(state, slot);
        state.paused = true;
        WorldMarkedRestart(state);
        snprintf(msg, sizeof(msg), "Removed row -- unmarked %d frame%s in %s.",
                 n, n == 1 ? "" : "s", name);
    }

    if (out_msg) *out_msg = msg;
    return true;
}

/* ---- Bringing a sideloaded sprite into the record's own file -----------
   A SEQSCR entry names an image by index inside its own IMG, so a row built
   from frames pulled out of other tabs has nothing it can write for them. But
   those files are one library by the time LOAD2 packs the character, and the
   sequence is meant to reference them, so dropping the entries produces a
   sequence that is wrong in a quieter way than a missing sprite.

   The sprite is brought into this document instead: matched by name when it
   is already here (the compiled library has one sprite of that name, so a
   second copy would be waste), copied in when it is not. */
static std::string WorldPalNameString(const PAL *pal)
{
    if (!pal) return std::string();
    size_t n = 0;
    while (n < sizeof(pal->n_s) && pal->n_s[n] != '\0') n++;
    return std::string(pal->n_s, pal->n_s + n);
}

/* The palette index in g_doc that `src_palnum` of `src_doc` should become:
   the same-named palette if this file already has one, otherwise a copy.
   Returns -1 when the source palette cannot be resolved at all. */
static int WorldMarkedImportPaletteInto(Document *src_doc, int src_palnum)
{
    PAL *src = doc_get_pal(src_doc, src_palnum);
    if (!src) return -1;

    std::string name = WorldPalNameString(src);
    if (!name.empty()) {
        int idx = 0;
        for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, idx++)
            if (WorldPalNameString(pal) == name) return idx;
    }

    PAL *dst = AllocPal();          /* appends to g_doc */
    if (!dst) return -1;
    memcpy(dst->n_s, src->n_s, sizeof(dst->n_s));
    dst->flags = src->flags;
    dst->bitspix = src->bitspix;
    dst->numc = src->numc;
    /* Two bytes a colour, 15-bit packed -- not RGB triplets. */
    if (src->data_p && src->numc > 0) {
        size_t bytes = (size_t)src->numc * 2u;
        dst->data_p = malloc(bytes);
        if (!dst->data_p) return -1;
        memcpy(dst->data_p, src->data_p, bytes);
    }
    memcpy(dst->file_name_raw, src->file_name_raw, sizeof(dst->file_name_raw));
    dst->file_colind = src->file_colind;
    dst->file_cmap = src->file_cmap;
    return (int)g_doc->palcnt - 1;
}

/* The image index in g_doc for `src_idx` of `src_doc`. `out_copied` reports
   whether a copy was made rather than an existing sprite reused. */
static int WorldMarkedImportSpriteInto(Document *src_doc, int src_idx,
                                       bool *out_copied)
{
    if (out_copied) *out_copied = false;
    IMG *src = doc_get_img(src_doc, src_idx);
    if (!src) return -1;

    std::string name = img_name_string(src);
    if (!name.empty()) {
        int idx = 0;
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++)
            if (img_name_string(img) == name) return idx;
    }

    int palnum = WorldMarkedImportPaletteInto(src_doc, src->palnum);

    IMG *dst = AllocImg();          /* appends to g_doc */
    if (!dst) return -1;
    if (src->data_p) {
        size_t stride = ((size_t)src->w + 3u) & ~(size_t)3u;
        size_t bytes = stride * (size_t)src->h;
        dst->data_p = malloc(bytes);
        if (!dst->data_p) return -1;
        memcpy(dst->data_p, src->data_p, bytes);
    }
    if (src->pttbl_p) {
        dst->pttbl_p = malloc(40);
        if (!dst->pttbl_p) return -1;
        memcpy(dst->pttbl_p, src->pttbl_p, 40);
    }
    if (src->opaltbl_p) {
        dst->opaltbl_p = malloc(16);
        if (!dst->opaltbl_p) return -1;
        memcpy(dst->opaltbl_p, src->opaltbl_p, 16);
    }

    /* The name travels verbatim, raw 16 bytes included: LOAD2 hashes those
       bytes for sprite allocation, and this sprite has to be the same sprite
       it was in the file it came from -- no DUP suffix. */
    memcpy(dst->n_s, src->n_s, sizeof(dst->n_s));
    memcpy(dst->file_name_raw, src->file_name_raw, sizeof(dst->file_name_raw));
    /* Everything but the mark: inheriting it would add this sprite to the
       marked set and hand the document a World View row nobody asked for. */
    dst->flags = (unsigned short)(src->flags & ~1u);
    dst->anix = src->anix;
    dst->aniy = src->aniy;
    dst->w = src->w;
    dst->h = src->h;
    dst->palnum = (palnum >= 0) ? (unsigned short)palnum : src->palnum;
    dst->anix2 = src->anix2;
    dst->aniy2 = src->aniy2;
    dst->aniz2 = src->aniz2;
    dst->opals = src->opals;
    /* Where it came from, so the Assets tree groups it with its own file. */
    const char *origin = (src_doc && src_doc->fname_s[0]) ? src_doc->fname_s
                                                          : src->src_filename;
    strncpy(dst->src_filename, origin, sizeof(dst->src_filename) - 1);
    dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';

    if (out_copied) *out_copied = true;
    return (int)g_doc->imgcnt - 1;
}

/* 16 chars of uppercase, the shape a SEQSCR name has to be. Derived from the
   row's first sprite with its frame number stripped, so a row of UGSTAB1..6
   arrives as UGSTAB rather than a wall of NEWSEQs. */
static std::string WorldMarkedSequenceNameForLane(const WorldMarkedLane &lane)
{
    std::string base;
    if (!lane.frames.empty()) {
        Document *doc = (!lane.frame_docs.empty() && lane.frame_docs[0])
                      ? lane.frame_docs[0] : lane.doc;
        IMG *img = doc_get_img(doc, lane.frames[0]);
        if (img) base = trim_sprite_name(img_name_string(img));
    }
    std::string stem;
    if (!base.empty() && strip_trailing_sequence_digits(base, &stem) && !stem.empty())
        base = stem;
    if (base.empty()) base = "NEWSEQ";
    if (base.size() > 16) base.resize(16);
    for (size_t i = 0; i < base.size(); i++)
        base[i] = (char)toupper((unsigned char)base[i]);
    return base;
}

bool WorldMarkedPromoteLaneToSequence(WorldMarkedSequenceState &state,
                                      const WorldMarkedLane &lane,
                                      const char *seq_name, int target_doc_idx,
                                      std::string *out_msg)
{
    int slot = lane.delay_slot;
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return false;
    if (lane.frames.empty()) return false;

    /* SeqScrAddRecord and friends all write g_doc's blob, so the destination
       tab has to be the active one while the record is built. It used to be
       required to ALREADY be active -- the row could only ever promote into
       whichever tab happened to be in front, which is why the caller now asks
       where it should go. Switch, write, switch back. */
    if (target_doc_idx < 0) target_doc_idx = document_active_index();
    Document *target = document_get(target_doc_idx);
    if (!target) return false;
    const int restore_doc = document_active_index();
    const bool switched = target_doc_idx != restore_doc;
    if (switched) document_set_active(target_doc_idx);
    struct DocRestore {
        bool on; int idx;
        ~DocRestore() { if (on) document_set_active(idx); }
    } doc_restore{switched, restore_doc};

    EnsureWorldMarkedFrameDelays(state, slot, (int)lane.frames.size());

    std::vector<SeqScrEntryValues> entries;
    entries.reserve(lane.frames.size());
    int imported = 0, reused = 0, unresolved = 0;
    bool undo_pushed = false;
    bool lost_flip = false, lost_z = false, lost_motion = false;
    bool lost_schedule = false, lost_dual = false, lost_pieces = false;

    bool *mirror_flag = WorldMarkedMirrorFlag(state, slot);
    if (mirror_flag && *mirror_flag) lost_flip = true;

    for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
        if (state.frame_mirror[slot][fi]) lost_flip = true;
        if (state.frame_z[slot][fi]) lost_z = true;
        if (state.motion_dx[slot][fi] || state.motion_dy[slot][fi] ||
            state.motion_cap_x[slot][fi] || state.motion_cap_y[slot][fi])
            lost_motion = true;
        if (state.visible_from[slot][fi] || state.visible_until[slot][fi])
            lost_schedule = true;
        if (state.dual_on[slot][fi]) lost_dual = true;
        if (fi < (int)lane.frame_pieces.size() && lane.frame_pieces[fi].size() > 1)
            lost_pieces = true;

        /* An ENTRY names an image index inside this record's own IMG. A frame
           dragged in from another tab has no such index yet, so give it one --
           the sequence is supposed to reference that sprite, and LOAD2 will
           have both files in one library by the time it matters. */
        int fdoc = fi < (int)state.frame_doc[slot].size()
                 ? state.frame_doc[slot][fi] : -1;
        Document *entry_doc = WorldMarkedResolveEntryDoc(lane.doc, fdoc);
        int img_idx = lane.frames[fi];
        if (entry_doc && entry_doc != g_doc) {
            if (!undo_pushed) { doc_undo_push(); undo_pushed = true; }
            bool copied = false;
            int local = WorldMarkedImportSpriteInto(entry_doc, img_idx, &copied);
            if (local < 0) { unresolved++; continue; }
            if (copied) imported++; else reused++;
            img_idx = local;
        } else if (img_idx < 0 || img_idx >= (int)g_doc->imgcnt) {
            unresolved++;
            continue;
        }

        SeqScrEntryValues v;
        v.index = img_idx;
        /* Holds are already 1..120 and offsets already fit a signed word, so
           both land inside what SeqScrReplaceEntries accepts. */
        v.ticks = ClampTimelineHold(state.frame_delays[slot][fi]);
        v.dx = ClampWorldMarkedAniptDelta(state.local_dx[slot][fi]);
        v.dy = ClampWorldMarkedAniptDelta(state.local_dy[slot][fi]);
        entries.push_back(v);
    }

    char msg[384];
    if (entries.empty()) {
        snprintf(msg, sizeof(msg),
                 "Nothing to promote: none of this row's entries resolve to a sprite.");
        if (out_msg) *out_msg = msg;
        return false;
    }

    /* Non-script records are STORED back-to-front: WorldDecodeSeqScrRecord
       reads them with e = num-1-display_e, and SeqScrLaneOwnEntries reverses
       for exactly this reason before writing one back. Promote built its list
       in lane order and handed it straight to SeqScrReplaceEntries, which
       writes raw slot for slot -- so the record came out reversed, and every
       dX/dY arrived attached to the wrong sprite. That is what "the local
       anipoints do not promote correctly" was. */
    std::reverse(entries.begin(), entries.end());

    if (!SeqScrAddRecord(false)) {
        snprintf(msg, sizeof(msg),
                 "Could not add a sequence (anim blob is truncated or out of memory).");
        if (out_msg) *out_msg = msg;
        return false;
    }
    int new_idx = (int)g_doc->seqcnt - 1;
    if (!SeqScrReplaceEntries(new_idx, entries)) {
        snprintf(msg, sizeof(msg),
                 "Added sequence %d but could not write its entries; it is empty.",
                 new_idx);
        if (out_msg) *out_msg = msg;
        return false;
    }
    /* The caller's name wins; the derived one is only the seed the prompt
       was filled with, and a fallback for callers that do not ask. */
    std::string name = (seq_name && *seq_name)
                     ? std::string(seq_name)
                     : WorldMarkedSequenceNameForLane(lane);
    SeqScrSetName(new_idx, name.c_str());

    /* Say what did not come across. Silence here would be the worst outcome:
       the row keeps playing with its flips and Z while the record it just
       produced has neither. */
    std::string lost;
    auto add_lost = [&lost](const char *what) {
        if (!lost.empty()) lost += ", ";
        lost += what;
    };
    if (lost_flip) add_lost("flip X/Y");
    if (lost_z) add_lost("Z");
    if (lost_motion) add_lost("motion");
    if (lost_schedule) add_lost("Show@/Hide@");
    if (lost_dual) add_lost("the dual copy");
    if (lost_pieces) add_lost("extra composite pieces");

    char tail[192];
    tail[0] = 0;
    if (!lost.empty())
        snprintf(tail, sizeof(tail),
                 " A sequence entry holds only sprite/ticks/dX/dY, so %s stayed behind.",
                 lost.c_str());
    char foreign_tail[160];
    foreign_tail[0] = 0;
    {
        char parts[128];
        parts[0] = 0;
        if (imported > 0)
            snprintf(parts, sizeof(parts), " Imported %d sprite%s from other files",
                     imported, imported == 1 ? "" : "s");
        if (reused > 0) {
            char more[64];
            snprintf(more, sizeof(more), "%s%d already here by name",
                     parts[0] ? ", " : " Matched ", reused);
            strncat(parts, more, sizeof(parts) - strlen(parts) - 1);
        }
        if (parts[0])
            snprintf(foreign_tail, sizeof(foreign_tail), "%s.", parts);
        if (unresolved > 0) {
            char bad[80];
            snprintf(bad, sizeof(bad), " %d entr%s could not be resolved at all.",
                     unresolved, unresolved == 1 ? "y" : "ies");
            strncat(foreign_tail, bad, sizeof(foreign_tail) - strlen(foreign_tail) - 1);
        }
    }

    snprintf(msg, sizeof(msg),
             "Promoted to sequence %d '%s' in %s (%d entr%s).%s%s",
             new_idx, name.c_str(),
             target->fname_s[0] ? target->fname_s : "Untitled",
             (int)entries.size(),
             entries.size() == 1 ? "y" : "ies", tail, foreign_tail);
    if (out_msg) *out_msg = msg;
    return true;
}

bool WorldMarkedDeleteSplitSlot(WorldMarkedSequenceState &state, int slot)
{
    if (!WorldMarkedSlotReservedForSplit(state, slot))
        return false;

    state.sequence_frames[slot].clear();
    state.default_frames[slot].clear();
    state.entry_pieces[slot].clear();
    WorldMarkedClearRowDoc(state, slot);
    state.lane_visible[slot] = true;
    state.hold_end[slot] = false;
    bool *mirror = WorldMarkedMirrorFlag(state, slot);
    if (mirror) *mirror = false;
    WorldMarkedClearSequenceState(state, slot);

    for (size_t i = 0; i < state.split_lanes.size(); i++) {
        if (state.split_lanes[i].slot == slot) {
            state.split_lanes.erase(state.split_lanes.begin() + (long)i);
            break;
        }
    }

    state.paused = true;
    WorldMarkedRestart(state);
    return true;
}

void WorldMarkedClearSplitLanes(WorldMarkedSequenceState &state)
{
    if (state.split_lanes.empty())
        return;

    bool split_slots[kWorldMarkedSourceTabs] = {};
    for (const WorldMarkedSplitLane &split : state.split_lanes) {
        if (split.slot >= 0 && split.slot < kWorldMarkedSourceTabs)
            split_slots[split.slot] = true;
    }

    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (split_slots[slot]) {
            state.sequence_frames[slot].clear();
            state.default_frames[slot].clear();
            WorldMarkedClearRowDoc(state, slot);
            state.lane_visible[slot] = true;
            state.hold_end[slot] = false;
            bool *mirror = WorldMarkedMirrorFlag(state, slot);
            if (mirror) *mirror = false;
            WorldMarkedClearSequenceState(state, slot);
        } else if (!state.default_frames[slot].empty()) {
            state.sequence_frames[slot] = state.default_frames[slot];
            WorldMarkedClearSequenceState(state, slot);
            EnsureWorldMarkedFrameDelays(state, slot,
                (int)state.sequence_frames[slot].size());
        }
    }

    state.split_lanes.clear();
    state.paused = true;
    WorldMarkedRestart(state);
}

void WorldMarkedDuplicateSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx)
{
    /* Any entry added or removed slides the indices the ctrl+click selection
       is holding, and a stale index that is still IN RANGE quietly points at
       a different frame -- which would copy the wrong art with no error. Drop
       the selection rather than try to fix it up. */
    WorldFrameSelClear();
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if (frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    int insert_at = frame_idx + 1;
    frames.insert(frames.begin() + insert_at, frames[frame_idx]);
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->insert(ref.vec->begin() + insert_at, (*ref.vec)[frame_idx]);
    std::vector<std::vector<int>> &entry_pieces = state.entry_pieces[slot];
    entry_pieces.insert(entry_pieces.begin() + insert_at, entry_pieces[frame_idx]);
    std::vector<int> &fdoc = state.frame_doc[slot];
    fdoc.insert(fdoc.begin() + insert_at, fdoc[frame_idx]);
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), insert_at);
}

void WorldMarkedMoveSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx, int dir)
{
    WorldFrameSelClear();   /* see WorldMarkedDuplicateSequenceEntry */
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    int n = (int)frames.size();
    int j = frame_idx + dir;
    if (frame_idx < 0 || frame_idx >= n || j < 0 || j >= n) return;
    EnsureWorldMarkedFrameDelays(state, slot, n);

    std::swap(frames[frame_idx], frames[j]);
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        std::swap((*ref.vec)[frame_idx], (*ref.vec)[j]);
    std::swap(state.entry_pieces[slot][frame_idx], state.entry_pieces[slot][j]);
    std::swap(state.frame_doc[slot][frame_idx], state.frame_doc[slot][j]);

    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, n, j);
}

void WorldMarkedDeleteSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx)
{
    WorldFrameSelClear();   /* see WorldMarkedDuplicateSequenceEntry */
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if ((int)frames.size() <= 1 || frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    frames.erase(frames.begin() + frame_idx);
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->erase(ref.vec->begin() + frame_idx);
    std::vector<std::vector<int>> &entry_pieces = state.entry_pieces[slot];
    entry_pieces.erase(entry_pieces.begin() + frame_idx);
    std::vector<int> &fdoc = state.frame_doc[slot];
    fdoc.erase(fdoc.begin() + frame_idx);
    if (frame_idx >= (int)frames.size())
        frame_idx = (int)frames.size() - 1;
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), frame_idx);
}

/* Copy the ctrl+click selection into `dst_slot`, appended in row-then-index
   order. Modelled on WorldMarkedMoveEntryBetweenSlots, minus the erase.

   Every source entry is snapshotted BEFORE anything is appended. Copying into
   a row that is also a source would otherwise read entries through indices its
   own growth had already invalidated -- and copying a row into itself is the
   ordinary way to repeat a run, so that is not an edge case. */
bool WorldMarkedCopySelectionToSlot(WorldMarkedSequenceState &state,
                                    int dst_slot, int *out_copied,
                                    int *out_skipped)
{
    if (out_copied) *out_copied = 0;
    if (out_skipped) *out_skipped = 0;
    if (dst_slot < 0 || dst_slot >= kWorldMarkedMaxTabs) return false;
    if (dst_slot == kWorldDummyDecapSlot) return false;
    if (g_world_frame_sel.empty()) return false;

    std::vector<WorldFrameSel> picks = g_world_frame_sel;
    std::sort(picks.begin(), picks.end(),
              [](const WorldFrameSel &a, const WorldFrameSel &b) {
                  if (a.slot != b.slot) return a.slot < b.slot;
                  return a.frame_idx < b.frame_idx;
              });

    struct Snapshot {
        int frame_val;
        std::vector<int> arrays;
        std::vector<int> pieces;
        int doc_idx;
    };
    std::vector<Snapshot> snaps;
    int skipped = 0;

    bool dst_had_frames = !state.sequence_frames[dst_slot].empty();
    int first_src_slot = -1;

    for (const WorldFrameSel &pick : picks) {
        int src_slot = pick.slot;
        if (src_slot < 0 || src_slot >= kWorldMarkedMaxTabs) { skipped++; continue; }
        std::vector<int> &src_frames = state.sequence_frames[src_slot];
        if (pick.frame_idx < 0 || pick.frame_idx >= (int)src_frames.size()) {
            skipped++;
            continue;
        }
        EnsureWorldMarkedFrameDelays(state, src_slot, (int)src_frames.size());

        int src_doc_idx = (pick.frame_idx < (int)state.frame_doc[src_slot].size())
                        ? state.frame_doc[src_slot][pick.frame_idx] : -1;
        Document *src_doc = WorldMarkedResolveEntryDoc(WorldMarkedRowDoc(state, src_slot),
                                                       src_doc_idx);

        /* Same restriction the move has: a composite entry's extra pieces
           resolve against its row's own bound doc, and per-piece cross-doc
           tracking is not wired up. */
        bool is_composite = pick.frame_idx < (int)state.entry_pieces[src_slot].size() &&
                            state.entry_pieces[src_slot][pick.frame_idx].size() > 1;
        if (src_slot != dst_slot && is_composite && dst_had_frames &&
            src_doc != WorldMarkedRowDoc(state, dst_slot)) {
            skipped++;
            continue;
        }

        Snapshot snap;
        snap.frame_val = src_frames[pick.frame_idx];
        std::vector<WorldSeqArrayRef> src_refs = WorldMarkedSeqArrays(state, src_slot);
        snap.arrays.resize(src_refs.size());
        for (size_t i = 0; i < src_refs.size(); i++)
            snap.arrays[i] = (*src_refs[i].vec)[pick.frame_idx];
        snap.pieces = state.entry_pieces[src_slot][pick.frame_idx];
        /* "-1" means "this row's own doc", which would silently re-point at
           the DESTINATION row once the entry lands there. */
        snap.doc_idx = (src_slot == dst_slot)
                     ? src_doc_idx
                     : (src_doc_idx >= 0 ? src_doc_idx
                                         : WorldMarkedRowDocIndex(state, src_slot));
        if (first_src_slot < 0) first_src_slot = src_slot;
        snaps.push_back(snap);
    }

    if (out_skipped) *out_skipped = skipped;
    if (snaps.empty()) return false;

    /* An empty destination adopts the source's document binding, the same way
       a move into an empty row does. */
    if (!dst_had_frames && first_src_slot >= 0 && first_src_slot != dst_slot)
        state.sequence_doc_uid[dst_slot] = state.sequence_doc_uid[first_src_slot];

    std::vector<int> &dst_frames = state.sequence_frames[dst_slot];
    for (const Snapshot &snap : snaps) {
        EnsureWorldMarkedFrameDelays(state, dst_slot, (int)dst_frames.size());
        int at = (int)dst_frames.size();
        dst_frames.insert(dst_frames.begin() + at, snap.frame_val);
        std::vector<WorldSeqArrayRef> dst_refs = WorldMarkedSeqArrays(state, dst_slot);
        for (size_t i = 0; i < dst_refs.size() && i < snap.arrays.size(); i++)
            dst_refs[i].vec->insert(dst_refs[i].vec->begin() + at, snap.arrays[i]);
        state.entry_pieces[dst_slot].insert(state.entry_pieces[dst_slot].begin() + at,
                                            snap.pieces);
        state.frame_doc[dst_slot].insert(state.frame_doc[dst_slot].begin() + at,
                                         snap.doc_idx);
    }

    EnsureWorldMarkedFrameDelays(state, dst_slot, (int)dst_frames.size());
    if (out_copied) *out_copied = (int)snaps.size();
    state.paused = true;
    state.timer = 0.0f;
    return true;
}

bool WorldMarkedMoveEntryBetweenSlots(WorldMarkedSequenceState &state,
                                      int src_slot, int src_frame_idx,
                                      int dst_slot, int dst_frame_idx)
{
    WorldFrameSelClear();   /* see WorldMarkedDuplicateSequenceEntry */
    if (src_slot < 0 || src_slot >= kWorldMarkedMaxTabs) return false;
    if (dst_slot < 0 || dst_slot >= kWorldMarkedMaxTabs) return false;

    std::vector<int> &src_frames = state.sequence_frames[src_slot];
    if (src_frame_idx < 0 || src_frame_idx >= (int)src_frames.size()) return false;
    EnsureWorldMarkedFrameDelays(state, src_slot, (int)src_frames.size());

    bool same_slot = (src_slot == dst_slot);
    if (same_slot && src_frame_idx == dst_frame_idx)
        return false;
    /* Unlike Delete, a cross-slot move is allowed to drain a row down to
       zero frames — the row stays put (visible, empty) rather than getting
       reseeded, so the user can drag the moved frame back in later. See the
       default_frames-based "was this row ever built" check in
       WorldMarkedSyncSequenceOverride. */

    bool dst_has_frames = !state.sequence_frames[dst_slot].empty();
    int src_entry_doc_idx = (src_frame_idx < (int)state.frame_doc[src_slot].size())
                          ? state.frame_doc[src_slot][src_frame_idx] : -1;
    Document *src_entry_doc = WorldMarkedResolveEntryDoc(WorldMarkedRowDoc(state, src_slot),
                                                         src_entry_doc_idx);

    /* A composite ("Use Subframe") entry's extra pieces still resolve
       against this row's own bound doc — per-piece cross-doc tracking isn't
       wired up — so only let those move within the same document. A plain
       single-image entry carries its own doc with it via frame_doc and can
       move freely between rows built from different files. */
    bool is_composite = src_frame_idx < (int)state.entry_pieces[src_slot].size() &&
                        state.entry_pieces[src_slot][src_frame_idx].size() > 1;
    if (!same_slot && is_composite && dst_has_frames &&
        src_entry_doc != WorldMarkedRowDoc(state, dst_slot))
        return false;

    int frame_val = src_frames[src_frame_idx];
    std::vector<WorldSeqArrayRef> src_refs = WorldMarkedSeqArrays(state, src_slot);
    std::vector<int> saved(src_refs.size());
    for (size_t i = 0; i < src_refs.size(); i++)
        saved[i] = (*src_refs[i].vec)[src_frame_idx];
    std::vector<int> saved_pieces = state.entry_pieces[src_slot][src_frame_idx];

    src_frames.erase(src_frames.begin() + src_frame_idx);
    for (WorldSeqArrayRef &ref : src_refs)
        ref.vec->erase(ref.vec->begin() + src_frame_idx);
    state.entry_pieces[src_slot].erase(state.entry_pieces[src_slot].begin() + src_frame_idx);
    state.frame_doc[src_slot].erase(state.frame_doc[src_slot].begin() + src_frame_idx);

    int insert_at = dst_frame_idx;
    if (same_slot && src_frame_idx < insert_at)
        insert_at--;

    std::vector<int> &dst_frames = state.sequence_frames[dst_slot];
    if (insert_at < 0) insert_at = 0;
    if (insert_at > (int)dst_frames.size()) insert_at = (int)dst_frames.size();

    if (!same_slot && !dst_has_frames) {
        state.sequence_doc_uid[dst_slot] = state.sequence_doc_uid[src_slot];
    }

    /* Within the same row, the original value (whether -1 "this row's own
       doc" or an absolute index) still means the same thing after the move.
       Across rows, "-1" would silently flip to mean the DESTINATION row's
       own doc instead, so resolve a deferred entry to its source row's
       current absolute doc index before handing it to a different row. */
    int insert_doc_idx = same_slot ? src_entry_doc_idx
                        : (src_entry_doc_idx >= 0 ? src_entry_doc_idx
                                                  : WorldMarkedRowDocIndex(state, src_slot));

    EnsureWorldMarkedFrameDelays(state, dst_slot, (int)dst_frames.size());
    dst_frames.insert(dst_frames.begin() + insert_at, frame_val);
    std::vector<WorldSeqArrayRef> dst_refs = WorldMarkedSeqArrays(state, dst_slot);
    for (size_t i = 0; i < dst_refs.size(); i++)
        dst_refs[i].vec->insert(dst_refs[i].vec->begin() + insert_at, saved[i]);
    state.entry_pieces[dst_slot].insert(state.entry_pieces[dst_slot].begin() + insert_at,
                                        saved_pieces);
    state.frame_doc[dst_slot].insert(state.frame_doc[dst_slot].begin() + insert_at,
                                     insert_doc_idx);

    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, dst_slot, (int)dst_frames.size(), insert_at);
    return true;
}

static void rebuild_world_onion_texture(IMG *img, int image_idx)
{
    if (!img || !img->data_p || img->w <= 0 || img->h <= 0 || !g_imgui_renderer)
        return;

    /* Rebuild while visible: the prior frame's pixels or assigned palette can
       change without its image-list index changing. */
    if (s_world_onion_tex) SDL_DestroyTexture(s_world_onion_tex);
    s_world_onion_tex = SDL_CreateTexture(g_imgui_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        img->w, img->h);
    if (!s_world_onion_tex) {
        s_world_onion_tex_w = 0;
        s_world_onion_tex_h = 0;
        s_world_onion_idx = -1;
        return;
    }

    SDL_SetTextureBlendMode(s_world_onion_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(s_world_onion_tex, SDL_ScaleModeNearest);
    s_world_onion_tex_w = img->w;
    s_world_onion_tex_h = img->h;
    s_world_onion_idx = image_idx;

    void *pix;
    int pitch;
    if (SDL_LockTexture(s_world_onion_tex, NULL, &pix, &pitch) != 0)
        return;

    int stride = (img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    Uint32 *dst = (Uint32 *)pix;
    PAL *pal = get_pal(img->palnum);
    const unsigned char *pal_data = pal ? (const unsigned char *)pal->data_p : NULL;
    int pal_count = pal ? (int)pal->numc : 0;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char ci = src[y * stride + x];
            unsigned char r = 0, g = 0, b = 0;
            if (pal_data && ci < pal_count)
                pal_word_to_rgb8(pal_data + ci * 2, &r, &g, &b);
            Uint32 a = (ci == 0) ? 0u : 90u;  /* faint */
            dst[y * (pitch / 4) + x] =
                (a << 24) | ((Uint32)r << 16) |
                ((Uint32)g << 8) | b;
        }
    }
    SDL_UnlockTexture(s_world_onion_tex);
}

/* ImGui raises WantCaptureMouse for its own windows, and the canvas is one of
   them — so testing that flag alone suppresses every canvas gesture instead of
   only the ones a modal should swallow. Interaction is blocked when something
   else is on top of the canvas, which is what !IsWindowHovered() catches. */
static bool CanvasInputBlocked(const ImGuiIO &io)
{
    return io.WantCaptureMouse && !ImGui::IsWindowHovered();
}

bool DrawWorldViewSingleSprite(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io,
                               IMG *img, SDL_Texture *img_texture,
                               int image_idx, int image_count,
                               int world_w, int world_h,
                               int world_origin_x, int world_origin_y,
                               bool onion_enabled, bool mirror_active,
                               bool show_borders, bool show_anipoint)
{
    if (!img || !img_texture || world_w <= 0 || world_h <= 0)
        return false;

    WorldCanvasLayout layout =
        ComputeWorldCanvasLayout(avail, img_pos, world_w, world_h,
                                 world_origin_x, world_origin_y);
    float wscale = layout.scale;
    float ww = layout.width;
    float wh = layout.height;
    ImVec2 wpos = layout.pos;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(wpos, ImVec2(wpos.x + ww, wpos.y + wh),
                      IM_COL32(0, 0, 0, 255));
    WorldDrawReferenceBackground(dl, layout, g_world_state);

    float ox = layout.origin_x;
    float oy = layout.origin_y;
    if (show_anipoint) {
        dl->AddLine(ImVec2(ox - 8, oy), ImVec2(ox + 8, oy),
                    IM_COL32(120, 120, 120, 255));
        dl->AddLine(ImVec2(ox, oy - 8), ImVec2(ox, oy + 8),
                    IM_COL32(120, 120, 120, 255));
    }

    WorldDrawReferenceFigure(dl, layout, g_world_state);

    /* Onion-skin: faintly draw the previous sprite. */
    if (onion_enabled && image_count > 1) {
        int prev_idx = (image_idx <= 0) ? image_count - 1 : image_idx - 1;
        int timeline_pos = TimelineFramePosition(image_idx);
        if (timeline_pos >= 0 && g_timeline_frames.size() > 1)
            prev_idx = g_timeline_frames[(size_t)WrapTimelinePosition(timeline_pos - 1)];
        IMG *prev_img = get_img(prev_idx);
        if (prev_img && prev_img->data_p && prev_img->w > 0 && prev_img->h > 0) {
            rebuild_world_onion_texture(prev_img, prev_idx);
            if (s_world_onion_tex) {
                float pw = prev_img->w * wscale;
                float ph = prev_img->h * wscale;
                int pax = (int)(short)prev_img->anix;
                float pleft = ox - anipoint_effective(pax, (int)prev_img->w,
                                                      mirror_active,
                                                      g_mirror_convention) * wscale;
                ImVec2 ppos(pleft, oy - (int)(short)prev_img->aniy * wscale);
                ImVec2 puv0 = mirror_active ? ImVec2(1, 0) : ImVec2(0, 0);
                ImVec2 puv1 = mirror_active ? ImVec2(0, 1) : ImVec2(1, 1);
                dl->AddImage((ImTextureID)(intptr_t)s_world_onion_tex,
                             ppos, ImVec2(ppos.x + pw, ppos.y + ph),
                             puv0, puv1);
            }
        }
    }

    int ax = (int)(short)img->anix;
    int ay = (int)(short)img->aniy;
    float spw = img->w * wscale;
    float sph = img->h * wscale;
    float sleft = ox - anipoint_effective(ax, (int)img->w, mirror_active,
                                          g_mirror_convention) * wscale;
    ImVec2 spos(sleft, oy - ay * wscale);
    ImVec2 suv0 = mirror_active ? ImVec2(1, 0) : ImVec2(0, 0);
    ImVec2 suv1 = mirror_active ? ImVec2(0, 1) : ImVec2(1, 1);

    dl->AddImage((ImTextureID)(intptr_t)img_texture,
                 spos, ImVec2(spos.x + spw, spos.y + sph), suv0, suv1);

    if (show_anipoint)
        dl->AddCircle(ImVec2(ox, oy), 4.0f,
                      IM_COL32(255, 200, 0, 255), 0, 1.5f);
    if (g_world_marked_state.show_boundary_overlay)
        WorldDrawBoundaryGuides(dl, layout);

    if (!CanvasInputBlocked(io)) {
        /* The whole canvas drags, not just the playfield rect. A sprite whose
           anipoint puts it past the world edge is still drawn out there, and
           it has to stay reachable — otherwise the one frame that most needs
           re-anchoring is the one you cannot grab. It also keeps a drag alive
           when the cursor runs past the edge mid-gesture. */
        bool over_canvas =
            io.MousePos.x >= img_pos.x && io.MousePos.x < img_pos.x + avail.x &&
            io.MousePos.y >= img_pos.y && io.MousePos.y < img_pos.y + avail.y;
        if (over_canvas &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            int dx = (int)(d.x / wscale);
            int dy = (int)(d.y / wscale);
            if (dx != 0 || dy != 0) {
                /* Subframes are not draggable from World View. A piece is
                   authored through its parent, so aim the gesture at the
                   parent: the whole composite moves and the pieces inherit,
                   instead of one slice sliding out of the drawing. */
                IMG *anchor_img = img;
                int anchor_idx = image_idx;
                std::string owner =
                    InferSubframeParentName(img_name_string(img).c_str());
                if (!owner.empty()) {
                    int oidx = WorldFindFrameByName(g_doc, owner);
                    IMG *owner_img = doc_get_img(g_doc, oidx);
                    if (owner_img) { anchor_img = owner_img; anchor_idx = oidx; }
                }

                int next_ax = (int)(short)anchor_img->anix +
                              (mirror_active ? dx : -dx);
                int next_ay = (int)(short)anchor_img->aniy - dy;
                /* World View is a frame-by-frame alignment workspace.  Keep a
                   drag local so stepping frames cannot rearrange anchors that
                   were already placed on name-matched sequence frames. */
                int prev_ax = (int)(short)anchor_img->anix;
                int prev_ay = (int)(short)anchor_img->aniy;
                if (set_primary_anipoint_local(anchor_img, next_ax, next_ay)) {
                    /* "Local" means no sibling frames, not no subframes: a
                       chopped child anchors off its parent, so it has to come
                       along or the composite comes apart. Use the delta that
                       actually landed, after clamping. */
                    shift_subframe_anipoints(g_doc, anchor_img,
                                             (int)(short)anchor_img->anix - prev_ax,
                                             (int)(short)anchor_img->aniy - prev_ay);
                    /* set_primary_anipoint_local refreshes the *selected*
                       thumbnail, which is the piece, not the frame we moved. */
                    if (anchor_idx != image_idx) InvalidateThumb(anchor_idx);
                }
            }
        }
    }

    if (show_borders) {
        ImVec2 sprite_max(spos.x + spw, spos.y + sph);
        bool bad_y_anchor = WorldBadYAnchor(img, ay);
        WorldBoundaryRect sprite_rect =
            WorldBoundaryRectFromScreen(spos, sprite_max, layout);
        WorldBoundaryClass sprite_class =
            WorldBoundaryClassify(sprite_rect, bad_y_anchor);
        ImU32 sprite_border =
            sprite_class == WorldBoundary_Green
                ? IM_COL32(85, 170, 255, 230)
                : WorldBoundaryColor(sprite_class, 255);
        float sprite_thick = sprite_class == WorldBoundary_Red ? 3.0f :
                             sprite_class == WorldBoundary_Yellow ? 2.4f : 1.3f;
        dl->AddRect(spos, sprite_max, sprite_border, 0.0f, 0, sprite_thick);
        if (sprite_class != WorldBoundary_Green) {
            char tag[64];
            snprintf(tag, sizeof(tag), "%s", WorldBoundaryName(sprite_class));
            ImVec2 tag_sz = ImGui::CalcTextSize(tag);
            ImVec2 tag_pos(spos.x, sprite_max.y + 2.0f);
            dl->AddRectFilled(ImVec2(tag_pos.x - 2.0f, tag_pos.y - 1.0f),
                              ImVec2(tag_pos.x + tag_sz.x + 2.0f,
                                     tag_pos.y + tag_sz.y + 1.0f),
                              IM_COL32(0, 0, 0, 190));
            dl->AddText(tag_pos, sprite_border, tag);
        }

        char buf[128];
        snprintf(buf, sizeof(buf),
                 "[%d] %s%s   anix=%d aniy=%d   y=%d..%d   world=%dx%d",
                 image_idx, img->n_s,
                 mirror_active ? " mirror" : "",
                 ax, ay, sprite_rect.top, sprite_rect.bottom,
                 world_w, world_h);
        ImVec2 buf_sz = ImGui::CalcTextSize(buf);
        float bg_w = buf_sz.x + 8.0f;
        if (bg_w > ww) bg_w = ww;
        dl->AddRectFilled(ImVec2(wpos.x, wpos.y),
                          ImVec2(wpos.x + bg_w, wpos.y + 18),
                          IM_COL32(0, 0, 0, 180));
        dl->AddText(ImVec2(wpos.x + 4, wpos.y + 2),
                    IM_COL32(220, 220, 220, 255), buf);
    }

    ImGui::Dummy(ImVec2(avail.x, avail.y));
    return true;
}

void ClearCanvasUiTextures(void)
{
    if (s_world_onion_tex) {
        SDL_DestroyTexture(s_world_onion_tex);
        s_world_onion_tex = NULL;
    }
    s_world_onion_tex_w = 0;
    s_world_onion_tex_h = 0;
    s_world_onion_idx = -1;
}

bool DrawAnipointLinkCanvas(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    AnipointLinkState &state = AnipointLink();
    /* The left toolbar and Ctrl+/- shortcuts use the shared canvas zoom
       queue.  Consume it here too, rather than making Link a special case
       that only responds to its local controls. */
    if (g_zoom_pending_fit) {
        state.zoom = 1.0f;
        g_zoom_pending_fit = false;
    }
    while (g_zoom_pending_steps > 0) {
        state.zoom *= 1.25f;
        g_zoom_pending_steps--;
    }
    while (g_zoom_pending_steps < 0) {
        state.zoom /= 1.25f;
        g_zoom_pending_steps++;
    }
    if (state.zoom < 0.25f) state.zoom = 0.25f;
    if (state.zoom > 16.0f) state.zoom = 16.0f;
    int doc_count = document_tab_count();
    if (doc_count <= 0) return false;

    auto clamp_pick = [&](int *doc_idx, int *img_idx) {
        if (!doc_idx || !img_idx) return;
        if (*doc_idx < 0 || *doc_idx >= doc_count) *doc_idx = document_active_index();
        Document *doc = document_get(*doc_idx);
        if (!doc || doc->imgcnt == 0) { *img_idx = -1; return; }
        if (*img_idx < 0 || *img_idx >= (int)doc->imgcnt) *img_idx = doc->ilselected;
        if (*img_idx < 0 || *img_idx >= (int)doc->imgcnt) *img_idx = 0;
    };
    clamp_pick(&state.reference_doc_idx, &state.reference_img_idx);
    clamp_pick(&state.target_doc_idx, &state.target_img_idx);

    auto draw_picker = [&](const char *title, int *doc_idx, int *img_idx) {
        ImGui::TextUnformatted(title);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(145.0f);
        Document *doc = document_get(*doc_idx);
        const char *doc_name = doc && doc->fname_s[0] ? doc->fname_s : "Untitled";
        if (ImGui::BeginCombo((std::string("##link_doc_") + title).c_str(), doc_name)) {
            for (int i = 0; i < doc_count; i++) {
                Document *candidate = document_get(i);
                const char *name = candidate && candidate->fname_s[0]
                                 ? candidate->fname_s : "Untitled";
                char label[180];
                snprintf(label, sizeof(label), "[%d] %s", i + 1, name);
                if (ImGui::Selectable(label, i == *doc_idx)) {
                    *doc_idx = i;
                    *img_idx = candidate ? candidate->ilselected : -1;
                    clamp_pick(doc_idx, img_idx);
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        doc = document_get(*doc_idx);
        IMG *img = doc_get_img(doc, *img_idx);
        ImGui::SetNextItemWidth(180.0f);
        const char *img_name = img ? img->n_s : "(none)";
        if (ImGui::BeginCombo((std::string("##link_img_") + title).c_str(), img_name)) {
            if (doc) {
                for (int i = 0; i < (int)doc->imgcnt; i++) {
                    IMG *candidate = doc_get_img(doc, i);
                    if (!candidate) continue;
                    char label[128];
                    snprintf(label, sizeof(label), "%d  %s", i, candidate->n_s);
                    if (ImGui::Selectable(label, i == *img_idx)) *img_idx = i;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton((std::string("Use Active##link_") + title).c_str())) {
            *doc_idx = document_active_index();
            *img_idx = g_doc ? g_doc->ilselected : -1;
            clamp_pick(doc_idx, img_idx);
        }
    };

    draw_picker("Reference", &state.reference_doc_idx, &state.reference_img_idx);
    draw_picker("Target", &state.target_doc_idx, &state.target_img_idx);
    ImGui::TextDisabled("Drag from a feature on Reference to its matching feature on Target.");
    ImGui::SameLine();
    if (ImGui::Button("Zoom -##link_zoom_out")) state.zoom /= 1.25f;
    ImGui::SameLine();
    if (ImGui::Button("Zoom +##link_zoom_in")) state.zoom *= 1.25f;
    ImGui::SameLine();
    if (ImGui::Button("Fit##link_zoom_fit")) state.zoom = 1.0f;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(115.0f);
    ImGui::SliderFloat("Zoom##link_zoom", &state.zoom, 1.0f, 16.0f,
                       "%.1fx", ImGuiSliderFlags_Logarithmic);
    if (state.zoom < 0.25f) state.zoom = 0.25f;
    if (state.zoom > 16.0f) state.zoom = 16.0f;
    ImGui::SameLine();
    ImGui::TextDisabled("Stage X/Y");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(52.0f);
    ImGui::InputInt("##link_stage_x", &state.target_offset_x, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(52.0f);
    ImGui::InputInt("##link_stage_y", &state.target_offset_y, 0, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(68.0f);
    ImGui::SliderInt("Ghost##link_ref_alpha", &state.reference_alpha, 25, 255, "%d");
    if (state.target_offset_x < -4096) state.target_offset_x = -4096;
    if (state.target_offset_x >  4096) state.target_offset_x =  4096;
    if (state.target_offset_y < -4096) state.target_offset_y = -4096;
    if (state.target_offset_y >  4096) state.target_offset_y =  4096;

    /* Link is often entered from an old embedded SEQSCR table.  Surface that
       table's current rendered entry here, rather than forcing the user to
       manually hunt down the same sprite in a document picker. */
    if (WorldEmbeddedSeqScrActive(g_world_marked_state)) {
        const int seq_slot = kWorldEmbeddedSeqScrSlot;
        const std::vector<int> &frames =
            g_world_marked_state.sequence_frames[seq_slot];
        int fi = g_world_marked_state.frame;
        if (fi < 0) fi = 0;
        if (fi >= (int)frames.size()) fi = (int)frames.size() - 1;
        if (fi >= 0 && fi < (int)frames.size()) {
            int seq_doc_idx = g_world_marked_state.embedded_doc_idx;
            Document *seq_doc = document_get(seq_doc_idx);
            IMG *seq_img = doc_get_img(seq_doc, frames[(size_t)fi]);
            int dx = fi < (int)g_world_marked_state.local_dx[seq_slot].size()
                   ? g_world_marked_state.local_dx[seq_slot][(size_t)fi] : 0;
            int dy = fi < (int)g_world_marked_state.local_dy[seq_slot].size()
                   ? g_world_marked_state.local_dy[seq_slot][(size_t)fi] : 0;
            ImGui::TextDisabled("SEQSCR %s  entry %d/%d  %s  dAX=%d dAY=%d",
                                g_world_marked_state.embedded_name.c_str(),
                                fi + 1, (int)frames.size(),
                                seq_img ? seq_img->n_s : "(missing)", dx, dy);
            ImGui::SameLine();
            if (ImGui::SmallButton("Use SEQSCR Frame as Reference")) {
                state.reference_doc_idx = seq_doc_idx;
                state.reference_img_idx = frames[(size_t)fi];
            }
        }
    }

    Document *reference_doc = document_get(state.reference_doc_idx);
    Document *target_doc = document_get(state.target_doc_idx);
    IMG *reference = doc_get_img(reference_doc, state.reference_img_idx);
    IMG *target = doc_get_img(target_doc, state.target_img_idx);
    if (!reference || !target || !reference->data_p || !target->data_p) return false;

    ImVec2 stage_pos = ImGui::GetCursorScreenPos();
    float top = ImGui::GetCursorPosY();
    float stage_h = avail.y - top;
    if (stage_h < 80.0f) stage_h = 80.0f;
    float stage_w = avail.x;
    float scale_x = stage_w / 512.0f;
    float scale_y = stage_h / 320.0f;
    float scale = floorf(scale_x < scale_y ? scale_x : scale_y) * state.zoom;
    if (scale < 1.0f) scale = 1.0f;
    ImVec2 stage_size(stage_w, stage_h);
    ImVec2 anchor(stage_pos.x + stage_w * 0.5f, stage_pos.y + stage_h * 0.5f);
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(stage_pos, ImVec2(stage_pos.x + stage_w, stage_pos.y + stage_h),
                      IM_COL32(0, 0, 0, 255));
    dl->AddLine(ImVec2(anchor.x - 10, anchor.y), ImVec2(anchor.x + 10, anchor.y),
                IM_COL32(255, 210, 70, 255), 2.0f);
    dl->AddLine(ImVec2(anchor.x, anchor.y - 10), ImVec2(anchor.x, anchor.y + 10),
                IM_COL32(255, 210, 70, 255), 2.0f);

    auto sprite_pos = [&](IMG *img) {
        return ImVec2(anchor.x - (float)(short)img->anix * scale,
                      anchor.y - (float)(short)img->aniy * scale);
    };
    ImVec2 ref_pos = sprite_pos(reference);
    ImVec2 target_pos = sprite_pos(target);
    target_pos.x += (float)state.target_offset_x * scale;
    target_pos.y += (float)state.target_offset_y * scale;
    SDL_Texture *ref_tex = BuildWorldSpriteTexture(reference_doc, reference, 255);
    SDL_Texture *target_tex = BuildWorldSpriteTexture(target_doc, target, 255);
    if (ref_tex)
        dl->AddImage((ImTextureID)(intptr_t)ref_tex, ref_pos,
                     ImVec2(ref_pos.x + reference->w * scale, ref_pos.y + reference->h * scale),
                     ImVec2(0, 0), ImVec2(1, 1),
                     IM_COL32(255, 255, 255, state.reference_alpha));
    if (target_tex)
        dl->AddImage((ImTextureID)(intptr_t)target_tex, target_pos,
                     ImVec2(target_pos.x + target->w * scale, target_pos.y + target->h * scale));
    dl->AddRect(ref_pos, ImVec2(ref_pos.x + reference->w * scale, ref_pos.y + reference->h * scale),
                IM_COL32(90, 180, 255, 255), 0.0f, 0, 2.0f);
    dl->AddRect(target_pos, ImVec2(target_pos.x + target->w * scale, target_pos.y + target->h * scale),
                IM_COL32(90, 255, 150, 255), 0.0f, 0, 2.0f);
    dl->AddText(ref_pos, IM_COL32(120, 200, 255, 255), "Reference");
    dl->AddText(target_pos, IM_COL32(120, 255, 170, 255), "Target");

    bool over_stage = ImGui::IsMouseHoveringRect(stage_pos,
                                                  ImVec2(stage_pos.x + stage_w, stage_pos.y + stage_h));
    if (over_stage && io.MouseWheel != 0.0f) {
        state.zoom *= powf(1.20f, io.MouseWheel);
        if (state.zoom < 0.25f) state.zoom = 0.25f;
        if (state.zoom > 16.0f) state.zoom = 16.0f;
    }
    auto inside = [](ImVec2 p, ImVec2 origin, IMG *img, float s) {
        return p.x >= origin.x && p.y >= origin.y &&
               p.x < origin.x + img->w * s && p.y < origin.y + img->h * s;
    };
    if (!CanvasInputBlocked(io) && over_stage && !state.dragging &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        inside(io.MousePos, ref_pos, reference, scale)) {
        state.dragging = true;
        state.drag_start = io.MousePos;
    }
    if (state.dragging) {
        dl->AddLine(state.drag_start, io.MousePos, IM_COL32(90, 235, 255, 255), 2.0f);
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (inside(io.MousePos, target_pos, target, scale)) {
                int dx = (int)lroundf((io.MousePos.x - state.drag_start.x) / scale);
                int dy = (int)lroundf((io.MousePos.y - state.drag_start.y) / scale);
                /* The target may belong to another open document.  Do not use
                   the active-document sequence helper here: its propagation
                   and undo record are intentionally scoped to g_doc. */
                target->anix = signed_to_img_word((int)(short)target->anix + dx);
                target->aniy = signed_to_img_word((int)(short)target->aniy + dy);
                target_doc->dirty = true;
                if (target_doc == g_doc) {
                    InvalidateThumb(state.target_img_idx);
                    g_img_tex_idx = -2;
                }
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Linked target anipoint by %d, %d px.", dx, dy);
                g_restore_msg_timer = 3.0f;
            }
            state.dragging = false;
        }
    }
    ImGui::Dummy(stage_size);
    return true;
}

/* Clear the marquee's region to transparent across every marked sprite.
   Sprites differ in size, so the rect is applied in each one's own pixel
   space and clipped to it — a region that falls entirely outside a smaller
   frame simply leaves that frame alone. Mask selections (lasso/wand) apply
   their mask, not the bounding box. One undo step covers the whole batch. */
static int ClearSelectionRegionInMarkedFrames(void)
{
    int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
    int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);

    int touched = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!(img->flags & 1) || !img->data_p || img->w <= 0 || img->h <= 0)
            continue;
        unsigned short stride = (unsigned short)((img->w + 3) & ~3);
        unsigned char *data = (unsigned char *)img->data_p;
        bool changed = false;
        for (int y = y1; y <= y2; y++) {
            if (y < 0 || y >= (int)img->h) continue;
            for (int x = x1; x <= x2; x++) {
                if (x < 0 || x >= (int)img->w) continue;
                if (g_grid_sel.is_mask) {
                    int mx = x - x1, my = y - y1;
                    if (mx < 0 || my < 0 || mx >= g_grid_sel.mask_w) continue;
                    size_t mi = (size_t)my * (size_t)g_grid_sel.mask_w + (size_t)mx;
                    if (mi >= g_grid_sel.pixel_mask.size() || !g_grid_sel.pixel_mask[mi])
                        continue;
                }
                if (data[y * stride + x] != 0) {
                    data[y * stride + x] = 0;
                    changed = true;
                }
            }
        }
        if (changed) {
            InvalidateThumb(idx);
            touched++;
        }
    }
    return touched;
}

/* Right-click inside an active marquee: batch operations that apply the same
   region to every marked frame. Anchored to the marquee rect so the menu only
   appears when the cursor is actually inside it. */
static void DrawSelectionBatchContextMenu(ImVec2 img_pos, float sx, float sy)
{
    int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
    int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
    if (x1 > x2) std::swap(x1, x2);
    if (y1 > y2) std::swap(y1, y2);
    ImVec2 rmin(img_pos.x + x1 * sx, img_pos.y + y1 * sy);
    ImVec2 rmax(img_pos.x + (x2 + 1) * sx, img_pos.y + (y2 + 1) * sy);

    if (!ImGui::IsPopupOpen("##sel_batch_ctx")) {
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) &&
            ImGui::IsWindowHovered() &&
            ImGui::IsMouseHoveringRect(rmin, rmax))
            ImGui::OpenPopup("##sel_batch_ctx");
    }
    if (!ImGui::BeginPopup("##sel_batch_ctx")) return;

    int marked = CountMarkedImages();
    ImGui::TextDisabled("Selection %dx%d at %d,%d",
                        x2 - x1 + 1, y2 - y1 + 1, x1, y1);
    ImGui::Separator();
    ImGui::BeginDisabled(marked <= 0);
    char label[96];
    snprintf(label, sizeof(label), "Clear Region in All Marked Frames (%d)", marked);
    if (ImGui::MenuItem(label)) {
        doc_undo_push();
        int touched = ClearSelectionRegionInMarkedFrames();
        g_img_tex_idx = -2;
        mark_dirty();
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Cleared the selected region in %d of %d marked frame%s.",
                 touched, marked, marked == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    ImGui::EndDisabled();
    if (marked <= 0) {
        ImGui::TextDisabled("Mark some sprites first (M / double-click a row).");
    } else if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Set every pixel in this region to transparent index 0\n"
                          "across all %d marked sprites. The rect is applied in each\n"
                          "sprite's own pixel space and clipped to its bounds.", marked);
    }
    ImGui::EndPopup();
}

void DrawCanvasWindow(float canvas_x, float canvas_y, float canvas_w, float canvas_h)
{
    ImGui::SetNextWindowPos(ImVec2(canvas_x, canvas_y));
    ImGui::SetNextWindowSize(ImVec2(canvas_w, canvas_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x06, 0x06, 0x06, 0xFF));
    ImGui::Begin("##canvas", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    {
        ImGuiIO &io = ImGui::GetIO();
        /* Main-view modes deliberately live above the canvas rather than in
           the sidebar: Image is the normal pixel editor, World is the
           animation staging view, Anim is the SEQSCR sequence/script
           workspace, Link is the focused two-sprite anchor matcher, and React
           lists an opponent's reactions with the anipoints of each frame. */
        auto current_canvas_mode = []() {
            return AnipointLink().enabled ? 3
                 : g_reactions_workspace ? 4
                 : g_seqscr_workspace ? 2
                 : g_world_state.enabled ? 1 : 0;
        };
        int requested_canvas_mode = current_canvas_mode();
        static int last_canvas_mode = -1;
        bool sync_canvas_tab = requested_canvas_mode != last_canvas_mode;
        /* View-mode tabs are tinted away from the document tabs above them:
           two tab bars stacked in the same corner otherwise read as one strip,
           and picking a view looks like picking a file. */
        ImGui::PushStyleColor(ImGuiCol_Tab,         ImVec4(0.16f, 0.13f, 0.20f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_TabHovered,  ImVec4(0.42f, 0.30f, 0.58f, 1.00f));
        ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.34f, 0.24f, 0.48f, 1.00f));
        if (ImGui::BeginTabBar("##canvas_mode_tabs",
                               ImGuiTabBarFlags_FittingPolicyResizeDown)) {
            if (ImGui::BeginTabItem("Image", NULL,
                                    sync_canvas_tab && requested_canvas_mode == 0
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                g_world_state.enabled = false;
                g_seqscr_workspace = false;
                AnipointLink().enabled = false;
                g_reactions_workspace = false;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World", NULL,
                                    sync_canvas_tab && requested_canvas_mode == 1
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                g_world_state.enabled = true;
                g_seqscr_workspace = false;
                AnipointLink().enabled = false;
                g_reactions_workspace = false;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Anim", NULL,
                                    sync_canvas_tab && requested_canvas_mode == 2
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                g_seqscr_workspace = true;
                g_world_state.enabled = false;
                AnipointLink().enabled = false;
                g_reactions_workspace = false;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Build and preview this IMG's embedded sequences and scripts.");
            if (ImGui::BeginTabItem("Link", NULL,
                                    sync_canvas_tab && requested_canvas_mode == 3
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                AnipointLink().enabled = true;
                g_world_state.enabled = false;
                g_seqscr_workspace = false;
                g_reactions_workspace = false;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("React", NULL,
                                    sync_canvas_tab && requested_canvas_mode == 4
                                        ? ImGuiTabItemFlags_SetSelected : 0)) {
                g_reactions_workspace = true;
                g_world_state.enabled = false;
                g_seqscr_workspace = false;
                AnipointLink().enabled = false;
                ImGui::EndTabItem();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Every reaction in a character ASM — the animations that\n"
                                  "happen TO that fighter — with the anipoints of each\n"
                                  "frame in the reaction's sequence.");
            /* Backdrop lives here rather than in a menu: it is a per-look
               decision you make while staring at the sprite, and it only
               applies to the Image canvas. */
            if (requested_canvas_mode == 0) {
                char bg_label[32];
                snprintf(bg_label, sizeof(bg_label), "BG: %s",
                         CanvasBackdropName(g_canvas_backdrop));
                if (ImGui::TabItemButton(bg_label, ImGuiTabItemFlags_Trailing |
                                                   ImGuiTabItemFlags_NoTooltip))
                    g_canvas_backdrop = (g_canvas_backdrop + 1) % CanvasBackdrop_Count;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Backdrop behind transparent pixels.\n"
                                      "Click to cycle: Checker, Pink, Green, Blue.\n"
                                      "A flat key makes stray fringe pixels obvious.");
            }
            /* World View's display toggles, trailing on the right, and only
               while World View owns the canvas — the same rule the backdrop
               button follows. They used to sit on the document tab strip,
               where they read as files and outlived the view that used them.
               The label never changes width: state is carried by the selected
               tint and spelled out in the tooltip. A label that grew when you
               switched it on re-laid out the whole trailing group, so every
               button jumped out from under the cursor on click. */
            if (requested_canvas_mode == 1) {
                auto world_toggle = [](const char *label, bool *value,
                                       const char *what) {
                    bool was_on = *value;
                    if (was_on) {
                        ImGui::PushStyleColor(ImGuiCol_Tab,
                            ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
                        ImGui::PushStyleColor(ImGuiCol_TabHovered,
                            ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
                    }
                    bool clicked = ImGui::TabItemButton(label,
                        ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip);
                    if (was_on) ImGui::PopStyleColor(2);
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("%s\nCurrently %s.", what,
                                          was_on ? "on" : "off");
                    if (clicked) *value = !was_on;
                    return clicked;
                };
                /* Submission order is left-to-right within the trailing group
                   (TabItemComparerBySection ties break on IndexDuringLayout),
                   so these read in the order they are written. */
                world_toggle("Onion", &g_world_state.onion,
                             "Ghost the previous frame behind this one.");
                world_toggle("Borders", &g_world_state.show_borders,
                             "Outline each sprite's bounds.");
                world_toggle("Anipt", &g_world_state.show_anipoint,
                             "Draw the shared anchor crosshair.");
                if (world_toggle("Marked", &g_world_marked_state.marked_play,
                                 "Play every marked row as its own lane instead\n"
                                 "of showing the selected sprite alone."))
                    WorldMarkedRestart(g_world_marked_state);
                world_toggle("Mirror 1", &g_world_marked_state.mirror_active,
                             "Flip the first marked lane horizontally.");
                world_toggle("Mirror 2", &g_world_marked_state.mirror_other,
                             "Flip the second marked lane horizontally.");
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleColor(3);
        /* Anim and Link are both driven from the Animation sidebar, so
           entering either view brings that panel forward: the two tab strips
           are two halves of one mode and should never disagree about what you
           are working on. Keyed off the mode transition rather than the tab
           click because ImGui queues tab selection — the frame a tab is
           clicked is not the frame its body runs, so IsItemActivated() inside
           the tab body never fires. The sidebar's matching sync (panel ->
           canvas) is a transition too, so the pair settles instead of
           ping-ponging. */
        int new_canvas_mode = current_canvas_mode();
        if (new_canvas_mode != last_canvas_mode &&
            (new_canvas_mode == 2 || new_canvas_mode == 3))
            g_request_animation_sidebar = true;
        last_canvas_mode = new_canvas_mode;
        ImVec2 avail   = ImGui::GetContentRegionAvail();
        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_origin = img_pos;
        ImVec2 img_sz(0, 0);
        float sx = 1.0f, sy = 1.0f;
        bool timeline_composite_preview_active = false;
        bool rotate_buttons_visible = false;
        bool rotate_button_hovered = false;
        int rotate_button_hover_idx = -1;
        ImVec2 rotate_button_min[2] = {};
        ImVec2 rotate_button_max[2] = {};

        /* ---- World View mode (DOS-style anipoint alignment workspace) ----
         * Renders the sprite inside a fixed black canvas, sprite anchored at
         * (world origin - sprite.anipoint). Left-drag adjusts anix/aniy.
         * Up/Down (handled in the global shortcut block) flicks frames.
         * When this branch runs, the rest of the canvas pipeline (pixel
         * paint, marquee, anim-point handles, hitboxes, DMA overlay,
         * grid-selection) is skipped. */
        if (AnipointLink().enabled) {
            DrawAnipointLinkCanvas(avail, img_pos, io);
        }
        else if (g_reactions_workspace) {
            DrawReactionWorkspace(avail, img_pos, io);
        }
        else if (g_seqscr_workspace) {
            DrawSeqScrWorkspace(avail, img_pos, io);
        }
        else if (g_world_state.enabled) {
            bool drew_dual_marked = DrawWorldMarkedTabs(avail, img_pos, io);
            if (!drew_dual_marked && g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
                IMG *cimg = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
                /* Single-sprite World View now lives in ui_canvas.{h,cpp};
                   the larger marked-tab World View path remains above. */
                DrawWorldViewSingleSprite(avail, img_pos, io,
                                          cimg, g_img_texture,
                                          g_doc->ilselected, (int)g_doc->imgcnt,
                                          g_world_state.w, g_world_state.h,
                                          g_world_state.origin_x, g_world_state.origin_y,
                                          g_world_state.onion, g_world_marked_state.mirror_active,
                                          g_world_state.show_borders,
                                          g_world_state.show_anipoint);
            }
        }
        else if ((timeline_composite_preview_active = DrawTimelineCompositePreview(avail, img_pos))) {
            /* Composite preview is read-only: the canvas is showing two
               timeline frames in shared anipoint space, not one editable IMG. */
        }
        else if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            if (g_zoom_reset) ResetZoomForSelection(avail);

            auto apply_canvas_zoom_step = [&](int dir, ImVec2 anchor) {
                ImVec2 old_pos, old_size;
                float old_scale = 1.0f;
                ZoomImageRectForAvailable(avail, canvas_origin,
                                          &old_pos, &old_size, &old_scale);
                float new_scale = ZoomNextLevel(old_scale, dir);
                if (fabsf(new_scale - old_scale) < 0.001f) return;
                ApplyZoomScale(old_scale, new_scale, anchor, old_pos, old_size, avail);
            };

            if (g_zoom_pending_fit) {
                ResetZoomToFit();
                g_zoom_pending_fit = false;
            }

            ImVec2 canvas_center(canvas_origin.x + avail.x * 0.5f,
                                 canvas_origin.y + avail.y * 0.5f);

            /* ---- Mouse wheel: scroll normally, Ctrl+wheel zooms from center ---- */
            if (ImGui::IsWindowHovered()) {
                if (io.KeyCtrl) {
                    g_zoom_wheel_accum += io.MouseWheel;
                    while (g_zoom_wheel_accum >= 1.0f) {
                        apply_canvas_zoom_step(1, canvas_center);
                        g_zoom_wheel_accum -= 1.0f;
                    }
                    while (g_zoom_wheel_accum <= -1.0f) {
                        apply_canvas_zoom_step(-1, canvas_center);
                        g_zoom_wheel_accum += 1.0f;
                    }
                } else {
                    if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) {
                        const float wheel_pan_step = 80.0f;
                        float dx = io.MouseWheelH * wheel_pan_step;
                        float dy = io.MouseWheel * wheel_pan_step;
                        if (io.KeyShift && io.MouseWheel != 0.0f && io.MouseWheelH == 0.0f) {
                            dx = io.MouseWheel * wheel_pan_step;
                            dy = 0.0f;
                        }
                        ZoomPanBy(avail, dx, dy);
                    }
                    g_zoom_wheel_accum = 0.0f;
                }
            }

            while (g_zoom_pending_steps > 0) {
                apply_canvas_zoom_step(1, canvas_center);
                g_zoom_pending_steps--;
            }
            while (g_zoom_pending_steps < 0) {
                apply_canvas_zoom_step(-1, canvas_center);
                g_zoom_pending_steps++;
            }
            ZoomClampPanForAvailable(avail);

            float scale = 1.0f;
            ZoomImageRectForAvailable(avail, canvas_origin, &img_pos, &img_sz, &scale);
            ImGui::SetCursorScreenPos(img_pos);

            float tw = img_sz.x;
            float th = img_sz.y;
            img_sz  = ImVec2(tw, th);
            sx = tw / (float)g_img_tex_w;
            sy = th / (float)g_img_tex_h;
            rotate_buttons_visible = true;
            CanvasRotateButtonRects(img_pos, img_sz, canvas_origin, avail,
                                    rotate_button_min, rotate_button_max);

            ImDrawList *dl = ImGui::GetWindowDrawList();
            if (g_canvas_backdrop == CanvasBackdrop_Checker) {
                DrawCanvasCheckerboard(dl, img_pos, img_sz, scale);
            } else {
                dl->AddRectFilled(img_pos,
                                  ImVec2(img_pos.x + img_sz.x, img_pos.y + img_sz.y),
                                  CanvasBackdropColor(g_canvas_backdrop));
            }
            /* Timeline onion-skin: draw prev/next frames of the current
               timeline order behind the live sprite, anipoint-aligned and
               faint, so the user can scrub or play and see motion arcs. */
            if (g_timeline_onion && !g_timeline_frames.empty()
                && g_timeline_play_idx >= 0
                && g_timeline_play_idx < (int)g_timeline_frames.size()
                && g_doc->ilselected >= 0)
            {
                IMG *cur_img = get_img(g_doc->ilselected);
                if (cur_img) {
                    int cur_ax = (int)(short)cur_img->anix;
                    int cur_ay = (int)(short)cur_img->aniy;
                    int neighbors[2] = {
                        g_timeline_play_idx == 0
                            ? (int)g_timeline_frames.size() - 1
                            : g_timeline_play_idx - 1,
                        (g_timeline_play_idx + 1) % (int)g_timeline_frames.size()
                    };
                    ImU32 tints[2] = {
                        IM_COL32(120, 180, 255, 70), /* prev: cool */
                        IM_COL32(255, 160, 120, 70)  /* next: warm */
                    };
                    for (int side = 0; side < 2; side++) {
                        if (neighbors[side] == g_timeline_play_idx) continue;
                        int img_idx = g_timeline_frames[neighbors[side]];
                        if (img_idx == g_doc->ilselected) continue;
                        IMG *nimg = get_img(img_idx);
                        if (!nimg) continue;
                        TimelineThumb *t = EnsureThumb(img_idx);
                        if (!t || !t->tex) continue;
                        /* Place the neighbor so that its anipoint coincides
                           with the current sprite's anipoint on screen. */
                        int n_ax = (int)(short)nimg->anix;
                        int n_ay = (int)(short)nimg->aniy;
                        float scale_x = sx * ((float)nimg->w / (float)t->w);
                        float scale_y = sy * ((float)nimg->h / (float)t->h);
                        float nw_screen = t->w * scale_x;
                        float nh_screen = t->h * scale_y;
                        ImVec2 npos(img_pos.x + (cur_ax - n_ax) * sx,
                                    img_pos.y + (cur_ay - n_ay) * sy);
                        dl->AddImage((ImTextureID)(intptr_t)t->tex,
                                     npos,
                                     ImVec2(npos.x + nw_screen, npos.y + nh_screen),
                                     ImVec2(0,0), ImVec2(1,1),
                                     tints[side]);
                    }
                }
            }

            AutoChopPreview auto_chop_preview;
            bool show_auto_chop_preview =
                g_show_auto_chop && SelectedImageWillAutoChop();
            if (show_auto_chop_preview) {
                IMG *chop_img = get_img(g_doc->ilselected);
                if (g_chop_mode == AutoChopMode_BestHorizontal ||
                    g_chop_mode == AutoChopMode_BestVertical) {
                    bool vertical = (g_chop_mode == AutoChopMode_BestVertical);
                    show_auto_chop_preview =
                        BuildBestAutoSplitPreviewForImage(chop_img, vertical,
                                                          &auto_chop_preview) &&
                        !auto_chop_preview.pieces.empty();
                } else {
                    show_auto_chop_preview =
                        BuildAutoChopPreviewForImage(chop_img, &auto_chop_preview) &&
                        !auto_chop_preview.pieces.empty();
                }
                if (show_auto_chop_preview) {
                    DrawAutoChopPreviewRects(dl, auto_chop_preview,
                                             img_pos, sx, sy, false);
                }
            }

            /* Mirror preview. The flipped placement is drawn from the same
               texture with mirrored UVs, offset so both placements share the
               anipoint — see DrawCanvasFlipPreview. In Only mode the unflipped
               sprite fades to a hint so the flipped art reads as the subject;
               the ImGui::Image call itself always happens because it is what
               defines the canvas item rect every tool hit-tests against. */
            IMG *flip_img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            bool flip_preview_on = g_flip_preview != FlipPreviewMode::Off &&
                                   flip_img && flip_img->w > 0 &&
                                   !g_world_state.enabled &&
                                   !timeline_composite_preview_active;
            bool flip_only = flip_preview_on && g_flip_preview == FlipPreviewMode::Only;

            ImGui::Image((ImTextureID)(intptr_t)g_img_texture, img_sz,
                         ImVec2(0, 0), ImVec2(1, 1),
                         flip_only ? ImVec4(1, 1, 1, 0.22f) : ImVec4(1, 1, 1, 1));

            if (flip_preview_on) {
                DrawCanvasFlipPreview(dl, flip_img, g_img_texture,
                                      img_pos, img_sz, sx, sy,
                                      g_flip_preview == FlipPreviewMode::Ghost);
            }

            /* Color isolation: dim everything that isn't in the "kept" set.
               The set is either (a) the single Alt-clicked isolate index, or
               (b) the multi-selected swatch set (yellow rings in the palette
               grid). Alt-isolate wins when both are active.
               Scanline-coalesced so a 256x256 sprite emits at most ~256 rects
               per row of contiguous non-target pixels, not 65k per-pixel. */
            bool kept[256];
            bool any_kept = false;
            if (g_isolate_color >= 0) {
                memset(kept, 0, sizeof(kept));
                kept[g_isolate_color] = true;
                any_kept = true;
            } else {
                for (int ki = 0; ki < 256; ki++) {
                    kept[ki] = g_palette_selection[ki];
                    if (kept[ki]) any_kept = true;
                }
            }
            if (any_kept && g_doc->ilselected >= 0) {
                IMG *iimg = get_img(g_doc->ilselected);
                DrawCanvasColorIsolationOverlay(dl, iimg, kept, img_pos, sx, sy);
            }

            if (g_show_dma_comp) {
                IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
                DrawCanvasDmaCompressionOverlay(dl, img, img_pos, sx, sy);
            }

            DrawCanvasPixelGrid(dl, img_pos, img_sz, g_img_tex_w, g_img_tex_h, scale);

            if (show_auto_chop_preview) {
                DrawAutoChopPreviewRects(dl, auto_chop_preview,
                                         img_pos, sx, sy, true);
            }

        } else {
            g_zoom_pending_steps = 0;
            g_zoom_pending_fit = false;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.45f);
            float tw = ImGui::CalcTextSize("No image selected").x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - tw) * 0.5f);
            ImGui::TextDisabled("No image selected");
        }

        ImVec2 mouse = io.MousePos;
        bool   mbdn  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

        /* When a modal or popup window is on top, ImGui sets WantCaptureMouse —
         * suppress all canvas interaction (paint, eyedropper, highlight, drag,
         * marquee, anim-point handles, etc.) so clicks meant for the modal
         * don't bleed through to the sprite underneath. */
        bool canvas_input_blocked = CanvasInputBlocked(io);
        if (canvas_input_blocked) mbdn = false;

        /* Set when an overlay widget (anim point, hitbox corner) eats this frame's
           click, so the grid-selection block below doesn't also start a selection. */
        bool widget_consumed_click = false;

        if (rotate_buttons_visible && !canvas_input_blocked && !timeline_composite_preview_active) {
            for (int i = 0; i < 1; i++) {
                if (CanvasPointInRect(mouse, rotate_button_min[i], rotate_button_max[i])) {
                    rotate_button_hovered = true;
                    rotate_button_hover_idx = i;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Rotate 90 Clockwise (preserve anim points)");
                    widget_consumed_click = true;
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        TransformSelectedSprite(SpriteTransformOp::Rotate90CW);
                    }
                    break;
                }
            }
        }

        /* Pixel highlight at high zoom */
        if (!canvas_input_blocked && !timeline_composite_preview_active) {
            DrawCanvasPixelHoverHighlight(ImGui::GetWindowDrawList(),
                                          mouse, img_pos, img_sz, sx, sy,
                                          rotate_button_hovered);
        }

        /* ---- Pencil + eyedropper + fill + pan tools ---- */
        if (!canvas_input_blocked && !timeline_composite_preview_active) {
            IMG *cimg = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            bool over = mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                        mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y &&
                        !rotate_button_hovered;

            /* Pan: middle-mouse drag or spacebar+drag or right-drag at zoom */
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
                ZoomPanBy(avail, d.x, d.y);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                widget_consumed_click = true;
            }
            if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                ZoomPanBy(avail, d.x, d.y);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                widget_consumed_click = true;
            }
            if (g_active_tool == ActiveTool::None && g_zoom > 1.0f &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) && over) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
                ZoomPanBy(avail, d.x, d.y);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
                widget_consumed_click = true;
            }

            if (cimg && cimg->data_p && cimg->w > 0 && cimg->h > 0 && over) {
                int px = (int)((mouse.x - img_pos.x) / sx);
                int py = (int)((mouse.y - img_pos.y) / sy);
                if (px >= 0 && px < (int)cimg->w && py >= 0 && py < (int)cimg->h) {
                    unsigned short stride = (cimg->w + 3) & ~3;
                    unsigned char *pix = (unsigned char *)cimg->data_p + py * stride + px;

                    /* Right-click: eyedropper (works in any tool mode). When the
                       Single-Color Shading window is open, feed it the sampled
                       color instead of the normal fill-color slot. */
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        if (!PickColorForSingleColorDialog(*pix))
                            g_sel_color = *pix;
                        widget_consumed_click = true;
                    }
                    /* Eyedropper tool active: left-click also picks color.
                       Consumes the click so the pencil branch below is skipped. */
                    if (g_active_tool == ActiveTool::Eyedropper &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        if (!ApplyEyedropperColorToLockedSwatches(*pix))
                            g_sel_color = *pix;
                        widget_consumed_click = true;
                    }
                    /* Left-click: pencil, paint bucket, background eraser, clone stamp, or smart remap.
                       Suppress when the cursor is over (or dragging) an anipoint or hitbox
                       handle. The anipoint render block runs *after* this branch, so the
                       in-progress drag flag isn't enough on the first click frame — we
                       need to also detect "about to start dragging" via a fresh hover test. */
                    bool over_anipoint = false;
                    if (g_show_points && cimg) {
                        over_anipoint =
                            CanvasAnipointHitTest(cimg, img_pos, sx, sy,
                                                  mouse, NULL, NULL);
                    }
                    if (!g_pasted.active && !over_anipoint && !g_anipoint_drag1 && !g_anipoint_drag2 && g_hitbox_drag_corner < 0
                        && (g_active_tool == ActiveTool::None || g_active_tool == ActiveTool::Pencil || g_active_tool == ActiveTool::PaintBucket || g_active_tool == ActiveTool::VariantPaint || g_active_tool == ActiveTool::BackgroundEraser || g_active_tool == ActiveTool::CloneStamp || g_active_tool == ActiveTool::SmartRemap
                            || g_active_tool == ActiveTool::Blur || g_active_tool == ActiveTool::Smudge
                            || g_active_tool == ActiveTool::ContentErase)) {
                        /* Stroke begin: capture a pre-stroke snapshot of the
                           image's pixel buffer on the first frame of left-mouse
                           down for any paint tool. Skipped for Clone Stamp's
                           Alt-click "set source" which doesn't modify pixels. */
                        bool stroke_begin = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                            && !(g_active_tool == ActiveTool::CloneStamp && io.KeyAlt);
                        if (stroke_begin) {
                            if (g_active_tool == ActiveTool::VariantPaint && g_sel_color > 0)
                                doc_undo_push();
                            else if (g_active_tool != ActiveTool::VariantPaint)
                                pixel_hist_push_stroke();
                        }
                        if (g_active_tool == ActiveTool::Blur ||
                            g_active_tool == ActiveTool::Smudge ||
                            g_active_tool == ActiveTool::ContentErase) {
                            if (stroke_begin) {
                                g_smudge_have_last = false;
                                g_smudge_last_x = px;
                                g_smudge_last_y = py;
                            }
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                PAL *tp = get_pal(g_doc->plselected);
                                int n = 0;
                                if (g_active_tool == ActiveTool::Blur) {
                                    n = PaintBlurStamp(cimg, tp, px, py,
                                                       g_blur_brush, g_blur_strength);
                                } else if (g_active_tool == ActiveTool::Smudge) {
                                    /* Needs a direction, so the first sample of a
                                       stroke only records where the brush started. */
                                    if (g_smudge_have_last)
                                        n = PaintSmudgeStamp(cimg, tp,
                                                             g_smudge_last_x, g_smudge_last_y,
                                                             px, py, g_smudge_brush,
                                                             g_smudge_strength);
                                } else {
                                    n = PaintContentAwareErase(cimg, tp, px, py,
                                                               g_content_erase_brush,
                                                               g_content_erase_passes);
                                }
                                g_smudge_last_x = px;
                                g_smudge_last_y = py;
                                g_smudge_have_last = true;
                                if (n > 0) {
                                    mark_dirty();
                                    g_img_tex_idx = -2;
                                }
                                widget_consumed_click = true;
                            }
                        }
                        else if (g_active_tool == ActiveTool::CloneStamp) {
                            if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                g_clone_src_x = px;
                                g_clone_src_y = py;
                                g_clone_source_set = true;
                                g_clone_offset_set = false;
                                widget_consumed_click = true;
                            } else if (!io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_clone_source_set) {
                                if (!g_clone_offset_set) {
                                    g_clone_dx = g_clone_src_x - px;
                                    g_clone_dy = g_clone_src_y - py;
                                    g_clone_offset_set = true;
                                }
                                if (g_pixel_undo_img != g_doc->ilselected) {
                                    free(g_pixel_undo); g_pixel_undo = NULL;
                                    unsigned short s = (cimg->w + 3) & ~3;
                                    unsigned int sz = (unsigned int)s * cimg->h;
                                    g_pixel_undo = (unsigned char *)malloc(sz);
                                    if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                                    g_pixel_undo_img = g_doc->ilselected;
                                }
                                /* Round-brush stamp. g_clone_brush is the radius;
                                   1 = single pixel (kept for sharp work), >1 = soft disc. */
                                int r = g_clone_brush > 0 ? g_clone_brush : 1;
                                int r2 = (r - 1) * (r - 1);
                                unsigned short stride = (cimg->w + 3) & ~3;
                                unsigned char *cdata = (unsigned char *)cimg->data_p;
                                for (int by = -(r - 1); by <= (r - 1); by++) {
                                    for (int bx = -(r - 1); bx <= (r - 1); bx++) {
                                        if (r > 1 && bx * bx + by * by > r2) continue;
                                        int dx_px = px + bx;
                                        int dy_px = py + by;
                                        if (dx_px < 0 || dy_px < 0 ||
                                            dx_px >= (int)cimg->w || dy_px >= (int)cimg->h) continue;
                                        int src_px = dx_px + g_clone_dx;
                                        int src_py = dy_px + g_clone_dy;
                                        if (src_px < 0 || src_py < 0 ||
                                            src_px >= (int)cimg->w || src_py >= (int)cimg->h) continue;
                                        unsigned char src_col = cdata[src_py * stride + src_px];
                                        cdata[dy_px * stride + dx_px] = src_col;
                                    }
                                }
                                mark_dirty();
                                g_img_tex_idx = -2;
                                widget_consumed_click = true;
                            }
                        } else if (g_active_tool == ActiveTool::PaintBucket) {
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                int changed = PaintBucketFill(cimg, px, py,
                                                              (unsigned char)g_sel_color,
                                                              g_bucket_tolerance,
                                                              g_bucket_contiguous);
                                if (changed > 0) {
                                    mark_dirty();
                                    g_img_tex_idx = -2;
                                }
                                if (changed > 0) {
                                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                                             "Paint bucket filled %d pixel%s.",
                                             changed, changed == 1 ? "" : "s");
                                } else {
                                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                                             "Paint bucket: no pixels changed.");
                                }
                                g_restore_msg_timer = 3.0f;
                                widget_consumed_click = true;
                            }
                        } else if (g_active_tool == ActiveTool::VariantPaint) {
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                VariantPaintResult vr = ApplyVariantBrush(cimg, px, py, g_variant_brush);
                                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                    if (g_sel_color == 0) {
                                        snprintf(g_restore_msg, sizeof(g_restore_msg), "Variant paint needs an opaque target color.");
                                        g_restore_msg_timer = 4.0f;
                                    } else if (vr.skipped_no_slot > 0 && vr.pixels == 0) {
                                        snprintf(g_restore_msg, sizeof(g_restore_msg), "No free palette index for variant shadow.");
                                        g_restore_msg_timer = 4.0f;
                                    }
                                }
                                widget_consumed_click = true;
                            }
                        } else if (g_active_tool == ActiveTool::SmartRemap) {
                            /* On the first click of a stroke, capture the target
                               color. Hold to paint replacement over every pixel
                               within tolerance of that target. The target sticks
                               until mouse-up so a single drag has consistent
                               behavior even as the brush crosses varied pixels. */
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                g_remap_target_color = *pix;
                            }
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_remap_target_color != -1) {
                                int diff = (int)*pix - g_remap_target_color;
                                if (diff < 0) diff = -diff;
                                if (diff <= g_remap_tolerance) {
                                    if (g_pixel_undo_img != g_doc->ilselected) {
                                        free(g_pixel_undo); g_pixel_undo = NULL;
                                        unsigned short s = (cimg->w + 3) & ~3;
                                        unsigned int sz = (unsigned int)s * cimg->h;
                                        g_pixel_undo = (unsigned char *)malloc(sz);
                                        if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                                        g_pixel_undo_img = g_doc->ilselected;
                                    }
                                    *pix = (unsigned char)g_sel_color;
                                    mark_dirty();
                                    g_img_tex_idx = -2;
                                }
                                widget_consumed_click = true;
                            }
                            /* Release: forget the target so the next click can pick
                               a different reference color. */
                            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                                g_remap_target_color = -1;
                            }
                        } else if (g_active_tool == ActiveTool::BackgroundEraser
                                       ? ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                                       : ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            if (g_pixel_undo_img != g_doc->ilselected) {
                                free(g_pixel_undo); g_pixel_undo = NULL;
                                unsigned short s = (cimg->w + 3) & ~3;
                                unsigned int sz = (unsigned int)s * cimg->h;
                                g_pixel_undo = (unsigned char *)malloc(sz);
                                if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                                g_pixel_undo_img = g_doc->ilselected;
                            }
                            if (g_active_tool == ActiveTool::BackgroundEraser) {
                                mark_dirty();
                                SmartErase(cimg, px, py,
                                           g_eraser_tolerance,
                                           g_eraser_contiguous,
                                           g_eraser_defringe);
                            } else if (io.KeyShift) {
                                mark_dirty();
                                FloodFill(cimg, px, py, (unsigned char)g_sel_color);
                            } else {
                                mark_dirty();
                                /* Pencil with radius >1 stamps a disc. r=1 keeps
                                   the single-pixel behavior the underlying paint
                                   path has always had. */
                                int r = (g_active_tool == ActiveTool::Pencil && g_pencil_brush > 1)
                                            ? g_pencil_brush : 1;
                                if (r == 1) {
                                    *pix = (unsigned char)g_sel_color;
                                } else {
                                    int r2 = (r - 1) * (r - 1);
                                    unsigned short stride = (cimg->w + 3) & ~3;
                                    unsigned char *cdata = (unsigned char *)cimg->data_p;
                                    for (int by = -(r - 1); by <= (r - 1); by++)
                                    for (int bx = -(r - 1); bx <= (r - 1); bx++) {
                                        if (bx * bx + by * by > r2) continue;
                                        int dx_px = px + bx, dy_px = py + by;
                                        if (dx_px < 0 || dy_px < 0
                                            || dx_px >= (int)cimg->w
                                            || dy_px >= (int)cimg->h) continue;
                                        cdata[dy_px * stride + dx_px] = (unsigned char)g_sel_color;
                                    }
                                }
                            }
                            g_img_tex_idx = -2;
                            widget_consumed_click = true;
                        }
                    }
                }
            }
        }

        /* --- Anim point overlay + dragging --- */
        /* Anipoints + IMG hitbox don't render in World View. The World
           View canvas anchors the sprite at world-origin-minus-anipoint
           so the on-sprite anipoint marker would land outside or at the
           wrong spot, and the IMG hitbox box would visually float
           detached from the playfield rectangle. Both stay reachable
           via their normal modes when World View is off. */
        if (g_show_points && !canvas_input_blocked && !g_world_state.enabled && !timeline_composite_preview_active) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img && img->w > 0) {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                bool h1 = false;
                bool h2 = false;
                IMG *prev = (g_timeline_onion && g_doc->ilselected > 0)
                    ? get_img(g_doc->ilselected - 1)
                    : NULL;
                DrawCanvasAnipointOverlay(dl, img, prev,
                                          img_pos, sx, sy, mouse,
                                          &h1, &h2);

                if (h1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { g_anipoint_drag1 = true; widget_consumed_click = true; }
                if (g_anipoint_drag1 && mbdn) {
                    int nx = (int)((mouse.x - img_pos.x) / sx);
                    int ny = (int)((mouse.y - img_pos.y) / sy);
                    set_primary_anipoint_with_sequence(img, nx, ny);
                    widget_consumed_click = true;
                } else if (!mbdn && g_anipoint_drag1) { g_anipoint_drag1 = false; }

                /* Secondary anipoint drag/editing stays here; drawing and
                   hover detection live in ui_canvas. */
                if (secondary_anipoint_in_use(img)) {
                    if (h2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { g_anipoint_drag2 = true; widget_consumed_click = true; }
                    if (g_anipoint_drag2 && mbdn) {
                        int nx = (int)((mouse.x - img_pos.x) / sx);
                        int ny = (int)((mouse.y - img_pos.y) / sy);
                        set_secondary_anipoint_with_sequence(img, nx, ny);
                        widget_consumed_click = true;
                    } else if (!mbdn && g_anipoint_drag2) { g_anipoint_drag2 = false; }
                }
            }
        }

        /* --- Strike-box overlay (MKSTK.ASM) ---
           The one hitbox the game has. Draws the collision box of the move
           the selected frame belongs to, with corner handles that write
           straight into the MKSTK document.

           There used to be a second, cyan overlay over g_hitbox_x/y/w/h here.
           Those were four app-wide globals attached to no sprite, loaded from
           nothing and saved nowhere, so it drew a box that could not mean
           anything; the Hitboxes toggle now shows this one instead.

           Drawing runs whenever a move resolves -- the editor panel can hold
           focus (which sets canvas_input_blocked) without hiding the box.
           Only the corner-drag interaction is gated. */
        bool strike_visible = (g_show_hitbox || g_show_mk2) &&
                              !g_world_state.enabled &&
                              !timeline_composite_preview_active;
        int mk2_rec = strike_visible ? Mk2CurrentRecord() : -1;
        if (mk2_rec >= 0) {
            const mk2::StrikeRecord &rec = g_mk2_doc.records[mk2_rec];
            int hx = rec.fields[mk2::F_X_OFFSET].has_value ? (int)rec.fields[mk2::F_X_OFFSET].value : 0;
            int hy = rec.fields[mk2::F_Y_OFFSET].has_value ? (int)rec.fields[mk2::F_Y_OFFSET].value : 0;
            int hw = rec.fields[mk2::F_X_SIZE  ].has_value ? (int)rec.fields[mk2::F_X_SIZE  ].value : 0;
            int hh = rec.fields[mk2::F_Y_SIZE  ].has_value ? (int)rec.fields[mk2::F_Y_SIZE  ].value : 0;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            char tag[80];
            snprintf(tag, sizeof(tag), "%s  (%d,%d %dx%d)", rec.label.c_str(), hx, hy, hw, hh);
            bool  hovering[4] = { false, false, false, false };
            DrawCanvasStrikeBoxOverlay(dl, img_pos, sx, sy,
                                       hx, hy, hw, hh,
                                       tag, mouse, !canvas_input_blocked,
                                       hovering);
            if (!canvas_input_blocked) {
                for (int c = 0; c < 4; c++) {
                    if (hovering[c] && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        /* Snapshot the pre-drag state once. Subsequent
                           per-pixel updates during the drag coalesce. */
                        mk2::undo_push(&g_mk2_doc, mk2_rec, false);
                        g_mk2_drag_corner = c;
                        widget_consumed_click = true;
                    }
                }
            }
            if (g_mk2_drag_corner >= 0 && mbdn) {
                int mx = (int)((mouse.x - img_pos.x) / sx);
                int my = (int)((mouse.y - img_pos.y) / sy);
                int nx = hx, ny = hy, nw = hw, nh = hh;
                CanvasResizeRectFromCorner(g_mk2_drag_corner, mx, my,
                                           &nx, &ny, &nw, &nh);
                /* Push values through the document so the .ASM line buffer
                   stays in sync and Save picks them up. */
                if (nx != hx) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_X_OFFSET, nx);
                if (ny != hy) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_Y_OFFSET, ny);
                if (nw != hw) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_X_SIZE,   nw);
                if (nh != hh) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_Y_SIZE,   nh);
                widget_consumed_click = true;
            } else if (!mbdn && g_mk2_drag_corner >= 0) {
                g_mk2_drag_corner = -1;
            }
        }

        /* --- Grid selection tool (for copy/paste) --- */
        if (!timeline_composite_preview_active && g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            ImDrawList *dl = ImGui::GetWindowDrawList();

            /* Mouse-over-sprite test — clicks outside this rect must NOT start a selection. */
            bool mouse_over_sprite =
                mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y &&
                !rotate_button_hovered;

            /* Pencil cursor indicator — color tracks the currently-selected
               palette entry so the user previews what they're about to paint.
               Index 0 (transparent) falls back to white. Two render modes:
                 brush > 1: ring around the round-disc stamp footprint.
                 brush = 1: small offset crosshair so the single target pixel
                            stays visible underneath. The crosshair lives
                            outside the pixel rect itself so it never hides
                            the pixel it points at. */
            if (g_active_tool == ActiveTool::Pencil && mouse_over_sprite) {
                int mx = (int)((mouse.x - img_pos.x) / sx);
                int my = (int)((mouse.y - img_pos.y) / sy);
                ImU32 col;
                if (g_sel_color > 0) {
                    SDL_Color &c = g_palette[g_sel_color];
                    col = IM_COL32(c.r, c.g, c.b, 230);
                } else {
                    col = IM_COL32(255, 255, 255, 200);
                }
                DrawCanvasPencilCursor(dl, img_pos, sx, sy, mx, my,
                                       g_pencil_brush, col);
            }

            /* Clone Stamp visual aids: source crosshair and destination brush ring. */
            if (g_active_tool == ActiveTool::CloneStamp && g_clone_source_set) {
                bool show_dest_brush = mouse_over_sprite && g_clone_brush > 1;
                int mx = 0;
                int my = 0;
                if (show_dest_brush) {
                    mx = (int)((mouse.x - img_pos.x) / sx);
                    my = (int)((mouse.y - img_pos.y) / sy);
                }
                DrawCanvasCloneStampAids(dl, img_pos, sx, sy,
                                         g_clone_src_x, g_clone_src_y,
                                         show_dest_brush, mx, my,
                                         g_clone_brush);
            }

            /* Start a new selection only on a fresh click that lands on the sprite
               and isn't already being consumed by an anim-point or hitbox-corner drag.
               Once a drag is in progress we keep updating x2/y2 wherever the mouse
               goes (clamped) until the button is released. */
            /* Block selection when:
               - the mouse is over a hovered ImGui widget (menu item, button)
                 OR an active item is being interacted with;
               - any popup/menu is open (its dropdown can overlap the canvas
                 and clicking through it must not start a marquee).
               Geometric mouse_over_sprite still has to be true. */
            bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            bool ui_blocking = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || any_popup;
            if (!g_pasted.active && (g_active_tool == ActiveTool::Marquee || g_active_tool == ActiveTool::MagicWand || g_active_tool == ActiveTool::Lasso)) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                        && mouse_over_sprite
                        && !widget_consumed_click
                        && !ui_blocking) {
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    if (mx < 0) mx = 0; if (mx >= (int)g_img_tex_w) mx = g_img_tex_w - 1;
                    if (my < 0) my = 0; if (my >= (int)g_img_tex_h) my = g_img_tex_h - 1;
                    
                    if (g_active_tool == ActiveTool::MagicWand && g_doc->ilselected >= 0) {
                        IMG* simg = get_img(g_doc->ilselected);
                        if (simg && simg->data_p) {
                            int sw = simg->w;
                            int sh = simg->h;
                            int stride = (sw + 3) & ~3;
                            unsigned char* pdata = (unsigned char*)simg->data_p;
                            int target_color = pdata[my * stride + mx];
                            int tol = g_wand_tolerance;
                            selection_begin_add_drag(sw, sh, ImGui::GetIO().KeyCtrl);

                            g_grid_sel.active = true;
                            g_grid_sel.is_mask = true;
                            g_grid_sel.mask_w = sw;
                            g_grid_sel.mask_h = sh;
                            g_grid_sel.pixel_mask.assign((size_t)sw * sh, false);

                            int min_x = mx, max_x = mx;
                            int min_y = my, max_y = my;

                            if (g_wand_contiguous) {
                                std::vector<std::pair<int, int>> stack;
                                stack.push_back({mx, my});
                                g_grid_sel.pixel_mask[my * sw + mx] = true;

                                while(!stack.empty()) {
                                    std::pair<int, int> pt = stack.back();
                                    stack.pop_back();
                                    int cx = pt.first;
                                    int cy = pt.second;

                                    if (cx < min_x) min_x = cx;
                                    if (cx > max_x) max_x = cx;
                                    if (cy < min_y) min_y = cy;
                                    if (cy > max_y) max_y = cy;

                                    const int dx[] = {0, 1, 0, -1};
                                    const int dy[] = {-1, 0, 1, 0};
                                    for(int i = 0; i < 4; i++) {
                                        int nx = cx + dx[i];
                                        int ny = cy + dy[i];
                                        if (nx >= 0 && nx < sw && ny >= 0 && ny < sh) {
                                            int diff = (int)pdata[ny * stride + nx] - target_color;
                                            if (diff < 0) diff = -diff;
                                            if (!g_grid_sel.pixel_mask[ny * sw + nx] && diff <= tol) {
                                                g_grid_sel.pixel_mask[ny * sw + nx] = true;
                                                stack.push_back({nx, ny});
                                            }
                                        }
                                    }
                                }
                            } else {
                                /* Global non-contiguous: every pixel in image within tolerance */
                                bool first = true;
                                for (int y = 0; y < sh; y++) {
                                    for (int x = 0; x < sw; x++) {
                                        int diff = (int)pdata[y * stride + x] - target_color;
                                        if (diff < 0) diff = -diff;
                                        if (diff <= tol) {
                                            g_grid_sel.pixel_mask[y * sw + x] = true;
                                            if (first) {
                                                min_x = max_x = x; min_y = max_y = y;
                                                first = false;
                                            } else {
                                                if (x < min_x) min_x = x;
                                                if (x > max_x) max_x = x;
                                                if (y < min_y) min_y = y;
                                                if (y > max_y) max_y = y;
                                            }
                                        }
                                    }
                                }
                            }
                            g_grid_sel.x1 = min_x;
                            g_grid_sel.y1 = min_y;
                            g_grid_sel.x2 = max_x;
                            g_grid_sel.y2 = max_y;
                            g_grid_sel.dragging = false;
                            selection_finish_add_drag(sw, sh);
                        }
                    } else if (g_active_tool == ActiveTool::Lasso) {
                        selection_begin_add_drag(g_img_tex_w, g_img_tex_h, ImGui::GetIO().KeyCtrl);
                        g_lasso_points.clear();
                        g_lasso_points.push_back({mx, my});
                        g_grid_sel.active = true;
                        g_grid_sel.dragging = true;
                        g_grid_sel.is_mask = false; /* becomes a mask on release */
                        g_grid_sel.x1 = g_grid_sel.x2 = mx;
                        g_grid_sel.y1 = g_grid_sel.y2 = my;
                    } else {
                        selection_begin_add_drag(g_img_tex_w, g_img_tex_h, ImGui::GetIO().KeyCtrl);
                        g_grid_sel.active = true;
                        g_grid_sel.dragging = true;
                        g_grid_sel.is_mask = false;
                        g_grid_sel.x1 = g_grid_sel.x2 = mx;
                        g_grid_sel.y1 = g_grid_sel.y2 = my;
                    }
                } else if (g_grid_sel.dragging && mbdn) {
                    /* Only extend the rect while we're in the user-initiated
                       drag — not on every frame the button happens to be
                       down (e.g. a click on a menu would otherwise reposition
                       the marquee to wherever the menu click landed). */
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    if (mx < 0) mx = 0; if (mx >= (int)g_img_tex_w) mx = g_img_tex_w - 1;
                    if (my < 0) my = 0; if (my >= (int)g_img_tex_h) my = g_img_tex_h - 1;
                    if (g_active_tool == ActiveTool::Lasso) {
                        /* Append point if it moved at least 1 pixel from the last vertex */
                        if (g_lasso_points.empty() ||
                            g_lasso_points.back().first != mx ||
                            g_lasso_points.back().second != my) {
                            g_lasso_points.push_back({mx, my});
                        }
                    } else {
                        g_grid_sel.x2 = mx;
                        g_grid_sel.y2 = my;
                    }
                } else if (g_grid_sel.dragging && !mbdn) {
                    g_grid_sel.dragging = false;
                    if (g_active_tool == ActiveTool::Lasso && g_doc->ilselected >= 0 && g_lasso_points.size() >= 3) {
                        /* Rasterize the polygon into a pixel mask using a scanline
                           even-odd test. */
                        IMG *simg = get_img(g_doc->ilselected);
                        if (simg && simg->data_p) {
                            int sw = simg->w, sh = simg->h;
                            int min_x = sw, max_x = -1, min_y = sh, max_y = -1;
                            for (auto &p : g_lasso_points) {
                                if (p.first  < min_x) min_x = p.first;
                                if (p.first  > max_x) max_x = p.first;
                                if (p.second < min_y) min_y = p.second;
                                if (p.second > max_y) max_y = p.second;
                            }
                            if (min_x < 0) min_x = 0;
                            if (min_y < 0) min_y = 0;
                            if (max_x >= sw) max_x = sw - 1;
                            if (max_y >= sh) max_y = sh - 1;

                            g_grid_sel.is_mask = true;
                            g_grid_sel.mask_w = sw;
                            g_grid_sel.mask_h = sh;
                            g_grid_sel.pixel_mask.assign((size_t)sw * sh, false);

                            int n = (int)g_lasso_points.size();
                            for (int y = min_y; y <= max_y; y++) {
                                /* Crossings at half-pixel y */
                                float yf = y + 0.5f;
                                std::vector<float> xs;
                                for (int i = 0; i < n; i++) {
                                    float ax = (float)g_lasso_points[i].first;
                                    float ay = (float)g_lasso_points[i].second;
                                    float bx = (float)g_lasso_points[(i + 1) % n].first;
                                    float by = (float)g_lasso_points[(i + 1) % n].second;
                                    if ((ay <= yf) != (by <= yf)) {
                                        float t = (yf - ay) / (by - ay);
                                        xs.push_back(ax + t * (bx - ax));
                                    }
                                }
                                std::sort(xs.begin(), xs.end());
                                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                                    int x0 = (int)ceilf(xs[i]);
                                    int x1 = (int)floorf(xs[i + 1]);
                                    if (x0 < min_x) x0 = min_x;
                                    if (x1 > max_x) x1 = max_x;
                                    for (int x = x0; x <= x1; x++) {
                                        g_grid_sel.pixel_mask[y * sw + x] = true;
                                    }
                                }
                            }
                            g_grid_sel.x1 = min_x;
                            g_grid_sel.y1 = min_y;
                            g_grid_sel.x2 = max_x < min_x ? min_x : max_x;
                            g_grid_sel.y2 = max_y < min_y ? min_y : max_y;
                        }
                        g_lasso_points.clear();
                    } else if (g_active_tool == ActiveTool::Lasso) {
                        /* Aborted / too few points */
                        g_lasso_points.clear();
                        g_grid_sel.active = false;
                    }
                    if (ImGui::GetIO().KeyShift && !g_grid_sel.is_mask && g_doc->ilselected >= 0) {
                        IMG *simg = get_img(g_doc->ilselected);
                        if (simg && simg->data_p) {
                            int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
                            int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
                            if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
                            if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
                            int min_x = x2, max_x = x1, min_y = y2, max_y = y1;
                            bool found = false;
                            unsigned short stride = (simg->w + 3) & ~3;
                            unsigned char *pdata = (unsigned char *)simg->data_p;
                            for (int y = y1; y <= y2; y++) {
                                for (int x = x1; x <= x2; x++) {
                                    if (pdata[y * stride + x] != 0) {
                                        if (x < min_x) min_x = x;
                                        if (x > max_x) max_x = x;
                                        if (y < min_y) min_y = y;
                                        if (y > max_y) max_y = y;
                                        found = true;
                                    }
                                }
                            }
                            if (found) {
                                g_grid_sel.x1 = min_x;
                                g_grid_sel.y1 = min_y;
                                g_grid_sel.x2 = max_x;
                                g_grid_sel.y2 = max_y;
                            }
                        }
                    }
                    if (g_selection_add_drag && g_doc->ilselected >= 0) {
                        IMG *simg = get_img(g_doc->ilselected);
                        if (simg) selection_finish_add_drag(simg->w, simg->h);
                    }
                }
            }

            /* Draw selection rectangle only when the marquee tool is on. Toggling
               the tool off via the toolbar/R also clears g_grid_sel, but this
               extra gate makes sure no stray green box renders if some other
               code path leaves g_grid_sel.active=true with the tool off. */
            /* Live lasso path while drawing */
            if (g_active_tool == ActiveTool::Lasso && g_grid_sel.dragging && g_lasso_points.size() >= 2) {
                DrawCanvasLassoPath(dl, img_pos, sx, sy, g_lasso_points);
            }

            if (g_grid_sel.active && (g_active_tool == ActiveTool::Marquee || g_active_tool == ActiveTool::MagicWand || g_active_tool == ActiveTool::Lasso)) {
                DrawCanvasSelectionOverlay(dl, img_pos, sx, sy,
                                           g_grid_sel.x1, g_grid_sel.y1,
                                           g_grid_sel.x2, g_grid_sel.y2,
                                           g_grid_sel.is_mask,
                                           g_grid_sel.mask_w,
                                           &g_grid_sel.pixel_mask);
                DrawSelectionBatchContextMenu(img_pos, sx, sy);
            }

            /* Defensive: transform mode can't exist without a floating paste.
               Several state-clearing paths (ClearAll, file-open, etc.) drop
               g_pasted.active without knowing about the transform, so latch
               g_xform off here rather than scatter g_xform.active = false
               across every site. */
            if (g_xform.active && !g_pasted.active) {
                g_xform.active = false;
                g_xform.handle = TransformHandle::None;
            }

            /* Paste overlay with pixel preview */
            if (g_pasted.active && g_clipboard.valid && g_clipboard.w > 0 && g_clipboard.h > 0) {
                CanvasPasteGeometry paste_geom = CanvasPasteGeometryForState(
                    img_pos, sx, sy, img_sz, mouse,
                    g_xform.active,
                    g_xform.rx, g_xform.ry, g_xform.rw, g_xform.rh,
                    g_xform.angle_deg,
                    g_pasted.paste_x, g_pasted.paste_y,
                    g_clipboard.w, g_clipboard.h);
                int px = paste_geom.x;
                int py = paste_geom.y;
                int pw = paste_geom.w;
                int ph = paste_geom.h;
                unsigned short cs = g_clipboard.stride;

                const CanvasTransform2D &paste_xf = paste_geom.transform;
                CanvasPasteControlsLayout paste_controls =
                    CanvasPasteControlsLayoutFor(canvas_origin, mouse);
                bool paste_controls_block = paste_controls.blocks_mouse;
                bool hovering = paste_geom.hit.hovering;
                bool over_sprite = paste_geom.hit.over_sprite;

                /* Render clipboard pixel preview, including live scale/rotation
                   while Free Transform is active. At Normal/100 this is fully
                   opaque so the pasted sprite is visible; opacity/blend choices
                   preview the same RGB composite used by final commit. */
                IMG *simg = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
                PAL *spal = simg ? get_pal(simg->palnum) : NULL;
                const unsigned char *dst_pixels = simg ? (const unsigned char *)simg->data_p : NULL;
                int dst_stride = simg ? ((int)simg->w + 3) & ~3 : 0;
                const unsigned char *src = (const unsigned char *)g_clipboard.data_p;
                unsigned char paste_pal_map[256];
                bool paste_remap = BuildClipboardPaletteMap(spal, paste_pal_map);
                int cw = g_clipboard.w;
                int ch = g_clipboard.h;
                for (int y = 0; y < ch; y++) {
                    for (int x = 0; x < cw; x++) {
                        unsigned char ci = src[y * cs + x];
                        if (ci == 0) continue;
                        CanvasPastePreviewCell cell =
                            CanvasPastePreviewCellForPixel(
                                paste_xf, px, py, pw, ph, cw, ch, x, y);
                        unsigned char dst_ci = 0;
                        if (dst_pixels && simg && cell.target_x >= 0 &&
                            cell.target_y >= 0 &&
                            cell.target_x < (int)simg->w &&
                            cell.target_y < (int)simg->h)
                            dst_ci = dst_pixels[cell.target_y * dst_stride +
                                                cell.target_x];
                        int rr = 255, gg = 255, bb = 255, aa = 255;
                        if (g_cookie_cut_mode) {
                            /* A red translucent stencil makes the pixels that
                               will be erased unambiguous on every palette. */
                            rr = 255; gg = 48; bb = 48; aa = 150;
                        } else if (!paste_preview_rgba(ci, dst_ci, spal, paste_pal_map,
                                                       paste_remap,
                                                       cell.target_x, cell.target_y,
                                                       &rr, &gg, &bb, &aa)) {
                            continue;
                        }
                        ImU32 col = IM_COL32((unsigned char)rr,
                                             (unsigned char)gg,
                                             (unsigned char)bb,
                                             (unsigned char)aa);
                        dl->AddQuadFilled(cell.quad[0], cell.quad[1],
                                          cell.quad[2], cell.quad[3], col);
                    }
                }

                DrawCanvasPasteBorder(dl, paste_geom.corners,
                                      g_xform.active, hovering);

                /* Snap guides — drawn while a snap is active this frame so
                   the user sees exactly which edge their paste locked onto. */
                DrawCanvasPasteSnapGuides(dl, img_pos, sx, sy,
                                          g_img_tex_w, g_img_tex_h,
                                          g_pasted.dragging,
                                          g_snap_hit_x, g_snap_guide_x,
                                          g_snap_hit_y, g_snap_guide_y);

                CanvasPasteHint paste_hint =
                    CanvasPasteHintForState(g_xform.active, g_xform.handle,
                                            g_pasted.dragging);
                DrawCanvasPasteHint(dl, img_pos,
                                    paste_hint.text, paste_hint.color);

                dl->AddRectFilled(paste_controls.min, paste_controls.max,
                                  IM_COL32(18, 20, 24, 230), 4.0f);
                dl->AddRect(paste_controls.min, paste_controls.max,
                            IM_COL32(90, 130, 180, 210), 4.0f, 0, 1.0f);
                ImGui::PushID("paste_controls");
                ImGui::SetCursorScreenPos(paste_controls.blend_label_pos);
                ImGui::TextUnformatted(g_cookie_cut_mode ? "COOKIE CUT" : "Blend");
                ImGui::SetCursorScreenPos(paste_controls.blend_control_pos);
                ImGui::SetNextItemWidth(paste_controls.item_width);
                if (ImGui::BeginCombo("##blend", PasteBlendModeName(g_paste_blend_mode))) {
                    for (PasteBlendMode mode : k_paste_blend_modes) {
                        bool selected = (g_paste_blend_mode == mode);
                        if (ImGui::Selectable(PasteBlendModeName(mode), selected))
                            g_paste_blend_mode = mode;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetCursorScreenPos(paste_controls.opacity_label_pos);
                ImGui::TextUnformatted("Opacity");
                ImGui::SetCursorScreenPos(paste_controls.opacity_control_pos);
                ImGui::SetNextItemWidth(paste_controls.item_width);
                ImGui::SliderInt("##opacity", &g_paste_opacity, 0, 100, "%d%%");
                ImGui::SetCursorScreenPos(paste_controls.smooth_control_pos);
                ImGui::Checkbox("Smooth Scaling", &g_paste_smooth_resize);
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("On: blends colors when stretching/rotating this paste,\n"
                                      "so it doesn't look as blocky when enlarged.\n"
                                      "Off: keeps crisp pixel-art edges (nearest-neighbor).");
                }
                ImGui::PopID();

                /* ----- Free Transform handles + interaction ----- */
                if (g_xform.active && !canvas_input_blocked) {
                    CanvasTransformHandleOverlay handle_overlay =
                        DrawCanvasTransformHandles(dl, paste_geom.corners,
                                                   mouse,
                                                   g_xform.handle,
                                                   g_xform.aspect_locked);
                    TransformHandle hover_h = handle_overlay.hover;
                    float center_sx = handle_overlay.center.x;
                    float center_sy = handle_overlay.center.y;
                    ImVec2 ch1 = handle_overlay.chain_min;
                    ImVec2 ch2 = handle_overlay.chain_max;
                    bool chain_hov = handle_overlay.chain_hover;
                    if (handle_overlay.rotate_hover)
                        ImGui::SetTooltip("Rotate paste");

                    if (chain_hov) {
                        ImGui::SetTooltip(g_xform.aspect_locked
                            ? "Aspect ratio locked. Click to unlock (free scale)."
                            : "Aspect ratio free. Click to lock (proportional scale).");
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            g_xform.aspect_locked = !g_xform.aspect_locked;
                        }
                    }

                    /* Handle drag: pick on click, scale on drag, release commits. */
                    if (!paste_controls_block &&
                        g_xform.handle == TransformHandle::None && hover_h != TransformHandle::None &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_xform.handle  = hover_h;
                        g_xform.drag_mx = mouse.x;
                        g_xform.drag_my = mouse.y;
                        g_xform.drag_rx = g_xform.rx;
                        g_xform.drag_ry = g_xform.ry;
                        g_xform.drag_rw = g_xform.rw;
                        g_xform.drag_rh = g_xform.rh;
                        g_xform.drag_angle_deg = g_xform.angle_deg;
                        g_xform.ref_aspect = (g_xform.rh > 0)
                            ? (float)g_xform.rw / (float)g_xform.rh
                            : 1.0f;
                    }
                    if (!paste_controls_block &&
                        g_xform.handle == TransformHandle::None && hover_h == TransformHandle::None &&
                        hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_xform.handle  = TransformHandle::Move;
                        g_xform.drag_mx = mouse.x;
                        g_xform.drag_my = mouse.y;
                        g_xform.drag_rx = g_xform.rx;
                        g_xform.drag_ry = g_xform.ry;
                        g_xform.drag_rw = g_xform.rw;
                        g_xform.drag_rh = g_xform.rh;
                        g_xform.drag_angle_deg = g_xform.angle_deg;
                    }
                    if (g_xform.handle != TransformHandle::None && mbdn) {
                        CanvasTransformDragStart drag_start;
                        drag_start.handle = g_xform.handle;
                        drag_start.mouse = ImVec2(g_xform.drag_mx,
                                                  g_xform.drag_my);
                        drag_start.x = g_xform.drag_rx;
                        drag_start.y = g_xform.drag_ry;
                        drag_start.w = g_xform.drag_rw;
                        drag_start.h = g_xform.drag_rh;
                        drag_start.angle_deg = g_xform.drag_angle_deg;
                        drag_start.ref_aspect = g_xform.ref_aspect;
                        CanvasTransformDragResult drag =
                            CanvasResolveTransformDrag(
                                drag_start, mouse, sx, sy,
                                ImVec2(center_sx, center_sy),
                                g_xform.aspect_locked,
                                ImGui::GetIO().KeyShift);
                        g_xform.rx = drag.x;
                        g_xform.ry = drag.y;
                        g_xform.rw = drag.w;
                        g_xform.rh = drag.h;
                        g_xform.angle_deg = drag.angle_deg;
                    }
                    if (g_xform.handle != TransformHandle::None && !mbdn) {
                        g_xform.handle = TransformHandle::None;
                    }

                    /* Click outside the transform rect (but on the sprite,
                       not on a handle and not on the chain icon) commits the
                       transform AND applies the paste — matches Photoshop's
                       "click anywhere outside the bbox to commit" behavior.
                       Without this, every paste would require an extra
                       Enter / Ctrl+T keystroke before the user could click
                       to drop it, because paste auto-enters transform now. */
                    if (!paste_controls_block &&
                        g_xform.handle == TransformHandle::None && !hovering &&
                        over_sprite && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        /* Skip if the click landed on the chain icon — that
                           click is consumed by the chain toggle above. */
                        bool on_chain = mouse.x >= ch1.x && mouse.x <= ch2.x &&
                                        mouse.y >= ch1.y && mouse.y <= ch2.y;
                        if (!on_chain) {
                            xform_commit();
                            apply_pasted_region();
                            g_pasted.active = false;
                            g_pasted.dragging = false;
                        }
                    }
                }

                if (!canvas_input_blocked && !g_xform.active) {
                    /* Start drag: click inside paste rect */
                    if (!paste_controls_block && hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_pasted.dragging = true;
                        g_pasted.drag_start_mx = mouse.x;
                        g_pasted.drag_start_my = mouse.y;
                        g_pasted.drag_start_px = g_pasted.paste_x;
                        g_pasted.drag_start_py = g_pasted.paste_y;
                    }

                    /* Drag to move */
                    if (g_pasted.dragging && mbdn) {
                        IMG *snap_img = (g_doc->ilselected >= 0)
                            ? get_img(g_doc->ilselected) : NULL;
                        CanvasContentBounds snap_bounds;
                        bool snap_to_content = false;
                        if (ImGui::GetIO().KeyShift && g_doc->ilselected >= 0) {
                            /* Cache the content bbox of the underlying sprite
                               on the first frame Shift is held during this
                               drag; recompute only on image change. */
                            if (!g_snap_bbox.valid || g_snap_bbox.img_idx != g_doc->ilselected) {
                                CanvasContentBounds bounds = CanvasFindOpaqueBounds(snap_img);
                                if (bounds.valid) {
                                    g_snap_bbox = {true, bounds.min_x, bounds.min_y,
                                                   bounds.max_x, bounds.max_y,
                                                   g_doc->ilselected};
                                }
                            }
                            if (g_snap_bbox.valid && snap_img) {
                                snap_bounds.valid = true;
                                snap_bounds.min_x = g_snap_bbox.min_x;
                                snap_bounds.min_y = g_snap_bbox.min_y;
                                snap_bounds.max_x = g_snap_bbox.max_x;
                                snap_bounds.max_y = g_snap_bbox.max_y;
                                snap_to_content = true;
                            }
                        } else {
                            g_snap_bbox.valid = false;
                        }

                        /* Passive centering guide: even without Shift, show a
                           magenta center line when the paste rect's center
                           lands exactly on the sprite's center axis. Lets the
                           user see "I'm centered" without engaging snap. */
                        CanvasPasteDragResult drag = CanvasResolvePasteDrag(
                            ImVec2(g_pasted.drag_start_mx,
                                   g_pasted.drag_start_my),
                            mouse, sx, sy,
                            g_pasted.drag_start_px,
                            g_pasted.drag_start_py,
                            pw, ph,
                            g_img_tex_w, g_img_tex_h,
                            snap_img ? snap_img->w : 0,
                            snap_img ? snap_img->h : 0,
                            snap_to_content, snap_bounds, snap_img != NULL);
                        g_snap_hit_x = drag.hit_x;
                        g_snap_hit_y = drag.hit_y;
                        g_snap_guide_x = drag.guide_x;
                        g_snap_guide_y = drag.guide_y;
                        g_pasted.paste_x = drag.x;
                        g_pasted.paste_y = drag.y;
                    }

                    /* Stop drag on release — keep floating */
                    if (g_pasted.dragging && !mbdn)
                        g_pasted.dragging = false;

                    /* Click outside paste rect (but on sprite) to confirm */
                    if (!paste_controls_block && !hovering && over_sprite && !g_pasted.dragging &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        apply_pasted_region();
                        g_pasted.active = false;
                    }
                }
            }
            if (rotate_buttons_visible)
                DrawCanvasRotateButtons(dl, rotate_button_min, rotate_button_max, rotate_button_hover_idx);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}


/* =========================================================
   Extracted canvas/pixel operations from imgui_overlay.cpp
   ========================================================= */

// Extracted from imgui_overlay.cpp: FloodFill
void FloodFill(IMG *img, int sx, int sy, unsigned char new_color)
{
    if (!img || !img->data_p || sx < 0 || sy < 0 || sx >= (int)img->w || sy >= (int)img->h)
        return;
    unsigned short stride = (unsigned short)((img->w + 3) & ~3);
    unsigned char *pixels = (unsigned char *)img->data_p;
    unsigned char old_color = pixels[sy * stride + sx];
    if (old_color == new_color) return;
    struct Pt { int x, y; };
    std::vector<Pt> stack; stack.reserve(4096);
    stack.push_back({sx, sy});
    while (!stack.empty()) {
        Pt p = stack.back(); stack.pop_back();
        if (p.x < 0 || p.x >= (int)img->w || p.y < 0 || p.y >= (int)img->h) continue;
        unsigned char *px = &pixels[p.y * stride + p.x];
        if (*px != old_color) continue;
        *px = new_color;
        stack.push_back({p.x + 1, p.y}); stack.push_back({p.x - 1, p.y});
        stack.push_back({p.x, p.y + 1}); stack.push_back({p.x, p.y - 1});
    }
}


// Extracted from imgui_overlay.cpp: PaintBucketFill
int PaintBucketFill(IMG *img, int sx, int sy, unsigned char new_color, int tolerance, bool contiguous)
{
    if (!img || !img->data_p || sx < 0 || sy < 0 || sx >= (int)img->w || sy >= (int)img->h)
        return 0;
    if (tolerance < 0) tolerance = 0;
    if (tolerance > 255) tolerance = 255;

    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pixels = (unsigned char *)img->data_p;
    unsigned char target = pixels[sy * stride + sx];
    if (target == new_color) return 0;

    auto in_range = [&](unsigned char v) {
        int d = (int)v - (int)target;
        if (d < 0) d = -d;
        return d <= tolerance;
    };

    int changed = 0;
    if (contiguous) {
        struct Pt { int x, y; };
        std::vector<Pt> stack;
        std::vector<unsigned char> seen((size_t)w * h, 0);
        stack.reserve(4096);
        stack.push_back({sx, sy});
        seen[sy * w + sx] = 1;

        while (!stack.empty()) {
            Pt p = stack.back();
            stack.pop_back();
            unsigned char *px = &pixels[p.y * stride + p.x];
            if (!in_range(*px)) continue;
            *px = new_color;
            changed++;

            const int dx[4] = {1, -1, 0, 0};
            const int dy[4] = {0, 0, 1, -1};
            for (int i = 0; i < 4; i++) {
                int nx = p.x + dx[i], ny = p.y + dy[i];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                size_t off = (size_t)ny * w + nx;
                if (seen[off]) continue;
                seen[off] = 1;
                if (in_range(pixels[ny * stride + nx]))
                    stack.push_back({nx, ny});
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                unsigned char *px = &pixels[y * stride + x];
                if (!in_range(*px)) continue;
                *px = new_color;
                changed++;
            }
        }
    }
    return changed;
}


// Extracted from imgui_overlay.cpp: SmartErase
void SmartErase(IMG *img, int sx, int sy, int tolerance, bool contiguous, bool defringe)
{
    if (!img || !img->data_p || sx < 0 || sy < 0 ||
        sx >= (int)img->w || sy >= (int)img->h) return;
    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pix = (unsigned char *)img->data_p;
    int target = pix[sy * stride + sx];
    if (target == 0) return; /* clicked on existing transparent */

    auto in_range = [&](unsigned char v) {
        int d = (int)v - target;
        if (d < 0) d = -d;
        return d <= tolerance;
    };

    /* Mark which pixels we'll erase, so defringe can scan against the
       original neighborhood before zeroing. Multiply in size_t so the
       computation can't overflow int for pathological sprite sizes. */
    std::vector<unsigned char> kill((size_t)w * h, 0);

    if (contiguous) {
        struct Pt { int x, y; };
        std::vector<Pt> stack; stack.reserve(4096);
        stack.push_back({sx, sy});
        kill[sy * w + sx] = 1;
        while (!stack.empty()) {
            Pt p = stack.back(); stack.pop_back();
            const int dx[] = {1, -1, 0, 0};
            const int dy[] = {0, 0, 1, -1};
            for (int i = 0; i < 4; i++) {
                int nx = p.x + dx[i], ny = p.y + dy[i];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                if (kill[ny * w + nx]) continue;
                if (!in_range(pix[ny * stride + nx])) continue;
                kill[ny * w + nx] = 1;
                stack.push_back({nx, ny});
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (in_range(pix[y * stride + x]))
                    kill[y * w + x] = 1;
            }
        }
    }

    /* Optionally compute defringe replacements before applying the kill. */
    std::vector<std::pair<int,unsigned char>> defringe_writes;
    if (defringe) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (kill[y * w + x]) continue;
                unsigned char self = pix[y * stride + x];
                if (self == 0) continue;
                /* Edge test: any 8-neighbor will be killed. */
                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; dy++) {
                    for (int dx = -1; dx <= 1 && !touches; dx++) {
                        if (!dx && !dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        if (kill[ny * w + nx]) touches = true;
                    }
                }
                if (!touches) continue;
                /* Average palette indices of safe (non-killed, non-zero,
                   not-itself-the-chroma) neighbors. This is a coarse proxy
                   for picking the nearest "skin/fabric" color in the
                   indexed palette — gives a much cleaner edge than just
                   leaving the blue-spill pixel alone. */
                int sum = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        if (kill[ny * w + nx]) continue;
                        unsigned char nv = pix[ny * stride + nx];
                        if (nv == 0) continue;
                        if (in_range(nv)) continue;
                        sum += nv; n++;
                    }
                }
                if (n > 0) {
                    defringe_writes.push_back({y * stride + x, (unsigned char)(sum / n)});
                }
            }
        }
    }

    /* Apply the kill, then the defringe overrides. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (kill[y * w + x]) pix[y * stride + x] = 0;
        }
    }
    for (auto &w_ : defringe_writes) pix[w_.first] = w_.second;
}


// Extracted from imgui_overlay.cpp: LikenessBBox
struct LikenessBBox {
    int x1, y1, x2, y2;
    int w, h;
    float anchor_u, anchor_v;
    bool anchor_ok;
};


// Extracted from imgui_overlay.cpp: LikenessSample
struct LikenessSample {
    float u, v;
    int x, y;
    unsigned char idx;
    int r, g, b, luma;
};


// Extracted from imgui_overlay.cpp: LikenessColor
struct LikenessColor {
    unsigned char idx;
    float r, g, b, luma;
    bool found;
};


// Extracted from imgui_overlay.cpp: LikenessOpaqueBBox
static bool LikenessOpaqueBBox(IMG *img, LikenessBBox *bbox)
{
    if (!img || !img->data_p || !bbox || img->w == 0 || img->h == 0)
        return false;

    int min_x = img->w, min_y = img->h, max_x = -1, max_y = -1;
    int stride = (img->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            if (pix[y * stride + x] == 0) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return false;

    bbox->x1 = min_x; bbox->y1 = min_y;
    bbox->x2 = max_x; bbox->y2 = max_y;
    bbox->w = max_x - min_x + 1;
    bbox->h = max_y - min_y + 1;

    float span_x = (bbox->w > 1) ? (float)(bbox->w - 1) : 1.0f;
    float span_y = (bbox->h > 1) ? (float)(bbox->h - 1) : 1.0f;
    bbox->anchor_u = ((float)(short)img->anix - (float)bbox->x1) / span_x;
    bbox->anchor_v = ((float)(short)img->aniy - (float)bbox->y1) / span_y;
    bbox->anchor_ok = bbox->anchor_u >= -0.25f && bbox->anchor_u <= 1.25f &&
                      bbox->anchor_v >= -0.25f && bbox->anchor_v <= 1.25f;
    return true;
}


// Extracted from imgui_overlay.cpp: LikenessWordLuma8
static int LikenessWordLuma8(unsigned short word)
{
    int r = (int)((word >> 10) & 0x1F) * 255 / 31;
    int g = (int)((word >>  5) & 0x1F) * 255 / 31;
    int b = (int)( word        & 0x1F) * 255 / 31;
    return (r * 54 + g * 183 + b * 19) >> 8;
}


// Extracted from imgui_overlay.cpp: LikenessWordRgb8
static void LikenessWordRgb8(unsigned short word, int *r, int *g, int *b)
{
    if (r) *r = (int)((word >> 10) & 0x1F) * 255 / 31;
    if (g) *g = (int)((word >>  5) & 0x1F) * 255 / 31;
    if (b) *b = (int)( word        & 0x1F) * 255 / 31;
}


// Extracted from imgui_overlay.cpp: LikenessBuildSamples
static void LikenessBuildSamples(IMG *src, PAL *src_pal,
                                 const LikenessBBox &bbox,
                                 std::vector<LikenessSample> &samples,
                                 bool used_slots[256],
                                 int *mean_luma)
{
    samples.clear();
    memset(used_slots, 0, sizeof(bool) * 256);
    if (mean_luma) *mean_luma = 128;
    if (!src || !src->data_p || !src_pal || !src_pal->data_p) return;

    int stride = (src->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)src->data_p;
    float span_x = (bbox.w > 1) ? (float)(bbox.w - 1) : 1.0f;
    float span_y = (bbox.h > 1) ? (float)(bbox.h - 1) : 1.0f;
    long long luma_sum = 0;

    samples.reserve((size_t)bbox.w * bbox.h / 2);
    for (int y = bbox.y1; y <= bbox.y2; y++) {
        for (int x = bbox.x1; x <= bbox.x2; x++) {
            unsigned char ci = pix[y * stride + x];
            if (ci == 0) continue;
            unsigned short word = pal_word_or_black(src_pal, ci);
            int r, g, b;
            LikenessWordRgb8(word, &r, &g, &b);
            int luma = LikenessWordLuma8(word);
            LikenessSample s = {};
            s.u = ((float)x - (float)bbox.x1) / span_x;
            s.v = ((float)y - (float)bbox.y1) / span_y;
            s.x = x;
            s.y = y;
            s.idx = ci;
            s.r = r; s.g = g; s.b = b; s.luma = luma;
            samples.push_back(s);
            used_slots[ci] = true;
            luma_sum += luma;
        }
    }
    if (mean_luma && !samples.empty())
        *mean_luma = (int)(luma_sum / (long long)samples.size());
}


// Extracted from imgui_overlay.cpp: LikenessMaskScore
static float LikenessMaskScore(IMG *target, const LikenessBBox &tb,
                               const std::vector<LikenessSample> &samples,
                               bool mirror)
{
    const int G = 32;
    bool src_occ[G * G] = {};
    for (const LikenessSample &s : samples) {
        float u = mirror ? (1.0f - s.u) : s.u;
        int bx = (int)(u * (G - 1) + 0.5f);
        int by = (int)(s.v * (G - 1) + 0.5f);
        if (bx < 0) bx = 0; if (bx >= G) bx = G - 1;
        if (by < 0) by = 0; if (by >= G) by = G - 1;
        src_occ[by * G + bx] = true;
    }

    int stride = (target->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)target->data_p;
    float span_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    float score = 0.0f;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            if (pix[y * stride + x] == 0) continue;
            int bx = (int)((((float)x - tb.x1) / span_x) * (G - 1) + 0.5f);
            int by = (int)((((float)y - tb.y1) / span_y) * (G - 1) + 0.5f);
            if (bx < 0) bx = 0; if (bx >= G) bx = G - 1;
            if (by < 0) by = 0; if (by >= G) by = G - 1;
            if (src_occ[by * G + bx]) {
                score += 2.0f;
            } else {
                bool has_near_source = false;
                for (int dy = -1; dy <= 1 && !has_near_source; dy++) {
                    for (int dx = -1; dx <= 1 && !has_near_source; dx++) {
                        int nx = bx + dx, ny = by + dy;
                        if (nx < 0 || nx >= G || ny < 0 || ny >= G) continue;
                        if (src_occ[ny * G + nx]) has_near_source = true;
                    }
                }
                if (has_near_source) score += 0.75f;
            }
        }
    }
    return score;
}


// Extracted from imgui_overlay.cpp: LikenessNearestUsedSlot
static int LikenessNearestUsedSlot(PAL *pal, const bool used_slots[256],
                                   int r, int g, int b)
{
    if (!pal || !pal->data_p) return 0;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    unsigned short desired = rgb_to_word15((unsigned char)r,
                                           (unsigned char)g,
                                           (unsigned char)b);
    int count = pal->numc;
    if (count > 256) count = 256;
    int best = 0;
    int best_dist = 0x7FFFFFFF;
    for (int i = 1; i < count; i++) {
        if (!used_slots[i]) continue;
        int dist = PaletteColorDistance5(desired, pal_word_or_black(pal, i));
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
            if (dist == 0) break;
        }
    }
    if (best > 0) return best;
    return FindNearestPaletteSlot(pal, desired);
}


// Extracted from imgui_overlay.cpp: LikenessSampleSourceColor
static bool LikenessSampleSourceColor(const std::vector<LikenessSample> &samples,
                                      const std::vector<std::vector<int>> &bins,
                                      float u, float v,
                                      LikenessColor *out)
{
    if (out) *out = {};
    if (samples.empty() || !out) return false;
    const int G = 32;
    const float radii[] = { 0.055f, 0.095f, 0.16f, 0.27f };
    for (float radius : radii) {
        float r2 = radius * radius;
        int bx0 = (int)((u - radius) * G);
        int bx1 = (int)((u + radius) * G);
        int by0 = (int)((v - radius) * G);
        int by1 = (int)((v + radius) * G);
        if (bx0 < 0) bx0 = 0; if (bx1 >= G) bx1 = G - 1;
        if (by0 < 0) by0 = 0; if (by1 >= G) by1 = G - 1;

        double wr = 0.0, wg = 0.0, wb = 0.0, wl = 0.0, wsum = 0.0;
        const LikenessSample *nearest = NULL;
        float nearest_d2 = 9999.0f;
        for (int by = by0; by <= by1; by++) {
            for (int bx = bx0; bx <= bx1; bx++) {
                const std::vector<int> &bucket = bins[by * G + bx];
                for (int si : bucket) {
                    const LikenessSample &s = samples[si];
                    float dx = (s.u - u) * 1.12f;
                    float dy = s.v - v;
                    float d2 = dx * dx + dy * dy;
                    if (d2 > r2) continue;
                    if (d2 < nearest_d2) {
                        nearest_d2 = d2;
                        nearest = &s;
                    }
                    double wt = 1.0 / (0.0009 + (double)d2);
                    wr += (double)s.r * wt;
                    wg += (double)s.g * wt;
                    wb += (double)s.b * wt;
                    wl += (double)s.luma * wt;
                    wsum += wt;
                }
            }
        }
        if (wsum > 0.0) {
            out->r = (float)(wr / wsum);
            out->g = (float)(wg / wsum);
            out->b = (float)(wb / wsum);
            out->luma = (float)(wl / wsum);
            if (nearest) {
                out->r = out->r * 0.65f + nearest->r * 0.35f;
                out->g = out->g * 0.65f + nearest->g * 0.35f;
                out->b = out->b * 0.65f + nearest->b * 0.35f;
                out->luma = out->luma * 0.65f + nearest->luma * 0.35f;
                out->idx = nearest->idx;
            }
            out->found = true;
            return true;
        }
    }

    const LikenessSample *nearest = &samples[0];
    float best_d2 = 9999.0f;
    for (const LikenessSample &s : samples) {
        float dx = (s.u - u) * 1.12f;
        float dy = s.v - v;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            nearest = &s;
        }
    }
    out->r = (float)nearest->r;
    out->g = (float)nearest->g;
    out->b = (float)nearest->b;
    out->luma = (float)nearest->luma;
    out->idx = nearest->idx;
    out->found = true;
    return true;
}


// Extracted from imgui_overlay.cpp: LikenessPartBox
struct LikenessPartBox {
    int x1, y1, x2, y2;
    int count;
    float cx, cy;
    bool valid;
};


// Extracted from imgui_overlay.cpp: LikenessClampInt
static int LikenessClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}


// Extracted from imgui_overlay.cpp: LikenessAbsInt
static int LikenessAbsInt(int v)
{
    return v < 0 ? -v : v;
}


// Extracted from imgui_overlay.cpp: LikenessPaletteRgb
static void LikenessPaletteRgb(PAL *pal, unsigned char ci,
                               int *r, int *g, int *b, int *luma)
{
    unsigned short word = pal_word_or_black(pal, ci);
    int rr, gg, bb;
    LikenessWordRgb8(word, &rr, &gg, &bb);
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
    if (luma) *luma = LikenessWordLuma8(word);
}


// Extracted from imgui_overlay.cpp: LikenessIsGoldPixel
static bool LikenessIsGoldPixel(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return false;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    return luma >= 48 && luma <= 212 &&
           r >= 82 && g >= 50 &&
           r > b + 34 && g > b + 16 &&
           r >= g - 28;
}


// Extracted from imgui_overlay.cpp: LikenessIsHeadPixel
static bool LikenessIsHeadPixel(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return false;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;

    bool pale_hair = luma >= 108 && chroma <= 88;
    bool skin = luma >= 78 &&
                r >= g - 18 && g >= b - 34 &&
                r > b + 10 && chroma <= 132;
    bool hot_gold = LikenessIsGoldPixel(pal, ci) && b < 72 && luma < 150;
    return (pale_hair || skin) && !hot_gold;
}


// Extracted from imgui_overlay.cpp: LikenessTransparentNeighborCount
static int LikenessTransparentNeighborCount(const unsigned char *pix,
                                            int w, int h, int stride,
                                            int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx;
            int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h ||
                pix[(size_t)ny * stride + nx] == 0)
                n++;
        }
    }
    return n;
}


// Extracted from imgui_overlay.cpp: LikenessHasLocalContrast
static bool LikenessHasLocalContrast(const unsigned char *pix,
                                     int w, int h, int stride,
                                     int x, int y, PAL *pal)
{
    if (!pix || !pal) return false;
    unsigned char ci = pix[(size_t)y * stride + x];
    if (ci == 0) return false;
    unsigned short self = pal_word_or_black(pal, ci);
    int self_luma = LikenessWordLuma8(self);
    int max_dist = 0;
    int max_luma_delta = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx;
            int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            unsigned char ni = pix[(size_t)ny * stride + nx];
            if (ni == 0 || ni == ci) continue;
            unsigned short nw = pal_word_or_black(pal, ni);
            int dist = PaletteColorDistance5(self, nw);
            int ld = self_luma - LikenessWordLuma8(nw);
            if (ld < 0) ld = -ld;
            if (dist > max_dist) max_dist = dist;
            if (ld > max_luma_delta) max_luma_delta = ld;
        }
    }
    return max_dist >= 86 || max_luma_delta >= 34;
}


// Extracted from imgui_overlay.cpp: LikenessFindHeadPart
static bool LikenessFindHeadPart(const unsigned char *pix,
                                 int w, int h, int stride,
                                 PAL *pal,
                                 const LikenessBBox &bbox,
                                 bool prefer_top,
                                 LikenessPartBox *out)
{
    if (out) *out = {};
    if (!pix || !pal || !out) return false;

    std::vector<unsigned char> visited((size_t)stride * h, 0);
    std::vector<int> queue;
    float best_score = -1.0f;
    float span_y = (bbox.h > 1) ? (float)(bbox.h - 1) : 1.0f;

    for (int y = bbox.y1; y <= bbox.y2; y++) {
        for (int x = bbox.x1; x <= bbox.x2; x++) {
            size_t off = (size_t)y * stride + x;
            if (visited[off] || !LikenessIsHeadPixel(pal, pix[off])) continue;

            int min_x = x, min_y = y, max_x = x, max_y = y;
            int count = 0;
            long long sum_x = 0, sum_y = 0;
            queue.clear();
            queue.push_back(y * stride + x);
            visited[off] = 1;

            for (size_t qi = 0; qi < queue.size(); qi++) {
                int qoff = queue[qi];
                int cy = qoff / stride;
                int cx = qoff - cy * stride;
                count++;
                sum_x += cx;
                sum_y += cy;
                if (cx < min_x) min_x = cx;
                if (cy < min_y) min_y = cy;
                if (cx > max_x) max_x = cx;
                if (cy > max_y) max_y = cy;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = cx + dx;
                        int ny = cy + dy;
                        if (nx < bbox.x1 || nx > bbox.x2 ||
                            ny < bbox.y1 || ny > bbox.y2)
                            continue;
                        size_t noff = (size_t)ny * stride + nx;
                        if (visited[noff] || !LikenessIsHeadPixel(pal, pix[noff]))
                            continue;
                        visited[noff] = 1;
                        queue.push_back(ny * stride + nx);
                    }
                }
            }

            if (count < 3) continue;
            int bw = max_x - min_x + 1;
            int bh = max_y - min_y + 1;
            float cx = (float)sum_x / (float)count;
            float cy = (float)sum_y / (float)count;
            float norm_y = (cy - (float)bbox.y1) / span_y;
            float density = (float)count / (float)(bw * bh);
            float score = (float)count * (1.0f + density * 0.8f);
            if (prefer_top) score *= 1.0f + (1.0f - norm_y) * 1.15f;
            if (bw > bbox.w * 7 / 10 || bh > bbox.h * 5 / 10) score *= 0.45f;
            if (prefer_top && norm_y > 0.70f) score *= 0.25f;

            if (score > best_score) {
                best_score = score;
                out->x1 = min_x; out->y1 = min_y;
                out->x2 = max_x; out->y2 = max_y;
                out->count = count;
                out->cx = cx; out->cy = cy;
                out->valid = true;
            }
        }
    }
    return out->valid;
}


// Extracted from imgui_overlay.cpp: LikenessExpandPartBox
static void LikenessExpandPartBox(LikenessPartBox *box,
                                  int w, int h, int px, int py)
{
    if (!box || !box->valid) return;
    box->x1 = LikenessClampInt(box->x1 - px, 0, w - 1);
    box->x2 = LikenessClampInt(box->x2 + px, 0, w - 1);
    box->y1 = LikenessClampInt(box->y1 - py, 0, h - 1);
    box->y2 = LikenessClampInt(box->y2 + py, 0, h - 1);
}


// Extracted from imgui_overlay.cpp: LikenessPointInPartBox
static bool LikenessPointInPartBox(const LikenessPartBox &box, int x, int y)
{
    return box.valid &&
           x >= box.x1 && x <= box.x2 &&
           y >= box.y1 && y <= box.y2;
}


// Extracted from imgui_overlay.cpp: LikenessNearestOpaqueInBox
char LikenessNearestOpaqueInBox(const unsigned char *pix,
                                                int w, int h, int stride,
                                                const LikenessPartBox &box,
                                                int x, int y)
{
    if (!pix || !box.valid) return 0;
    x = LikenessClampInt(x, box.x1, box.x2);
    y = LikenessClampInt(y, box.y1, box.y2);
    unsigned char direct = pix[(size_t)y * stride + x];
    if (direct != 0) return direct;

    int max_radius = box.x2 - box.x1;
    int bh = box.y2 - box.y1;
    if (bh > max_radius) max_radius = bh;
    if (max_radius > 4) max_radius = 4;
    for (int r = 1; r <= max_radius; r++) {
        unsigned char best = 0;
        int best_d2 = 0x7FFFFFFF;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int sx = x + dx;
                int sy = y + dy;
                if (sx < box.x1 || sx > box.x2 ||
                    sy < box.y1 || sy > box.y2 ||
                    sx < 0 || sy < 0 || sx >= w || sy >= h)
                    continue;
                unsigned char ci = pix[(size_t)sy * stride + sx];
                if (ci == 0) continue;
                int d2 = dx * dx + dy * dy;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = ci;
                }
            }
        }
        if (best != 0) return best;
    }
    return 0;
}


// Extracted from imgui_overlay.cpp: LikenessProjectPartBox
static LikenessPartBox LikenessProjectPartBox(const LikenessPartBox &src_box,
                                              const LikenessBBox &sb,
                                              const LikenessBBox &tb,
                                              bool mirror,
                                              int target_w, int target_h)
{
    LikenessPartBox out = {};
    if (!src_box.valid) return out;
    float sxspan = (sb.w > 1) ? (float)(sb.w - 1) : 1.0f;
    float syspan = (sb.h > 1) ? (float)(sb.h - 1) : 1.0f;
    float txspan = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float tyspan = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;

    float u1 = ((float)src_box.x1 - (float)sb.x1) / sxspan;
    float u2 = ((float)src_box.x2 - (float)sb.x1) / sxspan;
    if (mirror) {
        float mu1 = 1.0f - u2;
        float mu2 = 1.0f - u1;
        u1 = mu1; u2 = mu2;
    }
    float v1 = ((float)src_box.y1 - (float)sb.y1) / syspan;
    float v2 = ((float)src_box.y2 - (float)sb.y1) / syspan;

    if (sb.anchor_ok && tb.anchor_ok) {
        float src_anchor_u = mirror ? (1.0f - sb.anchor_u) : sb.anchor_u;
        float du = 0.28f * (src_anchor_u - tb.anchor_u);
        float dv = 0.24f * (sb.anchor_v - tb.anchor_v);
        u1 -= du; u2 -= du;
        v1 -= dv; v2 -= dv;
    }

    out.x1 = LikenessClampInt((int)(tb.x1 + u1 * txspan + 0.5f), 0, target_w - 1);
    out.x2 = LikenessClampInt((int)(tb.x1 + u2 * txspan + 0.5f), 0, target_w - 1);
    out.y1 = LikenessClampInt((int)(tb.y1 + v1 * tyspan + 0.5f), 0, target_h - 1);
    out.y2 = LikenessClampInt((int)(tb.y1 + v2 * tyspan + 0.5f), 0, target_h - 1);
    if (out.x1 > out.x2) { int t = out.x1; out.x1 = out.x2; out.x2 = t; }
    if (out.y1 > out.y2) { int t = out.y1; out.y1 = out.y2; out.y2 = t; }
    out.count = src_box.count;
    out.cx = (out.x1 + out.x2) * 0.5f;
    out.cy = (out.y1 + out.y2) * 0.5f;
    out.valid = out.x2 >= out.x1 && out.y2 >= out.y1;
    return out;
}


// Extracted from imgui_overlay.cpp: LikenessProjectSourceToTarget
static bool LikenessProjectSourceToTarget(float raw_u, float raw_v,
                                          const LikenessBBox &sb,
                                          const LikenessBBox &tb,
                                          bool mirror,
                                          float *out_u, float *out_v)
{
    float u = mirror ? (1.0f - raw_u) : raw_u;
    float v = raw_v;
    if (sb.anchor_ok && tb.anchor_ok) {
        float src_anchor_u = mirror ? (1.0f - sb.anchor_u) : sb.anchor_u;
        u -= 0.28f * (src_anchor_u - tb.anchor_u);
        v -= 0.24f * (sb.anchor_v - tb.anchor_v);
    }
    if (u < -0.08f || u > 1.08f || v < -0.08f || v > 1.08f)
        return false;
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    if (out_u) *out_u = u;
    if (out_v) *out_v = v;
    return true;
}


// Extracted from imgui_overlay.cpp: LikenessMaskMismatch
static int LikenessMaskMismatch(const unsigned char *src_pix,
                                int sw, int sh, int ss, int sx, int sy,
                                const unsigned char *dst_pix,
                                int dw, int dh, int ds, int dx0, int dy0)
{
    int mismatch = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            bool so = false;
            bool do_ = false;
            int ax = sx + dx, ay = sy + dy;
            int bx = dx0 + dx, by = dy0 + dy;
            if (ax >= 0 && ay >= 0 && ax < sw && ay < sh)
                so = src_pix[(size_t)ay * ss + ax] != 0;
            if (bx >= 0 && by >= 0 && bx < dw && by < dh)
                do_ = dst_pix[(size_t)by * ds + bx] != 0;
            if (so != do_) mismatch++;
        }
    }
    return mismatch;
}


// Extracted from imgui_overlay.cpp: LikenessApplyHeadPatch
static int LikenessApplyHeadPatch(const unsigned char *source_pix,
                                  int sw, int sh, int ss,
                                  const unsigned char *target_original,
                                  int tw, int th, int ts,
                                  unsigned char *target_pix,
                                  PAL *source_pal, PAL *target_pal,
                                  const LikenessBBox &sb,
                                  const LikenessBBox &tb,
                                  bool mirror,
                                  std::vector<unsigned char> &patch_mask,
                                  LikenessPartBox *source_head_out)
{
    LikenessPartBox source_head = {};
    LikenessPartBox target_head = {};
    if (!LikenessFindHeadPart(source_pix, sw, sh, ss, source_pal, sb,
                              true, &source_head))
        return 0;

    if (!LikenessFindHeadPart(target_original, tw, th, ts, target_pal, tb,
                              false, &target_head))
        target_head = LikenessProjectPartBox(source_head, sb, tb, mirror, tw, th);

    if (!target_head.valid) return 0;
    if (source_head_out) *source_head_out = source_head;

    LikenessExpandPartBox(&source_head, sw, sh, 1, 1);
    LikenessExpandPartBox(&target_head, tw, th, 1, 1);

    float source_span_x = (source_head.x2 > source_head.x1)
        ? (float)(source_head.x2 - source_head.x1) : 1.0f;
    float source_span_y = (source_head.y2 > source_head.y1)
        ? (float)(source_head.y2 - source_head.y1) : 1.0f;
    float target_span_x = (target_head.x2 > target_head.x1)
        ? (float)(target_head.x2 - target_head.x1) : 1.0f;
    float target_span_y = (target_head.y2 > target_head.y1)
        ? (float)(target_head.y2 - target_head.y1) : 1.0f;

    int changed = 0;
    for (int y = target_head.y1; y <= target_head.y2; y++) {
        for (int x = target_head.x1; x <= target_head.x2; x++) {
            int off = y * ts + x;
            if (target_original[(size_t)off] == 0) continue;

            float rx = ((float)x - (float)target_head.x1) / target_span_x;
            float ry = ((float)y - (float)target_head.y1) / target_span_y;
            float sx_f = mirror
                ? (float)source_head.x2 - rx * source_span_x
                : (float)source_head.x1 + rx * source_span_x;
            float sy_f = (float)source_head.y1 + ry * source_span_y;
            int sx = LikenessClampInt((int)(sx_f + 0.5f), 0, sw - 1);
            int sy = LikenessClampInt((int)(sy_f + 0.5f), 0, sh - 1);
            unsigned char ci = LikenessNearestOpaqueInBox(source_pix, sw, sh, ss,
                                                          source_head, sx, sy);
            if (ci == 0) continue;
            if (target_pix[off] != ci) {
                target_pix[off] = ci;
                changed++;
            }
            patch_mask[(size_t)off] = 1;
        }
    }
    return changed;
}


// Extracted from imgui_overlay.cpp: LikenessApplyDetailPatches
static int LikenessApplyDetailPatches(const unsigned char *source_pix,
                                      int sw, int sh, int ss,
                                      const unsigned char *target_original,
                                      int tw, int th, int ts,
                                      unsigned char *target_pix,
                                      PAL *source_pal, PAL *target_pal,
                                      const LikenessBBox &sb,
                                      const LikenessBBox &tb,
                                      bool mirror,
                                      const LikenessPartBox &source_head,
                                      std::vector<unsigned char> &patch_mask)
{
    if (!source_pix || !target_original || !target_pix ||
        !source_pal || !target_pal)
        return 0;

    float sspan_x = (sb.w > 1) ? (float)(sb.w - 1) : 1.0f;
    float sspan_y = (sb.h > 1) ? (float)(sb.h - 1) : 1.0f;
    float tspan_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float tspan_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int search_radius = 2 + ((tb.w > tb.h ? tb.w : tb.h) / 46);
    if (search_radius < 2) search_radius = 2;
    if (search_radius > 5) search_radius = 5;

    std::vector<int> best_score((size_t)ts * th, 0x7FFFFFFF);
    int changed = 0;

    for (int sy = sb.y1; sy <= sb.y2; sy++) {
        for (int sx = sb.x1; sx <= sb.x2; sx++) {
            int soff = sy * ss + sx;
            unsigned char ci = source_pix[(size_t)soff];
            if (ci == 0) continue;
            if (LikenessPointInPartBox(source_head, sx, sy)) continue;

            float raw_u = ((float)sx - (float)sb.x1) / sspan_x;
            float raw_v = ((float)sy - (float)sb.y1) / sspan_y;
            bool gold = LikenessIsGoldPixel(source_pal, ci);
            bool headish = LikenessIsHeadPixel(source_pal, ci);
            bool contrast = LikenessHasLocalContrast(source_pix, sw, sh, ss,
                                                     sx, sy, source_pal);
            bool edge = LikenessTransparentNeighborCount(source_pix, sw, sh, ss,
                                                         sx, sy) > 0;
            if (!(gold && (raw_v < 0.80f || contrast || edge)) &&
                !(headish && contrast && raw_v < 0.70f) &&
                !(contrast && raw_v < 0.72f && edge))
                continue;

            float tu, tv;
            if (!LikenessProjectSourceToTarget(raw_u, raw_v, sb, tb, mirror,
                                               &tu, &tv))
                continue;
            int tx0 = LikenessClampInt((int)(tb.x1 + tu * tspan_x + 0.5f),
                                       0, tw - 1);
            int ty0 = LikenessClampInt((int)(tb.y1 + tv * tspan_y + 0.5f),
                                       0, th - 1);

            int best_off = -1;
            int best = 0x7FFFFFFF;
            for (int dy = -search_radius; dy <= search_radius; dy++) {
                for (int dx = -search_radius; dx <= search_radius; dx++) {
                    int tx = tx0 + dx;
                    int ty = ty0 + dy;
                    if (tx < 0 || ty < 0 || tx >= tw || ty >= th) continue;
                    int toff = ty * ts + tx;
                    if (target_original[(size_t)toff] == 0) continue;
                    if (patch_mask[(size_t)toff] == 1) continue;

                    int dist2 = dx * dx + dy * dy;
                    int mismatch = LikenessMaskMismatch(source_pix, sw, sh, ss,
                                                        sx, sy,
                                                        target_original,
                                                        tw, th, ts, tx, ty);
                    unsigned char old_ci = target_original[(size_t)toff];
                    bool target_warm = LikenessIsGoldPixel(target_pal, old_ci) ||
                                       LikenessIsHeadPixel(target_pal, old_ci);
                    int target_edge = LikenessTransparentNeighborCount(
                        target_original, tw, th, ts, tx, ty);

                    int score = dist2 * 7 + mismatch * 8;
                    if (gold && target_warm) score -= 10;
                    else if (gold) score += 4;
                    if (contrast) score -= 4;
                    if (target_edge > 0) score -= 3;

                    if (score < best) {
                        best = score;
                        best_off = toff;
                    }
                }
            }

            if (best_off < 0 || best > 70 || best >= best_score[(size_t)best_off])
                continue;
            best_score[(size_t)best_off] = best;
            if (target_pix[best_off] != ci) {
                target_pix[best_off] = ci;
                changed++;
            }
            patch_mask[(size_t)best_off] = 2;
        }
    }
    return changed;
}


// Extracted from imgui_overlay.cpp: LikenessBlendPatchEdges
static int LikenessBlendPatchEdges(unsigned char *target_pix,
                                   const unsigned char *target_original,
                                   int w, int h, int stride,
                                   PAL *source_pal,
                                   const bool used_slots[256],
                                   const std::vector<unsigned char> &patch_mask)
{
    if (!target_pix || !target_original || !source_pal || !used_slots)
        return 0;

    struct BlendWrite { int off; unsigned char ci; };
    std::vector<BlendWrite> writes;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int off = y * stride + x;
            if (target_original[(size_t)off] == 0 || patch_mask[(size_t)off])
                continue;
            unsigned char old_ci = target_pix[off];
            if (old_ci == 0) continue;

            int patch_neighbors = 0;
            int rsum = 0, gsum = 0, bsum = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    int noff = ny * stride + nx;
                    if (!patch_mask[(size_t)noff]) continue;
                    unsigned char pci = target_pix[noff];
                    if (pci == 0) continue;
                    int pr, pg, pb;
                    LikenessPaletteRgb(source_pal, pci, &pr, &pg, &pb, NULL);
                    rsum += pr; gsum += pg; bsum += pb;
                    patch_neighbors++;
                }
            }
            if (patch_neighbors < 2) continue;

            int sr, sg, sbc;
            LikenessPaletteRgb(source_pal, old_ci, &sr, &sg, &sbc, NULL);
            int ar = rsum / patch_neighbors;
            int ag = gsum / patch_neighbors;
            int ab = bsum / patch_neighbors;
            int mix = patch_neighbors >= 4 ? 112 : 78; /* 0..256 */
            int nr = (sr * (256 - mix) + ar * mix) >> 8;
            int ng = (sg * (256 - mix) + ag * mix) >> 8;
            int nb = (sbc * (256 - mix) + ab * mix) >> 8;
            int ci = LikenessNearestUsedSlot(source_pal, used_slots, nr, ng, nb);
            if (ci > 0 && ci != old_ci)
                writes.push_back({off, (unsigned char)ci});
        }
    }

    int changed = 0;
    for (const BlendWrite &wr : writes) {
        if (target_pix[wr.off] != wr.ci) {
            target_pix[wr.off] = wr.ci;
            changed++;
        }
    }
    return changed;
}


// Extracted from imgui_overlay.cpp: LikenessMaterial
enum LikenessMaterial {
    LK_MATERIAL_ANY = 0,
    LK_MATERIAL_HEAD,
    LK_MATERIAL_SKIN,
    LK_MATERIAL_BLUE,
    LK_MATERIAL_LOWER,
    LK_MATERIAL_BLACK
};


// Extracted from imgui_overlay.cpp: LikenessActorStats
struct LikenessActorStats {
    int opaque;
    int remapped;
    int accents;
    int cleaned;
    int edge_darkened;
    int head_pixels;
    int skin_pixels;
};


// Extracted from imgui_overlay.cpp: LikenessClampByte
static int LikenessClampByte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}


// Extracted from imgui_overlay.cpp: LikenessRgbLuma8
static int LikenessRgbLuma8(int r, int g, int b)
{
    return (r * 54 + g * 183 + b * 19) >> 8;
}


// Extracted from imgui_overlay.cpp: LikenessIsBadGreenRgb
static bool LikenessIsBadGreenRgb(int r, int g, int b, int luma)
{
    (void)luma;
    return g > 65 && g > r + 7 && g > b + 18 &&
           (r < 105 || b < 55);
}


// Extracted from imgui_overlay.cpp: LikenessIsBadGreenPixel
static bool LikenessIsBadGreenPixel(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return false;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    return LikenessIsBadGreenRgb(r, g, b, luma);
}


// Extracted from imgui_overlay.cpp: LikenessIsBlackRgb
static bool LikenessIsBlackRgb(int r, int g, int b, int luma)
{
    return luma <= 34 || (r < 44 && g < 44 && b < 54);
}


// Extracted from imgui_overlay.cpp: LikenessIsBlueRgb
static bool LikenessIsBlueRgb(int r, int g, int b, int luma)
{
    if (LikenessIsBadGreenRgb(r, g, b, luma)) return false;
    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;
    return luma >= 14 && luma <= 184 &&
           b >= r + 4 && b >= g - 12 &&
           (b >= 46 || chroma >= 26);
}


// Extracted from imgui_overlay.cpp: LikenessIsLowerRgb
static bool LikenessIsLowerRgb(int r, int g, int b, int luma)
{
    if (LikenessIsBadGreenRgb(r, g, b, luma)) return false;
    if (luma < 28 || luma > 214) return false;
    return r >= 54 && g >= 34 &&
           r > b + 22 && g > b + 7 &&
           r >= g - 38;
}


// Extracted from imgui_overlay.cpp: LikenessIsSkinRgb
static bool LikenessIsSkinRgb(int r, int g, int b, int luma)
{
    if (LikenessIsBadGreenRgb(r, g, b, luma) ||
        LikenessIsBlueRgb(r, g, b, luma))
        return false;

    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;
    bool warm_skin = luma >= 58 &&
                     r >= g - 28 && g >= b - 42 &&
                     r > b + 7 && chroma <= 140;
    bool pale_hair = luma >= 102 &&
                     chroma <= 86 &&
                     r >= b - 18 && g >= b - 24;
    bool goldish = LikenessIsLowerRgb(r, g, b, luma) &&
                   b < 96 && r > b + 34 && g > b + 18;
    return (warm_skin || pale_hair) && !goldish;
}


// Extracted from imgui_overlay.cpp: LikenessIsHeadRgb
static bool LikenessIsHeadRgb(int r, int g, int b, int luma)
{
    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;
    bool grey_hair = luma >= 94 && chroma <= 78 &&
                     !LikenessIsBlueRgb(r, g, b, luma) &&
                     !LikenessIsLowerRgb(r, g, b, luma);
    return LikenessIsSkinRgb(r, g, b, luma) || grey_hair;
}


// Extracted from imgui_overlay.cpp: LikenessPaletteSlotMatchesMaterial
static bool LikenessPaletteSlotMatchesMaterial(PAL *pal, unsigned char ci,
                                               LikenessMaterial material)
{
    if (!pal || ci == 0) return false;
    if (material == LK_MATERIAL_ANY) return true;

    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    switch (material) {
    case LK_MATERIAL_HEAD:
        return LikenessIsHeadPixel(pal, ci) || LikenessIsHeadRgb(r, g, b, luma);
    case LK_MATERIAL_SKIN:
        return LikenessIsHeadRgb(r, g, b, luma) ||
               LikenessIsSkinRgb(r, g, b, luma);
    case LK_MATERIAL_BLUE:
        return LikenessIsBlueRgb(r, g, b, luma) ||
               (LikenessIsBlackRgb(r, g, b, luma) && b >= r - 4);
    case LK_MATERIAL_LOWER:
        return LikenessIsGoldPixel(pal, ci) ||
               LikenessIsLowerRgb(r, g, b, luma);
    case LK_MATERIAL_BLACK:
        return LikenessIsBlackRgb(r, g, b, luma);
    default:
        return true;
    }
}


// Extracted from imgui_overlay.cpp: LikenessClassifyPaletteSlot
static LikenessMaterial LikenessClassifyPaletteSlot(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return LK_MATERIAL_ANY;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    if (LikenessIsBlackRgb(r, g, b, luma)) return LK_MATERIAL_BLACK;
    if (LikenessPaletteSlotMatchesMaterial(pal, ci, LK_MATERIAL_SKIN))
        return LK_MATERIAL_SKIN;
    if (LikenessPaletteSlotMatchesMaterial(pal, ci, LK_MATERIAL_BLUE))
        return LK_MATERIAL_BLUE;
    if (LikenessPaletteSlotMatchesMaterial(pal, ci, LK_MATERIAL_LOWER))
        return LK_MATERIAL_LOWER;
    return LK_MATERIAL_ANY;
}


// Extracted from imgui_overlay.cpp: LikenessNearestMaterialSlot
static int LikenessNearestMaterialSlot(PAL *pal, const bool used_slots[256],
                                       LikenessMaterial material,
                                       int r, int g, int b,
                                       int prefer_r = -1,
                                       int prefer_g = -1,
                                       int prefer_b = -1,
                                       bool forbid_bad_green = false)
{
    if (!pal || !pal->data_p || !used_slots) return 0;
    r = LikenessClampByte(r);
    g = LikenessClampByte(g);
    b = LikenessClampByte(b);
    int want_luma = LikenessRgbLuma8(r, g, b);
    int prefer_luma = (prefer_r >= 0 && prefer_g >= 0 && prefer_b >= 0)
        ? LikenessRgbLuma8(prefer_r, prefer_g, prefer_b)
        : -1;

    int count = pal->numc;
    if (count > 256) count = 256;
    int pass_count = forbid_bad_green ? 2 : 4;
    for (int pass = 0; pass < pass_count; pass++) {
        bool require_material = (pass == 0 || pass == 2) &&
                                material != LK_MATERIAL_ANY;
        bool avoid_green = forbid_bad_green || pass < 2;
        int best = 0;
        double best_score = DBL_MAX;
        for (int i = 1; i < count; i++) {
            if (!used_slots[i]) continue;
            int pr, pg, pb, pl;
            LikenessPaletteRgb(pal, (unsigned char)i, &pr, &pg, &pb, &pl);
            bool bad_green = LikenessIsBadGreenRgb(pr, pg, pb, pl);
            if (avoid_green && bad_green) continue;
            bool material_match =
                LikenessPaletteSlotMatchesMaterial(pal, (unsigned char)i, material);
            if (require_material && !material_match) continue;

            int dr = pr - r, dg = pg - g, db = pb - b;
            int dl = pl - want_luma; if (dl < 0) dl = -dl;
            double score = (double)dl * 6.0 +
                           (double)(dr * dr + dg * dg + db * db) * 0.020;
            if (prefer_luma >= 0) {
                int pdr = pr - prefer_r, pdg = pg - prefer_g, pdb = pb - prefer_b;
                int pdl = pl - prefer_luma; if (pdl < 0) pdl = -pdl;
                score += (double)(pdr * pdr + pdg * pdg + pdb * pdb) * 0.010;
                score += (double)pdl * 0.75;
            }
            if (!require_material && material != LK_MATERIAL_ANY && !material_match)
                score += 160.0;
            if (material == LK_MATERIAL_BLACK)
                score += (double)pl * 2.5;
            if (bad_green)
                score += 9000.0;

            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best > 0) return best;
    }

    if (forbid_bad_green) return 0;
    return LikenessNearestUsedSlot(pal, used_slots, r, g, b);
}


// Extracted from imgui_overlay.cpp: LikenessSourceSampleMatches
static bool LikenessSourceSampleMatches(const LikenessSample &s,
                                        PAL *source_pal,
                                        LikenessMaterial material,
                                        const LikenessPartBox *source_head,
                                        bool strict_head)
{
    if (s.idx == 0) return false;
    if (material != LK_MATERIAL_ANY &&
        LikenessIsBadGreenRgb(s.r, s.g, s.b, s.luma))
        return false;

    if (material == LK_MATERIAL_HEAD &&
        source_head && source_head->valid && strict_head) {
        return LikenessPointInPartBox(*source_head, s.x, s.y) &&
               (LikenessIsHeadRgb(s.r, s.g, s.b, s.luma) ||
                LikenessPaletteSlotMatchesMaterial(source_pal, s.idx,
                                                   LK_MATERIAL_SKIN));
    }

    return LikenessPaletteSlotMatchesMaterial(source_pal, s.idx, material);
}


// Extracted from imgui_overlay.cpp: LikenessSampleMaterialSource
static bool LikenessSampleMaterialSource(const std::vector<LikenessSample> &samples,
                                         const std::vector<std::vector<int>> &bins,
                                         PAL *source_pal,
                                         LikenessMaterial material,
                                         const LikenessPartBox *source_head,
                                         float u, float v,
                                         LikenessColor *out,
                                         const LikenessSample **nearest_out)
{
    if (out) *out = {};
    if (nearest_out) *nearest_out = NULL;
    if (samples.empty() || !out) return false;

    const int G = 32;
    const float radii[] = { 0.052f, 0.088f, 0.145f, 0.24f, 0.39f, 0.66f };
    int attempts = (material == LK_MATERIAL_HEAD &&
                    source_head && source_head->valid) ? 2 : 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        bool strict_head = attempt == 0;
        for (float radius : radii) {
            float r2 = radius * radius;
            int bx0 = (int)((u - radius) * G);
            int bx1 = (int)((u + radius) * G);
            int by0 = (int)((v - radius) * G);
            int by1 = (int)((v + radius) * G);
            if (bx0 < 0) bx0 = 0; if (bx1 >= G) bx1 = G - 1;
            if (by0 < 0) by0 = 0; if (by1 >= G) by1 = G - 1;

            double wr = 0.0, wg = 0.0, wb = 0.0, wl = 0.0, wsum = 0.0;
            const LikenessSample *nearest = NULL;
            float nearest_d2 = 9999.0f;
            for (int by = by0; by <= by1; by++) {
                for (int bx = bx0; bx <= bx1; bx++) {
                    const std::vector<int> &bucket = bins[by * G + bx];
                    for (int si : bucket) {
                        const LikenessSample &s = samples[si];
                        if (!LikenessSourceSampleMatches(s, source_pal, material,
                                                         source_head, strict_head))
                            continue;
                        float dx = (s.u - u) * 1.10f;
                        float dy = s.v - v;
                        float d2 = dx * dx + dy * dy;
                        if (d2 > r2) continue;
                        if (d2 < nearest_d2) {
                            nearest_d2 = d2;
                            nearest = &s;
                        }
                        double wt = 1.0 / (0.0008 + (double)d2);
                        wr += (double)s.r * wt;
                        wg += (double)s.g * wt;
                        wb += (double)s.b * wt;
                        wl += (double)s.luma * wt;
                        wsum += wt;
                    }
                }
            }
            if (wsum > 0.0) {
                out->r = (float)(wr / wsum);
                out->g = (float)(wg / wsum);
                out->b = (float)(wb / wsum);
                out->luma = (float)(wl / wsum);
                if (nearest) {
                    out->r = out->r * 0.56f + nearest->r * 0.44f;
                    out->g = out->g * 0.56f + nearest->g * 0.44f;
                    out->b = out->b * 0.56f + nearest->b * 0.44f;
                    out->luma = out->luma * 0.56f + nearest->luma * 0.44f;
                    out->idx = nearest->idx;
                }
                out->found = true;
                if (nearest_out) *nearest_out = nearest;
                return true;
            }
        }
    }

    const LikenessSample *nearest = NULL;
    float best_d2 = 9999.0f;
    for (const LikenessSample &s : samples) {
        if (!LikenessSourceSampleMatches(s, source_pal, material,
                                         source_head, false))
            continue;
        float dx = (s.u - u) * 1.10f;
        float dy = s.v - v;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            nearest = &s;
        }
    }
    if (!nearest) return false;
    out->r = (float)nearest->r;
    out->g = (float)nearest->g;
    out->b = (float)nearest->b;
    out->luma = (float)nearest->luma;
    out->idx = nearest->idx;
    out->found = true;
    if (nearest_out) *nearest_out = nearest;
    return true;
}


// Extracted from imgui_overlay.cpp: LikenessLocalSourceMeanLuma
static int LikenessLocalSourceMeanLuma(const unsigned char *pix,
                                       int w, int h, int stride,
                                       PAL *pal,
                                       const LikenessSample *sample,
                                       LikenessMaterial material,
                                       const LikenessPartBox *source_head)
{
    if (!pix || !pal || !sample) return 128;
    int radius = (material == LK_MATERIAL_HEAD) ? 1 : 2;
    int sum = 0, count = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int x = sample->x + dx;
            int y = sample->y + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            unsigned char ci = pix[(size_t)y * stride + x];
            if (ci == 0) continue;
            int r, g, b, luma;
            LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
            LikenessSample temp = {};
            temp.x = x; temp.y = y; temp.idx = ci;
            temp.r = r; temp.g = g; temp.b = b; temp.luma = luma;
            if (!LikenessSourceSampleMatches(temp, pal, material,
                                             source_head, false))
                continue;
            sum += luma;
            count++;
        }
    }
    return count > 0 ? sum / count : sample->luma;
}


// Extracted from imgui_overlay.cpp: LikenessBuildActorMasks
static void LikenessBuildActorMasks(const unsigned char *target_original,
                                    int w, int h, int stride,
                                    PAL *target_pal,
                                    const LikenessBBox &tb,
                                    std::vector<unsigned char> &head_mask,
                                    std::vector<unsigned char> &skin_mask,
                                    int *head_pixels,
                                    int *skin_pixels)
{
    size_t bytes = (size_t)stride * h;
    head_mask.assign(bytes, 0);
    skin_mask.assign(bytes, 0);
    if (head_pixels) *head_pixels = 0;
    if (skin_pixels) *skin_pixels = 0;
    if (!target_original || !target_pal) return;

    LikenessPartBox target_head = {};
    LikenessFindHeadPart(target_original, w, h, stride, target_pal, tb,
                         true, &target_head);
    LikenessPartBox head_zone = target_head;
    if (head_zone.valid)
        LikenessExpandPartBox(&head_zone, w, h, 1, 1);

    float span_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * stride + x;
            unsigned char ci = target_original[off];
            if (ci == 0) continue;
            int r, g, b, luma;
            LikenessPaletteRgb(target_pal, ci, &r, &g, &b, &luma);
            float u = ((float)x - (float)tb.x1) / span_x;
            float v = ((float)y - (float)tb.y1) / span_y;
            bool headish = LikenessIsHeadRgb(r, g, b, luma) ||
                            LikenessIsHeadPixel(target_pal, ci);
            bool skinish = LikenessIsSkinRgb(r, g, b, luma);
            bool in_head_zone = LikenessPointInPartBox(head_zone, x, y);
            bool fallback_head = !head_zone.valid &&
                                 v < 0.56f && u > 0.08f && u < 0.94f;
            if (headish && (in_head_zone || fallback_head)) {
                head_mask[off] = 1;
                skin_mask[off] = 1;
            } else if (skinish && v < 0.82f &&
                       !LikenessIsBlueRgb(r, g, b, luma) &&
                       !LikenessIsLowerRgb(r, g, b, luma)) {
                skin_mask[off] = 1;
            }
        }
    }

    std::vector<unsigned char> dilated = head_mask;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * stride + x;
            if (target_original[off] == 0 || head_mask[off]) continue;
            if (head_zone.valid && !LikenessPointInPartBox(head_zone, x, y))
                continue;
            float v = ((float)y - (float)tb.y1) / span_y;
            if (v > 0.72f) continue;
            int r, g, b, luma;
            LikenessPaletteRgb(target_pal, target_original[off], &r, &g, &b, &luma);
            if (luma < 78 || LikenessIsBlueRgb(r, g, b, luma) ||
                LikenessIsLowerRgb(r, g, b, luma) ||
                LikenessIsBadGreenRgb(r, g, b, luma))
                continue;
            bool near_head = false;
            for (int dy = -1; dy <= 1 && !near_head; dy++) {
                for (int dx = -1; dx <= 1 && !near_head; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    if (head_mask[(size_t)ny * stride + nx]) near_head = true;
                }
            }
            if (near_head) {
                dilated[off] = 1;
                skin_mask[off] = 1;
            }
        }
    }
    head_mask.swap(dilated);

    int hc = 0, sc = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (head_mask[i]) hc++;
        if (skin_mask[i]) sc++;
    }
    if (head_pixels) *head_pixels = hc;
    if (skin_pixels) *skin_pixels = sc;
}


// Extracted from imgui_overlay.cpp: LikenessTargetMaterial
static LikenessMaterial LikenessTargetMaterial(const unsigned char *target_original,
                                               int w, int h, int stride,
                                               PAL *target_pal,
                                               int x, int y,
                                               const std::vector<unsigned char> &head_mask,
                                               const std::vector<unsigned char> &skin_mask,
                                               float v)
{
    size_t off = (size_t)y * stride + x;
    if (!target_original || target_original[off] == 0 || !target_pal)
        return LK_MATERIAL_ANY;
    if (off < head_mask.size() && head_mask[off])
        return LK_MATERIAL_HEAD;
    if (off < skin_mask.size() && skin_mask[off])
        return LK_MATERIAL_SKIN;

    unsigned char ci = target_original[off];
    int r, g, b, luma;
    LikenessPaletteRgb(target_pal, ci, &r, &g, &b, &luma);
    int edge = LikenessTransparentNeighborCount(target_original, w, h, stride, x, y);
    if (edge >= 4 && LikenessIsBlackRgb(r, g, b, luma))
        return LK_MATERIAL_BLACK;
    if (LikenessIsBlueRgb(r, g, b, luma))
        return LK_MATERIAL_BLUE;
    if (LikenessIsGoldPixel(target_pal, ci) ||
        LikenessIsLowerRgb(r, g, b, luma))
        return LK_MATERIAL_LOWER;
    if (LikenessIsSkinRgb(r, g, b, luma) && v < 0.82f)
        return LK_MATERIAL_SKIN;
    if (v > 0.56f)
        return LK_MATERIAL_LOWER;
    if (luma < 48)
        return LK_MATERIAL_BLUE;
    return LK_MATERIAL_BLUE;
}


// Extracted from imgui_overlay.cpp: LikenessScaleRgbToLuma
static void LikenessScaleRgbToLuma(float r, float g, float b, float target_luma,
                                   int *out_r, int *out_g, int *out_b)
{
    float src_luma = (float)LikenessRgbLuma8((int)(r + 0.5f),
                                            (int)(g + 0.5f),
                                            (int)(b + 0.5f));
    float scale = target_luma / (src_luma < 1.0f ? 1.0f : src_luma);
    *out_r = LikenessClampByte((int)(r * scale + 0.5f));
    *out_g = LikenessClampByte((int)(g * scale + 0.5f));
    *out_b = LikenessClampByte((int)(b * scale + 0.5f));
}


// Extracted from imgui_overlay.cpp: LikenessApplyActorComposite
static int LikenessApplyActorComposite(const unsigned char *source_pix,
                                       int sw, int sh, int ss,
                                       const unsigned char *target_original,
                                       int tw, int th, int ts,
                                       unsigned char *target_pix,
                                       PAL *source_pal, PAL *target_pal,
                                       const LikenessBBox &sb,
                                       const LikenessBBox &tb,
                                       const std::vector<LikenessSample> &samples,
                                       const std::vector<std::vector<int>> &bins,
                                       const bool used_slots[256],
                                       const LikenessPartBox &source_head,
                                       const std::vector<unsigned char> &head_mask,
                                       const std::vector<unsigned char> &skin_mask,
                                       bool mirror,
                                       bool palette_changed,
                                       int *opaque_out,
                                       int *edge_darkened_out)
{
    if (opaque_out) *opaque_out = 0;
    if (edge_darkened_out) *edge_darkened_out = 0;
    if (!source_pix || !target_original || !target_pix ||
        !source_pal || !target_pal)
        return 0;

    float span_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int changed = 0;
    int opaque = 0;
    int edge_darkened = 0;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * ts + x;
            unsigned char old_ci = target_original[off];
            if (old_ci == 0) continue;
            opaque++;

            float u = ((float)x - (float)tb.x1) / span_x;
            float v = ((float)y - (float)tb.y1) / span_y;
            float su = u;
            float sv = v;
            if (sb.anchor_ok && tb.anchor_ok) {
                float src_anchor_u = mirror ? (1.0f - sb.anchor_u) : sb.anchor_u;
                float anchor_u = src_anchor_u + (u - tb.anchor_u);
                float anchor_v = sb.anchor_v + (v - tb.anchor_v);
                su = u * 0.82f + anchor_u * 0.18f;
                sv = v * 0.84f + anchor_v * 0.16f;
            }
            if (su < 0.0f) su = 0.0f; if (su > 1.0f) su = 1.0f;
            if (sv < 0.0f) sv = 0.0f; if (sv > 1.0f) sv = 1.0f;

            int tr, tg, tbv, tluma;
            LikenessPaletteRgb(target_pal, old_ci, &tr, &tg, &tbv, &tluma);
            LikenessMaterial material = LikenessTargetMaterial(target_original,
                                                              tw, th, ts,
                                                              target_pal,
                                                              x, y,
                                                              head_mask,
                                                              skin_mask,
                                                              v);
            LikenessColor src_col = {};
            const LikenessSample *nearest = NULL;
            if (!LikenessSampleMaterialSource(samples, bins, source_pal,
                                              material, &source_head,
                                              su, sv, &src_col, &nearest)) {
                if (!LikenessSampleMaterialSource(samples, bins, source_pal,
                                                  LK_MATERIAL_ANY, NULL,
                                                  su, sv, &src_col, &nearest))
                    continue;
            }
            if (!src_col.found || src_col.idx == 0) continue;

            int local_mean = LikenessLocalSourceMeanLuma(source_pix, sw, sh, ss,
                                                        source_pal, nearest,
                                                        material, &source_head);
            float hi = src_col.luma - (float)local_mean;
            float source_pull = 0.13f;
            float texture_strength = 0.28f;
            float target_rgb_keep = 0.10f;
            switch (material) {
            case LK_MATERIAL_HEAD:
                source_pull = 0.10f;
                texture_strength = 0.15f;
                target_rgb_keep = 0.24f;
                break;
            case LK_MATERIAL_SKIN:
                source_pull = 0.14f;
                texture_strength = 0.18f;
                target_rgb_keep = 0.18f;
                break;
            case LK_MATERIAL_BLUE:
                source_pull = 0.13f;
                texture_strength = 0.30f;
                target_rgb_keep = 0.08f;
                break;
            case LK_MATERIAL_LOWER:
                source_pull = 0.12f;
                texture_strength = 0.34f;
                target_rgb_keep = 0.07f;
                break;
            case LK_MATERIAL_BLACK:
                source_pull = 0.08f;
                texture_strength = 0.18f;
                target_rgb_keep = 0.16f;
                break;
            default:
                break;
            }

            float desired_luma = (float)tluma * (1.0f - source_pull) +
                                 src_col.luma * source_pull +
                                 hi * texture_strength;
            int edge = LikenessTransparentNeighborCount(target_original,
                                                        tw, th, ts, x, y);
            unsigned int h = (unsigned int)(x * 73856093u) ^
                             (unsigned int)(y * 19349663u) ^
                             (unsigned int)(src_col.idx * 83492791u);
            if (edge > 0 &&
                material != LK_MATERIAL_HEAD &&
                material != LK_MATERIAL_SKIN &&
                (edge >= 5 || (edge >= 3 && (h & 15u) < 3u))) {
                desired_luma *= edge >= 5 ? 0.72f : 0.84f;
                edge_darkened++;
            }
            if (desired_luma < 6.0f) desired_luma = 6.0f;
            if (desired_luma > 244.0f) desired_luma = 244.0f;

            int rr, gg, bb;
            LikenessScaleRgbToLuma(src_col.r, src_col.g, src_col.b,
                                   desired_luma, &rr, &gg, &bb);
            rr = (int)((float)rr * (1.0f - target_rgb_keep) +
                       (float)tr * target_rgb_keep + 0.5f);
            gg = (int)((float)gg * (1.0f - target_rgb_keep) +
                       (float)tg * target_rgb_keep + 0.5f);
            bb = (int)((float)bb * (1.0f - target_rgb_keep) +
                       (float)tbv * target_rgb_keep + 0.5f);

            int new_ci = LikenessNearestMaterialSlot(source_pal, used_slots,
                                                     material, rr, gg, bb,
                                                     (int)src_col.r,
                                                     (int)src_col.g,
                                                     (int)src_col.b);
            if (new_ci <= 0) continue;
            if (target_pix[off] != (unsigned char)new_ci || palette_changed) {
                target_pix[off] = (unsigned char)new_ci;
                changed++;
            }
        }
    }
    if (opaque_out) *opaque_out = opaque;
    if (edge_darkened_out) *edge_darkened_out = edge_darkened;
    return changed;
}


// Extracted from imgui_overlay.cpp: LikenessApplyActorAccents
static int LikenessApplyActorAccents(const unsigned char *source_pix,
                                     int sw, int sh, int ss,
                                     const unsigned char *target_original,
                                     int tw, int th, int ts,
                                     unsigned char *target_pix,
                                     PAL *source_pal, PAL *target_pal,
                                     const LikenessBBox &sb,
                                     const LikenessBBox &tb,
                                     bool mirror,
                                     const bool used_slots[256],
                                     const LikenessPartBox &source_head,
                                     const std::vector<unsigned char> &head_mask,
                                     const std::vector<unsigned char> &skin_mask,
                                     std::vector<unsigned char> &accent_mask)
{
    if (!source_pix || !target_original || !target_pix ||
        !source_pal || !target_pal)
        return 0;

    size_t bytes = (size_t)ts * th;
    accent_mask.assign(bytes, 0);
    std::vector<int> best_score(bytes, 0x7FFFFFFF);
    float tspan_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float tspan_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int changed = 0;

    for (int sy = sb.y1; sy <= sb.y2; sy++) {
        for (int sx = sb.x1; sx <= sb.x2; sx++) {
            unsigned char sci = source_pix[(size_t)sy * ss + sx];
            if (sci == 0 || LikenessIsBadGreenPixel(source_pal, sci))
                continue;

            float raw_u = ((float)sx - (float)sb.x1) /
                          ((sb.w > 1) ? (float)(sb.w - 1) : 1.0f);
            float raw_v = ((float)sy - (float)sb.y1) /
                          ((sb.h > 1) ? (float)(sb.h - 1) : 1.0f);
            LikenessMaterial sm = LikenessClassifyPaletteSlot(source_pal, sci);
            bool contrast = LikenessHasLocalContrast(source_pix, sw, sh, ss,
                                                     sx, sy, source_pal);
            bool edge = LikenessTransparentNeighborCount(source_pix, sw, sh, ss,
                                                         sx, sy) > 0;
            bool in_source_head = LikenessPointInPartBox(source_head, sx, sy);
            bool gold_accent = sm == LK_MATERIAL_LOWER &&
                               raw_v < 0.82f &&
                               (contrast || edge ||
                                LikenessIsGoldPixel(source_pal, sci));
            bool skin_accent = (sm == LK_MATERIAL_SKIN ||
                                sm == LK_MATERIAL_HEAD) &&
                               raw_v < 0.58f &&
                               contrast && !in_source_head;
            if (!gold_accent && !skin_accent)
                continue;

            float tu, tv;
            if (!LikenessProjectSourceToTarget(raw_u, raw_v, sb, tb, mirror,
                                               &tu, &tv))
                continue;
            int tx0 = LikenessClampInt((int)(tb.x1 + tu * tspan_x + 0.5f),
                                       0, tw - 1);
            int ty0 = LikenessClampInt((int)(tb.y1 + tv * tspan_y + 0.5f),
                                       0, th - 1);
            int search_radius = contrast ? 2 : 1;
            if (gold_accent && raw_v > 0.46f) search_radius = 3;

            int best_off = -1;
            int best = 0x7FFFFFFF;
            LikenessMaterial best_tm = LK_MATERIAL_ANY;
            float best_v = 0.0f;
            for (int dy = -search_radius; dy <= search_radius; dy++) {
                for (int dx = -search_radius; dx <= search_radius; dx++) {
                    int tx = tx0 + dx;
                    int ty = ty0 + dy;
                    if (tx < 0 || ty < 0 || tx >= tw || ty >= th) continue;
                    size_t off = (size_t)ty * ts + tx;
                    if (target_original[off] == 0 || accent_mask[off])
                        continue;
                    float tvn = ((float)ty - (float)tb.y1) / tspan_y;
                    LikenessMaterial tm = LikenessTargetMaterial(target_original,
                                                                 tw, th, ts,
                                                                 target_pal,
                                                                 tx, ty,
                                                                 head_mask,
                                                                 skin_mask,
                                                                 tvn);
                    bool allowed = false;
                    if (gold_accent) {
                        allowed = tm == LK_MATERIAL_BLUE ||
                                  tm == LK_MATERIAL_LOWER ||
                                  tm == LK_MATERIAL_BLACK;
                    } else if (skin_accent) {
                        allowed = tm == LK_MATERIAL_SKIN ||
                                  tm == LK_MATERIAL_HEAD;
                    }
                    if (!allowed) continue;

                    int dist2 = dx * dx + dy * dy;
                    int mismatch = LikenessMaskMismatch(source_pix, sw, sh, ss,
                                                        sx, sy,
                                                        target_original,
                                                        tw, th, ts, tx, ty);
                    int target_edge = LikenessTransparentNeighborCount(
                        target_original, tw, th, ts, tx, ty);
                    int score = dist2 * 10 + mismatch * 8;
                    if (gold_accent && tm == LK_MATERIAL_BLUE) score -= 7;
                    if (gold_accent && tm == LK_MATERIAL_LOWER) score -= 5;
                    if (skin_accent && tm == LK_MATERIAL_SKIN) score -= 6;
                    if (target_edge > 0) score -= 2;
                    if (score < best) {
                        best = score;
                        best_off = (int)off;
                        best_tm = tm;
                        best_v = tvn;
                    }
                }
            }

            int threshold = gold_accent ? 58 : 44;
            if (best_off < 0 || best > threshold ||
                best >= best_score[(size_t)best_off])
                continue;
            best_score[(size_t)best_off] = best;

            unsigned char old_tci = target_original[(size_t)best_off];
            int tr, tg, tbv, tluma;
            LikenessPaletteRgb(target_pal, old_tci, &tr, &tg, &tbv, &tluma);
            int sr, sg, sbc, sluma;
            LikenessPaletteRgb(source_pal, sci, &sr, &sg, &sbc, &sluma);
            float pull = gold_accent ? 0.38f : 0.28f;
            if (best_tm == LK_MATERIAL_LOWER && best_v > 0.58f)
                pull = 0.30f;
            float want_luma = (float)tluma * (1.0f - pull) +
                              (float)sluma * pull;
            int rr, gg, bb;
            LikenessScaleRgbToLuma((float)sr, (float)sg, (float)sbc,
                                   want_luma, &rr, &gg, &bb);
            int new_ci = LikenessNearestMaterialSlot(source_pal, used_slots,
                                                     sm, rr, gg, bb,
                                                     sr, sg, sbc);
            if (new_ci <= 0 || target_pix[(size_t)best_off] == (unsigned char)new_ci)
                continue;
            target_pix[(size_t)best_off] = (unsigned char)new_ci;
            accent_mask[(size_t)best_off] = 1;
            changed++;
        }
    }
    return changed;
}


// Extracted from imgui_overlay.cpp: LikenessCleanActorPaletteOutliers
static int LikenessCleanActorPaletteOutliers(unsigned char *target_pix,
                                             const unsigned char *target_original,
                                             int w, int h, int stride,
                                             PAL *source_pal, PAL *target_pal,
                                             const bool used_slots[256],
                                             const LikenessBBox &tb,
                                             const std::vector<unsigned char> &head_mask,
                                             const std::vector<unsigned char> &skin_mask)
{
    if (!target_pix || !target_original || !source_pal || !target_pal)
        return 0;

    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int changed = 0;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * stride + x;
            unsigned char ci = target_pix[off];
            if (target_original[off] == 0 || ci == 0 ||
                !LikenessIsBadGreenPixel(source_pal, ci))
                continue;
            float v = ((float)y - (float)tb.y1) / span_y;
            LikenessMaterial material = LikenessTargetMaterial(target_original,
                                                              w, h, stride,
                                                              target_pal,
                                                              x, y,
                                                              head_mask,
                                                              skin_mask,
                                                              v);
            int tr, tg, tbv, tluma;
            LikenessPaletteRgb(target_pal, target_original[off],
                               &tr, &tg, &tbv, &tluma);
            int new_ci = LikenessNearestMaterialSlot(source_pal, used_slots,
                                                     material,
                                                     tr, tg, tbv,
                                                     -1, -1, -1,
                                                     true);
            if (new_ci <= 0 || new_ci == ci)
                continue;
            target_pix[off] = (unsigned char)new_ci;
            changed++;
        }
    }
    return changed;
}


// Extracted from imgui_overlay.cpp: ApplyMarkedLikenessToSelected
int ApplyMarkedLikenessToSelected(void)

{
    int target_idx = g_doc->ilselected;
    IMG *target = (target_idx >= 0) ? get_img(target_idx) : NULL;
    if (!target || !target->data_p || target->w == 0 || target->h == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Select a target sprite first.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    int source_idx = -1;
    int marked_sources = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!(img->flags & 1) || idx == target_idx) continue;
        source_idx = idx;
        marked_sources++;
    }
    if (marked_sources != 1) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark exactly one source sprite, then select a different target sprite.");
        g_restore_msg_timer = 5.0f;
        return 0;
    }

    IMG *source = get_img(source_idx);
    PAL *source_pal = source ? get_pal((int)source->palnum) : NULL;
    PAL *target_pal = get_pal((int)target->palnum);
    if (!source || !source->data_p || source->w == 0 || source->h == 0 ||
        !source_pal || !source_pal->data_p || !target_pal || !target_pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Source and target sprites need valid palettes.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    LikenessBBox sb = {}, tb = {};
    if (!LikenessOpaqueBBox(source, &sb) || !LikenessOpaqueBBox(target, &tb)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Source and target need non-transparent pixels.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<LikenessSample> samples;
    bool used_slots[256];
    LikenessBuildSamples(source, source_pal, sb, samples, used_slots, NULL);
    if (samples.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Marked source has no visible pixels to transfer.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    float normal_score = LikenessMaskScore(target, tb, samples, false);
    float mirror_score = LikenessMaskScore(target, tb, samples, true);
    bool mirror = mirror_score > normal_score * 1.08f;

    const int G = 32;
    std::vector<std::vector<int>> bins((size_t)G * G);
    for (int i = 0; i < (int)samples.size(); i++) {
        if (mirror) samples[i].u = 1.0f - samples[i].u;
        int bx = (int)(samples[i].u * G);
        int by = (int)(samples[i].v * G);
        if (bx < 0) bx = 0; if (bx >= G) bx = G - 1;
        if (by < 0) by = 0; if (by >= G) by = G - 1;
        bins[by * G + bx].push_back(i);
    }

    if (!doc_undo_push()) return 0;
    target = get_img(target_idx);
    source = get_img(source_idx);
    source_pal = source ? get_pal((int)source->palnum) : NULL;
    target_pal = target ? get_pal((int)target->palnum) : NULL;
    if (!target || !target->data_p || !source || !source_pal || !target_pal)
        return 0;

    int target_stride = (target->w + 3) & ~3;
    unsigned char *target_pix = (unsigned char *)target->data_p;
    size_t target_bytes = (size_t)target_stride * target->h;
    std::vector<unsigned char> target_original(target_bytes);
    memcpy(target_original.data(), target_pix, target_bytes);
    int source_stride = (source->w + 3) & ~3;
    const unsigned char *source_pix = (const unsigned char *)source->data_p;
    bool palette_changed = target->palnum != source->palnum;
    LikenessPartBox source_head = {};
    LikenessFindHeadPart(source_pix, source->w, source->h, source_stride,
                         source_pal, sb, true, &source_head);
    if (source_head.valid)
        LikenessExpandPartBox(&source_head, source->w, source->h, 1, 1);

    LikenessActorStats stats = {};
    std::vector<unsigned char> head_mask;
    std::vector<unsigned char> skin_mask;
    LikenessBuildActorMasks(target_original.data(),
                            target->w, target->h, target_stride,
                            target_pal, tb,
                            head_mask, skin_mask,
                            &stats.head_pixels, &stats.skin_pixels);

    stats.remapped = LikenessApplyActorComposite(source_pix,
                                                 source->w, source->h, source_stride,
                                                 target_original.data(),
                                                 target->w, target->h, target_stride,
                                                 target_pix,
                                                 source_pal, target_pal,
                                                 sb, tb,
                                                 samples, bins, used_slots,
                                                 source_head,
                                                 head_mask, skin_mask,
                                                 mirror, palette_changed,
                                                 &stats.opaque,
                                                 &stats.edge_darkened);

    std::vector<unsigned char> accent_mask;
    stats.accents = LikenessApplyActorAccents(source_pix,
                                              source->w, source->h, source_stride,
                                              target_original.data(),
                                              target->w, target->h, target_stride,
                                              target_pix,
                                              source_pal, target_pal,
                                              sb, tb, mirror,
                                              used_slots,
                                              source_head,
                                              head_mask, skin_mask,
                                              accent_mask);
    stats.cleaned = LikenessCleanActorPaletteOutliers(target_pix,
                                                      target_original.data(),
                                                      target->w, target->h,
                                                      target_stride,
                                                      source_pal, target_pal,
                                                      used_slots, tb,
                                                      head_mask, skin_mask);
    int total_changed = stats.remapped + stats.accents + stats.cleaned;

    target->palnum = source->palnum;
    g_doc->plselected = source->palnum;
    ApplyPalette(source->palnum);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    InvalidateThumb(target_idx);
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Applied %s likeness to %s: %d/%d remapped, %d accents, %d cleaned, %d head px%s.",
             source->n_s, target->n_s,
             stats.remapped, stats.opaque, stats.accents, stats.cleaned,
             stats.head_pixels,
             mirror ? " (mirrored source fit)" : "");
    g_restore_msg_timer = 6.0f;
    return total_changed;
}


// Extracted from imgui_overlay.cpp: unlink_and_free_img
void unlink_and_free_img(IMG *victim)
{
    if (!victim) return;
    IMG *prev = NULL;
    IMG *cur = (IMG *)g_doc->img_p;
    while (cur && cur != victim) { prev = cur; cur = (IMG *)cur->nxt_p; }
    if (cur == victim) {
        if (prev) prev->nxt_p = cur->nxt_p;
        else g_doc->img_p = cur->nxt_p;
        g_doc->imgcnt--;
    }
    FreeImg(victim);
}


// Extracted from imgui_overlay.cpp: SplitSelectionToOverlayFrame
void SplitSelectionToOverlayFrame(bool clear_source)
{
    IMG *src = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return;
    if (!g_grid_sel.active) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select pixels first, then split an overlay frame.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int stride = (src->w + 3) & ~3;
    unsigned int sz = (unsigned int)stride * src->h;
    PixelHist snap = {};
    bool have_snap = clear_source && pixel_hist_capture(&snap);

    IMG *dst = (IMG *)AllocImg();
    if (!dst) {
        if (have_snap) pixel_hist_free(&snap);
        return;
    }

    dst->w = src->w; dst->h = src->h;
    dst->palnum = src->palnum; dst->flags = src->flags;
    dst->anix = src->anix; dst->aniy = src->aniy;
    dst->anix2 = src->anix2; dst->aniy2 = src->aniy2; dst->aniz2 = src->aniz2;
    dst->opals = src->opals;
    if (src->opaltbl_p) {
        dst->opaltbl_p = malloc(16);
        if (!dst->opaltbl_p) {
            if (have_snap) pixel_hist_free(&snap);
            unlink_and_free_img(dst);
            return;
        }
        memcpy(dst->opaltbl_p, src->opaltbl_p, 16);
    }
    strncpy(dst->src_filename, src->src_filename, sizeof(dst->src_filename) - 1);
    dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';
    snprintf(dst->n_s, sizeof(dst->n_s), "%.12sOVR", src->n_s);

    dst->data_p = PoolAlloc(sz);
    if (!dst->data_p) {
        if (have_snap) pixel_hist_free(&snap);
        unlink_and_free_img(dst);
        return;
    }

    unsigned char *sp = (unsigned char *)src->data_p;
    unsigned char *dp = (unsigned char *)dst->data_p;
    int copied = 0;
    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            if (!selection_contains_pixel(src, x, y)) continue;
            unsigned char v = sp[y * stride + x];
            if (v == 0) continue;
            dp[y * stride + x] = v;
            if (clear_source) sp[y * stride + x] = 0;
            copied++;
        }
    }

    if (copied == 0) {
        if (have_snap) pixel_hist_free(&snap);
        unlink_and_free_img(dst);
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No opaque selected pixels to split.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    if (have_snap) {
        snap.seq = ++g_undo_seq;
        if (g_pixel_hist.size() >= kPixelHistMax) {
            pixel_hist_free(&g_pixel_hist.front());
            g_pixel_hist.erase(g_pixel_hist.begin());
        }
        g_pixel_hist.push_back(snap);
        for (auto &redo : g_pixel_redo) pixel_hist_free(&redo);
        g_pixel_redo.clear();
        ClearDocumentRedoStack();
    }

    g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "%s %d px into overlay frame.",
             clear_source ? "Split" : "Copied", copied);
    g_restore_msg_timer = 4.0f;
}


// Extracted from imgui_overlay.cpp: RemoveHardStrokeFromImage
static int RemoveHardStrokeFromImage(IMG *img, PAL *pal, int max_width, bool apply)
{
    if (!img || !img->data_p || !pal || !pal->data_p ||
        img->w == 0 || img->h == 0)
        return 0;
    if (max_width < 1) max_width = 1;
    if (max_width > 2) max_width = 2;

    int w = img->w;
    int h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pixels = (unsigned char *)img->data_p;
    size_t bytes = (size_t)stride * h;
    std::vector<unsigned char> work(bytes);
    memcpy(work.data(), pixels, bytes);
    int changed = 0;
    std::vector<std::pair<int, unsigned char>> writes;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = work[(size_t)y * stride + x];
            if (ci == 0) continue;

            int trans = EdgeBufferTransparentNeighbors(work.data(), w, h, stride, x, y);
            if (trans <= 0) continue;

            unsigned short edge_word = pal_word_or_black(pal, ci);
            int edge_luma = StrokeWordLuma8(edge_word);
            int same_edge_neighbors = 0;
            int interior_count = 0;
            int interior_luma_sum = 0;
            int min_dist = 0x7FFFFFFF;
            unsigned char inward_ci = 0;
            bool have_inward = FindInwardEdgeReplacement(work.data(), w, h, stride,
                                                         x, y, pal, ci, max_width,
                                                         &inward_ci);

            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    unsigned char ni = work[(size_t)ny * stride + nx];
                    if (ni == 0) continue;
                    unsigned short nw = pal_word_or_black(pal, ni);
                    int dist = PaletteColorDistance5(edge_word, nw);
                    if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 &&
                        dist <= 6)
                        same_edge_neighbors++;

                    if (dist <= 6) continue;
                    interior_count++;
                    interior_luma_sum += StrokeWordLuma8(nw);
                    if (dist < min_dist) min_dist = dist;
                }
            }

            if (interior_count <= 0 || !have_inward) continue;
            int avg_luma = interior_luma_sum / interior_count;
            int luma_delta = avg_luma - edge_luma;
            if (luma_delta < 0) luma_delta = -luma_delta;

            bool hard_color_step = min_dist >= 28 || luma_delta >= 34;
            bool dark_outline = edge_luma + 20 < avg_luma && min_dist >= 12;
            bool strong_inward = EdgeColorStrongVariant(pal, ci, inward_ci);
            bool stroke_supported = same_edge_neighbors >= 1 || trans >= 3;
            if (stroke_supported && strong_inward && (hard_color_step || dark_outline)) {
                writes.push_back({(int)((size_t)y * stride + x), inward_ci});
            }
        }
    }

    for (const auto &wr : writes) {
        if (work[(size_t)wr.first] != wr.second) {
            work[(size_t)wr.first] = wr.second;
            changed++;
        }
    }

    if (apply && changed > 0)
        memcpy(pixels, work.data(), bytes);
    return changed;
}


// Extracted from imgui_overlay.cpp: RemoveHardStrokeFromTargets
int RemoveHardStrokeFromTargets(int max_width)

{
    int marked = CountMarkedImages();
    int selected = g_doc->ilselected;
    if (marked == 0 && selected < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark sprites, or select one sprite, before removing hard strokes.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<int> changed_indices;
    int expected_pixels = 0;
    int scanned = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0) : (idx == selected);
        if (!target || !img->data_p || img->w == 0 || img->h == 0) continue;
        scanned++;
        PAL *pal = get_pal((int)img->palnum);
        int n = RemoveHardStrokeFromImage(img, pal, max_width, false);
        if (n > 0) {
            changed_indices.push_back(idx);
            expected_pixels += n;
        }
    }

    if (expected_pixels <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No hard 1-2px stroke found in %d sprite%s.",
                 scanned, scanned == 1 ? "" : "s");
        g_restore_msg_timer = 5.0f;
        return 0;
    }

    doc_undo_push();
    int changed_images = 0;
    int changed_pixels = 0;
    for (int changed_idx : changed_indices) {
        IMG *img = get_img(changed_idx);
        PAL *pal = img ? get_pal((int)img->palnum) : NULL;
        int n = RemoveHardStrokeFromImage(img, pal, max_width, true);
        if (n > 0) {
            changed_images++;
            changed_pixels += n;
            InvalidateThumb(changed_idx);
        }
    }

    if (changed_pixels > 0) {
        g_img_tex_idx = -2;
        mark_dirty();
    }

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Removed %d hard-stroke pixel%s from %d/%d sprite%s.",
             changed_pixels,
             changed_pixels == 1 ? "" : "s",
             changed_images,
             scanned,
             scanned == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    return changed_pixels;
}

int CleanSpriteArtifactsInTargets(const SpriteCleanupOptions *options)
{
    int marked = CountMarkedImages();
    int selected = g_doc->ilselected;
    if (marked == 0 && selected < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark sprites, or select one sprite, before cleanup.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<int> changed_indices;
    int expected_pixels = 0;
    int scanned = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0) : (idx == selected);
        if (!target || !img->data_p || img->w == 0 || img->h == 0) continue;
        scanned++;

        PAL *pal = get_pal((int)img->palnum);
        int stride = ((int)img->w + 3) & ~3;
        int n = CleanupSpriteArtifacts((unsigned char *)img->data_p,
                                       (int)img->w, (int)img->h, stride,
                                       pal, options, false);
        if (n > 0) {
            changed_indices.push_back(idx);
            expected_pixels += n;
        }
    }

    if (expected_pixels <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No isolated sprite artifacts found in %d sprite%s.",
                 scanned, scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    if (!doc_undo_push()) return 0;

    int changed_images = 0;
    int changed_pixels = 0;
    for (int changed_idx : changed_indices) {
        IMG *img = get_img(changed_idx);
        PAL *pal = img ? get_pal((int)img->palnum) : NULL;
        if (!img || !img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = ((int)img->w + 3) & ~3;
        int n = CleanupSpriteArtifacts((unsigned char *)img->data_p,
                                       (int)img->w, (int)img->h, stride,
                                       pal, options, true);
        if (n > 0) {
            changed_images++;
            changed_pixels += n;
            InvalidateThumb(changed_idx);
        }
    }

    if (changed_pixels > 0) {
        g_img_tex_idx = -2;
        mark_dirty();
    }

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Cleaned %d artifact pixel%s from %d/%d sprite%s.",
             changed_pixels,
             changed_pixels == 1 ? "" : "s",
             changed_images,
             scanned,
             scanned == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    return changed_pixels;
}


// Extracted from imgui_overlay.cpp: StripMarkedImages
void StripMarkedImages(int max_transparent_neighbors, int specific_color)
{
    if (max_transparent_neighbors < 1) max_transparent_neighbors = 1;
    if (max_transparent_neighbors > 8) max_transparent_neighbors = 8;

    bool undo_pushed = false;
    int changed_images = 0;
    int changed_pixels = 0;
    int scanned = 0;
    int idx = 0;
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            scanned++;
            int w = img->w;
            int h = img->h;
            int stride = (w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            size_t bytes = (size_t)stride * h;

            std::vector<unsigned char> original(bytes);
            std::vector<unsigned char> flags(bytes, 0);
            if (original.empty() || flags.empty()) {
                img = (IMG *)img->nxt_p;
                idx++;
                continue;
            }
            memcpy(original.data(), pixels, bytes);

            /* Bounds checking counts out-of-bounds as transparent (matches ASM logic) */
            auto is_transparent = [&](int x, int y) -> bool {
                if (x < 0 || x >= w || y < 0 || y >= h) return true;
                return original[(size_t)y * stride + x] == 0;
            };

            /* Pass 1: flag the original outer ring, then repaint only those
               saved edge pixels from a nearby inward color. This keeps the
               operation from chasing its own freshly edited pixels inward. */
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char c = original[(size_t)y * stride + x];
                    if (c == 0) continue;
                    if (specific_color >= 0 && c != specific_color) continue;

                    int trans_count = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            if (is_transparent(x + dx, y + dy)) trans_count++;
                        }
                    }

                    if (trans_count >= 2 && trans_count <= max_transparent_neighbors) {
                        flags[(size_t)y * stride + x] = 1;
                    }
                }
            }

            PAL *pal = get_pal((int)img->palnum);
            std::vector<std::pair<int, unsigned char>> edge_writes;
            if (pal && pal->data_p) {
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        int off = y * stride + x;
                        if (!flags[(size_t)off]) continue;
                        unsigned char edge_ci = original[(size_t)off];
                        unsigned char repl = 0;
                        if (FindInwardEdgeReplacement(original.data(), w, h, stride,
                                                      x, y, pal, edge_ci, 2, &repl) &&
                            EdgeColorStrongVariant(pal, edge_ci, repl) &&
                            repl != edge_ci) {
                            edge_writes.push_back({off, repl});
                        }
                    }
                }
            }

            if (!edge_writes.empty() && !undo_pushed) {
                if (!doc_undo_push()) return;
                undo_pushed = true;
            }

            int image_changes = 0;
            for (const auto &wr : edge_writes) {
                if (pixels[wr.first] != wr.second) {
                    pixels[wr.first] = wr.second;
                    image_changes++;
                }
            }

            memset(flags.data(), 0, bytes);

            /* Pass 2: Flag lonely pixels (stray dust specs) */
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char c = pixels[(size_t)y * stride + x];
                    if (c == 0) continue;
                    if (specific_color >= 0 && c != specific_color) continue;

                    int trans_count = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            if (x + dx < 0 || x + dx >= w ||
                                y + dy < 0 || y + dy >= h ||
                                pixels[(size_t)(y + dy) * stride + (x + dx)] == 0)
                                trans_count++;
                        }
                    }
                    if (trans_count == 8) flags[(size_t)y * stride + x] = 1;
                }
            }

            /* Pass 2: Delete flagged lonely pixels */
            bool has_lonely = false;
            for (size_t i = 0; i < bytes; i++) {
                if (flags[i]) { has_lonely = true; break; }
            }
            if (has_lonely && !undo_pushed) {
                if (!doc_undo_push()) return;
                undo_pushed = true;
            }
            for (size_t i = 0; i < bytes; i++) {
                if (flags[i] && pixels[i] != 0) {
                    pixels[i] = 0;
                    image_changes++;
                }
            }

            if (image_changes > 0) {
                changed_images++;
                changed_pixels += image_changes;
                InvalidateThumb(idx);
            }
        }
        img = (IMG *)img->nxt_p;
        idx++;
    }
    if (changed_pixels > 0) {
        mark_dirty();
        g_img_tex_idx = -2; /* Force texture rebuild */
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Repainted %d edge pixel%s in %d/%d marked sprite%s.",
                 changed_pixels,
                 changed_pixels == 1 ? "" : "s",
                 changed_images,
                 scanned,
                 scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    } else if (scanned == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark one or more sprites before stripping edges.");
        g_restore_msg_timer = 4.0f;
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No replaceable edge stroke found in %d marked sprite%s.",
                 scanned,
                 scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
}


// Extracted from imgui_overlay.cpp: DitherReplaceMarkedImages
void DitherReplaceMarkedImages(int specific_color)

{
    mark_dirty();
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            int h = img->h;
            int stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            
            for (int y = 0; y < h; y++) {
                int start_x = y & 1; // 0 for even rows, 1 for odd rows
                for (int x = start_x; x < stride; x += 2) {
                    if (pixels[y * stride + x] != 0) {
                        pixels[y * stride + x] = (unsigned char)specific_color;
                    }
                }
            }
        }
        img = (IMG *)img->nxt_p;
    }
    g_img_tex_idx = -2; /* Force texture rebuild */
}


// Extracted from imgui_overlay.cpp: LeastSquaresReduceMarked
void LeastSquaresReduceMarked()

{
    mark_dirty();
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            int w = img->w;
            int h = img->h;
            unsigned short stride = (w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;

            int min_x = w, min_y = h, max_x = -1, max_y = -1;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    if (pixels[y * stride + x] != 0) {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                    }
                }
            }

            if (max_x == -1) {
                /* Image is completely empty. Shrink to 1x1 transparent. */
                img->w = 1; img->h = 1;
                img->anix = 0; img->aniy = 0;
                pixels[0] = 0;
            } else if (min_x > 0 || min_y > 0 || max_x < w - 1 || max_y < h - 1) {
                int new_w = max_x - min_x + 1;
                int new_h = max_y - min_y + 1;
                unsigned short new_stride = (new_w + 3) & ~3;

                /* In-place compaction (safe because new_stride <= stride) */
                for (int y = 0; y < new_h; y++) {
                    for (int x = 0; x < new_w; x++) {
                        pixels[y * new_stride + x] = pixels[(y + min_y) * stride + (x + min_x)];
                    }
                    /* Zero out the padding bytes to be safe */
                    for (int x = new_w; x < new_stride; x++) {
                        pixels[y * new_stride + x] = 0;
                    }
                }

                img->w = (unsigned short)new_w;
                img->h = (unsigned short)new_h;
                img->anix -= (unsigned short)min_x;
                img->aniy -= (unsigned short)min_y;
            }
        }
        img = (IMG *)img->nxt_p;
    }
    g_img_tex_idx = -2; /* Force texture rebuild */
}



/* =========================================================
   Clipboard, Paste, Layers, Selections, and Free Transform
   ========================================================= */

void ClearPixelClipboard(void)
{
    if (g_clipboard.data_p) free(g_clipboard.data_p);
    memset(&g_clipboard, 0, sizeof(g_clipboard));
}


bool BuildClipboardPaletteMap(const PAL *target_pal, unsigned char map[256])
{
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;
    if (!g_clipboard.has_palette || !target_pal || !target_pal->data_p)
        return false;

    int src_n = g_clipboard.palette_numc;
    int dst_n = target_pal->numc;
    if (src_n > 256) src_n = 256;
    if (dst_n > 256) dst_n = 256;
    if (src_n <= 0 || dst_n <= 0) return false;

    const unsigned char *td = (const unsigned char *)target_pal->data_p;
    if (src_n == dst_n &&
        memcmp(g_clipboard.palette_data, td, (size_t)src_n * 2u) == 0)
        return false;

    map[0] = 0;
    for (int i = 1; i < 256; i++) {
        if (i < src_n) {
            unsigned short src_word = palette_word_at(g_clipboard.palette_data, i);
            map[i] = nearest_palette_index_for_word(src_word, target_pal);
        } else {
            map[i] = (i < dst_n) ? (unsigned char)i : 0;
        }
    }
    return true;
}


bool selection_contains_pixel(IMG *img, int x, int y)
{
    if (!img || !g_grid_sel.active) return false;
    int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
    int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x < x1 || x > x2 || y < y1 || y > y2) return false;
    if (!g_grid_sel.is_mask) return true;
    if (x < 0 || y < 0 || x >= g_grid_sel.mask_w || y >= g_grid_sel.mask_h) return false;
    return g_grid_sel.pixel_mask[(size_t)y * g_grid_sel.mask_w + x];
}


void copy_image(bool cut)

{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    /* Cut clears pixels in the source — capture them for pixel-level undo
       (undo_push only saves metadata, which left cut un-undoable). */
    PixelHist cut_snap = {};
    bool cut_captured = cut && pixel_hist_capture(&cut_snap, false);

    ClearPixelClipboard();

    int x1 = 0, y1 = 0, x2 = img->w - 1, y2 = img->h - 1;

    /* If grid selection is active, copy only the selected region */
    if (g_grid_sel.active) {
        x1 = g_grid_sel.x1; y1 = g_grid_sel.y1;
        x2 = g_grid_sel.x2; y2 = g_grid_sel.y2;
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        if (x1 < 0) x1 = 0; if (x1 >= (int)img->w) x1 = img->w - 1;
        if (y1 < 0) y1 = 0; if (y1 >= (int)img->h) y1 = img->h - 1;
        if (x2 < 0) x2 = 0; if (x2 >= (int)img->w) x2 = img->w - 1;
        if (y2 < 0) y2 = 0; if (y2 >= (int)img->h) y2 = img->h - 1;
    }

    int w = (x2 - x1) + 1;
    int h = (y2 - y1) + 1;
    int origin_x = x1;
    int origin_y = y1;
    unsigned short stride = (img->w + 3) & ~3;
    unsigned short clip_stride = (w + 3) & ~3;
    unsigned int size = clip_stride * h;

    /* Copy selected pixel data */
    g_clipboard.data_p = malloc(size);
    if (!g_clipboard.data_p) { if (cut_captured) pixel_hist_free(&cut_snap); return; }

    for (int y = 0; y < h; y++) {
        unsigned char *src = (unsigned char *)img->data_p + (y1 + y) * stride + x1;
        unsigned char *dst = (unsigned char *)g_clipboard.data_p + y * clip_stride;
        if (g_grid_sel.active && g_grid_sel.is_mask) {
            for (int x = 0; x < w; x++) {
                if (g_grid_sel.pixel_mask[(y1 + y) * g_grid_sel.mask_w + (x1 + x)]) {
                    dst[x] = src[x];
                    if (cut) src[x] = 0;
                } else {
                    dst[x] = 0;
                }
            }
        } else {
            memcpy(dst, src, w);
            if (cut) {
                memset(src, 0, w);
            }
        }
    }

    if (cut) {
        if (cut_captured) push_pixel_history_entry(&cut_snap);
        mark_dirty();
        g_img_tex_idx = -2;
        /* Don't drop the marquee on cut, acts more like Photoshop where selection stays */
    }

    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.stride = clip_stride;
    g_clipboard.valid = true;
    g_clipboard.has_meta = true;
    g_clipboard.has_opaque = false;
    g_clipboard.from_cut = cut;
    g_clipboard.origin_x = origin_x;
    g_clipboard.origin_y = origin_y;
    g_clipboard.palnum = img->palnum;
    g_clipboard.anix = img->anix;
    g_clipboard.aniy = img->aniy;
    g_clipboard.anix2 = img->anix2;
    g_clipboard.aniy2 = img->aniy2;
    g_clipboard.aniz2 = img->aniz2;
    g_clipboard.opals = img->opals;
    g_clipboard.has_opaltbl = img->opaltbl_p != NULL;
    memset(g_clipboard.opaltbl, 0, sizeof(g_clipboard.opaltbl));
    if (img->opaltbl_p) memcpy(g_clipboard.opaltbl, img->opaltbl_p, 16);
    g_clipboard.has_palette = false;
    g_clipboard.palette_numc = 0;
    memset(g_clipboard.palette_data, 0, sizeof(g_clipboard.palette_data));
    {
        PAL *clip_pal = get_pal(img->palnum);
        if (clip_pal && clip_pal->data_p && clip_pal->numc > 0) {
            int n = clip_pal->numc;
            if (n > 256) n = 256;
            memcpy(g_clipboard.palette_data, clip_pal->data_p, (size_t)n * 2u);
            g_clipboard.palette_numc = (unsigned short)n;
            g_clipboard.has_palette = true;
        }
    }
    strncpy(g_clipboard.source_name, img->n_s, 15);
    g_clipboard.source_name[15] = '\0';
    strncpy(g_clipboard.src_filename, img->src_filename, sizeof(g_clipboard.src_filename) - 1);
    g_clipboard.src_filename[sizeof(g_clipboard.src_filename) - 1] = '\0';

    /* Tight-crop the clipboard to its non-transparent content bbox. Adobe
       behaviour: a cut/copy carries the visible pixels, not the empty
       transparent padding around them. Without this, a small motif inside a
       large marquee pastes off-center because the rect is bigger than what
       the user actually sees. The crop is applied to *all* copies (not just
       marquee or mask copies) so a full-image copy still drops the empty
       margin most sprite frames have around them. */
    {
        unsigned char *cd = (unsigned char *)g_clipboard.data_p;
        int min_x = w, min_y = h, max_x = -1, max_y = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (cd[y * clip_stride + x] != 0) {
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    if (y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                }
            }
        }
        if (max_x >= 0 && max_y >= 0) {
            g_clipboard.has_opaque = true;
            if (min_x > 0 || min_y > 0 || max_x < w - 1 || max_y < h - 1) {
                int nw = max_x - min_x + 1;
                int nh = max_y - min_y + 1;
                unsigned short nstride = (unsigned short)((nw + 3) & ~3);
                unsigned int nsize = (unsigned int)nstride * nh;
                unsigned char *nbuf = (unsigned char *)malloc(nsize);
                if (nbuf) {
                    memset(nbuf, 0, nsize);
                    for (int y = 0; y < nh; y++) {
                        memcpy(nbuf + y * nstride,
                               cd + (min_y + y) * clip_stride + min_x,
                               nw);
                    }
                    free(g_clipboard.data_p);
                    g_clipboard.data_p = nbuf;
                    g_clipboard.w      = (unsigned short)nw;
                    g_clipboard.h      = (unsigned short)nh;
                    g_clipboard.stride = nstride;
                    g_clipboard.origin_x += min_x;
                    g_clipboard.origin_y += min_y;
                }
            }
        }
        /* If max_x < 0 the selection was entirely transparent; the clipboard
           is left as-is (the user explicitly copied empty pixels — possibly
           intentional for blanking). */
    }
}


void PasteClipboardAsNewImage(void)

{
    if (!g_clipboard.valid || !g_clipboard.data_p || g_clipboard.w == 0 || g_clipboard.h == 0) return;

    /* Adds a whole new image — needs a document snapshot so undo removes it
       (undo_push only restores the selected image's metadata). */
    doc_undo_push();

    IMG *dst = (IMG *)AllocImg();
    if (!dst) return;

    int w = g_clipboard.w;
    int h = g_clipboard.h;
    int src_stride = g_clipboard.stride;
    int dst_stride = (w + 3) & ~3;
    size_t sz = (size_t)dst_stride * h;

    dst->data_p = PoolAlloc(sz);
    if (!dst->data_p) {
        unlink_and_free_img(dst);
        return;
    }

    unsigned char *src = (unsigned char *)g_clipboard.data_p;
    unsigned char *dp = (unsigned char *)dst->data_p;
    for (int y = 0; y < h; y++) {
        memcpy(dp + y * dst_stride, src + y * src_stride, w);
    }

    dst->w = (unsigned short)w;
    dst->h = (unsigned short)h;
    dst->flags = 0;
    dst->palnum = g_clipboard.has_meta ? g_clipboard.palnum
                 : (g_doc->plselected >= 0 ? (unsigned short)g_doc->plselected : 0);
    if (g_doc->palcnt > 0 && dst->palnum >= g_doc->palcnt)
        dst->palnum = (g_doc->plselected >= 0 && (unsigned)g_doc->plselected < g_doc->palcnt)
                    ? (unsigned short)g_doc->plselected : 0;
    dst->opals = (g_clipboard.has_meta && g_clipboard.has_opaltbl)
        ? g_clipboard.opals : (unsigned short)-1;
    if (g_clipboard.has_meta && g_clipboard.has_opaltbl) {
        dst->opaltbl_p = malloc(16);
        if (!dst->opaltbl_p) {
            unlink_and_free_img(dst);
            return;
        }
        memcpy(dst->opaltbl_p, g_clipboard.opaltbl, 16);
    }
    if (g_clipboard.has_meta) dst->aniz2 = g_clipboard.aniz2;
    else clear_secondary_anipoint(dst);

    if (g_clipboard.has_meta) {
        dst->anix  = (unsigned short)((short)g_clipboard.anix  - (short)g_clipboard.origin_x);
        dst->aniy  = (unsigned short)((short)g_clipboard.aniy  - (short)g_clipboard.origin_y);
        if (clipboard_secondary_anipoint_in_use()) {
            dst->anix2 = (unsigned short)((short)g_clipboard.anix2 - (short)g_clipboard.origin_x);
            dst->aniy2 = (unsigned short)((short)g_clipboard.aniy2 - (short)g_clipboard.origin_y);
        } else {
            clear_secondary_anipoint(dst);
        }
        strncpy(dst->src_filename, g_clipboard.src_filename, sizeof(dst->src_filename) - 1);
        dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';
    }

    MakeDerivedImageName(g_clipboard.source_name[0] ? g_clipboard.source_name : "PASTE",
                         g_clipboard.from_cut ? "CUT" : "CPY",
                         dst->n_s);

    g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_palette_nav = false;
    /* The marquee that produced the clipboard is in the *source* sprite's
       coordinate space; leaving it up paints a meaningless highlight over the
       brand-new frame. */
    deselect_all();
    /* Nothing floats after this paste, so hand the arrow keys to the content
       nudge instead of letting them walk the image list off the new sprite. */
    g_content_nudge_img = g_doc->ilselected;
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Pasted clipboard as new sprite: %dx%d. Arrow keys nudge the art "
             "(Shift = 10px); Esc or another sprite stops.", w, h);
    g_restore_msg_timer = 5.0f;
}


void CutSelectionToNewImage(void)

{
    if (g_doc->ilselected < 0) return;
    copy_image(true);
    if (g_clipboard.valid) PasteClipboardAsNewImage();
}


void CopySelectionToNewImage(void)

{
    if (g_doc->ilselected < 0) return;
    copy_image(false);
    if (g_clipboard.valid) PasteClipboardAsNewImage();
}


const PasteBlendMode k_paste_blend_modes[] = {
    PasteBlendMode::Normal,
    PasteBlendMode::Dissolve,
    PasteBlendMode::Darken,
    PasteBlendMode::Multiply,
    PasteBlendMode::ColorBurn,
    PasteBlendMode::LinearBurn,
    PasteBlendMode::Lighten,
    PasteBlendMode::Screen,
    PasteBlendMode::ColorDodge,
    PasteBlendMode::Overlay,
    PasteBlendMode::SoftLight,
    PasteBlendMode::HardLight,
    PasteBlendMode::Difference,
    PasteBlendMode::Exclusion
};

const char *PasteBlendModeName(PasteBlendMode mode)
{
    switch (mode) {
    case PasteBlendMode::Normal:     return "Normal";
    case PasteBlendMode::Dissolve:   return "Dissolve";
    case PasteBlendMode::Darken:     return "Darken";
    case PasteBlendMode::Multiply:   return "Multiply";
    case PasteBlendMode::ColorBurn:  return "Color Burn";
    case PasteBlendMode::LinearBurn: return "Linear Burn";
    case PasteBlendMode::Lighten:    return "Lighten";
    case PasteBlendMode::Screen:     return "Screen";
    case PasteBlendMode::ColorDodge: return "Color Dodge";
    case PasteBlendMode::Overlay:    return "Overlay";
    case PasteBlendMode::SoftLight:  return "Soft Light";
    case PasteBlendMode::HardLight:  return "Hard Light";
    case PasteBlendMode::Difference: return "Difference";
    case PasteBlendMode::Exclusion:  return "Exclusion";
    }
    return "Normal";
}

struct PasteRGB {
    int r, g, b;
};



static int paste_clamp_byte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}


static PasteRGB paste_rgb_from_target_index(const PAL *pal, unsigned char ci)
{
    if (pal && pal->data_p && ci < pal->numc && ci < 256) {
        const unsigned char *pd = (const unsigned char *)pal->data_p;
        unsigned char r = 0, g = 0, b = 0;
        pal_word_to_rgb8(pd + ci * 2, &r, &g, &b);
        return {(int)r, (int)g, (int)b};
    }
    SDL_Color c = g_palette[ci];
    return {(int)c.r, (int)c.g, (int)c.b};
}


static PasteRGB paste_rgb_from_clipboard_index(unsigned char ci, const PAL *target_pal,
                                               const unsigned char pal_map[256],
                                               bool remap_palette)
{
    if (g_clipboard.has_palette && ci < g_clipboard.palette_numc && ci < 256) {
        unsigned char r = 0, g = 0, b = 0;
        pal_word_to_rgb8(g_clipboard.palette_data + ci * 2, &r, &g, &b);
        return {(int)r, (int)g, (int)b};
    }
    unsigned char draw_ci = remap_palette ? pal_map[ci] : ci;
    return paste_rgb_from_target_index(target_pal, draw_ci);
}


static PasteRGB paste_quantize_rgb_to_target(const PAL *target_pal, PasteRGB rgb)
{
    if (!target_pal || !target_pal->data_p || target_pal->numc <= 1) return rgb;
    unsigned short word = rgb_to_word15((unsigned char)paste_clamp_byte(rgb.r),
                                        (unsigned char)paste_clamp_byte(rgb.g),
                                        (unsigned char)paste_clamp_byte(rgb.b));
    int idx = FindNearestPaletteSlot(target_pal, word);
    return paste_rgb_from_target_index(target_pal, (unsigned char)idx);
}


static unsigned int paste_dissolve_hash(int x, int y, unsigned char src_ci)
{
    unsigned int h = (unsigned int)x * 73856093u
                   ^ (unsigned int)y * 19349663u
                   ^ (unsigned int)src_ci * 83492791u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}


static bool paste_dissolve_keeps(int x, int y, unsigned char src_ci, int opacity)
{
    if (opacity >= 100) return true;
    if (opacity <= 0) return false;
    return (int)(paste_dissolve_hash(x, y, src_ci) % 100u) < opacity;
}


static int paste_blend_channel(PasteBlendMode mode, int s, int d)
{
    s = paste_clamp_byte(s);
    d = paste_clamp_byte(d);
    switch (mode) {
    case PasteBlendMode::Darken:
        return (s < d) ? s : d;
    case PasteBlendMode::Multiply:
        return (s * d + 127) / 255;
    case PasteBlendMode::ColorBurn:
        return (s == 0) ? 0 : paste_clamp_byte(255 - ((255 - d) * 255 + s / 2) / s);
    case PasteBlendMode::LinearBurn:
        return paste_clamp_byte(s + d - 255);
    case PasteBlendMode::Lighten:
        return (s > d) ? s : d;
    case PasteBlendMode::Screen:
        return 255 - ((255 - s) * (255 - d) + 127) / 255;
    case PasteBlendMode::ColorDodge:
        return (s >= 255) ? 255 : paste_clamp_byte((d * 255 + (255 - s) / 2) / (255 - s));
    case PasteBlendMode::Overlay:
        return (d < 128)
            ? paste_clamp_byte((2 * s * d + 127) / 255)
            : paste_clamp_byte(255 - (2 * (255 - s) * (255 - d) + 127) / 255);
    case PasteBlendMode::SoftLight: {
        float sf = (float)s / 255.0f;
        float df = (float)d / 255.0f;
        float out = (sf < 0.5f)
            ? (df - (1.0f - 2.0f * sf) * df * (1.0f - df))
            : (df + (2.0f * sf - 1.0f) * (sqrtf(df) - df));
        return paste_clamp_byte((int)(out * 255.0f + 0.5f));
    }
    case PasteBlendMode::HardLight:
        return (s < 128)
            ? paste_clamp_byte((2 * s * d + 127) / 255)
            : paste_clamp_byte(255 - (2 * (255 - s) * (255 - d) + 127) / 255);
    case PasteBlendMode::Difference:
        return (s > d) ? (s - d) : (d - s);
    case PasteBlendMode::Exclusion:
        return paste_clamp_byte(s + d - (2 * s * d + 127) / 255);
    case PasteBlendMode::Normal:
    case PasteBlendMode::Dissolve:
    default:
        return s;
    }
}


static bool paste_composite_rgb(PasteBlendMode mode, PasteRGB src, PasteRGB dst,
                                int opacity, int x, int y, unsigned char src_ci,
                                PasteRGB *out)
{
    if (!out) return false;
    if (opacity <= 0) return false;
    if (opacity > 100) opacity = 100;

    if (mode == PasteBlendMode::Dissolve) {
        if (!paste_dissolve_keeps(x, y, src_ci, opacity)) return false;
        *out = src;
        return true;
    }

    PasteRGB blended = src;
    if (mode != PasteBlendMode::Normal) {
        blended.r = paste_blend_channel(mode, src.r, dst.r);
        blended.g = paste_blend_channel(mode, src.g, dst.g);
        blended.b = paste_blend_channel(mode, src.b, dst.b);
    }

    out->r = paste_clamp_byte((dst.r * (100 - opacity) + blended.r * opacity + 50) / 100);
    out->g = paste_clamp_byte((dst.g * (100 - opacity) + blended.g * opacity + 50) / 100);
    out->b = paste_clamp_byte((dst.b * (100 - opacity) + blended.b * opacity + 50) / 100);
    return true;
}


static unsigned char paste_composite_index(unsigned char src_ci, unsigned char dst_ci,
                                           const PAL *target_pal,
                                           const unsigned char pal_map[256],
                                           bool remap_palette, int x, int y)
{
    if (src_ci == 0) return dst_ci;
    int opacity = g_paste_opacity;
    if (opacity <= 0) return dst_ci;
    if (opacity > 100) opacity = 100;

    unsigned char mapped = remap_palette ? pal_map[src_ci] : src_ci;
    if (g_paste_blend_mode == PasteBlendMode::Normal && opacity >= 100)
        return mapped;

    if (g_paste_blend_mode == PasteBlendMode::Dissolve)
        return paste_dissolve_keeps(x, y, src_ci, opacity) ? mapped : dst_ci;

    if (dst_ci == 0 || !target_pal || !target_pal->data_p || target_pal->numc <= 1)
        return mapped;

    PasteRGB src = paste_rgb_from_clipboard_index(src_ci, target_pal, pal_map, remap_palette);
    PasteRGB dst = paste_rgb_from_target_index(target_pal, dst_ci);
    PasteRGB out;
    if (!paste_composite_rgb(g_paste_blend_mode, src, dst, opacity, x, y, src_ci, &out))
        return dst_ci;

    unsigned short word = rgb_to_word15((unsigned char)out.r,
                                        (unsigned char)out.g,
                                        (unsigned char)out.b);
    return (unsigned char)FindNearestPaletteSlot(target_pal, word);
}



bool paste_preview_rgba(unsigned char src_ci, unsigned char dst_ci,
                        const PAL *target_pal,
                        const unsigned char pal_map[256],
                        bool remap_palette, int x, int y,
                        int *r, int *g, int *b, int *a)
{
    if (src_ci == 0 || !r || !g || !b || !a) return false;
    int opacity = g_paste_opacity;
    if (opacity <= 0) return false;
    if (opacity > 100) opacity = 100;

    unsigned char mapped = remap_palette ? pal_map[src_ci] : src_ci;
    PasteRGB src = (g_paste_blend_mode == PasteBlendMode::Normal)
        ? paste_rgb_from_target_index(target_pal, mapped)
        : paste_rgb_from_clipboard_index(src_ci, target_pal, pal_map, remap_palette);

    if (g_paste_blend_mode == PasteBlendMode::Normal && opacity >= 100) {
        *r = src.r; *g = src.g; *b = src.b; *a = 255;
        return true;
    }

    if (g_paste_blend_mode == PasteBlendMode::Dissolve) {
        if (!paste_dissolve_keeps(x, y, src_ci, opacity)) return false;
        *r = src.r; *g = src.g; *b = src.b; *a = 255;
        return true;
    }

    if (dst_ci == 0) {
        *r = src.r; *g = src.g; *b = src.b;
        *a = (g_paste_blend_mode == PasteBlendMode::Normal)
            ? paste_clamp_byte((opacity * 255 + 50) / 100)
            : 255;
        return true;
    }

    PasteRGB dst = paste_rgb_from_target_index(target_pal, dst_ci);
    PasteRGB out;
    if (!paste_composite_rgb(g_paste_blend_mode, src, dst, opacity, x, y, src_ci, &out))
        return false;
    out = paste_quantize_rgb_to_target(target_pal, out);
    *r = out.r; *g = out.g; *b = out.b; *a = 255;
    return true;
}


void flip_clipboard_horizontal(void)

{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int w = g_clipboard.w, h = g_clipboard.h, stride = g_clipboard.stride;
    unsigned char *d = (unsigned char *)g_clipboard.data_p;
    for (int y = 0; y < h; y++) {
        unsigned char *row = d + (size_t)y * stride;
        for (int x = 0; x < w / 2; x++) {
            unsigned char t = row[x];
            row[x] = row[w - 1 - x];
            row[w - 1 - x] = t;
        }
    }
}


void flip_clipboard_vertical(void)

{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int w = g_clipboard.w, h = g_clipboard.h, stride = g_clipboard.stride;
    unsigned char *d = (unsigned char *)g_clipboard.data_p;
    for (int y = 0; y < h / 2; y++) {
        unsigned char *r0 = d + (size_t)y * stride;
        unsigned char *r1 = d + (size_t)(h - 1 - y) * stride;
        for (int x = 0; x < w; x++) {
            unsigned char t = r0[x]; r0[x] = r1[x]; r1[x] = t;
        }
    }
}


void flatten_img_layer(IMG *img)
{
    SpriteLayer *L = img_layer(img);
    if (!L || !img->data_p) { if (L) { free(img->layer_p); img->layer_p = NULL; } return; }
    if (L->visible) {
        int stride = (img->w + 3) & ~3;
        composite_layer_onto(L, (unsigned char *)img->data_p, img->w, img->h, stride);
    }
    free(img->layer_p);
    img->layer_p = NULL;
    g_img_tex_idx = -2;
}


void delete_img_layer(IMG *img)
{
    if (img && img->layer_p) { free(img->layer_p); img->layer_p = NULL; g_img_tex_idx = -2; }
}


void flip_layer_horizontal(SpriteLayer *L)
{
    if (!L) return;
    unsigned char *p = layer_pixels(L);
    for (int y = 0; y < L->h; y++) {
        unsigned char *row = p + (size_t)y * L->stride;
        for (int x = 0; x < L->w / 2; x++) {
            unsigned char t = row[x]; row[x] = row[L->w - 1 - x]; row[L->w - 1 - x] = t;
        }
    }
}


void flip_layer_vertical(SpriteLayer *L)
{
    if (!L) return;
    unsigned char *p = layer_pixels(L);
    for (int y = 0; y < L->h / 2; y++) {
        unsigned char *r0 = p + (size_t)y * L->stride;
        unsigned char *r1 = p + (size_t)(L->h - 1 - y) * L->stride;
        for (int x = 0; x < L->w; x++) { unsigned char t = r0[x]; r0[x] = r1[x]; r1[x] = t; }
    }
}


void drop_paste_to_layer(void)

{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    int w = g_clipboard.w, h = g_clipboard.h;
    if (w <= 0 || h <= 0) return;
    int stride = (w + 3) & ~3;

    SpriteLayer *L = (SpriteLayer *)malloc(layer_total_bytes(w, h));
    if (!L) return;

    doc_undo_push();

    L->w = w; L->h = h; L->stride = stride;
    L->x = g_pasted.paste_x; L->y = g_pasted.paste_y;
    L->visible = 1;

    unsigned char pal_map[256];
    PAL *target_pal = get_pal(img->palnum);
    bool remap = BuildClipboardPaletteMap(target_pal, pal_map);

    unsigned char *dpix = layer_pixels(L);
    int clip_stride = g_clipboard.stride;
    const unsigned char *sp = (const unsigned char *)g_clipboard.data_p;
    for (int y = 0; y < h; y++) {
        unsigned char *drow = dpix + (size_t)y * stride;
        const unsigned char *srow = sp + (size_t)y * clip_stride;
        for (int x = 0; x < w; x++) {
            unsigned char ci = srow[x];
            drow[x] = (ci && remap) ? pal_map[ci] : ci;
        }
        for (int x = w; x < stride; x++) drow[x] = 0;   /* pad */
    }

    if (img->layer_p) free(img->layer_p);
    img->layer_p = L;

    /* The paste has become the layer; clear the floating paste. */
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_img_tex_idx = -2;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Dropped paste to layer (%dx%d). Edit it in the Sprite Layer panel; "
             "it flattens on save.", w, h);
    g_restore_msg_timer = 5.0f;
}


void apply_pasted_region(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    mark_dirty();

    /* Capture pre-paste pixels so committing a paste is undoable. */
    PixelHist paste_snap = {};
    bool paste_captured = pixel_hist_capture(&paste_snap, false);

    unsigned short stride = (img->w + 3) & ~3;
    unsigned short clip_stride = g_clipboard.stride;
    int px = g_pasted.paste_x, py = g_pasted.paste_y;
    int pw = g_clipboard.w, ph = g_clipboard.h;
    unsigned char pal_map[256];
    PAL *target_pal = get_pal(img->palnum);
    bool remap_palette = BuildClipboardPaletteMap(target_pal, pal_map);
    std::vector<unsigned char> cookie_cut_pixels;
    bool cookie_has_opaque = false;
    if (g_cookie_cut_mode)
        cookie_cut_pixels.assign((size_t)clip_stride * ph, 0);

    /* Adobe-like clipping: allow pasting partially off-canvas */
    int start_x = (px < 0) ? -px : 0;
    int start_y = (py < 0) ? -py : 0;
    int end_x = pw;
    int end_y = ph;

    if (px + pw > (int)img->w) end_x = img->w - px;
    if (py + ph > (int)img->h) end_y = img->h - py;

    /* Copy clipboard data to target location with clipping and transparency support. */
    for (int y = start_y; y < end_y; y++) {
        unsigned char *src = (unsigned char *)g_clipboard.data_p + y * clip_stride;
        unsigned char *dst = (unsigned char *)img->data_p + (py + y) * stride + px + start_x;
        for (int x = start_x; x < end_x; x++) {
            /* 0 remains transparent. Opaque pixels overwrite by default;
               blend/opacity modes composite in RGB and quantize back to the
               target indexed palette. */
            if (src[x] != 0) {
                if (g_cookie_cut_mode) {
                    unsigned char removed = dst[x - start_x];
                    cookie_cut_pixels[(size_t)y * clip_stride + x] = removed;
                    if (removed != 0) cookie_has_opaque = true;
                    dst[x - start_x] = 0;
                } else
                    dst[x - start_x] = paste_composite_index(src[x], dst[x - start_x],
                                                             target_pal, pal_map,
                                                             remap_palette,
                                                             px + x, py + y);
            }
        }
    }

    if (g_cookie_cut_mode) {
        /* The captured source supplied only the stencil. Once applied, the
           clipboard becomes the pixels actually removed from the target and
           carries the target's palette snapshot for correct later remapping. */
        void *cut_data = malloc(cookie_cut_pixels.size());
        if (cut_data) {
            memcpy(cut_data, cookie_cut_pixels.data(), cookie_cut_pixels.size());
            free(g_clipboard.data_p);
            g_clipboard.data_p = cut_data;
            g_clipboard.w = (unsigned short)pw;
            g_clipboard.h = (unsigned short)ph;
            g_clipboard.stride = clip_stride;
            g_clipboard.valid = true;
            g_clipboard.has_meta = true;
            g_clipboard.has_opaque = cookie_has_opaque;
            g_clipboard.from_cut = true;
            g_clipboard.origin_x = px;
            g_clipboard.origin_y = py;
            g_clipboard.palnum = img->palnum;
            g_clipboard.anix = img->anix;
            g_clipboard.aniy = img->aniy;
            g_clipboard.anix2 = img->anix2;
            g_clipboard.aniy2 = img->aniy2;
            g_clipboard.aniz2 = img->aniz2;
            g_clipboard.opals = img->opals;
            g_clipboard.has_opaltbl = img->opaltbl_p != NULL;
            memset(g_clipboard.opaltbl, 0, sizeof(g_clipboard.opaltbl));
            if (img->opaltbl_p) memcpy(g_clipboard.opaltbl, img->opaltbl_p, 16);
            g_clipboard.has_palette = false;
            g_clipboard.palette_numc = 0;
            memset(g_clipboard.palette_data, 0, sizeof(g_clipboard.palette_data));
            if (target_pal && target_pal->data_p && target_pal->numc > 0) {
                int n = target_pal->numc;
                if (n > 256) n = 256;
                memcpy(g_clipboard.palette_data, target_pal->data_p, (size_t)n * 2u);
                g_clipboard.palette_numc = (unsigned short)n;
                g_clipboard.has_palette = true;
            }
            strncpy(g_clipboard.source_name, img->n_s, 15);
            g_clipboard.source_name[15] = '\0';
            strncpy(g_clipboard.src_filename, img->src_filename,
                    sizeof(g_clipboard.src_filename) - 1);
            g_clipboard.src_filename[sizeof(g_clipboard.src_filename) - 1] = '\0';
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     cookie_has_opaque
                         ? "Cookie cut captured %dx%d target pixels with palette %.9s."
                         : "Cookie cut completed, but the target area was transparent.",
                     pw, ph, target_pal ? target_pal->n_s : "");
            g_restore_msg_timer = 4.0f;
        }
        g_cookie_cut_mode = false;
    }
    if (paste_captured) push_pixel_history_entry(&paste_snap);
    g_img_tex_idx = -2;
}

void CaptureCookieCutter(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p) return;

    /* A cutter always comes from the complete visible silhouette, independent
       of a marquee that may happen to be active. copy_image supplies the tight
       non-transparent crop and preserves it across IMG file changes. */
    bool selection_was_active = g_grid_sel.active;
    g_grid_sel.active = false;
    copy_image(false);
    g_grid_sel.active = selection_was_active;
    g_cookie_cut_mode = false;

    if (!g_clipboard.valid || !g_clipboard.has_opaque) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Cookie cutter not captured: frame has no opaque pixels.");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Cookie cutter captured: %dx%d tight silhouette.",
                 (int)g_clipboard.w, (int)g_clipboard.h);
    }
    g_restore_msg_timer = 4.0f;
}

void PlaceCookieCutter(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p ||
        !g_clipboard.has_opaque)
        return;

    /* Do not auto-fit: the captured bounds are exact. Parts positioned beyond
       the target frame are simply clipped when the cutter is committed. */
    undo_push();
    g_cookie_cut_mode = true;
    g_pasted.active = true;
    g_pasted.paste_x = ((int)img->w - (int)g_clipboard.w) / 2;
    g_pasted.paste_y = ((int)img->h - (int)g_clipboard.h) / 2;
    g_pasted.dragging = false;
    g_grid_sel.active = false;
    xform_begin();
}


/* Builds a 256-entry RGB table for the clipboard's own colors (its captured
   palette snapshot if it has one, else its source image's current palette,
   else the global default) so resize sampling can blend in RGB space
   instead of interpolating raw index values. */
static void BuildClipboardResizePalette(ResizeRgb out[256], int *out_count)
{
    int count;
    if (g_clipboard.has_palette && g_clipboard.palette_numc > 0) {
        count = g_clipboard.palette_numc;
        if (count > 256) count = 256;
        for (int i = 0; i < count; i++)
            pal_word_to_rgb8(g_clipboard.palette_data + i * 2,
                             &out[i].r, &out[i].g, &out[i].b);
    } else {
        PAL *src_pal = get_pal(g_clipboard.palnum);
        if (src_pal && src_pal->data_p && src_pal->numc > 1) {
            count = src_pal->numc;
            if (count > 256) count = 256;
            const unsigned char *pd = (const unsigned char *)src_pal->data_p;
            for (int i = 0; i < count; i++)
                pal_word_to_rgb8(pd + i * 2, &out[i].r, &out[i].g, &out[i].b);
        } else {
            count = 256;
            for (int i = 0; i < 256; i++) {
                out[i].r = g_palette[i].r;
                out[i].g = g_palette[i].g;
                out[i].b = g_palette[i].b;
            }
        }
    }
    for (int i = count; i < 256; i++) out[i].r = out[i].g = out[i].b = 0;
    if (out_count) *out_count = count;
}


static void scale_clipboard_to(int nw, int nh)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int sw = g_clipboard.w, sh = g_clipboard.h;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == sw && nh == sh) return;

    unsigned short src_stride = g_clipboard.stride;
    unsigned char *src = (unsigned char *)g_clipboard.data_p;

    /* When "Smooth Scaling" is on, blend in RGB space (using the clipboard's
       own captured palette) and remap back to the nearest matching index,
       rather than picking a raw nearest-neighbor index. This avoids the
       blocky/degraded look from stretching a small paste, since adjacent
       indices aren't necessarily similar colors. When it's off, fall through
       to nearest-neighbor to keep crisp pixel-art edges. */
    unsigned char *dst = NULL;
    unsigned int dst_stride_u = 0;
    if (g_paste_smooth_resize) {
        ResizeRgb pal_rgb[256];
        int pal_count = 0;
        BuildClipboardResizePalette(pal_rgb, &pal_count);
        dst = ResizeIndexedPixelsQuality(src, src_stride, sw, sh,
                                         pal_rgb, pal_count,
                                         nw, nh, false, &dst_stride_u);
    }
    unsigned short dst_stride = (unsigned short)((nw + 3) & ~3);
    if (!dst) {
        dst = (unsigned char *)malloc((size_t)dst_stride * nh);
        if (!dst) return;
        memset(dst, 0, (size_t)dst_stride * nh);
        for (int dy = 0; dy < nh; dy++) {
            int sy_idx = (int)(((long long)dy * sh + sh / 2) / nh);
            if (sy_idx >= sh) sy_idx = sh - 1;
            unsigned char *srow = src + sy_idx * src_stride;
            unsigned char *drow = dst + dy * dst_stride;
            for (int dx = 0; dx < nw; dx++) {
                int sx_idx = (int)(((long long)dx * sw + sw / 2) / nw);
                if (sx_idx >= sw) sx_idx = sw - 1;
                drow[dx] = srow[sx_idx];
            }
        }
    } else {
        dst_stride = (unsigned short)dst_stride_u;
    }

    free(g_clipboard.data_p);
    g_clipboard.data_p = dst;
    g_clipboard.w      = (unsigned short)nw;
    g_clipboard.h      = (unsigned short)nh;
    g_clipboard.stride = dst_stride;
}


static void transform_clipboard_to(int scaled_w, int scaled_h, float angle_deg,
                                   int *out_w, int *out_h)
{
    if (out_w) *out_w = g_clipboard.w;
    if (out_h) *out_h = g_clipboard.h;
    if (!g_clipboard.valid || !g_clipboard.data_p) return;

    int sw = g_clipboard.w;
    int sh = g_clipboard.h;
    if (scaled_w < 1) scaled_w = 1;
    if (scaled_h < 1) scaled_h = 1;

    while (angle_deg <= -180.0f) angle_deg += 360.0f;
    while (angle_deg >   180.0f) angle_deg -= 360.0f;
    const float PI_F = 3.14159265358979323846f;
    float rad = angle_deg * PI_F / 180.0f;
    float c = cosf(rad);
    float s = sinf(rad);

    int dw = scaled_w;
    int dh = scaled_h;
    if (fabsf(angle_deg) > 0.001f) {
        dw = (int)ceilf(fabsf((float)scaled_w * c) + fabsf((float)scaled_h * s));
        dh = (int)ceilf(fabsf((float)scaled_w * s) + fabsf((float)scaled_h * c));
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    unsigned short src_stride = g_clipboard.stride;
    unsigned short dst_stride = (unsigned short)((dw + 3) & ~3);
    unsigned char *src = (unsigned char *)g_clipboard.data_p;
    unsigned char *dst = (unsigned char *)malloc((size_t)dst_stride * dh);
    if (!dst) return;
    memset(dst, 0, (size_t)dst_stride * dh);

    /* When "Smooth Scaling" is on, blend in RGB space and remap to the
       nearest palette index, same as scale_clipboard_to, so rotated/stretched
       pastes don't degrade as fast as raw index-space nearest-neighbor
       sampling. When it's off, keep crisp pixel-art edges. */
    ResizeRgb pal_rgb[256];
    int pal_count = 0;
    BuildClipboardResizePalette(pal_rgb, &pal_count);
    bool use_quality = g_paste_smooth_resize && pal_count > 1;

    float dst_cx = (float)dw * 0.5f;
    float dst_cy = (float)dh * 0.5f;
    float scaled_cx = (float)scaled_w * 0.5f;
    float scaled_cy = (float)scaled_h * 0.5f;

    for (int y = 0; y < dh; y++) {
        unsigned char *drow = dst + y * dst_stride;
        for (int x = 0; x < dw; x++) {
            float dx = ((float)x + 0.5f) - dst_cx;
            float dy = ((float)y + 0.5f) - dst_cy;
            float ux =  c * dx + s * dy + scaled_cx;
            float uy = -s * dx + c * dy + scaled_cy;
            if (ux < 0.0f || uy < 0.0f || ux >= (float)scaled_w || uy >= (float)scaled_h)
                continue;

            float src_ux = ux * (float)sw / (float)scaled_w;
            float src_uy = uy * (float)sh / (float)scaled_h;

            if (use_quality) {
                drow[x] = SampleIndexedBilinear(src, src_stride, sw, sh,
                                                pal_rgb, pal_count,
                                                src_ux, src_uy, false);
                continue;
            }

            int sx_idx = (int)src_ux;
            int sy_idx = (int)src_uy;
            if (sx_idx < 0) sx_idx = 0;
            if (sy_idx < 0) sy_idx = 0;
            if (sx_idx >= sw) sx_idx = sw - 1;
            if (sy_idx >= sh) sy_idx = sh - 1;
            drow[x] = src[sy_idx * src_stride + sx_idx];
        }
    }

    free(g_clipboard.data_p);
    g_clipboard.data_p = dst;
    g_clipboard.w      = (unsigned short)dw;
    g_clipboard.h      = (unsigned short)dh;
    g_clipboard.stride = dst_stride;
    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
}


static void scale_clipboard_to_fit(int max_w, int max_h)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int sw = g_clipboard.w, sh = g_clipboard.h;
    if (sw <= max_w && sh <= max_h) return;

    long long rx = ((long long)max_w << 16) / sw;
    long long ry = ((long long)max_h << 16) / sh;
    long long r  = (rx < ry) ? rx : ry;
    int nw = (int)((long long)sw * r >> 16);
    int nh = (int)((long long)sh * r >> 16);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > max_w) nw = max_w;
    if (nh > max_h) nh = max_h;
    scale_clipboard_to(nw, nh);
}


bool ImageCanvasActive(void)
{
    return !g_world_state.enabled && !g_seqscr_workspace &&
           !g_reactions_workspace && !AnipointLink().enabled;
}

void select_all(void)

{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || img->w == 0 || img->h == 0) return;
    g_active_tool        = ActiveTool::Marquee;
    g_grid_sel.active    = true;
    g_grid_sel.dragging  = false;
    g_grid_sel.is_mask   = false;
    g_grid_sel.pixel_mask.clear();
    g_grid_sel.x1 = 0;             g_grid_sel.y1 = 0;
    g_grid_sel.x2 = img->w - 1;    g_grid_sel.y2 = img->h - 1;
}


void deselect_all(void)

{
    g_grid_sel.active   = false;
    g_grid_sel.dragging = false;
    g_grid_sel.is_mask  = false;
    g_grid_sel.pixel_mask.clear();
    g_lasso_points.clear();
    g_selection_add_drag = false;
    g_selection_add_mask.clear();
    g_selection_add_mask_w = g_selection_add_mask_h = 0;
}


void invert_selection(void)

{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || img->w == 0 || img->h == 0) return;
    int sw = img->w, sh = img->h;

    if (!g_grid_sel.active) {
        /* Inverting "nothing" = "everything". */
        select_all();
        return;
    }

    /* Build a fresh mask covering the whole sprite, flipping bits inside the
       current selection. Promotes rect selections to masks transparently. */
    std::vector<bool> new_mask((size_t)sw * sh, false);

    int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
    int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0; if (x2 >= sw) x2 = sw - 1;
    if (y1 < 0) y1 = 0; if (y2 >= sh) y2 = sh - 1;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            bool inside;
            if (g_grid_sel.is_mask) {
                inside = g_grid_sel.pixel_mask[(size_t)y * g_grid_sel.mask_w + x];
            } else {
                inside = (x >= x1 && x <= x2 && y >= y1 && y <= y2);
            }
            new_mask[(size_t)y * sw + x] = !inside;
        }
    }

    g_grid_sel.active     = true;
    g_grid_sel.is_mask    = true;
    g_grid_sel.mask_w     = sw;
    g_grid_sel.mask_h     = sh;
    g_grid_sel.pixel_mask = std::move(new_mask);
    g_grid_sel.x1 = 0; g_grid_sel.y1 = 0;
    g_grid_sel.x2 = sw - 1; g_grid_sel.y2 = sh - 1;
}


static bool selection_bbox_from_mask(const std::vector<bool> &mask, int sw, int sh,
                                     int *x1, int *y1, int *x2, int *y2)
{
    int min_x = sw, min_y = sh, max_x = -1, max_y = -1;
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            if (!mask[(size_t)y * sw + x]) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return false;
    if (x1) *x1 = min_x; if (y1) *y1 = min_y;
    if (x2) *x2 = max_x; if (y2) *y2 = max_y;
    return true;
}


static bool selection_current_to_mask(int sw, int sh, std::vector<bool> *out)
{
    if (!out) return false;
    out->assign((size_t)sw * sh, false);
    if (!g_grid_sel.active) return false;

    if (g_grid_sel.is_mask &&
        g_grid_sel.mask_w == sw && g_grid_sel.mask_h == sh &&
        g_grid_sel.pixel_mask.size() == (size_t)sw * sh) {
        *out = g_grid_sel.pixel_mask;
    } else {
        int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
        int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        if (x1 < 0) x1 = 0; if (x2 >= sw) x2 = sw - 1;
        if (y1 < 0) y1 = 0; if (y2 >= sh) y2 = sh - 1;
        if (x1 > x2 || y1 > y2) return false;
        for (int y = y1; y <= y2; y++)
            for (int x = x1; x <= x2; x++)
                (*out)[(size_t)y * sw + x] = true;
    }

    int bx1, by1, bx2, by2;
    return selection_bbox_from_mask(*out, sw, sh, &bx1, &by1, &bx2, &by2);
}


static void selection_commit_mask(int sw, int sh, const std::vector<bool> &mask)
{
    int x1, y1, x2, y2;
    if (!selection_bbox_from_mask(mask, sw, sh, &x1, &y1, &x2, &y2)) {
        deselect_all();
        return;
    }
    g_grid_sel.active = true;
    g_grid_sel.dragging = false;
    g_grid_sel.is_mask = true;
    g_grid_sel.mask_w = sw;
    g_grid_sel.mask_h = sh;
    g_grid_sel.pixel_mask = mask;
    g_grid_sel.x1 = x1; g_grid_sel.y1 = y1;
    g_grid_sel.x2 = x2; g_grid_sel.y2 = y2;
}


static void selection_apply_mask(int sw, int sh, std::vector<bool> mask, bool add)
{
    if (add) {
        std::vector<bool> base;
        if (selection_current_to_mask(sw, sh, &base)) {
            for (size_t i = 0; i < mask.size() && i < base.size(); i++)
                mask[i] = mask[i] || base[i];
        }
    }
    selection_commit_mask(sw, sh, mask);
}


void selection_begin_add_drag(int sw, int sh, bool add)
{
    g_selection_add_drag = false;
    g_selection_add_mask.clear();
    g_selection_add_mask_w = g_selection_add_mask_h = 0;
    if (!add) return;
    if (selection_current_to_mask(sw, sh, &g_selection_add_mask)) {
        g_selection_add_drag = true;
        g_selection_add_mask_w = sw;
        g_selection_add_mask_h = sh;
    }
}


void selection_finish_add_drag(int sw, int sh)
{
    if (!g_selection_add_drag ||
        g_selection_add_mask_w != sw || g_selection_add_mask_h != sh ||
        g_selection_add_mask.size() != (size_t)sw * sh) {
        g_selection_add_drag = false;
        g_selection_add_mask.clear();
        return;
    }

    std::vector<bool> current;
    if (selection_current_to_mask(sw, sh, &current)) {
        for (size_t i = 0; i < current.size(); i++)
            current[i] = current[i] || g_selection_add_mask[i];
        selection_commit_mask(sw, sh, current);
    }
    g_selection_add_drag = false;
    g_selection_add_mask.clear();
}


void paste_image(void)

{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    g_cookie_cut_mode = false;

    /* Before anything is composited: give the target palette the clipboard's
       missing colors if it has indices to spare. Both the live preview and the
       commit remap through BuildClipboardPaletteMap, so widening the palette
       here is what turns "nearest existing color" into an exact match for the
       rest of the paste. Done first so its document snapshot sits below the
       paste's own undo entry. */
    if (g_paste_import_colors) {
        int unmatched = 0;
        int added = ImportClipboardColorsIntoImagePalette(img, &unmatched);
        if (added > 0) {
            PAL *pal = get_pal(img->palnum);
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     unmatched > 0
                         ? "Added %d pasted color%s to %.9s; %d had no free slot."
                         : "Added %d pasted color%s to %.9s.",
                     added, added == 1 ? "" : "s",
                     pal ? pal->n_s : "", unmatched);
            g_restore_msg_timer = 4.0f;
        }
    }

    undo_push();

    /* If the clipboard is larger than the target sprite, nearest-neighbor
       downscale it to fit (preserving aspect ratio). Without this, the user
       has to manually clip away anything that hangs off the edge. */
    int orig_w = g_clipboard.w, orig_h = g_clipboard.h;
    if (g_clipboard.w > img->w || g_clipboard.h > img->h) {
        scale_clipboard_to_fit(img->w, img->h);
        if (g_clipboard.w != orig_w || g_clipboard.h != orig_h) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Paste scaled to fit: %dx%d -> %dx%d",
                     orig_w, orig_h,
                     (int)g_clipboard.w, (int)g_clipboard.h);
            g_restore_msg_timer = 3.0f;
        }
    }

    /* Show paste boundary centered on the target so the floating sprite is
       immediately visible and ready to resize/move. */
    g_pasted.active = true;
    g_pasted.paste_x = ((int)img->w - (int)g_clipboard.w) / 2;
    g_pasted.paste_y = ((int)img->h - (int)g_clipboard.h) / 2;
    if (g_pasted.paste_x < 0) g_pasted.paste_x = 0;
    if (g_pasted.paste_y < 0) g_pasted.paste_y = 0;
    g_pasted.dragging = false;

    /* Clear grid selection since paste is now active */
    g_grid_sel.active = false;

    /* Auto-enter Free Transform so the user can immediately resize without
       having to press Ctrl+T as a separate step. Enter / Ctrl+T / click
       outside the rect all still commit the transform and then commit the
       paste; Esc reverts the transform first, then a second Esc cancels
       the paste entirely. */
    xform_begin();
}


void xform_begin(void)
{
    if (!g_pasted.active || !g_clipboard.valid) return;
    if (g_xform.active) return; /* already transforming */
    g_xform.active   = true;
    g_xform.rx       = g_pasted.paste_x;
    g_xform.ry       = g_pasted.paste_y;
    g_xform.rw       = g_clipboard.w;
    g_xform.rh       = g_clipboard.h;
    g_xform.start_x  = g_xform.rx;
    g_xform.start_y  = g_xform.ry;
    g_xform.start_w  = g_xform.rw;
    g_xform.start_h  = g_xform.rh;
    g_xform.angle_deg = 0.0f;
    g_xform.start_angle_deg = 0.0f;
    g_xform.handle   = TransformHandle::None;
    g_xform.ref_aspect = (g_xform.rh > 0) ? (float)g_xform.rw / (float)g_xform.rh : 1.0f;
}


/* Cancel transform — revert rect to its pre-transform geometry; the paste
   stays floating at its original size. */
void xform_cancel(void)
{
    if (!g_xform.active) return;
    g_pasted.paste_x = g_xform.start_x;
    g_pasted.paste_y = g_xform.start_y;
    g_xform.angle_deg = g_xform.start_angle_deg;
    g_xform.active   = false;
    g_xform.handle   = TransformHandle::None;
}


void xform_commit(void)
{
    if (!g_xform.active) return;
    int nw = g_xform.rw, nh = g_xform.rh;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    int out_w = nw;
    int out_h = nh;
    if (nw != (int)g_clipboard.w || nh != (int)g_clipboard.h ||
        fabsf(g_xform.angle_deg) > 0.001f) {
        transform_clipboard_to(nw, nh, g_xform.angle_deg, &out_w, &out_h);
    }
    float cx = (float)g_xform.rx + (float)nw * 0.5f;
    float cy = (float)g_xform.ry + (float)nh * 0.5f;
    g_pasted.paste_x = (int)floorf(cx - (float)out_w * 0.5f + 0.5f);
    g_pasted.paste_y = (int)floorf(cy - (float)out_h * 0.5f + 0.5f);
    g_xform.active   = false;
    g_xform.handle   = TransformHandle::None;
}

bool clipboard_secondary_anipoint_in_use(void)
{
    return secondary_anipoint_words_in_use(g_clipboard.anix2,
                                           g_clipboard.aniy2,
                                           g_clipboard.aniz2);
}





/* =========================================================
   Sprite Resizing and Transformations (Logic)
   ========================================================= */

int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

int round_to_int(double v)
{
    return (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

void default_anipoints_to_center(IMG *img)
{
    if (!img) return;
    img->anix = signed_to_img_word((int)img->w / 2);
    img->aniy = signed_to_img_word((int)img->h / 2);
    clear_secondary_anipoint(img);
}

int scaled_coord(unsigned short coord, int old_dim, int new_dim)
{
    if (old_dim <= 0) return (int)(short)coord;
    return round_to_int((double)(short)coord * (double)new_dim / (double)old_dim);
}

void resize_sync_scale_from_dims(void)
{
    if (g_resize_source_w > 0)
        g_resize_scale_x = clamp_int(round_to_int((double)g_resize_w * 100.0 / (double)g_resize_source_w), 1, 3200);
    if (g_resize_source_h > 0)
        g_resize_scale_y = clamp_int(round_to_int((double)g_resize_h * 100.0 / (double)g_resize_source_h), 1, 3200);
}

void resize_sync_dims_from_scale(void)
{
    if (g_resize_source_w > 0)
        g_resize_w = clamp_int(round_to_int((double)g_resize_source_w * (double)g_resize_scale_x / 100.0), 1, 4096);
    if (g_resize_source_h > 0)
        g_resize_h = clamp_int(round_to_int((double)g_resize_source_h * (double)g_resize_scale_y / 100.0), 1, 4096);
}

bool trim_image_to_content(IMG *img, bool shrink_empty,
                                  int *out_trim_x, int *out_trim_y,
                                  bool adjust_hitbox = true)
{
    if (out_trim_x) *out_trim_x = 0;
    if (out_trim_y) *out_trim_y = 0;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *src = (unsigned char *)img->data_p;
    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (src[y * stride + x] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    if (max_x < 0) {
        if (!shrink_empty || (w == 1 && h == 1)) return false;
        unsigned char *dst = (unsigned char *)PoolAlloc(4);
        if (!dst) return false;
        free(img->data_p);
        img->data_p = dst;
        img->w = 1;
        img->h = 1;
        img->anix = 0;
        img->aniy = 0;
        clear_secondary_anipoint(img);
        return true;
    }

    if (min_x == 0 && min_y == 0 && max_x == w - 1 && max_y == h - 1) return false;

    int new_w = max_x - min_x + 1;
    int new_h = max_y - min_y + 1;
    int new_stride = (new_w + 3) & ~3;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)new_stride * new_h);
    if (!dst) return false;
    for (int y = 0; y < new_h; y++)
        memcpy(dst + y * new_stride, src + (y + min_y) * stride + min_x, new_w);

    free(img->data_p);
    img->data_p = dst;
    img->w = (unsigned short)new_w;
    img->h = (unsigned short)new_h;
    img->anix = signed_to_img_word((int)(short)img->anix - min_x);
    img->aniy = signed_to_img_word((int)(short)img->aniy - min_y);
    if (secondary_anipoint_in_use(img)) {
        img->anix2 = signed_to_img_word((int)(short)img->anix2 - min_x);
        img->aniy2 = signed_to_img_word((int)(short)img->aniy2 - min_y);
    }
    if (adjust_hitbox) {
        g_hitbox_x -= min_x;
        g_hitbox_y -= min_y;
    }
    if (out_trim_x) *out_trim_x = min_x;
    if (out_trim_y) *out_trim_y = min_y;
    return true;
}

const char *sprite_transform_name(SpriteTransformOp op)
{
    switch (op) {
    case SpriteTransformOp::FlipHorizontal: return "Flipped horizontal";
    case SpriteTransformOp::FlipVertical:   return "Flipped vertical";
    case SpriteTransformOp::Rotate90CW:     return "Rotated 90 CW";
    case SpriteTransformOp::Rotate90CCW:    return "Rotated 90 CCW";
    case SpriteTransformOp::Rotate180:      return "Rotated 180";
    }
    return "Transformed";
}

bool sprite_transform_preserves_anipoints(SpriteTransformOp op)
{
    return op == SpriteTransformOp::Rotate90CW ||
           op == SpriteTransformOp::Rotate90CCW;
}

void transform_hitbox(SpriteTransformOp op, int old_w, int old_h)
{
    if (g_hitbox_w <= 0 || g_hitbox_h <= 0) return;

    int x = g_hitbox_x;
    int y = g_hitbox_y;
    int w = g_hitbox_w;
    int h = g_hitbox_h;

    switch (op) {
    case SpriteTransformOp::FlipHorizontal:
        g_hitbox_x = old_w - (x + w);
        break;
    case SpriteTransformOp::FlipVertical:
        g_hitbox_y = old_h - (y + h);
        break;
    case SpriteTransformOp::Rotate90CW:
        g_hitbox_x = old_h - (y + h);
        g_hitbox_y = x;
        g_hitbox_w = h;
        g_hitbox_h = w;
        break;
    case SpriteTransformOp::Rotate90CCW:
        g_hitbox_x = y;
        g_hitbox_y = old_w - (x + w);
        g_hitbox_w = h;
        g_hitbox_h = w;
        break;
    case SpriteTransformOp::Rotate180:
        g_hitbox_x = old_w - (x + w);
        g_hitbox_y = old_h - (y + h);
        break;
    }
}

void transform_anipoint(SpriteTransformOp op, int old_w, int old_h,
                               unsigned short *x, unsigned short *y)
{
    int sx = (int)(short)*x;
    int sy = (int)(short)*y;
    int dx = sx;
    int dy = sy;

    switch (op) {
    case SpriteTransformOp::FlipHorizontal:
        dx = old_w - sx;
        dy = sy;
        break;
    case SpriteTransformOp::FlipVertical:
        dx = sx;
        dy = old_h - sy;
        break;
    case SpriteTransformOp::Rotate90CW:
        dx = old_h - sy;
        dy = sx;
        break;
    case SpriteTransformOp::Rotate90CCW:
        dx = sy;
        dy = old_w - sx;
        break;
    case SpriteTransformOp::Rotate180:
        dx = old_w - sx;
        dy = old_h - sy;
        break;
    }

    *x = signed_to_img_word(dx);
    *y = signed_to_img_word(dy);
}

int rounded_half_delta(int current_dim, int reference_dim)
{
    return round_to_int(((double)current_dim - (double)reference_dim) * 0.5);
}

bool timeline_image_locked(int img_idx)
{
    for (int i = 0; i < 2; i++) {
        if (g_timeline_composite_locked[i] && g_timeline_composite[i] == img_idx)
            return true;
    }
    return false;
}

int locked_timeline_anchor_position(void)
{
    if (!TimelineAnyCompositeLocked()) return -1;

    for (int slot = 0; slot < 2; slot++) {
        if (g_timeline_composite_locked[slot] &&
            g_timeline_composite[slot] == g_doc->ilselected) {
            return TimelineFramePosition(g_timeline_composite[slot]);
        }
    }
    for (int slot = 0; slot < 2; slot++) {
        if (g_timeline_composite_locked[slot])
            return TimelineFramePosition(g_timeline_composite[slot]);
    }
    return -1;
}

int AutoCalculateTimelineAnipointsFromLock(void)
{
    int n = (int)g_timeline_frames.size();
    int anchor_pos = locked_timeline_anchor_position();
    if (n < 2 || anchor_pos < 0 || anchor_pos >= n) return 0;

    struct FrameAnipointState {
        int img_idx;
        int w, h;
        int anix, aniy;
        bool valid;
        bool locked;
    };

    std::vector<FrameAnipointState> states;
    states.reserve(g_timeline_frames.size());
    for (int img_idx : g_timeline_frames) {
        IMG *img = get_img(img_idx);
        FrameAnipointState st = {};
        st.img_idx = img_idx;
        st.valid = img && img->w > 0 && img->h > 0;
        st.locked = timeline_image_locked(img_idx);
        if (st.valid) {
            st.w = img->w;
            st.h = img->h;
            st.anix = (int)(short)img->anix;
            st.aniy = (int)(short)img->aniy;
        }
        states.push_back(st);
    }
    if (!states[anchor_pos].valid) return 0;

    for (int i = anchor_pos + 1; i < n; i++) {
        if (!states[i].valid || !states[i - 1].valid || states[i].locked) continue;
        states[i].anix = states[i - 1].anix + rounded_half_delta(states[i].w, states[i - 1].w);
        states[i].aniy = states[i - 1].aniy + rounded_half_delta(states[i].h, states[i - 1].h);
    }

    for (int i = anchor_pos - 1; i >= 0; i--) {
        if (!states[i].valid || !states[i + 1].valid || states[i].locked) continue;
        states[i].anix = states[i + 1].anix + rounded_half_delta(states[i].w, states[i + 1].w);
        states[i].aniy = states[i + 1].aniy + rounded_half_delta(states[i].h, states[i + 1].h);
    }

    int changed = 0;
    for (const FrameAnipointState &st : states) {
        if (!st.valid || st.locked) continue;
        IMG *img = get_img(st.img_idx);
        if (!img) continue;
        if ((short)img->anix != st.anix || (short)img->aniy != st.aniy)
            changed++;
    }
    if (changed == 0) return 0;
    if (!doc_undo_push()) return 0;

    for (const FrameAnipointState &st : states) {
        if (!st.valid || st.locked) continue;
        IMG *img = get_img(st.img_idx);
        if (!img) continue;
        img->anix = signed_to_img_word(st.anix);
        img->aniy = signed_to_img_word(st.aniy);
    }
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    return changed;
}

struct ResizeImageResult {
    int old_w = 0;
    int old_h = 0;
    int trim_x = 0;
    int trim_y = 0;
    bool did_trim = false;
};

static int ResizeImageInPlaceNoUndo(IMG *img, int nw, int nh,
                                    SpriteResizeMode mode,
                                    bool trim_bounds,
                                    bool adjust_hitbox,
                                    const ResizeRgb fallback_rgb[256],
                                    ResizeImageResult *result)
{
    if (result) *result = ResizeImageResult();
    if (!img || !img->data_p || img->w == 0 || img->h == 0)
        return -1;

    nw = clamp_int(nw, 1, 4096);
    nh = clamp_int(nh, 1, 4096);

    int old_w = img->w;
    int old_h = img->h;
    unsigned short old_anix = img->anix;
    unsigned short old_aniy = img->aniy;
    unsigned short old_anix2 = img->anix2;
    unsigned short old_aniy2 = img->aniy2;
    unsigned short old_aniz2 = img->aniz2;

    if (result) {
        result->old_w = old_w;
        result->old_h = old_h;
    }

    bool optimize_bytes = (mode == SpriteResizeMode::QualitySmallBytes);
    bool use_quality = (mode != SpriteResizeMode::IndexNearest);
    if (nw == old_w && nh == old_h && !(trim_bounds || optimize_bytes))
        return 0;

    PAL *pal = get_pal(img->palnum);
    unsigned int new_stride = 0;
    unsigned char *new_pixels = use_quality
        ? ResizeSpritePixelsQuality(img, pal, fallback_rgb, nw, nh,
                                    optimize_bytes, &new_stride)
        : ResizeSpritePixelsNearest(img, nw, nh, &new_stride);
    if (!new_pixels) return -1;

    free(img->data_p);
    img->data_p = new_pixels;
    img->w = (unsigned short)nw;
    img->h = (unsigned short)nh;
    img->anix = signed_to_img_word(scaled_coord(old_anix, old_w, nw));
    img->aniy = signed_to_img_word(scaled_coord(old_aniy, old_h, nh));
    if (secondary_anipoint_words_in_use(old_anix2, old_aniy2, old_aniz2)) {
        img->anix2 = signed_to_img_word(scaled_coord(old_anix2, old_w, nw));
        img->aniy2 = signed_to_img_word(scaled_coord(old_aniy2, old_h, nh));
        img->aniz2 = old_aniz2;
    } else {
        clear_secondary_anipoint(img);
    }

    if (adjust_hitbox && g_hitbox_w > 0 && g_hitbox_h > 0) {
        g_hitbox_x = scaled_coord((unsigned short)(short)g_hitbox_x, old_w, nw);
        g_hitbox_y = scaled_coord((unsigned short)(short)g_hitbox_y, old_h, nh);
        g_hitbox_w = clamp_int(round_to_int((double)g_hitbox_w * (double)nw / (double)old_w), 1, 4096);
        g_hitbox_h = clamp_int(round_to_int((double)g_hitbox_h * (double)nh / (double)old_h), 1, 4096);
    }

    int trim_x = 0;
    int trim_y = 0;
    bool did_trim = false;
    if (trim_bounds || optimize_bytes)
        did_trim = trim_image_to_content(img, optimize_bytes, &trim_x, &trim_y,
                                         adjust_hitbox);

    if (result) {
        result->trim_x = trim_x;
        result->trim_y = trim_y;
        result->did_trim = did_trim;
    }

    (void)new_stride;
    return 1;
}

static void CollectResizeSubframesForParent(int parent_idx,
                                            std::vector<int> *out)
{
    if (!out) return;
    out->clear();
    IMG *parent = get_img(parent_idx);
    if (!parent) return;

    std::string parent_name = img_name_string(parent);
    if (parent_name.empty()) return;
    std::string parent_src =
        parent->src_filename[0] ? parent->src_filename : "Workspace";

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (idx == parent_idx) continue;
        if (!img->data_p || img->w == 0 || img->h == 0) continue;

        std::string src =
            img->src_filename[0] ? img->src_filename : "Workspace";
        if (src != parent_src) continue;

        std::string child_name = img_name_string(img);
        bool belongs_to_parent =
            (InferSubframeParentName(child_name.c_str()) == parent_name);
        if (!belongs_to_parent) {
            std::string numbered_parent;
            belongs_to_parent =
                strip_trailing_sequence_digits(child_name, &numbered_parent) &&
                numbered_parent == parent_name;
        }

        if (belongs_to_parent)
            out->push_back(idx);
    }
}

static bool ResizeSelectedSpriteWithSubframes(IMG *img, int img_idx,
                                              int nw, int nh,
                                              SpriteResizeMode mode,
                                              bool trim_bounds,
                                              const std::vector<int> &child_indices)
{
    if (!img || child_indices.empty()) return false;

    struct ChildPlan {
        IMG *img;
        int idx;
        int old_w;
        int old_h;
        int offset_x;
        int offset_y;
    };

    int parent_old_w = img->w;
    int parent_old_h = img->h;
    int parent_old_anix = (int)(short)img->anix;
    int parent_old_aniy = (int)(short)img->aniy;

    std::vector<ChildPlan> children;
    children.reserve(child_indices.size());
    for (int child_idx : child_indices) {
        IMG *child = get_img(child_idx);
        if (!child || !child->data_p || child->w == 0 || child->h == 0)
            continue;
        ChildPlan plan = {};
        plan.img = child;
        plan.idx = child_idx;
        plan.old_w = child->w;
        plan.old_h = child->h;
        plan.offset_x = parent_old_anix - (int)(short)child->anix;
        plan.offset_y = parent_old_aniy - (int)(short)child->aniy;
        children.push_back(plan);
    }
    if (children.empty()) return false;

    bool optimize_bytes = (mode == SpriteResizeMode::QualitySmallBytes);
    bool same_size = (nw == parent_old_w && nh == parent_old_h);
    if (same_size && !(trim_bounds || optimize_bytes))
        return false;

    if (!doc_undo_push()) return false;

    ResizeRgb fallback_rgb[256];
    for (int i = 0; i < 256; i++) {
        fallback_rgb[i].r = g_palette[i].r;
        fallback_rgb[i].g = g_palette[i].g;
        fallback_rgb[i].b = g_palette[i].b;
    }

    ResizeImageResult parent_result;
    int parent_status = ResizeImageInPlaceNoUndo(img, nw, nh, mode,
                                                 trim_bounds, true,
                                                 fallback_rgb,
                                                 &parent_result);
    if (parent_status <= 0)
        return false;

    int parent_scaled_anix = scaled_coord((unsigned short)(short)parent_old_anix,
                                          parent_old_w, nw);
    int parent_scaled_aniy = scaled_coord((unsigned short)(short)parent_old_aniy,
                                          parent_old_h, nh);

    int changed_children = 0;
    for (const ChildPlan &child_plan : children) {
        IMG *child = child_plan.img;
        int child_nw = clamp_int(round_to_int((double)child_plan.old_w *
                                              (double)nw /
                                              (double)parent_old_w), 1, 4096);
        int child_nh = clamp_int(round_to_int((double)child_plan.old_h *
                                              (double)nh /
                                              (double)parent_old_h), 1, 4096);

        ResizeImageResult child_result;
        int child_status = ResizeImageInPlaceNoUndo(child, child_nw, child_nh,
                                                    mode, trim_bounds, false,
                                                    fallback_rgb,
                                                    &child_result);
        if (child_status < 0)
            continue;

        int scaled_offset_x = round_to_int((double)child_plan.offset_x *
                                           (double)nw /
                                           (double)parent_old_w);
        int scaled_offset_y = round_to_int((double)child_plan.offset_y *
                                           (double)nh /
                                           (double)parent_old_h);
        unsigned short new_anix = signed_to_img_word(parent_scaled_anix -
                                                     scaled_offset_x -
                                                     child_result.trim_x);
        unsigned short new_aniy = signed_to_img_word(parent_scaled_aniy -
                                                     scaled_offset_y -
                                                     child_result.trim_y);
        bool anipoint_changed =
            child->anix != new_anix || child->aniy != new_aniy;
        child->anix = new_anix;
        child->aniy = new_aniy;

        if (child_status > 0 || anipoint_changed) {
            InvalidateThumb(child_plan.idx);
            changed_children++;
        }
    }

    mark_dirty();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    deselect_all();
    InvalidateThumb(img_idx);

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             parent_result.did_trim
                 ? "Resized %s and %d subframe%s: %dx%d -> %dx%d (trimmed %d,%d)."
                 : "Resized %s and %d subframe%s: %dx%d -> %dx%d.",
             img->n_s,
             changed_children,
             changed_children == 1 ? "" : "s",
             parent_old_w, parent_old_h, (int)img->w, (int)img->h,
             parent_result.trim_x, parent_result.trim_y);
    g_restore_msg_timer = 4.0f;
    return true;
}

bool TransformSelectedSprite(SpriteTransformOp op)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int old_w = img->w;
    int old_h = img->h;
    int old_stride = (old_w + 3) & ~3;
    int new_w = old_w;
    int new_h = old_h;
    if (op == SpriteTransformOp::Rotate90CW || op == SpriteTransformOp::Rotate90CCW) {
        new_w = old_h;
        new_h = old_w;
    }

    PixelHist snap = {};
    if (!pixel_hist_capture(&snap, true)) return false;

    unsigned int new_stride = ((unsigned int)new_w + 3) & ~3u;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)new_stride * new_h);
    if (!dst) {
        pixel_hist_free(&snap);
        return false;
    }
    memset(dst, 0, (size_t)new_stride * new_h);

    const unsigned char *src = (const unsigned char *)img->data_p;
    for (int sy = 0; sy < old_h; sy++) {
        for (int sx = 0; sx < old_w; sx++) {
            int dx = sx, dy = sy;
            switch (op) {
            case SpriteTransformOp::FlipHorizontal:
                dx = old_w - 1 - sx;
                dy = sy;
                break;
            case SpriteTransformOp::FlipVertical:
                dx = sx;
                dy = old_h - 1 - sy;
                break;
            case SpriteTransformOp::Rotate90CW:
                dx = old_h - 1 - sy;
                dy = sx;
                break;
            case SpriteTransformOp::Rotate90CCW:
                dx = sy;
                dy = old_w - 1 - sx;
                break;
            case SpriteTransformOp::Rotate180:
                dx = old_w - 1 - sx;
                dy = old_h - 1 - sy;
                break;
            }
            dst[dy * new_stride + dx] = src[sy * old_stride + sx];
        }
    }

    free(img->data_p);
    img->data_p = dst;
    img->w = (unsigned short)new_w;
    img->h = (unsigned short)new_h;
    if (!sprite_transform_preserves_anipoints(op)) {
        transform_anipoint(op, old_w, old_h, &img->anix, &img->aniy);
        if (secondary_anipoint_in_use(img))
            transform_anipoint(op, old_w, old_h, &img->anix2, &img->aniy2);
    }
    transform_hitbox(op, old_w, old_h);

    push_pixel_history_entry(&snap);
    mark_dirty();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    deselect_all();
    InvalidateThumb(g_doc->ilselected);

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             sprite_transform_preserves_anipoints(op)
                 ? "%s: %s (%dx%d -> %dx%d), anipoints preserved."
                 : "%s: %s (%dx%d -> %dx%d).",
             sprite_transform_name(op), img->n_s, old_w, old_h, new_w, new_h);
    g_restore_msg_timer = 4.0f;
    return true;
}

bool ResizeSelectedSprite(int nw, int nh, SpriteResizeMode mode, bool trim_bounds)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;
    nw = clamp_int(nw, 1, 4096);
    nh = clamp_int(nh, 1, 4096);

    int old_w = img->w;
    int old_h = img->h;
    bool optimize_bytes = (mode == SpriteResizeMode::QualitySmallBytes);
    bool use_quality = (mode != SpriteResizeMode::IndexNearest);

    std::vector<int> child_indices;
    CollectResizeSubframesForParent(g_doc->ilselected, &child_indices);
    if (!child_indices.empty()) {
        return ResizeSelectedSpriteWithSubframes(img, g_doc->ilselected,
                                                 nw, nh, mode, trim_bounds,
                                                 child_indices);
    }

    if (nw == old_w && nh == old_h && !(trim_bounds || optimize_bytes)) return false;

    PixelHist snap = {};
    if (!pixel_hist_capture(&snap, true)) return false;

    PAL *pal = get_pal(img->palnum);
    unsigned int new_stride = 0;
    unsigned char *new_pixels = NULL;
    if (use_quality) {
        ResizeRgb fallback_rgb[256];
        for (int i = 0; i < 256; i++) {
            fallback_rgb[i].r = g_palette[i].r;
            fallback_rgb[i].g = g_palette[i].g;
            fallback_rgb[i].b = g_palette[i].b;
        }
        new_pixels = ResizeSpritePixelsQuality(img, pal, fallback_rgb, nw, nh,
                                               optimize_bytes, &new_stride);
    } else {
        new_pixels = ResizeSpritePixelsNearest(img, nw, nh, &new_stride);
    }
    if (!new_pixels) {
        pixel_hist_free(&snap);
        return false;
    }

    free(img->data_p);
    img->data_p = new_pixels;
    img->w = (unsigned short)nw;
    img->h = (unsigned short)nh;
    img->anix = signed_to_img_word(scaled_coord(snap.anix, old_w, nw));
    img->aniy = signed_to_img_word(scaled_coord(snap.aniy, old_h, nh));
    if (secondary_anipoint_words_in_use(snap.anix2, snap.aniy2, snap.aniz2)) {
        img->anix2 = signed_to_img_word(scaled_coord(snap.anix2, old_w, nw));
        img->aniy2 = signed_to_img_word(scaled_coord(snap.aniy2, old_h, nh));
        img->aniz2 = snap.aniz2;
    } else {
        clear_secondary_anipoint(img);
    }
    if (g_hitbox_w > 0 && g_hitbox_h > 0) {
        g_hitbox_x = scaled_coord((unsigned short)(short)g_hitbox_x, old_w, nw);
        g_hitbox_y = scaled_coord((unsigned short)(short)g_hitbox_y, old_h, nh);
        g_hitbox_w = clamp_int(round_to_int((double)g_hitbox_w * (double)nw / (double)old_w), 1, 4096);
        g_hitbox_h = clamp_int(round_to_int((double)g_hitbox_h * (double)nh / (double)old_h), 1, 4096);
    }

    int trim_x = 0, trim_y = 0;
    bool did_trim = false;
    if (trim_bounds || optimize_bytes)
        did_trim = trim_image_to_content(img, optimize_bytes, &trim_x, &trim_y);

    push_pixel_history_entry(&snap);
    mark_dirty();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    deselect_all();
    InvalidateThumb(g_doc->ilselected);

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             did_trim ? "Resized %s: %dx%d -> %dx%d (trimmed %d,%d)."
                      : "Resized %s: %dx%d -> %dx%d.",
             img->n_s, old_w, old_h, (int)img->w, (int)img->h, trim_x, trim_y);
    g_restore_msg_timer = 4.0f;
    (void)new_stride;
    return true;
}

/* Shared tail of the two "move the art, keep the pixels" operations. Takes the
   already-captured undo snapshot so the caller decides whether the change was
   worth recording at all. */
static bool ReframeSelectedSprite(IMG *img, int nw, int nh, int dx, int dy,
                                  PixelHist *snap)
{
    unsigned int old_stride = (unsigned int)(((unsigned int)img->w + 3u) & ~3u);
    unsigned int new_stride = (unsigned int)(((unsigned int)nw + 3u) & ~3u);
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)new_stride * nh);
    if (!dst) {
        pixel_hist_free(snap);
        return false;
    }
    memset(dst, 0, (size_t)new_stride * nh);
    CanvasBlitIndexedOffset((const unsigned char *)img->data_p,
                            img->w, img->h, (int)old_stride,
                            dst, nw, nh, (int)new_stride, dx, dy);

    free(img->data_p);
    img->data_p = dst;
    img->w = (unsigned short)nw;
    img->h = (unsigned short)nh;

    /* The art moved by (dx, dy) inside the frame, so every coordinate that
       points at the art has to move with it or the sprite's registration
       silently shifts by that much in game. */
    img->anix = signed_to_img_word((int)(short)snap->anix + dx);
    img->aniy = signed_to_img_word((int)(short)snap->aniy + dy);
    if (secondary_anipoint_words_in_use(snap->anix2, snap->aniy2, snap->aniz2)) {
        img->anix2 = signed_to_img_word((int)(short)snap->anix2 + dx);
        img->aniy2 = signed_to_img_word((int)(short)snap->aniy2 + dy);
        img->aniz2 = snap->aniz2;
    }
    if (g_hitbox_w > 0 && g_hitbox_h > 0) {
        g_hitbox_x += dx;
        g_hitbox_y += dy;
    }

    push_pixel_history_entry(snap);
    mark_dirty();
    g_img_tex_idx = -2;
    InvalidateThumb(g_doc->ilselected);
    return true;
}

bool ResizeSelectedSpriteCanvas(int nw, int nh, int anchor)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;
    nw = clamp_int(nw, 1, 4096);
    nh = clamp_int(nh, 1, 4096);

    int old_w = img->w, old_h = img->h;
    if (nw == old_w && nh == old_h) return false;

    int dx = 0, dy = 0;
    CanvasAnchorOffset(old_w, old_h, nw, nh, anchor, &dx, &dy);

    /* full_state: w/h and the anipoints both change, and only a full-state
       entry restores those. */
    PixelHist snap = {};
    if (!pixel_hist_capture(&snap, true)) return false;
    if (!ReframeSelectedSprite(img, nw, nh, dx, dy, &snap)) return false;

    g_zoom_reset = true;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    deselect_all();
    /* Growing a canvas is nearly always followed by "now put the art where I
       want it", so hand the arrow keys straight to the nudge. */
    g_content_nudge_img = g_doc->ilselected;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Canvas %s: %dx%d -> %dx%d, art unchanged at %+d,%+d. "
             "Arrow keys nudge it (Shift = 10px).",
             img->n_s, old_w, old_h, nw, nh, dx, dy);
    g_restore_msg_timer = 5.0f;
    return true;
}

bool NudgeSelectedSpriteContent(int dx, int dy)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;
    if (dx == 0 && dy == 0) return false;

    unsigned int stride = (unsigned int)(((unsigned int)img->w + 3u) & ~3u);
    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    if (!CanvasIndexedContentBounds((const unsigned char *)img->data_p,
                                    img->w, img->h, (int)stride,
                                    &min_x, &min_y, &max_x, &max_y))
        return false; /* nothing opaque to move */

    CanvasClampContentNudge(min_x, min_y, max_x, max_y, img->w, img->h,
                            &dx, &dy);
    if (dx == 0 && dy == 0) return false;

    PixelHist snap = {};
    if (!pixel_hist_capture(&snap, true)) return false;
    return ReframeSelectedSprite(img, img->w, img->h, dx, dy, &snap);
}

int ClearSelectionPixels(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || !g_grid_sel.active) return 0;

    unsigned int stride = (unsigned int)(((unsigned int)img->w + 3u) & ~3u);
    /* g_grid_sel.pixel_mask is a std::vector<bool> bitfield with no contiguous
       storage, so flatten it before handing it to the pure helper. */
    std::vector<unsigned char> mask_bytes;
    const unsigned char *mask = NULL;
    if (g_grid_sel.is_mask && !g_grid_sel.pixel_mask.empty()) {
        mask_bytes.assign(g_grid_sel.pixel_mask.size(), 0);
        for (size_t i = 0; i < g_grid_sel.pixel_mask.size(); i++)
            mask_bytes[i] = g_grid_sel.pixel_mask[i] ? 1 : 0;
        mask = mask_bytes.data();
    }

    PixelHist snap = {};
    bool captured = pixel_hist_capture(&snap, false);
    int cleared = CanvasClearIndexedRect((unsigned char *)img->data_p,
                                         img->w, img->h, (int)stride,
                                         g_grid_sel.x1, g_grid_sel.y1,
                                         g_grid_sel.x2, g_grid_sel.y2,
                                         mask,
                                         g_grid_sel.mask_w, g_grid_sel.mask_h);
    if (cleared <= 0) {
        if (captured) pixel_hist_free(&snap);
        return 0;
    }

    if (captured) push_pixel_history_entry(&snap);
    mark_dirty();
    g_img_tex_idx = -2;
    InvalidateThumb(g_doc->ilselected);
    return cleared;
}

int ImportClipboardColorsIntoImagePalette(IMG *img, int *out_unmatched)
{
    if (out_unmatched) *out_unmatched = 0;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return 0;
    if (!g_clipboard.has_palette || g_clipboard.palette_numc <= 1) return 0;

    PAL *pal = get_pal(img->palnum);
    if (!pal || !pal->data_p || pal->numc <= 0) return 0;

    /* Same colors in the same order means the paste needs no remap at all. */
    int src_n = g_clipboard.palette_numc;
    if (src_n > 256) src_n = 256;
    int dst_n = pal->numc;
    if (dst_n > 256) dst_n = 256;
    if (src_n == dst_n &&
        memcmp(g_clipboard.palette_data, pal->data_p, (size_t)src_n * 2u) == 0)
        return 0;

    bool used[256] = {false};
    CanvasCollectUsedIndices((const unsigned char *)g_clipboard.data_p,
                             g_clipboard.w, g_clipboard.h,
                             g_clipboard.stride, used);

    /* Indices below numc are only reusable when nothing in the document draws
       with them — overwriting a color another sprite references would recolor
       that sprite instead. */
    bool free_slot[256] = {false};
    for (int i = 1; i < dst_n; i++) free_slot[i] = true;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (p->palnum != img->palnum || !p->data_p || !p->w || !p->h) continue;
        bool seen[256] = {false};
        CanvasCollectUsedIndices((const unsigned char *)p->data_p, p->w, p->h,
                                 (int)(((unsigned int)p->w + 3u) & ~3u), seen);
        for (int i = 1; i < 256; i++)
            if (seen[i]) free_slot[i] = false;
    }

    int capacity = PaletteColorCountForBpp(pal->bitspix);
    if (capacity < dst_n) capacity = dst_n;

    PaletteImportPlan plan =
        PlanPaletteColorImport(g_clipboard.palette_data, src_n, used,
                               (const unsigned char *)pal->data_p, dst_n,
                               capacity, free_slot);
    if (out_unmatched) *out_unmatched = plan.unmatched;
    if (plan.added.empty()) return 0;

    doc_undo_push();
    if (plan.new_numc > (int)pal->numc && !ensure_palette_numc(pal, plan.new_numc))
        return 0;

    unsigned char *pd = (unsigned char *)pal->data_p;
    for (const PaletteImportSlot &slot : plan.added) {
        pd[slot.dst_index * 2]     = (unsigned char)(slot.word & 0xFF);
        pd[slot.dst_index * 2 + 1] = (unsigned char)((slot.word >> 8) & 0xFF);
    }

    if ((int)img->palnum == g_doc->plselected) {
        ApplyPalette(g_doc->plselected);
        save_palette_baseline();
        reset_palette_adjust_sliders();
    }
    InvalidatePaletteUsage();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
    mark_dirty();
    return (int)plan.added.size();
}

void BuildResizeFallbackRgb(ResizeRgb fallback_rgb[256])
{
    for (int i = 0; i < 256; i++) {
        fallback_rgb[i].r = g_palette[i].r;
        fallback_rgb[i].g = g_palette[i].g;
        fallback_rgb[i].b = g_palette[i].b;
    }
}

int BulkResizeMarkedSprites(int scale_x, int scale_y,
                                   SpriteResizeMode mode, bool trim_bounds)
{
    scale_x = clamp_int(scale_x, 1, 3200);
    scale_y = clamp_int(scale_y, 1, 3200);

    struct BulkResizeTarget {
        IMG *img;
        int idx;
        int nw;
        int nh;
    };
    std::vector<BulkResizeTarget> targets;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!(img->flags & 1) || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        int nw = clamp_int(round_to_int((double)img->w * (double)scale_x / 100.0), 1, 4096);
        int nh = clamp_int(round_to_int((double)img->h * (double)scale_y / 100.0), 1, 4096);
        bool force_trim = (mode == SpriteResizeMode::QualitySmallBytes);
        if (nw == (int)img->w && nh == (int)img->h && !(trim_bounds || force_trim))
            continue;
        targets.push_back({img, idx, nw, nh});
    }
    if (targets.empty()) return 0;
    if (!doc_undo_push()) return 0;

    bool optimize_bytes = (mode == SpriteResizeMode::QualitySmallBytes);
    bool use_quality = (mode != SpriteResizeMode::IndexNearest);
    ResizeRgb fallback_rgb[256];
    BuildResizeFallbackRgb(fallback_rgb);

    int changed = 0;
    for (const BulkResizeTarget &target : targets) {
        IMG *img = target.img;
        int old_w = img->w;
        int old_h = img->h;
        unsigned short old_anix = img->anix;
        unsigned short old_aniy = img->aniy;
        unsigned short old_anix2 = img->anix2;
        unsigned short old_aniy2 = img->aniy2;
        unsigned short old_aniz2 = img->aniz2;

        PAL *pal = get_pal(img->palnum);
        unsigned int new_stride = 0;
        unsigned char *new_pixels = use_quality
            ? ResizeSpritePixelsQuality(img, pal, fallback_rgb,
                                        target.nw, target.nh,
                                        optimize_bytes, &new_stride)
            : ResizeSpritePixelsNearest(img, target.nw, target.nh,
                                        &new_stride);
        if (!new_pixels) continue;

        free(img->data_p);
        img->data_p = new_pixels;
        img->w = (unsigned short)target.nw;
        img->h = (unsigned short)target.nh;
        img->anix = signed_to_img_word(scaled_coord(old_anix, old_w, target.nw));
        img->aniy = signed_to_img_word(scaled_coord(old_aniy, old_h, target.nh));
        if (secondary_anipoint_words_in_use(old_anix2, old_aniy2, old_aniz2)) {
            img->anix2 = signed_to_img_word(scaled_coord(old_anix2, old_w, target.nw));
            img->aniy2 = signed_to_img_word(scaled_coord(old_aniy2, old_h, target.nh));
            img->aniz2 = old_aniz2;
        } else {
            clear_secondary_anipoint(img);
        }

        bool selected = (target.idx == g_doc->ilselected);
        if (selected && g_hitbox_w > 0 && g_hitbox_h > 0) {
            g_hitbox_x = scaled_coord((unsigned short)(short)g_hitbox_x, old_w, target.nw);
            g_hitbox_y = scaled_coord((unsigned short)(short)g_hitbox_y, old_h, target.nh);
            g_hitbox_w = clamp_int(round_to_int((double)g_hitbox_w * (double)target.nw / (double)old_w), 1, 4096);
            g_hitbox_h = clamp_int(round_to_int((double)g_hitbox_h * (double)target.nh / (double)old_h), 1, 4096);
        }

        if (trim_bounds || optimize_bytes) {
            int trim_x = 0, trim_y = 0;
            trim_image_to_content(img, optimize_bytes, &trim_x, &trim_y, selected);
        }

        InvalidateThumb(target.idx);
        changed++;
        (void)new_stride;
    }

    if (changed > 0) {
        mark_dirty();
        g_img_tex_idx = -2;
        g_zoom_reset = true;
        g_pasted.active = false;
        g_pasted.dragging = false;
        g_xform.active = false;
        deselect_all();
    }
    return changed;
}

/* ---- Marked-image helpers ---------------------------------------- */

int CountMarkedImages(void)
{
    int count = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p)
        if (img->flags & 1) count++;
    return count;
}

void MirrorMarkedAnipointsToReverseWithToast(void)
{
    int marked = CountMarkedImages();
    int changed = MirrorMarkedAnipointsToReverse();
    if (changed > 0) {
        /* Thumbnails and the canvas texture draw the anipoint crosshair, so
           they go stale the moment an anchor moves. */
        ClearTimelineThumbCache();
        g_img_tex_idx = -2;
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mirrored anipoints on %d marked sprite%s (%s).",
                 changed, changed == 1 ? "" : "s",
                 mirror_convention_label(g_mirror_convention));
    } else if (marked > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No marked anipoints moved; X values are centered.");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark sprites first, then mirror anipoints.");
    }
    g_restore_msg_timer = 4.0f;
}

/* ---- Variant paint ------------------------------------------------ */

static bool ensure_all_palettes_numc(int min_numc)
{
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
        if (!ensure_palette_numc(p, min_numc)) return false;
    }
    return true;
}

static void collect_library_used_indices(bool used[256])
{
    memset(used, 0, sizeof(bool) * 256);
    used[0] = true;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++)
                used[pix[y * stride + x]] = true;
    }
}

static bool slot_matches_variant_shadow(int slot, int base_idx, int target_pal_idx, unsigned short target_word)
{
    PAL *target = get_pal(target_pal_idx);
    if (!target || !target->data_p || slot >= (int)target->numc) return false;
    if (pal_word_or_black(target, slot) != target_word) return false;

    int pal_idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, pal_idx++) {
        if (pal_idx == target_pal_idx) continue;
        if (!p->data_p || slot >= (int)p->numc) return false;
        if (pal_word_or_black(p, slot) != pal_word_or_black(p, base_idx)) return false;
    }
    return true;
}

static int find_variant_shadow_slot(int base_idx, int target_pal_idx, unsigned short target_word)
{
    if (base_idx <= 0 || base_idx >= 256) return -1;
    for (int slot = 1; slot < 256; slot++) {
        if (slot == base_idx) continue;
        if (slot_matches_variant_shadow(slot, base_idx, target_pal_idx, target_word))
            return slot;
    }
    return -1;
}

static int create_variant_shadow_slot(int base_idx, int target_pal_idx, unsigned short target_word, bool *created)
{
    if (created) *created = false;
    int existing = find_variant_shadow_slot(base_idx, target_pal_idx, target_word);
    if (existing >= 0) return existing;

    bool used[256];
    collect_library_used_indices(used);
    int slot = -1;
    for (int i = 1; i < 256; i++) {
        if (!used[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;
    if (!ensure_all_palettes_numc(slot + 1)) return -1;

    int pal_idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, pal_idx++) {
        unsigned short w = (pal_idx == target_pal_idx)
            ? target_word
            : pal_word_or_black(p, base_idx);
        unsigned char *pd = (unsigned char *)p->data_p;
        pd[slot * 2 + 0] = (unsigned char)(w & 0xFF);
        pd[slot * 2 + 1] = (unsigned char)(w >> 8);
    }

    if (created) *created = true;
    return slot;
}

int ApplySelectedInnerStroke(unsigned char r, unsigned char g, unsigned char b)
{
    IMG *img = (g_doc && g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return 0;
    PAL *source_pal = get_pal((int)img->palnum);
    if (!source_pal || !source_pal->data_p || source_pal->bitspix < 2) return 0;

    int bpp = source_pal->bitspix > 8 ? 8 : source_pal->bitspix;
    int color_cap = 1 << bpp;
    if (g_sel_color <= 0 || g_sel_color >= color_cap) return 0;

    /* A 2bpp image has transparent #0 plus only three opaque indices.  Its
       selected fill therefore doubles as the lightest (third) inside band. */
    int dedicated_slots = (bpp == 2) ? 2 : 3;
    int slots[3] = { g_sel_color, g_sel_color, g_sel_color };
    int next_slot = 1;
    for (int band = 0; band < dedicated_slots; band++) {
        while (next_slot == g_sel_color) next_slot++;
        int slot = next_slot++;
        if (slot < 0) return 0;
        slots[band] = slot;
    }
    if (bpp == 2) slots[2] = g_sel_color;

    int highest_slot = slots[0];
    for (int i = 1; i < dedicated_slots; i++)
        if (slots[i] > highest_slot) highest_slot = slots[i];
    if (!doc_undo_push()) return 0;

    /* This operation is deliberately self-contained.  The source palette can
       be shared by a whole animation library, so give this one sprite a copy
       before turning every opaque entry into the requested fill/stroke ramp. */
    PAL *target_pal = (PAL *)AllocPal();
    if (!target_pal) return 0;
    target_pal->flags = source_pal->flags;
    target_pal->bitspix = source_pal->bitspix;
    target_pal->numc = source_pal->numc > highest_slot ? source_pal->numc
                                                        : (unsigned short)(highest_slot + 1);
    /* The stroke ramp can need slots past the source's last color, so the
       inherited depth may no longer address the whole palette. */
    if (PaletteBppTooSmall(target_pal->bitspix, (int)target_pal->numc))
        target_pal->bitspix = (unsigned char)PaletteBppForColorCount((int)target_pal->numc);
    target_pal->pad = 0;
    snprintf(target_pal->n_s, sizeof(target_pal->n_s), "STRK%03u", g_doc->palcnt - 1);
    unsigned char *palette_data = (unsigned char *)malloc(512);
    if (!palette_data) return 0;
    memset(palette_data, 0, 512);
    memcpy(palette_data, source_pal->data_p, (size_t)source_pal->numc * 2);
    target_pal->data_p = palette_data;

    const float shade[3] = { 0.34f, 0.62f, 0.92f };
    unsigned short stroke_words[3];
    for (int band = 0; band < 3; band++) {
        stroke_words[band] = rgb_to_word15(
            (unsigned char)(r * shade[band] + 0.5f),
            (unsigned char)(g * shade[band] + 0.5f),
            (unsigned char)(b * shade[band] + 0.5f));
    }

    unsigned short fill_word = pal_word_or_black(source_pal, g_sel_color);
    for (int i = 1; i < (int)target_pal->numc; i++) {
        palette_data[i * 2] = (unsigned char)(fill_word & 0xff);
        palette_data[i * 2 + 1] = (unsigned char)(fill_word >> 8);
    }
    for (int band = 0; band < dedicated_slots; band++) {
        palette_data[slots[band] * 2] = (unsigned char)(stroke_words[band] & 0xff);
        palette_data[slots[band] * 2 + 1] = (unsigned char)(stroke_words[band] >> 8);
    }
    img->palnum = (unsigned short)(g_doc->palcnt - 1);
    g_doc->plselected = (int)img->palnum;

    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pixels = (unsigned char *)img->data_p;
    std::vector<int> distance((size_t)w * h, -1);
    std::vector<int> queue;
    queue.reserve((size_t)w * h);
    static const int dx[4] = { -1, 1, 0, 0 };
    static const int dy[4] = { 0, 0, -1, 1 };
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        if (pixels[y * stride + x] == 0) continue;
        bool edge = false;
        for (int n = 0; n < 4; n++) {
            int nx = x + dx[n], ny = y + dy[n];
            if (nx < 0 || nx >= w || ny < 0 || ny >= h || pixels[ny * stride + nx] == 0) {
                edge = true; break;
            }
        }
        if (edge) { distance[y * w + x] = 0; queue.push_back(y * w + x); }
    }
    for (size_t head = 0; head < queue.size(); head++) {
        int at = queue[head], d = distance[at];
        if (d >= 2) continue;
        int x = at % w, y = at / w;
        for (int n = 0; n < 4; n++) {
            int nx = x + dx[n], ny = y + dy[n];
            int ni = ny * w + nx;
            if (nx >= 0 && nx < w && ny >= 0 && ny < h &&
                pixels[ny * stride + nx] != 0 && distance[ni] < 0) {
                distance[ni] = d + 1;
                queue.push_back(ni);
            }
        }
    }

    int changed = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        unsigned char *pixel = pixels + y * stride + x;
        if (*pixel == 0) continue;
        int d = distance[y * w + x];
        unsigned char replacement = (d >= 0 && d < 3) ? (unsigned char)slots[d]
                                                       : (unsigned char)g_sel_color;
        if (*pixel != replacement) { *pixel = replacement; changed++; }
    }

    ApplyPalette((int)img->palnum);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    InvalidateThumb(g_doc->ilselected);
    g_img_tex_idx = -2;
    mark_dirty();
    return changed;
}

static VariantPaintResult ApplyVariantPaintToPixels(IMG *img, const std::vector<std::pair<int,int>>& pixels)
{
    VariantPaintResult r = {0, 0, 0, 0};
    if (!img || !img->data_p || pixels.empty()) return r;
    int target_pal_idx = img->palnum;
    PAL *target_pal = get_pal(target_pal_idx);
    if (!target_pal || !target_pal->data_p || target_pal_idx < 0) {
        r.skipped_no_slot = (int)pixels.size();
        return r;
    }
    if (g_sel_color == 0) {
        r.skipped_no_slot = (int)pixels.size();
        return r;
    }

    SDL_Color &tc = g_palette[g_sel_color];
    unsigned short target_word = rgb_to_word15(tc.r, tc.g, tc.b);
    int base_to_slot[256];
    for (int i = 0; i < 256; i++) base_to_slot[i] = -1;

    int stride = (img->w + 3) & ~3;
    unsigned char *data = (unsigned char *)img->data_p;
    for (const auto &pt : pixels) {
        int x = pt.first, y = pt.second;
        if (x < 0 || y < 0 || x >= (int)img->w || y >= (int)img->h) continue;
        unsigned char *pix = data + y * stride + x;
        int base_idx = *pix;
        if (base_idx == 0) { r.skipped_transparent++; continue; }
        if (pal_word_or_black(target_pal, base_idx) == target_word) continue;

        int slot = base_to_slot[base_idx];
        if (slot == -1) {
            bool created = false;
            slot = create_variant_shadow_slot(base_idx, target_pal_idx, target_word, &created);
            base_to_slot[base_idx] = (slot >= 0) ? slot : -2;
            if (created) r.slots++;
        }
        if (slot < 0) {
            r.skipped_no_slot++;
            continue;
        }
        if (*pix != (unsigned char)slot) {
            *pix = (unsigned char)slot;
            r.pixels++;
        }
    }

    if (r.pixels > 0 || r.slots > 0) {
        ApplyPalette(target_pal_idx);
        g_img_tex_idx = -2;
        mark_dirty();
    }
    return r;
}

VariantPaintResult ApplyVariantBrush(IMG *img, int cx, int cy, int brush)
{
    std::vector<std::pair<int,int>> pts;
    int r = brush > 0 ? brush : 1;
    int r2 = (r - 1) * (r - 1);
    for (int by = -(r - 1); by <= (r - 1); by++) {
        for (int bx = -(r - 1); bx <= (r - 1); bx++) {
            if (r > 1 && bx * bx + by * by > r2) continue;
            pts.push_back({cx + bx, cy + by});
        }
    }
    return ApplyVariantPaintToPixels(img, pts);
}

void ApplyVariantToSelection(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
    if (!g_grid_sel.active) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select pixels first, then apply variant paint.");
        g_restore_msg_timer = 4.0f;
        return;
    }
    if (g_sel_color == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Variant paint needs an opaque target color.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    std::vector<std::pair<int,int>> pts;
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++)
            if (selection_contains_pixel(img, x, y)) pts.push_back({x, y});

    doc_undo_push();
    VariantPaintResult r = ApplyVariantPaintToPixels(img, pts);
    if (r.pixels > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Variantized %d px using %d shadow slot%s.",
                 r.pixels, r.slots, r.slots == 1 ? "" : "s");
    } else if (r.skipped_no_slot > 0 && g_sel_color == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Variant paint needs an opaque target color.");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No pixels changed (%d transparent skipped).", r.skipped_transparent);
    }
    g_restore_msg_timer = 4.0f;
}

/* ---- World marked tabs -------------------------------------------- */

static void WorldCollectActiveAsmLanes(std::vector<WorldMarkedAsmLaneInput> &asm_lanes)
{
    asm_lanes.clear();
    asm_lanes.reserve(2);
    /* Resolved from the uid on entry rather than held across frames: the tab
       an ASM was loaded against can be closed while its lane stays enabled. */
    auto add_asm_lane = [&](std::vector<AsmAnim> &anims, bool enabled, int sel,
                            int slot_id, unsigned int doc_uid) {
        int doc_idx = document_index_of_uid(doc_uid);
        Document *doc = document_get(doc_idx);
        if (!enabled || sel < 0 || sel >= (int)anims.size() || !doc) return;
        AsmAnim &a = anims[sel];
        if (a.frames.empty()) return;
        WorldMarkedAsmLaneInput input = {};
        input.enabled = true;
        input.slot_id = slot_id;
        input.doc = doc;
        input.doc_idx = doc_idx;
        input.name = a.name.c_str();
        input.frames.reserve(a.frames.size());
        for (const AsmAnimFrame &fr : a.frames) {
            WorldAsmLaneFrame view = {};
            view.piece_img = &fr.piece_img;
            view.piece_doc_uid = &fr.piece_doc_uid;
            view.dx = fr.dx;
            view.dy = fr.dy;
            view.mirror = fr.mirror;
            view.mirror_v = fr.mirror_v;
            input.frames.push_back(view);
        }
        asm_lanes.push_back(input);
    };
    add_asm_lane(g_asm_anims, g_asm_lane_enabled, g_asm_anim_sel,
                 kWorldAsmSlot, g_asm_anim_doc_uid);
    add_asm_lane(g_asm_opp_anims, g_asm_opp_enabled, g_asm_opp_sel,
                 kWorldAsmOpponentSlot, g_asm_opp_doc_uid);
}

static void WorldHandleMarkedPanelResult(const WorldMarkedPanelResult &panel_result)
{
    WorldMarkedPanelAction panel_action = panel_result.header;
    if (panel_action.copied_asm) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Copied World View ASM for %d lane%s.",
                 panel_action.copied_lane_count,
                 panel_action.copied_lane_count == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    if (panel_action.request_save_asm)
        g_request_save_world_asm = true;   /* dialog opened in main loop */
    if (panel_action.request_save_project)
        g_request_save_world_project = true;
    if (panel_action.request_save_png)
        g_request_save_world_png = true;
    if (panel_action.request_save_png_seq)
        g_request_save_world_png_seq = true;
    if (panel_action.request_load_project)
        g_request_load_world_project = true;
    if (panel_action.request_append_project)
        g_request_append_world_project = true;
    if (panel_action.request_load_bg)
        g_request_load_world_bg = true;
    if (panel_action.request_load_asm) {
        g_show_asm_anim = true;
        g_request_load_asm = true;
    }
    if (panel_action.dummy_assigned) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Assigned dummy body to [%d] %sDECAP.",
                 g_world_marked_state.dummy_decap_doc_idx,
                 g_world_marked_state.dummy_decap_prefix.c_str());
        g_restore_msg_timer = 4.0f;
    } else if (panel_action.dummy_assign_failed) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Select a DECAP body frame/piece first.");
        g_restore_msg_timer = 4.0f;
    }
    if (panel_result.thumb_click.clicked) {
        Document *doc = document_get(panel_result.thumb_click.doc_idx);
        WorldSyncEditorSelectionToSprite(doc,
                                         panel_result.thumb_click.doc_idx,
                                         panel_result.thumb_click.img_idx);
    }
    if (panel_result.copied_popup_asm) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Copied World View ASM.");
        g_restore_msg_timer = 4.0f;
    }
}

bool DrawWorldMarkedTabs(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    std::vector<WorldMarkedAsmLaneInput> asm_lanes;
    WorldCollectActiveAsmLanes(asm_lanes);

    IMG *selected_img = get_img(g_doc ? g_doc->ilselected : -1);
    WorldMarkedTabsResult tabs_result =
        WorldDrawMarkedTabs(g_world_marked_state, g_world_state,
                            avail, img_pos, io.DeltaTime,
                            document_active_index(), selected_img, asm_lanes,
                            !g_world_marked_panel_docked);
    if (!tabs_result.drew)
        return false;

    if (!g_world_marked_panel_docked)
        WorldHandleMarkedPanelResult(tabs_result.panel);
    return true;
}

/* ---- Sequence / Script workspace ("Anim" canvas mode) ----------------
 * SEQSCR building used to ride along inside World View as an extra lane,
 * which meant its table competed with the marked rows for the same panel and
 * its record could only be seen mixed into that scene. It now owns a mode:
 *
 *   +-------------------------------------------------+
 *   | toolbar (record, transport, exports)            |
 *   |                                   +-----------+ |
 *   |   world-sized animation viewport  | sprite    | |
 *   |   for the loaded record           | inspector | |
 *   |                                   +-----------+ |
 *   +-------------------------------------------------+
 *   | record list | entry table for the loaded record |
 *   +-------------------------------------------------+
 *
 * The viewport, lane drag-editing, and ASM/PNG export all reuse the World
 * View machinery; only the framing and the surrounding chrome are new. */

enum { kSeqScrInspectorMaxSide = 148 };

/* Draw the corner sprite inspector: the single IMG under the playhead, at the
   largest integer scale that fits, with the numbers you need while timing a
   sequence (index, size, anipoint). Sits inside the animation viewport's
   top-right corner so it never steals layout height from the table below. */
static void SeqScrDrawSpriteInspector(const WorldMarkedLane *lane,
                                      ImVec2 area_min, ImVec2 area_max)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float pad = 8.0f;
    const float header_h = ImGui::GetTextLineHeight() + 4.0f;
    const float footer_h = ImGui::GetTextLineHeight() * 2.0f + 4.0f;

    float box_w = (float)kSeqScrInspectorMaxSide;
    float box_h = (float)kSeqScrInspectorMaxSide + header_h + footer_h;
    if (box_w > (area_max.x - area_min.x) * 0.5f)
        box_w = (area_max.x - area_min.x) * 0.5f;
    if (box_h > (area_max.y - area_min.y) * 0.9f)
        box_h = (area_max.y - area_min.y) * 0.9f;
    if (box_w < 64.0f || box_h < 64.0f) return;

    ImVec2 p0(area_max.x - box_w - pad, area_min.y + pad);
    ImVec2 p1(p0.x + box_w, p0.y + box_h);
    dl->AddRectFilled(p0, p1, IM_COL32(12, 12, 15, 235), 4.0f);
    dl->AddRect(p0, p1, IM_COL32(90, 90, 105, 255), 4.0f, 0, 1.0f);

    /* A frame picked in the sidebar browser wins over the playhead entry: the
       box is how you look at a candidate before committing it to a sequence. */
    int browse_doc = -1;
    int browse_img = -1;
    bool browsing = SeqScrBrowserPreviewFrame(&browse_doc, &browse_img);

    int fi = lane ? lane->frame_pos : -1;
    bool have_frame = lane && fi >= 0 && fi < (int)lane->frames.size();
    int img_idx = -1;
    Document *doc = NULL;
    IMG *img = NULL;
    char header[64];

    if (browsing) {
        img_idx = browse_img;
        doc = document_get(browse_doc);
        img = doc_get_img(doc, img_idx);
        snprintf(header, sizeof(header), "Browse");
    } else if (have_frame) {
        img_idx = lane->frames[(size_t)fi];
        doc = (fi < (int)lane->frame_docs.size() && lane->frame_docs[(size_t)fi])
            ? lane->frame_docs[(size_t)fi] : lane->doc;
        img = doc_get_img(doc, img_idx);
        snprintf(header, sizeof(header), "Entry %d/%d",
                 fi + 1, (int)lane->frames.size());
    } else {
        snprintf(header, sizeof(header), "Entry 0/%d",
                 lane ? (int)lane->frames.size() : 0);
    }
    dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 3.0f),
                IM_COL32(210, 210, 220, 255), header);

    ImVec2 view_min(p0.x + 4.0f, p0.y + header_h);
    ImVec2 view_max(p1.x - 4.0f, p1.y - footer_h);
    dl->AddRectFilled(view_min, view_max, IM_COL32(0, 0, 0, 255));

    if (!img || img->w <= 0 || img->h <= 0) {
        dl->AddText(ImVec2(view_min.x + 6.0f, view_min.y + 6.0f),
                    IM_COL32(150, 150, 150, 255), "(no sprite)");
        return;
    }

    SDL_Texture *tex = BuildWorldSpriteTexture(doc, img, 255);
    float view_w = view_max.x - view_min.x;
    float view_h = view_max.y - view_min.y;
    float fit = (view_w / (float)img->w < view_h / (float)img->h)
              ? view_w / (float)img->w : view_h / (float)img->h;
    /* Integer scale above 1:1 keeps arcade pixels square and readable; below
       1:1 a big sprite still has to shrink to fit, so allow the fraction. */
    float scale = fit >= 1.0f ? (float)(int)fit : fit;
    float draw_w = (float)img->w * scale;
    float draw_h = (float)img->h * scale;
    ImVec2 d0(view_min.x + (view_w - draw_w) * 0.5f,
              view_min.y + (view_h - draw_h) * 0.5f);
    ImVec2 d1(d0.x + draw_w, d0.y + draw_h);
    if (tex)
        dl->AddImage((ImTextureID)(intptr_t)tex, d0, d1);
    else
        dl->AddRect(d0, d1, IM_COL32(120, 60, 60, 255));

    /* Anipoint crosshair: the anchor every world placement is measured from. */
    float ax = d0.x + (float)(short)img->anix * scale;
    float ay = d0.y + (float)(short)img->aniy * scale;
    if (ax >= view_min.x && ax <= view_max.x &&
        ay >= view_min.y && ay <= view_max.y) {
        dl->AddLine(ImVec2(ax - 5.0f, ay), ImVec2(ax + 5.0f, ay),
                    IM_COL32(255, 200, 0, 220));
        dl->AddLine(ImVec2(ax, ay - 5.0f), ImVec2(ax, ay + 5.0f),
                    IM_COL32(255, 200, 0, 220));
    }

    char line1[96];
    char line2[96];
    std::string name;
    if (!browsing && have_frame && fi < (int)lane->frame_labels.size() &&
        !lane->frame_labels[(size_t)fi].empty())
        name = lane->frame_labels[(size_t)fi];
    else
        name = img_name_string(img);
    snprintf(line1, sizeof(line1), "[%d] %s", img_idx, name.c_str());
    /* Browsing crosses files, so say which one this sprite came from. */
    const char *src = (doc && doc->fname_s[0]) ? doc->fname_s : "";
    if (browsing && src[0])
        snprintf(line2, sizeof(line2), "%dx%d  ani %d,%d  %s",
                 (int)img->w, (int)img->h,
                 (int)(short)img->anix, (int)(short)img->aniy, src);
    else
        snprintf(line2, sizeof(line2), "%dx%d  ani %d,%d",
                 (int)img->w, (int)img->h,
                 (int)(short)img->anix, (int)(short)img->aniy);
    /* Clip to the panel rather than letting a long sprite name bleed out over
       the animation behind it. */
    ImVec4 text_clip(p0.x + 5.0f, view_max.y, p1.x - 4.0f, p1.y);
    dl->AddText(NULL, 0.0f, ImVec2(p0.x + 6.0f, view_max.y + 2.0f),
                IM_COL32(200, 205, 215, 255), line1, NULL, 0.0f, &text_clip);
    dl->AddText(NULL, 0.0f,
                ImVec2(p0.x + 6.0f, view_max.y + 2.0f + ImGui::GetTextLineHeight()),
                IM_COL32(150, 155, 165, 255), line2, NULL, 0.0f, &text_clip);
}

/* Sequences / Scripts picker. Returns the record index to load, or -1.
   Loading is deferred to the end of the frame because it rewrites the lane
   slot the entry table alongside this list is still drawing from.
   "+" appends an empty record so entries can be built up from scratch. */
static int SeqScrDrawRecordPicker(WorldMarkedSequenceState &state)
{
    std::vector<SeqScrRecordView> records;
    bool truncated = false;
    bool have_records = SeqScrBuildRecords(records, &truncated);
    int active_doc_idx = document_active_index();

    static int s_kind = 0;   /* 0 = sequences, 1 = scripts */
    if (ImGui::BeginTabBar("##seqscr_ws_kind")) {
        if (ImGui::BeginTabItem("Sequences")) { s_kind = 0; ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Scripts"))   { s_kind = 1; ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    bool scripts = (s_kind == 1);
    int load_request = -1;

    ImGui::BeginDisabled(truncated || !g_doc);
    if (ImGui::SmallButton(scripts ? "+ Script##seqscr_ws_add"
                                   : "+ Sequence##seqscr_ws_add")) {
        if (SeqScrAddRecord(scripts)) {
            load_request = scripts ? (int)(g_doc->seqcnt + g_doc->scrcnt) - 1
                                   : (int)g_doc->seqcnt - 1;
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Added an empty %s. Add entries from the table.",
                     scripts ? "script" : "sequence");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Could not add %s (anim blob is truncated or out of memory).",
                     scripts ? "script" : "sequence");
        }
        g_restore_msg_timer = 4.0f;
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::SmallButton("Raw Data...##seqscr_ws_raw"))
        g_show_seqscr_editor = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Open the raw SEQSCR/ENTRY field editor for this IMG.");

    if (truncated) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                           "SEQSCR blob is truncated.");
    }

    if (!have_records) {
        ImGui::TextDisabled("This IMG has no sequence/script data yet.");
        return load_request;
    }

    if (ImGui::BeginListBox("##seqscr_ws_records", ImVec2(-1, -1))) {
        bool any = false;
        for (const SeqScrRecordView &rec : records) {
            if (rec.script != scripts) continue;
            any = true;
            int local_idx = scripts ? rec.index - (int)g_doc->seqcnt : rec.index;
            char fallback[32];
            snprintf(fallback, sizeof(fallback), "%s %d",
                     scripts ? "Script" : "Sequence", local_idx);
            const char *name = rec.name[0] ? rec.name : fallback;
            char label[112];
            snprintf(label, sizeof(label), "%02d  %.40s  (%d)%s",
                     local_idx, name, rec.num,
                     rec.truncated ? "  truncated" : "");
            bool loaded = state.embedded_active &&
                          state.embedded_doc_idx == active_doc_idx &&
                          state.embedded_record_index == rec.index;

            ImGui::PushID(rec.index);
            if (rec.truncated) ImGui::BeginDisabled();
            if (loaded)
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
            if (ImGui::Selectable(label, loaded))
                load_request = rec.index;
            if (loaded) ImGui::PopStyleColor();
            if (ImGui::BeginPopupContextItem("##seqscr_ws_ctx")) {
                if (ImGui::MenuItem("Load"))
                    load_request = rec.index;
                /* Renaming from the list itself, not only from the loaded
                   record's panel: this list is where both sequences AND
                   scripts are visible, and a record you want to name is often
                   not the one currently loaded. rec.index is the combined
                   sequence+script index the blob helpers expect, so this is
                   correct for scripts as well. */
                if (ImGui::MenuItem("Rename...")) {
                    SeqScrBeginRename(kSeqScrRenameAnimTab, rec.index, rec.name);
                    ImGui::CloseCurrentPopup();
                }
                if (ImGui::MenuItem("Copy Anim ASM")) {
                    state.generated_asm = WorldBuildSeqScrAsmExport(rec.index);
                    ImGui::SetClipboardText(state.generated_asm.c_str());
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Copied anim ASM for '%s'.", name);
                    g_restore_msg_timer = 4.0f;
                }
                if (ImGui::MenuItem("Save Anim ASM")) {
                    state.generated_asm = WorldBuildSeqScrAsmExport(rec.index);
                    g_request_save_world_asm = true;
                }
                ImGui::EndPopup();
            }
            if (rec.truncated) ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!any) ImGui::TextDisabled("None");
        ImGui::EndListBox();
    }

    /* Renaming the loaded record has to update the label the workspace is
       already showing; every other row just redraws from the blob. */
    int renamed = -1;
    std::string renamed_name;
    if (SeqScrDrawRenamePopup(kSeqScrRenameAnimTab, &renamed, &renamed_name) &&
        state.embedded_active && state.embedded_doc_idx == active_doc_idx &&
        state.embedded_record_index == renamed) {
        state.embedded_name = renamed_name;
    }

    return load_request;
}

/* ---- Frame library across numbered sibling IMGs ----------------------
 * A character's sprites are split over CAGE1.IMG .. CAGE10.IMG, so building a
 * sequence means reaching across files. This gathers the whole numbered set
 * into one browsable list.
 *
 * A SEQSCR entry is a bare 16-bit index into its own file's image list, so a
 * frame from a sibling file can be previewed and exported to ASM but never
 * written into this IMG's blob. Those entries are marked as such rather than
 * silently writing an index that resolves to a different sprite. */

static SeqScrFrameLibrary g_seqscr_frame_lib;
static int s_seqscr_lib_file_filter = -1;     /* -1 = all files */
static int s_seqscr_lib_selected = -1;        /* index into lib.frames */
static char s_seqscr_lib_search[48] = {};
static bool s_seqscr_lib_scroll_to_sel = false;

const SeqScrFrameLibrary &SeqScrFrameLib(void) { return g_seqscr_frame_lib; }

static void SeqScrClearBrowseSelection(void) { s_seqscr_lib_selected = -1; }

/* index -> IMG* per document, built once per use.

   doc_get_img walks the image list from its head, so it is O(n) per call. The
   browser has hundreds of rows across ten files, and resolving each row that
   way every frame cost more than the rest of the UI put together — enough to
   drop the frame rate far below the animation's tick rate, which is what made
   playback in the Anim tab judder. One pass builds the whole table. */
struct SeqScrImgCache {
    std::vector<int> docs;
    std::vector<std::vector<IMG*>> imgs;

    void build(const SeqScrFrameLibrary &lib)
    {
        docs.clear();
        imgs.clear();
        for (int d : lib.file_docs) {
            if (d < 0) continue;
            bool have = false;
            for (int e : docs) if (e == d) { have = true; break; }
            if (have) continue;
            Document *doc = document_get(d);
            if (!doc) continue;
            docs.push_back(d);
            imgs.push_back(std::vector<IMG*>());
            std::vector<IMG*> &v = imgs.back();
            for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p)
                v.push_back(img);
        }
    }
    IMG *get(int doc_idx, int img_idx) const
    {
        for (size_t i = 0; i < docs.size(); i++) {
            if (docs[i] != doc_idx) continue;
            if (img_idx < 0 || img_idx >= (int)imgs[i].size()) return NULL;
            return imgs[i][(size_t)img_idx];
        }
        return NULL;
    }
};

/* Rows currently on screen, honouring the file dropdown and the text filter,
   so keyboard stepping walks exactly what the eye sees. Deliberately does no
   IMG lookup: this runs from the keyboard handlers every frame, and a row that
   turns out to be unresolvable is simply skipped when drawn. */
static std::vector<int> SeqScrVisibleFrameRows(void)
{
    std::vector<int> rows;
    const SeqScrFrameLibrary &lib = g_seqscr_frame_lib;
    std::string filter_low(s_seqscr_lib_search);
    for (char &c : filter_low) c = (char)tolower((unsigned char)c);
    rows.reserve(lib.frames.size());
    for (int i = 0; i < (int)lib.frames.size(); i++) {
        const SeqScrFrameRef &ref = lib.frames[(size_t)i];
        if (s_seqscr_lib_file_filter >= 0 &&
            ref.file_slot != s_seqscr_lib_file_filter) continue;
        if (!filter_low.empty()) {
            std::string low = ref.name;
            for (char &c : low) c = (char)tolower((unsigned char)c);
            if (low.find(filter_low) == std::string::npos) continue;
        }
        rows.push_back(i);
    }
    return rows;
}

bool SeqScrFrameNavActive(void)
{
    return g_seqscr_frame_nav && !SeqScrVisibleFrameRows().empty();
}

void SeqScrStepFrameSelection(int delta)
{
    std::vector<int> rows = SeqScrVisibleFrameRows();
    if (rows.empty()) return;
    int pos = -1;
    for (int i = 0; i < (int)rows.size(); i++)
        if (rows[(size_t)i] == s_seqscr_lib_selected) { pos = i; break; }
    if (pos < 0)
        pos = delta >= 0 ? 0 : (int)rows.size() - 1;
    else
        pos = (pos + delta + (int)rows.size()) % (int)rows.size();
    s_seqscr_lib_selected = rows[(size_t)pos];
    s_seqscr_lib_scroll_to_sel = true;
}

void SeqScrToggleSelectedMark(void)
{
    const SeqScrFrameLibrary &lib = g_seqscr_frame_lib;
    if (s_seqscr_lib_selected < 0 ||
        s_seqscr_lib_selected >= (int)lib.frames.size())
        return;
    const SeqScrFrameRef &ref = lib.frames[(size_t)s_seqscr_lib_selected];
    Document *owner = document_get(ref.doc_idx);
    IMG *img = doc_get_img(owner, ref.img_idx);
    if (!img) return;
    img->flags ^= 1;
    if (owner) owner->dirty = true;
}

static int SeqScrFindOpenDoc(const std::string &full_path)
{
    for (int i = 0; i < document_tab_count(); i++) {
        if (WorldPathsEqual(WorldDocFullPath(document_get(i)), full_path))
            return i;
    }
    return -1;
}

/* "CAGE3.IMG" -> stem "CAGE", number 3. False when the name carries no
   trailing number, in which case the file stands alone. */
static bool SeqScrSplitNumberedName(const std::string &file,
                                    std::string *stem_out, int *num_out)
{
    size_t dot = file.find_last_of('.');
    std::string base = (dot == std::string::npos) ? file : file.substr(0, dot);
    size_t end = base.size();
    size_t digits = end;
    while (digits > 0 && isdigit((unsigned char)base[digits - 1])) digits--;
    if (digits == end || digits == 0) return false;
    if (stem_out) *stem_out = base.substr(0, digits);
    if (num_out) *num_out = atoi(base.c_str() + digits);
    return true;
}

/* True when `name` is a chopped piece of another sprite -- the same
   parent/child rule the Assets image list folds by.

   The parent is looked for in `doc` first and then in every other open tab.
   Restricting it to the owning document was fine while a character was one
   file, but sprites get sideloaded: keep the pieces in UGM09SP.IMG and their
   parent in UGM09.IMG and every piece looked like a top-level frame, so the
   library listed them and sequences built from it were full of chop pieces.
   The pieces are pieces wherever they were parked; LOAD2 sees one library at
   compile time and so should this. */
static bool SeqScrDocHasSprite(Document *doc, const std::string &name)
{
    for (IMG *img = doc ? (IMG *)doc->img_p : NULL; img; img = (IMG *)img->nxt_p)
        if (img_name_string(img) == name) return true;
    return false;
}

static bool SeqScrIsSubframe(Document *doc, const char *name)
{
    std::string parent = InferSubframeParentName(name);
    if (parent.empty()) return false;
    if (SeqScrDocHasSprite(doc, parent)) return true;
    for (int i = 0; i < document_tab_count(); i++) {
        Document *other = document_get(i);
        if (!other || other == doc) continue;
        if (SeqScrDocHasSprite(other, parent)) return true;
    }
    return false;
}

/* Pieces of `parent_name`, counted across every open tab for the same
   reason -- a chop split over two files still has all its pieces. */
static int SeqScrCountSubframes(Document *doc, const std::string &parent_name)
{
    int count = 0;
    for (int i = -1; i < document_tab_count(); i++) {
        Document *scan = (i < 0) ? doc : document_get(i);
        if (!scan) continue;
        if (i >= 0 && scan == doc) continue;   /* already counted */
        for (IMG *img = (IMG *)scan->img_p; img; img = (IMG *)img->nxt_p) {
            std::string nm = img_name_string(img);
            if (nm != parent_name &&
                InferSubframeParentName(nm.c_str()) == parent_name)
                count++;
        }
    }
    return count;
}

void SeqScrRebuildFrameLibrary(bool open_missing)
{
    SeqScrFrameLibrary lib;
    Document *active = g_doc;
    if (!active || !active->fname_s[0]) {
        g_seqscr_frame_lib = lib;
        s_seqscr_lib_selected = -1;
        return;
    }

    std::string dir(active->fpath_s);
    std::string active_file(active->fname_s);
    int active_num = 0;
    if (!SeqScrSplitNumberedName(active_file, &lib.stem, &active_num) ||
        dir.empty()) {
        /* Unnumbered or in-memory document: the set is just this file. */
        lib.stem = active_file;
        lib.files.push_back(active_file);
        lib.file_docs.push_back(document_active_index());
    } else {
        std::vector<FileEntry> entries;
        GetDirectoryFiles(dir, entries, "IMG");
        std::vector<std::pair<int, std::string>> numbered;
        for (const FileEntry &e : entries) {
            if (e.is_dir) continue;
            std::string stem;
            int num = 0;
            if (!SeqScrSplitNumberedName(e.name, &stem, &num)) continue;
            if (stem.size() != lib.stem.size()) continue;
            bool same = true;
            for (size_t i = 0; i < stem.size() && same; i++)
                same = tolower((unsigned char)stem[i]) ==
                       tolower((unsigned char)lib.stem[i]);
            if (!same) continue;
            numbered.push_back({num, e.name});
        }
        std::sort(numbered.begin(), numbered.end(),
                  [](const std::pair<int, std::string> &a,
                     const std::pair<int, std::string> &b) {
                      return a.first < b.first;
                  });
        for (auto &nf : numbered) {
            lib.files.push_back(nf.second);
            lib.file_docs.push_back(SeqScrFindOpenDoc(PathCombine(dir, nf.second)));
        }
    }

    /* Opening a sibling activates its tab, so remember where we were and go
       back: browsing the library must not move the user's editing focus. */
    if (open_missing && !dir.empty()) {
        int restore = document_active_index();
        std::string restore_path = WorldDocFullPath(document_get(restore));
        bool opened_any = false;
        for (size_t i = 0; i < lib.files.size(); i++) {
            if (lib.file_docs[i] >= 0) continue;
            std::string path = PathCombine(dir, lib.files[i]);
            OpenImgFile(path);
            opened_any = true;
        }
        if (opened_any) {
            for (size_t i = 0; i < lib.files.size(); i++)
                lib.file_docs[i] = SeqScrFindOpenDoc(PathCombine(dir, lib.files[i]));
            int back = restore_path.empty() ? restore
                                            : SeqScrFindOpenDoc(restore_path);
            if (back >= 0) {
                document_set_active(back);
                g_doc_tab_select_request = back;
            }
        }
    }

    /* Anything else the user has open joins the set, whether or not its name
       fits the numbered-sibling pattern. Sideloaded scrap files are the point:
       UGM09SP.IMG does not split into stem+number at all (the digits are not
       at the end), so the pattern search found nothing but itself and every
       sprite the user had deliberately opened alongside it was invisible here.
       An open tab is an explicit statement that the file belongs to this
       character, and by the time LOAD2 packs it they are one library anyway. */
    for (int di = 0; di < document_tab_count(); di++) {
        Document *doc = document_get(di);
        if (!doc || !doc->fname_s[0] || doc->imgcnt == 0) continue;
        bool already = false;
        for (size_t i = 0; i < lib.file_docs.size() && !already; i++)
            already = (lib.file_docs[i] == di);
        if (already) continue;
        lib.files.push_back(doc->fname_s);
        lib.file_docs.push_back(di);
    }

    for (size_t i = 0; i < lib.files.size(); i++) {
        int doc_idx = lib.file_docs[i];
        Document *doc = document_get(doc_idx);
        if (!doc) continue;
        lib.open_files++;
        int img_idx = 0;
        for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
            std::string nm = img_name_string(img);
            if (SeqScrIsSubframe(doc, nm.c_str())) continue;
            SeqScrFrameRef ref;
            ref.doc_idx = doc_idx;
            ref.img_idx = img_idx;
            ref.file_slot = (int)i;
            ref.name = nm;
            ref.subframes = SeqScrCountSubframes(doc, nm);
            lib.frames.push_back(ref);
        }
    }

    g_seqscr_frame_lib = lib;
    if (s_seqscr_lib_selected >= (int)lib.frames.size())
        s_seqscr_lib_selected = -1;
    if (s_seqscr_lib_file_filter >= (int)lib.files.size())
        s_seqscr_lib_file_filter = -1;
}

bool SeqScrBrowserPreviewFrame(int *doc_idx, int *img_idx)
{
    const SeqScrFrameLibrary &lib = g_seqscr_frame_lib;
    if (s_seqscr_lib_selected < 0 ||
        s_seqscr_lib_selected >= (int)lib.frames.size())
        return false;
    const SeqScrFrameRef &ref = lib.frames[(size_t)s_seqscr_lib_selected];
    if (!doc_get_img(document_get(ref.doc_idx), ref.img_idx)) return false;
    if (doc_idx) *doc_idx = ref.doc_idx;
    if (img_idx) *img_idx = ref.img_idx;
    return true;
}

int SeqScrForeignEntryCount(const WorldMarkedSequenceState &state)
{
    if (!state.embedded_active) return 0;
    const int slot = kWorldEmbeddedSeqScrSlot;
    const std::vector<int> &fdoc = state.frame_doc[slot];
    int n = (int)state.sequence_frames[slot].size();
    int foreign = 0;
    for (int i = 0; i < n && i < (int)fdoc.size(); i++) {
        if (fdoc[(size_t)i] >= 0 && fdoc[(size_t)i] != state.embedded_doc_idx)
            foreign++;
    }
    return foreign;
}

/* Append one entry to the loaded record's preview lane. `doc_idx` is stored
   per entry so a frame from a sibling IMG still resolves and still renders. */
static void SeqScrAppendPreviewEntry(WorldMarkedSequenceState &state,
                                     int doc_idx, int img_idx)
{
    const int slot = kWorldEmbeddedSeqScrSlot;
    std::vector<int> &frames = state.sequence_frames[slot];
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());
    frames.push_back(img_idx);
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->push_back(ref.fresh);
    state.entry_pieces[slot].push_back(std::vector<int>());
    state.frame_doc[slot].push_back(doc_idx);
}

struct SeqScrAddOutcome {
    int added = 0;
    int persisted = 0;
    int preview_only = 0;
    bool failed = false;
};

/* Add `refs` to the loaded record, in the order given. Frames from the record's
   own IMG are written into the SEQSCR blob; frames from a sibling IMG go into
   the preview lane only. Every frame lands in the lane so the order you built
   is the order you see. */
static SeqScrAddOutcome SeqScrAddFramesToLoadedRecord(
    WorldMarkedSequenceState &state, const std::vector<SeqScrFrameRef> &refs)
{
    SeqScrAddOutcome out;
    if (refs.empty() || !state.embedded_active || state.embedded_is_script ||
        !document_get(state.embedded_doc_idx)) {
        out.failed = true;
        return out;
    }

    /* Append to the lane only. SeqScrSyncLaneToBlob() writes the own-file
       entries into the record at the end of the frame, so there is exactly one
       place that edits the blob and one undo step for the whole add. */
    for (const SeqScrFrameRef &ref : refs) {
        SeqScrAppendPreviewEntry(state, ref.doc_idx, ref.img_idx);
        out.added++;
        if (ref.doc_idx != state.embedded_doc_idx) out.preview_only++;
        else out.persisted++;
    }
    state.paused = true;
    state.timer = 0.0f;
    return out;
}

/* Every marked frame in the library, in file then list order. Marking reuses
   the IMG's own mark bit, so a selection made here is the same one World View
   rows, TBL export, and "Add Marked Frames" already act on. */
static std::vector<SeqScrFrameRef> SeqScrMarkedLibraryFrames(const SeqScrImgCache &cache)
{
    std::vector<SeqScrFrameRef> out;
    const SeqScrFrameLibrary &lib = g_seqscr_frame_lib;
    for (const SeqScrFrameRef &ref : lib.frames) {
        IMG *img = cache.get(ref.doc_idx, ref.img_idx);
        if (img && (img->flags & 1)) out.push_back(ref);
    }
    return out;
}

void DrawSeqScrFrameBrowser(float avail_h)
{
    WorldMarkedSequenceState &state = g_world_marked_state;

    /* Rebuild whenever the numbered set could have changed: a different file
       opened, a tab closed, or sprites added/removed in the active one. */
    static int s_lib_doc_idx = -2;
    static unsigned int s_lib_imgcnt = 0;
    static int s_lib_tab_count = -1;
    static std::string s_lib_opened_stem;
    int active_doc_idx = document_active_index();
    bool stale = s_lib_doc_idx != active_doc_idx ||
                 s_lib_imgcnt != (g_doc ? g_doc->imgcnt : 0u) ||
                 s_lib_tab_count != document_tab_count();
    if (stale) {
        /* Auto-open the rest of the set once per stem: the point of the
           browser is that opening CAGE3 gives you CAGE1-10, but re-opening on
           every tab switch would thrash the tab bar. */
        SeqScrRebuildFrameLibrary(false);
        if (!g_seqscr_frame_lib.stem.empty() &&
            g_seqscr_frame_lib.stem != s_lib_opened_stem &&
            g_seqscr_frame_lib.open_files < (int)g_seqscr_frame_lib.files.size()) {
            s_lib_opened_stem = g_seqscr_frame_lib.stem;
            SeqScrRebuildFrameLibrary(true);
        }
        s_lib_doc_idx = document_active_index();
        s_lib_imgcnt = g_doc ? g_doc->imgcnt : 0u;
        s_lib_tab_count = document_tab_count();
    }

    const SeqScrFrameLibrary &lib = g_seqscr_frame_lib;
    if (lib.files.empty()) {
        ImGui::TextDisabled("Save or open an IMG to browse its frames.");
        return;
    }

    char combo_label[64];
    if (s_seqscr_lib_file_filter < 0)
        snprintf(combo_label, sizeof(combo_label), "%s1-%d  (all)",
                 lib.stem.c_str(), (int)lib.files.size());
    else
        snprintf(combo_label, sizeof(combo_label), "%s",
                 lib.files[(size_t)s_seqscr_lib_file_filter].c_str());
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##seqscr_lib_file", combo_label)) {
        char all_label[64];
        snprintf(all_label, sizeof(all_label), "All %d files",
                 (int)lib.files.size());
        if (ImGui::Selectable(all_label, s_seqscr_lib_file_filter < 0))
            s_seqscr_lib_file_filter = -1;
        for (int i = 0; i < (int)lib.files.size(); i++) {
            char label[80];
            int frames_here = 0;
            for (const SeqScrFrameRef &ref : lib.frames)
                if (ref.file_slot == i) frames_here++;
            snprintf(label, sizeof(label), "%s  (%d)%s",
                     lib.files[(size_t)i].c_str(), frames_here,
                     lib.file_docs[(size_t)i] < 0 ? "  closed" : "");
            if (ImGui::Selectable(label, s_seqscr_lib_file_filter == i))
                s_seqscr_lib_file_filter = i;
        }
        ImGui::EndCombo();
    }

    ImGui::SetNextItemWidth(-58.0f);
    ImGui::InputTextWithHint("##seqscr_lib_search", "filter",
                             s_seqscr_lib_search, sizeof(s_seqscr_lib_search));
    ImGui::SameLine();
    if (ImGui::SmallButton("Reload##seqscr_lib_reload")) {
        SeqScrRebuildFrameLibrary(true);
        s_lib_opened_stem = g_seqscr_frame_lib.stem;
        s_lib_tab_count = document_tab_count();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Rescan the folder and open any %s file that is not loaded yet.",
                          lib.stem.c_str());

    std::string filter_low(s_seqscr_lib_search);
    for (char &c : filter_low) c = (char)tolower((unsigned char)c);

    float list_h = avail_h;
    if (list_h < 120.0f) list_h = 120.0f;

    /* One O(total images) pass instead of an O(n) list walk per row. */
    SeqScrImgCache cache;
    cache.build(lib);

    int marked_total = 0;
    for (const SeqScrFrameRef &r : lib.frames) {
        IMG *m = cache.get(r.doc_idx, r.img_idx);
        if (m && (m->flags & 1)) marked_total++;
    }

    std::vector<int> rows = SeqScrVisibleFrameRows();
    int shown = (int)rows.size();

    if (ImGui::BeginListBox("##seqscr_lib_frames", ImVec2(-1, list_h))) {
        if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_palette_nav = false;
            g_seqscr_frame_nav = true;   /* Up/Down/Space now walk this list */
        }
        /* Clip to what is on screen. Submitting all ~470 rows every frame cost
           more than the animation had left to run on. */
        ImGuiListClipper clipper;
        clipper.Begin(shown);
        /* Keyboard stepping can land on a row the clipper would skip, so tell
           it to include that one. */
        if (s_seqscr_lib_scroll_to_sel) {
            for (int k = 0; k < shown; k++) {
                if (rows[(size_t)k] == s_seqscr_lib_selected) {
                    clipper.IncludeItemByIndex(k);
                    break;
                }
            }
        }
        while (clipper.Step())
        for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
            int i = rows[(size_t)row];
            const SeqScrFrameRef &ref = lib.frames[(size_t)i];
            IMG *img = cache.get(ref.doc_idx, ref.img_idx);
            if (!img) continue;
            bool marked = (img->flags & 1) != 0;

            ImGui::PushID(i);
            if (ImGui::Checkbox("##mark", &marked)) {
                if (marked) img->flags |= 1; else img->flags &= ~1;
                Document *owner = document_get(ref.doc_idx);
                if (owner) owner->dirty = true;
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Mark this frame. Marks are the IMG's own mark bit,\nshared with World View rows and TBL export.");
            ImGui::SameLine();

            char label[96];
            if (s_seqscr_lib_file_filter < 0) {
                snprintf(label, sizeof(label), "%.28s  %.10s%s", ref.name.c_str(),
                         lib.files[(size_t)ref.file_slot].c_str(),
                         ref.subframes > 0 ? " *" : "");
            } else {
                snprintf(label, sizeof(label), "%.34s%s", ref.name.c_str(),
                         ref.subframes > 0 ? "  *" : "");
            }
            bool selected = s_seqscr_lib_selected == i;
            /* Toggle, so the corner box can be handed back to the playhead
               without hunting for a separate control. */
            if (ImGui::Selectable(label, selected)) {
                s_seqscr_lib_selected = selected ? -1 : i;
                g_seqscr_frame_nav = true;
            }
            /* Keyboard stepping has to drag the viewport along with it. */
            if (selected && s_seqscr_lib_scroll_to_sel)
                ImGui::SetScrollHereY(0.5f);
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("[%d] %s in %s\n%dx%d  anipoint %d,%d%s\n%s",
                                  ref.img_idx, ref.name.c_str(),
                                  lib.files[(size_t)ref.file_slot].c_str(),
                                  (int)img->w, (int)img->h,
                                  (int)(short)img->anix, (int)(short)img->aniy,
                                  ref.subframes > 0 ? "\n* has subframe pieces" : "",
                                  selected
                                    ? "Click again to return the corner box to the playhead."
                                    : "Click to preview it in the Anim workspace corner box.");
            }
            ImGui::PopID();
        }
        if (shown == 0) ImGui::TextDisabled("No frames match.");
        ImGui::EndListBox();
    }
    s_seqscr_lib_scroll_to_sel = false;
    if (g_seqscr_frame_nav)
        ImGui::TextDisabled("Up/Down walk frames, Space marks.");

    ImGui::TextDisabled("%d frames, %d marked, %d/%d files open",
                        (int)lib.frames.size(), marked_total,
                        lib.open_files, (int)lib.files.size());

    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
    if (ImGui::Button("Mark Shown##seqscr_lib_mark_all", ImVec2(84, 20))) {
        for (int i : rows) {
            const SeqScrFrameRef &ref = lib.frames[(size_t)i];
            IMG *img = cache.get(ref.doc_idx, ref.img_idx);
            if (!img) continue;
            img->flags |= 1;
            Document *owner = document_get(ref.doc_idx);
            if (owner) owner->dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear##seqscr_lib_clear", ImVec2(-1, 20))) {
        for (const SeqScrFrameRef &ref : lib.frames) {
            IMG *img = cache.get(ref.doc_idx, ref.img_idx);
            if (img) img->flags &= ~1;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unmark every frame in the library, in all files.");

    bool seq_loaded = state.embedded_active && !state.embedded_is_script &&
                      state.embedded_record_index >= 0;
    std::vector<SeqScrFrameRef> marked = SeqScrMarkedLibraryFrames(cache);
    bool any_marked = !marked.empty();

    auto report = [&](const SeqScrAddOutcome &out) {
        if (out.failed) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Could not add frames to the sequence.");
        } else if (out.preview_only > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Added %d frame%s: %d saved into the IMG, %d preview/ASM only (from sibling files).",
                     out.added, out.added == 1 ? "" : "s",
                     out.persisted, out.preview_only);
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Added %d frame%s to the sequence.",
                     out.added, out.added == 1 ? "" : "s");
        }
        g_restore_msg_timer = 5.0f;
    };

    /* Renaming used to exist only in the raw Anim Scripts / Seqs window, behind
       an "Enable in-place editing" checkbox — so a sequence built here stayed
       called NEWSEQ with no visible way to change it. The name is what the ASM
       export emits, so it belongs next to the thing that creates sequences. */
    if (seq_loaded && state.embedded_record_index >= 0) {
        static int  s_rename_record = -1;
        static char s_rename_buf[17] = {};
        if (s_rename_record == state.embedded_record_index) {
            ImGui::SetNextItemWidth(-64.0f);
            bool commit = ImGui::InputText("##seqscr_lib_rename", s_rename_buf,
                                           sizeof(s_rename_buf),
                                           ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::SmallButton("OK##seqscr_lib_rename_ok") || commit) {
                if (SeqScrSetName(state.embedded_record_index, s_rename_buf)) {
                    state.embedded_name = s_rename_buf;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Renamed to '%s'.", s_rename_buf);
                } else {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Could not rename (blob truncated or missing).");
                }
                g_restore_msg_timer = 4.0f;
                s_rename_record = -1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("X##seqscr_lib_rename_cancel"))
                s_rename_record = -1;
        } else {
            ImGui::TextDisabled("Name:");
            ImGui::SameLine();
            ImGui::TextUnformatted(state.embedded_name.empty()
                                       ? "(unnamed)" : state.embedded_name.c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Rename##seqscr_lib_rename_start")) {
                s_rename_record = state.embedded_record_index;
                memset(s_rename_buf, 0, sizeof(s_rename_buf));
                strncpy(s_rename_buf, state.embedded_name.c_str(),
                        sizeof(s_rename_buf) - 1);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rename this record. Sequences and scripts are\n"
                                  "both renameable; the name is what the ASM\n"
                                  "export emits as the record label.\n"
                                  "Right-click any row in the list to rename it\n"
                                  "without loading it first.");
        }
    }

    ImGui::BeginDisabled(!any_marked || !seq_loaded);
    if (ImGui::Button("Add Marked to Sequence##seqscr_lib_add_cur", ImVec2(-1, 20)))
        report(SeqScrAddFramesToLoadedRecord(state, marked));
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered()) {
        if (!seq_loaded)
            ImGui::SetTooltip("Load a sequence in the Anim tab first.");
        else if (!any_marked)
            ImGui::SetTooltip("Mark some frames above first.");
        else
            ImGui::SetTooltip("Append the %d marked frames to '%s', in file order.",
                              (int)marked.size(),
                              state.embedded_name.empty() ? "(unnamed)"
                                                          : state.embedded_name.c_str());
    }

    ImGui::BeginDisabled(!any_marked || !g_doc);
    if (ImGui::Button("New Sequence from Marked##seqscr_lib_add_new", ImVec2(-1, 20))) {
        if (SeqScrAddRecord(false)) {
            int new_idx = (int)g_doc->seqcnt - 1;
            if (WorldLoadSeqScrRecord(new_idx)) {
                /* A fresh record has no entries; the load leaves an empty lane
                   that the append below fills. */
                report(SeqScrAddFramesToLoadedRecord(state, marked));
                /* Every new record is called NEWSEQ until told otherwise, and
                   a list of identical NEWSEQs is useless. Prompt while the
                   user still knows what they just built; Cancel leaves the
                   default, so nothing is forced. */
                SeqScrBeginRename(kSeqScrRenameAnimTab, new_idx, "NEWSEQ");
            } else {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Created the sequence but could not open it.");
                g_restore_msg_timer = 4.0f;
            }
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Could not create a sequence (anim blob is truncated or out of memory).");
            g_restore_msg_timer = 4.0f;
        }
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered() && any_marked)
        ImGui::SetTooltip("Append an empty sequence to %s and fill it with the %d marked frames.",
                          g_doc && g_doc->fname_s[0] ? g_doc->fname_s : "this IMG",
                          (int)marked.size());

    /* Scripts had no entry point here at all -- creating one meant opening the
       raw Anim Scripts / Seqs window. A script takes no marked frames, since
       its entries call sequences rather than naming sprites, so this is not
       gated on a selection. */
    ImGui::BeginDisabled(!g_doc);
    if (ImGui::Button("New Script##seqscr_lib_add_script", ImVec2(-1, 20))) {
        if (SeqScrAddRecord(true)) {
            int new_idx = (int)g_doc->seqcnt + (int)g_doc->scrcnt - 1;
            if (WorldLoadSeqScrRecord(new_idx)) {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Created a script. Add calls to sequences with the picker above.");
            } else {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Created the script but could not open it.");
            }
            g_restore_msg_timer = 5.0f;
            SeqScrBeginRename(kSeqScrRenameAnimTab, new_idx, "NEWSCRIPT");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Could not create a script (anim blob is truncated or out of memory).");
            g_restore_msg_timer = 4.0f;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Scripts chain sequences together. Create the sequences\n"
                          "first, then add calls to them from the script's row.");
    ImGui::EndDisabled();
    ImGui::PopStyleVar();

    if (seq_loaded) {
        int foreign = SeqScrForeignEntryCount(state);
        if (foreign > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "%d entr%s from sibling IMGs:", foreign,
                               foreign == 1 ? "y" : "ies");
            ImGui::TextWrapped("preview and ASM export only. A SEQSCR entry cannot name a sprite outside its own file.");
        }
    }
}

/* Transport, view toggles, and exports for the loaded record. Mirrors the
   World View header's vocabulary so the two modes feel like one tool.
   Returns false when the record was unloaded, so the caller can drop the now
   stale lane instead of drawing a frame's worth of emptied-out tables. */
static bool SeqScrDrawToolbar(WorldMarkedSequenceState &state,
                              const std::vector<WorldMarkedLane> &lanes,
                              IMG *selected_img, int active_doc_idx)
{
    int slot = kWorldEmbeddedSeqScrSlot;
    int n = (int)state.sequence_frames[slot].size();
    bool is_script = state.embedded_is_script;
    int entry = WorldMarkedFrameForTick(state, slot, n, state.frame,
                                        state.hold_end[slot]);
    int total_ticks = WorldMarkedSequenceTicks(state, slot, n);

    ImGui::Text("%s: %s   Entry %d/%d   Tick %d/%d",
                is_script ? "Script" : "Sequence",
                state.embedded_name.empty() ? "(unnamed)"
                                            : state.embedded_name.c_str(),
                n > 0 ? entry + 1 : 0, n, state.frame, total_ticks);
    ImGui::SameLine();
    if (ImGui::SmallButton("Close##seqscr_ws_close")) {
        WorldExitEmbeddedSeqScr(state);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Unloaded the sequence/script.");
        g_restore_msg_timer = 4.0f;
        return false;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unload this record. The SEQSCR data itself is untouched.");

    /* Scripts are inspected row by row rather than played: each script entry
       stands in for a whole sequence, so a wall-clock tick has no meaning. */
    ImGui::SameLine();
    ImGui::BeginDisabled(is_script);
    if (ImGui::SmallButton(state.paused ? "Play##seqscr_ws_play"
                                        : "Pause##seqscr_ws_play"))
        state.paused = !state.paused;
    ImGui::EndDisabled();
    if (is_script && ImGui::IsItemHovered())
        ImGui::SetTooltip("Scripts load as paused tables. Select rows to inspect/edit them.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Restart##seqscr_ws_restart"))
        WorldMarkedRestart(state);
    ImGui::SameLine();
    WorldDrawTickHoldControl(state, "seqscr_ws");
    ImGui::SameLine();
    WorldDrawTickRateReadout();
    ImGui::SameLine();
    ImGui::TextDisabled("Tick");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70.0f);
    int goto_tick = state.frame;
    if (ImGui::InputInt("##seqscr_ws_goto_tick", &goto_tick, 0, 0))
        WorldMarkedSetTick(state, goto_tick);

    /* Scripts call sequences, so the frame-adding controls above are useless
       to them -- a script populated with sprite indices would resolve to the
       wrong thing entirely. Without this, a script created here could only be
       filled in from the raw Anim Scripts / Seqs window. */
    if (is_script && state.embedded_record_index >= 0 &&
        active_doc_idx == state.embedded_doc_idx && g_doc && g_doc->seqcnt > 0) {
        std::vector<SeqScrRecordView> recs;
        bool trunc = false;
        SeqScrBuildRecords(recs, &trunc);

        static int s_call_target = 0;
        if (s_call_target >= (int)g_doc->seqcnt) s_call_target = (int)g_doc->seqcnt - 1;
        if (s_call_target < 0) s_call_target = 0;

        /* Name the sequences rather than making the user match raw indices to
           whatever the list is showing. */
        char preview[64];
        const char *tname = (s_call_target < (int)recs.size() && recs[(size_t)s_call_target].name[0])
                                ? recs[(size_t)s_call_target].name : "(unnamed)";
        snprintf(preview, sizeof(preview), "%02d  %.40s", s_call_target, tname);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(190.0f);
        if (ImGui::BeginCombo("##seqscr_ws_call_target", preview)) {
            for (int i = 0; i < (int)g_doc->seqcnt && i < (int)recs.size(); i++) {
                char row[64];
                snprintf(row, sizeof(row), "%02d  %.40s  (%d)", i,
                         recs[(size_t)i].name[0] ? recs[(size_t)i].name : "(unnamed)",
                         recs[(size_t)i].num);
                if (ImGui::Selectable(row, i == s_call_target)) s_call_target = i;
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Which sequence this script should call next.");

        ImGui::SameLine();
        if (ImGui::SmallButton("Add Call##seqscr_ws_add_call")) {
            if (SeqScrAppendEntry(state.embedded_record_index, s_call_target)) {
                /* Reload so the table shows the entry that was just written. */
                WorldLoadSeqScrRecord(state.embedded_record_index);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Added a call to sequence %02d.", s_call_target);
            } else {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Could not add the call (blob truncated or out of memory).");
            }
            g_restore_msg_timer = 4.0f;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Append a call to the chosen sequence, at the end of this script.");
    }

    /* A sequence entry can be a multi-sprite frame. Capture the sprite
       selected in the editor as another piece; its own anipoint anchors it to
       the same world frame as the primary sprite. */
    if (!is_script && active_doc_idx == state.embedded_doc_idx &&
        selected_img && g_doc && g_doc->ilselected >= 0 && n > 0) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Add Selected to Frame##seqscr_ws_add_piece")) {
            if (WorldMarkedAttachSpriteToFrame(state, slot, entry, g_doc,
                                               active_doc_idx,
                                               g_doc->ilselected)) {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Added selected sprite to sequence frame %d (each piece uses its own anipoint).",
                         entry + 1);
            } else {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Sprite is already in this frame, or belongs to another IMG tab.");
            }
            g_restore_msg_timer = 4.0f;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Make the currently selected sprite a second piece of this frame. Its anipoint is preserved, and the composite exports with the frame.");
    }

    if (ImGui::Checkbox("Game Placement##seqscr_ws_game_place",
                        &g_world_state.game_placement)) {
        if (g_world_state.game_placement) WorldApplyGamePlacement(state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Stand the animation on the playfield floor, the way the\n"
                          "game draws it, instead of hanging it off the stock\n"
                          "(%d, 20) anchor. Each entry then offsets by its own dX/dY.\n"
                          "Right-click to set the floor line.",
                          g_world_state.origin_x);
    if (ImGui::BeginPopupContextItem("##seqscr_ws_floor_cfg")) {
        ImGui::TextDisabled("Playfield floor, in world pixels from the top.");
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt("Floor Y##seqscr_floor_y", &g_world_state.floor_y)) {
            if (g_world_state.floor_y < 1) g_world_state.floor_y = 1;
            if (g_world_state.floor_y > 512) g_world_state.floor_y = 512;
            WorldApplyGamePlacement(state);
        }
        ImGui::SetNextItemWidth(110.0f);
        if (ImGui::InputInt("Stand X##seqscr_stand_x", &g_world_state.origin_x)) {
            if (g_world_state.origin_x < 0) g_world_state.origin_x = 0;
            if (g_world_state.origin_x > g_world_state.w) g_world_state.origin_x = g_world_state.w;
        }
        if (ImGui::SmallButton("Re-stand on first frame##seqscr_restand"))
            WorldApplyGamePlacement(state);
        ImGui::EndPopup();
    }
    ImGui::SameLine();
    /* Render rate next to the tick rate: ticks advance off real elapsed time,
       so timing stays right even when frames are dropped, but you can only SEE
       one frame per render. If this reads below the tick rate the animation is
       correct and under-sampled, which looks like judder. */
    {
        float render_fps = ImGui::GetIO().Framerate;
        bool starved = render_fps > 1.0f && render_fps < kMk2TickHz * 0.9f;
        if (starved)
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "anchor y=%d  %.0f fps < %.1f ticks/s",
                               g_world_state.origin_y, render_fps, kMk2TickHz);
        else
            ImGui::TextDisabled("anchor y=%d  %.0f fps",
                                g_world_state.origin_y, render_fps);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ticks advance from elapsed time, so a hold of N ticks\n"
                              "lasts N/%.1f s regardless of frame rate. When the render\n"
                              "rate falls below the tick rate you see fewer than every\n"
                              "frame, which reads as judder rather than wrong timing.",
                              kMk2TickHz);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy ASM##seqscr_ws_copy_asm")) {
        state.generated_asm = WorldBuildSeqScrAsmExport(state.embedded_record_index);
        ImGui::SetClipboardText(state.generated_asm.c_str());
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Copied anim ASM for the loaded record.");
        g_restore_msg_timer = 4.0f;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("View ASM##seqscr_ws_view_asm")) {
        state.generated_asm = WorldBuildSeqScrAsmExport(state.embedded_record_index);
        state.show_asm = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save ASM##seqscr_ws_save_asm")) {
        state.generated_asm = WorldBuildSeqScrAsmExport(state.embedded_record_index);
        g_request_save_world_asm = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save PNG##seqscr_ws_save_png"))
        g_request_save_world_png = true;
    ImGui::SameLine();
    if (ImGui::SmallButton("Save PNG Seq##seqscr_ws_save_png_seq"))
        g_request_save_world_png_seq = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Write one PNG per tick across the whole record,\n"
                          "numbered <name>_0000.PNG onward.");
    ImGui::SameLine();
    ImGui::TextDisabled("%d lane%s", (int)lanes.size(),
                        lanes.size() == 1 ? "" : "s");
    return true;
}

/* The Anim viewport draws the loaded record and nothing else: no lane tags, no
   TV-safe bounds, no reference figure, no marked-row status line. Those are
   World View's staging overlays and belong to World View. */
static WorldMarkedSceneResult SeqScrDrawScene(WorldMarkedSequenceState &state,
                                              const std::vector<WorldMarkedLane> &lanes,
                                              ImVec2 avail, ImVec2 img_pos)
{
    WorldMarkedSceneResult result = {};
    result.layout = ComputeWorldCanvasLayout(avail, img_pos,
                                             g_world_state.w, g_world_state.h,
                                             g_world_state.origin_x,
                                             g_world_state.origin_y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 world_pos = result.layout.pos;
    float ox = result.layout.origin_x;
    float oy = result.layout.origin_y;

    dl->AddRectFilled(world_pos,
                      ImVec2(world_pos.x + result.layout.width,
                             world_pos.y + result.layout.height),
                      IM_COL32(0, 0, 0, 255));
    dl->AddLine(ImVec2(ox - 8, oy), ImVec2(ox + 8, oy), IM_COL32(120, 120, 120, 255));
    dl->AddLine(ImVec2(ox, oy - 8), ImVec2(ox, oy + 8), IM_COL32(120, 120, 120, 255));

    WorldDrawMarkedLaneSprites(dl, state, lanes, result.layout,
                               result.render_info);
    dl->AddCircle(ImVec2(ox, oy), 4.0f, IM_COL32(255, 200, 0, 255), 0, 1.5f);

    /* Dragging a sprite edits dX/dY, which are real ENTRY fields, so it stays.
       The panel rect is parked off-screen: this mode has no floating panel for
       the drag code to avoid. */
    WorldMarkedPanelLayout no_panel;
    no_panel.pos = ImVec2(-10000.0f, -10000.0f);
    WorldHandleMarkedLaneDrag(dl, state, lanes, result.render_info,
                              result.layout, no_panel,
                              img_pos, ImVec2(img_pos.x + avail.x,
                                              img_pos.y + avail.y));
    result.panel_layout = no_panel;
    return result;
}

bool DrawSeqScrWorkspace(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    WorldMarkedSequenceState &state = g_world_marked_state;
    IMG *selected_img = get_img(g_doc ? g_doc->ilselected : -1);
    int active_doc_idx = document_active_index();
    if (avail.x < 32.0f || avail.y < 32.0f) return true;
    ImGui::SetCursorScreenPos(img_pos);

    /* A record loaded from a document that has since closed leaves stale slot
       data behind; drop it rather than animating a dangling lane. */
    if (state.embedded_active && !document_get(state.embedded_doc_idx))
        WorldExitEmbeddedSeqScr(state);

    std::vector<WorldMarkedLane> lanes;
    lanes.reserve(2);
    int embedded_lane_idx = -1;
    if (WorldAppendEmbeddedSeqScrLane(state, lanes))
        embedded_lane_idx = (int)lanes.size() - 1;

    if (embedded_lane_idx >= 0) {
        state.marked_play = true;
        WorldUpdateMarkedLanePlayback(state, lanes, io.DeltaTime);
        if (!SeqScrDrawToolbar(state, lanes, selected_img, active_doc_idx)) {
            lanes.clear();
            embedded_lane_idx = -1;
        }
    } else {
        ImGui::TextDisabled("No sequence or script loaded. Pick one from the list below.");
    }

    /* Split what's left: viewport on top, record list + entry table beneath.
       ComputeWorldCanvasLayout never scales the playfield below 2x and centres
       it in whatever it is handed, so a viewport shorter than that overflows
       in both directions — over the toolbar above and the table below. Ask for
       the height that rule actually wants, cap it so the table keeps a usable
       share, and clip the remainder to the viewport rect. */
    ImVec2 body_pos = ImGui::GetCursorScreenPos();
    ImVec2 body_avail = ImGui::GetContentRegionAvail();
    if (body_avail.x < 32.0f || body_avail.y < 32.0f) return true;

    /* The table claims the height its rows actually need, so a short sequence
       is fully visible without scrolling; the viewport takes the rest, keeping
       a floor so the animation stays readable. A long record still scrolls,
       but only once it has taken its share of the window. */
    int entry_count = (embedded_lane_idx >= 0)
                    ? (int)lanes[(size_t)embedded_lane_idx].frames.size() : 0;
    float row_h = ImGui::GetFrameHeight() + ImGui::GetStyle().CellPadding.y * 2.0f;
    float table_chrome = ImGui::GetTextLineHeightWithSpacing() * 2.0f + 24.0f;
    float thumbs_h = (embedded_lane_idx >= 0 && !state.embedded_is_script)
                   ? 48.0f : 0.0f;
    float wanted_table_h = table_chrome + thumbs_h +
                           (float)(entry_count + 1) * row_h + 16.0f;
    if (entry_count <= 0) wanted_table_h = 200.0f;

    float min_anim_h = 240.0f;
    float table_h = wanted_table_h;
    if (table_h > body_avail.y - min_anim_h) table_h = body_avail.y - min_anim_h;
    if (table_h < 140.0f) table_h = 140.0f;
    if (table_h > body_avail.y - 60.0f) table_h = body_avail.y - 60.0f;
    float anim_h = body_avail.y - table_h - 6.0f;
    if (anim_h < 48.0f) anim_h = 48.0f;
    ImVec2 anim_avail(body_avail.x, anim_h);
    ImVec2 anim_max(body_pos.x + anim_avail.x, body_pos.y + anim_avail.y);

    if (embedded_lane_idx >= 0) {
        /* ComputeWorldCanvasLayout never scales the playfield below 2x and
           centres it in whatever it is handed, so it can overflow the viewport
           rect in both directions. Clip it rather than let it paint over the
           toolbar above and the table below. */
        ImGui::PushClipRect(body_pos, anim_max, true);
        SeqScrDrawScene(state, lanes, anim_avail, body_pos);
        ImGui::PopClipRect();
        SeqScrDrawSpriteInspector(&lanes[(size_t)embedded_lane_idx], body_pos,
                                  anim_max);
    } else {
        ImGui::GetWindowDrawList()->AddRectFilled(body_pos, anim_max,
                                                  IM_COL32(0, 0, 0, 255));
        /* With no record loaded the box is still the browser's viewfinder. */
        if (SeqScrBrowserPreviewFrame(NULL, NULL))
            SeqScrDrawSpriteInspector(NULL, body_pos, anim_max);
    }
    ImGui::SetCursorScreenPos(body_pos);
    ImGui::Dummy(anim_avail);

    int load_request = -1;
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6.0f, 4.0f));
    if (ImGui::BeginChild("##seqscr_ws_bottom", ImVec2(body_avail.x, table_h),
                          false)) {
        float list_w = body_avail.x * 0.24f;
        if (list_w < 190.0f) list_w = 190.0f;
        if (list_w > 300.0f) list_w = 300.0f;
        if (ImGui::BeginChild("##seqscr_ws_list", ImVec2(list_w, 0.0f), true))
            load_request = SeqScrDrawRecordPicker(state);
        ImGui::EndChild();
        ImGui::SameLine();
        if (ImGui::BeginChild("##seqscr_ws_entries", ImVec2(0.0f, 0.0f), true,
                              ImGuiWindowFlags_HorizontalScrollbar)) {
            if (embedded_lane_idx >= 0) {
                WorldMarkedLane &lane = lanes[(size_t)embedded_lane_idx];
                float inner = ImGui::GetContentRegionAvail().y;
                float rows_h = inner - ImGui::GetTextLineHeightWithSpacing() -
                               (state.embedded_is_script ? 8.0f : 52.0f);
                if (rows_h < 90.0f) rows_h = 90.0f;
                if (state.embedded_is_script) {
                    SeqScrDrawScriptTable(state, lane, rows_h);
                } else {
                    SeqScrDrawEntryTable(state, lane, rows_h);
                    WorldMarkedLaneThumbClick thumb_click =
                        WorldDrawMarkedLaneThumbnails(state, lane, lanes);
                    if (thumb_click.clicked) {
                        state.active_slot = lane.delay_slot;
                        Document *doc = document_get(thumb_click.doc_idx);
                        WorldSyncEditorSelectionToSprite(doc,
                                                         thumb_click.doc_idx,
                                                         thumb_click.img_idx);
                    }
                }
            } else {
                ImGui::TextDisabled("Select a sequence or script on the left to build and preview it here.");
                ImGui::Spacing();
                ImGui::TextWrapped("Sequences are frame lists: each entry targets an IMG sprite, with Ticks as the hold and dX/dY as local animation offsets.");
                ImGui::TextWrapped("Scripts chain sequences: each entry targets a sequence, and \"Load Seq\" opens that sequence here.");
            }
        }
        ImGui::EndChild();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();

    if (WorldDrawMarkedAsmPopup(state)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Copied anim ASM.");
        g_restore_msg_timer = 4.0f;
    }

    /* Push this frame's edits — typed fields, row reorders, viewport drags —
       into the record. A no-op when nothing moved, so idle frames neither push
       undo nor dirty the document. Runs before any pending load so an edit is
       never lost by switching records. */
    if (embedded_lane_idx >= 0)
        SeqScrSyncLaneToBlob(state);

    /* Deferred to here: loading rewrites the embedded slot the entry table
       above was drawing from. */
    if (load_request >= 0 && !WorldLoadSeqScrRecord(load_request)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Could not load that sequence/script.");
        g_restore_msg_timer = 4.0f;
    }
    return true;
}

/* ---- World View PNG export ----------------------------------------
   Rebuilds the same lane set the World View draws, composites it in world
   pixels, and writes it out. Kept separate from the draw path so an export
   never depends on the panel being on screen or on a particular zoom. */

static bool WorldBuildExportLanes(std::vector<WorldMarkedLane> &lanes)
{
    WorldMarkedSequenceState &state = g_world_marked_state;
    lanes.clear();
    lanes.reserve(kWorldMarkedMaxTabs);

    bool dummy_decap_missing = false;
    /* Export whatever the active canvas mode is showing: the Sequence/Script
       workspace's own record (plus its companions, when enabled), or World
       View's marked rows and ASM lanes. */
    bool embedded_focus_only = g_seqscr_workspace && state.embedded_active &&
                               !state.embedded_show_companions;
    if (!embedded_focus_only) {
        WorldAppendMarkedDocumentLanes(state, document_active_index(), lanes,
                                       &dummy_decap_missing);
    }
    if (g_seqscr_workspace)
        WorldAppendEmbeddedSeqScrLane(state, lanes);

    if (!embedded_focus_only) {
        std::vector<WorldMarkedAsmLaneInput> asm_lanes;
        WorldCollectActiveAsmLanes(asm_lanes);
        for (const WorldMarkedAsmLaneInput &input : asm_lanes) {
            if (!input.enabled) continue;
            WorldAppendAsmLane(state, input.name, input.frames, input.doc,
                               input.doc_idx, input.slot_id, lanes);
        }
    }
    /* Export in the order the panel shows, so a PNG composite and the row
       list cannot disagree about which lane is in front. */
    WorldMarkedApplyLaneOrder(state, lanes);
    return !lanes.empty();
}

/* Tightest rect containing any non-transparent pixel. Returns false when the
   buffer is entirely empty. */
static bool WorldRgbaContentBounds(const std::vector<unsigned char> &rgba,
                                   int w, int h,
                                   int *out_x, int *out_y,
                                   int *out_w, int *out_h)
{
    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; x++) {
            if (row[x * 4 + 3] == 0) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x) return false;
    *out_x = min_x;
    *out_y = min_y;
    *out_w = max_x - min_x + 1;
    *out_h = max_y - min_y + 1;
    return true;
}

static void WorldCropRgba(const std::vector<unsigned char> &src, int src_w,
                          int x, int y, int w, int h,
                          std::vector<unsigned char> &dst)
{
    dst.assign((size_t)w * (size_t)h * 4u, 0);
    for (int row = 0; row < h; row++) {
        memcpy(dst.data() + (size_t)row * w * 4,
               src.data() + ((size_t)(y + row) * src_w + x) * 4,
               (size_t)w * 4u);
    }
}

/* Longest lane, in ticks — how many frames a full sequence export covers. */
static int WorldExportTotalTicks(const std::vector<WorldMarkedLane> &lanes)
{
    WorldMarkedSequenceState &state = g_world_marked_state;
    int total = 0;
    for (const WorldMarkedLane &lane : lanes) {
        int n = (int)lane.frames.size();
        if (n <= 0) continue;
        int ticks = WorldMarkedSequenceTicks(state, lane.delay_slot, n);
        if (ticks > total) total = ticks;
        /* An entry can be scheduled to stay visible past its own lane's
           cycle; make sure those tail ticks are exported too. */
        for (int fi = 0; fi < n && fi < (int)state.visible_until[lane.delay_slot].size(); fi++) {
            int until = state.visible_until[lane.delay_slot][fi];
            if (until > total) total = until;
        }
    }
    return total;
}

/* Build the composite for the current tick. Returns false when nothing at all
   landed inside the world rect. */
static bool WorldComposeCurrentTick(std::vector<WorldMarkedLane> &lanes,
                                    bool crop, bool use_lane_alpha,
                                    std::vector<unsigned char> &out,
                                    int *out_w, int *out_h)
{
    WorldMarkedSequenceState &state = g_world_marked_state;
    int world_w = g_world_state.w > 0 ? g_world_state.w : 400;
    int world_h = g_world_state.h > 0 ? g_world_state.h : 254;

    if (!WorldUpdateMarkedLanePlayback(state, lanes, 0.0f)) return false;
    std::vector<unsigned char> full;
    if (WorldComposeMarkedSceneRgba(state, lanes, world_w, world_h,
                                    g_world_state.origin_x,
                                    g_world_state.origin_y,
                                    use_lane_alpha, full) <= 0)
        return false;

    if (crop) {
        int cx, cy, cw, ch;
        if (!WorldRgbaContentBounds(full, world_w, world_h, &cx, &cy, &cw, &ch))
            return false;
        WorldCropRgba(full, world_w, cx, cy, cw, ch, out);
        *out_w = cw;
        *out_h = ch;
        return true;
    }

    out.swap(full);
    *out_w = world_w;
    *out_h = world_h;
    return true;
}

bool ExportWorldViewPng(const char *path, bool crop_to_content, bool use_lane_alpha)
{
    if (!path || !*path) return false;

    std::vector<WorldMarkedLane> lanes;
    if (!WorldBuildExportLanes(lanes)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "World View PNG export needs at least one marked/ASM lane.");
        g_restore_msg_timer = 4.0f;
        return false;
    }

    /* Freeze playback for the duration so the exported frame is exactly the
       tick on screen, then restore whatever the user had running. */
    WorldMarkedSequenceState &state = g_world_marked_state;
    bool was_paused = state.paused;
    float was_timer = state.timer;
    state.paused = true;
    state.timer = 0.0f;

    std::vector<unsigned char> rgba;
    int w = 0, h = 0;
    bool composed = WorldComposeCurrentTick(lanes, crop_to_content, use_lane_alpha,
                                            rgba, &w, &h);

    state.paused = was_paused;
    state.timer = was_timer;

    if (!composed) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Nothing visible at tick %d — no PNG written.", state.frame);
        g_restore_msg_timer = 4.0f;
        return false;
    }
    if (!WriteRgbaPng(path, w, h, rgba.data())) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "World View PNG export failed.");
        g_restore_msg_timer = 4.0f;
        return false;
    }

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Exported World View PNG (%dx%d) at tick %d.", w, h, state.frame);
    g_restore_msg_timer = 4.0f;
    return true;
}

/* Write one PNG per tick, named <base>_0000.png ... Frames share a single crop
   rect (the union of every tick's content) so the sequence stays registered. */
int ExportWorldViewPngSequence(const char *path_base, bool crop_to_content,
                               bool use_lane_alpha, int *out_total_ticks)
{
    if (out_total_ticks) *out_total_ticks = 0;
    if (!path_base || !*path_base) return 0;

    std::vector<WorldMarkedLane> lanes;
    if (!WorldBuildExportLanes(lanes)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "World View PNG export needs at least one marked/ASM lane.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    WorldMarkedSequenceState &state = g_world_marked_state;
    int total_ticks = WorldExportTotalTicks(lanes);
    if (total_ticks <= 0) total_ticks = 1;
    if (total_ticks > kWorldPngSequenceMaxFrames)
        total_ticks = kWorldPngSequenceMaxFrames;
    if (out_total_ticks) *out_total_ticks = total_ticks;

    int world_w = g_world_state.w > 0 ? g_world_state.w : 400;
    int world_h = g_world_state.h > 0 ? g_world_state.h : 254;

    bool was_paused = state.paused;
    float was_timer = state.timer;
    int was_frame = state.frame;
    state.paused = true;
    state.timer = 0.0f;

    /* Pass 1 (crop only): union of content across every tick. */
    int crop_x = 0, crop_y = 0, crop_w = world_w, crop_h = world_h;
    if (crop_to_content) {
        int min_x = world_w, min_y = world_h, max_x = -1, max_y = -1;
        for (int tick = 0; tick < total_ticks; tick++) {
            state.frame = tick;
            std::vector<WorldMarkedLane> tick_lanes;
            if (!WorldBuildExportLanes(tick_lanes)) continue;
            if (!WorldUpdateMarkedLanePlayback(state, tick_lanes, 0.0f)) continue;
            std::vector<unsigned char> full;
            if (WorldComposeMarkedSceneRgba(state, tick_lanes, world_w, world_h,
                                            g_world_state.origin_x,
                                            g_world_state.origin_y,
                                            use_lane_alpha, full) <= 0)
                continue;
            int bx, by, bw, bh;
            if (!WorldRgbaContentBounds(full, world_w, world_h, &bx, &by, &bw, &bh))
                continue;
            if (bx < min_x) min_x = bx;
            if (by < min_y) min_y = by;
            if (bx + bw - 1 > max_x) max_x = bx + bw - 1;
            if (by + bh - 1 > max_y) max_y = by + bh - 1;
        }
        if (max_x >= min_x) {
            crop_x = min_x;
            crop_y = min_y;
            crop_w = max_x - min_x + 1;
            crop_h = max_y - min_y + 1;
        }
    }

    /* Strip any extension off the base so we can suffix the tick number. */
    std::string base = path_base;
    size_t dot = base.find_last_of('.');
    size_t sep = base.find_last_of("\\/");
    if (dot != std::string::npos && (sep == std::string::npos || dot > sep))
        base = base.substr(0, dot);

    int written = 0;
    std::vector<unsigned char> cropped;
    for (int tick = 0; tick < total_ticks; tick++) {
        state.frame = tick;
        std::vector<WorldMarkedLane> tick_lanes;
        if (!WorldBuildExportLanes(tick_lanes)) continue;
        if (!WorldUpdateMarkedLanePlayback(state, tick_lanes, 0.0f)) continue;

        std::vector<unsigned char> full;
        if (WorldComposeMarkedSceneRgba(state, tick_lanes, world_w, world_h,
                                        g_world_state.origin_x,
                                        g_world_state.origin_y,
                                        use_lane_alpha, full) <= 0)
            continue;

        const unsigned char *pixels = full.data();
        if (crop_w != world_w || crop_h != world_h) {
            WorldCropRgba(full, world_w, crop_x, crop_y, crop_w, crop_h, cropped);
            pixels = cropped.data();
        }

        char out_path[1200];
        snprintf(out_path, sizeof(out_path), "%s_%04d.PNG", base.c_str(), tick);
        if (WriteRgbaPng(out_path, crop_w, crop_h, pixels))
            written++;
    }

    state.frame = was_frame;
    state.paused = was_paused;
    state.timer = was_timer;

    if (written > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Exported %d World View PNG frame%s (%dx%d) over %d tick%s.",
                 written, written == 1 ? "" : "s", crop_w, crop_h,
                 total_ticks, total_ticks == 1 ? "" : "s");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "World View PNG sequence export wrote no frames.");
    }
    g_restore_msg_timer = 5.0f;
    return written;
}

void DrawWorldMarkedTimelinePanel(void)
{
    if (!g_world_state.enabled || !g_world_marked_state.marked_play) {
        ImGui::TextDisabled("World View frame sequence is inactive.");
        return;
    }

    std::vector<WorldMarkedLane> lanes;
    lanes.reserve(kWorldMarkedMaxTabs);
    bool dummy_decap_missing = false;
    WorldAppendMarkedDocumentLanes(g_world_marked_state,
                                   document_active_index(), lanes,
                                   &dummy_decap_missing);

    std::vector<WorldMarkedAsmLaneInput> asm_lanes;
    WorldCollectActiveAsmLanes(asm_lanes);
    for (const WorldMarkedAsmLaneInput &input : asm_lanes) {
        if (!input.enabled) continue;
        WorldAppendAsmLane(g_world_marked_state, input.name,
                           input.frames, input.doc, input.doc_idx,
                           input.slot_id, lanes);
    }

    WorldMarkedApplyLaneOrder(g_world_marked_state, lanes);

    /* Both of these used to return early with a line of grey text, which took
       the whole Frame Sequence strip with them -- transport, Ticks/frame, and
       the File/Overlays/Lanes menus. Those are scene-wide controls, and the
       moment nothing is marked is exactly when someone wants to load a
       project or check the tick rate. Draw the panel either way; the reason
       goes inside it, under the header. */
    WorldUpdateMarkedLanePlayback(g_world_marked_state, lanes, 0.0f);

    WorldMarkedPanelLayout layout;
    layout.pos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    layout.width = avail.x;
    layout.height = avail.y;
    if (layout.width < 240.0f) layout.width = 240.0f;
    if (layout.height < 88.0f) layout.height = 88.0f;

    IMG *selected_img = get_img(g_doc ? g_doc->ilselected : -1);
    WorldMarkedPanelResult panel_result =
        WorldDrawMarkedPanel(g_world_marked_state, lanes, layout,
                             dummy_decap_missing, selected_img,
                             document_active_index());
    WorldHandleMarkedPanelResult(panel_result);
}
