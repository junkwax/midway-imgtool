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

#include "anipoint.h"       /* secondary_anipoint_in_use */
#include "anipoint_edit.h"  /* set_primary_anipoint_with_sequence */
#include "img_format.h"     /* get_img */
#include "img_util.h"       /* img_name_string */
#include "shim_vid.h"       /* g_palette */
#include "ui_timeline.h"    /* ClampTimelineHold */
#include "world_render.h"   /* doc_get_img */
#include "sprite_resize_ops.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>

static SDL_Texture *s_world_onion_tex = NULL;
static int s_world_onion_tex_w = 0;
static int s_world_onion_tex_h = 0;
static int s_world_onion_idx = -1;

static int  g_selection_add_mask_w = 0;
static int  g_selection_add_mask_h = 0;
static std::vector<bool> g_selection_add_mask;


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
    if (layout.scale < 2.0f) layout.scale = 2.0f;
    layout.scale = (float)(int)layout.scale;
    if (layout.scale < 2.0f) layout.scale = 2.0f;

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
    state.visible_until[kWorldDummyDecapSlot].assign((size_t)frame_count, 0);
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
        if (state.sequence_doc[slot] == doc && state.sequence_doc_idx[slot] == doc_idx)
            return slot;
    }
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (used_source_slots[slot] || WorldMarkedSlotReservedForSplit(state, slot))
            continue;
        if (state.sequence_doc_idx[slot] < 0 || state.sequence_frames[slot].empty())
            return slot;
    }
    for (int slot = 0; slot < kWorldMarkedSourceTabs; slot++) {
        if (!used_source_slots[slot] && !WorldMarkedSlotReservedForSplit(state, slot))
            return slot;
    }
    return -1;
}

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

    WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                    lane.frame_pieces, lane.frame_labels);
    WorldMarkedSyncSequenceOverride(state, source_slot, doc, doc_idx,
                                    lane.frames, lane.frame_pieces,
                                    lane.frame_labels);
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

        WorldMarkedBuildSingleFrameLane(doc, lane.frames,
                                        lane.frame_pieces, lane.frame_labels);
        EnsureWorldMarkedFrameDelays(state, split.slot, (int)lane.frames.size());
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
        state.visible_until[slot_id][k] = 0;
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

