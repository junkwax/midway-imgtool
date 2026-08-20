/*************************************************************
 * platform/anipoint_edit.h
 * Anipoint editing with animation-sequence propagation.
 *
 * Extracted from the overlay split. Unlike the pure anipoint module, these
 * helpers mutate the active document (g_doc), push undo, mark
 * dirty, and invalidate thumbnails — so they sit a layer up, atop anipoint +
 * img_util + the ui_internal / ui_timeline services.
 *
 * Setting an anipoint here propagates the same delta to every other frame in
 * the same numbered animation sequence (e.g. JCWALK1 -> JCWALK2 ...), coalesced
 * into a single undo step for the duration of a drag.
 *************************************************************/
#pragma once
#include "img_format.h"  /* IMG */
#include <vector>

struct Document;

/* Begin (or continue) a sequence anipoint edit as one coalesced undo step.
   Returns false if the undo snapshot could not be pushed. */
bool begin_sequence_anipoint_edit(void);
/* End the coalesced undo step once the mouse / widget interaction goes idle. */
void finish_sequence_anipoint_edit_if_idle(void);

/* Set img's primary anipoint to (new_ax,new_ay), propagating the delta across
   its animation sequence. Returns true if anything changed. */
bool set_primary_anipoint_with_sequence(IMG *img, int new_ax, int new_ay);
/* As above for the secondary anipoint (activating it first if needed). */
bool set_secondary_anipoint_with_sequence(IMG *img, int new_ax2, int new_ay2);

/* Local-only variants for numeric/property-panel edits. These still coalesce
   undo while the widget is active, but they only touch the selected image. */
bool set_primary_anipoint_local(IMG *img, int new_ax, int new_ay);
bool set_secondary_anipoint_local(IMG *img, int new_ax2, int new_ay2);
bool set_secondary_anipoint_z_local(IMG *img, int new_az2);
bool clear_secondary_anipoint_local(IMG *img);

/* Shift every direct subframe of `parent` inside `doc` by (dx,dy).

   A subframe is an image whose InferSubframeParentName() resolves to
   `parent`'s own name -- UGSPARK11 owns UGSPARK11A and UGSPARK11B and nothing
   else. That is precisely the relation Auto-Chop and Body Split establish when
   they derive child->aniy = parent->aniy - piece_offset_y, so a parent that
   moves without its children pulls the composite apart by exactly the delta.

   Deliberately narrower than set_primary_anipoint_with_sequence(), which
   matches on the numbered stem and would sweep in every sibling frame too.

   Does not touch `parent` itself, push undo, or set the dirty flag -- the
   caller already owns those for the parent's own edit. Returns the number of
   children moved. */
int shift_subframe_anipoints(Document *doc, const IMG *parent, int dx, int dy);

/* How many direct subframes `parent` has in `doc`. */
int count_subframes(Document *doc, const IMG *parent);

/* The IMG whose name `child` declares itself a piece of (UGSPARK1A -> UGSPARK1),
   or NULL when `child` is not a subframe or that parent is not in `doc`. */
IMG *find_subframe_parent(Document *doc, const IMG *child);

/* Image-list indices of `parent`'s direct subframes in `doc`, in list order.
   Empty when `parent` has none. */
void collect_subframe_indices(Document *doc, const IMG *parent,
                              std::vector<int> *out);

struct SubframeRecalcReport {
    int considered;    /* subframes examined */
    int changed;       /* anipoints actually rewritten */
    int exact;         /* placed on a pixel-perfect match */
    int approximate;   /* best placement was only a partial match */
    int unplaced;      /* empty or unscorable -- left alone */

    SubframeRecalcReport()
        : considered(0), changed(0), exact(0), approximate(0), unplaced(0) {}
};

/* Re-derive every direct subframe's anipoint from where its art actually sits
   inside `parent`'s bitmap, rather than trusting the stored offset.

   This is the repair for a composite that came apart: the parent is the
   authority, the pieces are located in it by their own pixels, and each one's
   anipoint is set to parent_anipoint - found_offset. Already-correct pieces
   score their stored offset first and are left untouched, so running it is
   idempotent and safe on a healthy file.

   Pushes one undo step and marks `doc` dirty only if something changed. */
SubframeRecalcReport recalc_subframe_anipoints_from_parent(Document *doc,
                                                           IMG *parent);
