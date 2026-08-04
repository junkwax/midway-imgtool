/*************************************************************
 * platform/canvas_ops.cpp
 * Implementation of the pure canvas/paste helpers declared in canvas_ops.h.
 *************************************************************/
#include "canvas_ops.h"
#include "palette_math.h"   /* palette_word_at */

#include <cstring>

void CanvasAnchorOffset(int old_w, int old_h, int new_w, int new_h,
                        int anchor, int *out_dx, int *out_dy)
{
    if (anchor < 0 || anchor >= CanvasAnchor_Count) anchor = CanvasAnchor_TopLeft;
    int col = anchor % 3;   /* 0 = left, 1 = center, 2 = right */
    int row = anchor / 3;   /* 0 = top,  1 = middle, 2 = bottom */

    int slack_x = new_w - old_w;
    int slack_y = new_h - old_h;

    /* Integer division truncates toward zero, which is exactly what keeps
       growing and cropping symmetric on an odd difference: growing 4 -> 7
       parks the art at +1 (spare pixel on the right), and cropping 7 -> 4
       takes it back with -1 instead of shaving a column off the art. */
    int dx = (col == 0) ? 0 : (col == 2 ? slack_x : slack_x / 2);
    int dy = (row == 0) ? 0 : (row == 2 ? slack_y : slack_y / 2);

    if (out_dx) *out_dx = dx;
    if (out_dy) *out_dy = dy;
}

void CanvasBlitIndexedOffset(const unsigned char *src, int src_w, int src_h,
                             int src_stride,
                             unsigned char *dst, int dst_w, int dst_h,
                             int dst_stride,
                             int dx, int dy)
{
    if (!src || !dst || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0)
        return;

    for (int sy = 0; sy < src_h; sy++) {
        int ty = sy + dy;
        if (ty < 0 || ty >= dst_h) continue;

        /* Clip the row span once instead of testing every column. */
        int sx0 = 0;
        int sx1 = src_w;
        if (dx < 0) sx0 = -dx;
        if (dx + src_w > dst_w) sx1 = dst_w - dx;
        if (sx0 >= sx1) continue;

        memcpy(dst + (size_t)ty * dst_stride + (sx0 + dx),
               src + (size_t)sy * src_stride + sx0,
               (size_t)(sx1 - sx0));
    }
}

bool CanvasIndexedContentBounds(const unsigned char *src, int w, int h,
                                int stride,
                                int *out_min_x, int *out_min_y,
                                int *out_max_x, int *out_max_y)
{
    if (!src || w <= 0 || h <= 0) return false;

    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = src + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            if (!row[x]) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < 0) return false;

    if (out_min_x) *out_min_x = min_x;
    if (out_min_y) *out_min_y = min_y;
    if (out_max_x) *out_max_x = max_x;
    if (out_max_y) *out_max_y = max_y;
    return true;
}

/* Clamp one axis of a nudge. Split out because both axes want the identical
   "don't push further out, but let it come back" rule. */
static int ClampNudgeAxis(int lo, int hi, int extent, int delta)
{
    if (delta < 0) {
        /* Moving toward 0: stop when the leading edge reaches it, unless it
           already started past it — then the move is a recovery, not a crop. */
        int room = lo;
        if (room < 0) room = 0;
        if (-delta > room) delta = -room;
    } else if (delta > 0) {
        int room = extent - 1 - hi;
        if (room < 0) room = 0;
        if (delta > room) delta = room;
    }
    return delta;
}

void CanvasClampContentNudge(int min_x, int min_y, int max_x, int max_y,
                             int canvas_w, int canvas_h,
                             int *dx, int *dy)
{
    if (dx) *dx = ClampNudgeAxis(min_x, max_x, canvas_w, *dx);
    if (dy) *dy = ClampNudgeAxis(min_y, max_y, canvas_h, *dy);
}

