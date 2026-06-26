/*************************************************************
 * platform/sprite_resize_ops.h
 * Pixel resampling helpers for full-sprite resize operations.
 *
 * These helpers allocate new indexed-pixel buffers with PoolAlloc and touch
 * no UI state. The caller owns undo, IMG metadata updates, hitbox updates,
 * texture invalidation, and freeing/replacing the old pixel buffer.
 *************************************************************/
#pragma once
#include "img_format.h"

struct ResizeRgb {
    unsigned char r, g, b;
};

/* Build a 256-entry RGB lookup for resize sampling. `fallback_rgb` supplies
   colors for indices not present in `pal`; pass NULL to use black. */
void BuildResizePalette(PAL *pal, const ResizeRgb fallback_rgb[256],
                        ResizeRgb out[256]);

/* Nearest-neighbor resize in palette-index space. Returns a PoolAlloc buffer
   and writes its padded row stride to `out_stride`. */
unsigned char *ResizeSpritePixelsNearest(const IMG *img, int nw, int nh,
                                         unsigned int *out_stride);

/* RGB-area/bilinear resize with nearest palette remap. `optimize_bytes`
   raises the transparency threshold to favor smaller LOAD2-packed output. */
unsigned char *ResizeSpritePixelsQuality(const IMG *img, PAL *pal,
                                         const ResizeRgb fallback_rgb[256],
                                         int nw, int nh, bool optimize_bytes,
                                         unsigned int *out_stride);

/* Same RGB-area/bilinear resize, but for callers that hold a raw indexed
   buffer rather than an IMG (e.g. the clipboard) and have already built the
   256-entry RGB table themselves. Returns NULL if pal_count <= 1; caller
   should fall back to nearest-neighbor in that case. */
unsigned char *ResizeIndexedPixelsQuality(const unsigned char *src, int src_stride,
                                          int sw, int sh,
                                          const ResizeRgb pal_rgb[256], int pal_count,
                                          int nw, int nh, bool optimize_bytes,
                                          unsigned int *out_stride);

/* Single-point bilinear RGB sample at a possibly-fractional source
   coordinate, remapped to the nearest palette index. Used for rotated
   transforms where each destination pixel maps to an arbitrary
   (non-grid-aligned) source point. Returns 0 (transparent) if the blended
   alpha falls below threshold, or if (ux,uy) falls outside the source. */
unsigned char SampleIndexedBilinear(const unsigned char *src, int src_stride,
                                    int sw, int sh,
                                    const ResizeRgb pal_rgb[256], int pal_count,
                                    double ux, double uy, bool optimize_bytes);
