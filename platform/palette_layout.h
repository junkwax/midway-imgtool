/*************************************************************
 * platform/palette_layout.h
 *
 * Saved palette layouts: which slots of a palette hold which material.
 * MK2's ninja palettes keep the gear at 1-32 and the skin at 47-62; UMK3's
 * keep the skin at 1-15 and the gear at 31-48. A layout names those sections
 * once so the Alternate Costume dialog can pull them back for any palette
 * laid out the same way, and pair sections by name across two palettes.
 *
 * One layout per line of a text file in the SDL pref folder:
 *
 *   MK2 NINJA|64|SCORP_P,SUB_P|Costume=1-32;Skin=47-62
 *   name     |numc|palettes used with|section=ranges;...
 *
 * Ranges use palette_transfer.h's ParseSlotRanges syntax. Names lose the
 * separator characters | ; = , when stored. Pure strings, no UI; unit-tested
 * in test/palette_layout_test.cpp.
 *************************************************************/
#ifndef PLATFORM_PALETTE_LAYOUT_H
#define PLATFORM_PALETTE_LAYOUT_H

#include <string>
#include <vector>

struct PaletteLayoutSection {
    std::string name;       /* "Costume", "Skin", "Fans" */
    std::string ranges;     /* "1-5, 10-32" */
};

struct PaletteLayout {
    std::string name;
    int numc = 0;                           /* color count it was made on; 0 = any */
    std::vector<std::string> palettes;      /* palette names it has been used with */
    std::vector<PaletteLayoutSection> sections;
};

/* A costume template: the sections of BOTH palettes of a borrow, saved
 * together, e.g. UMK3 ninja -> MK2 ninja with Costume, Skin and Trim on
 * each. Picked once, it fills the sections for any pair of palettes with
 * these color counts. Kept in its own file, one per line:
 *
 *   UMK3 TO MK2|64|64|Costume=1-32;Skin=47-62|Costume=31-48;Skin=1-15
 *   name       |this numc|reference numc|this palette's sections|reference's
 */
struct CostumeTemplate {
    std::string name;
    int dst_numc = 0;                       /* 0 = any */
    int ref_numc = 0;
    std::vector<PaletteLayoutSection> dst, ref;
};

bool ParseCostumeTemplateLine(const char *line, CostumeTemplate *out);
std::string FormatCostumeTemplateLine(const CostumeTemplate &t);

/* `s` with the separator characters removed and surrounding spaces trimmed,
   cut to `max_len`. */
std::string CleanLayoutField(const std::string &s, size_t max_len);

/* Parse one line; false for blank, malformed or nameless lines. */
bool ParsePaletteLayoutLine(const char *line, PaletteLayout *out);

/* The line for a layout, without a newline. */
std::string FormatPaletteLayoutLine(const PaletteLayout &layout);

/* Case-blind name comparison, for layout, section and palette names. */
bool SameLayoutName(const std::string &a, const std::string &b);

/* The first layout that lists `palette` (case-blind) and, when both are
   known, has `numc` colors. -1 when none does. */
int FindLayoutForPalette(const std::vector<PaletteLayout> &layouts, const char *palette, int numc);

#endif /* PLATFORM_PALETTE_LAYOUT_H */
