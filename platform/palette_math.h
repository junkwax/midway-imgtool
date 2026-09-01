/*************************************************************
 * platform/palette_math.h
 * Pure 15-bit palette-word color math helpers.
 *
 * Extracted from imgui_overlay.cpp while decoupling business logic from UI.
 * These functions operate purely on raw palette buffers / PAL structs and
 * packed 15-bit (5/5/5) color words — they touch no UI global state, so they
 * live here as a standalone, unit-testable unit.
 *************************************************************/
#pragma once
#include "img_format.h"   /* PAL, rgb8_to_pal_word */

/* Bits per pixel needed to index `numc` palette entries: ceil(log2(numc)),
   clamped to [1, 8]. This is the value a PAL's `bitspix` should carry — an
   imported 41-color palette is 6bpp art, not 8bpp, and packing it as 8 wastes
   ROM in the TBL/IRW/LOAD2 exports that read bitspix. */
int PaletteBppForColorCount(int numc);

/* Entries addressable at `bpp` bits: 1 << bpp, clamped to [2, 256]. */
int PaletteColorCountForBpp(int bpp);

/* True when `bitspix` cannot address all `numc` entries — i.e. the palette's
   declared depth is too small for its own color count. */
bool PaletteBppTooSmall(int bitspix, int numc);

/* Read the packed 15-bit color word at palette index `idx` from a raw
   2-bytes-per-entry palette buffer. No bounds checking. */
unsigned short palette_word_at(const unsigned char *data, int idx);

/* Squared distance between two packed 15-bit (5/5/5) color words, measured
   in RGB component space. Used for nearest-color matching. */
int palette_word_distance_sq(unsigned short a, unsigned short b);

/* Find the palette index in `target_pal` whose color is nearest to
   `src_word`. Returns 0 (transparent) if the palette is empty/invalid;
   index 0 is treated as transparent and never matched. */
unsigned char nearest_palette_index_for_word(unsigned short src_word,
                                             const PAL *target_pal);

/* Packed 15-bit word at `idx`, or 0 ("black"/transparent) when the
   palette or index is invalid. */
unsigned short pal_word_or_black(PAL *pal, int idx);

/* Convert 8-bit RGB to a packed 15-bit palette word. */
unsigned short rgb_to_word15(unsigned char r, unsigned char g, unsigned char b);

/* Squared 5/5/5 channel distance between two packed words. Equivalent to
   palette_word_distance_sq; kept under this name for the slot-selection call
   sites that pair with the perceptual variant below. */
int PaletteColorDistance5(unsigned short a, unsigned short b);

/* Distance used for slot SELECTION. With `perceptual` on, squared channel
   diffs are luma-weighted (R 0.30, G 0.59, B 0.11) so matches favor the
   colors the eye is most sensitive to. */
int PaletteColorDistance5W(unsigned short a, unsigned short b, bool perceptual);

/* Nearest non-transparent slot [1..numc) in `pal` to `color_word`, by
   PaletteColorDistance5. Returns 0 when the palette is empty/invalid. */
int FindNearestPaletteSlot(const PAL *pal, unsigned short color_word);

/* Nearest slot in the target as it will look after growth: original target
   colors [1..base_count) plus queued additions [base_count..+added_count).
   Returns a final-space slot index, or 0 only when there is no usable color. */
int FindNearestMergedSlot(const PAL *target, int base_count,
                          const unsigned short *added, int added_count,
                          unsigned short color_word, bool perceptual);

/* ---- Tool tolerance ----
   The index-image tools (magic wand, paint bucket, smart eraser, smart remap)
   all ask the same question: "is this pixel close enough to the one the user
   clicked?" Answering it by subtracting palette INDEX numbers is wrong — the
   slots either side of a background's black hold whatever the artist put
   there, usually the sprite's own dark shading — so the comparison belongs in
   color space.

   PaletteToleranceDistSq maps a tool's tolerance slider onto a squared
   distance in 5-bit channel space: the slider is a radius at half scale, so
   0 means "exactly this color" and each 2 notches widen the match by one
   5-bit step per channel. (The widest meaningful radius is 31*sqrt(3), about
   54, so even a slider pinned at 64 stops short of matching everything.) */
int PaletteToleranceDistSq(int tolerance);

/* True when slot `ci` is within `tolerance` of slot `target_ci`, by color.
   Slot 0 is transparent whatever color it stores — almost always black — so a
   match never crosses between transparent and opaque, which is what kept the
   wand from eating a sprite's black outline when asked for its transparent
   background. Falls back to comparing index numbers only when `pal` is
   missing or empty and there are no colors to compare. */
bool PaletteIndexWithinTolerance(PAL *pal, int target_ci, int ci, int tolerance);

/* ---- Palette index isolation ----
   Work out how to give one region of a sprite exclusive ownership of the
   palette indices it draws with, so those indices can be recolored without
   dragging the rest of the art along — Baraka's blades without his teeth.

   The plan repoints the pixels OUTSIDE the region; the region itself never
   moves. For each contested index (owned by the region, also drawn outside
   it) a destination slot is chosen, cheapest first:

     1. a slot already holding that exact color   — free,
     2. a slot no sprite on the palette draws with — costs a color, no slot,
     3. a freshly appended duplicate               — grows numc,
     4. the nearest color the region does not own  — only once the palette is
        full, and the only outcome where the art actually changes.

   This is pure planning: nothing is written, the palette is not grown, and no
   pixel is touched. The caller grows the palette to `numc`, performs the
   `copy_from` color writes, then rewrites its outside pixels through `dest`. */
struct PaletteIsolatePlan {
    /* dest[i] is the slot that outside pixels of index i must become, or -1
       when index i is not contested and should be left exactly as it is. */
    int dest[256];
    /* copy_from[d] is the index whose color slot d has to be given before the
       plan is valid, or -1 when d already holds the right color. Sources are
       always reserved indices, which are never themselves destinations, so
       these writes are safe to apply in any order. */
    int copy_from[256];
    int numc;           /* palette size the plan needs; >= the numc passed in */
    int matched;        /* destinations that cost nothing at all */
    int recycled;       /* destinations taken from slots nothing was drawing */
    int appended;       /* destinations added past the old end of the palette */
    int approximated;   /* destinations that change the color of the art */
};

/* Fill `out` with the isolation plan for `words` (2-bytes-per-entry, `numc`
   colors). `reserved` marks every index the region draws with; `contested`
   marks the subset of those also drawn outside it; `live` marks every index
   any sprite on this palette draws with, so a slot that is merely unused can
   be recycled without disturbing another frame. Index 0 is transparent and is
   never reserved, recycled, or handed out. `max_numc` caps growth (256). */
void PlanPaletteIsolation(const unsigned char *words, int numc,
                          const bool *reserved, const bool *contested,
                          const bool *live, int max_numc,
                          PaletteIsolatePlan *out);
