/*************************************************************
 * platform/ramp_remap.h
 *
 * Last step of the import pipeline digitize_matte.h -> material_mask.h ->
 * palette_ramp.h feeds into: assemble each material's fitted ramp into one
 * palette, then remap a frame's pixels onto it.
 *
 * Midway's ramps are luminance-ordered — that is the whole premise
 * palette_ramp.h validates against the shipped MK2 data. So the remap here
 * is deliberately NOT nearest-RGB against the assembled palette. It is
 * nearest-LUMINANCE against just the one ramp the pixel's OWN material owns.
 * Nearest-RGB would let a bright highlight in the cloth jump to a similarly
 * bright slot over in the skin ramp; that is exactly the cross-material
 * bleed one-ramp-per-material exists to prevent, and it is why this file
 * needs material_mask's per-pixel mask as an input, not just the pixel's
 * own color.
 *
 * Pure buffers and plain ints throughout — no g_doc, no UI globals, no undo.
 *************************************************************/
#ifndef PLATFORM_RAMP_REMAP_H
#define PLATFORM_RAMP_REMAP_H

#include "palette_ramp.h"   /* RampColor */

/* Generous caps, well past anything the shipped MK2 art uses (Liu Kang's
   64-color LKRED_P peaks at 44 slots for skin alone; his whole roster of
   materials per palette tops out around 4). Hardware addresses at most 256
   total colors (1-byte index), so MAX_RAMP_COLORS * MAX_MATERIALS need not
   itself fit in 256 — AssemblePalette enforces the real 256 budget as it
   concatenates, not these. */
#define RAMP_REMAP_MAX_RAMP_COLORS 64
#define RAMP_REMAP_MAX_MATERIALS   32

/* One material's fitted ramp, in the luminance order palette_ramp.h's
   fitters already produce (dark to bright). `material_id` matches the ids
   material_mask.h assigned; 0 is reserved by that module for "unassigned"
   and must not be used here. */
struct MaterialRamp {
    unsigned char material_id;
    RampColor     colors[RAMP_REMAP_MAX_RAMP_COLORS];
    int           count;
};

/* The concatenated palette: index 0 transparent, then each material's ramp
   in the order it was assembled, back-to-back. */
struct AssembledPalette {
    unsigned short words[256];   /* packed 15-bit, index 0 = 0/transparent */
    int numc;                    /* includes the transparent slot */

    struct Span { unsigned char material_id; int start; int count; };
    Span spans[RAMP_REMAP_MAX_MATERIALS];
    int  span_count;
};

/* Concatenate `ramps` (in the order given) into `out`, starting at index 1
 * (index 0 is always the transparent slot, left at word 0). A ramp that
 * would push the total past the 256-color hardware limit is skipped
 * entirely — its material gets no span, so RemapFrameToRamps will leave
 * that material's pixels at index 0 rather than write out of bounds.
 * Ramps after the skipped one are still attempted, so a caller previewing a
 * budget that doesn't quite fit sees everything that DOES fit, not just a
 * truncated prefix.
 *
 * Returns the number of ramps that got a span (may be less than
 * `ramp_count`). */
int AssemblePalette(const MaterialRamp *ramps, int ramp_count, AssembledPalette *out);

/* Populate `out` directly from an EXISTING palette's own colors instead of
 * concatenating freshly fit ramps — for matching new digitized frames onto
 * a shipped character's palette (e.g. Kung Lao's TMBLUE_P) so they land on
 * its exact indices instead of getting a palette of their own.
 *
 * `target_words` is `target_numc` packed 15-bit words, copied into `out`
 * verbatim (index 0 included, whatever it holds). `blocks` names which
 * existing index range belongs to which material — typically
 * material_mask.h's DetectPaletteRampBlocks proposal, adjusted by hand — and
 * is otherwise used exactly like AssemblePalette's spans: RemapFrameToRamps
 * cannot tell the two apart, so a frame processed this way is remapped by
 * the same nearest-luminance-within-the-material's-own-range rule either way.
 *
 * A block outside [0, target_numc) is dropped entirely; one that only
 * partly overlaps is clamped to the part that exists. Returns spans
 * written (<= block_count). */
int AssembleFromExistingPalette(const unsigned short *target_words, int target_numc,
                                const AssembledPalette::Span *blocks, int block_count,
                                AssembledPalette *out);

/* Remap one frame onto `assembled`: for every opaque pixel (alpha > 0), look
 * up its material id in `mask`, find that material's span, and write the
 * index within that span whose color is nearest in LUMINANCE to the pixel's
 * own — not nearest RGB; see the file header for why. A pixel that is
 * transparent, or whose mask id has no span in `assembled` (unassigned, or
 * a material AssemblePalette had to skip), is written as index 0.
 *
 * `out_indices` is w*h bytes. Returns opaque pixels successfully mapped to a
 * non-zero index. */
int RemapFrameToRamps(const unsigned char *rgb, const unsigned char *alpha,
                      const unsigned char *mask, int w, int h,
                      const AssembledPalette &assembled,
                      unsigned char *out_indices);

#endif /* PLATFORM_RAMP_REMAP_H */
