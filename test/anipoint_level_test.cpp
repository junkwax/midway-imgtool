/*************************************************************
 * test/anipoint_level_test.cpp
 *
 * Unit coverage for the stance-referenced anipoint leveller in
 * platform/anipoint_level.cpp. No UI/SDL dependencies.
 *
 * The BOSS8 case at the bottom is the real one this was built for: a boss on
 * one palette with a shipped stance frame, four added frames left at 0,0, and
 * a spark effect on a second palette.
 *************************************************************/
#include "anipoint_level.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static int stride_of(int w) { return (w + 3) & ~3; }

/* Build a sprite with a solid opaque block at [x0,x1] x [y0,y1]; the rest is
   transparent. Pixel storage is owned by the caller-supplied vector so the
   IMG can stay a plain value. */
static void make_img(IMG &img, std::vector<unsigned char> &pixels,
                     const char *name, int w, int h, int palnum,
                     int x0, int y0, int x1, int y1,
                     int anix = 0, int aniy = 0)
{
    std::memset(&img, 0, sizeof(img));
    std::snprintf(img.n_s, sizeof(img.n_s), "%s", name);
    img.w = (unsigned short)w;
    img.h = (unsigned short)h;
    img.palnum = (unsigned short)palnum;
    img.anix = (unsigned short)(short)anix;
    img.aniy = (unsigned short)(short)aniy;

    pixels.assign((size_t)stride_of(w) * (size_t)h, 0u);
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            pixels[(size_t)y * (size_t)stride_of(w) + (size_t)x] = 7u;
    img.data_p = pixels.data();
}

