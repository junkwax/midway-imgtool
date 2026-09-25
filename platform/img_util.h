/*************************************************************
 * platform/img_util.h
 * Small pure helpers over IMG records: name extraction and the signed -> IMG
 * 16-bit word clamp used for anipoint / offset coordinates.
 *
 * Extracted from the overlay split. No g_doc / ImGui coupling.
 *************************************************************/
#pragma once
#include <string>
#include "img_format.h"  /* IMG */

/* The IMG's name as a std::string. n_s is a fixed-size, possibly
   non-NUL-terminated char field; returns "" for a null IMG. */
std::string img_name_string(const IMG *img);

/* Clamp a signed value into [-32768, 32767] and pack it as the 16-bit word
   used for IMG anipoint / offset fields. */
unsigned short signed_to_img_word(int v);

/* The next file in a numbered series, for Split Marked: "CAGE4.IMG" ->
   "CAGE5.IMG", "CAGE09.IMG" -> "CAGE10.IMG", "CAGE.IMG" -> "CAGE2.IMG". Any
   leading directory and the extension are kept (".IMG" is used when there is
   none). The stem stays within DOS 8.3's eight characters: when the bigger
   number no longer fits, letters are dropped from the end of the prefix
   ("ABCDEFG9" -> "ABCDEF10"). `step` (1 or more) skips further along the series,
   so a caller can walk past names that already exist on disk. */
std::string next_sequential_img_name(const std::string &name, int step = 1);
