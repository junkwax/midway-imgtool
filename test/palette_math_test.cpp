/*************************************************************
 * test/palette_math_test.cpp
 *
 * Unit coverage for the pure 15-bit palette-word helpers extracted from
 * imgui_overlay.cpp into platform/palette_math.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "palette_math.h"

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* Pack 5/5/5 components into a 15-bit word. */
static unsigned short mkword(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

/* Write `n` words little-endian into a 2-bytes-per-entry buffer. */
static void fill_words(unsigned char *buf, const unsigned short *words, int n)
{
    for (int i = 0; i < n; i++) {
        buf[i * 2 + 0] = (unsigned char)(words[i] & 0xFF);
        buf[i * 2 + 1] = (unsigned char)(words[i] >> 8);
    }
}

int main(void)
{
    /* palette_word_at reads little-endian packed words. */
    {
        unsigned char buf[4] = { 0x34, 0x12, 0xFF, 0x7F };
        CHECK(palette_word_at(buf, 0) == 0x1234);
        CHECK(palette_word_at(buf, 1) == 0x7FFF);
    }

    /* palette_word_distance_sq == PaletteColorDistance5 (same formula), and
       both are 0 for identical colors and the channel-sum-of-squares else. */
    {
        unsigned short a = mkword(10, 20, 5);
        unsigned short b = mkword(13, 16, 5);   /* dr=-3, dg=+4, db=0 */
        CHECK(palette_word_distance_sq(a, a) == 0);
        CHECK(palette_word_distance_sq(a, b) == 9 + 16 + 0);
        CHECK(PaletteColorDistance5(a, b) == palette_word_distance_sq(a, b));
    }

    /* Perceptual weighting favors green (59) over red (30) over blue (11). */
    {
        unsigned short base  = mkword(10, 10, 10);
        unsigned short red   = mkword(15, 10, 10);  /* dr = 5 */
        unsigned short green = mkword(10, 15, 10);  /* dg = 5 */
        unsigned short blue  = mkword(10, 10, 15);  /* db = 5 */
        /* Unweighted: all three are equal distance. */
        CHECK(PaletteColorDistance5W(base, red,   false) ==
              PaletteColorDistance5W(base, green, false));
        /* Weighted: green farthest, blue nearest. */
        CHECK(PaletteColorDistance5W(base, green, true) >
              PaletteColorDistance5W(base, red,   true));
        CHECK(PaletteColorDistance5W(base, red,   true) >
              PaletteColorDistance5W(base, blue,  true));
    }

    /* pal_word_or_black: valid index returns the word; out-of-range / null
       returns 0. */
    {
        unsigned short words[3] = { mkword(1,2,3), mkword(31,0,0), mkword(0,0,31) };
        unsigned char data[6];
        fill_words(data, words, 3);
        PAL pal {};
        pal.data_p = data;
        pal.numc = 3;
        CHECK(pal_word_or_black(&pal, 0) == words[0]);
        CHECK(pal_word_or_black(&pal, 2) == words[2]);
        CHECK(pal_word_or_black(&pal, 3) == 0);   /* >= numc */
        CHECK(pal_word_or_black(&pal, -1) == 0);
        CHECK(pal_word_or_black(nullptr, 0) == 0);
    }

    /* nearest_palette_index_for_word / FindNearestPaletteSlot: index 0 is
       transparent and never matched; nearest non-zero slot wins. */
    {
        /* slot0=black, slot1=pure red, slot2=pure green, slot3=pure blue */
        unsigned short words[4] = {
            mkword(0,0,0), mkword(31,0,0), mkword(0,31,0), mkword(0,0,31)
        };
        unsigned char data[8];
        fill_words(data, words, 4);
        PAL pal {};
        pal.data_p = data;
        pal.numc = 4;

        unsigned short nearly_green = mkword(2, 29, 1);
        CHECK(nearest_palette_index_for_word(nearly_green, &pal) == 2);
        CHECK(FindNearestPaletteSlot(&pal, nearly_green) == 2);

        unsigned short nearly_red = mkword(30, 1, 0);
        CHECK(FindNearestPaletteSlot(&pal, nearly_red) == 1);

        /* Empty/degenerate palettes return 0. */
        PAL empty {};
        CHECK(nearest_palette_index_for_word(nearly_red, &empty) == 0);
        CHECK(FindNearestPaletteSlot(&empty, nearly_red) == 0);
    }

    /* FindNearestMergedSlot: searches existing target colors [1..base) plus
       queued additions, returning a final-space index. */
    {
        /* target: slot0 black, slot1 red */
        unsigned short twords[2] = { mkword(0,0,0), mkword(31,0,0) };
        unsigned char tdata[4];
        fill_words(tdata, twords, 2);
        PAL target {};
        target.data_p = tdata;
        target.numc = 2;

        unsigned short added[2] = { mkword(0,31,0), mkword(0,0,31) }; /* green, blue */
        const int base_count = 2;

        /* An exact existing color resolves to its base slot. */
        CHECK(FindNearestMergedSlot(&target, base_count, added, 2,
                                    mkword(31,0,0), false) == 1);
        /* A color matching a queued addition resolves to base_count + j. */
        CHECK(FindNearestMergedSlot(&target, base_count, added, 2,
                                    mkword(0,31,0), false) == base_count + 0);
        CHECK(FindNearestMergedSlot(&target, base_count, added, 2,
                                    mkword(0,0,31), false) == base_count + 1);
    }

    if (g_fails == 0) {
        std::printf("PASS: palette_math helpers behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d palette_math check(s)\n", g_fails);
    return 1;
}
