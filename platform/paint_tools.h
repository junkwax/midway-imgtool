/*************************************************************
 * platform/paint_tools.h
 * Blur, smudge, and content-aware erase for indexed sprites.
 *
 * These are RGB operations on palette-indexed data, so every one of them
 * works the same way: read indices, resolve them to the palette's 15-bit
 * colors, do the arithmetic in color space, then map the result back to the
 * nearest available index. Averaging index NUMBERS instead would produce
 * whatever colour happens to sit between two slots, which in a hand-authored
 * MK2 palette is usually an unrelated hue.
 *
 * Index 0 is transparent throughout. Blur and smudge neither read it as a
 * colour nor write over it, so a stroke near an edge softens the art without
 * bleeding the silhouette outward or eating into it. Content-aware erase is
 * the exception: writing transparency is the whole point, and it fills the
 * hole it leaves from the surrounding pixels first.
 *************************************************************/
#pragma once

#include "img_format.h"   /* IMG, PAL */

/* Soften a disc of pixels by averaging each one with its neighbours.
   `radius` is in pixels (1 = single pixel, no-op). `strength` is 0..100 and
   blends the blurred colour back toward the original, so repeated passes
   build up gradually rather than flattening in one click.
   Returns the number of pixels changed. */
int PaintBlurStamp(IMG *img, const PAL *pal, int cx, int cy,
                   int radius, int strength);

/* Drag paint from (px,py) toward (cx,cy), the way a finger pulls wet paint.
   Each pixel under the brush takes a weighted mix of its own colour and the
   colour at the same offset from the previous brush position. `strength`
   0..100 sets how much is carried. Returns pixels changed. */
int PaintSmudgeStamp(IMG *img, const PAL *pal, int px, int py,
                     int cx, int cy, int radius, int strength);

/* Erase a disc and heal it from its surroundings: the removed area is filled
   by diffusing colour inward from the ring of pixels just outside it, so a
   patch of texture closes over instead of punching a transparent hole.
   Pixels whose neighbourhood is entirely transparent become transparent —
   erasing at the edge of a sprite still trims the silhouette.
   `passes` controls how far colour is carried inward. Returns pixels changed. */
int PaintContentAwareErase(IMG *img, const PAL *pal, int cx, int cy,
                           int radius, int passes);

/* Rectangle form of the above, used by the marquee. */
int PaintContentAwareEraseRect(IMG *img, const PAL *pal,
                               int x0, int y0, int x1, int y1, int passes);
