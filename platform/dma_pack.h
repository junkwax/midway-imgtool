/*************************************************************
 * platform/dma_pack.h
 *
 * Model of what LOAD2 will actually do to a sprite when it packages it into
 * ROM, so the editor can report the real cost instead of approximating it.
 *
 * This is a port of the shipping DOS toolchain, not a reimplementation:
 * DmaZeroCompress() follows doc/load2/zcom.c's zcom_analysis() statement for
 * statement, including its clamps and its quirks, and DmaSuperBpp() follows
 * the do_superbpp scan in doc/load2/load2.c. Numbers that disagree with the
 * real packager are worse than no numbers at all, so where the original is
 * odd this code is odd in the same way — see the comments at each site.
 *
 * Pure logic: no UI globals, no SDL, unit-tested in test/dma_pack_test.cpp.
 *************************************************************/
#pragma once

/* Minimum non-zero pixels a compressed line must retain (load2.h ZCOMPIXELS).
   Also the width below which an image is not zero-compressed at all. */
enum { kDmaZcomPixels = 10 };

/* zcom_analysis() records lead/trail per line into a fixed zero_array[256]
   (load2.h), so taller art silently walks off the end of that table in the
   real tool. We refuse instead, and say so. */
enum { kDmaZcomMaxRows = 256 };

struct DmaPackResult {
    int  bpp = 8;                  /* bits per pixel the data is packed at */
    unsigned long raw_bits = 0;    /* w * h * bpp, before zero compression */
    unsigned long packed_bits = 0; /* after compression; == raw_bits if unused */
    int  control_word = 0;         /* DMA control word LOAD2 would emit */
    bool compressed = false;       /* zero compression applied and it won */
    int  lead_factor = 1;          /* leading run unit: 1, 2, 4 or 8 pixels */
    int  trail_factor = 1;         /* trailing run unit: 1, 2, 4 or 8 pixels */

    /* Why compression did not happen, when it didn't. Empty when it did, or
       when it was never requested. */
    const char *skip_reason = nullptr;
};

/* Bits per pixel LOAD2's auto-packer (do_superbpp) would choose, driven by the
   largest index actually present rather than the palette's declared depth.
   `palette_bpp` is the fallback, used only when the art needs all 8 bits.
   Fully transparent art comes back 2bpp — a quirk of the original's ladder
   that this reproduces rather than corrects. */
int DmaSuperBpp(const unsigned char *pixels, int w, int h, int stride,
                int palette_bpp);

/* Model LOAD2's packaging of one sprite.
 *
 * `stride` must be the row pitch of `pixels`; LOAD2 rounds every row up to a
 * multiple of 4 (round_x in wmpstruc.h), which is the same pitch imgtool
 * stores. `bpp` is the depth the data will be packed at — normally
 * DmaSuperBpp() or the palette's declared depth, matching PPP>.
 *
 * `zero_compress` mirrors the ZON>/ZOF> directive. Compression is dropped
 * automatically when it would not pay, exactly as the original does by
 * comparing against the uncompressed size and returning a zero control word.
 */
DmaPackResult DmaAnalyzeSprite(const unsigned char *pixels, int w, int h,
                               int stride, int bpp, bool zero_compress);

/* Bits -> bytes the way LOAD2 reports them: (bits + 7) >> 3. */
unsigned long DmaBitsToBytes(unsigned long bits);
