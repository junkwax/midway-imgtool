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

#include "anipoint.h"       /* secondary_anipoint_in_use */
#include "anipoint_edit.h"  /* set_primary_anipoint_with_sequence */
#include "img_format.h"     /* get_img */
#include "img_util.h"       /* img_name_string */
#include "shim_vid.h"       /* g_palette */
#include "ui_timeline.h"    /* ClampTimelineHold */
#include "world_render.h"   /* doc_get_img */

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>

static SDL_Texture *s_world_onion_tex = NULL;
static int s_world_onion_tex_w = 0;
static int s_world_onion_tex_h = 0;
static int s_world_onion_idx = -1;

WorldViewState &WorldView(void)
{
    static WorldViewState state;
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
                        layout.min.y + 62.0f);
    layout.blend_label_pos = ImVec2(layout.min.x + 8.0f,
                                    layout.min.y + 7.0f);
    layout.blend_control_pos = ImVec2(layout.min.x + 76.0f,
                                      layout.min.y + 5.0f);
    layout.opacity_label_pos = ImVec2(layout.min.x + 8.0f,
                                      layout.min.y + 34.0f);
    layout.opacity_control_pos = ImVec2(layout.min.x + 76.0f,
                                        layout.min.y + 32.0f);
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

void DrawCanvasZoomIndicator(bool zoom_fit, float zoom)
{
    if (zoom_fit) return;

    char zbuf[32];
    snprintf(zbuf, sizeof(zbuf), "%.0f%%", zoom * 100.0f);
    ImGui::SetCursorPos(ImVec2(8, 4));
    ImGui::TextDisabled("%s", zbuf);
}

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
    float fit_x = avail.x / (float)world_w;
    float fit_y = avail.y / (float)world_h;
    layout.scale = (fit_x < fit_y) ? fit_x : fit_y;
    if (layout.scale < 1.0f) layout.scale = 1.0f;
    layout.scale = (float)(int)layout.scale;
    if (layout.scale < 1.0f) layout.scale = 1.0f;

    layout.width = (float)world_w * layout.scale;
    layout.height = (float)world_h * layout.scale;
    layout.pos = ImVec2(img_pos.x + (avail.x - layout.width) * 0.5f,
                        img_pos.y + (avail.y - layout.height) * 0.5f);
    layout.origin_x = layout.pos.x + world_origin_x * layout.scale;
    layout.origin_y = layout.pos.y + world_origin_y * layout.scale;
    return layout;
}

WorldMarkedPanelLayout ComputeWorldMarkedPanelLayout(ImVec2 avail,
                                                     ImVec2 img_pos,
                                                     int lane_count)
{
    WorldMarkedPanelLayout layout;
    layout.width = avail.x - 16.0f;
    if (layout.width < 240.0f) layout.width = 240.0f;
    layout.height = 54.0f + (float)lane_count * 104.0f;
    float max_h = avail.y - 24.0f;
    if (max_h > 380.0f) max_h = 380.0f;
    if (layout.height > max_h) layout.height = max_h;
    if (layout.height < 96.0f) layout.height = 96.0f;
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

void WorldCollectMarkedFrames(Document *doc, std::vector<int> &out)
{
    out.clear();
    if (!doc) return;
    int idx = 0;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0)
            out.push_back(idx);
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

bool WorldAppendMarkedSourceLane(WorldMarkedSequenceState &state, int doc_idx,
                                 std::vector<WorldMarkedLane> &lanes)
{
    if ((int)lanes.size() >= kWorldMarkedSourceTabs) return false;
    Document *doc = document_get(doc_idx);
    if (!doc) return false;

    WorldMarkedLane lane = {};
    lane.doc = doc;
    lane.doc_idx = doc_idx;
    lane.delay_slot = (int)lanes.size();
    lane.frame_pos = 0;
    lane.img = NULL;
    lane.dummy_decap = false;
    WorldCollectMarkedFrames(doc, lane.frames);
    if (lane.frames.empty()) return false;

    WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                    lane.frame_pieces, lane.frame_labels);
    WorldMarkedSyncSequenceOverride(state, lane.delay_slot, doc, doc_idx,
                                    lane.frames, lane.frame_pieces,
                                    lane.frame_labels);
    lanes.push_back(lane);
    return true;
}

