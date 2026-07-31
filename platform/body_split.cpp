/*************************************************************
 * platform/body_split.cpp
 * Head / arms / torso / legs segmentation declared in body_split.h.
 *
 * The shape of the heuristic: a standing fighter's silhouette narrows at the
 * neck and again at the hips, and its arms stick out sideways from a dense
 * torso column. So we cut horizontally at the two narrowest rows in the
 * plausible bands, then cut the torso band vertically at the edges of its
 * densest column run. Everything is derived from row/column occupancy, which
 * survives palette and outline differences between characters.
 *************************************************************/
#include "body_split.h"

#include <string.h>
#include <stdio.h>
#include <vector>

namespace {

/* Bands are expressed as fractions of the content height so the same numbers
   work for a 40px portrait sprite and a 200px full-body one. */
const float kNeckSearchTop     = 0.10f;
const float kNeckSearchBottom  = 0.45f;
const float kNeckExpected      = 0.22f;
const float kWaistSearchTop    = 0.38f;
const float kWaistSearchBottom = 0.75f;
const float kWaistExpected     = 0.55f;

/* Fallback proportions when the silhouette has no usable narrowing. */
const float kFallbackNeck  = 0.25f;
const float kFallbackWaist = 0.55f;

/* A column belongs to the torso core when it is opaque for at least this
   fraction of the torso band's rows. Arms are thinner than that. */
const float kTorsoCoreDensity = 0.45f;

const int kMinBandHeight = 2;

struct RowSpan {
    int min_x;
    int max_x;
    int width;   /* 0 when the row is empty */
};

int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Pick the narrowest row in [lo, hi], breaking ties toward `expected` so a
   flat-sided sprite cuts near the anatomically likely spot instead of at
   whichever equal-width row happened to come first. */
int narrowest_row(const RowSpan *rows, int lo, int hi, int expected,
                  int *out_width)
{
    int best_y = -1;
    long long best_score = 0;
    for (int y = lo; y <= hi; y++) {
        if (rows[y].width <= 0) continue;   /* a gap, not a neck */
        int dist = y > expected ? y - expected : expected - y;
        long long score = (long long)rows[y].width * 1000 + dist;
        if (best_y < 0 || score < best_score) {
            best_score = score;
            best_y = y;
            if (out_width) *out_width = rows[y].width;
        }
    }
    return best_y;
}

} /* namespace */

const char *BodyPartName(int part)
{
    switch (part) {
        case BodyPart_Head:  return "Head";
        case BodyPart_ArmL:  return "Arm L";
        case BodyPart_Torso: return "Torso";
        case BodyPart_ArmR:  return "Arm R";
        case BodyPart_Legs:  return "Legs";
    }
    return "";
}

const char *BodyPartSuffix(int part)
{
    switch (part) {
        case BodyPart_Head:  return "HD";
        case BodyPart_ArmL:  return "AL";
        case BodyPart_Torso: return "TR";
        case BodyPart_ArmR:  return "AR";
        case BodyPart_Legs:  return "LG";
    }
    return "";
}

int BodyRectOpaquePixels(const unsigned char *pixels, int w, int h, int stride,
                         const BodyPartRect &rect)
{
    if (!pixels || w <= 0 || h <= 0 || rect.w <= 0 || rect.h <= 0) return 0;
    int x0 = clamp_int(rect.x, 0, w);
    int y0 = clamp_int(rect.y, 0, h);
    int x1 = clamp_int(rect.x + rect.w, 0, w);
    int y1 = clamp_int(rect.y + rect.h, 0, h);
    int count = 0;
    for (int y = y0; y < y1; y++) {
        const unsigned char *row = pixels + (size_t)y * stride;
        for (int x = x0; x < x1; x++)
            if (row[x] != 0) count++;
    }
    return count;
}

