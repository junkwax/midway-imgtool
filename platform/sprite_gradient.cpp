/*************************************************************
 * platform/sprite_gradient.cpp
 * Implementation of the index-assigning gradient declared in
 * sprite_gradient.h. See that header for what shade_span means and why the
 * dither is load-bearing rather than decorative.
 *************************************************************/
#include "sprite_gradient.h"

#include <cmath>
#include <cstring>

namespace {

int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

float clamp01(float v)
{
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
}

/* Rec.601-weighted luminance of a packed 15-bit word, in 0..255*100 units —
   the same integer weighting the Indexed Color Gradient fits its stops by. */
int luma_of_word(unsigned short word)
{
    int r = ((word >> 10) & 31) * 255 / 31;
    int g = ((word >>  5) & 31) * 255 / 31;
    int b = ( word        & 31) * 255 / 31;
    return 30 * r + 59 * g + 11 * b;
}

unsigned int pixel_hash(int x, int y, unsigned char ci, int image_idx, int seed)
{
    unsigned int h = 2166136261u;
    h ^= (unsigned int)(x + seed * 17); h *= 16777619u;
    h ^= (unsigned int)(y + seed * 31); h *= 16777619u;
    h ^= (unsigned int)ci;              h *= 16777619u;
    h ^= (unsigned int)(image_idx + 1); h *= 16777619u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}

/* Threshold in 0..1 for the pixel, per screen. Same matrices the Opacity
   Gradient screens with, so the two tools' dithers interleave predictably when
   both are used on one sprite. */
float dither_threshold(int mode, int x, int y, unsigned char ci,
                       int image_idx, int seed)
{
    static const int kBayer2[2][2] = { { 0, 2 },
                                       { 3, 1 } };
    static const int kBayer4[4][4] = { {  0,  8,  2, 10 },
                                       { 12,  4, 14,  6 },
                                       {  3, 11,  1,  9 },
                                       { 15,  7, 13,  5 } };
    switch (mode) {
    case kSpriteGradientDitherNoise:
        return (float)(pixel_hash(x, y, ci, image_idx, seed) % 1000u) / 1000.0f;
    case kSpriteGradientDitherChecker:
        return ((float)kBayer2[y & 1][x & 1] + 0.5f) / 4.0f;
    case kSpriteGradientDitherBayer:
        return ((float)kBayer4[y & 3][x & 3] + 0.5f) / 16.0f;
    default:
        return 0.5f;   /* unreachable: kSpriteGradientDitherNone rounds instead */
    }
}

bool opaque_bounds(const unsigned char *pixels, int w, int h, int stride,
                   int *min_x, int *min_y, int *max_x, int *max_y)
{
    int lx = w, ly = h, rx = -1, by = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (pixels[(size_t)y * stride + x] == 0) continue;
            if (x < lx) lx = x;
            if (y < ly) ly = y;
            if (x > rx) rx = x;
            if (y > by) by = y;
        }
    }
    if (rx < 0) return false;
    *min_x = lx; *min_y = ly; *max_x = rx; *max_y = by;
    return true;
}

/* Chamfer distance from every opaque pixel to the nearest transparent one, in
   pixels. Two passes with 1 / sqrt(2) weights, and anything off-canvas counts
   as transparent — a sprite trimmed flush to its art still has an edge there.
   Deliberately identical to the Opacity Gradient's edge feather, so a feather
   and a rim ramp of the same width cover exactly the same pixels. */