int CanvasClearIndexedRect(unsigned char *buf, int w, int h, int stride,
                           int x1, int y1, int x2, int y2,
                           const unsigned char *mask, int mask_w, int mask_h)
{
    if (!buf || w <= 0 || h <= 0) return 0;

    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0;
    if (y1 < 0) y1 = 0;
    if (x2 > w - 1) x2 = w - 1;
    if (y2 > h - 1) y2 = h - 1;
    if (x1 > x2 || y1 > y2) return 0;

    int cleared = 0;
    for (int y = y1; y <= y2; y++) {
        unsigned char *row = buf + (size_t)y * stride;
        for (int x = x1; x <= x2; x++) {
            if (mask) {
                if (x >= mask_w || y >= mask_h) continue;
                if (!mask[(size_t)y * mask_w + x]) continue;
            }
            if (!row[x]) continue;
            row[x] = 0;
            cleared++;
        }
    }
    return cleared;
}

void CanvasCollectUsedIndices(const unsigned char *buf, int w, int h, int stride,
                              bool used[256])
{
    if (!used) return;
    if (!buf || w <= 0 || h <= 0) return;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = buf + (size_t)y * stride;
        for (int x = 0; x < w; x++)
            used[row[x]] = true;
    }
}

PaletteImportPlan PlanPaletteColorImport(const unsigned char *src_pal, int src_numc,
                                         const bool src_used[256],
                                         const unsigned char *dst_pal, int dst_numc,
                                         int dst_capacity,
                                         const bool *dst_free)
{
    PaletteImportPlan plan;
    if (dst_numc < 0) dst_numc = 0;
    if (dst_numc > 256) dst_numc = 256;
    plan.new_numc = dst_numc;

    if (!src_pal || !src_used || src_numc <= 1) return plan;
    if (src_numc > 256) src_numc = 256;
    if (dst_capacity > 256) dst_capacity = 256;
    if (dst_capacity < dst_numc) dst_capacity = dst_numc;

    /* Slot ledger for the target as it will look while the plan is built, so
       two source colors that are equal never claim two slots and a color that
       matches one just added is reported as matched. */
    unsigned short word[256] = {0};
    bool occupied[256] = {false};
    if (dst_pal) {
        for (int i = 0; i < dst_numc; i++) {
            word[i] = palette_word_at(dst_pal, i);
            occupied[i] = true;
        }
    }
    /* Reusable holes are occupied for lookup purposes only until claimed. */
    bool reusable[256] = {false};
    if (dst_free) {
        for (int i = 1; i < dst_numc; i++)
            reusable[i] = dst_free[i];
    }

    int next_append = dst_numc;
    int next_hole = 1;

    for (int i = 1; i < src_numc; i++) {
        if (!src_used[i]) continue;
        unsigned short w = palette_word_at(src_pal, i);

        /* Already there — the ordinary nearest-match remap lands on it at
           distance 0. A hole that happens to hold the right color counts, but
           gets pinned so a later color can't claim it out from under this
           one. */
        int match = -1;
        for (int j = 1; j < 256; j++) {
            if (!occupied[j] || word[j] != w) continue;
            match = j;
            break;
        }
        if (match >= 0) {
            reusable[match] = false;
            plan.matched++;
            continue;
        }

        int slot = -1;
        if (next_append < dst_capacity) {
            slot = next_append++;
        } else {
            while (next_hole < dst_numc && !reusable[next_hole]) next_hole++;
            if (next_hole < dst_numc) slot = next_hole++;
        }
        if (slot < 0) { plan.unmatched++; continue; }

        PaletteImportSlot entry;
        entry.src_index = (unsigned char)i;
        entry.dst_index = (unsigned char)slot;
        entry.word = w;
        plan.added.push_back(entry);

        word[slot] = w;
        occupied[slot] = true;
        reusable[slot] = false;
        if (slot + 1 > plan.new_numc) plan.new_numc = slot + 1;
    }

    return plan;
}