bool WorldAppendMarkedDocumentLanes(WorldMarkedSequenceState &state,
                                    int active_doc_idx,
                                    std::vector<WorldMarkedLane> &lanes,
                                    bool *dummy_decap_missing)
{
    if (dummy_decap_missing) *dummy_decap_missing = false;
    bool appended = false;

    for (int i = 0; i < document_tab_count(); i++) {
        if (WorldAppendMarkedSourceLane(state, i, lanes))
            appended = true;
        if ((int)lanes.size() >= kWorldMarkedSourceTabs) break;
    }

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
        const std::vector<Document*> *piece_doc = fr.piece_doc;
        if (piece_img) {
            for (size_t p = 0; p < piece_img->size(); p++) {
                int ri = (*piece_img)[p];
                if (ri < 0) continue;
                Document *pdoc = (piece_doc && p < piece_doc->size() &&
                                  (*piece_doc)[p])
                               ? (*piece_doc)[p] : doc;
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
    for (int k = 0; k < n; k++) {
        const WorldAsmLaneFrame &fr = frames[k];
        state.frame_delays[slot_id][k] = 1;
        state.local_dx[slot_id][k] = fr.dx;
        state.local_dy[slot_id][k] = fr.dy;
        state.visible_from[slot_id][k] = 0;
        state.frame_mirror[slot_id][k] = fr.mirror ? 1 : 0;
    }

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
                                          const std::vector<WorldMarkedAsmLaneInput> &asm_lanes)
{
    WorldMarkedTabsResult result = {};
    if (!state.marked_play)
        return result;

    std::vector<WorldMarkedLane> lanes;
    lanes.reserve(kWorldMarkedMaxTabs);

    bool dummy_decap_missing = false;
    WorldAppendMarkedDocumentLanes(state, active_doc_idx, lanes,
                                   &dummy_decap_missing);

    bool asm_present = false;
    for (const WorldMarkedAsmLaneInput &input : asm_lanes) {
        if (!input.enabled) continue;
        if (WorldAppendAsmLane(state, input.name, input.frames, input.doc,
                               input.doc_idx, input.slot_id, lanes))
            asm_present = true;
    }

    if (lanes.empty()) return result;
    if (lanes.size() < 2 && !asm_present) return result;
    if (!WorldUpdateMarkedLanePlayback(state, lanes, delta_time))
        return result;

    WorldMarkedSceneResult scene =
        WorldDrawMarkedScene(state, world, lanes, avail, img_pos);

    ImGui::SetCursorScreenPos(img_pos);
    ImGui::Dummy(ImVec2(avail.x, avail.y));

    result.panel =
        WorldDrawMarkedPanel(state, lanes, scene.panel_layout,
                             dummy_decap_missing, selected_img,
                             active_doc_idx);
    result.drew = true;
    return result;
}

bool WorldUpdateMarkedLanePlayback(WorldMarkedSequenceState &state,
                                   std::vector<WorldMarkedLane> &lanes,
                                   float delta_time)
{
    if (state.fps < 1.0f) state.fps = 1.0f;
    if (state.fps > 60.0f) state.fps = 60.0f;
    if (!state.paused)
        state.timer += delta_time;
    float step = 1.0f / state.fps;
    while (state.timer >= step) {
        state.timer -= step;
        state.frame++;
    }

    bool have_image = false;
    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        WorldMarkedLane &lane = lanes[slot];
        int n = (int)lane.frames.size();
        if (n <= 0) continue;
        lane.frame_pos = WorldMarkedFrameForTick(state, lane.delay_slot, n,
                                                 state.frame,
                                                 state.hold_end[lane.delay_slot]);
        Document *fdoc = (lane.frame_pos < (int)lane.frame_docs.size() &&
                          lane.frame_docs[lane.frame_pos])
                       ? lane.frame_docs[lane.frame_pos] : lane.doc;
        lane.img = doc_get_img(fdoc, lane.frames[lane.frame_pos]);
        if (lane.img) have_image = true;
    }
    return have_image;
}

static unsigned char WorldMarkedLaneAlpha(int slot)
{
    static const unsigned char kAlpha[kWorldMarkedMaxTabs] =
        {255, 185, 170, 155, 205, 255, 235};
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return 255;
    return kAlpha[slot];
}

ImU32 WorldMarkedLaneOutlineColor(int slot)
{
    static const ImU32 kOutline[kWorldMarkedMaxTabs] = {
        IM_COL32(120, 190, 255, 230),
        IM_COL32(255, 190, 90, 230),
        IM_COL32(120, 230, 150, 230),
        IM_COL32(230, 130, 230, 230),
        IM_COL32(240, 80, 80, 230),
        IM_COL32(120, 190, 255, 230),
        IM_COL32(240, 80, 80, 230)
    };
    if (slot < 0 || slot >= kWorldMarkedMaxTabs)
        return IM_COL32(220, 220, 220, 230);
    return kOutline[slot];
}

void WorldDrawMarkedLaneSprites(ImDrawList *dl, WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldCanvasLayout &layout,
                                WorldMarkedLaneRenderInfo &render_info)
{
    if (!dl) return;

    auto draw_slot = [&](int slot) {
        if (slot < 0 || slot >= (int)lanes.size()) return;
        const WorldMarkedLane &lane = lanes[slot];
        int state_slot = lane.delay_slot;
        EnsureWorldMarkedFrameDelays(state, state_slot, (int)lane.frames.size());
        if (lane.frame_pos >= 0 &&
            lane.frame_pos < (int)state.visible_from[state_slot].size() &&
            state.frame < state.visible_from[state_slot][lane.frame_pos])
            return;

        bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
        bool mirror_x = mirror_flag ? *mirror_flag : false;
        if (lane.frame_pos >= 0 &&
            lane.frame_pos < (int)state.frame_mirror[state_slot].size() &&
            state.frame_mirror[state_slot][lane.frame_pos])
            mirror_x = !mirror_x;
        render_info.lane_mirror_x[slot] = mirror_x;

        const std::vector<int> *pieces = NULL;
        const std::vector<Document*> *piece_docs = NULL;
        if (lane.frame_pos >= 0 &&
            lane.frame_pos < (int)lane.frame_pieces.size())
            pieces = &lane.frame_pieces[lane.frame_pos];
        if (lane.frame_pos >= 0 &&
            lane.frame_pos < (int)lane.frame_piece_docs.size())
            piece_docs = &lane.frame_piece_docs[lane.frame_pos];

        std::vector<int> fallback_piece;
        if (!pieces || pieces->empty()) {
            if (!lane.img) return;
            fallback_piece.push_back(lane.frames[lane.frame_pos]);
            pieces = &fallback_piece;
            piece_docs = NULL;
        }

        for (size_t pi = 0; pi < pieces->size(); pi++) {
            int piece_idx = (*pieces)[pi];
            Document *pdoc = (piece_docs && pi < piece_docs->size() && (*piece_docs)[pi])
                           ? (*piece_docs)[pi] : lane.doc;
            IMG *img = doc_get_img(pdoc, piece_idx);
            if (!img) continue;
            SDL_Texture *tex = BuildWorldSpriteTexture(pdoc, img,
                                                       WorldMarkedLaneAlpha(slot));
            if (!tex) continue;

            int ax = (int)(short)img->anix + state.local_dx[state_slot][lane.frame_pos];
            int ay = (int)(short)img->aniy + state.local_dy[state_slot][lane.frame_pos];
            float spw = img->w * layout.scale;
            float sph = img->h * layout.scale;
            float left = mirror_x
                ? (layout.origin_x - ((int)img->w - ax) * layout.scale)
                : (layout.origin_x - ax * layout.scale);
            ImVec2 spos(left, layout.origin_y - ay * layout.scale);
            ImVec2 uv0 = mirror_x ? ImVec2(1, 0) : ImVec2(0, 0);
            ImVec2 uv1 = mirror_x ? ImVec2(0, 1) : ImVec2(1, 1);
            dl->AddImage((ImTextureID)(intptr_t)tex,
                         spos, ImVec2(spos.x + spw, spos.y + sph), uv0, uv1);
            dl->AddRect(spos, ImVec2(spos.x + spw, spos.y + sph),
                        WorldMarkedLaneOutlineColor(slot), 0.0f, 0, 1.0f);

            ImVec2 rmax(spos.x + spw, spos.y + sph);
            if (!render_info.lane_rect_valid[slot]) {
                render_info.lane_rect_valid[slot] = true;
                render_info.lane_rect_min[slot] = spos;
                render_info.lane_rect_max[slot] = rmax;
            } else {
                if (spos.x < render_info.lane_rect_min[slot].x)
                    render_info.lane_rect_min[slot].x = spos.x;
                if (spos.y < render_info.lane_rect_min[slot].y)
                    render_info.lane_rect_min[slot].y = spos.y;
                if (rmax.x > render_info.lane_rect_max[slot].x)
                    render_info.lane_rect_max[slot].x = rmax.x;
                if (rmax.y > render_info.lane_rect_max[slot].y)
                    render_info.lane_rect_max[slot].y = rmax.y;
            }
        }
    };

    for (int slot = (int)lanes.size() - 1; slot >= 1; slot--)
        draw_slot(slot);
    draw_slot(0);
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
                               ImVec2 world_pos, float world_width)
{
    if (!dl) return;

    std::string label = "Marked tabs: ";
    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        const WorldMarkedLane &lane = lanes[slot];
        if (!lane.img) continue;
        const char *doc_name = !lane.label.empty()
                             ? lane.label.c_str()
                             : (lane.doc->fname_s[0] ? lane.doc->fname_s : "Untitled");
        bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
        char part[224];
        snprintf(part, sizeof(part), "%s[%d] %s:%s %d/%d%s%s",
                 slot == 0 ? "" : " + ",
                 lane.doc_idx, doc_name, img_name_string(lane.img).c_str(),
                 lane.frame_pos + 1, (int)lane.frames.size(),
                 (mirror_flag && *mirror_flag) ? " mirror" : "",
                 state.hold_end[lane.delay_slot] ? " hold" : "");
        label += part;
    }
    char fps_buf[32];
    snprintf(fps_buf, sizeof(fps_buf), "   fps=%.1f", state.fps);
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
    dl->AddLine(ImVec2(origin_x - 8, origin_y),
                ImVec2(origin_x + 8, origin_y),
                IM_COL32(120, 120, 120, 255));
    dl->AddLine(ImVec2(origin_x, origin_y - 8),
                ImVec2(origin_x, origin_y + 8),
                IM_COL32(120, 120, 120, 255));

    WorldDrawMarkedLaneSprites(dl, state, lanes, result.layout,
                               result.render_info);
    dl->AddCircle(ImVec2(origin_x, origin_y), 4.0f,
                  IM_COL32(255, 200, 0, 255), 0, 1.5f);
    WorldDrawMarkedLaneTags(dl, lanes, result.render_info, world_pos);
    WorldDrawMarkedLaneStatus(dl, state, lanes, world_pos, world_width);

    result.panel_layout =
        ComputeWorldMarkedPanelLayout(avail, img_pos, (int)lanes.size());
    WorldHandleMarkedLaneDrag(dl, state, lanes, result.render_info,
                              result.layout, result.panel_layout);
    return result;
}

