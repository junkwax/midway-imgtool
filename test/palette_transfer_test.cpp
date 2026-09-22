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

int main(void)
{
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
