/*************************************************************
 * test/body_split_test.cpp
 *
 * Unit coverage for the head/arms/torso/legs heuristic in
 * platform/body_split.cpp. The fixtures are hand-drawn silhouettes, so a
 * regression in the neck/waist/arm math shows up as a concrete wrong cut
 * rather than a subjective "looks off".
 *************************************************************/
#include "body_split.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* Build an indexed buffer from ASCII art: '.' is transparent, anything else
   is opaque. Stride is padded to a multiple of 4 like real IMG data. */
struct Sprite {
    std::vector<unsigned char> px;
    int w = 0, h = 0, stride = 0;
};

static Sprite MakeSprite(const std::vector<std::string> &art)
{
    Sprite s;
    s.h = (int)art.size();
    for (const std::string &row : art)
        if ((int)row.size() > s.w) s.w = (int)row.size();
    s.stride = (s.w + 3) & ~3;
    s.px.assign((size_t)s.stride * s.h, 0);
    for (int y = 0; y < s.h; y++) {
        const std::string &row = art[(size_t)y];
        for (int x = 0; x < (int)row.size(); x++)
            if (row[(size_t)x] != '.') s.px[(size_t)y * s.stride + x] = 1;
    }
    return s;
}

int main(void)
{
    /* A standing figure: 3-wide head, a 1px neck, arms out to both sides over
       a solid torso, then legs. Columns 0..8, rows 0..15. */
    const std::vector<std::string> fighter = {
        "...###...",   /*  0 head */
        "..#####..",   /*  1 */
        "..#####..",   /*  2 */
        "...###...",   /*  3 */
        "....#....",   /*  4 neck (narrowest) */
        "#..###..#",   /*  5 torso + arms */
        "#########",   /*  6 */
        "#..###..#",   /*  7 */
        "...###...",   /*  8 */
        "...###...",   /*  9 */
        "....#....",   /* 10 waist */
        "...#.#...",   /* 11 legs */
        "...#.#...",   /* 12 */
        "...#.#...",   /* 13 */
        "..##.##..",   /* 14 */
        "..##.##..",   /* 15 */
    };
    Sprite s = MakeSprite(fighter);

    BodySplitPlan plan;
    CHECK(BuildBodySplitPlan(s.px.data(), s.w, s.h, s.stride, &plan));
    CHECK(plan.valid);

    /* Content bounds cover the whole drawing. */
    CHECK(plan.content_x == 0);
    CHECK(plan.content_y == 0);
    CHECK(plan.content_w == 9);
    CHECK(plan.content_h == 16);

    /* The two narrowest rows in their bands are the neck (row 4) and the
       waist (row 10). */
    CHECK(plan.neck_y == 4);
    CHECK(plan.waist_y == 10);
    CHECK(!plan.used_proportional_fallback);

    /* Head is everything above the neck row. */
    const BodyPartRect &head = plan.parts[BodyPart_Head];
    CHECK(head.enabled);
    CHECK(head.y == 0);
    CHECK(head.h == 4);

    /* Legs start at the waist and run to the bottom. */
    const BodyPartRect &legs = plan.parts[BodyPart_Legs];
    CHECK(legs.enabled);
    CHECK(legs.y == 10);
    CHECK(legs.y + legs.h == 16);

    /* Torso band is rows 4..9; arms are the columns outside the dense core. */
    const BodyPartRect &torso = plan.parts[BodyPart_Torso];
    const BodyPartRect &arm_l = plan.parts[BodyPart_ArmL];
    const BodyPartRect &arm_r = plan.parts[BodyPart_ArmR];
    CHECK(torso.enabled);
    CHECK(torso.y == 4 && torso.h == 6);
    CHECK(arm_l.enabled);
    CHECK(arm_r.enabled);
    CHECK(arm_l.x == 0);
    CHECK(arm_r.x + arm_r.w == 9);
    /* Arms and torso tile the band exactly once, left to right. */
    CHECK(arm_l.x + arm_l.w == torso.x);
    CHECK(torso.x + torso.w == arm_r.x);

    /* Trimming pulls each rect onto its own pixels. The left arm box covers
       columns 0..2 of the band, which are opaque only on rows 5..7 (the
       shoulder row 6 reaches all the way across). */
    BodySplitPlan trimmed = plan;
    int kept = TrimBodySplitPlan(s.px.data(), s.w, s.h, s.stride, &trimmed);
    CHECK(kept == 5);
    CHECK(trimmed.parts[BodyPart_ArmL].x == 0);
    CHECK(trimmed.parts[BodyPart_ArmL].w == 3);
    CHECK(trimmed.parts[BodyPart_ArmL].y == 5);
    CHECK(trimmed.parts[BodyPart_ArmL].h == 3);
    CHECK(trimmed.parts[BodyPart_ArmR].x + trimmed.parts[BodyPart_ArmR].w == 9);
    CHECK(trimmed.parts[BodyPart_Head].x == 2);   /* widest head row is 1..2 */
    CHECK(trimmed.parts[BodyPart_Head].w == 5);

    /* Opaque-pixel counting is clipped to the image. */
    BodyPartRect whole = {0, 0, s.w, s.h, true};
    CHECK(BodyRectOpaquePixels(s.px.data(), s.w, s.h, s.stride, whole) > 0);
    BodyPartRect outside = {100, 100, 8, 8, true};
    CHECK(BodyRectOpaquePixels(s.px.data(), s.w, s.h, s.stride, outside) == 0);

    /* A sprite with no narrowing anywhere still yields three stacked bands,
       flagged as a proportional fallback. */
    {
        std::vector<std::string> slab;
        for (int i = 0; i < 12; i++) slab.push_back("########");
        Sprite b = MakeSprite(slab);
        BodySplitPlan bp;
        CHECK(BuildBodySplitPlan(b.px.data(), b.w, b.h, b.stride, &bp));
        CHECK(bp.valid);
        CHECK(bp.neck_y > 0 && bp.waist_y > bp.neck_y);
        CHECK(bp.parts[BodyPart_Head].enabled);
        CHECK(bp.parts[BodyPart_Torso].enabled);
        CHECK(bp.parts[BodyPart_Legs].enabled);
        /* A uniform slab has no arms sticking out of its core. */
        CHECK(!bp.parts[BodyPart_ArmL].enabled);
        CHECK(!bp.parts[BodyPart_ArmR].enabled);
        CHECK(bp.parts[BodyPart_Torso].w == 8);
    }

    /* Fully transparent input is rejected rather than guessed at. */
    {
        std::vector<std::string> empty(6, "......");
        Sprite e = MakeSprite(empty);
        BodySplitPlan ep;
        CHECK(!BuildBodySplitPlan(e.px.data(), e.w, e.h, e.stride, &ep));
        CHECK(!ep.valid);
    }

    /* Child names keep the part tag even when the parent name is long. */
    {
        char name[32];
        BodyPartChildName("KANO", BodyPart_Head, name, sizeof(name));
        CHECK(std::string(name) == "KANO_HD");
        BodyPartChildName("ABCDEFGHIJKLMNO", BodyPart_Legs, name, sizeof(name));
        CHECK(std::strlen(name) == 15);
        CHECK(std::string(name).substr(12) == "_LG");
        BodyPartChildName("", BodyPart_Torso, name, sizeof(name));
        CHECK(std::string(name) == "SPRITE_TR");
    }

    /* Names and suffixes are defined for every part and nothing else. */
    for (int i = 0; i < BodyPart_Count; i++) {
        CHECK(BodyPartName(i)[0] != '\0');
        CHECK(BodyPartSuffix(i)[0] != '\0');
    }
    CHECK(BodyPartName(BodyPart_Count)[0] == '\0');
    CHECK(BodyPartSuffix(-1)[0] == '\0');

    if (g_fails == 0) {
        std::printf("PASS: body_split segmentation behaves as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d body_split check(s)\n", g_fails);
    return 1;
}
