/*************************************************************
 * test/digitize_matte_test.cpp
 *
 * Unit coverage for the backdrop key in platform/digitize_matte.cpp. No
 * UI/SDL dependencies.
 *************************************************************/
#include "digitize_matte.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* ---- MatteExtract ---- */

static void pure_key_pixel_is_fully_transparent(void)
{
    MatteParams p = MatteParamsDefault();   /* white key, tolerance 12 */
    unsigned char rgba[4] = { 255, 255, 255, 255 };
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteStats st;
    MatteExtract(rgba, 1, 1, p, out_rgb, out_a, &st);
    CHECK(out_a[0] == 0);
    CHECK(out_rgb[0] == 0 && out_rgb[1] == 0 && out_rgb[2] == 0);
    CHECK(st.keyed == 1 && st.partial == 0 && st.opaque == 0);
}

static void pixel_far_from_key_is_opaque_and_unchanged(void)
{
    MatteParams p = MatteParamsDefault();
    unsigned char rgba[4] = { 10, 20, 30, 255 };   /* far from white */
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteStats st;
    MatteExtract(rgba, 1, 1, p, out_rgb, out_a, &st);
    CHECK(out_a[0] == 255);
    CHECK(out_rgb[0] == 10 && out_rgb[1] == 20 && out_rgb[2] == 30);
    CHECK(st.opaque == 1);
}

static void source_alpha_multiplies_through(void)
{
    MatteParams p = MatteParamsDefault();
    unsigned char rgba[4] = { 10, 20, 30, 128 };   /* key sees this as opaque */
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteExtract(rgba, 1, 1, p, out_rgb, out_a, nullptr);
    /* key_a=255 combined with src_a=128 -> (255*128+127)/255 = 128 */
    CHECK(out_a[0] == 128);
}

/* Constructs a pixel that is a KNOWN exact blend of a true actor color and
 * the key, with key_softness set so the key's own alpha estimate for THIS
 * pixel lands on the true alpha (t1 == the actor color's own distance from
 * the key, so the true actor color would sit at the very top of the ramp).
 * Then checks decontamination actually reconstructs the actor color, not
 * just that it changes something. This is the halo fix the header describes
 * as the point of the whole module -- verify it actually removes the
 * backdrop rather than merely flagging the pixel as partial. */
static void decontam_recovers_actor_color_from_known_blend(void)
{
    const int kr = 255, kg = 255, kb = 255;
    const int fr = 80, fg = 40, fb = 160;   /* true actor color */
    const double D0 = std::sqrt(double((fr-kr)*(fr-kr) + (fg-kg)*(fg-kg) + (fb-kb)*(fb-kb)));
    const double true_af = 0.5;

    auto rnd8 = [](double v) {
        int i = (int)std::lround(v);
        if (i < 0) i = 0; if (i > 255) i = 255;
        return (unsigned char)i;
    };
    unsigned char cr = rnd8(true_af * fr + (1 - true_af) * kr);
    unsigned char cg = rnd8(true_af * fg + (1 - true_af) * kg);
    unsigned char cb = rnd8(true_af * fb + (1 - true_af) * kb);

    MatteParams p = MatteParamsDefault();
    p.key_r = (unsigned char)kr; p.key_g = (unsigned char)kg; p.key_b = (unsigned char)kb;
    p.key_tolerance = 0;
    p.key_softness  = (int)std::lround(D0);
    p.decontaminate = true;
    p.despill = false;

    unsigned char rgba[4] = { cr, cg, cb, 255 };
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteStats st;
    MatteExtract(rgba, 1, 1, p, out_rgb, out_a, &st);

    CHECK(out_a[0] > 0 && out_a[0] < 255);
    CHECK(st.partial == 1);
    CHECK(st.decontam == 1);
    CHECK(std::abs((int)out_rgb[0] - fr) <= 4);
    CHECK(std::abs((int)out_rgb[1] - fg) <= 4);
    CHECK(std::abs((int)out_rgb[2] - fb) <= 4);
}

static void despill_pulls_dominant_key_channel_down_to_its_neighbours(void)
{
    /* A saturated blue-ish key: a clear lead over the runner-up channel, so
       dominant_key_channel treats it as a real screen color, not a neutral
       backdrop. */
    MatteParams p = MatteParamsDefault();
    p.key_r = 30; p.key_g = 30; p.key_b = 220;
    p.key_tolerance = 5; p.key_softness = 5;   /* narrow ramp: this pixel is
                                                   far outside it, so it is
                                                   opaque and decontamination
                                                   cannot be what moves it */
    p.decontaminate = true;
    p.despill = true;
    p.despill_strength = 100;

    unsigned char rgba[4] = { 80, 90, 150, 255 };   /* blue riding above r/g */
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteStats st;
    MatteExtract(rgba, 1, 1, p, out_rgb, out_a, &st);

    CHECK(out_a[0] == 255);           /* confirms this is despill, not decontam */
    CHECK(st.despilled == 1);
    CHECK(out_rgb[0] == 80 && out_rgb[1] == 90);   /* untouched */
    CHECK(out_rgb[2] == 90);          /* blue pulled down to max(r,g) */
}

