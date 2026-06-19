/*************************************************************
 * test/image_ops_test.cpp
 *
 * Unit coverage for the pure indexed-image edge/stroke helpers extracted from
 * imgui_overlay.cpp into platform/image_ops.cpp. No UI/SDL dependencies.
 *************************************************************/
#include "image_ops.h"

#include <cstdio>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned short mkword(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

static void put_word(unsigned char *buf, int i, unsigned short w)
{
    buf[i * 2 + 0] = (unsigned char)(w & 0xFF);
    buf[i * 2 + 1] = (unsigned char)(w >> 8);
}

int main(void)
{
    /* StrokeWordLuma8: black -> 0, white -> 255, and green outweighs blue. */
    {
        CHECK(StrokeWordLuma8(mkword(0, 0, 0)) == 0);
        CHECK(StrokeWordLuma8(mkword(31, 31, 31)) == 255);
        CHECK(StrokeWordLuma8(mkword(0, 31, 0)) > StrokeWordLuma8(mkword(0, 0, 31)));
    }

    /* A 4x4 indexed buffer (stride 4): transparent border, a 2-px edge row of
       index 2 over a 2-px inner row of index 3. */
    const int w = 4, h = 4, stride = 4;
    unsigned char img[stride * h] = {
        0, 0, 0, 0,
        0, 2, 2, 0,
        0, 3, 3, 0,
        0, 0, 0, 0,
    };

    /* EdgeBufferTransparent: bounds + index 0. */
    {
        CHECK(EdgeBufferTransparent(img, w, h, stride, -1, 0));   /* OOB */
        CHECK(EdgeBufferTransparent(img, w, h, stride, 0, 0));    /* index 0 */
        CHECK(!EdgeBufferTransparent(img, w, h, stride, 1, 1));   /* index 2 */
    }

    /* EdgeBufferTransparentNeighbors: edge pixel (1,1) sees 5 transparent
       neighbors; the top-left corner (0,0), mostly out of bounds, sees 7
       (only (1,1) is non-transparent). */
    {
        CHECK(EdgeBufferTransparentNeighbors(img, w, h, stride, 1, 1) == 5);
        CHECK(EdgeBufferTransparentNeighbors(img, w, h, stride, 0, 0) == 7);
    }

    /* Palette: 0 black, 2 near-black stroke, 3 bright fill. */
    unsigned char paldata[5 * 2];
    put_word(paldata, 0, mkword(0, 0, 0));
    put_word(paldata, 1, mkword(0, 0, 0));
    put_word(paldata, 2, mkword(2, 2, 2));
    put_word(paldata, 3, mkword(31, 31, 31));
    put_word(paldata, 4, mkword(3, 2, 2));
    PAL pal {};
    pal.data_p = paldata;
    pal.numc = 5;

    /* EdgeColorStrongVariant: bright fill strongly differs from dark stroke;
       a near-identical color does not; index 0 / equal indices never qualify. */
    {
        CHECK(EdgeColorStrongVariant(&pal, 2, 3) == true);
        CHECK(EdgeColorStrongVariant(&pal, 2, 2) == false);   /* equal */
        CHECK(EdgeColorStrongVariant(&pal, 2, 0) == false);   /* transparent */
        /* Add a near-clone of the stroke at slot 1 and confirm it's weak. */
        put_word(paldata, 1, mkword(2, 2, 3));
        CHECK(EdgeColorStrongVariant(&pal, 2, 1) == false);
        put_word(paldata, 1, mkword(0, 0, 0));
    }

    /* FindInwardEdgeReplacement: from the edge pixel (1,1) it should step
       inward (down) and pick the bright fill index 3. */
    {
        unsigned char out = 0xFF;
        bool ok = FindInwardEdgeReplacement(img, w, h, stride, 1, 1, &pal,
                                            /*edge_ci=*/2, /*max_depth=*/2, &out);
        CHECK(ok == true);
        CHECK(out == 3);

        /* edge_ci 0 is rejected. */
        out = 0xFF;
        CHECK(FindInwardEdgeReplacement(img, w, h, stride, 1, 1, &pal,
                                        0, 2, &out) == false);
        CHECK(out == 0);
    }

    /* CleanupSpriteArtifacts: isolated outside dust is removed to transparent. */
    {
        unsigned char dust[5 * 5] = {};
        dust[2 * 5 + 2] = 2;
        SpriteCleanupOptions opt;
        opt.search_radius = 2;
        int n = CleanupSpriteArtifacts(dust, 5, 5, 5, &pal, &opt, false);
        CHECK(n == 1);
        CHECK(dust[2 * 5 + 2] == 2);  /* preview mode leaves source alone */
        n = CleanupSpriteArtifacts(dust, 5, 5, 5, &pal, &opt, true);
        CHECK(n == 1);
        CHECK(dust[2 * 5 + 2] == 0);
    }

    /* CleanupSpriteArtifacts: an interior wrong-color speck is repainted from
       the locally-supported neighboring color. */
    {
        unsigned char speck[5 * 5];
        for (int i = 0; i < 25; i++) speck[i] = 3;
        speck[2 * 5 + 2] = 2;
        SpriteCleanupOptions opt;
        opt.search_radius = 2;
        int n = CleanupSpriteArtifacts(speck, 5, 5, 5, &pal, &opt, true);
        CHECK(n == 1);
        CHECK(speck[2 * 5 + 2] == 3);
    }

    /* A tiny two-pixel same-color cluster has similar-color support, so it is
       not treated as random dust. */
    {
        unsigned char cluster[5 * 5];
        for (int i = 0; i < 25; i++) cluster[i] = 3;
        cluster[2 * 5 + 2] = 2;
        cluster[2 * 5 + 3] = 2;
        SpriteCleanupOptions opt;
        opt.search_radius = 2;
        int n = CleanupSpriteArtifacts(cluster, 5, 5, 5, &pal, &opt, true);
        CHECK(n == 0);
        CHECK(cluster[2 * 5 + 2] == 2);
        CHECK(cluster[2 * 5 + 3] == 2);
    }

    if (g_fails == 0) {
        std::printf("PASS: image_ops edge/stroke/cleanup helpers behave as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d image_ops check(s)\n", g_fails);
    return 1;
}
