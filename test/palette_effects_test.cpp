/*************************************************************
 * test/palette_effects_test.cpp
 *
 * Unit coverage for platform/palette_effects.cpp: the gradient refit behind
 * the generated FREZ / NUFRZ / ELPUP / ELRED / REDEL / PURPL palettes, the
 * blood ramp, and the fighter naming rules.
 *************************************************************/
#include "palette_effects.h"

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned short w555(int r, int g, int b) { return (unsigned short)((r << 10) | (g << 5) | b); }

static void fit_checks(void)
{
    /* Gray ramp, darkest to lightest, slot 0 transparent. */
    unsigned short src[5] = { 0x1234, w555(2, 2, 2), w555(10, 10, 10), w555(20, 20, 20), w555(31, 31, 31) };
    const float stops[] = { 0, 0, 0,   0, 0, 1 };      /* black -> blue */
    unsigned short out[5];
    int changed = FitPaletteToGradient(src, 5, nullptr, stops, 2, out);
    CHECK(changed == 4);
    CHECK(out[0] == 0x1234);                 /* transparent slot untouched */
    CHECK(out[1] == 0);                      /* darkest -> first stop */
    CHECK(out[4] == w555(0, 0, 31));         /* brightest -> last stop */
    for (int i = 1; i < 5; i++) CHECK((out[i] >> 5) == 0);   /* only blue */
    CHECK((out[2] & 31) < (out[3] & 31));    /* brightness order kept */

    /* Masked slots and the stored high bit copy through. */
    unsigned short hi[3] = { 0, (unsigned short)(0x8000 | w555(5, 5, 5)), w555(30, 30, 30) };
    unsigned char mask[3] = { 0, 1, 0 };
    CHECK(FitPaletteToGradient(hi, 3, mask, stops, 2, out) == 1);
    CHECK(out[1] & 0x8000);
    CHECK(out[2] == hi[2]);
    /* One fitted slot sits mid-ramp. */
    CHECK((out[1] & 0x7fff) == w555(0, 0, 16));

    /* In place, and bad arguments. */
    CHECK(FitPaletteToGradient(src, 5, nullptr, stops, 2, src) == 4);
    CHECK(src[4] == w555(0, 0, 31));
    CHECK(FitPaletteToGradient(src, 5, nullptr, stops, 1, out) == -1);
    CHECK(FitPaletteToGradient(src, 0, nullptr, stops, 2, out) == -1);
}

static void blood_checks(void)
{
    unsigned short w[16] = {};
    for (int i = 0; i < 16; i++) w[i] = 0x7777;
    FillBloodRamp(w, 6, 10);
    for (int i = 0; i < 6; i++) CHECK(w[i] == 0x7777);
    CHECK(w[6] == w555(4, 0, 0));
    CHECK(w[15] == w555(31, 8, 5));
    for (int i = 7; i < 16; i++) CHECK(((w[i] >> 10) & 31) >= ((w[i - 1] >> 10) & 31));
    FillBloodRamp(w, 0, 1);
    CHECK(w[0] == w555(4, 0, 0));
}