WorldMarkedPanelAction WorldDrawMarkedPanelHeader(WorldMarkedSequenceState &state,
                                                  const std::vector<WorldMarkedLane> &lanes,
                                                  bool dummy_decap_missing,
                                                  IMG *selected_img,
                                                  int active_doc_idx)
{
    WorldMarkedPanelAction action = {};

    ImGui::Text("Frame Sequence");
    ImGui::SameLine();
    if (ImGui::SmallButton(state.paused ? "Play##world_marked_pause"
                                        : "Pause##world_marked_pause")) {
        state.paused = !state.paused;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh##world_marked_restart"))
        WorldMarkedRestart(state);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Restart every marked tab sequence from frame 1.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(105.0f);
    ImGui::SliderFloat("FPS##world_marked_panel_fps", &state.fps, 1.0f, 60.0f, "%.1f");
    ImGui::SameLine();
    ImGui::TextDisabled("Tick %d", state.frame);
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy ASM##world_marked_copy_asm")) {
        state.generated_asm = WorldBuildMarkedAsm(state, lanes);
        ImGui::SetClipboardText(state.generated_asm.c_str());
        action.copied_asm = true;
        action.copied_lane_count = (int)lanes.size();
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Copies one animation table per marked tab, plus aligned local-anipoint tables.");
    ImGui::SameLine();
    if (ImGui::SmallButton("View ASM##world_marked_view_asm")) {
        state.generated_asm = WorldBuildMarkedAsm(state, lanes);
        state.show_asm = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Preview the generated animation-table source.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save ASM##world_marked_save_asm")) {
        state.generated_asm = WorldBuildMarkedAsm(state, lanes);
        action.request_save_asm = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Save the generated animation tables to a .ASM file.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Load ASM##world_marked_load_asm"))
        action.request_load_asm = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load a saved/character .ASM into the ASM Animations viewer.\n"
                          "The sprite IMGs it references are opened automatically.");
    ImGui::SameLine();
    if (ImGui::Checkbox("Dummy Body##world_dummy_decap_body",
                        &state.dummy_decap_body)) {
        state.dummy_decap_reset = true;
        state.hold_end[kWorldDummyDecapSlot] = true;
        WorldMarkedRestart(state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Adds the stock fatality decap body as its own sync lane using *DECAP1-7 frames from open tabs.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Use Selected##world_dummy_assign")) {
        if (WorldAssignSelectedDummyDecap(state, selected_img, active_doc_idx))
            action.dummy_assigned = true;
        else
            action.dummy_assign_failed = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Assign the dummy body from the selected *DECAP frame, *DECAPLEG piece, or *DECAPTORSO piece.");
    if (state.dummy_decap_manual) {
        ImGui::SameLine();
        ImGui::TextDisabled("[%d] %sDECAP",
                            state.dummy_decap_doc_idx,
                            state.dummy_decap_prefix.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton("Auto##world_dummy_auto")) {
            state.dummy_decap_manual = false;
            state.dummy_decap_reset = true;
            WorldMarkedRestart(state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Return to automatic dummy body selection.");
    }
    if (state.dummy_decap_body && dummy_decap_missing) {
        ImGui::SameLine();
        ImGui::TextDisabled("No assigned *DECAP body found");
    }

    return action;
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
    if (ImGui::BeginChild("##world_marked_sequence",
                          ImVec2(layout.width, layout.height), true,
                          ImGuiWindowFlags_HorizontalScrollbar)) {
        result.header =
            WorldDrawMarkedPanelHeader(state, lanes, dummy_decap_missing,
                                       selected_img, active_doc_idx);

        for (int slot = 0; slot < (int)lanes.size(); slot++) {
            WorldMarkedLane &lane = lanes[slot];
            ImGui::PushID(slot);
            WorldDrawMarkedLaneControls(state, lane, slot);

            WorldMarkedLaneThumbClick thumb_click =
                WorldDrawMarkedLaneThumbnails(state, lane);
            if (thumb_click.clicked)
                result.thumb_click = thumb_click;
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor();

    result.copied_popup_asm = WorldDrawMarkedAsmPopup(state);
    return result;
}

void WorldDrawMarkedLaneControls(WorldMarkedSequenceState &state,
                                 WorldMarkedLane &lane,
                                 int display_slot)
{
    ImGui::Separator();
    const char *doc_name = !lane.label.empty()
                         ? lane.label.c_str()
                         : (lane.doc && lane.doc->fname_s[0] ? lane.doc->fname_s : "Untitled");
    ImGui::Text("Slot %d  [%d] %s", display_slot + 1, lane.doc_idx, doc_name);
    ImGui::SameLine();
    ImGui::Checkbox("Stop##world_lane_stop", &state.hold_end[lane.delay_slot]);
    if (lane.dummy_decap) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset Body##world_dummy_decap_reset")) {
            WorldResetDummyDecapDelays(state, (int)lane.frames.size());
            WorldMarkedRestart(state);
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restore stock decap timing: 48, 6/6, 10-tick wobble, 6-tick fall.");
    }
    bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
    if (mirror_flag) {
        ImGui::SameLine();
        ImGui::Checkbox("Mirror##world_lane_mirror", mirror_flag);
    }

    EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());
    int edit_fi = lane.frame_pos;
    if (edit_fi < 0) edit_fi = 0;
    if (edit_fi >= (int)lane.frames.size()) edit_fi = (int)lane.frames.size() - 1;

    if (edit_fi >= 0) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Entry %d/%d", edit_fi + 1, (int)lane.frames.size());
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
            ImGui::SameLine();
            if (ImGui::SmallButton("Reset Seq##world_seq_reset")) {
                WorldMarkedResetSequenceToDefaults(state, lane.delay_slot);
                WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Rebuild this lane from the currently marked sprites and clear local sequence offsets.");
        }

        int delay = state.frame_delays[lane.delay_slot][edit_fi];
        int local_dx = state.local_dx[lane.delay_slot][edit_fi];
        int local_dy = state.local_dy[lane.delay_slot][edit_fi];
        int show_at = state.visible_from[lane.delay_slot][edit_fi];
        ImGui::SameLine();
        ImGui::TextDisabled("Delay");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(38.0f);
        if (ImGui::InputInt("##world_edit_delay", &delay, 0, 0))
            state.frame_delays[lane.delay_slot][edit_fi] = ClampTimelineHold(delay);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Repeat count for this sequence entry.");
        ImGui::SameLine();
        ImGui::TextDisabled("dAX");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(46.0f);
        if (ImGui::InputInt("##world_edit_dax", &local_dx, 0, 0))
            state.local_dx[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(local_dx);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Local anipoint X delta for this entry. You can also drag the sprite in the world canvas.");
        ImGui::SameLine();
        ImGui::TextDisabled("dAY");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(46.0f);
        if (ImGui::InputInt("##world_edit_day", &local_dy, 0, 0))
            state.local_dy[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(local_dy);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Local anipoint Y delta for this entry. Positive values move the effective anipoint down.");
        ImGui::SameLine();
        ImGui::TextDisabled("Show@");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(52.0f);
        if (ImGui::InputInt("##world_edit_show", &show_at, 0, 0))
            state.visible_from[lane.delay_slot][edit_fi] = ClampWorldMarkedVisibleFrom(show_at);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide this entry until the global preview tick reaches this value.");
    }
}

WorldMarkedLaneThumbClick WorldDrawMarkedLaneThumbnails(WorldMarkedSequenceState &state,
                                                        WorldMarkedLane &lane)
{
    WorldMarkedLaneThumbClick action = {};

    ImGui::BeginChild("##world_lane_frames", ImVec2(0.0f, 42.0f), false,
                      ImGuiWindowFlags_HorizontalScrollbar |
                      ImGuiWindowFlags_NoBackground);
    for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
        if (fi > 0) ImGui::SameLine(0.0f, 6.0f);
        ImGui::PushID(fi);
        ImGui::BeginGroup();
        int img_idx = lane.frames[fi];
        Document *thumb_doc = (fi < (int)lane.frame_docs.size() && lane.frame_docs[fi])
                            ? lane.frame_docs[fi] : lane.doc;
        IMG *thumb_img = doc_get_img(thumb_doc, img_idx);
        SDL_Texture *thumb_tex = BuildWorldSpriteTexture(thumb_doc, thumb_img, 255);
        bool current = (fi == lane.frame_pos);
        if (current)
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.20f, 0.46f, 0.72f, 1.0f));
        bool clicked = false;
        if (thumb_tex) {
            clicked = ImGui::ImageButton("##world_thumb",
                                         (ImTextureID)(intptr_t)thumb_tex,
                                         ImVec2(34.0f, 34.0f),
                                         ImVec2(0, 0), ImVec2(1, 1),
                                         ImVec4(0, 0, 0, 0),
                                         ImVec4(1, 1, 1, 1));
        } else {
            char fallback[16];
            snprintf(fallback, sizeof(fallback), "%d", img_idx);
            clicked = ImGui::Button(fallback, ImVec2(34.0f, 34.0f));
        }
        if (current)
            ImGui::PopStyleColor();
        if (clicked) {
            state.paused = true;
            state.timer = 0.0f;
            state.frame = WorldMarkedTickForFrame(state, lane.delay_slot,
                                                  (int)lane.frames.size(), fi);
            lane.frame_pos = fi;
            action.clicked = true;
            action.doc_idx = lane.doc_idx;
            action.img_idx = img_idx;
        }
        if (ImGui::IsItemHovered()) {
            std::string sprite_name = (fi < (int)lane.frame_labels.size() &&
                                       !lane.frame_labels[fi].empty())
                                    ? lane.frame_labels[fi]
                                    : (thumb_img ? img_name_string(thumb_img) : std::string());
            ImGui::SetTooltip("[%d] %s", img_idx, sprite_name.c_str());
        }
        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();

    return action;
}

std::string WorldBuildMarkedAsm(WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes)
{
    std::string out;
    out.reserve(4096);
    out += "; IMGTOOL World View fatality sequence draft\n";
    out += "; One lane is one actor/object animation table.\n";
    out += "; Delay ticks are encoded by repeating that frame label.\n";
    out += "; Hidden entries export as 0 until their Show@ preview tick.\n";
    out += "; Each *_local_anipts table is aligned 1:1 with the .long rows.\n";
    out += "; Run these lanes at the same animation sleep/FPS used in the preview.\n\n";

    for (int slot = 0; slot < (int)lanes.size(); slot++) {
        const WorldMarkedLane &lane = lanes[slot];
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
        if (lane.dummy_decap)
            out += "; Stock decap body timing: stand, fall-to-knees, wobble, fall-to-ground.\n";

        bool *mirror_flag = WorldMarkedMirrorFlag(state, lane.delay_slot);
        if (mirror_flag && *mirror_flag)
            out += "; Preview mirror is enabled; spawn/draw this object mirrored in routine code.\n";

        out += anim_label;
        out += "\n";
        EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());
        std::string local_table;
        local_table += anim_label;
        local_table += "_local_anipts\n";
        int tick = 0;
        for (int fi = 0; fi < (int)lane.frames.size(); fi++) {
            IMG *frame_img = doc_get_img(lane.doc, lane.frames[fi]);
            char fallback[32];
            snprintf(fallback, sizeof(fallback), "slot%d_frame%d", slot + 1, fi + 1);
            std::string raw_label = (fi < (int)lane.frame_labels.size() &&
                                     !lane.frame_labels[fi].empty())
                                  ? lane.frame_labels[fi]
                                  : img_name_string(frame_img);
            std::string sprite = WorldMarkedAsmToken(raw_label, fallback);
            int delay = ClampTimelineHold(state.frame_delays[lane.delay_slot][fi]);
            int local_dx = state.local_dx[lane.delay_slot][fi];
            int local_dy = state.local_dy[lane.delay_slot][fi];
            int visible_from = state.visible_from[lane.delay_slot][fi];
            for (int repeat = 0; repeat < delay; repeat++) {
                bool hidden = tick < visible_from;
                out += "\t.long\t";
                out += hidden ? "0" : sprite;
                if (repeat == 0) {
                    out += "\t; ";
                    out += sprite;
                    out += " delay x";
                    out += std::to_string(delay);
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
                } else if (hidden) {
                    out += "\t; hidden";
                }
                out += "\n";

                local_table += "\t.word\t";
                local_table += std::to_string(local_dx);
                local_table += ",";
                local_table += std::to_string(local_dy);
                local_table += "\t; tick ";
                local_table += std::to_string(tick);
                local_table += hidden ? " hidden " : " ";
                local_table += sprite;
                local_table += "\n";
                tick++;
            }
        }
        if (state.hold_end[lane.delay_slot]) {
            out += "\t.long\t0\t; stop on final frame\n\n";
        } else {
            out += "\t.long\tani_jump,";
            out += anim_label;
            out += "\t; loop\n\n";
        }
        out += local_table;
        out += "\n";
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
        ImGui::TextDisabled("Generated from the current marked-tab World View sequence.");
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
                               const WorldMarkedPanelLayout &panel_layout)
{
    ImVec2 mouse = ImGui::GetMousePos();
    bool over_panel =
        mouse.x >= panel_layout.pos.x &&
        mouse.x <= panel_layout.pos.x + panel_layout.width &&
        mouse.y >= panel_layout.pos.y &&
        mouse.y <= panel_layout.pos.y + panel_layout.height;
    bool over_world =
        mouse.x >= world_layout.pos.x &&
        mouse.x <= world_layout.pos.x + world_layout.width &&
        mouse.y >= world_layout.pos.y &&
        mouse.y <= world_layout.pos.y + world_layout.height;

    int hover_slot = -1;
    if (over_world && !over_panel) {
        for (int slot = 0; slot < (int)lanes.size(); slot++) {
            if (!render_info.lane_rect_valid[slot]) continue;
            if (mouse.x >= render_info.lane_rect_min[slot].x &&
                mouse.x <= render_info.lane_rect_max[slot].x &&
                mouse.y >= render_info.lane_rect_min[slot].y &&
                mouse.y <= render_info.lane_rect_max[slot].y) {
                hover_slot = slot;
                break;
            }
        }
    }

    if (hover_slot >= 0) {
        if (dl) {
            dl->AddRect(render_info.lane_rect_min[hover_slot],
                        render_info.lane_rect_max[hover_slot],
                        IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f);
        }
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if (hover_slot >= 0 && ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const WorldMarkedLane &lane = lanes[hover_slot];
        int state_slot = lane.delay_slot;
        EnsureWorldMarkedFrameDelays(state, state_slot, (int)lane.frames.size());
        if (lane.frame_pos >= 0 && lane.frame_pos < (int)lane.frames.size()) {
            state.paused = true;
            state.drag_slot = state_slot;
            state.drag_frame = lane.frame_pos;
            state.drag_mouse = mouse;
            state.drag_dx = state.local_dx[state_slot][lane.frame_pos];
            state.drag_dy = state.local_dy[state_slot][lane.frame_pos];
            state.drag_mirror = render_info.lane_mirror_x[hover_slot];
        }
    }

    if (state.drag_slot >= 0) {
        int state_slot = state.drag_slot;
        int frame_idx = state.drag_frame;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
            state_slot < 0 || state_slot >= kWorldMarkedMaxTabs ||
            frame_idx < 0 ||
            frame_idx >= (int)state.local_dx[state_slot].size()) {
            state.drag_slot = -1;
            state.drag_frame = -1;
            state.drag_mirror = false;
        } else {
            int px = (int)((mouse.x - state.drag_mouse.x) / world_layout.scale);
            int py = (int)((mouse.y - state.drag_mouse.y) / world_layout.scale);
            state.local_dx[state_slot][frame_idx] =
                ClampWorldMarkedAniptDelta(state.drag_dx +
                                           (state.drag_mirror ? px : -px));
            state.local_dy[state_slot][frame_idx] =
                ClampWorldMarkedAniptDelta(state.drag_dy - py);
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

void WorldMarkedRestart(WorldMarkedSequenceState &state)
{
    state.timer = 0.0f;
    state.frame = 0;
}

void StepWorldMarkedSequence(WorldMarkedSequenceState &state, int delta)
{
    state.paused = true;
    state.timer = 0.0f;
    state.frame += delta;
    if (state.frame < 0)
        state.frame = 0;
}

void EnsureWorldMarkedFrameDelays(WorldMarkedSequenceState &state, int slot, int frame_count)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    if (frame_count < 0) frame_count = 0;

    std::vector<int> &delays = state.frame_delays[slot];
    if ((int)delays.size() < frame_count)
        delays.resize((size_t)frame_count, 1);
    else if ((int)delays.size() > frame_count)
        delays.resize((size_t)frame_count);
    for (int &delay : delays)
        delay = ClampTimelineHold(delay);

    std::vector<int> &local_dx = state.local_dx[slot];
    std::vector<int> &local_dy = state.local_dy[slot];
    std::vector<int> &visible_from = state.visible_from[slot];
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
    for (int &dx : local_dx)
        dx = ClampWorldMarkedAniptDelta(dx);
    for (int &dy : local_dy)
        dy = ClampWorldMarkedAniptDelta(dy);
    for (int &show_tick : visible_from)
        show_tick = ClampWorldMarkedVisibleFrom(show_tick);

    std::vector<int> &fmir = state.frame_mirror[slot];
    if ((int)fmir.size() < frame_count)
        fmir.resize((size_t)frame_count, 0);
    else if ((int)fmir.size() > frame_count)
        fmir.resize((size_t)frame_count);
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

void WorldMarkedClearSequenceState(WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.frame_delays[slot].clear();
    state.local_dx[slot].clear();
    state.local_dy[slot].clear();
    state.visible_from[slot].clear();
    state.frame_mirror[slot].clear();
}

void WorldMarkedBuildSingleFrameLane(Document *doc, const std::vector<int> &frames,
                                     std::vector<std::vector<int>> &frame_pieces,
                                     std::vector<std::string> &frame_labels)
{
    frame_pieces.clear();
    frame_labels.clear();
    frame_pieces.reserve(frames.size());
    frame_labels.reserve(frames.size());
    for (int idx : frames) {
        frame_pieces.push_back(std::vector<int>(1, idx));
        frame_labels.push_back(img_name_string(doc_get_img(doc, idx)));
    }
}

void WorldRefreshMarkedLaneAfterSequenceEdit(WorldMarkedSequenceState &state,
                                             WorldMarkedLane &lane,
                                             int &edit_frame)
{
    if (!lane.dummy_decap) {
        lane.frames = state.sequence_frames[lane.delay_slot];
        WorldMarkedBuildSingleFrameLane(lane.doc, lane.frames,
                                        lane.frame_pieces, lane.frame_labels);
    }

    EnsureWorldMarkedFrameDelays(state, lane.delay_slot, (int)lane.frames.size());
    lane.frame_pos = WorldMarkedFrameForTick(state, lane.delay_slot,
                                             (int)lane.frames.size(),
                                             state.frame,
                                             state.hold_end[lane.delay_slot]);
    if (lane.frame_pos < 0) lane.frame_pos = 0;
    if (lane.frame_pos >= (int)lane.frames.size())
        lane.frame_pos = (int)lane.frames.size() - 1;
    lane.img = (lane.frame_pos >= 0 && lane.frame_pos < (int)lane.frames.size())
             ? doc_get_img(lane.doc, lane.frames[lane.frame_pos])
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
    bool doc_changed = state.sequence_doc[slot] != doc ||
                       state.sequence_doc_idx[slot] != doc_idx;
    bool have_seq = !state.sequence_frames[slot].empty();

    /* Does the persisted sequence still reference only frames the doc has? */
    bool stale_entry = false;
    if (!doc_changed && have_seq) {
        for (int idx : state.sequence_frames[slot]) {
            if (!doc_get_img(doc, idx)) { stale_entry = true; break; }
        }
    }
    bool defaults_changed = state.default_frames[slot] != defaults;

    if (doc_changed || !have_seq) {
        /* A different sprite/tab now occupies this slot (or there is nothing
           built yet): seed the sequence straight from the marked frames. */
        state.sequence_doc[slot] = doc;
        state.sequence_doc_idx[slot] = doc_idx;
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
        std::vector<int> old_seq   = state.sequence_frames[slot];
        std::vector<int> old_delay = state.frame_delays[slot];
        std::vector<int> old_dx    = state.local_dx[slot];
        std::vector<int> old_dy    = state.local_dy[slot];
        std::vector<int> old_vis   = state.visible_from[slot];
        std::vector<int> old_mir   = state.frame_mirror[slot];

        std::vector<int> new_seq, new_delay, new_dx, new_dy, new_vis, new_mir;
        new_seq.reserve(old_seq.size() + defaults.size());
        for (size_t i = 0; i < old_seq.size(); i++) {
            int idx = old_seq[i];
            if (!doc_get_img(doc, idx)) continue;   /* frame deleted from doc */
            if (std::find(defaults.begin(), defaults.end(), idx) == defaults.end())
                continue;                            /* sprite was unmarked */
            new_seq.push_back(idx);
            new_delay.push_back(old_delay[i]);
            new_dx.push_back(old_dx[i]);
            new_dy.push_back(old_dy[i]);
            new_vis.push_back(old_vis[i]);
            new_mir.push_back(old_mir[i]);
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
            new_delay.push_back(1);
            new_dx.push_back(0);
            new_dy.push_back(0);
            new_vis.push_back(0);
            new_mir.push_back(0);
        }

        state.default_frames[slot] = defaults;
        state.sequence_frames[slot] = new_seq;
        state.frame_delays[slot]    = new_delay;
        state.local_dx[slot]        = new_dx;
        state.local_dy[slot]        = new_dy;
        state.visible_from[slot]    = new_vis;
        state.frame_mirror[slot]    = new_mir;
        EnsureWorldMarkedFrameDelays(state, slot, (int)new_seq.size());
    }

    frames = state.sequence_frames[slot];
    WorldMarkedBuildSingleFrameLane(doc, frames, frame_pieces, frame_labels);
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());
}

void WorldMarkedResetSequenceToDefaults(WorldMarkedSequenceState &state, int slot)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    state.sequence_frames[slot] = state.default_frames[slot];
    WorldMarkedClearSequenceState(state, slot);
    EnsureWorldMarkedFrameDelays(state, slot, (int)state.sequence_frames[slot].size());
    WorldMarkedRestart(state);
}

void WorldMarkedDuplicateSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if (frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    int insert_at = frame_idx + 1;
    frames.insert(frames.begin() + insert_at, frames[frame_idx]);
    state.frame_delays[slot].insert(state.frame_delays[slot].begin() + insert_at,
                                    state.frame_delays[slot][frame_idx]);
    state.local_dx[slot].insert(state.local_dx[slot].begin() + insert_at,
                                state.local_dx[slot][frame_idx]);
    state.local_dy[slot].insert(state.local_dy[slot].begin() + insert_at,
                                state.local_dy[slot][frame_idx]);
    state.visible_from[slot].insert(state.visible_from[slot].begin() + insert_at,
                                    state.visible_from[slot][frame_idx]);
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), insert_at);
}

void WorldMarkedMoveSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx, int dir)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    int n = (int)frames.size();
    int j = frame_idx + dir;
    if (frame_idx < 0 || frame_idx >= n || j < 0 || j >= n) return;
    EnsureWorldMarkedFrameDelays(state, slot, n);

    std::swap(frames[frame_idx], frames[j]);
    std::swap(state.frame_delays[slot][frame_idx], state.frame_delays[slot][j]);
    std::swap(state.local_dx[slot][frame_idx],     state.local_dx[slot][j]);
    std::swap(state.local_dy[slot][frame_idx],     state.local_dy[slot][j]);
    std::swap(state.visible_from[slot][frame_idx], state.visible_from[slot][j]);

    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, n, j);
}

