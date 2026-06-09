/*************************************************************
 * platform/img_util.h
 * Small pure helpers over IMG records: name extraction and the signed -> IMG
 * 16-bit word clamp used for anipoint / offset coordinates.
 *
 * Phase C of the overlay split (refactoring_plan.md). No g_doc / ImGui coupling.
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