static void condense_checks(void)
{
    /* Unused slots are dropped before anything is merged. */
    unsigned short pal[8] = { 0, w555(31, 0, 0), w555(0, 31, 0), w555(0, 0, 31),
                              w555(31, 31, 0), w555(0, 31, 31), w555(9, 9, 9), w555(5, 5, 5) };
    const unsigned short *one[] = { pal };
    unsigned long long use[8] = { 50, 10, 10, 10, 10, 10, 0, 0 };
    BloodCondensePlan plan;
    CHECK(PlanBloodCondense(one, 1, 8, use, 2, plan));
    CHECK(plan.dropped == 2 && plan.merged == 0 && plan.changed_pixels == 0);
    CHECK(plan.reps.size() == 5);
    for (int i = 1; i <= 5; i++) CHECK(plan.map[i] == i);
    CHECK(plan.map[0] == 0);
    CHECK(plan.map[6] >= 1 && plan.map[6] <= 5);     /* stray slots still land on a color */

    unsigned short blood[2] = { w555(10, 0, 0), w555(31, 0, 0) };
    unsigned short out[8];
    BuildBloodPalette(plan, pal, blood, out);
    for (int i = 0; i <= 5; i++) CHECK(out[i] == pal[i]);
    CHECK(out[6] == blood[0] && out[7] == blood[1]);

    /* Every slot drawn: the closest pairs merge, identical ones first. */
    unsigned short busy[8] = { 0, w555(10, 0, 0), w555(11, 0, 0), w555(0, 20, 0),
                               w555(0, 20, 0), w555(0, 0, 31), w555(31, 31, 31), w555(31, 0, 31) };
    const unsigned short *b1[] = { busy };
    unsigned long long all[8] = { 0, 5, 3, 10, 10, 10, 10, 10 };
    CHECK(PlanBloodCondense(b1, 1, 8, all, 2, plan));
    CHECK(plan.merged == 2 && plan.reps.size() == 5);
    CHECK(plan.map[3] == plan.map[4]);
    CHECK(plan.map[1] == plan.map[2]);
    CHECK(plan.reps[0] == 1);                        /* the busier slot represents */
    CHECK(plan.changed_pixels == 3);                 /* slot 2 changes; 3/4 were identical */
    for (int i = 1; i < 8; i++) CHECK(plan.map[i] >= 1 && plan.map[i] <= 5);

    /* Alike in one costume but not the other: not merged. */
    unsigned short alt[8] = { 0, w555(10, 0, 0), w555(31, 31, 0), w555(0, 20, 0),
                              w555(31, 0, 31), w555(0, 0, 31), w555(31, 31, 31), w555(30, 31, 31) };
    const unsigned short *b2[] = { busy, alt };
    unsigned long long even[8] = { 0, 10, 10, 10, 10, 10, 10, 10 };
    CHECK(PlanBloodCondense(b2, 2, 8, even, 1, plan));   /* room 6: one merge */
    CHECK(plan.merged == 1);
    CHECK(plan.map[1] != plan.map[2] && plan.map[3] != plan.map[4]);
    CHECK(plan.map[6] == plan.map[7]);                 /* close in both */

    CHECK(!PlanBloodCondense(one, 1, 8, use, 7, plan));
    CHECK(!PlanBloodCondense(one, 1, 8, use, 0, plan));

    unsigned short sampled[3];
    const float ramp[] = { 0, 0, 0,   1, 1, 1 };
    SampleGradientWords(ramp, 2, 3, sampled);
    CHECK(sampled[0] == 0 && sampled[2] == w555(31, 31, 31));
    CHECK(sampled[1] == w555(16, 16, 16));
}

static void name_checks(void)
{
    CHECK(CharacterPrefixOf("jcalt1_p") == "JC");
    CHECK(CharacterPrefixOf("J").empty());
    CHECK(EffectPaletteName("JC", kCharacterEffects[1].suffix) == "JCNUFRZ_P");
    CHECK(BloodPaletteName("JC", "JCMAIN_P") == "JCBLUD_P");
    CHECK(BloodPaletteName("JC", "JCALT3_P") == "JCBLUD3_P");
    CHECK(BloodPaletteName("K1", "K1MAIN_P") == "K1BLUD_P");
    CHECK(IsGeneratedPaletteName("JCFREZ_P", "JC"));
    CHECK(IsGeneratedPaletteName("jcpurpl_p", "JC"));
    CHECK(IsGeneratedPaletteName("JCBLUD2_P", "JC"));
    CHECK(IsGeneratedPaletteName("JCBLD1_P", "JC"));
    CHECK(!IsGeneratedPaletteName("JCALT1_P", "JC"));
    CHECK(!IsGeneratedPaletteName("LKFREZ_P", "JC"));
    for (const CharacterEffectKind &e : kCharacterEffects)
        CHECK(EffectPaletteName("JC", e.suffix).size() <= PALETTE_NAME_MAX);
}

int main(void)
{
    fit_checks();
    blood_checks();
    condense_checks();
    name_checks();
    if (g_fails) { std::fprintf(stderr, "%d failure(s)\n", g_fails); return 1; }
    std::printf("palette_effects: all checks passed\n");
    return 0;
}
