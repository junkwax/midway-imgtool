/*************************************************************
 * platform/subframe_align.h
 *
 * Re-derive a subframe's anipoint from its parent's art.
 *
 * Auto-Chop and Body Split create a child with
 *     child->anix = parent->anix - piece_offset_x
 *     child->aniy = parent->aniy - piece_offset_y
 * and that difference is the *only* record of where the piece sits inside the
 * parent. img_io.cpp's Bulk Restore reads it back the same way to copy parent
 * pixels into a child. Nothing re-checks it, so once a parent is moved without
 * its children -- or a child is hand-edited, or pieces are re-chopped from
 * different art -- the stored offset and the actual picture disagree, and
 * there is no way to notice short of looking.
 *
 * The art itself is the ground truth: the child's pixels came out of the
 * parent's bitmap, so finding where they sit inside it recovers the true
 * offset, and the correct anipoint follows. The search reuses stamp_erase's
 * matcher, which exists for the same shape of problem (locate one sprite's
 * pixels inside another's) and is already measured against real Midway art.
 *
 * Pure: indexed buffers and ints, no g_doc / IMG / UI, so it unit-tests
 * standalone.
 *************************************************************/
#ifndef PLATFORM_SUBFRAME_ALIGN_H
#define PLATFORM_SUBFRAME_ALIGN_H

#include "stamp_erase.h"  /* StampBuf, StampMatch */

struct SubframeAlign {
    bool valid;      /* a placement was found and scored */
    int  off_x;      /* child pixel (0,0) sits at parent pixel (off_x, off_y) */
    int  off_y;
    int  new_anix;   /* parent_anix - off_x */
    int  new_aniy;   /* parent_aniy - off_y */
    int  matched;    /* opaque child pixels whose index equals the parent's */
    int  total;      /* opaque pixels in the child */
    bool exact;      /* every opaque child pixel matched */

    SubframeAlign()
        : valid(false), off_x(0), off_y(0), new_anix(0), new_aniy(0),
          matched(0), total(0), exact(false) {}
};

/* Fraction of the child's opaque pixels found at their expected index, 0..1. */
float subframe_align_score(const SubframeAlign &a);

/* The offset the stored anipoints currently claim:
       (parent_anix - child_anix, parent_aniy - child_aniy) */
void subframe_claimed_offset(int parent_anix, int parent_aniy,
                             int child_anix, int child_aniy,
                             int *out_x, int *out_y);

/* Locate `child` inside `parent` and report the anipoint that placement
   implies.

   The claimed offset is scored first, so art that is already correct returns
   it unchanged and costs one comparison. Otherwise every placement that fits
   the child wholly inside the parent is scored -- the chop invariant, and a
   far smaller space than a free search. A child too large to fit in either
   axis falls back to stamp_erase's content-driven search, which can place a
   piece that overhangs.

   Returns valid=false when the child has no opaque pixels at all. */
SubframeAlign subframe_align_from_parent(const StampBuf &parent,
                                         int parent_anix, int parent_aniy,
                                         const StampBuf &child,
                                         int child_anix, int child_aniy);

#endif /* PLATFORM_SUBFRAME_ALIGN_H */
