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
