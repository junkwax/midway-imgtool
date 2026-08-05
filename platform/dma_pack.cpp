/*************************************************************
 * platform/dma_pack.cpp
 *
 * Port of LOAD2's ROM packaging math. See dma_pack.h for why this follows the
 * original so literally.
 *
 * Reference sources, checked in under doc/load2/:
 *   zcom.c    zcom_analysis()      — zero compression + control word
 *   load2.c   ~2415 do_superbpp    — auto pixel packing
 *   load2.c   ~2477 destbits       — uncompressed size, the bar to beat
 *   load2.h   ZCOMPIXELS, zero_array[256]
 *************************************************************/
#include "dma_pack.h"

#include <vector>

/* Widest run either field can encode: 15 units of at most 8 pixels. The
   original spells this 8*15 inline in both scan directions. */
enum { kDmaMaxRunPixels = 8 * 15 };

unsigned long DmaBitsToBytes(unsigned long bits)
{
    return (bits + 7) >> 3;
}

int DmaSuperBpp(const unsigned char *pixels, int w, int h, int stride,
                int palette_bpp)
{
    if (!pixels || w <= 0 || h <= 0 || stride < w) return palette_bpp;

    int max = 0;
    for (int y = 0; y < h; y++) {
        const unsigned char *row = pixels + (size_t)y * (size_t)stride;
        for (int x = 0; x < w; x++)
            if (row[x] > max) max = row[x];
    }

    /* load2.c's ladder verbatim. Only max >= 128 falls through to keep the
       palette's declared depth. Note the low end: a fully transparent sprite
       (max == 0) misses `== 1` and lands on `< 4`, so LOAD2 packs empty art at
       2bpp rather than at the palette depth. Odd, but it is what ships. */
    if (max == 1) return 1;
    if (max < 4)   return 2;
    if (max < 8)   return 3;
    if (max < 16)  return 4;
    if (max < 32)  return 5;
    if (max < 64)  return 6;
    if (max < 128) return 7;
    return palette_bpp;
}

