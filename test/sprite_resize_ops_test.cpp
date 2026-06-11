/*************************************************************
 * test/sprite_resize_ops_test.cpp
 *
 * Unit coverage for the pixel resampling helpers extracted from
 * imgui_overlay.cpp into platform/sprite_resize_ops.cpp. No UI/SDL deps
 * (PoolAlloc is the inline calloc shim from img_format.h).
 *************************************************************/
#include "sprite_resize_ops.h"
#include "img_format.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

static unsigned short mkword(int r5, int g5, int b5)
{
    return (unsigned short)(((r5 & 0x1F) << 10) | ((g5 & 0x1F) << 5) | (b5 & 0x1F));
}

int main(void)
{
    /* BuildResizePalette: decodes the PAL into 8-bit RGB, fills the rest from
       the fallback (or black when no fallback is given). */
    {
        unsigned char pal_words[4];
        unsigned short w0 = mkword(0, 0, 0);    /* black  */
        unsigned short w1 = mkword(31, 0, 0);   /* full red -> (255,0,0) */
        pal_words[0] = (unsigned char)(w0 & 0xFF); pal_words[1] = (unsigned char)(w0 >> 8);
        pal_words[2] = (unsigned char)(w1 & 0xFF); pal_words[3] = (unsigned char)(w1 >> 8);
        PAL pal {};
        pal.data_p = pal_words;
        pal.numc = 2;

        ResizeRgb fallback[256];
        for (int i = 0; i < 256; i++) { fallback[i].r = 7; fallback[i].g = 8; fallback[i].b = 9; }

        ResizeRgb out[256];
        BuildResizePalette(&pal, fallback, out);
        CHECK(out[0].r == 0   && out[0].g == 0 && out[0].b == 0);
        CHECK(out[1].r == 255 && out[1].g == 0 && out[1].b == 0);
        /* Index beyond numc uses the fallback. */
        CHECK(out[2].r == 7 && out[2].g == 8 && out[2].b == 9);

        /* No fallback -> indices beyond numc are black. */
        ResizeRgb out2[256];
        BuildResizePalette(&pal, nullptr, out2);
        CHECK(out2[2].r == 0 && out2[2].g == 0 && out2[2].b == 0);
    }

    /* ResizeSpritePixelsNearest: a 2x2 source upscaled to 4x4 is a clean
       block-double. Source stride = (2+3)&~3 = 4; dest stride = 4. */
    {
        const unsigned char A = 10, B = 20, C = 30, D = 40;
        unsigned char src[8] = { A, B, 0, 0,
                                 C, D, 0, 0 };
        IMG img {};
        img.w = 2;
        img.h = 2;
        img.data_p = src;

        unsigned int stride = 0;
        unsigned char *dst = ResizeSpritePixelsNearest(&img, 4, 4, &stride);
        CHECK(dst != nullptr);
        CHECK(stride == 4);
        if (dst) {
            const unsigned char expect[16] = {
                A, A, B, B,
                A, A, B, B,
                C, C, D, D,
                C, C, D, D,
            };
            for (int y = 0; y < 4; y++)
                for (int x = 0; x < 4; x++)
                    CHECK(dst[y * stride + x] == expect[y * 4 + x]);
            std::free(dst);   /* PoolAlloc == calloc in img_format.h shim */
        }
    }

    if (g_fails == 0) {
        std::printf("PASS: sprite_resize_ops resampling behaves as specified\n");
        return 0;
    }
    std::fprintf(stderr, "FAILED: %d sprite_resize_ops check(s)\n", g_fails);
    return 1;
}
