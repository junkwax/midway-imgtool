/*************************************************************
 * test/ramp_remap_test.cpp
 *
 * Unit coverage for the palette assembly and luminance remap in
 * platform/ramp_remap.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "ramp_remap.h"

#include <cstdio>
#include <cstring>
#include <initializer_list>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static MaterialRamp mkramp(unsigned char id, std::initializer_list<RampColor> colors)
{
    MaterialRamp r{};
    r.material_id = id;
    r.count = 0;
    for (RampColor c : colors) r.colors[r.count++] = c;
    return r;
}

/* ---- AssemblePalette ---- */

static void assembles_spans_back_to_back_after_the_transparent_slot(void)
{
    MaterialRamp ramps[2] = {
        mkramp(1, { {0,0,0}, {15,15,15}, {31,31,31} }),
        mkramp(2, { {31,0,0}, {16,0,0} }),
    };
    AssembledPalette out;
    int assembled = AssemblePalette(ramps, 2, &out);
    CHECK(assembled == 2);
    CHECK(out.numc == 6);
    CHECK(out.words[0] == 0);
    CHECK(out.words[1] == RampColorWord({0,0,0}));
    CHECK(out.words[2] == RampColorWord({15,15,15}));
    CHECK(out.words[3] == RampColorWord({31,31,31}));
    CHECK(out.words[4] == RampColorWord({31,0,0}));
    CHECK(out.words[5] == RampColorWord({16,0,0}));

    CHECK(out.span_count == 2);
    CHECK(out.spans[0].material_id == 1 && out.spans[0].start == 1 && out.spans[0].count == 3);
    CHECK(out.spans[1].material_id == 2 && out.spans[1].start == 4 && out.spans[1].count == 2);
}

static void skips_a_ramp_that_overflows_but_keeps_trying_later_ones(void)
{
    /* 1 (transparent) + 64 + 64 + 64 = 193, fits. A fourth 64-color ramp
       would push to 257 > 256 and must be skipped -- but a fifth, smaller
       ramp that still fits on its own must not be skipped just because an
       earlier one was. */
    RampColor fill[64];
    for (int i = 0; i < 64; i++) fill[i] = RampColor{ (unsigned char)(i % 32), 1, 1 };

    MaterialRamp ramps[5];
    for (int i = 0; i < 4; i++) {
        ramps[i].material_id = (unsigned char)(i + 1);
        ramps[i].count = 64;
        std::memcpy(ramps[i].colors, fill, sizeof(fill));
    }
    ramps[4].material_id = 5;
    ramps[4].count = 10;
    for (int i = 0; i < 10; i++) ramps[4].colors[i] = RampColor{ (unsigned char)i, 2, 2 };

    AssembledPalette out;
    int assembled = AssemblePalette(ramps, 5, &out);
    CHECK(assembled == 4);          /* ramps 1,2,3 and 5 -- ramp 4 (index 3) skipped */
    CHECK(out.span_count == 4);
    CHECK(out.spans[0].material_id == 1);
    CHECK(out.spans[1].material_id == 2);
    CHECK(out.spans[2].material_id == 3);
    CHECK(out.spans[3].material_id == 5);   /* the skipped id (4) is absent */
    CHECK(out.numc == 1 + 64 + 64 + 64 + 10);
}

static void caps_at_the_maximum_material_count(void)
{
    MaterialRamp ramps[RAMP_REMAP_MAX_MATERIALS + 1];
    for (int i = 0; i < RAMP_REMAP_MAX_MATERIALS + 1; i++) {
        ramps[i].material_id = (unsigned char)(i + 1);
        ramps[i].count = 1;
        ramps[i].colors[0] = RampColor{ 5, 5, 5 };
    }
    AssembledPalette out;
    int assembled = AssemblePalette(ramps, RAMP_REMAP_MAX_MATERIALS + 1, &out);
    CHECK(assembled == RAMP_REMAP_MAX_MATERIALS);
    CHECK(out.span_count == RAMP_REMAP_MAX_MATERIALS);
}

/* ---- AssembleFromExistingPalette ---- */

static void copies_the_target_palette_verbatim_and_keeps_named_blocks(void)
{
    unsigned short target[6] = { 0, 100, 200, 300, 400, 500 };
    AssembledPalette::Span blocks[2] = {
        { 1, 1, 2 },   /* material 1: indices 1-2 */
        { 2, 3, 3 },   /* material 2: indices 3-5 */
    };
    AssembledPalette out;
    int written = AssembleFromExistingPalette(target, 6, blocks, 2, &out);
    CHECK(written == 2);
    CHECK(out.numc == 6);
    for (int i = 0; i < 6; i++) CHECK(out.words[i] == target[i]);
    CHECK(out.span_count == 2);
    CHECK(out.spans[0].material_id == 1 && out.spans[0].start == 1 && out.spans[0].count == 2);
    CHECK(out.spans[1].material_id == 2 && out.spans[1].start == 3 && out.spans[1].count == 3);
}

static void a_block_entirely_outside_the_target_is_dropped(void)
{
    unsigned short target[4] = { 0, 10, 20, 30 };
    AssembledPalette::Span blocks[2] = {
        { 1, 1, 3 },     /* valid */
        { 2, 10, 5 },    /* start >= target_numc: dropped */
    };
    AssembledPalette out;
    int written = AssembleFromExistingPalette(target, 4, blocks, 2, &out);
    CHECK(written == 1);
    CHECK(out.span_count == 1);
    CHECK(out.spans[0].material_id == 1);
}