DmaPackResult DmaAnalyzeSprite(const unsigned char *pixels, int w, int h,
                               int stride, int bpp, bool zero_compress)
{
    DmaPackResult out;
    if (bpp < 1) bpp = 1;
    if (bpp > 8) bpp = 8;
    out.bpp = bpp;

    if (!pixels || w <= 0 || h <= 0 || stride < w) {
        out.skip_reason = "no pixel data";
        return out;
    }

    /* destbits in load2.c: the uncompressed cost, and the bar zero
       compression has to beat before it is kept. */
    out.raw_bits = (unsigned long)w * (unsigned long)h * (unsigned long)bpp;
    out.packed_bits = out.raw_bits;
    /* dma2_field before zcom runs: bpp only, no compression bit. */
    out.control_word = (bpp & 7) << 12;

    if (!zero_compress) {
        out.skip_reason = "zero compression off (ZOF>)";
        return out;
    }
    if (w <= kDmaZcomPixels) {
        /* load2.c refuses at animg->xsize <= ZCOMPIXELS. */
        out.skip_reason = "too narrow to zero-compress";
        return out;
    }
    if (h > kDmaZcomMaxRows) {
        out.skip_reason = "taller than LOAD2's 256-line zero_array";
        return out;
    }

    /* The original walks a flat pointer and steps over each row's padding with
       srcaddr += (4 - xsize) & 3; indexing by stride is the same traversal. */
    struct ZeroThing { long lead; long trail; };
    ZeroThing leftover[4] = {};
    std::vector<int> line_lead((size_t)h, 0);
    std::vector<int> line_trail((size_t)h, 0);

    /* Pass 1: measure each line's leading and trailing zero runs, and total up
       how many zeros each of the four run-unit sizes would fail to encode. */
    for (int y = 0; y < h; y++) {
        const unsigned char *src = pixels + (size_t)y * (size_t)stride;
        int zlc = 0, ztc = 0;
        int lf = 1;
        for (int x = 0; x < w; x++) {
            unsigned int pinhead = src[x] & 0xFF;
            if (lf) {
                if (zlc != kDmaMaxRunPixels) {
                    if (!pinhead) zlc++;
                    else lf = 0;
                } else {
                    lf = 0;
                }
            } else if (x > w - kDmaMaxRunPixels) {
                /* Only the tail window is watched, and any opaque pixel resets
                   the count, so this ends up holding the real trailing run. */
                if (!pinhead) ztc++;
                else ztc = 0;
            }
        }
        for (int k = 0; k < 4; k++) {
            int factor0 = 1 << k;
            int factor1 = zlc / factor0;
            if (factor1 > 15) factor1 = 15;
            leftover[k].lead += zlc - factor1 * factor0;

            factor1 = ztc / factor0;
            if (factor1 > 15) factor1 = 15;
            leftover[k].trail += ztc - factor1 * factor0;
        }
        line_lead[(size_t)y] = zlc;
        line_trail[(size_t)y] = ztc;
    }

    /* Pick the unit size that wastes the least, lead and trail independently.
       Strict < means ties keep the smaller unit, as in the original. */
    int lm = 0, tm = 0;
    for (int k = 1; k < 4; k++) {
        if (leftover[k].lead < leftover[lm].lead) lm = k;
        if (leftover[k].trail < leftover[tm].trail) tm = k;
    }

    int ctrl_word = ((bpp & 7) << 12) | (tm << 10) | (lm << 8) | (1 << 7);
    const int lf_shift = lm, tf_shift = tm;
    const int lead_unit = 1 << lf_shift;
    const int trail_unit = 1 << tf_shift;

    /* Pass 2: quantize each line to those units and total the encoded size. */
    unsigned long imgsize = 0;
    for (int y = 0; y < h; y++) {
        int zlc = line_lead[(size_t)y];
        int ztc = line_trail[(size_t)y];

        if (zlc > lead_unit * 15) zlc = 15;
        else                      zlc = zlc / lead_unit;
        if (ztc > trail_unit * 15) ztc = 15;
        else                       ztc = ztc / trail_unit;

        int x0 = zlc * lead_unit;
        int x1 = w - 1 - ztc * trail_unit;
        if ((x1 - x0 + 1) < kDmaZcomPixels) {
            /* Give pixels back until the line keeps its minimum span, leading
               edge first, then trailing. */
            int zeros = kDmaZcomPixels - (x1 - x0 + 1);
            if (zeros > x0) zeros -= x0;
            else            x0 = zeros;
            zlc = (zlc * lead_unit - x0) / lead_unit;
            x0 = zlc * lead_unit;
            if ((x1 - x0 + 1) < kDmaZcomPixels) {
                ztc = (ztc * trail_unit - zeros) / trail_unit;
                x1 = w - 1 - ztc * trail_unit;
            }
        }
        /* This is exactly x1 - x0 + 1, the span the line keeps. The original
           computes it in signed ints and accumulates into an unsigned total,
           so a pathological line that gave back more than it had would wrap to
           a nonsense size; clamp rather than report that. */
        int kept = w - (ztc << tf_shift) - (zlc << lf_shift);
        if (kept < 0) kept = 0;
        if (kept > w) kept = w;
        imgsize += (unsigned long)kept * (unsigned long)bpp;
    }
    imgsize += (unsigned long)h << 3;  /* one lead/trail byte per line */

    if (out.raw_bits < imgsize) {
        /* Compression lost. The original zeroes the control word and keeps the
           uncompressed size; the caller's dma2_field (bpp only) stands. */
        out.skip_reason = "compressed size exceeds raw";
        return out;
    }

    out.packed_bits = imgsize;
    out.control_word = ctrl_word;
    out.compressed = true;
    out.lead_factor = lead_unit;
    out.trail_factor = trail_unit;
    return out;
}
