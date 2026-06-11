/*************************************************************
 * test/color_ops_test.cpp
 *
 * Unit coverage for the pure HSL palette adjustment extracted from
 * imgui_overlay.cpp into platform/color_ops.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "color_ops.h"
#include "img_format.h"   /* pal_word_to_rgb8 for decoding outputs */

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned short mkword(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

static void put_word(unsigned char *buf, int i, unsigned short w)
{
    buf[i * 2 + 0] = (unsigned char)(w & 0xFF);
    buf[i * 2 + 1] = (unsigned char)(w >> 8);
}

static unsigned short get_word(const unsigned char *buf, int i)
{
    return (unsigned short)(buf[i * 2] | (buf[i * 2 + 1] << 8));
}

int main(void)
{
    /* A small baseline: red, green, mid-gray, blue. */
    const int N = 4;
    unsigned char baseline[N * 2];
    put_word(baseline, 0, mkword(31, 0, 0));
    put_word(baseline, 1, mkword(0, 31, 0));
    put_word(baseline, 2, mkword(15, 15, 15));
    put_word(baseline, 3, mkword(0, 0, 31));

    /* light_pct = -100 drives every adjusted color to black (l -> 0). */
    {
        unsigned char out[N * 2];
        std::memset(out, 0xAB, sizeof(out));
        HslAdjustPaletteWordsFromBaseline(baseline, N, nullptr,
                                          0, 0, -100, out, nullptr);
        for (int i = 0; i < N; i++)
            CHECK(get_word(out, i) == 0);
    }

    /* light_pct = +100 drives every adjusted color to white (l -> 1). */
    {
        unsigned char out[N * 2];
        HslAdjustPaletteWordsFromBaseline(baseline, N, nullptr,
                                          0, 0, 100, out, nullptr);
        for (int i = 0; i < N; i++)
            CHECK(get_word(out, i) == mkword(31, 31, 31));
    }

    /* Selection mask: only selected entries are adjusted; unselected entries
       are copied byte-exact from the baseline. */
    {
        bool mask[N] = { false, true, false, false };  /* only the green slot */
        unsigned char out[N * 2];
        std::memset(out, 0x00, sizeof(out));
        HslAdjustPaletteWordsFromBaseline(baseline, N, mask,
                                          0, 0, -100, out, nullptr);
        /* Unselected slots identical to baseline. */
        CHECK(get_word(out, 0) == get_word(baseline, 0));
        CHECK(get_word(out, 2) == get_word(baseline, 2));
        CHECK(get_word(out, 3) == get_word(baseline, 3));
        /* The selected green slot was darkened to black. */
        CHECK(get_word(out, 1) == 0);
    }

    /* out_rgb is full 8-bit and out_words is its 5-bit-quantized form, so they
       are not bit-identical after decode (e.g. 200 -> r5 25 -> 206). The exact
       invariant is that quantizing out_rgb reproduces out_words. */
    {
        unsigned char out_words[N * 2];
        unsigned char out_rgb[N * 3];
        HslAdjustPaletteWordsFromBaseline(baseline, N, nullptr,
                                          40, 10, 5, out_words, out_rgb);
        for (int i = 0; i < N; i++) {
            unsigned char w2[2];
            rgb8_to_pal_word(out_rgb[i * 3 + 0], out_rgb[i * 3 + 1],
                             out_rgb[i * 3 + 2], w2);
            CHECK(w2[0] == out_words[i * 2 + 0]);
            CHECK(w2[1] == out_words[i * 2 + 1]);
        }
    }

    /* Guard rails: null baseline / non-positive count are no-ops (no crash). */
    {
        unsigned char out[2] = { 0x11, 0x22 };
        HslAdjustPaletteWordsFromBaseline(nullptr, N, nullptr, 0, 0, 0, out, nullptr);
        HslAdjustPaletteWordsFromBaseline(baseline, 0, nullptr, 0, 0, 0, out, nullptr);
        CHECK(out[0] == 0x11 && out[1] == 0x22);
    }

    if (g_fails == 0) {
        std::printf("PASS: color_ops HSL adjustment behaves as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d color_ops check(s)\n", g_fails);
    return 1;
}
