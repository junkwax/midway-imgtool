/*************************************************************
 * platform/material_mask.h
 *
 * Split a digitized character into the material regions Midway built one
 * palette ramp per: cloth, skin, metal, hair, and so on (see palette_ramp.h
 * for what happens with each region once it is separated).
 *
 * "The Making of Mortal Kombat" shows the mechanism directly: a digitized
 * frame is split into a LUMINANCE channel and a CHROMINANCE channel, the way
 * NTSC carries color in a black-and-white signal. That split is the reason
 * this segments on chrominance DIRECTION rather than raw RGB distance. A
 * material is one hue shaded across a luminance range (that's what a
 * palette_ramp.h ramp fits), so two pixels of the same red cloth in full
 * light and in shadow can be far apart in RGB and even in raw chrominance
 * MAGNITUDE — shading scales a pixel's whole chroma vector toward the origin
 * — while the ANGLE between their chroma vectors barely moves. Matching on
 * that angle (cosine similarity) is what makes a fill or a classification
 * survive shading instead of fragmenting at every shadow line.
 *
 * A pixel with very little chroma magnitude of its own — near-black, near-
 * white, near-gray — carries almost no hue evidence, so its angle to any
 * seed is noise, not signal. Below `noise_floor` a pixel matches
 * unconditionally rather than being tested. This is not a fallback bolted on
 * to cover a weakness: it is why a genuinely gray/black material (RAID1.IMG's
 * own GRAYS sub-palette) segments correctly under this same rule with no
 * special case — a desaturated material floods because everything in it
 * fails the noise floor, not because it happens to share one hue.
 *
 * Two entry points, for two different problems:
 *
 *   FloodFillMaterial     — one frame, one click: grow a region outward by
 *                           connectivity AND hue from a seed pixel. This is
 *                           the tool a person uses on the first frame of a
 *                           new character, and to touch up any frame by hand.
 *
 *   ClassifyMaterialsByHue — a later frame, no click: MK2's own three-frame
 *                           idle stance ships at 71x134, 71x133, and 71x132
 *                           — three different sizes for "the same" pose — so
 *                           there is no spatial mask to carry frame to
 *                           frame. What DOES carry over, because the shipped
 *                           palettes show each material keeps one hue for
 *                           the whole character, is the hue itself. This
 *                           reclassifies a new frame from the hues already
 *                           learned, independent of where anything sits.
 *
 * Pure buffers and plain ints throughout — no g_doc, no UI globals, no undo.
 * `rgb`/`alpha` are meant to be digitize_matte.h's decontaminated output, not
 * a raw flattened composite — segmenting before decontamination reintroduces
 * exactly the halo-color noise this module needs to not be there.
 *************************************************************/
#ifndef PLATFORM_MATERIAL_MASK_H
#define PLATFORM_MATERIAL_MASK_H

/* Tuning for the hue-match test both entry points share. */
struct MaterialSeedParams {
    /* Max angle, in degrees, between a candidate pixel's chroma direction
       and the reference's before it is rejected as a different material.
       0..180; smaller is stricter. */
    double tolerance_deg;

    /* Chroma magnitude (roughly 0..208 for 8-bit RGB — a fully saturated
       primary sits near 208, gray sits at 0) below which a pixel is treated
       as carrying no usable hue evidence and matches unconditionally. See
       the file header for why this is not a special case. */
    int noise_floor;
};

/* tolerance_deg = 30, noise_floor = 14 — tight enough to keep Raiden's
   flesh, cloth, and grays apart (their reference hues are well over 30
   degrees apart), loose enough to ride out compression noise in a single
   video-grain pixel. */
MaterialSeedParams MaterialSeedParamsDefault();

/* Flood-fill outward from (seed_x, seed_y) over 4-connected opaque
 * (alpha > 0) pixels whose chrominance direction matches the pixel AT THE
 * SEED — the reference is fixed for the whole call, not path-dependent, the
 * same way the existing index-space PaintBucketFill behaves.
 *
 * Writes `material_id` into `out_mask` (w*h bytes, caller-owned). This call
 * does NOT clear out_mask first and never overwrites a pixel that is already
 * nonzero — 0 must mean "unassigned" going in, so multiple materials can be
 * seeded into one mask by calling this once per material with a distinct
 * nonzero id, and a later call can't eat an earlier one's territory. Pass
 * `material_id` == 0 to erase instead: a "0" fill clears connected pixels
 * back to unassigned regardless of the already-assigned rule above, which is
 * how a brush tool built on this would support "start over" on one region.
 *
 * Returns pixels newly assigned (or cleared, for material_id 0). */
int FloodFillMaterial(const unsigned char *rgb, const unsigned char *alpha,
                      int w, int h, int seed_x, int seed_y,
                      const MaterialSeedParams &p,
                      unsigned char material_id, unsigned char *out_mask);

/* One material's remembered look: a representative RGB color (typically the
   pixel a person clicked to seed it, or MaterialMeanColor's result from an
   already-segmented frame) tagged with the id it stands for. */
struct MaterialRef {
    unsigned char id;
    unsigned char r, g, b;
};

/* Classify every opaque pixel of `rgb`/`alpha` against up to `ref_count`
 * remembered material colors, by the same chrominance-direction rule
 * FloodFillMaterial tests pixel-to-seed with — see the file header for why
 * this, not a spatial copy, is the right way to reseed a new frame.
 *
 * NOT connectivity-constrained: every opaque pixel is tested independently
 * against every reference, and lands in whichever reference is the smallest
 * angle away (within `p`'s tolerance); a pixel matching none is left at 0.
 * `out_mask` IS fully overwritten (unlike FloodFillMaterial, there is no
 * accumulation contract here — a classification is a fresh proposal for the
 * whole frame, meant to be brushed over where it guesses wrong, not merged
 * with a prior pass).
 *
 * Returns pixels classified (nonzero in out_mask). */
int ClassifyMaterialsByHue(const unsigned char *rgb, const unsigned char *alpha,
                           int w, int h,
                           const MaterialRef *refs, int ref_count,
                           const MaterialSeedParams &p,
                           unsigned char *out_mask);

/* Pixel-weighted mean RGB of every pixel in `mask` (w*h) tagged
 * `material_id` — the usual way to turn one frame's FloodFillMaterial result
 * into the MaterialRef ClassifyMaterialsByHue reseeds the next frame with.
 * Returns false (leaving the outputs unset) when no pixel carries that id. */
bool MaterialMeanColor(const unsigned char *rgb, const unsigned char *mask,
                       int w, int h, unsigned char material_id,
                       unsigned char *out_r, unsigned char *out_g, unsigned char *out_b);

#endif /* PLATFORM_MATERIAL_MASK_H */
