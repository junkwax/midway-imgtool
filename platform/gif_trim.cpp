/*************************************************************
 * platform/gif_trim.cpp
 * Implementation of the border-trim analysis declared in gif_trim.h.
 *************************************************************/
#include "gif_trim.h"

#include <algorithm>
#include <cstring>
#include <vector>

bool GifBorderBackgroundColor(const unsigned char *rgba, int w, int h,
                              int frame_count, unsigned char out_rgb[3])
{
    if (!rgba || !out_rgb || w <= 0 || h <= 0 || frame_count <= 0) return false;

    /* Exact 8-bit colors, not 15-bit buckets. A bucketed answer is reconstructed
       a shade off the pixels it came from (rgb 0,20,44 comes back 0,16,41), and
       then a tolerance of 0 matches nothing at all — which is the failure this
       whole path exists to avoid. There are only 2*(w+h) border samples per
       frame, so counting them exactly is cheap. */
    std::vector<unsigned int> samples;
    samples.reserve((size_t)2 * (w + h) * frame_count);

    const size_t pixels_per_frame = (size_t)w * (size_t)h;
    for (int f = 0; f < frame_count; f++) {
        const unsigned char *frame = rgba + (size_t)f * pixels_per_frame * 4;
        for (int y = 0; y < h; y++) {
            bool edge_row = (y == 0 || y == h - 1);
            int step = edge_row ? 1 : (w > 1 ? w - 1 : 1);
            for (int x = 0; x < w; x += step) {
                const unsigned char *p = frame + ((size_t)y * w + x) * 4;
                if (p[3] < 128) continue;
                samples.push_back(((unsigned int)p[0] << 16) |
                                  ((unsigned int)p[1] << 8) | p[2]);
            }
        }
    }
    if (samples.empty()) return false;

    /* Sorted run-length scan rather than a hash map: a dithered border splits
       evenly between two shades, and ties have to resolve the same way every
       run or the trim result would wobble between imports. Ascending order with
       a strict > keeps the lowest color on a tie. */
    std::sort(samples.begin(), samples.end());
    unsigned int best = samples[0];
    size_t best_count = 0;
    size_t i = 0;
    while (i < samples.size()) {
        size_t j = i;
        while (j < samples.size() && samples[j] == samples[i]) j++;
        if (j - i > best_count) { best_count = j - i; best = samples[i]; }
        i = j;
    }

    out_rgb[0] = (unsigned char)((best >> 16) & 0xFF);
    out_rgb[1] = (unsigned char)((best >> 8) & 0xFF);
    out_rgb[2] = (unsigned char)(best & 0xFF);
    return true;
}

GifTrimBasis GifFramesContentBBox(const unsigned char *rgba, int w, int h,
                                  int frame_count, int tolerance,
                                  unsigned char out_bg[3],
                                  int *out_x0, int *out_y0,
                                  int *out_x1, int *out_y1)
{
    if (!rgba || w <= 0 || h <= 0 || frame_count <= 0) return GifTrim_None;
    const size_t pixels_per_frame = (size_t)w * (size_t)h;

    bool any_transparent = false;
    for (int f = 0; f < frame_count && !any_transparent; f++) {
        const unsigned char *frame = rgba + (size_t)f * pixels_per_frame * 4;
        for (size_t i = 0; i < pixels_per_frame; i++) {
            if (frame[i * 4 + 3] < 128) { any_transparent = true; break; }
        }
    }

    unsigned char bg[3] = {0, 0, 0};
    bool use_bg = false;
    if (!any_transparent) {
        if (!GifBorderBackgroundColor(rgba, w, h, frame_count, bg))
            return GifTrim_None;
        use_bg = true;
    }
    if (tolerance < 0) tolerance = 0;
    if (tolerance > 255) tolerance = 255;

    int x0 = w, y0 = h, x1 = -1, y1 = -1;
    for (int f = 0; f < frame_count; f++) {
        const unsigned char *frame = rgba + (size_t)f * pixels_per_frame * 4;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                const unsigned char *p = frame + ((size_t)y * w + x) * 4;
                if (p[3] < 128) continue;
                if (use_bg) {
                    int dr = (int)p[0] - (int)bg[0];
                    int dg = (int)p[1] - (int)bg[1];
                    int db = (int)p[2] - (int)bg[2];
                    if (dr < 0) dr = -dr;
                    if (dg < 0) dg = -dg;
                    if (db < 0) db = -db;
                    if (dr <= tolerance && dg <= tolerance && db <= tolerance)
                        continue;
                }
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
        }
    }
    if (x1 < x0 || y1 < y0) return GifTrim_None;

    if (out_x0) *out_x0 = x0;
    if (out_y0) *out_y0 = y0;
    if (out_x1) *out_x1 = x1;
    if (out_y1) *out_y1 = y1;
    if (out_bg) { out_bg[0] = bg[0]; out_bg[1] = bg[1]; out_bg[2] = bg[2]; }
    return use_bg ? GifTrim_Background : GifTrim_Alpha;
}
