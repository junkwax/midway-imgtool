/*************************************************************
 * test/anipoint_test.cpp
 *
 * Unit coverage for the pure anipoint predicates + sprite-name sequence
 * parsing extracted into platform/anipoint.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "anipoint.h"

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

int main(void)
{
    /* secondary_anipoint_words_in_use: z == -1 (0xFFFF) or negative x/y means
       "unused"; otherwise active. */
    CHECK(secondary_anipoint_words_in_use(0, 0, 0) == true);
    CHECK(secondary_anipoint_words_in_use(10, 20, 0) == true);
    CHECK(secondary_anipoint_words_in_use(0, 0, 0xFFFF) == false); /* z == -1 */
    CHECK(secondary_anipoint_words_in_use(0x8000, 0, 0) == false); /* x < 0 */
    CHECK(secondary_anipoint_words_in_use(0, 0x8000, 0) == false); /* y < 0 */

    /* secondary_anipoint_in_use mirrors the above for an IMG's anix2/aniy2/aniz2. */
    {
        IMG img {};
        img.anix2 = 5; img.aniy2 = 6; img.aniz2 = 0;
        CHECK(secondary_anipoint_in_use(&img) == true);
        img.aniz2 = 0xFFFF;
        CHECK(secondary_anipoint_in_use(&img) == false);
        CHECK(secondary_anipoint_in_use(nullptr) == false);
    }

    /* clear_secondary_anipoint sets the -1 sentinel; activate normalizes it so
       the point reads as in-use. */
    {
        IMG img {};
        img.anix2 = 3; img.aniy2 = 4; img.aniz2 = 0;
        clear_secondary_anipoint(&img);
        CHECK(secondary_anipoint_in_use(&img) == false);
        CHECK(img.aniz2 == (unsigned short)-1);

        activate_secondary_anipoint(&img);
        CHECK(secondary_anipoint_in_use(&img) == true);  /* sentinels -> 0 */
        CHECK(img.anix2 == 0 && img.aniy2 == 0 && img.aniz2 == 0);

        /* activate leaves an already-valid point untouched. */
        IMG img2 {};
        img2.anix2 = 7; img2.aniy2 = 9; img2.aniz2 = 0;
        activate_secondary_anipoint(&img2);
        CHECK(img2.anix2 == 7 && img2.aniy2 == 9 && img2.aniz2 == 0);
    }

    /* trim_sprite_name strips surrounding whitespace. */
    CHECK(trim_sprite_name("  WALK  ") == "WALK");
    CHECK(trim_sprite_name("WALK") == "WALK");
    CHECK(trim_sprite_name("   ") == "");

    /* ascii_iequals: case-insensitive, length-sensitive. */
    CHECK(ascii_iequals("Walk", "walk") == true);
    CHECK(ascii_iequals("WALK", "WALK") == true);
    CHECK(ascii_iequals("walk", "walks") == false);
    CHECK(ascii_iequals("run", "jog") == false);

    /* strip_trailing_sequence_digits: trailing digits -> stem. */
    {
        std::string stem;
        CHECK(strip_trailing_sequence_digits("JCWALK1", &stem) == true);
        CHECK(stem == "JCWALK");
        CHECK(strip_trailing_sequence_digits("JCWALK12", &stem) == true);
        CHECK(stem == "JCWALK");
        CHECK(strip_trailing_sequence_digits("JCWALK", nullptr) == false);
        CHECK(strip_trailing_sequence_digits("123", nullptr) == false); /* all digits */
    }

    /* InferSubframeParentName: numbered/lettered subframes -> folder/base name.
       EDHANG1A belongs under EDHANG1; EDHANG1 belongs under EDHANG. */
    CHECK(InferSubframeParentName("BASE_1A") == "BASE");
    CHECK(InferSubframeParentName("RUN_2B") == "RUN");
    CHECK(InferSubframeParentName("EDHANG1A") == "EDHANG1");
    CHECK(InferSubframeParentName("EDHANG1") == "EDHANG");
    CHECK(InferSubframeParentName("WALK") == "");      /* no digits */
    CHECK(InferSubframeParentName("") == "");

    if (g_fails == 0) {
        std::printf("PASS: anipoint predicates and name parsing behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d anipoint check(s)\n", g_fails);
    return 1;
}
