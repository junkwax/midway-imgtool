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


/* ---- PlanPaletteIsolation ----
   The Baraka-blades case: a region owns some indices, other parts of the
   frame draw with the same ones, and the plan has to hand those other parts
   somewhere else to point without changing how the frame looks. */
static void iso_clear(bool *m) { for (int i = 0; i < 256; i++) m[i] = false; }

static void isolation_checks(void)
{
    /* A four-shade silver ramp at 1..4, a tan ramp at 5..7, one dead slot at
       8, and slot 9 an exact duplicate of silver index 2. */
    unsigned short words[10] = {
        mkword(0, 0, 0),      /* 0 transparent */
        mkword(8, 8, 9),      /* 1 silver */
        mkword(16, 16, 17),   /* 2 silver */
        mkword(24, 24, 25),   /* 3 silver */
        mkword(31, 31, 31),   /* 4 silver */
        mkword(20, 12, 4),    /* 5 tan */
        mkword(26, 18, 8),    /* 6 tan */
        mkword(31, 24, 14),   /* 7 tan */
        mkword(3, 0, 0),      /* 8 dead */
        mkword(16, 16, 17),   /* 9 duplicate of 2 */
    };
    unsigned char buf[20];
    fill_words(buf, words, 10);

    bool reserved[256], contested[256], live[256];

    /* Blades own silver 1..4; teeth outside also draw 2 and 3. Slot 8 is dead,
       slot 9 is live and already holds silver-2's exact color. */
    {
        iso_clear(reserved); iso_clear(contested); iso_clear(live);
        for (int i = 1; i <= 4; i++) reserved[i] = true;
        contested[2] = contested[3] = true;
        for (int i = 1; i <= 7; i++) live[i] = true;
        live[9] = true;   /* 8 stays dead */

        PaletteIsolatePlan p;
        PlanPaletteIsolation(buf, 10, reserved, contested, live, 256, &p);

        /* Index 2 costs nothing: slot 9 already holds that exact color. */
        CHECK(p.dest[2] == 9);
        CHECK(p.copy_from[9] == -1);
        /* Index 3 has no twin, so it recycles the one slot nothing draws. */
        CHECK(p.dest[3] == 8);
        CHECK(p.copy_from[8] == 3);
        /* Uncontested indices are left strictly alone. */
        CHECK(p.dest[1] == -1 && p.dest[4] == -1 && p.dest[5] == -1);
        CHECK(p.matched == 1 && p.recycled == 1);
        CHECK(p.appended == 0 && p.approximated == 0);
        CHECK(p.numc == 10);   /* nothing had to be appended */
        /* Destinations must never land on an index the region owns. */
        for (int i = 1; i < 10; i++)
            CHECK(p.dest[i] < 0 || !reserved[p.dest[i]]);
    }

    /* Same ramp with every slot live: no twin, nothing to recycle, so the
       plan appends duplicates past the end and reports the new size. */
    {
        iso_clear(reserved); iso_clear(contested); iso_clear(live);
        for (int i = 1; i <= 4; i++) { reserved[i] = true; contested[i] = true; }
        for (int i = 1; i < 9; i++) live[i] = true;

        PaletteIsolatePlan p;
        PlanPaletteIsolation(buf, 9, reserved, contested, live, 256, &p);

        CHECK(p.appended == 4);
        CHECK(p.recycled == 0 && p.approximated == 0);
        CHECK(p.numc == 13);
        for (int c = 1; c <= 4; c++) {
            CHECK(p.dest[c] >= 9 && p.dest[c] < 13);
            CHECK(p.copy_from[p.dest[c]] == c);
        }
        /* Appended slots are distinct, so the four shades stay four shades. */
        CHECK(p.dest[1] != p.dest[2] && p.dest[2] != p.dest[3] && p.dest[3] != p.dest[4]);
    }

    /* Two contested indices sharing one color collapse onto a single
       duplicate: the second finds the first's new slot as an exact match. */
    {
        unsigned short dup[6] = {
            mkword(0, 0, 0), mkword(9, 9, 9), mkword(9, 9, 9),
            mkword(31, 0, 0), mkword(0, 31, 0), mkword(0, 0, 31),
        };
        unsigned char dbuf[12];
        fill_words(dbuf, dup, 6);

        iso_clear(reserved); iso_clear(contested); iso_clear(live);
        reserved[1] = reserved[2] = true;
        contested[1] = contested[2] = true;
        for (int i = 1; i < 6; i++) live[i] = true;

        PaletteIsolatePlan p;
        PlanPaletteIsolation(dbuf, 6, reserved, contested, live, 256, &p);

        CHECK(p.appended == 1);
        CHECK(p.matched == 1);
        CHECK(p.numc == 7);
        CHECK(p.dest[1] == 6 && p.dest[2] == 6);
        CHECK(p.copy_from[6] == 1);
    }

    /* A full palette cannot grow, so the last resort is the nearest color the
       region does not own -- the one case where the art changes. */
    {
        iso_clear(reserved); iso_clear(contested); iso_clear(live);
        reserved[1] = contested[1] = true;
        for (int i = 1; i < 10; i++) live[i] = true;

        PaletteIsolatePlan p;
        PlanPaletteIsolation(buf, 10, reserved, contested, live, 10, &p);

        /* Silver-1 is darkest; slot 8 (near-black) is the closest colour the
           region does not own, and 9 is silver-2's twin but reserved-free. */
        CHECK(p.approximated == 1);
        CHECK(p.numc == 10);
        CHECK(p.dest[1] >= 1 && p.dest[1] < 10);
        CHECK(!reserved[p.dest[1]]);
    }

    /* Nothing contested means nothing planned. */
    {
        iso_clear(reserved); iso_clear(contested); iso_clear(live);
        reserved[1] = reserved[2] = true;
        for (int i = 1; i < 10; i++) live[i] = true;

        PaletteIsolatePlan p;
        PlanPaletteIsolation(buf, 10, reserved, contested, live, 256, &p);

        CHECK(p.numc == 10);
        CHECK(p.matched == 0 && p.recycled == 0);
        CHECK(p.appended == 0 && p.approximated == 0);
        for (int i = 0; i < 256; i++) CHECK(p.dest[i] == -1);
    }

    /* Degenerate inputs must not reach past the plan. */
    {
        PaletteIsolatePlan p;
        iso_clear(reserved); iso_clear(contested); iso_clear(live);
        PlanPaletteIsolation(NULL, 10, reserved, contested, live, 256, &p);
        CHECK(p.numc == 0 && p.dest[1] == -1);
        PlanPaletteIsolation(buf, 0, reserved, contested, live, 256, &p);
        CHECK(p.numc == 0);
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

    /* ---- Palette depth from color count ---- */
    {
        /* Exact powers of two are satisfied by their own width: 16 colors are
           indices 0..15, which fit in 4 bits. */
        CHECK(PaletteBppForColorCount(2) == 1);
        CHECK(PaletteBppForColorCount(4) == 2);
        CHECK(PaletteBppForColorCount(8) == 3);
        CHECK(PaletteBppForColorCount(16) == 4);
        CHECK(PaletteBppForColorCount(32) == 5);
        CHECK(PaletteBppForColorCount(64) == 6);
        CHECK(PaletteBppForColorCount(128) == 7);
        CHECK(PaletteBppForColorCount(256) == 8);

        /* One past a power of two needs the next bit. */
        CHECK(PaletteBppForColorCount(17) == 5);
        CHECK(PaletteBppForColorCount(33) == 6);
        CHECK(PaletteBppForColorCount(65) == 7);
        CHECK(PaletteBppForColorCount(129) == 8);

        /* The case that motivated this: a quantized import landing on 41
           colors is 6bpp art, not 8. */
        CHECK(PaletteBppForColorCount(41) == 6);

        /* Degenerate and out-of-range counts stay in [1, 8]. */
        CHECK(PaletteBppForColorCount(0) == 1);
        CHECK(PaletteBppForColorCount(1) == 1);
        CHECK(PaletteBppForColorCount(-5) == 1);
        CHECK(PaletteBppForColorCount(300) == 8);

        CHECK(PaletteColorCountForBpp(1) == 2);
        CHECK(PaletteColorCountForBpp(4) == 16);
        CHECK(PaletteColorCountForBpp(8) == 256);
        CHECK(PaletteColorCountForBpp(0) == 2);     /* clamped up */
        CHECK(PaletteColorCountForBpp(99) == 256);  /* clamped down */

        /* Round-tripping any count gives a depth that can address it. */
        for (int n = 1; n <= 256; n++)
            CHECK(PaletteColorCountForBpp(PaletteBppForColorCount(n)) >= n);

        /* Too-small detection drives the growth guard. */
        CHECK(PaletteBppTooSmall(4, 200));      /* 4bpp addresses 16 */
        CHECK(!PaletteBppTooSmall(8, 200));
        CHECK(!PaletteBppTooSmall(4, 16));      /* exactly fits */
        CHECK(PaletteBppTooSmall(4, 17));
        CHECK(!PaletteBppTooSmall(8, 0));       /* no colors, nothing to fit */
        CHECK(PaletteBppTooSmall(0, 4));        /* nonsense depth is too small */
        CHECK(PaletteBppTooSmall(9, 4));
    }

    /* ---- PaletteToleranceDistSq / PaletteIndexWithinTolerance ----
       The bug these replace: the wand, bucket, eraser and remap all compared
       palette INDEX numbers, so grabbing a black background at any tolerance
       above 0 also grabbed whatever unrelated colors sat in the adjacent
       slots — usually the sprite's own dark shading. */
    {
        /* 0 transparent (stores black, as palettes almost always do),
           1 and 2 exact-duplicate blacks the background is split across,
           3 an all-but-black, 4 a mid grey, 5 pure red — the last two sit
           right next to the blacks in index order and nowhere near them in
           color, which is exactly what index arithmetic got wrong. */
        unsigned short words[6] = {
            mkword(0, 0, 0),      /* 0 transparent */
            mkword(0, 0, 0),      /* 1 black */
            mkword(0, 0, 0),      /* 2 black, duplicate slot */
            mkword(1, 0, 1),      /* 3 all-but-black */
            mkword(16, 16, 16),   /* 4 mid grey */
            mkword(31, 0, 0),     /* 5 red */
        };
        unsigned char data[12];
        fill_words(data, words, 6);
        PAL pal {};
        pal.data_p = data;
        pal.numc = 6;

        /* The slider is a radius at half scale. */
        CHECK(PaletteToleranceDistSq(0) == 0);
        CHECK(PaletteToleranceDistSq(2) == 1);    /* radius 1 */
        CHECK(PaletteToleranceDistSq(16) == 64);  /* radius 8 */
        CHECK(PaletteToleranceDistSq(-4) == 0);   /* clamped */

        /* A background split across duplicate black slots is caught whole at
           tolerance 0 — needing to raise the tolerance for this is what used
           to drag the sprite in. */
        CHECK(PaletteIndexWithinTolerance(&pal, 1, 2, 0));
        CHECK(PaletteIndexWithinTolerance(&pal, 2, 1, 0));

        /* Tolerance 0 still means exactly this color: the all-but-black slot
           is 2 away and needs the slider off zero. */
        CHECK(!PaletteIndexWithinTolerance(&pal, 1, 3, 0));
        CHECK(PaletteIndexWithinTolerance(&pal, 1, 3, 4));

        /* The neighbouring-index grey and red are not matched at anything the
           0..16 sliders can produce, nor at half the wand's range. Under the
           old index compare both fell inside a tolerance of 4. */
        CHECK(!PaletteIndexWithinTolerance(&pal, 1, 4, 16));
        CHECK(!PaletteIndexWithinTolerance(&pal, 1, 5, 16));
        CHECK(!PaletteIndexWithinTolerance(&pal, 1, 4, 32));
        CHECK(!PaletteIndexWithinTolerance(&pal, 1, 5, 32));

        /* Widening does still reach: the wand's top end is deliberately
           near-universal, the way a maxed tolerance is everywhere else. */
        CHECK(PaletteIndexWithinTolerance(&pal, 1, 4, 64));

        /* Transparent never matches opaque, however wide the tolerance and
           however identical the stored colors — slots 0 and 1 are both black
           here. This is what stopped the wand eating a sprite's outline when
           asked for its transparent background. */
        CHECK(!PaletteIndexWithinTolerance(&pal, 0, 1, 64));
        CHECK(!PaletteIndexWithinTolerance(&pal, 1, 0, 64));
        CHECK(PaletteIndexWithinTolerance(&pal, 0, 0, 0));

        /* No palette: fall back to the old index comparison rather than
           matching everything — but keep the transparency guard. */
        CHECK(PaletteIndexWithinTolerance(nullptr, 4, 6, 2));
        CHECK(!PaletteIndexWithinTolerance(nullptr, 4, 8, 2));
        CHECK(!PaletteIndexWithinTolerance(nullptr, 0, 1, 64));
    }

    isolation_checks();

    if (g_fails == 0) {
        std::printf("PASS: palette_math helpers behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d palette_math check(s)\n", g_fails);
    return 1;
}
