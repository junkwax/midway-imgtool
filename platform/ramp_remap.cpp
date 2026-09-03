/*************************************************************
 * platform/ramp_remap.cpp
 * Implementation of the palette assembly and luminance remap declared in
 * ramp_remap.h. See that header for why the remap is luminance-only.
 *************************************************************/
#include "ramp_remap.h"

#include <cstring>

namespace {

inline double luma_of_rgb8(int r, int g, int b)
{
    return 0.30 * r + 0.59 * g + 0.11 * b;
}

} /* namespace */

int AssemblePalette(const MaterialRamp *ramps, int ramp_count, AssembledPalette *out)
{
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    out->numc = 1;   /* index 0, transparent, already zeroed */
    if (!ramps || ramp_count <= 0) return 0;

    int assembled = 0;
    for (int i = 0; i < ramp_count; i++) {
        const MaterialRamp &r = ramps[i];
        if (r.count <= 0) continue;
        if (out->numc + r.count > 256) continue;
        if (out->span_count >= RAMP_REMAP_MAX_MATERIALS) break;

        AssembledPalette::Span &span = out->spans[out->span_count++];
        span.material_id = r.material_id;
        span.start = out->numc;
        span.count = r.count;

        for (int k = 0; k < r.count; k++)
            out->words[out->numc + k] = RampColorWord(r.colors[k]);
        out->numc += r.count;
        assembled++;
    }
    return assembled;
}

int AssembleFromExistingPalette(const unsigned short *target_words, int target_numc,
                                const AssembledPalette::Span *blocks, int block_count,
                                AssembledPalette *out)
{
    if (!out) return 0;
    std::memset(out, 0, sizeof(*out));
    out->numc = 1;
    if (!target_words || target_numc <= 0) return 0;

    if (target_numc > 256) target_numc = 256;
    std::memcpy(out->words, target_words, (size_t)target_numc * sizeof(unsigned short));
    out->numc = target_numc;

    if (!blocks || block_count <= 0) return 0;

    int written = 0;
    for (int i = 0; i < block_count && out->span_count < RAMP_REMAP_MAX_MATERIALS; i++) {
        int start = blocks[i].start;
        int count = blocks[i].count;
        if (count <= 0 || start >= target_numc || start + count <= 0) continue;
        if (start < 0) { count += start; start = 0; }
        if (start + count > target_numc) count = target_numc - start;
        if (count <= 0) continue;

        AssembledPalette::Span &span = out->spans[out->span_count++];
        span.material_id = blocks[i].material_id;
        span.start = start;
        span.count = count;
        written++;
    }
    return written;
}

int RemapFrameToRamps(const unsigned char *rgb, const unsigned char *alpha,
                      const unsigned char *mask, int w, int h,
                      const AssembledPalette &assembled,
                      unsigned char *out_indices)
{
    if (!rgb || !alpha || !mask || !out_indices || w <= 0 || h <= 0) return 0;

    const size_t n = (size_t)w * (size_t)h;
    std::memset(out_indices, 0, n);

    int mapped = 0;
    for (size_t i = 0; i < n; i++) {
        if (alpha[i] == 0) continue;

        const unsigned char mid = mask[i];
        const AssembledPalette::Span *span = nullptr;
        for (int s = 0; s < assembled.span_count; s++) {
            if (assembled.spans[s].material_id == mid) { span = &assembled.spans[s]; break; }
        }
        if (!span || span->count <= 0) continue;

        const double y = luma_of_rgb8(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);

        int best_k = 0;
        double best_d = -1.0;
        for (int k = 0; k < span->count; k++) {
            const int idx = span->start + k;
            int r5, g5, b5;
            r5 = (assembled.words[idx] >> 10) & 0x1F;
            g5 = (assembled.words[idx] >>  5) & 0x1F;
            b5 =  assembled.words[idx]        & 0x1F;
            /* Expand 5-bit to 8-bit by bit replication (31 -> 255, not 248),
               the same expansion img_format.h's pal_word_to_rgb8 uses, so
               the ramp side of the comparison lines up with the pixel's
               real 8-bit luma instead of reading systematically dark. */
            const int r8 = (r5 << 3) | (r5 >> 2);
            const int g8 = (g5 << 3) | (g5 >> 2);
            const int b8 = (b5 << 3) | (b5 >> 2);
            const double ry = luma_of_rgb8(r8, g8, b8);
            const double d = ry > y ? ry - y : y - ry;
            if (best_d < 0.0 || d < best_d) { best_d = d; best_k = k; }
        }

        out_indices[i] = (unsigned char)(span->start + best_k);
        mapped++;
    }
    return mapped;
}
