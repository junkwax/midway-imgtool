/*************************************************************
 * platform/anipoint_mirror.h
 * Mirror-aware anipoint math: the placement the MK2 hardware/engine actually
 * performs when a sprite is drawn h-flipped, plus the derived readouts that
 * make a bad anchor visible at authoring time.
 *
 * Pure logic — no g_doc / ImGui / SDL coupling, so it unit-tests standalone.
 *
 * ---- Why there are two mirror conventions -------------------------------
 *
 * The engine mirrors art *about the anipoint*, but the two draw paths do not
 * agree on the last pixel:
 *
 *   MKDISP.ASM  ganiof   (single-part objects)
 *       subi 000010001h,a2   ; a2 = (sizey-1):(sizex-1)
 *       neg  a6              ; a6 = -anix
 *       add  a2,a6           ; a6 = (sizex-1) - anix
 *     => anix_eff = (sizex - 1) - anix
 *
 *   MKUTIL.ASM  ani2     (multipart objects, and everything reached through
 *                         flip_multi / match_ani_points)
 *       neg  a2              ; a2 = -anix
 *       move a6,a7
 *       zext a7,w            ; a7 = sizex
 *       addxy a7,a2          ; a2 = sizex - anix
 *     => anix_eff = sizex - anix
 *
 * They differ by exactly one pixel. Instrumenting MK2 in MAME
 * (tools/scorpion_scortch_mk1_test.lua) read the offset that the *multipart*
 * path applied to MK1FIRE1 (w=89, anix=-68) as 157 == w - anix, confirming the
 * ani2 form for that path; ganiof's listing is unambiguous for the other.
 *
 * imgtool's historical MirrorMarkedAnipointsToReverse() and the World View
 * mirror both used `w - x`, i.e. Ani2. That stays the default so existing
 * projects don't shift by a pixel, but the choice is now explicit and the
 * caller can ask for Ganiof when the record is drawn single-part.
 *
 * A 1 px error is invisible on a flame column and very visible on a 13 px
 * bone strip, so the tool names the routine rather than hiding a literal.
 *************************************************************/
#pragma once

/* Which engine routine's mirror math to reproduce. */
enum MirrorConvention {
    /* MKUTIL.ASM ani2 — multipart records. anix_eff = size - a. */
    MirrorConvention_Ani2 = 0,
    /* MKDISP.ASM ganiof — single-part records. anix_eff = (size - 1) - a. */
    MirrorConvention_Ganiof = 1
};

/* Short label for menus/tooltips ("ani2 (multipart)"). */
const char *mirror_convention_label(MirrorConvention conv);
/* The engine routine and file this convention was read from. */
const char *mirror_convention_source(MirrorConvention conv);

/* Mirror one axis coordinate about the sprite's own extent.
   Ani2:   size - a
   Ganiof: (size - 1) - a
   `size` is clamped to >= 1 so a degenerate record cannot produce a
   nonsensical -1 term. */
int mirror_anipoint_axis(int a, int size, MirrorConvention conv);

/* The anipoint the engine actually subtracts from the anchor when placing the
   sprite: `a` unflipped, mirror_anipoint_axis(a, size, conv) when flipped.
   left_edge = anchor - anipoint_effective(...). */
int anipoint_effective(int a, int size, bool flipped, MirrorConvention conv);

/* Signed offset of the anipoint from the art's own centre, in pixels:

       c = a - (size - 1) / 2

   This is the single number that answers "is this anchored on the art or
   beside it". For an effect meant to sit *on* its anchor, c is near 0;
   MK1FIRE1 (w=89, anix=-68) read -112.

   It negates exactly under h-flip when the same convention is used on both
   sides, which is why an offset hand-tuned for one facing can never be right
   for the other. Returned as a float because (size-1)/2 is a half-pixel for
   even-sized art. */
float anipoint_center_offset(int a, int size);

/* Screen-space twin of the above: where the drawn centre lands relative to the
   anchor, which is the negation of anipoint_center_offset(). */
float anipoint_drawn_center_from_anchor(int a, int size, bool flipped,
                                        MirrorConvention conv);

/* True when `a` falls outside the sprite's own 0..size-1 box. Not always
   wrong — plenty of legitimate art anchors off the sprite — so callers should
   present this as a soft badge, not an error. */
bool anipoint_axis_outside(int a, int size);

/* How far outside the 0..size-1 box `a` sits, in pixels. 0 when inside. */
int anipoint_axis_slack(int a, int size);

struct AnipointBoundsReport {
    bool x_outside;
    bool y_outside;
    int  x_slack;   /* pixels beyond the nearest edge, 0 when inside */
    int  y_slack;
};

/* Bounds report for a whole frame. `outside()` is x_outside || y_outside. */
AnipointBoundsReport anipoint_bounds_report(int anix, int aniy,
                                            int sizex, int sizey);

/* ASCII case-insensitive glob over sprite names, supporting '*' (any run) and
   '?' (one character). Trailing spaces in the fixed-size IMG name field are
   ignored. An empty pattern matches nothing; "*" matches everything. */
bool sprite_name_matches_glob(const char *name, const char *pattern);
