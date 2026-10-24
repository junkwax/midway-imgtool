/*************************************************************
 * platform/palette_effects.h
 *
 * The derived palettes every MK2 fighter ships with, built from the
 * fighter's own costume palette:
 *
 *   FREEZE  -> JCFREZ_P    FIVEFRZ -> JCNUFRZ_P    ELEC    -> JCELPUP_P
 *   BGRDEL_P-> JCELRED_P   REDELI  -> JCREDEL_P    PURPELI -> JCPURPL_P
 *
 * Each is the source palette with every opaque slot refitted onto a saved
 * Indexed Color Gradient ramp (left side above) by original brightness, so
 * the slot order — and therefore every sprite's pixels — still line up.
 * Blood palettes (JCBLUD_P, JCBLUD1_P...) are a costume palette condensed
 * into fewer slots to make room for the gore colors at the end, the layout
 * MK2PALS.IMG's *BLD*_P records use.
 *
 * Pure word arithmetic, no UI or document access; unit-tested in
 * test/palette_effects_test.cpp.
 *************************************************************/
#ifndef PLATFORM_PALETTE_EFFECTS_H
#define PLATFORM_PALETTE_EFFECTS_H

#include <string>
#include <vector>

struct CharacterEffectKind {
    const char *preset;     /* saved gradient name in indexed_gradients.txt */
    const char *suffix;     /* JC + suffix + _P */
    const char *label;
};

enum { CHARACTER_EFFECT_COUNT = 6 };
extern const CharacterEffectKind kCharacterEffects[CHARACTER_EFFECT_COUNT];

/* The longest name a PAL record holds (n_s[10], NUL-terminated). */
enum { PALETTE_NAME_MAX = 9 };

/* Refit slots 1..n-1 (only those with mask[s] set, when mask is given) onto
   the ramp `stops_rgb` (nstops RGB triples, 0..1, darkest first). Each slot's
   place on the ramp is its luma between the darkest and brightest fitted
   slot — the Indexed Color Gradient's rule, so a preset gives the same result
   here as applied by hand. Slot 0 and unfitted slots copy through; the stored
   high bit of every word is kept. Returns the number of words that changed,
   or -1 for bad arguments. `out` may alias `src`. */
int FitPaletteToGradient(const unsigned short *src, int n, const unsigned char *mask,
                         const float *stops_rgb, int nstops, unsigned short *out);

/* Write the blood ramp into words[first .. first+count-1]: dark red rising
   to bright red with pink highlights. The same ramp Studio's blood lane uses. */
void FillBloodRamp(unsigned short *words, int first, int count);

/* `count` evenly spaced colors along a saved ramp (nstops RGB triples). */
void SampleGradientWords(const float *stops_rgb, int nstops, int count, unsigned short *out);

/* ---- Blood palettes: condense the costume, then add the blood lane ----
   MK2PALS.IMG's JCBLD_P is not JCMAIN_P with its tail painted over: the
   costume colors are packed into the low slots (JC keeps 51 of 64, KT 55)
   and the last 9-13 slots hold the gore colors — the blood ramp and, for
   torso rips, bone grays. Gore art drawn from the fighter's frames is
   remapped into that packed layout.

   One plan serves every costume. Alternate costumes share the main
   palette's slot layout (same sprites, different colors), so the plan is
   made over all of them at once: slots no sprite draws are dropped first,
   and only if that is not enough are the closest pair of slots merged, closeness
   being summed across every costume so a merge never joins two slots that
   look alike in one costume but not another. The same torso-rip sprite then
   works with every BLUD palette. */
struct BloodCondensePlan {
    int numc = 0;               /* palette size, unchanged */
    int blood = 0;              /* blood slots at the end */
    /* New costume slot j+1 takes original slot reps[j]; at most numc-blood-1. */
    std::vector<int> reps;
    unsigned char map[256] = {};    /* original slot -> new slot */
    int dropped = 0;            /* slots no sprite drew */
    int merged = 0;             /* drawn slots folded into another */
    unsigned long long changed_pixels = 0;  /* drawn pixels that would change color */
};

/* `costumes` holds `count` palettes of `n` words each; `usage` is the pixel
   count per slot over every sprite drawing any of them (null = treat every
   slot as drawn). False when the lane leaves no room (blood > n - 2). */
bool PlanBloodCondense(const unsigned short *const *costumes, int count, int n,
                       const unsigned long long *usage, int blood,
                       BloodCondensePlan &out);

/* One costume's blood palette, `plan.numc` words: slot 0, the plan's
   condensed costume colors, black filler, then `blood_words` in the last
   `plan.blood` slots. */
void BuildBloodPalette(const BloodCondensePlan &plan, const unsigned short *costume,
                       const unsigned short *blood_words, unsigned short *out);

/* The fighter prefix of a palette name: its first two characters,
   uppercased ("JCALT1_P" -> "JC"). Empty for names shorter than two. */
std::string CharacterPrefixOf(const char *pal_name);

/* prefix + suffix + "_P" ("JC", "FREZ" -> "JCFREZ_P"). Not truncated; a
   result longer than PALETTE_NAME_MAX is the caller's to reject. */
std::string EffectPaletteName(const std::string &prefix, const char *suffix);

/* prefix + "BLUD" + the last digit run of `source` + "_P":
   JCMAIN_P -> JCBLUD_P, JCALT3_P -> JCBLUD3_P. */
std::string BloodPaletteName(const std::string &prefix, const char *source);

/* True for a name this generator produces from `prefix` — one of the effect
   suffixes, or a BLUD / BLD name — so those are not offered as costumes. */
bool IsGeneratedPaletteName(const char *name, const std::string &prefix);

#endif /* PLATFORM_PALETTE_EFFECTS_H */