void edge_distance(const unsigned char *pixels, int w, int h, int stride,
                   std::vector<float> &dist)
{
    const float BIG = 1.0e9f;
    dist.assign((size_t)w * (size_t)h, BIG);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (pixels[(size_t)y * stride + x] == 0)
                dist[(size_t)y * w + x] = 0.0f;

    const float D1 = 1.0f;
    const float D2 = 1.41421356f;
    auto at = [&](int ax, int ay) -> float {
        if (ax < 0 || ay < 0 || ax >= w || ay >= h) return 0.0f;
        return dist[(size_t)ay * w + ax];
    };

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            float d = dist[(size_t)y * w + x];
            if (d == 0.0f) continue;
            float c;
            c = at(x - 1, y    ) + D1; if (c < d) d = c;
            c = at(x    , y - 1) + D1; if (c < d) d = c;
            c = at(x - 1, y - 1) + D2; if (c < d) d = c;
            c = at(x + 1, y - 1) + D2; if (c < d) d = c;
            dist[(size_t)y * w + x] = d;
        }
    }
    for (int y = h - 1; y >= 0; y--) {
        for (int x = w - 1; x >= 0; x--) {
            float d = dist[(size_t)y * w + x];
            if (d == 0.0f) continue;
            float c;
            c = at(x + 1, y    ) + D1; if (c < d) d = c;
            c = at(x    , y + 1) + D1; if (c < d) d = c;
            c = at(x + 1, y + 1) + D2; if (c < d) d = c;
            c = at(x - 1, y + 1) + D2; if (c < d) d = c;
            dist[(size_t)y * w + x] = d;
        }
    }
}

/* Distance along the gradient axis and the axis' full length, both in pixels,
   measured from the side t=0 sits on. Kept in pixels rather than a straight
   0..1 sweep so band_px can confine the ramp to one edge. */
void axis_depth(int direction, int x, int y,
                int min_x, int min_y, int max_x, int max_y,
                float *out_depth, float *out_span)
{
    float w_span = (float)(max_x - min_x);
    float h_span = (float)(max_y - min_y);
    float depth = 0.0f, span = 0.0f;
    switch (direction) {
    case kSpriteGradientLeftRight: depth = (float)(x - min_x); span = w_span; break;
    case kSpriteGradientRightLeft: depth = (float)(max_x - x); span = w_span; break;
    case kSpriteGradientTopBottom: depth = (float)(y - min_y); span = h_span; break;
    case kSpriteGradientBottomTop: depth = (float)(max_y - y); span = h_span; break;
    case kSpriteGradientCenterEdge:
    case kSpriteGradientEdgeCenter: {
        float cx = ((float)min_x + (float)max_x) * 0.5f;
        float cy = ((float)min_y + (float)max_y) * 0.5f;
        float dx = (float)x - cx;
        float dy = (float)y - cy;
        float d = sqrtf(dx * dx + dy * dy);
        float corners[4][2] = {
            { (float)min_x - cx, (float)min_y - cy },
            { (float)max_x - cx, (float)min_y - cy },
            { (float)min_x - cx, (float)max_y - cy },
            { (float)max_x - cx, (float)max_y - cy }
        };
        float max_d = 0.0f;
        for (int i = 0; i < 4; i++) {
            float cd = sqrtf(corners[i][0] * corners[i][0] +
                             corners[i][1] * corners[i][1]);
            if (cd > max_d) max_d = cd;
        }
        if (d > max_d) d = max_d;
        span = max_d;
        depth = (direction == kSpriteGradientCenterEdge) ? d : max_d - d;
        break;
    }
    default:
        break;
    }
    if (depth < 0.0f) depth = 0.0f;
    *out_depth = depth;
    *out_span = span;
}

} /* namespace */

int SpriteGradientBuildShadeLut(const unsigned char *pixels, int w, int h, int stride,
                                const unsigned short *pal_words, int pal_numc,
                                float lut[256])
{
    if (!lut) return 0;
    for (int i = 0; i < 256; i++) lut[i] = -1.0f;
    if (!pixels || w <= 0 || h <= 0 || stride < w) return 0;

    bool present[256] = {};
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            present[pixels[(size_t)y * stride + x]] = true;

    int luma[256] = {};
    int lo = 0x7fffffff, hi = -1, count = 0;
    for (int i = 1; i < 256; i++) {
        if (!present[i]) continue;
        unsigned short word = (pal_words && i < pal_numc) ? pal_words[i] : 0;
        luma[i] = luma_of_word(word);
        if (luma[i] < lo) lo = luma[i];
        if (luma[i] > hi) hi = luma[i];
        count++;
    }
    if (count == 0) return 0;

    for (int i = 1; i < 256; i++) {
        if (!present[i]) continue;
        lut[i] = (hi > lo) ? (float)(luma[i] - lo) / (float)(hi - lo) : 0.5f;
    }
    return count;
}

