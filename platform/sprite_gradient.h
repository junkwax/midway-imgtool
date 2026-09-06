/*************************************************************
 * platform/sprite_gradient.h
 *
 * Spread a low-bit-depth sprite across a WIDER index ramp: a directional
 * gradient that assigns palette INDICES to pixels, not colors to palette
 * slots.
 *
 * Two tools already sit either side of this one and neither does the job:
 *
 *   Opacity Gradient (ui_modals.cpp) sweeps a direction across the sprite and
 *     decides, per pixel, whether the pixel survives. Same geometry as here —
 *     the six box directions plus the silhouette feather, the content bounds,
 *     the ordered-dither screens — but its only verdict is keep or clear.
 *
 *   Indexed Color Gradient (ui_palette.cpp) fits a 2-11 stop ramp onto the
 *     palette entries the art already uses. It repaints slots; every sprite
 *     pixel keeps the index it had. A 2bpp sprite has three opaque indices
 *     before and three after, so a ramp is all it can ever be — three steps.
 *
 * What is missing is the depth change. A 2bpp sprite addresses 4 indices; the
 * DMA hardware packs at whatever depth the largest index needs (dma_pack.h's
 * DmaSuperBpp, which is LOAD2's own do_superbpp scan), so writing indices up
 * to 63 is exactly what makes a sprite 6bpp. This module does that: it takes
 * the sprite's few existing tones as the SHADING, slides that shading along a
 * long ramp as a function of position, and writes the resulting index.
 *
 * The two inputs multiply rather than fight:
 *
 *   shade — where a pixel sits in its sprite's own tonal range, by the
 *     luminance of the color its index points at (dark stays dark). A 2bpp
 *     sprite gives 0, ~0.5, 1.
 *   t — where the pixel sits along the gradient's axis, 0..1.
 *
 * `shade_span` is how many ramp entries the sprite's own shading occupies: a
 * window that slides along the ramp as t advances. That single number spans
 * both behaviors a caller might want. At 1 the window is a point and the
 * gradient overwrites the modeling flat, one index per position — a silhouette
 * washed in a ramp. At `ramp_count` the window is the whole ramp, nothing is
 * left to slide, and the sprite is simply re-shaded across all of it. In
 * between — the useful setting — the art keeps its own light and dark while
 * the whole tonal window walks from one end of the ramp to the other.
 *
 * Dithering is not optional decoration here. `pos` lands between two ramp
 * entries far more often than on one, and rounding produces visible stair
 * bands across a large flat area. The ordered screens break that boundary the
 * same way the era's art did, and for the same reason: no blending hardware.
 *
 * Pure buffers and plain ints throughout — no g_doc, no UI globals, no undo,
 * no SDL. Unit-tested in test/sprite_gradient_test.cpp.
 *************************************************************/
#ifndef PLATFORM_SPRITE_GRADIENT_H
#define PLATFORM_SPRITE_GRADIENT_H

#include <vector>

/* Gradient axis. The first six measure across the bounds box; the seventh
   measures inward from the silhouette itself, which is what a rim light or an
   inner glow needs — a bounding box cannot follow a shape. Values match the
   order the dialog lists them in. */
enum {
    kSpriteGradientLeftRight = 0,
    kSpriteGradientRightLeft,
    kSpriteGradientTopBottom,
    kSpriteGradientBottomTop,
    kSpriteGradientCenterEdge,
    kSpriteGradientEdgeCenter,
    kSpriteGradientEdgeInward,
    kSpriteGradientDirectionCount
};

/* How a position landing between two ramp entries is resolved. */
enum {
    kSpriteGradientDitherNone = 0,   /* round to nearest — bands, but exact */
    kSpriteGradientDitherNoise,      /* hashed per pixel; grain, no structure */
    kSpriteGradientDitherChecker,    /* 2x2 ordered */
    kSpriteGradientDitherBayer       /* 4x4 ordered */
};

struct SpriteGradientOptions {
    int  direction = kSpriteGradientTopBottom;
    int  ramp_start = 1;      /* first palette index of the ramp block */
    int  ramp_count = 2;      /* how many indices the ramp occupies, >= 2 */
    int  shade_span = 4;      /* ramp entries the sprite's own shading spans */
    int  start_pct = 0;       /* where the window sits at t=0, 0..100 of travel */
    int  end_pct = 100;       /* where it sits at t=1 */
    int  dither = kSpriteGradientDitherBayer;
    int  seed = 17;           /* noise dither only */
    bool content_bounds = true;  /* span the opaque bounds, not the padded canvas */
    /* For the box directions: confine the whole sweep to this many pixels from
       the edge it starts at, leaving deeper pixels untouched (0 = full span).
       For kSpriteGradientEdgeInward it is the feather width, and is always
       used — that direction has no other scale. */
    int  band_px = 0;
};

