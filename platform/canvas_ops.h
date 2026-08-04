/*************************************************************
 * platform/canvas_ops.h
 * Pure canvas-geometry and cross-palette paste helpers.
 *
 * These back three editor operations that all reduce to "move indexed pixels
 * around a buffer without resampling them", plus the slot bookkeeping a paste
 * needs when the clipboard came from a different palette:
 *
 *   - Canvas Size: grow/crop the frame around art that must not change size.
 *   - Content nudge: slide the art inside its own canvas.
 *   - Clear Selection: blank the pixels inside a marquee/mask.
 *   - Paste color import: park the clipboard's missing colors in the target
 *     palette's free indices so the paste keeps its real colors instead of
 *     being remapped to the nearest existing ones.
 *
 * Everything here operates on raw buffers and plain ints — no g_doc, no UI
 * globals, no undo — so it is unit-testable on its own.
 *************************************************************/
#pragma once
#include <vector>
#include "img_format.h"   /* PAL */

/* Which part of the old canvas keeps its position when the canvas is
   resized. Ordered row-major so the 3x3 button grid in the dialog can index
   it directly as (row * 3 + col). */
enum CanvasAnchor {
    CanvasAnchor_TopLeft = 0,
    CanvasAnchor_TopCenter,
    CanvasAnchor_TopRight,
    CanvasAnchor_MiddleLeft,
    CanvasAnchor_Center,
    CanvasAnchor_MiddleRight,
    CanvasAnchor_BottomLeft,
    CanvasAnchor_BottomCenter,
    CanvasAnchor_BottomRight,
    CanvasAnchor_Count
};

/* Where the old art's top-left lands inside the new canvas. Negative values
   mean the new canvas is smaller and that edge gets cropped. Extra odd pixels
   on a centered axis go to the left/top, matching Photoshop. */
void CanvasAnchorOffset(int old_w, int old_h, int new_w, int new_h,
                        int anchor, int *out_dx, int *out_dy);

/* Copy `src` into `dst` shifted by (dx, dy), clipping whatever falls outside.
   `dst` must already be zero-filled — pixels with no source stay transparent. */
void CanvasBlitIndexedOffset(const unsigned char *src, int src_w, int src_h,
                             int src_stride,
                             unsigned char *dst, int dst_w, int dst_h,
                             int dst_stride,
                             int dx, int dy);

/* Bounding box of the non-transparent (index != 0) pixels. Returns false and
   leaves the outputs untouched when the buffer is entirely transparent. */
bool CanvasIndexedContentBounds(const unsigned char *src, int w, int h,
                                int stride,
                                int *out_min_x, int *out_min_y,
                                int *out_max_x, int *out_max_y);

/* Trim a nudge so the art never walks off its own canvas: a nudge is meant to
   position art, not to shave it. Content already hanging off an edge is not
   pushed further out, but is still allowed to move back in. */
void CanvasClampContentNudge(int min_x, int min_y, int max_x, int max_y,
                             int canvas_w, int canvas_h,
                             int *dx, int *dy);

/* Blank (set to index 0) every pixel inside the inclusive rect. When `mask` is
   non-NULL only pixels with a nonzero entry are cleared, so lasso/wand
   selections erase their real shape rather than their bounding box. Returns
   how many pixels actually changed. */
int CanvasClearIndexedRect(unsigned char *buf, int w, int h, int stride,
                           int x1, int y1, int x2, int y2,
                           const unsigned char *mask, int mask_w, int mask_h);

/* ---- Cross-palette paste color import -------------------------------
   A paste between palettes normally remaps every pixel to the nearest color
   the target already has, which is the only safe thing to do when the target
   is full. It usually isn't: a 6bpp palette addressing 64 slots but holding
   42 colors has 22 indices nobody is using, and the pasted art's real colors
   fit there exactly. Planning the import separately from applying it keeps the
   "which slots are free, which colors are missing" decision testable and lets
   the caller report what it is about to do. */

struct PaletteImportSlot {
    unsigned char src_index;   /* index in the clipboard's palette */
    unsigned char dst_index;   /* index it will occupy in the target */
    unsigned short word;       /* packed 15-bit color being copied */
};

struct PaletteImportPlan {
    std::vector<PaletteImportSlot> added;
    int new_numc = 0;    /* target color count after the import */
    int matched = 0;     /* colors the target already held exactly */
    int unmatched = 0;   /* colors with no free slot — nearest-remapped as before */
};

/* Decide which of the clipboard's colors to copy into the target palette.
 *
 *   src_used     — which source indices the pasted pixels actually reference.
 *                  Index 0 is transparent and is never imported.
 *   dst_capacity — how many indices the target may address (1 << bitspix),
 *                  which is what caps growth: widening a palette past its own
 *                  declared depth would change how every export packs it.
 *   dst_free     — per-index "this slot is safe to overwrite" for indices
 *                  below dst_numc, i.e. no sprite in the document references
 *                  it. Slots at or above dst_numc are always free; pass NULL
 *                  to use only those.
 *
 * Slots are handed out low-index first past the end of the palette, then from
 * the reusable holes, so the common case only appends. */
PaletteImportPlan PlanPaletteColorImport(const unsigned char *src_pal, int src_numc,
                                         const bool src_used[256],
                                         const unsigned char *dst_pal, int dst_numc,
                                         int dst_capacity,
                                         const bool *dst_free);

/* Which palette indices a run of indexed pixels references. `used[0]` is set
   for transparent pixels like any other index; callers that only care about
   opaque colors should ignore it. */
void CanvasCollectUsedIndices(const unsigned char *buf, int w, int h, int stride,
                              bool used[256]);