bool SpriteGradientPlanBuild(const unsigned char *pixels, int w, int h, int stride,
                             const unsigned short *pal_words, int pal_numc,
                             const SpriteGradientOptions &opt,
                             SpriteGradientPlan *out)
{
    if (!out) return false;
    *out = SpriteGradientPlan();
    if (!pixels || w <= 0 || h <= 0 || stride < w) return false;

    SpriteGradientOptions o = opt;
    o.direction = clamp_int(o.direction, 0, kSpriteGradientDirectionCount - 1);
    o.ramp_start = clamp_int(o.ramp_start, 1, 255);
    o.ramp_count = clamp_int(o.ramp_count, 2, 256 - o.ramp_start);
    o.shade_span = clamp_int(o.shade_span, 1, o.ramp_count);
    o.start_pct = clamp_int(o.start_pct, 0, 100);
    o.end_pct = clamp_int(o.end_pct, 0, 100);
    o.dither = clamp_int(o.dither, 0, kSpriteGradientDitherBayer);
    o.band_px = clamp_int(o.band_px, 0, 512);
    if (o.direction == kSpriteGradientEdgeInward && o.band_px <= 0)
        o.band_px = 5;   /* the feather has no other scale to fall back on */

    out->opt = o;
    out->w = w;
    out->h = h;
    out->stride = stride;
    out->min_x = 0;
    out->min_y = 0;
    out->max_x = w - 1;
    out->max_y = h - 1;
    if (o.content_bounds || o.direction == kSpriteGradientEdgeInward) {
        if (!opaque_bounds(pixels, w, h, stride,
                           &out->min_x, &out->min_y, &out->max_x, &out->max_y))
            return false;
    }

    out->shade_count = SpriteGradientBuildShadeLut(pixels, w, h, stride,
                                                   pal_words, pal_numc,
                                                   out->shade_lut);
    if (out->shade_count == 0) return false;

    if (o.direction == kSpriteGradientEdgeInward)
        edge_distance(pixels, w, h, stride, out->edge_dist);
    return true;
}

int SpriteGradientIndexAt(const SpriteGradientPlan &plan, int x, int y,
                          unsigned char src_index, int image_idx)
{
    if (src_index == 0) return 0;
    if (x < 0 || y < 0 || x >= plan.w || y >= plan.h) return src_index;

    const SpriteGradientOptions &o = plan.opt;

    float t;
    if (o.direction == kSpriteGradientEdgeInward) {
        /* Depth 1 is the outermost opaque pixel; past the band the art is left
           exactly as it was. */
        float band = (float)o.band_px;
        float d = plan.edge_dist[(size_t)y * plan.w + x];
        if (d > band) return src_index;
        t = (band > 1.0f) ? (d - 1.0f) / (band - 1.0f) : 1.0f;
        t = clamp01(t);
    } else {
        float depth = 0.0f, span = 0.0f;
        axis_depth(o.direction, x, y, plan.min_x, plan.min_y, plan.max_x, plan.max_y,
                   &depth, &span);
        float denom = span;
        if (o.band_px > 0) {
            denom = (float)o.band_px;
            if (depth > denom) return src_index;   /* deeper than the band */
        }
        t = (denom > 0.0f) ? clamp01(depth / denom) : 0.0f;
    }

    float shade = plan.shade_lut[src_index];
    if (shade < 0.0f) shade = 0.5f;   /* index the sprite gained mid-edit */

    /* The shading occupies a window of the ramp; t slides that window. */
    int window = o.shade_span;
    float travel = (float)(o.ramp_count - window);
    float lo = travel * (float)o.start_pct / 100.0f;
    float hi = travel * (float)o.end_pct / 100.0f;
    float pos = lo + (hi - lo) * t + shade * (float)(window - 1);

    int step;
    if (o.dither == kSpriteGradientDitherNone) {
        step = (int)floorf(pos + 0.5f);
    } else {
        step = (int)floorf(pos);
        float frac = pos - (float)step;
        if (dither_threshold(o.dither, x, y, src_index, image_idx, o.seed) < frac)
            step++;
    }
    step = clamp_int(step, 0, o.ramp_count - 1);
    return o.ramp_start + step;
}

