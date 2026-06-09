/*************************************************************
 * platform/palette_math.h
 * Pure 15-bit palette-word color math helpers.
 *
 * Extracted from imgui_overlay.cpp as the first step of the
 * overlay modularization (see refactoring_plan.md, Phase 2:
 * decouple business logic from UI). These functions operate
 * purely on raw palette buffers / PAL structs and packed
 * 15-bit (5/5/5) color words — they touch no UI global state,
 * so they live here as a standalone, unit-testable unit.
 *************************************************************/
#pragma once
#include "img_format.h"   /* PAL, rgb8_to_pal_word */

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
