/*************************************************************
 * test/palette_transfer_test.cpp
 *
 * Unit coverage for the ramp-paired slot map in
 * platform/palette_transfer.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "palette_transfer.h"

#include <cstdio>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned short W(int r5, int g5, int b5)
{
    return (unsigned short)((r5 << 10) | (g5 << 5) | b5);
}

/* Source ("MK3"): [0] transparent, [1-3] red cloth dark->light, [4-6] skin.
   Target ("MK2"): the same two materials in the opposite order and with a
   different shade count — skin first at [1-4], cloth at [5-6]. */
static const unsigned short kSrc[7] = {
    0,
    W(10, 0, 0), W(20, 2, 2), W(31, 6, 6),
    W(14, 9, 6), W(22, 15, 10), W(30, 22, 16),
};
static const unsigned short kDst[7] = {
    0,
    W(12, 8, 5), W(18, 12, 8), W(24, 17, 12), W(31, 24, 18),
    W(12, 1, 1), W(28, 4, 4),
};
static const TransferBlock kSrcBlocks[2] = { {1, 3}, {4, 3} };
static const TransferBlock kDstBlocks[2] = { {1, 4}, {5, 2} };

static void suggests_pairs_by_hue_not_by_position(void)
{
    int pairs[2] = { 9, 9 };
    SuggestTransferPairs(kSrc, 7, kSrcBlocks, 2, kDst, 7, kDstBlocks, 2, pairs);
    CHECK(pairs[0] == 1);   /* red cloth -> target cloth */
    CHECK(pairs[1] == 0);   /* skin -> target skin */
}

static void every_slot_lands_inside_its_paired_block(void)
{
    int pairs[2] = { 1, 0 };
    unsigned char map[256];
    int paired = BuildPaletteTransferMap(kSrc, 7, kSrcBlocks, 2, kDst, 7, kDstBlocks, 2,
                                         pairs, kPaletteTransferLuma, map);
    CHECK(paired == 6);
    CHECK(map[0] == 0);
    for (int i = 1; i <= 3; i++) CHECK(map[i] >= 5 && map[i] <= 6);
    for (int i = 4; i <= 6; i++) CHECK(map[i] >= 1 && map[i] <= 4);
}

static void relative_mode_stretches_shading_across_the_target_ramp(void)
{
    int pairs[2] = { 1, 0 };
    unsigned char map[256];
    BuildPaletteTransferMap(kSrc, 7, kSrcBlocks, 2, kDst, 7, kDstBlocks, 2,
                            pairs, kPaletteTransferRelative, map);
    /* Darkest and lightest of each source ramp hit the ends of the target's. */
    CHECK(map[1] == 5);
    CHECK(map[3] == 6);
    CHECK(map[4] == 1);
    CHECK(map[6] == 4);
    /* Order is preserved: shading never inverts. */
    CHECK(map[4] <= map[5] && map[5] <= map[6]);
}

static void luma_mode_can_collapse_a_ramp_lit_differently(void)
{
    /* A target cloth ramp much darker than the source: absolute luma parks
       the lighter source shades on the one brightest slot, relative spreads
       them. */
    const unsigned short dst[4] = { 0, W(4, 0, 0), W(7, 0, 0), W(10, 0, 0) };
    const TransferBlock db[1] = { {1, 3} };
    const TransferBlock sb[1] = { {1, 3} };
    int pairs[1] = { 0 };
    unsigned char luma[256], rel[256];
    BuildPaletteTransferMap(kSrc, 4, sb, 1, dst, 4, db, 1, pairs, kPaletteTransferLuma, luma);
    BuildPaletteTransferMap(kSrc, 4, sb, 1, dst, 4, db, 1, pairs, kPaletteTransferRelative, rel);
    CHECK(luma[1] == 3 && luma[2] == 3 && luma[3] == 3);
    CHECK(rel[1] == 1 && rel[2] == 2 && rel[3] == 3);
}

static void unpaired_blocks_fall_back_to_nearest_rgb_and_never_to_zero(void)
{
    int pairs[2] = { -1, 0 };
    unsigned char map[256];
    int paired = BuildPaletteTransferMap(kSrc, 7, kSrcBlocks, 2, kDst, 7, kDstBlocks, 2,
                                         pairs, kPaletteTransferLuma, map);
    CHECK(paired == 3);
    for (int i = 1; i < 7; i++) CHECK(map[i] != 0);
    CHECK(map[3] == 6);   /* bright red's nearest color is the target's bright red */
}

