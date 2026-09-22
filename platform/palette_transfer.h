/*************************************************************
 * platform/palette_transfer.h
 *
 * Move an already-indexed sprite onto a DIFFERENT palette whose ramps are
 * laid out differently — the MK3-art-on-an-MK2-palette case, where both games
 * carry the same character but one keeps the arms at slots 10-20 and the
 * other at 40-52, with a different number of shades.
 *
 * Inherit Colors from Marked (ui_palette.cpp) does this by nearest RGB over
 * the whole target palette. That breaks the moment the two games shade a
 * material differently: a bright cloth highlight lands in the skin ramp
 * because skin happens to hold the closest color. ramp_remap.h solved the
 * same bleed for digitized footage by only ever searching the pixel's OWN
 * material's ramp — but it needs a hand-painted material mask to know which
 * material a pixel belongs to.
 *
 * An indexed sprite needs no mask: a pixel's palette index already says
 * which ramp it belongs to. So the whole transfer collapses to a 256-entry
 * slot map, built once per palette pair and applied to every frame:
 *
 *   1. Both palettes are split into ramp blocks (the caller runs
 *      material_mask.h's DetectPaletteRampBlocks; this file takes the
 *      result, so it stays single-source for its unit test).
 *   2. Each source block is paired with a target block —
 *      SuggestTransferPairs proposes one by hue, the dialog lets a person fix
 *      it.
 *   3. Every source slot maps to a slot INSIDE its paired target block,
 *      by one of two rules:
 *        Luma     — nearest absolute luminance. Right when both games light
 *                   the material alike.
 *        Relative — the slot's position within its own block's luminance
 *                   range, matched to the same position in the target
 *                   block's range. Right when one game's version is lit
 *                   brighter or darker: the shading keeps its shape and
 *                   stretches to fill the target ramp.
 *   Slots in no block, or in an unpaired block, fall back to nearest RGB over
 *   the whole target palette — Inherit's rule, no worse than before.
 *
 * Slot 0 always maps to 0, and a nonzero slot never maps to 0 while the
 * target has any opaque color, so a transfer can neither punch holes in a
 * sprite nor grow its silhouette.
 *
 * Pure buffers and plain ints — no g_doc, no UI globals, no undo. Words are
 * the IMG format's packed 15-bit colors (xRRRRRGGGGGBBBBB). Unit-tested in
 * test/palette_transfer_test.cpp.
 *************************************************************/
#ifndef PLATFORM_PALETTE_TRANSFER_H
#define PLATFORM_PALETTE_TRANSFER_H

enum {
    kPaletteTransferLuma = 0,
    kPaletteTransferRelative = 1
};

/* One contiguous run of palette slots [start, start+count). Same shape as
   material_mask.h's PaletteRampBlock, kept separate so this module does not
   link material_mask. */
struct TransferBlock {
    int start;
    int count;
};

/* Propose a target block for each source block: the one whose mean color
 * points the same hue direction, with mean luminance as the tie-breaker. A
 * near-gray block only pairs with a near-gray block unless the target has
 * none. Several source blocks may share a target block.
 *
 * Writes `src_count` entries to `out_dst_for_src` (a target block index, or
 * -1 when the target has no blocks). */
void SuggestTransferPairs(const unsigned short *src_words, int src_numc,
                          const TransferBlock *src_blocks, int src_count,
                          const unsigned short *dst_words, int dst_numc,
                          const TransferBlock *dst_blocks, int dst_count,
                          int *out_dst_for_src);

/* Fill `out_map[256]` with the target slot for every source slot (see the
 * file header for the rules). `dst_for_src[i]` pairs src_blocks[i] with
 * dst_blocks[dst_for_src[i]]; -1 or out of range leaves that block on the
 * nearest-RGB fallback. Slots at or past src_numc map to 0.
 *
 * Returns how many opaque source slots were mapped through a paired block
 * (the rest took the fallback). */
int BuildPaletteTransferMap(const unsigned short *src_words, int src_numc,
                            const TransferBlock *src_blocks, int src_count,
                            const unsigned short *dst_words, int dst_numc,
                            const TransferBlock *dst_blocks, int dst_count,
                            const int *dst_for_src, int mode,
                            unsigned char out_map[256]);

/* Rewrite `w` x `h` pixels (row pitch `stride`) through `map`. Returns
   pixels whose index changed. */
int ApplyPaletteTransferMap(unsigned char *pixels, int w, int h, int stride,
                            const unsigned char map[256]);

#endif /* PLATFORM_PALETTE_TRANSFER_H */
