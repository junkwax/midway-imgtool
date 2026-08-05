/*************************************************************
 * test/dma_pack_test.cpp
 *
 * Unit coverage for the LOAD2 packaging model in platform/dma_pack.cpp.
 * Expected values are derived by hand from doc/load2/zcom.c so the test fails
 * if the port drifts from the shipping tool, not merely if it changes.
 * No UI/SDL dependencies.
 *************************************************************/
#include "dma_pack.h"

#include <cstdio>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static int stride_for(int w) { return (w + 3) & ~3; }

/* Build a w*h indexed buffer where each row has `lead` zeros, then `body`
   pixels of index `ci`, then zeros to the end. */
static std::vector<unsigned char> make_rows(int w, int h, int lead, int body,
                                            unsigned char ci)
{
    std::vector<unsigned char> buf((size_t)stride_for(w) * (size_t)h, 0);
    for (int y = 0; y < h; y++)
        for (int x = lead; x < lead + body && x < w; x++)
            buf[(size_t)y * stride_for(w) + x] = ci;
    return buf;
}

int main(void)
{
    /* ---- DmaSuperBpp: the largest index present picks the depth ---- */
    {
        const int w = 8, h = 2, stride = stride_for(w);
        std::vector<unsigned char> buf((size_t)stride * h, 0);

        /* All transparent: max is 0, which misses `== 1` and lands on `< 4`,
           so LOAD2 packs empty art at 2bpp. Quirk, faithfully reproduced. */
        CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 2);

        buf[0] = 1;  CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 1);
        buf[0] = 3;  CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 2);
        buf[0] = 7;  CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 3);
        buf[0] = 15; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 4);
        buf[0] = 31; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 5);
        buf[0] = 63; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 6);
        buf[0] = 127; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 7);
        /* 128+ needs all 8 bits: falls through to the palette's depth. */
        buf[0] = 128; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 8) == 8);

        /* A 41-color palette is 6bpp art, which is the lever that matters:
           getting under 32 colors buys a whole bit per pixel. */
        buf[0] = 40; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 6);
        buf[0] = 30; CHECK(DmaSuperBpp(buf.data(), w, h, stride, 6) == 5);
    }

    /* ---- Raw sizing and the ZOF> path ---- */
    {
        const int w = 32, h = 16;
        std::vector<unsigned char> buf = make_rows(w, h, 0, w, 5);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 6, false);
        CHECK(r.bpp == 6);
        CHECK(r.raw_bits == (unsigned long)w * h * 6);
        CHECK(r.packed_bits == r.raw_bits);
        CHECK(!r.compressed);
        /* Uncompressed control word carries bpp only — no compression bit. */
        CHECK(r.control_word == (6 << 12));
        CHECK(r.skip_reason != nullptr);
    }

    /* ---- Too narrow to compress (w <= ZCOMPIXELS) ---- */
    {
        const int w = 10, h = 8;
        std::vector<unsigned char> buf = make_rows(w, h, 0, w, 3);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 4, true);
        CHECK(!r.compressed);
        CHECK(r.packed_bits == r.raw_bits);
        CHECK(r.control_word == (4 << 12));
    }

    /* ---- Wide margins: compression wins, and both clamps bite ----
       64 wide, 16 rows, 8 opaque pixels at x=28..35, so lead and trail are
       both 28. A field encodes at most 15 units, which is what picks the unit
       size: unit 1 can only describe 15 of the 28 (wasting 13), unit 2 covers
       it exactly in 14 units (wasting 0), unit 4 also wastes 0 but loses the
       strict-< tie-break, unit 8 wastes 4. So both factors come out 2.

       Then the ZCOMPIXELS floor bites: 28 + 8 + 28 leaves a span of 8, under
       the 10 a compressed line must keep, so 2 pixels are handed back to the
       leading run (14 units -> 13) and each line keeps 10. */
    {
        const int w = 64, h = 16;
        std::vector<unsigned char> buf = make_rows(w, h, 28, 8, 9);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 6, true);
        CHECK(r.compressed);
        CHECK(r.lead_factor == 2);
        CHECK(r.trail_factor == 2);
        CHECK(r.packed_bits == (unsigned long)h * (kDmaZcomPixels * 6 + 8));
        CHECK(r.packed_bits < r.raw_bits);
        /* Control word: bpp | trail shift | lead shift | compressed bit. */
        CHECK(r.control_word == ((6 << 12) | (1 << 10) | (1 << 8) | (1 << 7)));
        CHECK(r.skip_reason == nullptr);
    }

    /* ---- Runs longer than one unit can encode force a bigger unit ----
       120 leading zeros is 15 units of 8, the widest a field encodes. With
       unit 1, 2 or 4 the run cannot be fully described and the leftovers
       accumulate, so the analysis should settle on unit 8. */
    {
        const int w = 140, h = 4;
        std::vector<unsigned char> buf = make_rows(w, h, 120, 20, 2);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 6, true);
        CHECK(r.compressed);
        CHECK(r.lead_factor == 8);
        /* Lead fully encoded: 15 units * 8 = 120 skipped, 20 pixels kept. */
        CHECK(r.packed_bits == (unsigned long)h * (20 * 6 + 8));
    }

    /* ---- Fully opaque art: nothing to strip, header makes it bigger ----
       Every line keeps all w pixels and pays 8 extra bits, so the compressed
       size exceeds raw and LOAD2 drops the control word. */
    {
        const int w = 48, h = 12;
        std::vector<unsigned char> buf = make_rows(w, h, 0, w, 7);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 6, true);
        CHECK(!r.compressed);
        CHECK(r.packed_bits == r.raw_bits);
        CHECK(r.control_word == (6 << 12));
        CHECK(r.skip_reason != nullptr);
    }

    /* ---- The ZCOMPIXELS floor: a line may not shrink below 10 kept pixels ----
       A single opaque pixel per line would leave a span of 1, so the clamp
       hands pixels back until the line keeps at least 10. */
    {
        const int w = 64, h = 8;
        std::vector<unsigned char> buf = make_rows(w, h, 30, 1, 4);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 6, true);
        CHECK(r.compressed);
        unsigned long per_line_bits = (r.packed_bits / h) - 8;
        CHECK(per_line_bits >= (unsigned long)(kDmaZcomPixels * 6));
    }

    /* ---- Taller than LOAD2's zero_array[256] is refused, not guessed ---- */
    {
        const int w = 32, h = 300;
        std::vector<unsigned char> buf = make_rows(w, h, 4, 8, 1);
        DmaPackResult r = DmaAnalyzeSprite(buf.data(), w, h, stride_for(w), 6, true);
        CHECK(!r.compressed);
        CHECK(r.skip_reason != nullptr);
    }

    /* ---- Degenerate inputs don't crash or report nonsense ---- */
    {
        DmaPackResult r = DmaAnalyzeSprite(nullptr, 8, 8, 8, 6, true);
        CHECK(!r.compressed);
        CHECK(r.raw_bits == 0);

        std::vector<unsigned char> buf(64, 0);
        /* stride < w is rejected. */
        r = DmaAnalyzeSprite(buf.data(), 16, 4, 8, 6, true);
        CHECK(r.raw_bits == 0);
    }

    /* ---- Byte reporting rounds up, as LOAD2 prints it ---- */
    {
        CHECK(DmaBitsToBytes(0) == 0);
        CHECK(DmaBitsToBytes(1) == 1);
        CHECK(DmaBitsToBytes(8) == 1);
        CHECK(DmaBitsToBytes(9) == 2);
    }

    if (g_fails == 0) std::printf("dma_pack_test: all checks passed\n");
    return g_fails ? 1 : 0;
}
