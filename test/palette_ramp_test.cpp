/*************************************************************
 * test/palette_ramp_test.cpp
 *
 * Unit coverage for the material ramp fitters in platform/palette_ramp.cpp.
 * No UI/SDL dependencies.
 *************************************************************/
#include "palette_ramp.h"

#include <cmath>
#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static RampSample mksamp(int r5, int g5, int b5, unsigned int weight)
{
    RampSample s;
    s.word = (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
    s.weight = weight;
    return s;
}

/* ---- RampColorWord ---- */

static void colorword_checks(void)
{
    RampColor c{ 31, 0, 0 };
    CHECK(RampColorWord(c) == (31 << 10));
    c = RampColor{ 0, 31, 0 };
    CHECK(RampColorWord(c) == (31 << 5));
    c = RampColor{ 0, 0, 31 };
    CHECK(RampColorWord(c) == 31);
    c = RampColor{ 8, 16, 24 };
    CHECK(RampColorWord(c) == ((8 << 10) | (16 << 5) | 24));
}

/* ---- FitRampLinear ---- */

static void linear_reproduces_even_gray_ramp(void)
{
    /* A hand-authored ramp -- exactly what REDS/GRAYS in RAID1.IMG look
       like: even luminance steps, equal weight (a synthetic ramp has no
       "which shade is more common" to be biased by). Asking for the same
       count back should reproduce it exactly. */
    RampSample pop[8];
    for (int i = 0; i < 8; i++) pop[i] = mksamp(i * 4, i * 4, i * 4, 100);

    RampColor out[8];
    int got = FitRampLinear(pop, 8, 8, 0.0, 1.0, out);
    CHECK(got == 8);
    for (int i = 0; i < got; i++) {
        CHECK(out[i].r == i * 4);
        CHECK(out[i].g == i * 4);
        CHECK(out[i].b == i * 4);
    }
    /* Strictly increasing -- a luminance ramp should walk in one direction,
       never backtrack. */
    for (int i = 1; i < got; i++) CHECK(out[i].r > out[i - 1].r);
}

static void linear_asking_for_more_than_the_population_has_dedups(void)
{
    /* Only two distinct colors in the population; asking for 6 steps should
       collapse the interpolated duplicates rather than emit six colors none
       of which differ. */
    RampSample pop[2] = { mksamp(0, 0, 0, 50), mksamp(2, 2, 2, 50) };
    RampColor out[6];
    int got = FitRampLinear(pop, 2, 6, 0.0, 1.0, out);
    CHECK(got >= 1 && got <= 3);
    for (int i = 1; i < got; i++)
        CHECK(!(out[i].r == out[i - 1].r && out[i].g == out[i - 1].g && out[i].b == out[i - 1].b));
}

static void linear_single_sample_returns_one_color(void)
{
    RampSample pop[1] = { mksamp(10, 20, 30, 1) };
    RampColor out[5];
    int got = FitRampLinear(pop, 1, 5, 0.0, 1.0, out);
    CHECK(got == 1);
    CHECK(out[0].r == 10 && out[0].g == 20 && out[0].b == 30);
}

static void linear_rejects_degenerate_input(void)
{
    RampColor out[4];
    CHECK(FitRampLinear(nullptr, 0, 4, 0, 1, out) == 0);
    RampSample pop[1] = { mksamp(1, 1, 1, 1) };
    CHECK(FitRampLinear(pop, 1, 0, 0, 1, out) == 0);
    RampSample zero[1] = { mksamp(1, 1, 1, 0) };   /* zero-weight = empty */
    CHECK(FitRampLinear(zero, 1, 4, 0, 1, out) == 0);
}

/* ---- FitRampLloyd ---- */

static void lloyd_recovers_two_tight_clusters(void)
{
    /* Two well-separated clusters, one much heavier than the other. Lloyd
       should land a 2-color ramp on the two cluster means regardless of the
       weight imbalance -- it is fitting distinct shades, not doing a
       weighted average of everything. */
    RampSample pop[6] = {
        mksamp(1, 1, 1, 500), mksamp(2, 1, 1, 500), mksamp(1, 2, 1, 500),
        mksamp(28, 28, 28, 20), mksamp(29, 28, 28, 20), mksamp(28, 29, 28, 20),
    };
    RampColor out[2];
    int got = FitRampLloyd(pop, 6, 2, 0.0, 1.0, 32, out);
    CHECK(got == 2);
    CHECK(out[0].r <= 4);
    CHECK(out[1].r >= 25);
}

static void lloyd_matches_population_better_than_or_equal_to_linear(void)
{
    /* A population with real internal structure: dense near one end, sparse
       near the other -- the shape the header documents Lloyd being for.
       Lloyd directly minimizes reconstruction error, so it must never do
       worse than the linear fit on its own source population. */
    RampSample pop[10];
    for (int i = 0; i < 10; i++) {
        /* Heavy weight bunched at the dark end, light weight at the bright
           end -- an uneven population a linear fit has no way to favor. */
        unsigned int w = (i < 6) ? 200 : 10;
        pop[i] = mksamp(i * 3, i * 3, i * 3, w);
    }
    RampColor lin[4], llo[4];
    int nlin = FitRampLinear(pop, 10, 4, 0.0, 1.0, lin);
    int nllo = FitRampLloyd(pop, 10, 4, 0.0, 1.0, 32, llo);
    CHECK(nlin > 0 && nllo > 0);
    const double rms_lin = RampPopulationRMS(pop, 10, lin, nlin);
    const double rms_llo = RampPopulationRMS(pop, 10, llo, nllo);
    CHECK(rms_llo <= rms_lin + 1e-9);
}

static void lloyd_rejects_degenerate_input(void)
{
    RampColor out[4];
    CHECK(FitRampLloyd(nullptr, 0, 4, 0, 1, 0, out) == 0);
    RampSample pop[1] = { mksamp(1, 1, 1, 1) };
    CHECK(FitRampLloyd(pop, 1, 0, 0, 1, 0, out) == 0);
}

/* ---- RampPopulationRMS ---- */

static void rms_zero_for_exact_match(void)
{
    RampSample pop[3] = { mksamp(0, 0, 0, 10), mksamp(15, 15, 15, 10), mksamp(31, 31, 31, 10) };
    RampColor ramp[3] = { {0,0,0}, {15,15,15}, {31,31,31} };
    CHECK(RampPopulationRMS(pop, 3, ramp, 3) == 0.0);
}

static void rms_increases_with_distance(void)
{
    RampSample pop[1] = { mksamp(0, 0, 0, 1) };
    RampColor near_ramp[1] = { {1, 0, 0} };
    RampColor far_ramp[1]  = { {10, 0, 0} };
    const double d_near = RampPopulationRMS(pop, 1, near_ramp, 1);
    const double d_far  = RampPopulationRMS(pop, 1, far_ramp, 1);
    CHECK(d_near > 0.0);
    CHECK(d_far > d_near);
}

static void rms_empty_population_is_zero(void)
{
    RampColor ramp[1] = { {0, 0, 0} };
    CHECK(RampPopulationRMS(nullptr, 0, ramp, 1) == 0.0);
    RampSample zero[1] = { mksamp(5, 5, 5, 0) };
    CHECK(RampPopulationRMS(zero, 1, ramp, 1) == 0.0);
}

int main(void)
{
    colorword_checks();
    linear_reproduces_even_gray_ramp();
    linear_asking_for_more_than_the_population_has_dedups();
    linear_single_sample_returns_one_color();
    linear_rejects_degenerate_input();
    lloyd_recovers_two_tight_clusters();
    lloyd_matches_population_better_than_or_equal_to_linear();
    lloyd_rejects_degenerate_input();
    rms_zero_for_exact_match();
    rms_increases_with_distance();
    rms_empty_population_is_zero();

    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
