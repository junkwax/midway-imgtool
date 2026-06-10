/*************************************************************
 * platform/anipoint_edit.h
 * Anipoint editing with animation-sequence propagation.
 *
 * Phase C of the overlay split (refactoring_plan.md). Unlike the pure anipoint
 * module, these helpers mutate the active document (g_doc), push undo, mark
 * dirty, and invalidate thumbnails — so they sit a layer up, atop anipoint +
 * img_util + the ui_internal / ui_timeline services.
 *
 * Setting an anipoint here propagates the same delta to every other frame in
 * the same numbered animation sequence (e.g. JCWALK1 -> JCWALK2 ...), coalesced
 * into a single undo step for the duration of a drag.
 *************************************************************/
#pragma once
#include "img_format.h"  /* IMG */

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