static void despill_is_a_noop_against_a_neutral_key(void)
{
    MatteParams p = MatteParamsDefault();   /* white key: no dominant channel */
    p.key_tolerance = 5; p.key_softness = 5;
    p.despill = true;
    p.despill_strength = 100;

    unsigned char rgba[4] = { 80, 90, 150, 255 };
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteStats st;
    MatteExtract(rgba, 1, 1, p, out_rgb, out_a, &st);

    CHECK(st.despilled == 0);
    CHECK(out_rgb[0] == 80 && out_rgb[1] == 90 && out_rgb[2] == 150);
}

/* ---- MatteBinarize ---- */

static void binarize_thresholds_without_shrink(void)
{
    unsigned char alpha[4] = { 0, 127, 128, 255 };
    unsigned char mask[4];
    int kept = MatteBinarize(alpha, 4, 1, 128, 0, mask);
    CHECK(kept == 2);
    CHECK(mask[0] == 0 && mask[1] == 0 && mask[2] == 1 && mask[3] == 1);
}

static void shrink_erodes_a_solid_block_to_its_interior(void)
{
    /* A full 3x3 opaque block. One erosion pass should strip every pixel
       that has a neighbour off the edge of the image (treated as
       transparent), leaving only the center. */
    unsigned char alpha[9];
    for (int i = 0; i < 9; i++) alpha[i] = 255;
    unsigned char mask[9];

    int kept0 = MatteBinarize(alpha, 3, 3, 128, 0, mask);
    CHECK(kept0 == 9);

    int kept1 = MatteBinarize(alpha, 3, 3, 128, 1, mask);
    CHECK(kept1 == 1);
    CHECK(mask[1 * 3 + 1] == 1);
    for (int i = 0; i < 9; i++) if (i != 4) CHECK(mask[i] == 0);
}

static void shrink_removes_isolated_speckle(void)
{
    /* A single opaque pixel surrounded by transparent ones -- the leftover
       edge noise a soft key produces -- should vanish under one erosion,
       which is the point of `shrink` existing at all. */
    unsigned char alpha[9] = { 0,0,0, 0,255,0, 0,0,0 };
    unsigned char mask[9];
    int kept = MatteBinarize(alpha, 3, 3, 128, 1, mask);
    CHECK(kept == 0);
}

/* ---- MatteDownscale ---- */

static void downscale_does_not_leak_color_from_transparent_pixels(void)
{
    /* Pixel 0 is fully transparent but carries garbage color (as a real
       flattened source often does at its own former edges); pixel 1 is
       fully opaque with a known color. The averaged-down single pixel must
       come out exactly pixel 1's color, not a blend toward the garbage. */
    unsigned char rgb[2 * 3]   = { 255, 0, 0,   10, 20, 30 };
    unsigned char alpha[2]     = { 0, 255 };
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteDownscale(rgb, alpha, 2, 1, out_rgb, out_a, 1, 1);
    CHECK(out_rgb[0] == 10 && out_rgb[1] == 20 && out_rgb[2] == 30);
    CHECK(out_a[0] == 128);
}

static void downscale_of_fully_transparent_region_is_transparent_black(void)
{
    unsigned char rgb[4 * 3]  = { 1,2,3, 4,5,6, 7,8,9, 10,11,12 };
    unsigned char alpha[4]    = { 0, 0, 0, 0 };
    unsigned char out_rgb[3]; unsigned char out_a[1];
    MatteDownscale(rgb, alpha, 2, 2, out_rgb, out_a, 1, 1);
    CHECK(out_a[0] == 0);
    CHECK(out_rgb[0] == 0 && out_rgb[1] == 0 && out_rgb[2] == 0);
}

int main(void)
{
    pure_key_pixel_is_fully_transparent();
    pixel_far_from_key_is_opaque_and_unchanged();
    source_alpha_multiplies_through();
    decontam_recovers_actor_color_from_known_blend();
    despill_pulls_dominant_key_channel_down_to_its_neighbours();
    despill_is_a_noop_against_a_neutral_key();
    binarize_thresholds_without_shrink();
    shrink_erodes_a_solid_block_to_its_interior();
    shrink_removes_isolated_speckle();
    downscale_does_not_leak_color_from_transparent_pixels();
    downscale_of_fully_transparent_region_is_transparent_black();

    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