int SpriteGradientApply(unsigned char *pixels, const SpriteGradientPlan &plan,
                        int image_idx)
{
    if (!pixels || plan.w <= 0 || plan.h <= 0) return 0;
    int changed = 0;
    for (int y = 0; y < plan.h; y++) {
        for (int x = 0; x < plan.w; x++) {
            unsigned char *p = pixels + (size_t)y * plan.stride + x;
            if (*p == 0) continue;
            int ni = SpriteGradientIndexAt(plan, x, y, *p, image_idx);
            if (ni == (int)*p) continue;
            *p = (unsigned char)ni;
            changed++;
        }
    }
    return changed;
}

int SpriteGradientMaxIndex(const unsigned char *pixels, const SpriteGradientPlan &plan,
                           int image_idx)
{
    if (!pixels || plan.w <= 0 || plan.h <= 0) return 0;
    int max_idx = 0;
    for (int y = 0; y < plan.h; y++) {
        for (int x = 0; x < plan.w; x++) {
            unsigned char ci = pixels[(size_t)y * plan.stride + x];
            if (ci == 0) continue;
            int ni = SpriteGradientIndexAt(plan, x, y, ci, image_idx);
            if (ni > max_idx) max_idx = ni;
        }
    }
    return max_idx;
}

int SpriteGradientBuildRamp(const float *stops_rgb, int stop_count,
                            int count, unsigned short *out_words)
{
    if (!stops_rgb || !out_words || stop_count < 2 || count < 1) return 0;
    for (int i = 0; i < count; i++) {
        float t = (count > 1) ? (float)i / (float)(count - 1) : 0.0f;
        float scaled = t * (float)(stop_count - 1);
        int seg = (int)floorf(scaled);
        if (seg > stop_count - 2) seg = stop_count - 2;
        if (seg < 0) seg = 0;
        float f = scaled - (float)seg;
        int ch[3];
        for (int c = 0; c < 3; c++) {
            float a = stops_rgb[seg * 3 + c];
            float b = stops_rgb[(seg + 1) * 3 + c];
            ch[c] = clamp_int((int)lroundf(255.0f * (a + (b - a) * f)), 0, 255);
        }
        out_words[i] = (unsigned short)(((ch[0] >> 3) << 10) |
                                        ((ch[1] >> 3) <<  5) |
                                         (ch[2] >> 3));
    }
    return count;
}

int SpriteGradientFindRampBlock(const bool used[256], int count, int max_index)
{
    if (!used || count < 1) return -1;
    if (max_index > 255) max_index = 255;
    if (max_index < 1) return -1;

    int run = 0;
    for (int i = 1; i <= max_index; i++) {
        run = used[i] ? 0 : run + 1;
        if (run >= count) return i - count + 1;
    }
    return -1;
}

int SpriteGradientLargestFreeRun(const bool used[256], int max_index)
{
    if (!used) return 0;
    if (max_index > 255) max_index = 255;
    int run = 0, best = 0;
    for (int i = 1; i <= max_index; i++) {
        run = used[i] ? 0 : run + 1;
        if (run > best) best = run;
    }
    return best;
}