int main(void)
{
    /* ---- name matching ---- */
    CHECK(anipoint_level_name_is_stance("BGSTANCE1"));
    CHECK(anipoint_level_name_is_stance("JCSTANCE1"));
    CHECK(anipoint_level_name_is_stance("stance"));          /* case-insensitive */
    CHECK(!anipoint_level_name_is_stance("BGHAMMERTOP1"));
    CHECK(!anipoint_level_name_is_stance("BGSPARK1"));
    CHECK(!anipoint_level_name_is_stance(nullptr));
    {
        /* Real files reuse the bytes after the terminator as scratch. A match
           must not come from that trailing garbage. */
        char raw[16];
        std::memset(raw, 0, sizeof(raw));
        std::memcpy(raw, "BGSPARK1\0STANCE", 15);
        CHECK(!anipoint_level_name_is_stance(raw));
    }

    /* ---- content measurement ---- */
    {
        IMG img; std::vector<unsigned char> px;
        make_img(img, px, "T", 20, 30, 0, /*x*/4, /*y*/6, /*x1*/15, /*y1*/25);
        CHECK(anipoint_level_content_bottom(&img) == 25);
        int cx = -1, cy = -1;
        CHECK(anipoint_level_content_center(&img, &cx, &cy));
        CHECK(cx == (4 + 15) / 2);
        CHECK(cy == (6 + 25) / 2);
    }
    {
        /* Fully transparent: no bottom, no centre. */
        IMG img; std::vector<unsigned char> px;
        std::memset(&img, 0, sizeof(img));
        std::snprintf(img.n_s, sizeof(img.n_s), "EMPTY");
        img.w = 8; img.h = 8;
        px.assign((size_t)stride_of(8) * 8, 0u);
        img.data_p = px.data();
        CHECK(anipoint_level_content_bottom(&img) == -1);
        CHECK(!anipoint_level_content_center(&img, nullptr, nullptr));
    }
    {
        IMG img; std::memset(&img, 0, sizeof(img));
        CHECK(anipoint_level_content_bottom(&img) == -1);   /* no data_p */
    }

    /* ---- ground line is measured from the anipoint, not the frame ---- */
    {
        IMG img; std::vector<unsigned char> px;
        make_img(img, px, "S", 10, 20, 0, 0, 0, 9, 15, /*anix*/0, /*aniy*/-13);
        int ground = 0;
        CHECK(anipoint_level_ground_line(&img, &ground));
        CHECK(ground == 15 - (-13));   /* 28 */
    }

    /* ---- unset detection ---- */
    {
        IMG a; std::vector<unsigned char> pa;
        make_img(a, pa, "A", 4, 4, 0, 0, 0, 3, 3, 0, 0);
        CHECK(anipoint_level_is_unset(&a));
        a.aniy = (unsigned short)(short)-1;
        CHECK(!anipoint_level_is_unset(&a));
    }

    /* ---- no stance frame: nothing is planned at all ---- */
    {
        IMG a, b; std::vector<unsigned char> pa, pb;
        make_img(a, pa, "BGHAMMER1", 10, 10, 3, 0, 0, 9, 9, 1, 1);
        make_img(b, pb, "BGSPARK1",  10, 10, 4, 0, 0, 9, 9, 0, 0);
        std::vector<IMG *> imgs { &a, &b };
        AnipointLevelPlan plan = AnipointLevelBuildPlan(imgs);
        CHECK(!plan.has_reference);
        CHECK(plan.entries.empty());
    }

    /* ---- the BOSS8 shape ---- */
    {
        /* Stance: art bottom at row 155, anchored at -13 => ground line 168. */
        IMG stance; std::vector<unsigned char> p_stance;
        make_img(stance, p_stance, "BGSTANCE1", 67, 156, 3,
                 10, 0, 56, 155, /*anix*/30, /*aniy*/-13);

        /* Authored frame that disagrees with the stance, the way the real
           BGHAMMERTOP frames do. It must not be touched. */
        IMG hammer; std::vector<unsigned char> p_hammer;
        make_img(hammer, p_hammer, "BGHAMMERTOP1", 98, 156, 3,
                 0, 0, 97, 155, /*anix*/-9, /*aniy*/-28);

        /* Added frames, still at 0,0, at two different art heights. */
        IMG eye1; std::vector<unsigned char> p_eye1;
        make_img(eye1, p_eye1, "BGEYESHOT1", 95, 148, 3, 20, 0, 60, 147, 0, 0);
        IMG eye4; std::vector<unsigned char> p_eye4;
        make_img(eye4, p_eye4, "BGEYESHOT4", 88, 131, 3, 10, 0, 70, 130, 0, 0);

        /* Effect on its own palette, also unset. */
        IMG spark; std::vector<unsigned char> p_spark;
        make_img(spark, p_spark, "BGSPARK1", 66, 58, 4, 4, 6, 61, 55, 0, 0);

        std::vector<IMG *> imgs { &stance, &hammer, &eye1, &eye4, &spark };
        AnipointLevelPlan plan = AnipointLevelBuildPlan(imgs);

        CHECK(plan.has_reference);
        CHECK(plan.reference_index == 0);
        CHECK(plan.reference_pal == 3);
        CHECK(plan.ground_line == 168);

        /* The authored frame is skipped, not re-levelled. */
        CHECK(plan.skipped_anchored == 1);
        for (const auto &e : plan.entries) CHECK(e.index != 1);

        CHECK(plan.ground_count == 2);
        CHECK(plan.center_count == 1);
        CHECK(plan.entries.size() == 3u);

        for (const auto &e : plan.entries) {
            if (e.index == 2) {          /* BGEYESHOT1: bottom 147 */
                CHECK(e.action == AnipointLevelAction::GroundAlign);
                CHECK(e.new_aniy == 147 - 168);        /* -21 */
                CHECK(e.new_ground == 168);
                CHECK(e.new_anix == (20 + 60) / 2);    /* centred on the art */
            } else if (e.index == 3) {   /* BGEYESHOT4: bottom 130 */
                CHECK(e.action == AnipointLevelAction::GroundAlign);
                CHECK(e.new_aniy == 130 - 168);        /* -38 */
                CHECK(e.new_ground == 168);
            } else if (e.index == 4) {   /* BGSPARK1: centred, not grounded */
                CHECK(e.action == AnipointLevelAction::Center);
                CHECK(e.new_anix == (4 + 61) / 2);
                CHECK(e.new_aniy == (6 + 55) / 2);
            } else {
                CHECK(false);            /* unexpected entry */
            }
        }

        /* Both levelled frames end up standing on the same line -- the whole
           point of the exercise. */
        int g2 = 0, g3 = 0;
        for (const auto &e : plan.entries) {
            if (e.index == 2) g2 = e.new_ground;
            if (e.index == 3) g3 = e.new_ground;
        }
        CHECK(g2 == g3);
    }

    /* ---- a frame already sitting correctly is not reported as a change ---- */
    {
        IMG stance; std::vector<unsigned char> p_stance;
        make_img(stance, p_stance, "STANCE", 10, 20, 1, 0, 0, 9, 19, 0, 0);
        /* ground = 19 - 0 = 19; a same-palette frame whose art bottom is 19
           and whose centre x is 0 would need anix/aniy 0,0 -- already unset,
           so applying it would be a no-op and must not be listed. */
        IMG same; std::vector<unsigned char> p_same;
        make_img(same, p_same, "OTHER", 1, 20, 1, 0, 19, 0, 19, 0, 0);
        std::vector<IMG *> imgs { &stance, &same };
        AnipointLevelPlan plan = AnipointLevelBuildPlan(imgs);
        CHECK(plan.has_reference);
        CHECK(plan.ground_line == 19);
        CHECK(plan.entries.empty());
        CHECK(plan.ground_count == 0);
    }

    if (g_fails == 0) std::printf("anipoint_level_test: all checks passed\n");
    else std::fprintf(stderr, "anipoint_level_test: %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}
