/*************************************************************
 * test/sprite_gradient_test.cpp
 *
 * Unit coverage for the index-assigning gradient in
 * platform/sprite_gradient.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "sprite_gradient.h"

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* A palette where index i is a gray of luminance i/8 of full, so a sprite's
   tonal order is obvious by eye in the expectations below. */
static void gray_palette(unsigned short pal[256])
{
    std::memset(pal, 0, sizeof(unsigned short) * 256);
    for (int i = 0; i < 256; i++) {
        int v = i > 31 ? 31 : i;
        pal[i] = (unsigned short)((v << 10) | (v << 5) | v);
    }
}

static SpriteGradientOptions base_options(void)
{
    SpriteGradientOptions o;
    o.direction = kSpriteGradientTopBottom;
    o.ramp_start = 4;
    o.ramp_count = 8;
    o.shade_span = 1;
    o.start_pct = 0;
    o.end_pct = 100;
    o.dither = kSpriteGradientDitherNone;
    o.seed = 17;
    o.content_bounds = true;
    o.band_px = 0;
    return o;
}

/* ---- shade LUT ---- */

static void shade_lut_spans_the_tones_the_sprite_actually_uses(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    /* Indices 1, 2 and 4 present; 3 is in the palette but unused by the art. */
    unsigned char pix[16] = { 0, 1, 2, 4,
                              1, 1, 2, 4,
                              0, 0, 2, 4,
                              0, 0, 0, 4 };
    float lut[256];
    int count = SpriteGradientBuildShadeLut(pix, 4, 4, 4, pal, 256, lut);
    CHECK(count == 3);
    CHECK(lut[1] == 0.0f);
    CHECK(lut[4] == 1.0f);
    CHECK(lut[2] > 0.0f && lut[2] < 1.0f);
    CHECK(lut[3] < 0.0f);   /* present in the palette, absent from the art */
    CHECK(lut[0] < 0.0f);   /* transparent is never a tone */
}

static void one_tone_sits_in_the_middle_of_its_window(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[4] = { 1, 1, 1, 1 };
    float lut[256];
    int count = SpriteGradientBuildShadeLut(pix, 2, 2, 2, pal, 256, lut);
    CHECK(count == 1);
    CHECK(lut[1] == 0.5f);
}

static void an_index_past_the_palette_reads_as_black(void)
{
    unsigned short pal[8];
    std::memset(pal, 0, sizeof(pal));
    pal[1] = 0x7FFF;                     /* white */
    unsigned char pix[4] = { 1, 40, 1, 40 };   /* 40 is past pal_numc */
    float lut[256];
    int count = SpriteGradientBuildShadeLut(pix, 2, 2, 2, pal, 8, lut);
    CHECK(count == 2);
    CHECK(lut[1] == 1.0f);
    CHECK(lut[40] == 0.0f);
}

/* ---- plan ---- */

static void a_fully_transparent_sprite_has_no_plan(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[16] = {};
    SpriteGradientPlan plan;
    CHECK(!SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, base_options(), &plan));
}

static void options_are_clamped_into_the_plan(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[4] = { 1, 1, 1, 1 };
    SpriteGradientOptions o = base_options();
    o.ramp_count = 900;
    o.shade_span = 900;
    o.start_pct = -20;
    o.end_pct = 400;
    o.ramp_start = 200;
    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 2, 2, 2, pal, 256, o, &plan));
    CHECK(plan.opt.ramp_start == 200);
    CHECK(plan.opt.ramp_count == 56);          /* 256 - 200 */
    CHECK(plan.opt.shade_span == 56);          /* never wider than the ramp */
    CHECK(plan.opt.start_pct == 0);
    CHECK(plan.opt.end_pct == 100);
}

/* ---- the sweep ---- */

static void a_flat_window_walks_the_whole_ramp_top_to_bottom(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[16];
    std::memset(pix, 1, sizeof(pix));   /* one tone, 4x4, fully opaque */

    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, base_options(), &plan));
    /* window 1, travel 7, t = y/3 -> pos = 7y/3, rounded. */
    CHECK(SpriteGradientIndexAt(plan, 0, 0, 1, 0) == 4);
    CHECK(SpriteGradientIndexAt(plan, 0, 1, 1, 0) == 6);
    CHECK(SpriteGradientIndexAt(plan, 0, 2, 1, 0) == 9);
    CHECK(SpriteGradientIndexAt(plan, 0, 3, 1, 0) == 11);
    /* Nothing on a row varies across x for a vertical sweep. */
    CHECK(SpriteGradientIndexAt(plan, 3, 1, 1, 0) == 6);
}

