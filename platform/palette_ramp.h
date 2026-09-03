/*************************************************************
 * platform/palette_ramp.h
 *
 * Fit a palette ramp to one material's worth of pixels, Midway's shape: N
 * colors walking a single hue from dark to light.
 *
 * Their own "Making of Mortal Kombat" documentary shows the mechanism: a
 * digitized frame gets split into a luminance channel and a chrominance
 * channel — plain YUV, the same trick NTSC used to carry color in a
 * black-and-white signal. It shows up directly in the shipped palettes:
 * RAID1.IMG still carries the artist's working sub-palettes next to the
 * finished one. REDS (19 colors), GRAYS (25), and FLESH (24) are near-exact
 * subsets of the 64-color RADRED_P — 18/19, 24/25, and 24/24 colors match
 * verbatim — and each sub-palette is one hue walked through a luminance
 * range. Alternate costumes confirm it: RADRED_P and RADBLU_P are identical
 * at 46 of 64 slots, differing only where the cloth ramp sits — swap one
 * material's ramp and the rest of the character is untouched.
 *
 * So a ramp is fit along the population's principal color axis (its
 * chrominance direction — the axis a real material population is not
 * synthetic-gray on turns out to track luminance closely, which is the same
 * thing the documentary's split is doing), not by ordinary 3-D quantization.
 * Ordinary median-cut (see build_quantized_palette_from_rgba in img_io.cpp)
 * is still the right tool for a whole multi-hue image; this is for a single
 * material mask handed in by digitize_matte.h's decontaminated pixels, where
 * every sample is already believed to be one object's paint.
 *
 * Two fits are provided, and they are NOT interchangeable — which one to use
 * is a property of how the material was made, not a tuning knob:
 *
 *   FitRampLinear — even steps from one end of the axis to the other.
 *     Matches a HAND-DESIGNED ramp: REDS and GRAYS reproduce at 1.86 and 2.48
 *     mean 5-bit RGB error against the real thing. Use it for small, flat
 *     materials (trim, metal, an accent color) and for SYNTHESIZING a new
 *     ramp from nothing but a hue and a range — an alt-costume color with no
 *     source footage to be faithful to.
 *
 *   FitRampLloyd — weighted 1-D k-means along the axis; each color is the
 *     pixel-weighted mean of the samples nearest it, so the ramp bends to
 *     match where the footage's shading actually clusters. On Raiden's
 *     23-slot flesh block this beats the linear fit 5.17 to 9.87 mean error;
 *     on his 17-slot cloth block linear wins, 1.86 to 4.21 — flesh in real
 *     footage has a shading distribution worth chasing, a small saturated
 *     cloth ramp does not. Use it for large, gradient-heavy material
 *     populations sampled from real video — skin above all.
 *
 * Neither one dominates the other against the shipped palettes, which is the
 * honest result: this is reconstructing an unknown hand process, not solving
 * a well-posed optimization. RampPopulationRMS lets a caller measure fit
 * quality against the ACTUAL population in front of it (not against a
 * shipped answer that, for a new character, does not exist) and choose.
 *
 * Pure buffers and plain ints throughout — no g_doc, no UI globals, no undo.
 *************************************************************/
#ifndef PLATFORM_PALETTE_RAMP_H
#define PLATFORM_PALETTE_RAMP_H

/* One color's share of a material's pixel population: a packed 15-bit
   RGB555 word (the same on-disk form PAL.data_p stores, 5 bits/channel) and
   how many pixels were seen at it. Build this the way img_io.cpp's PNG
   importer already builds its median-cut histogram — quantize each source
   pixel's 8-bit RGB to 5-bit and tally — then hand only the material's own
   pixels here. Weight, not raw presence, is what makes FitRampLloyd track
   the footage: a color a thousand pixels used pulls harder than one four
   pixels used. */
struct RampSample {
    unsigned short word;
    unsigned int   weight;
};

/* A ramp color in the same 5-bit-per-channel space as RampSample::word.
   Pack with RampColorWord(), or expand with pal_word_to_rgb8 after packing. */
struct RampColor {
    unsigned char r, g, b;   /* 0..31 each */
};

/* Pack a RampColor the way PAL.data_p stores it: little-endian, XRRRRRGG
   GGGBBBBB. */
unsigned short RampColorWord(RampColor c);

/* Step `count` colors evenly along the population's principal color axis,
   from the lo_pct to hi_pct weighted percentile (0..1, e.g. 0.005/0.995 to
   trim outliers) of the projected population. See the file header for when
   this is the right fit. `samples`/`n` may be a single dominant color (axis
   degenerates to pure luminance) or even a single sample (returns that color
   `count` times, then collapses under dedup).

   Colors are deduplicated when a step rounds onto the same 5-bit color as
   its predecessor, so the returned count can be less than `count` — e.g. a
   population with less genuine color range than the slot budget asks for.
   Returns colors written into `out` (caller-sized to at least `count`). */
int FitRampLinear(const RampSample *samples, int n, int count,
                  double lo_pct, double hi_pct, RampColor *out);

/* Weighted 1-D k-means (Lloyd's algorithm) of `count` centroids along the
   population's principal axis, run to convergence or `max_iters` passes
   (<=0 uses a default that converges on any population this is meant for).
   See the file header for when this is the right fit, and RampPopulationRMS
   for measuring it against the population that produced it.

   A centroid that ends a pass with no samples nearest it is dropped, so the
   returned count can be less than `count` — a real outcome when `count`
   asks for more distinct steps than the population has weight to support,
   not a bug to route around by padding the result. Returns colors written
   into `out` (caller-sized to at least `count`). */
int FitRampLloyd(const RampSample *samples, int n, int count,
                 double lo_pct, double hi_pct, int max_iters,
                 RampColor *out);

/* Weighted RMS distance, in 5-bit-per-channel RGB units, from `samples` to
   their nearest color in `ramp` — how well `ramp` reconstructs the
   population, whether or not that population is what produced it. 0 for an
   empty population; a population with total weight 0 is treated as empty. */
double RampPopulationRMS(const RampSample *samples, int n,
                         const RampColor *ramp, int count);

#endif /* PLATFORM_PALETTE_RAMP_H */
