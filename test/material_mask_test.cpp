/*************************************************************
 * test/material_mask_test.cpp
 *
 * Unit coverage for the chrominance-direction segmentation in
 * platform/material_mask.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "material_mask.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned char rnd8(double v)
{
    int i = (int)std::lround(v);
    if (i < 0) i = 0; if (i > 255) i = 255;
    return (unsigned char)i;
}

/* ---- FloodFillMaterial: shading scale invariance ---- */

static void fills_across_shading_but_stops_at_a_different_hue(void)
{
    /* A row of one material shaded from dark to bright (same direction,
       different magnitude -- exactly what a luminance ramp looks like),
       followed by a distinctly different hue, followed by a near-black
       pixel that would be unreachable connectivity-wise. Seeding from the
       brightest skin pixel should walk the whole shaded run and stop dead
       at the hue change -- the "wall" the other hue's own opacity forms
       should keep even the noise-floor-exempt near-black pixel beyond it
       out, since it is never adjacent to anything that passed. */
    const double base[3] = { 200, 100, 50 };
    unsigned char rgb[6 * 3];
    unsigned char alpha[6];
    const double shade[4] = { 0.3, 0.5, 0.7, 1.0 };
    for (int i = 0; i < 4; i++) {
        rgb[i*3+0] = rnd8(base[0]*shade[i]);
        rgb[i*3+1] = rnd8(base[1]*shade[i]);
        rgb[i*3+2] = rnd8(base[2]*shade[i]);
        alpha[i] = 255;
    }
    rgb[4*3+0] = 50; rgb[4*3+1] = 100; rgb[4*3+2] = 200; alpha[4] = 255;   /* different hue */
    rgb[5*3+0] = 1;  rgb[5*3+1] = 1;   rgb[5*3+2] = 1;   alpha[5] = 255;   /* near-black, unreachable */

    unsigned char mask[6] = {0,0,0,0,0,0};
    MaterialSeedParams p = MaterialSeedParamsDefault();
    p.tolerance_deg = 15.0;

    int changed = FloodFillMaterial(rgb, alpha, 6, 1, /*seed*/3, 0, p, 9, mask);
    CHECK(changed == 4);
    for (int i = 0; i < 4; i++) CHECK(mask[i] == 9);
    CHECK(mask[4] == 0);
    CHECK(mask[5] == 0);
}

static void noise_floor_lets_a_shadow_pixel_join_without_hue_evidence(void)
{
    unsigned char rgb[2*3] = { 200,100,50,  1,1,1 };   /* seed, near-black */
    unsigned char alpha[2] = { 255, 255 };

    /* Strict angle tolerance -- if the near-black pixel is included, it can
       only be because the noise floor bypassed the angle test entirely. */
    MaterialSeedParams p;
    p.tolerance_deg = 1.0;
    p.noise_floor = 14;
    unsigned char mask[2] = {0,0};
    int changed = FloodFillMaterial(rgb, alpha, 2, 1, 0, 0, p, 4, mask);
    CHECK(changed == 2);
    CHECK(mask[0] == 4 && mask[1] == 4);

    /* With the noise floor disabled, that exact same near-black pixel has
       zero chroma magnitude of its own, so cos_similarity's epsilon guard
       returns a defined 0 (orthogonal) rather than matching -- it must now
       fail the 1-degree test and stay unassigned. */
    p.noise_floor = 0;
    unsigned char mask2[2] = {0,0};
    int changed2 = FloodFillMaterial(rgb, alpha, 2, 1, 0, 0, p, 4, mask2);
    CHECK(changed2 == 1);
    CHECK(mask2[0] == 4 && mask2[1] == 0);
}

static void does_not_reassign_an_already_claimed_seed(void)
{
    unsigned char rgb[1*3] = { 200, 100, 50 };
    unsigned char alpha[1] = { 255 };
    unsigned char mask[1] = { 3 };   /* already claimed by material 3 */
    MaterialSeedParams p = MaterialSeedParamsDefault();
    int changed = FloodFillMaterial(rgb, alpha, 1, 1, 0, 0, p, 7, mask);
    CHECK(changed == 0);
    CHECK(mask[0] == 3);
}

static void cannot_seed_on_a_transparent_pixel(void)
{
    unsigned char rgb[1*3] = { 200, 100, 50 };
    unsigned char alpha[1] = { 0 };
    unsigned char mask[1] = { 0 };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    int changed = FloodFillMaterial(rgb, alpha, 1, 1, 0, 0, p, 7, mask);
    CHECK(changed == 0);
    CHECK(mask[0] == 0);
}