static void slots_past_the_source_palette_map_to_zero(void)
{
    int pairs[2] = { 1, 0 };
    unsigned char map[256];
    BuildPaletteTransferMap(kSrc, 7, kSrcBlocks, 2, kDst, 7, kDstBlocks, 2,
                            pairs, kPaletteTransferLuma, map);
    CHECK(map[7] == 0);
    CHECK(map[255] == 0);
}

static void a_block_reaching_slot_zero_is_clipped_off_it(void)
{
    const TransferBlock sb[1] = { {0, 4} };
    const TransferBlock db[1] = { {0, 7} };
    int pairs[1] = { 0 };
    unsigned char map[256];
    BuildPaletteTransferMap(kSrc, 7, sb, 1, kDst, 7, db, 1, pairs, kPaletteTransferLuma, map);
    CHECK(map[0] == 0);
    for (int i = 1; i < 7; i++) CHECK(map[i] != 0);
}

static void apply_rewrites_through_the_map_and_counts_changes(void)
{
    unsigned char map[256];
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;
    map[1] = 5; map[2] = 5;
    /* 3x2 sprite in a 4-byte stride; the pad byte must be left alone. */
    unsigned char px[8] = { 0, 1, 2, 9,   2, 3, 0, 9 };
    int changed = ApplyPaletteTransferMap(px, 3, 2, 4, map);
    CHECK(changed == 3);
    const unsigned char want[8] = { 0, 5, 5, 9,   5, 3, 0, 9 };
    CHECK(std::memcmp(px, want, 8) == 0);
}

/* The MK2-Rain case in miniature. The destination ramp is lit high: 4 near
   white shades covering most of the pixels, then 2 darker. The reference has
   one white specular that almost nothing uses, then purples. By brightness,
   the four bright destination shades would all turn white. By coverage they
   take the purples. */
static void borrow_matches_by_coverage_not_brightness(void)
{
    const unsigned short dst[7] = { 0, W(31,31,31), W(31,31,26), W(31,30,22), W(31,29,18),
                                    W(20,15,7), W(10,5,2) };
    const int dslots[6] = { 1, 2, 3, 4, 5, 6 };
    const double dwt[6] = { 100, 100, 100, 100, 50, 50 };
    const unsigned short ref[5] = { 0, W(31,31,28), W(17,8,25), W(12,3,11), W(5,0,5) };
    const int rslots[4] = { 1, 2, 3, 4 };
    const double rwt[4] = { 1, 300, 300, 100 };
    unsigned short out[7] = { 0, 0, 0, 0, 0, 0, 0 };
    BorrowRampByCoverage(dst, dslots, dwt, 6, ref, rslots, rwt, 4, out);
    CHECK(out[0] == 0);
    for (int s = 1; s <= 6; s++) {
        const int r = (out[s] >> 10) & 31, g = (out[s] >> 5) & 31, b = out[s] & 31;
        /* No near-white shade survives. The top one may lean toward the
           specular, since it sits between it and the first purple. */
        CHECK(g < (s == 1 ? 24 : 16));
        CHECK(b >= g);          /* every shade leans purple */
        (void)r;
    }
    /* Order is kept: brighter destination shade -> brighter result. */
    for (int s = 1; s < 6; s++) {
        const int l0 = ((out[s] >> 10) & 31) * 3 + ((out[s] >> 5) & 31) * 6 + (out[s] & 31);
        const int l1 = ((out[s+1] >> 10) & 31) * 3 + ((out[s+1] >> 5) & 31) * 6 + (out[s+1] & 31);
        CHECK(l0 >= l1);
    }
}