bool BuildBodySplitPlan(const unsigned char *pixels, int w, int h, int stride,
                        BodySplitPlan *out)
{
    if (!out) return false;
    *out = BodySplitPlan();
    if (!pixels || w <= 0 || h <= 0 || stride < w) return false;

    /* ---- Row occupancy + content bounds ---- */
    std::vector<RowSpan> rows((size_t)h);
    int content_top = -1, content_bottom = -1;
    int content_left = w, content_right = -1;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = pixels + (size_t)y * stride;
        int min_x = w, max_x = -1;
        for (int x = 0; x < w; x++) {
            if (row[x] == 0) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
        }
        rows[y].min_x = min_x;
        rows[y].max_x = max_x;
        rows[y].width = (max_x >= min_x) ? (max_x - min_x + 1) : 0;
        if (rows[y].width > 0) {
            if (content_top < 0) content_top = y;
            content_bottom = y;
            if (min_x < content_left)  content_left = min_x;
            if (max_x > content_right) content_right = max_x;
        }
    }

    if (content_top < 0 || content_right < content_left)
        return false;               /* fully transparent sprite */

    out->content_x = content_left;
    out->content_y = content_top;
    out->content_w = content_right - content_left + 1;
    out->content_h = content_bottom - content_top + 1;

    int ch = out->content_h;
    if (ch < kMinBandHeight * 3) {
        /* Too short to hold three stacked bands; hand back one torso rect so
           the dialog still has something editable to show. */
        BodyPartRect &torso = out->parts[BodyPart_Torso];
        torso.x = out->content_x;
        torso.y = out->content_y;
        torso.w = out->content_w;
        torso.h = out->content_h;
        torso.enabled = true;
        out->neck_y = out->content_y;
        out->waist_y = content_bottom + 1;
        out->used_proportional_fallback = true;
        out->valid = true;
        return true;
    }

    /* ---- Horizontal cuts: neck, then waist ---- */
    int neck_lo = content_top + (int)(ch * kNeckSearchTop);
    int neck_hi = content_top + (int)(ch * kNeckSearchBottom);
    neck_lo = clamp_int(neck_lo, content_top + kMinBandHeight, content_bottom - kMinBandHeight * 2);
    neck_hi = clamp_int(neck_hi, neck_lo, content_bottom - kMinBandHeight * 2);
    int neck_expected = content_top + (int)(ch * kNeckExpected);

    int neck_width = 0;
    int neck_y = narrowest_row(rows.data(), neck_lo, neck_hi, neck_expected, &neck_width);

    int waist_lo = content_top + (int)(ch * kWaistSearchTop);
    int waist_hi = content_top + (int)(ch * kWaistSearchBottom);
    int waist_floor = (neck_y >= 0 ? neck_y : content_top) + kMinBandHeight;
    waist_lo = clamp_int(waist_lo, waist_floor, content_bottom - kMinBandHeight);
    waist_hi = clamp_int(waist_hi, waist_lo, content_bottom - kMinBandHeight);
    int waist_expected = content_top + (int)(ch * kWaistExpected);

    int waist_width = 0;
    int waist_y = narrowest_row(rows.data(), waist_lo, waist_hi, waist_expected, &waist_width);

    if (neck_y < 0 || waist_y < 0 || waist_y <= neck_y) {
        neck_y  = content_top + (int)(ch * kFallbackNeck);
        waist_y = content_top + (int)(ch * kFallbackWaist);
        neck_y  = clamp_int(neck_y, content_top + kMinBandHeight,
                            content_bottom - kMinBandHeight * 2);
        waist_y = clamp_int(waist_y, neck_y + kMinBandHeight,
                            content_bottom - kMinBandHeight);
        out->used_proportional_fallback = true;
    }
    out->neck_y = neck_y;
    out->waist_y = waist_y;

    /* ---- Head and legs take the full width of their own bands ---- */
    auto band_bounds = [&](int y0, int y1, int *bx, int *bw) {
        int lo = w, hi = -1;
        for (int y = y0; y <= y1 && y <= content_bottom; y++) {
            if (rows[y].width <= 0) continue;
            if (rows[y].min_x < lo) lo = rows[y].min_x;
            if (rows[y].max_x > hi) hi = rows[y].max_x;
        }
        if (hi < lo) { *bx = content_left; *bw = 0; return false; }
        *bx = lo;
        *bw = hi - lo + 1;
        return true;
    };

    BodyPartRect &head = out->parts[BodyPart_Head];
    int hx = 0, hw = 0;
    if (band_bounds(content_top, neck_y - 1, &hx, &hw) && hw > 0) {
        head.x = hx;
        head.y = content_top;
        head.w = hw;
        head.h = neck_y - content_top;
        head.enabled = head.h > 0;
    }

    BodyPartRect &legs = out->parts[BodyPart_Legs];
    int lx = 0, lw = 0;
    if (band_bounds(waist_y, content_bottom, &lx, &lw) && lw > 0) {
        legs.x = lx;
        legs.y = waist_y;
        legs.w = lw;
        legs.h = content_bottom - waist_y + 1;
        legs.enabled = legs.h > 0;
    }

    /* ---- Torso band: split the arms off the dense central column run ---- */
    int band_top = neck_y;
    int band_bottom = waist_y - 1;
    if (band_bottom < band_top) band_bottom = band_top;
    int band_h = band_bottom - band_top + 1;

    int bx = 0, bw = 0;
    band_bounds(band_top, band_bottom, &bx, &bw);
    if (bw <= 0) { bx = content_left; bw = out->content_w; }

    std::vector<int> col_count((size_t)bw, 0);
    for (int y = band_top; y <= band_bottom; y++) {
        const unsigned char *row = pixels + (size_t)y * stride;
        for (int x = 0; x < bw; x++)
            if (row[bx + x] != 0) col_count[x]++;
    }

    int dense_min = (int)(band_h * kTorsoCoreDensity);
    if (dense_min < 1) dense_min = 1;

    /* Longest run of dense columns = the torso core. */
    int best_start = -1, best_len = 0;
    int run_start = -1;
    for (int x = 0; x < bw; x++) {
        bool dense = col_count[x] >= dense_min;
        if (dense && run_start < 0) run_start = x;
        if ((!dense || x == bw - 1) && run_start >= 0) {
            int run_end = dense ? x : x - 1;
            int len = run_end - run_start + 1;
            if (len > best_len) { best_len = len; best_start = run_start; }
            run_start = -1;
        }
    }

    if (best_len <= 0) { best_start = 0; best_len = bw; }

    int core_x0 = bx + best_start;
    int core_x1 = core_x0 + best_len - 1;

    BodyPartRect &arm_l = out->parts[BodyPart_ArmL];
    if (core_x0 > bx) {
        arm_l.x = bx;
        arm_l.y = band_top;
        arm_l.w = core_x0 - bx;
        arm_l.h = band_h;
        arm_l.enabled = true;
    }

    BodyPartRect &arm_r = out->parts[BodyPart_ArmR];
    int band_right = bx + bw - 1;
    if (core_x1 < band_right) {
        arm_r.x = core_x1 + 1;
        arm_r.y = band_top;
        arm_r.w = band_right - core_x1;
        arm_r.h = band_h;
        arm_r.enabled = true;
    }

    BodyPartRect &torso = out->parts[BodyPart_Torso];
    /* An arm that got disabled hands its columns back to the torso, so the
       band is always covered exactly once. */
    torso.x = arm_l.enabled ? core_x0 : bx;
    torso.w = (arm_r.enabled ? core_x1 : band_right) - torso.x + 1;
    torso.y = band_top;
    torso.h = band_h;
    torso.enabled = torso.w > 0 && torso.h > 0;

    out->valid = true;
    return true;
}