void WorldDrawMarkedLaneSprites(ImDrawList *dl, WorldMarkedSequenceState &state,
                                const std::vector<WorldMarkedLane> &lanes,
                                const WorldCanvasLayout &layout,
                                WorldMarkedLaneRenderInfo &render_info)
{
    if (!dl) return;

    auto draw_instance = [&](int slot, int frame_idx, bool dual, bool mirror_x) {
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

        int local_dx = dual ? state.dual_dx[state_slot][frame_idx]
                            : state.local_dx[state_slot][frame_idx];
        int local_dy = dual ? state.dual_dy[state_slot][frame_idx]
                            : state.local_dy[state_slot][frame_idx];
        bool *rect_valid = dual ? &render_info.dual_rect_valid[slot]
                                : &render_info.lane_rect_valid[slot];
        ImVec2 *rect_min = dual ? &render_info.dual_rect_min[slot]
                                : &render_info.lane_rect_min[slot];
        ImVec2 *rect_max = dual ? &render_info.dual_rect_max[slot]
                                : &render_info.lane_rect_max[slot];

        for (size_t pi = 0; pi < pieces->size(); pi++) {
            int piece_idx = (*pieces)[pi];
            Document *pdoc = (piece_docs && pi < piece_docs->size() && (*piece_docs)[pi])
                           ? (*piece_docs)[pi] : lane.doc;
            IMG *img = doc_get_img(pdoc, piece_idx);
            if (!img) continue;
            SDL_Texture *tex = BuildWorldSpriteTexture(pdoc, img,
                                                       WorldMarkedLaneAlpha(slot));
            if (!tex) continue;

            int ax = (int)(short)img->anix + local_dx;
            int ay = (int)(short)img->aniy + local_dy;
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

    /* Collect one draw job per visible instance (lane sprite + optional dual
       copy), then paint back-to-front by per-entry Z. Equal Z keeps the legacy
       order: slot N-1 painted first (back), slot 0 last (top); a dual copy
       paints right after its primary. */
    struct WorldLaneDrawJob {
        int slot;
        int frame_idx;
        int z;
        int order;
        bool dual;
        bool mirror_x;
    };
    int n = (int)lanes.size();
    std::vector<WorldLaneDrawJob> jobs;
    jobs.reserve((size_t)n * 2);
    auto add_frame_jobs = [&](int slot, int frame_idx, int order,
                              bool mirror_x) {
        const WorldMarkedLane &lane = lanes[slot];
        int state_slot = lane.delay_slot;
        jobs.push_back({slot, frame_idx,
                        state.frame_z[state_slot][frame_idx],
                        order, false, mirror_x});
        if (state.dual_on[state_slot][frame_idx])
            jobs.push_back({slot, frame_idx,
                            state.dual_z[state_slot][frame_idx],
                            order, true, mirror_x});
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
                !WorldMarkedEntryVisibleAtTick(state, state_slot, fi, state.frame))
                continue;
            bool mirror_x = base_mirror_x;
            if (fi < (int)state.frame_mirror[state_slot].size() &&
                state.frame_mirror[state_slot][fi])
                mirror_x = !mirror_x;
            add_frame_jobs(slot, fi, order, mirror_x);
        }

        bool current_mirror_x = base_mirror_x;
        if (lane.frame_pos < (int)state.frame_mirror[state_slot].size() &&
            state.frame_mirror[state_slot][lane.frame_pos])
            current_mirror_x = !current_mirror_x;
        render_info.lane_mirror_x[slot] = current_mirror_x;
        if (WorldMarkedEntryVisibleAtTick(state, state_slot, lane.frame_pos,
                                          state.frame))
            add_frame_jobs(slot, lane.frame_pos, order, current_mirror_x);
    }
    std::stable_sort(jobs.begin(), jobs.end(),
                     [](const WorldLaneDrawJob &a, const WorldLaneDrawJob &b) {
                         if (a.z != b.z) return a.z < b.z;
                         return a.order < b.order;
                     });
    for (const WorldLaneDrawJob &job : jobs)
        draw_instance(job.slot, job.frame_idx, job.dual, job.mirror_x);
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

    std::string label = "Marked rows: ";
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
    ImGui::Checkbox("Borders##world_marked_borders", &state.draw_sprite_borders);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Draw colored sprite bounds in World View.");
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
    if (!state.split_lanes.empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear Splits##world_marked_clear_splits"))
            WorldMarkedClearSplitLanes(state);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Remove split rows and restore each source row to its marked-frame sequence.");
    }
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
            WorldDrawMarkedLaneControls(state, lane, lanes, slot);

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
                                 const std::vector<WorldMarkedLane> &lanes,
                                 int display_slot)
{
    ImGui::Separator();
    const char *doc_name = !lane.label.empty()
                         ? lane.label.c_str()
                         : (lane.doc && lane.doc->fname_s[0] ? lane.doc->fname_s : "Untitled");
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
    ImGui::SameLine();
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
            ImGui::SameLine();
            bool can_split = lane.delay_slot >= 0 &&
                             lane.delay_slot < kWorldMarkedSourceTabs &&
                             edit_fi > 0 &&
                             WorldMarkedFindFreeSplitSlot(lanes) >= 0;
            ImGui::BeginDisabled(!can_split);
            if (ImGui::SmallButton("Split Row##world_seq_split")) {
                if (WorldMarkedSplitLaneAtFrame(state, lane, lanes, edit_fi))
                    WorldRefreshMarkedLaneAfterSequenceEdit(state, lane, edit_fi);
            }
            ImGui::EndDisabled();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move this entry and all later entries into a new row from the same IMG.");
        }

        int delay = state.frame_delays[lane.delay_slot][edit_fi];
        int local_dx = state.local_dx[lane.delay_slot][edit_fi];
        int local_dy = state.local_dy[lane.delay_slot][edit_fi];
        int show_at = state.visible_from[lane.delay_slot][edit_fi];
        int hide_at = state.visible_until[lane.delay_slot][edit_fi];
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
        ImGui::SameLine();
        ImGui::TextDisabled("Hide@");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(52.0f);
        if (ImGui::InputInt("##world_edit_hide", &hide_at, 0, 0))
            state.visible_until[lane.delay_slot][edit_fi] = ClampWorldMarkedVisibleUntil(hide_at);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Hide this entry once the global preview tick reaches this value. 0 disables the cutoff.");

        int frame_z = state.frame_z[lane.delay_slot][edit_fi];
        ImGui::SameLine();
        ImGui::TextDisabled("Z");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(38.0f);
        if (ImGui::InputInt("##world_edit_z", &frame_z, 0, 0))
            state.frame_z[lane.delay_slot][edit_fi] = ClampWorldMarkedZ(frame_z);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw priority for this entry: higher Z draws on top of other lanes.\n"
                              "Equal Z keeps the normal lane order (slot 1 on top).");

        bool dual = state.dual_on[lane.delay_slot][edit_fi] != 0;
        ImGui::SameLine();
        if (ImGui::Checkbox("Dual##world_edit_dual", &dual))
            state.dual_on[lane.delay_slot][edit_fi] = dual ? 1 : 0;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw this entry's sprite a second time in the same frame,\n"
                              "at its own local offset and Z. Drag either copy in the world canvas.");
        if (dual) {
            int dual_dx = state.dual_dx[lane.delay_slot][edit_fi];
            int dual_dy = state.dual_dy[lane.delay_slot][edit_fi];
            int dual_z = state.dual_z[lane.delay_slot][edit_fi];
            ImGui::SameLine();
            ImGui::TextDisabled("dAX2");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(46.0f);
            if (ImGui::InputInt("##world_edit_dax2", &dual_dx, 0, 0))
                state.dual_dx[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(dual_dx);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Local anipoint X delta for the second copy of this sprite.");
            ImGui::SameLine();
            ImGui::TextDisabled("dAY2");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(46.0f);
            if (ImGui::InputInt("##world_edit_day2", &dual_dy, 0, 0))
                state.dual_dy[lane.delay_slot][edit_fi] = ClampWorldMarkedAniptDelta(dual_dy);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Local anipoint Y delta for the second copy of this sprite.");
            ImGui::SameLine();
            ImGui::TextDisabled("Z2");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(38.0f);
            if (ImGui::InputInt("##world_edit_z2", &dual_z, 0, 0))
                state.dual_z[lane.delay_slot][edit_fi] = ClampWorldMarkedZ(dual_z);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draw priority for the second copy. Lower than Z puts it behind\n"
                                  "the first copy (and behind other lanes it sorts under).");
        }
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
    out += "; Show@/Hide@ entries act as timed held subframes in preview;\n";
    out += "; export emits 0 outside that tick window.\n";
    out += "; Each *_local_anipts table is aligned 1:1 with the .long rows.\n";
    out += "; Entries with z= / dual annotations need routine code: z orders the\n";
    out += "; object's draw priority, dual draws the same sprite a second time.\n";
    out += "; Lanes with dual entries also emit a *_dual_anipts table aligned\n";
    out += "; 1:1 with the rows; -32768,-32768 means no second copy that tick.\n";
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
        bool lane_has_dual = false;
        for (int fi = 0; fi < (int)lane.frames.size(); fi++)
            if (state.dual_on[lane.delay_slot][fi]) { lane_has_dual = true; break; }
        std::string dual_table;
        if (lane_has_dual) {
            dual_table += anim_label;
            dual_table += "_dual_anipts\n";
        }
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
            int visible_until = state.visible_until[lane.delay_slot][fi];
            int frame_z = state.frame_z[lane.delay_slot][fi];
            bool dual = state.dual_on[lane.delay_slot][fi] != 0;
            int dual_dx = state.dual_dx[lane.delay_slot][fi];
            int dual_dy = state.dual_dy[lane.delay_slot][fi];
            int dual_z = state.dual_z[lane.delay_slot][fi];
            for (int repeat = 0; repeat < delay; repeat++) {
                bool hidden = tick < visible_from ||
                              (visible_until > 0 && tick >= visible_until);
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
                    if (visible_until > 0) {
                        out += " hide>=";
                        out += std::to_string(visible_until);
                    }
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

                local_table += "\t.word\t";
                local_table += std::to_string(local_dx);
                local_table += ",";
                local_table += std::to_string(local_dy);
                local_table += "\t; tick ";
                local_table += std::to_string(tick);
                local_table += hidden ? " hidden " : " ";
                local_table += sprite;
                local_table += "\n";

                if (lane_has_dual) {
                    dual_table += "\t.word\t";
                    if (dual && !hidden) {
                        dual_table += std::to_string(dual_dx);
                        dual_table += ",";
                        dual_table += std::to_string(dual_dy);
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
        if (lane_has_dual) {
            out += dual_table;
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
    bool hover_dual = false;
    if (over_world && !over_panel) {
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

    if (hover_slot >= 0 && ImGui::IsWindowHovered() &&
        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const WorldMarkedLane &lane = lanes[hover_slot];
        int state_slot = lane.delay_slot;
        EnsureWorldMarkedFrameDelays(state, state_slot, (int)lane.frames.size());
        if (lane.frame_pos >= 0 && lane.frame_pos < (int)lane.frames.size()) {
            state.paused = true;
            state.drag_slot = state_slot;
            state.drag_frame = lane.frame_pos;
            state.drag_dual = hover_dual;
            state.drag_mouse = mouse;
            state.drag_dx = hover_dual ? state.dual_dx[state_slot][lane.frame_pos]
                                       : state.local_dx[state_slot][lane.frame_pos];
            state.drag_dy = hover_dual ? state.dual_dy[state_slot][lane.frame_pos]
                                       : state.local_dy[state_slot][lane.frame_pos];
            state.drag_mirror = render_info.lane_mirror_x[hover_slot];
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
            state.drag_mirror = false;
        } else {
            int px = (int)((mouse.x - state.drag_mouse.x) / world_layout.scale);
            int py = (int)((mouse.y - state.drag_mouse.y) / world_layout.scale);
            dst_dx[state_slot][frame_idx] =
                ClampWorldMarkedAniptDelta(state.drag_dx +
                                           (state.drag_mirror ? px : -px));
            dst_dy[state_slot][frame_idx] =
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
    std::vector<int> &visible_until = state.visible_until[slot];
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
    for (int &dx : local_dx)
        dx = ClampWorldMarkedAniptDelta(dx);
    for (int &dy : local_dy)
        dy = ClampWorldMarkedAniptDelta(dy);
    for (int &show_tick : visible_from)
        show_tick = ClampWorldMarkedVisibleFrom(show_tick);
    for (int &hide_tick : visible_until)
        hide_tick = ClampWorldMarkedVisibleUntil(hide_tick);

    std::vector<int> &fmir = state.frame_mirror[slot];
    if ((int)fmir.size() < frame_count)
        fmir.resize((size_t)frame_count, 0);
    else if ((int)fmir.size() > frame_count)
        fmir.resize((size_t)frame_count);

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
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->clear();
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
        std::vector<int> old_seq = state.sequence_frames[slot];
        std::vector<WorldSeqArrayRef> refs = WorldMarkedSeqArrays(state, slot);
        std::vector<std::vector<int>> old_vals;
        old_vals.reserve(refs.size());
        for (const WorldSeqArrayRef &ref : refs)
            old_vals.push_back(*ref.vec);

        std::vector<int> new_seq;
        std::vector<std::vector<int>> new_vals(refs.size());
        new_seq.reserve(old_seq.size() + defaults.size());
        for (size_t i = 0; i < old_seq.size(); i++) {
            int idx = old_seq[i];
            if (!doc_get_img(doc, idx)) continue;   /* frame deleted from doc */
            if (std::find(defaults.begin(), defaults.end(), idx) == defaults.end())
                continue;                            /* sprite was unmarked */
            new_seq.push_back(idx);
            for (size_t a = 0; a < refs.size(); a++)
                new_vals[a].push_back(old_vals[a][i]);
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
        }

        state.default_frames[slot] = defaults;
        state.sequence_frames[slot] = new_seq;
        for (size_t a = 0; a < refs.size(); a++)
            *refs[a].vec = new_vals[a];
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

bool WorldMarkedSplitLaneAtFrame(WorldMarkedSequenceState &state,
                                 const WorldMarkedLane &lane,
                                 const std::vector<WorldMarkedLane> &lanes,
                                 int frame_idx)
{
    int src_slot = lane.delay_slot;
    if (lane.dummy_decap ||
        src_slot < 0 || src_slot >= kWorldMarkedSourceTabs)
        return false;

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

    state.sequence_doc[dst_slot] = lane.doc;
    state.sequence_doc_idx[dst_slot] = lane.doc_idx;
    state.default_frames[dst_slot] = tail;
    state.sequence_frames[dst_slot] = tail;
    state.lane_visible[dst_slot] = state.lane_visible[src_slot];
    state.hold_end[dst_slot] = state.hold_end[src_slot];
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
            state.sequence_doc[slot] = NULL;
            state.sequence_doc_idx[slot] = -1;
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
    if (slot < 0 || slot >= kWorldMarkedMaxTabs) return;
    std::vector<int> &frames = state.sequence_frames[slot];
    if (frame_idx < 0 || frame_idx >= (int)frames.size()) return;
    EnsureWorldMarkedFrameDelays(state, slot, (int)frames.size());

    int insert_at = frame_idx + 1;
    frames.insert(frames.begin() + insert_at, frames[frame_idx]);
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->insert(ref.vec->begin() + insert_at, (*ref.vec)[frame_idx]);
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
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        std::swap((*ref.vec)[frame_idx], (*ref.vec)[j]);

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
    for (const WorldSeqArrayRef &ref : WorldMarkedSeqArrays(state, slot))
        ref.vec->erase(ref.vec->begin() + frame_idx);
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
    dst->opals = g_clipboard.has_meta ? g_clipboard.opals : 0;
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
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Pasted clipboard as new sprite: %dx%d.", w, h);
    g_restore_msg_timer = 4.0f;
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
    mark_dirty();
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

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
            if (src[x] != 0)
                dst[x - start_x] = paste_composite_index(src[x], dst[x - start_x],
                                                         target_pal, pal_map,
                                                         remap_palette,
                                                         px + x, py + y);
        }
    }
    if (paste_captured) push_pixel_history_entry(&paste_snap);
    g_img_tex_idx = -2;
}


static void scale_clipboard_to(int nw, int nh)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int sw = g_clipboard.w, sh = g_clipboard.h;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == sw && nh == sh) return;

    unsigned short src_stride = g_clipboard.stride;
    unsigned short dst_stride = (unsigned short)((nw + 3) & ~3);
    unsigned char *src = (unsigned char *)g_clipboard.data_p;
    unsigned char *dst = (unsigned char *)malloc((size_t)dst_stride * nh);
    if (!dst) return;
    memset(dst, 0, (size_t)dst_stride * nh);

    /* Inverse mapping: for each destination pixel, sample the source pixel
       nearest to the center of that destination cell. Avoids the gaps you
       get from forward mapping when the ratio isn't integral. Works for
       both upscale and downscale. */
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

            int sx_idx = (int)(ux * (float)sw / (float)scaled_w);
            int sy_idx = (int)(uy * (float)sh / (float)scaled_h);
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
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mirrored anipoints on %d marked sprite%s.",
                 changed, changed == 1 ? "" : "s");
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

bool DrawWorldMarkedTabs(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    std::vector<WorldMarkedAsmLaneInput> asm_lanes;
    asm_lanes.reserve(2);
    auto add_asm_lane = [&](std::vector<AsmAnim> &anims, bool enabled, int sel,
                            int slot_id, Document *doc, int doc_idx) {
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
            view.piece_doc = &fr.piece_doc;
            view.dx = fr.dx;
            view.dy = fr.dy;
            view.mirror = fr.mirror;
            input.frames.push_back(view);
        }
        asm_lanes.push_back(input);
    };
    add_asm_lane(g_asm_anims, g_asm_lane_enabled, g_asm_anim_sel,
                 kWorldAsmSlot, g_asm_anim_doc, g_asm_anim_doc_idx);
    add_asm_lane(g_asm_opp_anims, g_asm_opp_enabled, g_asm_opp_sel,
                 kWorldAsmOpponentSlot, g_asm_opp_doc, g_asm_opp_doc_idx);

    IMG *selected_img = get_img(g_doc ? g_doc->ilselected : -1);
    WorldMarkedTabsResult tabs_result =
        WorldDrawMarkedTabs(g_world_marked_state, g_world_state,
                            avail, img_pos, io.DeltaTime,
                            document_active_index(), selected_img, asm_lanes);
    if (!tabs_result.drew)
        return false;

    WorldMarkedPanelAction panel_action = tabs_result.panel.header;
    if (panel_action.copied_asm) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Copied World View ASM for %d lane%s.",
                 panel_action.copied_lane_count,
                 panel_action.copied_lane_count == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    if (panel_action.request_save_asm)
        g_request_save_world_asm = true;   /* dialog opened in main loop */
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
    if (tabs_result.panel.thumb_click.clicked) {
        if (tabs_result.panel.thumb_click.doc_idx != document_active_index()) {
            document_set_active(tabs_result.panel.thumb_click.doc_idx);
            ResetPerDocumentUiState(false);
            g_doc_tab_select_request = tabs_result.panel.thumb_click.doc_idx;
        }
        g_doc->ilselected = tabs_result.panel.thumb_click.img_idx;
        g_zoom_reset = true;
    }
    if (tabs_result.panel.copied_popup_asm) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Copied World View ASM.");
        g_restore_msg_timer = 4.0f;
    }
    return true;
}