static void transparent_pixels_are_never_repainted(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[16];
    std::memset(pix, 1, sizeof(pix));
    pix[5] = 0;
    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, base_options(), &plan));
    CHECK(SpriteGradientIndexAt(plan, 1, 1, 0, 0) == 0);
}

static void a_full_width_window_re_shades_without_sliding(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    /* Two tones: index 1 dark, index 8 light. */
    unsigned char pix[16] = { 1, 1, 8, 8,
                              1, 1, 8, 8,
                              1, 1, 8, 8,
                              1, 1, 8, 8 };
    SpriteGradientOptions o = base_options();
    o.shade_span = o.ramp_count;    /* window fills the ramp: travel is 0 */
    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, o, &plan));
    /* Position now depends only on the pixel's own tone, not on where it is. */
    CHECK(SpriteGradientIndexAt(plan, 0, 0, 1, 0) == 4);
    CHECK(SpriteGradientIndexAt(plan, 0, 3, 1, 0) == 4);
    CHECK(SpriteGradientIndexAt(plan, 2, 0, 8, 0) == 11);
    CHECK(SpriteGradientIndexAt(plan, 2, 3, 8, 0) == 11);
}

static void the_sweep_can_be_confined_to_a_band_at_its_start_edge(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[8 * 8];
    std::memset(pix, 3, sizeof(pix));
    SpriteGradientOptions o = base_options();
    o.direction = kSpriteGradientLeftRight;
    o.band_px = 2;
    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 8, 8, 8, pal, 256, o, &plan));
    CHECK(SpriteGradientIndexAt(plan, 0, 0, 3, 0) == 4);    /* band start */
    CHECK(SpriteGradientIndexAt(plan, 2, 0, 3, 0) == 11);   /* band end */
    /* Deeper than the band the art keeps the index it had. */
    CHECK(SpriteGradientIndexAt(plan, 3, 0, 3, 0) == 3);
    CHECK(SpriteGradientIndexAt(plan, 7, 0, 3, 0) == 3);
}

static void the_inward_feather_follows_the_silhouette(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[5 * 5];
    std::memset(pix, 2, sizeof(pix));
    SpriteGradientOptions o = base_options();
    o.direction = kSpriteGradientEdgeInward;
    o.band_px = 2;
    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 5, 5, 5, pal, 256, o, &plan));
    CHECK(SpriteGradientIndexAt(plan, 0, 0, 2, 0) == 4);    /* outline, d=1 */
    CHECK(SpriteGradientIndexAt(plan, 1, 1, 2, 0) == 11);   /* d=2, feather end */
    CHECK(SpriteGradientIndexAt(plan, 2, 2, 2, 0) == 2);    /* deeper: untouched */
}

/* ---- dither ---- */

static void an_ordered_screen_splits_a_position_that_lands_between_entries(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[16];
    std::memset(pix, 1, sizeof(pix));

    /* One tone (shade 0.5), a 2-entry ramp and a window of 1 with the sweep
       pinned mid-travel: every pixel wants position 0.5 exactly. */
    SpriteGradientOptions o = base_options();
    o.ramp_count = 2;
    o.shade_span = 1;
    o.start_pct = 50;
    o.end_pct = 50;

    o.dither = kSpriteGradientDitherNone;
    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, o, &plan));
    int flat = SpriteGradientIndexAt(plan, 0, 0, 1, 0);
    bool all_same = true;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (SpriteGradientIndexAt(plan, x, y, 1, 0) != flat) all_same = false;
    CHECK(all_same);

    o.dither = kSpriteGradientDitherBayer;
    CHECK(SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, o, &plan));
    int low = 0, high = 0;
    for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
            int idx = SpriteGradientIndexAt(plan, x, y, 1, 0);
            if (idx == 4) low++;
            else if (idx == 5) high++;
        }
    }
    /* Half the 4x4 cell each way — the screen, not noise. */
    CHECK(low == 8);
    CHECK(high == 8);
}

/* ---- apply ---- */

static void apply_reports_the_pixels_it_moved_and_the_depth_it_needs(void)
{
    unsigned short pal[256];
    gray_palette(pal);
    unsigned char pix[16];
    std::memset(pix, 1, sizeof(pix));
    pix[0] = 0;

    SpriteGradientPlan plan;
    CHECK(SpriteGradientPlanBuild(pix, 4, 4, 4, pal, 256, base_options(), &plan));
    int max_idx = SpriteGradientMaxIndex(pix, plan, 0);
    CHECK(max_idx == 11);   /* ramp_start 4 + ramp_count 8 - 1 */

    int changed = SpriteGradientApply(pix, plan, 0);
    CHECK(changed == 15);   /* every opaque pixel; the transparent one stays */
    CHECK(pix[0] == 0);
    CHECK(pix[1] == 4);
    CHECK(pix[15] == 11);

    /* Re-applying the same plan to its own output is a no-op only by accident;
       what must hold is that transparency never moved. */
    for (int i = 0; i < 16; i++)
        CHECK((pix[i] == 0) == (i == 0));
}