int TrimBodySplitPlan(const unsigned char *pixels, int w, int h, int stride,
                      BodySplitPlan *plan)
{
    if (!plan || !pixels || w <= 0 || h <= 0) return 0;

    int kept = 0;
    for (int i = 0; i < BodyPart_Count; i++) {
        BodyPartRect &r = plan->parts[i];
        if (!r.enabled) continue;

        int x0 = clamp_int(r.x, 0, w);
        int y0 = clamp_int(r.y, 0, h);
        int x1 = clamp_int(r.x + r.w, 0, w);
        int y1 = clamp_int(r.y + r.h, 0, h);

        int min_x = x1, min_y = y1, max_x = x0 - 1, max_y = y0 - 1;
        for (int y = y0; y < y1; y++) {
            const unsigned char *row = pixels + (size_t)y * stride;
            for (int x = x0; x < x1; x++) {
                if (row[x] == 0) continue;
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }

        if (max_x < min_x || max_y < min_y) {
            r.enabled = false;
            continue;
        }
        r.x = min_x;
        r.y = min_y;
        r.w = max_x - min_x + 1;
        r.h = max_y - min_y + 1;
        kept++;
    }
    return kept;
}

void BodyPartChildName(const char *parent_name, int part,
                       char *out, size_t out_sz)
{
    if (!out || out_sz == 0) return;
    out[0] = '\0';

    const char *suffix = BodyPartSuffix(part);
    if (!suffix[0]) return;

    /* IMG names are 15 chars plus terminator; trim the parent so the part tag
       always survives, since that is what distinguishes the children. */
    const size_t kMaxName = 15;
    char stem[16] = {0};
    size_t stem_len = 0;
    if (parent_name) {
        while (stem_len < kMaxName && parent_name[stem_len] != '\0') {
            stem[stem_len] = parent_name[stem_len];
            stem_len++;
        }
    }
    if (stem_len == 0) {
        snprintf(stem, sizeof(stem), "SPRITE");
        stem_len = 6;
    }

    size_t tag_len = strlen(suffix) + 1;   /* '_' + suffix */
    if (stem_len + tag_len > kMaxName)
        stem_len = kMaxName - tag_len;
    stem[stem_len] = '\0';

    snprintf(out, out_sz, "%s_%s", stem, suffix);
}