static void borrow_without_weights_spreads_evenly_and_pools_duplicates(void)
{
    const unsigned short dst[4] = { 0, W(30,30,30), W(20,20,20), W(10,10,10) };
    const int dslots[3] = { 1, 2, 3 };
    /* Slots 3 and 4 hold one color: pooled, it is a single darkest shade. */
    const unsigned short ref[5] = { 0, W(0,0,30), W(0,0,20), W(0,0,4), W(0,0,4) };
    const int rslots[4] = { 1, 2, 3, 4 };
    unsigned short out[4] = { 0, 0, 0, 0 };
    BorrowRampByCoverage(dst, dslots, nullptr, 3, ref, rslots, nullptr, 4, out);
    CHECK(out[1] == W(0,0,30));
    CHECK(out[2] == W(0,0,20));
    CHECK(out[3] == W(0,0,4));
}

static void costume_run_is_the_differing_span_minus_a_flat_tail(void)
{
    /* Slots 1-3 shared (skin), 4-10 differ, and 8-10 of those are one flat
       color (RAIN1_P's pants). Slot 1 also differs on its own: a shorter run. */
    unsigned short a[12], b[12];
    for (int i = 0; i < 12; i++) a[i] = b[i] = W(i, i, i);
    a[1] = W(31, 0, 0);
    a[4] = W(20,5,25); a[5] = W(15,3,20); a[6] = W(10,1,15); a[7] = W(6,0,9);
    a[8] = a[9] = a[10] = W(0,0,4);
    const unsigned short *sb[1] = { b }, *sa[1] = { a };
    int first = -1, last = -1;
    CHECK(FindCostumeRun(a, sb, 1, 12, &first, &last));
    CHECK(first == 4);
    CHECK(last == 7);

    /* A flat pair is short enough to be shading; it stays. */
    a[10] = W(1,0,3);
    CHECK(FindCostumeRun(a, sb, 1, 12, &first, &last));
    CHECK(first == 4 && last == 10);

    CHECK(!FindCostumeRun(a, sa, 1, 12, &first, &last));

    /* One coincidentally shared shade inside the ramp does not split it;
       two in a row do. */
    unsigned short c[12];
    std::memcpy(c, a, sizeof(c));
    c[6] = b[6];
    CHECK(FindCostumeRun(c, sb, 1, 12, &first, &last));
    CHECK(first == 4 && last == 10);
    c[7] = b[7];
    CHECK(FindCostumeRun(c, sb, 1, 12, &first, &last));
    CHECK(first == 8 && last == 10);

    /* ...unless another sibling differs there. The union of every sibling's
       differences is the ramp (SCORP_P shares 31-32 with REP_P, not SUB_P). */
    unsigned short d[12];
    std::memcpy(d, b, sizeof(d));
    d[6] = W(9, 9, 0); d[7] = W(9, 9, 1);
    const unsigned short *two[2] = { b, d };
    CHECK(FindCostumeRun(c, two, 2, 12, &first, &last));
    CHECK(first == 4 && last == 10);
}

static void siblings_share_most_slots_outside_the_costume(void)
{
    unsigned short a[8], b[8], c[8];
    for (int i = 0; i < 8; i++) a[i] = b[i] = c[i] = W(i, 0, 0);
    b[1] = b[2] = b[3] = W(0, 0, 31);             /* costume 1-3 differs */
    for (int i = 1; i < 8; i++) c[i] = W(0, i, 0);  /* unrelated */
    CHECK(IsCostumeSibling(a, 8, b, 8, 1, 3));
    CHECK(!IsCostumeSibling(a, 8, c, 8, 1, 3));
    CHECK(!IsCostumeSibling(a, 8, b, 7, 1, 3));
    CHECK(CountSharedSlots(a, b, 8) == 4);
}

int main(void)
{
    borrow_matches_by_coverage_not_brightness();
    borrow_without_weights_spreads_evenly_and_pools_duplicates();
    costume_run_is_the_differing_span_minus_a_flat_tail();
    siblings_share_most_slots_outside_the_costume();
    suggests_pairs_by_hue_not_by_position();
    every_slot_lands_inside_its_paired_block();
    relative_mode_stretches_shading_across_the_target_ramp();
    luma_mode_can_collapse_a_ramp_lit_differently();
    unpaired_blocks_fall_back_to_nearest_rgb_and_never_to_zero();
    slots_past_the_source_palette_map_to_zero();
    a_block_reaching_slot_zero_is_clipped_off_it();
    apply_rewrites_through_the_map_and_counts_changes();

    if (g_fails) {
        std::fprintf(stderr, "%d check(s) failed\n", g_fails);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