void WorldMarkedDeleteSequenceEntry(WorldMarkedSequenceState &state, int slot, int frame_idx)
{
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if ((int)frames.size() <= 1 || frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    frames.erase(frames.begin() + frame_idx);
    state.frame_delays[slot].erase(state.frame_delays[slot].begin() + frame_idx);
    state.local_dx[slot].erase(state.local_dx[slot].begin() + frame_idx);
    state.local_dy[slot].erase(state.local_dy[slot].begin() + frame_idx);
    state.visible_from[slot].erase(state.visible_from[slot].begin() + frame_idx);
    if (frame_idx >= (int)frames.size())
        frame_idx = (int)frames.size() - 1;
    state.paused = true;
    state.timer = 0.0f;
    state.frame = WorldMarkedTickForFrame(state, slot, (int)frames.size(), frame_idx);
}

static void rebuild_world_onion_texture(IMG *img, int image_idx)
{
    if (!img || !img->data_p || img->w <= 0 || img->h <= 0 || !g_imgui_renderer)
        return;

    if (s_world_onion_tex &&
        s_world_onion_tex_w == img->w &&
        s_world_onion_tex_h == img->h &&
        s_world_onion_idx == image_idx)
        return;

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
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char ci = src[y * stride + x];
            SDL_Color c = g_palette[ci];
            Uint32 a = (ci == 0) ? 0u : 90u;  /* faint */
            dst[y * (pitch / 4) + x] =
                (a << 24) | ((Uint32)c.r << 16) |
                ((Uint32)c.g << 8) | c.b;
        }
    }
    SDL_UnlockTexture(s_world_onion_tex);
}

