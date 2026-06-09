/*************************************************************
 * platform/image_ops.h
 * Pure indexed-image analysis helpers (edge / stroke detection).
 *
 * Extracted from imgui_overlay.cpp (refactoring_plan.md, Phase A). These
 * helpers operate on raw indexed-pixel buffers and PAL color data; they touch
 * no UI globals, document state, or undo history. They back the Strip Edge and
 * Hard Stroke Remover operations, whose g_doc-walking wrappers stay in the
 * overlay.
 *************************************************************/
#pragma once
#include "img_format.h"   /* PAL */

/* Approximate 0..255 luma of a packed 15-bit (5/5/5) color word. */
int StrokeWordLuma8(unsigned short w);

/* True if (x,y) is outside the image or holds the transparent index 0. */
bool EdgeBufferTransparent(const unsigned char *buf, int w, int h,
                           int stride, int x, int y);

/* Count of the 8 neighbors of (x,y) that are transparent (out-of-bounds
   counts as transparent). */
int EdgeBufferTransparentNeighbors(const unsigned char *buf, int w, int h,
                                   int stride, int x, int y);

/* True if candidate index `cand_ci` differs strongly enough from edge index
   `edge_ci` (by RGB555 distance or luma delta) to serve as a replacement.
   Indices of 0 and equal indices never qualify. */
bool EdgeColorStrongVariant(PAL *pal, unsigned char edge_ci,
                            unsigned char cand_ci);

/* Search inward from an edge pixel at (x,y) for the best non-transparent,
   non-edge replacement color, preferring strongly-contrasting candidates.
   Writes the chosen index to *out_ci and returns true on success. */
bool FindInwardEdgeReplacement(const unsigned char *src,
                               int w, int h, int stride,
                               int x, int y, PAL *pal,
                               unsigned char edge_ci,
                               int max_depth,
                               unsigned char *out_ci);
