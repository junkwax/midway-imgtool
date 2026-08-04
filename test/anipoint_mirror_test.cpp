/*************************************************************
 * test/anipoint_mirror_test.cpp
 *
 * Unit coverage for the mirror-aware anipoint math in
 * platform/anipoint_mirror.cpp. No UI/SDL dependencies.
 *
 * The acceptance fixture is real MK1SKULL data: the shipped (bad) anipoints
 * that put Scorpion's MK1 toasty flame ~286 px off when he stood on the
 * right, and the corrected ones. A correct flip preview must show the bad
 * values putting the art on the opposite side of the anchor from the good
 * ones, and the centre-offset readout must report the published c column.
 *************************************************************/
#include "anipoint_mirror.h"

#include <cmath>
#include <cstdio>
#include <string>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)
#define CHECK_F(a, b) do { if (std::fabs((a) - (b)) > 1e-4f) { \
    std::fprintf(stderr, "FAIL %s:%d: %s == %g, expected %g\n", \
                 __FILE__, __LINE__, #a, (double)(a), (double)(b)); \
    g_fails++; } } while (0)

int main(void)
{
    /* ---- The two engine conventions, straight from the listings ----
       MKUTIL.ASM ani2:  anix_eff = sizex - anix
       MKDISP.ASM ganiof: anix_eff = (sizex - 1) - anix
       They differ by exactly one pixel, which is the whole reason the enum
       exists rather than a bare literal. */
    CHECK(mirror_anipoint_axis(10, 40, MirrorConvention_Ani2) == 30);
    CHECK(mirror_anipoint_axis(10, 40, MirrorConvention_Ganiof) == 29);
    CHECK(mirror_anipoint_axis(0, 1, MirrorConvention_Ganiof) == 0);

    /* Negative anipoints — the MK1FIRE1 case — mirror past the far edge.
       MAME instrumentation read the multipart path applying 157 to
       MK1FIRE1 (w=89, anix=-68), which is w - anix, not w - 1 - anix. */
    CHECK(mirror_anipoint_axis(-68, 89, MirrorConvention_Ani2) == 157);
    CHECK(mirror_anipoint_axis(-68, 89, MirrorConvention_Ganiof) == 156);

    /* Degenerate sizes clamp to 1 rather than producing a -1 term. */
    CHECK(mirror_anipoint_axis(0, 0, MirrorConvention_Ganiof) == 0);
    CHECK(mirror_anipoint_axis(3, -5, MirrorConvention_Ani2) == -2);

    /* anipoint_effective passes through unflipped and mirrors when flipped. */
    CHECK(anipoint_effective(75, 89, false, MirrorConvention_Ani2) == 75);
    CHECK(anipoint_effective(75, 89, true, MirrorConvention_Ani2) == 14);
    CHECK(anipoint_effective(75, 89, true, MirrorConvention_Ganiof) == 13);

    /* ---- Centre offset: c = a - (size - 1)/2 ----
       Published acceptance table (mk2-main/data/MK1SKULL.IMG):

         frame      w   anix bad  anix good   c bad     c good
         MK1FIRE1   89  -68       75          -112      +31
         MK1FIRE6   30  -106      37          -120.5    +22.5
         MK1SKEL1A  30   49        9          +34.5     -5.5              */
    CHECK_F(anipoint_center_offset(-68, 89), -112.0f);
    CHECK_F(anipoint_center_offset(75, 89), 31.0f);
    CHECK_F(anipoint_center_offset(-106, 30), -120.5f);
    CHECK_F(anipoint_center_offset(37, 30), 22.5f);
    CHECK_F(anipoint_center_offset(49, 30), 34.5f);
    CHECK_F(anipoint_center_offset(9, 30), -5.5f);

    /* An anchor on the art's exact centre reads 0 either way. */
    CHECK_F(anipoint_center_offset(20, 41), 0.0f);

    /* ---- The property that makes the whole class of bug findable ----
       Under ganiof the centre offset negates exactly, so a constant that
       centres the art one way is wrong by 2*c the other way. Checked here on
       the real MK1FIRE1 numbers. */
    {
        int w = 89, a = -68;
        float unflipped = -anipoint_drawn_center_from_anchor(
            a, w, false, MirrorConvention_Ganiof);
        float flipped = -anipoint_drawn_center_from_anchor(
            a, w, true, MirrorConvention_Ganiof);
        CHECK_F(unflipped, anipoint_center_offset(a, w));
        CHECK_F(flipped, -unflipped);
    }

    /* The drawn centre is the negation of the anipoint's offset from centre. */
    CHECK_F(anipoint_drawn_center_from_anchor(75, 89, false, MirrorConvention_Ani2),
            -31.0f);

    /* Bad and good anipoints must land the art on opposite sides of the
       anchor — the thing the flip preview has to make visible. */
    {
        float bad = anipoint_center_offset(-68, 89);
        float good = anipoint_center_offset(75, 89);
        CHECK(bad < 0.0f && good > 0.0f);
    }

    /* ---- Out-of-bounds badge ---- */
    CHECK(anipoint_axis_outside(-1, 89) == true);
    CHECK(anipoint_axis_outside(0, 89) == false);
    CHECK(anipoint_axis_outside(88, 89) == false);
    CHECK(anipoint_axis_outside(89, 89) == true);
    CHECK(anipoint_axis_slack(-68, 89) == 68);
    CHECK(anipoint_axis_slack(95, 89) == 7);
    CHECK(anipoint_axis_slack(40, 89) == 0);

    {
        /* Every shipped MK1FIRE/MK1SKEL frame tripped this; MK1FIRE1's y=-64
           is outside its 0..109 box too. */
        AnipointBoundsReport r = anipoint_bounds_report(-68, -64, 89, 110);
        CHECK(r.x_outside && r.y_outside);
        CHECK(r.x_slack == 68 && r.y_slack == 64);

        AnipointBoundsReport ok = anipoint_bounds_report(75, 46, 89, 110);
        CHECK(!ok.x_outside && !ok.y_outside);
        CHECK(ok.x_slack == 0 && ok.y_slack == 0);
    }

    /* ---- Name glob for the bulk shift scope ---- */
    CHECK(sprite_name_matches_glob("MK1FIRE1", "MK1FIRE*") == true);
    CHECK(sprite_name_matches_glob("MK1FIRE1", "mk1fire*") == true);  /* case-insensitive */
    CHECK(sprite_name_matches_glob("MK1SKEL1A", "MK1FIRE*") == false);
    CHECK(sprite_name_matches_glob("MK1FIRE1", "MK1FIRE?") == true);
    CHECK(sprite_name_matches_glob("MK1FIRE12", "MK1FIRE?") == false);
    CHECK(sprite_name_matches_glob("MK1FIRE1", "*FIRE*") == true);
    CHECK(sprite_name_matches_glob("MK1FIRE1", "*") == true);
    CHECK(sprite_name_matches_glob("MK1FIRE1", "MK1FIRE1") == true);
    CHECK(sprite_name_matches_glob("MK1FIRE1", "") == false);   /* empty matches nothing */
    CHECK(sprite_name_matches_glob(nullptr, "*") == false);

    /* IMG name fields are space-padded; the trailing run must not defeat an
       exact-name pattern. */
    CHECK(sprite_name_matches_glob("MK1FIRE1   ", "MK1FIRE1") == true);
    /* Collapsed '*' runs still match. */
    CHECK(sprite_name_matches_glob("MK1FIRE1", "MK1**FIRE**1") == true);

    /* ---- Labels name the routine, so a 1 px choice is never a bare literal. */
    CHECK(std::string(mirror_convention_label(MirrorConvention_Ani2)).find("ani2")
          != std::string::npos);
    CHECK(std::string(mirror_convention_source(MirrorConvention_Ganiof)).find("ganiof")
          != std::string::npos);

    if (g_fails == 0) std::printf("anipoint_mirror_test: all checks passed\n");
    else std::fprintf(stderr, "anipoint_mirror_test: %d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