static void a_block_that_overruns_the_target_is_clamped(void)
{
    unsigned short target[4] = { 0, 10, 20, 30 };
    AssembledPalette::Span blocks[1] = { { 1, 2, 10 } };   /* start=2, count=10 -- only 2 fit */
    AssembledPalette out;
    int written = AssembleFromExistingPalette(target, 4, blocks, 1, &out);
    CHECK(written == 1);
    CHECK(out.spans[0].start == 2);
    CHECK(out.spans[0].count == 2);
}

static void remap_works_the_same_whether_the_palette_was_fit_or_matched(void)
{
    /* An "existing" 5-color palette: transparent, a 2-step gray block, a
       2-step red block. RemapFrameToRamps must not be able to tell this
       AssembledPalette apart from one AssemblePalette built -- same
       nearest-luminance-within-the-material's-span behavior either way. */
    unsigned short target[5] = {
        0,
        RampColorWord({0,0,0}), RampColorWord({31,31,31}),
        RampColorWord({16,0,0}), RampColorWord({31,0,0}),
    };
    AssembledPalette::Span blocks[2] = { { 1, 1, 2 }, { 2, 3, 2 } };
    AssembledPalette assembled;
    CHECK(AssembleFromExistingPalette(target, 5, blocks, 2, &assembled) == 2);

    unsigned char rgb[1*3] = { 200, 10, 10 };   /* red-ish, closer to bright red than dim red */
    unsigned char alpha[1] = { 255 };
    unsigned char mask[1]  = { 2 };
    unsigned char out_idx[1];
    int mapped = RemapFrameToRamps(rgb, alpha, mask, 1, 1, assembled, out_idx);
    CHECK(mapped == 1);
    CHECK(out_idx[0] == 4);   /* span 2's bright-red slot, index 3+1 */
}

/* ---- RemapFrameToRamps ---- */

static void remaps_by_nearest_luminance_within_the_pixels_own_material(void)
{
    MaterialRamp ramps[2] = {
        /* "grays" -- 8-bit luma after 5->8 expansion: 0, 82, 165, 255 */
        mkramp(1, { {0,0,0}, {10,10,10}, {20,20,20}, {31,31,31} }),
        /* "red" -- 8-bit luma: bright ~76.5, dim ~39.6 */
        mkramp(2, { {31,0,0}, {16,0,0} }),
    };
    AssembledPalette assembled;
    CHECK(AssemblePalette(ramps, 2, &assembled) == 2);
    const int gray_start = assembled.spans[0].start;   /* == 1 */
    const int red_start  = assembled.spans[1].start;   /* == 5 */

    unsigned char rgb[5*3] = {
        150,150,150,      /* px0: gray material, luma 150 -> nearest gray slot2 (165) */
        200,10,10,        /* px1: red material,  luma 67  -> nearest red slot0 (76.5, bright) */
        255,50,50,        /* px2: red material,  luma 111.5 -> still nearest red slot0, even
                              though grays' slot1 (82) is numerically closer overall --
                              this is the cross-material-bleed check */
        1,1,1,             /* px3: an unassigned/unknown material id -> must stay index 0 */
        9,9,9,             /* px4: transparent -> must stay index 0 */
    };
    unsigned char alpha[5] = { 255, 255, 255, 255, 0 };
    unsigned char mask[5]  = { 1, 2, 2, 9, 2 };   /* id 9 has no span */

    unsigned char out_idx[5];
    int mapped = RemapFrameToRamps(rgb, alpha, mask, 5, 1, assembled, out_idx);

    CHECK(mapped == 3);
    CHECK(out_idx[0] == gray_start + 2);
    CHECK(out_idx[1] == red_start + 0);
    CHECK(out_idx[2] == red_start + 0);
    CHECK(out_idx[3] == 0);
    CHECK(out_idx[4] == 0);
}

static void empty_assembled_palette_maps_nothing(void)
{
    AssembledPalette empty;
    std::memset(&empty, 0, sizeof(empty));
    empty.numc = 1;

    unsigned char rgb[1*3] = { 100, 100, 100 };
    unsigned char alpha[1] = { 255 };
    unsigned char mask[1]  = { 1 };
    unsigned char out_idx[1] = { 0xFF };

    int mapped = RemapFrameToRamps(rgb, alpha, mask, 1, 1, empty, out_idx);
    CHECK(mapped == 0);
    CHECK(out_idx[0] == 0);
}

int main(void)
{
    assembles_spans_back_to_back_after_the_transparent_slot();
    skips_a_ramp_that_overflows_but_keeps_trying_later_ones();
    caps_at_the_maximum_material_count();
    remaps_by_nearest_luminance_within_the_pixels_own_material();
    empty_assembled_palette_maps_nothing();
    copies_the_target_palette_verbatim_and_keeps_named_blocks();
    a_block_entirely_outside_the_target_is_dropped();
    a_block_that_overruns_the_target_is_clamped();
    remap_works_the_same_whether_the_palette_was_fit_or_matched();

    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