/* ---- FloodFillMaterial: erase mode (material_id 0) ---- */

static void erase_clears_a_contiguous_run_of_the_seeded_id(void)
{
    unsigned char rgb[3*3] = { 1,1,1, 2,2,2, 3,3,3 };   /* colors irrelevant to erase */
    unsigned char alpha[3] = { 255, 255, 255 };
    unsigned char mask[3] = { 5, 5, 5 };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    int changed = FloodFillMaterial(rgb, alpha, 3, 1, /*seed*/1, 0, p, 0, mask);
    CHECK(changed == 3);
    CHECK(mask[0] == 0 && mask[1] == 0 && mask[2] == 0);
}

static void erase_does_not_cross_a_gap_to_a_disconnected_blob(void)
{
    unsigned char rgb[5*3] = {0};
    unsigned char alpha[5] = { 255,255,255,255,255 };
    unsigned char mask[5] = { 5, 0, 0, 0, 5 };   /* two isolated id-5 pixels */
    MaterialSeedParams p = MaterialSeedParamsDefault();
    int changed = FloodFillMaterial(rgb, alpha, 5, 1, /*seed*/0, 0, p, 0, mask);
    CHECK(changed == 1);
    CHECK(mask[0] == 0);
    CHECK(mask[4] == 5);   /* untouched */
}

static void erasing_an_unassigned_seed_is_a_noop(void)
{
    unsigned char rgb[1*3] = {0,0,0};
    unsigned char alpha[1] = { 255 };
    unsigned char mask[1] = { 0 };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    int changed = FloodFillMaterial(rgb, alpha, 1, 1, 0, 0, p, 0, mask);
    CHECK(changed == 0);
}

/* ---- ClassifyMaterialsByHue ---- */

static void classifies_disconnected_regions_by_remembered_hue(void)
{
    /* Two hue-A pixels separated by a transparent gap, then a hue-B pixel --
       proves this is NOT connectivity-constrained the way the flood fill
       is, which is the entire reason it exists (see the header: animation
       frames don't align pixel-for-pixel, so there is no mask to flood from). */
    unsigned char rgb[5*3] = {
        100,20,10,   0,0,0,   200,40,20,   0,0,0,   20,20,200,
    };
    unsigned char alpha[5] = { 255, 0, 255, 0, 255 };

    MaterialRef refs[2] = {
        { 1, 100, 20, 10 },
        { 2, 20, 20, 200 },
    };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    unsigned char mask[5];
    int n = ClassifyMaterialsByHue(rgb, alpha, 5, 1, refs, 2, p, mask);
    CHECK(n == 3);
    CHECK(mask[0] == 1);
    CHECK(mask[1] == 0);   /* transparent */
    CHECK(mask[2] == 1);   /* same direction as ref 1, scaled 2x */
    CHECK(mask[3] == 0);   /* transparent */
    CHECK(mask[4] == 2);
}

static void a_pixel_matching_no_reference_is_left_unassigned(void)
{
    unsigned char rgb[1*3] = { 20, 200, 20 };   /* green: neither reference */
    unsigned char alpha[1] = { 255 };
    MaterialRef refs[2] = { { 1, 200,20,20 }, { 2, 20,20,200 } };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    unsigned char mask[1];
    int n = ClassifyMaterialsByHue(rgb, alpha, 1, 1, refs, 2, p, mask);
    CHECK(n == 0);
    CHECK(mask[0] == 0);
}

static void classify_overwrites_rather_than_accumulates(void)
{
    /* Unlike the flood fill, a stale nonzero value already in out_mask must
       not block or survive a classification -- it is a fresh proposal for
       the whole frame each call. */
    unsigned char rgb[1*3] = { 100, 20, 10 };
    unsigned char alpha[1] = { 255 };
    MaterialRef refs[1] = { { 1, 100, 20, 10 } };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    unsigned char mask[1] = { 77 };
    ClassifyMaterialsByHue(rgb, alpha, 1, 1, refs, 1, p, mask);
    CHECK(mask[0] == 1);
}

/* ---- MaterialMeanColor ---- */

