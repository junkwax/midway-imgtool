/*************************************************************
 * platform/anipoint.h
 * Pure anipoint predicates + sprite-name sequence parsing.
 *
 * Extracted from the overlay split. These helpers classify an IMG's secondary
 * anipoint and parse animation-sequence names
 * (e.g. JCWALK1 -> JCWALK), which the overlay's anipoint-propagation editing
 * builds on. No g_doc / ImGui / undo coupling, so they live here as standalone,
 * unit-testable logic.
 *************************************************************/
#pragma once
#include <string>
#include "img_format.h"  /* IMG */

/* True if the secondary anipoint words encode an active point rather than the
   negative "unused" sentinel (z == -1, or negative x/y). */
bool secondary_anipoint_words_in_use(unsigned short x, unsigned short y, unsigned short z);
/* As above, for an IMG's anix2/aniy2/aniz2. */
bool secondary_anipoint_in_use(const IMG *img);

/* Mark the secondary anipoint unused (sets anix2/aniy2/aniz2 to the -1
   sentinel). */
void clear_secondary_anipoint(IMG *img);
/* Make the secondary anipoint active, normalizing the -1 sentinel fields to 0
   so secondary_anipoint_in_use() reports true. */
void activate_secondary_anipoint(IMG *img);

/* Trim leading/trailing whitespace from a sprite name. */
std::string trim_sprite_name(std::string s);
/* ASCII case-insensitive string equality. */
bool ascii_iequals(const std::string &a, const std::string &b);
/* If `name` ends in digits, strip them; returns true and writes the stem. */
bool strip_trailing_sequence_digits(const std::string &name, std::string *stem_out);
/* Infer the parent/base name of an unambiguous subframe (EDHANG1A -> EDHANG1,
   BASE_1A -> BASE). Bare numeric names such as EDHANG1 need list context, so
   callers should use strip_trailing_sequence_digits() only when a real parent
   sprite exists. */
std::string InferSubframeParentName(const char *name);