bool DrawWorldViewSingleSprite(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io,
                               IMG *img, SDL_Texture *img_texture,
                               int image_idx, int image_count,
                               int world_w, int world_h,
                               int world_origin_x, int world_origin_y,
                               bool onion_enabled, bool mirror_active)
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

    float ox = layout.origin_x;
    float oy = layout.origin_y;
    dl->AddLine(ImVec2(ox - 8, oy), ImVec2(ox + 8, oy),
                IM_COL32(120, 120, 120, 255));
    dl->AddLine(ImVec2(ox, oy - 8), ImVec2(ox, oy + 8),
                IM_COL32(120, 120, 120, 255));

    /* Onion-skin: faintly draw the previous sprite. */
    if (onion_enabled && image_count > 1) {
        int prev_idx = (image_idx <= 0) ? image_count - 1 : image_idx - 1;
        IMG *prev_img = get_img(prev_idx);
        if (prev_img && prev_img->data_p && prev_img->w > 0 && prev_img->h > 0) {
            rebuild_world_onion_texture(prev_img, prev_idx);
            if (s_world_onion_tex) {
                float pw = prev_img->w * wscale;
                float ph = prev_img->h * wscale;
                int pax = (int)(short)prev_img->anix;
                float pleft = mirror_active
                    ? (ox - ((int)prev_img->w - pax) * wscale)
                    : (ox - pax * wscale);
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
    float sleft = mirror_active
        ? (ox - ((int)img->w - ax) * wscale)
        : (ox - ax * wscale);
    ImVec2 spos(sleft, oy - ay * wscale);
    ImVec2 suv0 = mirror_active ? ImVec2(1, 0) : ImVec2(0, 0);
    ImVec2 suv1 = mirror_active ? ImVec2(0, 1) : ImVec2(1, 1);

    dl->AddImage((ImTextureID)(intptr_t)img_texture,
                 spos, ImVec2(spos.x + spw, spos.y + sph), suv0, suv1);

    dl->AddCircle(ImVec2(ox, oy), 4.0f,
                  IM_COL32(255, 200, 0, 255), 0, 1.5f);

    if (!io.WantCaptureMouse) {
        bool over_world =
            io.MousePos.x >= wpos.x && io.MousePos.x < wpos.x + ww &&
            io.MousePos.y >= wpos.y && io.MousePos.y < wpos.y + wh;
        if (over_world &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
        {
            ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            int dx = (int)(d.x / wscale);
            int dy = (int)(d.y / wscale);
            if (dx != 0 || dy != 0) {
                int next_ax = (int)(short)img->anix + (mirror_active ? dx : -dx);
                int next_ay = (int)(short)img->aniy - dy;
                set_primary_anipoint_with_sequence(img, next_ax, next_ay);
            }
        }
    }

    char buf[96];
    snprintf(buf, sizeof(buf),
             "[%d] %s%s   anix=%d aniy=%d   world=%dx%d",
             image_idx, img->n_s,
             mirror_active ? " mirror" : "",
             ax, ay, world_w, world_h);
    dl->AddRectFilled(ImVec2(wpos.x, wpos.y),
                      ImVec2(wpos.x + 320, wpos.y + 18),
                      IM_COL32(0, 0, 0, 180));
    dl->AddText(ImVec2(wpos.x + 4, wpos.y + 2),
                IM_COL32(220, 220, 220, 255), buf);

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
        if (g_world_state.enabled) {
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
                                          g_world_state.onion, g_world_marked_state.mirror_active);
            }
        }
        else if ((timeline_composite_preview_active = DrawTimelineCompositePreview(avail, img_pos))) {
            /* Composite preview is read-only: the canvas is showing two
               timeline frames in shared anipoint space, not one editable IMG. */
        }
        else if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            if (g_zoom_reset) ResetZoomToFit();

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
            DrawCanvasCheckerboard(dl, img_pos, img_sz, scale);
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

            ImGui::Image((ImTextureID)(intptr_t)g_img_texture, img_sz);

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

            DrawCanvasZoomIndicator(g_zoom_fit, g_zoom);
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
        bool canvas_input_blocked = io.WantCaptureMouse && !ImGui::IsWindowHovered();
        if (canvas_input_blocked) mbdn = false;

        /* Set when an overlay widget (anim point, hitbox corner) eats this frame's
           click, so the grid-selection block below doesn't also start a selection. */
        bool widget_consumed_click = false;
        bool blank_marquee_click = false;

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

                    /* Right-click: eyedropper (works in any tool mode) */
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        g_sel_color = *pix;
                        widget_consumed_click = true;
                    }
                    /* Eyedropper tool active: left-click also picks color.
                       Consumes the click so the pencil branch below is skipped. */
                    if (g_active_tool == ActiveTool::Eyedropper &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
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
                    if (!g_pasted.active && !over_anipoint && !g_anipoint_drag1 && !g_anipoint_drag2 &&
                        g_hitbox_drag_corner < 0 && g_active_tool == ActiveTool::None &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && *pix == 0 &&
                        !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
                        blank_marquee_click = true;
                        g_active_tool = ActiveTool::Marquee;
                    }
                    if (!blank_marquee_click && !g_pasted.active && !over_anipoint && !g_anipoint_drag1 && !g_anipoint_drag2 && g_hitbox_drag_corner < 0
                        && (g_active_tool == ActiveTool::None || g_active_tool == ActiveTool::Pencil || g_active_tool == ActiveTool::PaintBucket || g_active_tool == ActiveTool::VariantPaint || g_active_tool == ActiveTool::BackgroundEraser || g_active_tool == ActiveTool::CloneStamp || g_active_tool == ActiveTool::SmartRemap)) {
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
                        if (g_active_tool == ActiveTool::CloneStamp) {
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

        /* --- Hitbox overlay + corner dragging ---
           Suppressed when the MK2 strike-table overlay is showing a move,
           so the two hitbox systems don't pile on top of each other. */
        bool mk2_overlay_active = g_show_mk2 && Mk2CurrentRecord() >= 0;
        if (g_show_hitbox && !canvas_input_blocked && !mk2_overlay_active && !g_world_state.enabled && !timeline_composite_preview_active) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            bool hovering[4] = {false, false, false, false};
            DrawCanvasHitboxOverlay(dl, img_pos, sx, sy,
                                    g_hitbox_x, g_hitbox_y,
                                    g_hitbox_w, g_hitbox_h,
                                    mouse, hovering);
            for (int c = 0; c < 4; c++) {
                if (hovering[c] && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_hitbox_drag_corner = c;
                    undo_push();
                    widget_consumed_click = true;
                }
            }
            if (g_hitbox_drag_corner >= 0 && mbdn) {
                int mx = (int)((mouse.x - img_pos.x) / sx);
                int my = (int)((mouse.y - img_pos.y) / sy);
                CanvasResizeRectFromCorner(g_hitbox_drag_corner, mx, my,
                                           &g_hitbox_x, &g_hitbox_y,
                                           &g_hitbox_w, &g_hitbox_h);
            } else if (!mbdn && g_hitbox_drag_corner >= 0) {
                undo_push();
                g_hitbox_drag_corner = -1;
            }
        }

        /* --- MK2 strike-table overlay (separate from IMG hitbox) ---
           Draws the currently-selected MKSTK.ASM move's collision box on
           the sprite, with corner handles for drag-to-resize. Magenta to
           distinguish from the cyan IMG-hitbox overlay.
           Drawing always runs whenever a move is selected — the editor
           panel can hold focus (which sets canvas_input_blocked) without
           hiding the box. Only the corner-drag interaction is gated. */
        int mk2_rec = (g_show_mk2 && !g_world_state.enabled && !timeline_composite_preview_active) ? Mk2CurrentRecord() : -1;
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
                        if (!paste_preview_rgba(ci, dst_ci, spal, paste_pal_map,
                                                paste_remap,
                                                cell.target_x, cell.target_y,
                                                &rr, &gg, &bb, &aa))
                            continue;
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
                ImGui::TextUnformatted("Blend");
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