/* Everything the per-pixel decision needs, resolved once per sprite, so a live
   preview and the real apply cannot drift apart. Build it, then either walk it
   yourself with SpriteGradientIndexAt (preview) or hand it to
   SpriteGradientApply (commit). */
struct SpriteGradientPlan {
    SpriteGradientOptions opt;
    int   w = 0, h = 0, stride = 0;
    int   min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    int   shade_count = 0;        /* distinct opaque indices the sprite uses */
    float shade_lut[256] = {};    /* index -> 0..1 tonal position, -1 if absent */
    std::vector<float> edge_dist; /* kSpriteGradientEdgeInward only */
};

/* Tonal position, 0..1, of every palette index the sprite actually uses,
 * ranked by the luminance of the color it points at and normalized over the
 * range present. Indices the sprite never uses get -1. An index pointing past
 * `pal_numc` is treated as black.
 *
 * Luminance-linear rather than by rank: a 2bpp sprite whose three tones are
 * near-black, near-black and white should not have its two dark tones pushed a
 * third of the ramp apart. Midway's own ramps are luminance-ordered (see
 * palette_ramp.h), so this is the same axis the destination ramp runs along.
 *
 * A sprite whose used indices all share one luminance gets 0.5 for each: no
 * modeling to preserve, so it rides the middle of the window.
 *
 * Returns the number of distinct opaque indices found. */
int SpriteGradientBuildShadeLut(const unsigned char *pixels, int w, int h, int stride,
                                const unsigned short *pal_words, int pal_numc,
                                float lut[256]);

/* Resolve `opt` against one sprite. Returns false — and leaves `out` unusable
 * — for an empty sprite, or for a fully transparent one when content_bounds is
 * set (there is no content to span). Options are clamped into range here, so
 * `plan.opt` is the truth about what will happen, not the caller's raw input. */
bool SpriteGradientPlanBuild(const unsigned char *pixels, int w, int h, int stride,
                             const unsigned short *pal_words, int pal_numc,
                             const SpriteGradientOptions &opt,
                             SpriteGradientPlan *out);

/* The index this pixel should end up with.
 *
 * Transparent stays transparent: index 0 always returns 0, so the silhouette
 * never changes shape. A pixel outside the band (deeper than band_px, or past
 * the feather width) returns `src_index` unchanged — "leave it alone" has to
 * mean that, or a narrow rim ramp would repaint the whole interior.
 *
 * `image_idx` only seeds the noise dither, so that two sprites in one
 * animation do not receive the identical grain; pass anything stable. */
int SpriteGradientIndexAt(const SpriteGradientPlan &plan, int x, int y,
                          unsigned char src_index, int image_idx);

/* Write the gradient into `pixels` in place. Returns pixels whose index
 * actually changed. */
int SpriteGradientApply(unsigned char *pixels, const SpriteGradientPlan &plan,
                        int image_idx);

/* Highest index the plan can write, given the sprite's own indices survive
 * outside the band. Callers report the resulting pack depth from this. */
int SpriteGradientMaxIndex(const unsigned char *pixels, const SpriteGradientPlan &plan,
                           int image_idx);

/* ---- Ramp construction and placement -------------------------------- */

/* Interpolate `stop_count` RGB stops (3 floats each, 0..1, in ramp order) into
 * `count` packed 15-bit palette words — the same on-disk form PAL.data_p
 * stores, and the same piecewise-linear walk the Indexed Color Gradient's
 * preview bar draws, so a ramp looks in the palette exactly like it looked in
 * the dialog. Returns words written, or 0 on bad input. */
int SpriteGradientBuildRamp(const float *stops_rgb, int stop_count,
                            int count, unsigned short *out_words);

/* Lowest contiguous run of `count` indices in [1, max_index] that no sprite
 * references, or -1 if the depth ceiling has no room for one.
 *
 * `used` is per-index "some sprite sharing this palette draws with it", index
 * 0 included (transparent, never allocatable). Lowest-first placement is the
 * point, not a tiebreak: the pack depth follows the LARGEST index present, so
 * a ramp parked at 40 when 4 was free would cost a bit of depth for nothing.
 *
 * `max_index` is (1 << target_bpp) - 1. Asking for a block that would push the
 * sprite past the depth the caller intends is a refusal, not a clamp — the
 * caller wanted 6bpp, and silently shipping 7 is worse than saying no. */
int SpriteGradientFindRampBlock(const bool used[256], int count, int max_index);

/* Longest free run available in [1, max_index] — what to offer as the ramp
 * size before the user narrows it. 0 when nothing is free. */
int SpriteGradientLargestFreeRun(const bool used[256], int max_index);

#endif /* PLATFORM_SPRITE_GRADIENT_H */