/* ---- ramp construction ---- */

static void a_ramp_lands_on_its_end_stops_exactly(void)
{
    const float stops[6] = { 0.0f, 0.0f, 0.0f,
                             1.0f, 1.0f, 1.0f };
    unsigned short words[5] = {};
    CHECK(SpriteGradientBuildRamp(stops, 2, 5, words) == 5);
    CHECK(words[0] == 0);
    CHECK(words[4] == 0x7FFF);
    for (int i = 1; i < 5; i++) CHECK(words[i] >= words[i - 1]);
}

static void a_ramp_walks_every_stop_in_order(void)
{
    const float stops[9] = { 1.0f, 0.0f, 0.0f,      /* red */
                             0.0f, 1.0f, 0.0f,      /* green */
                             0.0f, 0.0f, 1.0f };    /* blue */
    unsigned short words[3] = {};
    CHECK(SpriteGradientBuildRamp(stops, 3, 3, words) == 3);
    CHECK(words[0] == (unsigned short)(31 << 10));
    CHECK(words[1] == (unsigned short)(31 <<  5));
    CHECK(words[2] == (unsigned short)31);
}

static void a_ramp_needs_two_stops(void)
{
    const float stops[3] = { 1.0f, 1.0f, 1.0f };
    unsigned short words[4] = {};
    CHECK(SpriteGradientBuildRamp(stops, 1, 4, words) == 0);
}

/* ---- slot placement ---- */

static void the_block_lands_as_low_as_it_fits(void)
{
    bool used[256] = {};
    used[0] = true;
    used[1] = used[2] = used[3] = true;   /* a 2bpp sprite's own indices */
    CHECK(SpriteGradientFindRampBlock(used, 60, 63) == 4);
    CHECK(SpriteGradientLargestFreeRun(used, 63) == 60);
    /* One more color than the 6bpp ceiling can hold is a refusal, not a
       silently 7bpp sprite. */
    CHECK(SpriteGradientFindRampBlock(used, 61, 63) == -1);
    CHECK(SpriteGradientFindRampBlock(used, 61, 127) == 4);
}

static void a_block_can_land_in_a_hole_between_used_slots(void)
{
    bool used[256] = {};
    used[0] = used[1] = used[4] = true;
    /* Free: 2,3 then 5..10. */
    CHECK(SpriteGradientFindRampBlock(used, 2, 10) == 2);
    CHECK(SpriteGradientFindRampBlock(used, 3, 10) == 5);
    CHECK(SpriteGradientLargestFreeRun(used, 10) == 6);
}

static void a_full_palette_has_nowhere_to_put_a_ramp(void)
{
    bool used[256];
    std::memset(used, 1, sizeof(used));
    CHECK(SpriteGradientFindRampBlock(used, 2, 255) == -1);
    CHECK(SpriteGradientLargestFreeRun(used, 255) == 0);
}

int main(void)
{
    shade_lut_spans_the_tones_the_sprite_actually_uses();
    one_tone_sits_in_the_middle_of_its_window();
    an_index_past_the_palette_reads_as_black();
    a_fully_transparent_sprite_has_no_plan();
    options_are_clamped_into_the_plan();
    a_flat_window_walks_the_whole_ramp_top_to_bottom();
    transparent_pixels_are_never_repainted();
    a_full_width_window_re_shades_without_sliding();
    the_sweep_can_be_confined_to_a_band_at_its_start_edge();
    the_inward_feather_follows_the_silhouette();
    an_ordered_screen_splits_a_position_that_lands_between_entries();
    apply_reports_the_pixels_it_moved_and_the_depth_it_needs();
    a_ramp_lands_on_its_end_stops_exactly();
    a_ramp_walks_every_stop_in_order();
    a_ramp_needs_two_stops();
    the_block_lands_as_low_as_it_fits();
    a_block_can_land_in_a_hole_between_used_slots();
    a_full_palette_has_nowhere_to_put_a_ramp();

    if (g_fails == 0) {
        std::printf("sprite_gradient_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "sprite_gradient_test: %d check(s) failed\n", g_fails);
    return 1;
}