static void mean_color_averages_only_the_requested_id(void)
{
    unsigned char rgb[4*3] = { 10,10,10,  20,20,20,  30,30,30,  99,99,99 };
    unsigned char mask[4]  = { 2, 2, 2, 5 };
    unsigned char r,g,b;
    bool ok = MaterialMeanColor(rgb, mask, 4, 1, 2, &r, &g, &b);
    CHECK(ok);
    CHECK(r == 20 && g == 20 && b == 20);
}

static void mean_color_fails_for_an_id_with_no_pixels(void)
{
    unsigned char rgb[1*3] = { 1,1,1 };
    unsigned char mask[1]  = { 3 };
    unsigned char r,g,b;
    bool ok = MaterialMeanColor(rgb, mask, 1, 1, 9, &r, &g, &b);
    CHECK(!ok);
}

/* ---- DetectPaletteRampBlocks ---- */

static unsigned short mkword(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

static void detects_a_clean_split_between_two_chromatic_blocks(void)
{
    unsigned short words[7] = {
        mkword(0,0,0),                                  /* 0: transparent */
        mkword(20,0,0), mkword(25,0,0), mkword(31,0,0),  /* 1-3: red ramp */
        mkword(0,0,20), mkword(0,0,25), mkword(0,0,31),  /* 4-6: blue ramp */
    };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    PaletteRampBlock blocks[8];
    int n = DetectPaletteRampBlocks(words, 7, p, blocks, 8);
    CHECK(n == 2);
    CHECK(blocks[0].start == 1 && blocks[0].count == 3);
    CHECK(blocks[1].start == 4 && blocks[1].count == 3);
}

static void a_leading_gray_run_is_absorbed_into_the_block_that_follows_it(void)
{
    /* Pure grays (r==g==b) carry zero chroma, so they never themselves
       establish a block's reference direction -- they ride along with
       whichever real hue shows up next, the same way a material's own
       near-black shading does inside FloodFillMaterial. */
    unsigned short words[6] = {
        mkword(0,0,0),
        mkword(0,0,0), mkword(10,10,10), mkword(20,20,20),   /* 1-3: gray prefix */
        mkword(20,0,0), mkword(31,0,0),                      /* 4-5: red */
    };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    PaletteRampBlock blocks[8];
    int n = DetectPaletteRampBlocks(words, 6, p, blocks, 8);
    CHECK(n == 1);
    CHECK(blocks[0].start == 1 && blocks[0].count == 5);
}

static void an_all_gray_palette_is_one_block(void)
{
    unsigned short words[4] = { mkword(0,0,0), mkword(0,0,0), mkword(10,10,10), mkword(31,31,31) };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    PaletteRampBlock blocks[8];
    int n = DetectPaletteRampBlocks(words, 4, p, blocks, 8);
    CHECK(n == 1);
    CHECK(blocks[0].start == 1 && blocks[0].count == 3);
}

static void detection_stops_at_max_blocks(void)
{
    unsigned short words[6] = {
        mkword(0,0,0),
        mkword(31,0,0), mkword(0,31,0), mkword(0,0,31),
        mkword(31,31,0), mkword(0,31,31),
    };
    MaterialSeedParams p = MaterialSeedParamsDefault();
    PaletteRampBlock blocks[2];
    int n = DetectPaletteRampBlocks(words, 6, p, blocks, 2);
    CHECK(n == 2);
    CHECK(blocks[0].start == 1 && blocks[0].count == 1);
    CHECK(blocks[1].start == 2 && blocks[1].count == 1);
}

int main(void)
{
    fills_across_shading_but_stops_at_a_different_hue();
    noise_floor_lets_a_shadow_pixel_join_without_hue_evidence();
    does_not_reassign_an_already_claimed_seed();
    cannot_seed_on_a_transparent_pixel();
    erase_clears_a_contiguous_run_of_the_seeded_id();
    erase_does_not_cross_a_gap_to_a_disconnected_blob();
    erasing_an_unassigned_seed_is_a_noop();
    classifies_disconnected_regions_by_remembered_hue();
    a_pixel_matching_no_reference_is_left_unassigned();
    classify_overwrites_rather_than_accumulates();
    mean_color_averages_only_the_requested_id();
    mean_color_fails_for_an_id_with_no_pixels();
    detects_a_clean_split_between_two_chromatic_blocks();
    a_leading_gray_run_is_absorbed_into_the_block_that_follows_it();
    an_all_gray_palette_is_one_block();
    detection_stops_at_max_blocks();

    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
