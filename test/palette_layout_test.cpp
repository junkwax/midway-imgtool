/*************************************************************
 * test/palette_layout_test.cpp
 *
 * Unit coverage for the saved palette layouts in
 * platform/palette_layout.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "palette_layout.h"

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static void a_layout_line_round_trips(void)
{
    PaletteLayout l;
    CHECK(ParsePaletteLayoutLine("MK2 NINJA|64|SCORP_P,SUB_P|Costume=1-32;Skin=47-62\r\n", &l));
    CHECK(l.name == "MK2 NINJA");
    CHECK(l.numc == 64);
    CHECK(l.palettes.size() == 2 && l.palettes[1] == "SUB_P");
    CHECK(l.sections.size() == 2);
    CHECK(l.sections[0].name == "Costume" && l.sections[0].ranges == "1-32");
    CHECK(l.sections[1].name == "Skin" && l.sections[1].ranges == "47-62");
    CHECK(FormatPaletteLayoutLine(l) == "MK2 NINJA|64|SCORP_P,SUB_P|Costume=1-32;Skin=47-62");
}

static void multi_range_sections_keep_their_commas(void)
{
    PaletteLayout l;
    l.name = "KITANA";
    l.numc = 64;
    l.palettes = { "KITANA_P" };
    l.sections = { { "Cloth", "1-5, 10-32" }, { "Fans", "6-9" } };
    const std::string line = FormatPaletteLayoutLine(l);
    PaletteLayout back;
    CHECK(ParsePaletteLayoutLine(line.c_str(), &back));
    CHECK(back.sections.size() == 2);
    CHECK(back.sections[0].ranges == "1-5, 10-32");
    CHECK(back.sections[1].name == "Fans");
}

static void separators_cannot_break_a_line(void)
{
    PaletteLayout l;
    l.name = " A|B;C=D ";
    l.palettes = { "X,Y" };
    l.sections = { { "Sk|in", "1-3|4" } };
    PaletteLayout back;
    CHECK(ParsePaletteLayoutLine(FormatPaletteLayoutLine(l).c_str(), &back));
    CHECK(back.name == "ABCD");
    CHECK(back.palettes.size() == 1 && back.palettes[0] == "XY");
    CHECK(back.sections.size() == 1 && back.sections[0].name == "Skin");
    CHECK(back.sections[0].ranges == "1-34");
}

static void malformed_lines_are_rejected(void)
{
    PaletteLayout l;
    CHECK(!ParsePaletteLayoutLine("", &l));
    CHECK(!ParsePaletteLayoutLine("NAME|64|P", &l));
    CHECK(!ParsePaletteLayoutLine("|64|P|Costume=1-3", &l));
    CHECK(ParsePaletteLayoutLine("EMPTY|0||", &l));
    CHECK(l.sections.empty() && l.palettes.empty());
}

static void layouts_are_found_by_palette_name(void)
{
    std::vector<PaletteLayout> ls(2);
    ls[0].name = "MK2 NINJA"; ls[0].numc = 64; ls[0].palettes = { "SCORP_P", "SUB_P" };
    ls[1].name = "UMK3 NINJA"; ls[1].numc = 64; ls[1].palettes = { "RAIN1_P" };
    CHECK(FindLayoutForPalette(ls, "rain1_p", 64) == 1);
    CHECK(FindLayoutForPalette(ls, "SUB_P", 64) == 0);
    CHECK(FindLayoutForPalette(ls, "SUB_P", 32) == -1);   /* different color count */
    CHECK(FindLayoutForPalette(ls, "SUB_P", 0) == 0);     /* count unknown */
    CHECK(FindLayoutForPalette(ls, "REP_P", 64) == -1);
    CHECK(SameLayoutName("Costume", "COSTUME"));
    CHECK(!SameLayoutName("Costume", "Costumes"));
}

int main(void)
{
    a_layout_line_round_trips();
    multi_range_sections_keep_their_commas();
    separators_cannot_break_a_line();
    malformed_lines_are_rejected();
    layouts_are_found_by_palette_name();

    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
