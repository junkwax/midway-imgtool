/*************************************************************
 * test/gif_trim_test.cpp
 *
 * Unit coverage for platform/gif_trim.cpp — the border-trim analysis behind
 * the GIF importer's "Trim Border" option. The regression that matters here is
 * the opaque-GIF case: the old alpha-only test made the option a silent no-op
 * on any file that declares no transparent index, which is most of them.
 *************************************************************/
#include "gif_trim.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

struct Frames {
    std::vector<unsigned char> px;
    int w, h, n;

    Frames(int w_, int h_, int n_) : px((size_t)w_ * h_ * n_ * 4, 0), w(w_), h(h_), n(n_) {}

    void fill(int f, unsigned char r, unsigned char g, unsigned char b, unsigned char a)
    {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) set(f, x, y, r, g, b, a);
    }
    void set(int f, int x, int y, unsigned char r, unsigned char g,
             unsigned char b, unsigned char a)
    {
        unsigned char *p = px.data() + (((size_t)f * h + y) * w + x) * 4;
        p[0] = r; p[1] = g; p[2] = b; p[3] = a;
    }
    void rect(int f, int x0, int y0, int x1, int y1,
              unsigned char r, unsigned char g, unsigned char b, unsigned char a)
    {
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) set(f, x, y, r, g, b, a);
    }
    const unsigned char *data() const { return px.data(); }
};

int main(void)
{
    /* ---- The regression: a fully opaque GIF still trims ----
       10x10, solid navy background, a red 4x3 blob at (3,2)-(6,4). Under the
       old alpha-only rule this produced the full 10x10 frame and the option
       appeared broken. */
    {
        Frames f(10, 10, 1);
        f.fill(0, 0, 20, 44, 255);
        f.rect(0, 3, 2, 6, 4, 200, 30, 30, 255);

        unsigned char bg[3] = {0, 0, 0};
        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        GifTrimBasis basis = GifFramesContentBBox(f.data(), 10, 10, 1, 0, bg,
                                                  &x0, &y0, &x1, &y1);
        CHECK(basis == GifTrim_Background);
        CHECK(x0 == 3 && y0 == 2 && x1 == 6 && y1 == 4);
        /* Exact, not 15-bit quantized: a quantized 0,16,41 would miss every
           actual background pixel at tolerance 0. */
        CHECK(bg[0] == 0 && bg[1] == 20 && bg[2] == 44);
    }

    /* ---- Alpha wins when the file actually declares transparency ---- */
    {
        Frames f(8, 8, 1);
        f.fill(0, 0, 0, 0, 0);                    /* transparent */
        f.rect(0, 2, 1, 5, 6, 90, 90, 90, 255);   /* opaque art */

        unsigned char bg[3] = {9, 9, 9};
        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        GifTrimBasis basis = GifFramesContentBBox(f.data(), 8, 8, 1, 0, bg,
                                                  &x0, &y0, &x1, &y1);
        CHECK(basis == GifTrim_Alpha);
        CHECK(x0 == 2 && y0 == 1 && x1 == 5 && y1 == 6);
    }

    /* ---- Tolerance: a dithered background needs slack ----
       Every other background pixel is nudged by 6/255, which is what GIF
       quantization does to a "flat" color. At tolerance 0 the dither itself
       reads as content; at 8 it does not. */
    {
        Frames f(12, 12, 1);
        for (int y = 0; y < 12; y++)
            for (int x = 0; x < 12; x++) {
                unsigned char v = ((x + y) & 1) ? 6 : 0;
                f.set(0, x, y, v, (unsigned char)(20 + v), (unsigned char)(44 - v), 255);
            }
        f.rect(0, 5, 5, 7, 8, 220, 10, 10, 255);

        unsigned char bg[3] = {0, 0, 0};
        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;

        GifTrimBasis strict = GifFramesContentBBox(f.data(), 12, 12, 1, 0, bg,
                                                   &x0, &y0, &x1, &y1);
        CHECK(strict == GifTrim_Background);
        CHECK(x0 == 0 && y0 == 0 && x1 == 11 && y1 == 11); /* dither defeats it */

        GifTrimBasis loose = GifFramesContentBBox(f.data(), 12, 12, 1, 8, bg,
                                                  &x0, &y0, &x1, &y1);
        CHECK(loose == GifTrim_Background);
        CHECK(x0 == 5 && y0 == 5 && x1 == 7 && y1 == 8);
    }

    /* ---- The box is the union across frames, never per-frame ----
       Frame 0's blob is top-left, frame 1's is bottom-right. Cropping to
       either alone would slide the frames relative to each other. */
    {
        Frames f(10, 10, 2);
        f.fill(0, 0, 0, 0, 0);
        f.fill(1, 0, 0, 0, 0);
        f.rect(0, 1, 1, 2, 2, 255, 255, 255, 255);
        f.rect(1, 6, 7, 8, 8, 255, 255, 255, 255);

        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        GifTrimBasis basis = GifFramesContentBBox(f.data(), 10, 10, 2, 0, NULL,
                                                  &x0, &y0, &x1, &y1);
        CHECK(basis == GifTrim_Alpha);
        CHECK(x0 == 1 && y0 == 1 && x1 == 8 && y1 == 8);
    }

    /* ---- A frame that is nothing but background reports None ---- */
    {
        Frames f(6, 6, 1);
        f.fill(0, 10, 10, 10, 255);
        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        GifTrimBasis basis = GifFramesContentBBox(f.data(), 6, 6, 1, 0, NULL,
                                                  &x0, &y0, &x1, &y1);
        CHECK(basis == GifTrim_None);
        CHECK(x0 == -1); /* outputs untouched */
    }

    /* ---- Fully transparent frames report None too ---- */
    {
        Frames f(6, 6, 1);
        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        CHECK(GifFramesContentBBox(f.data(), 6, 6, 1, 0, NULL,
                                   &x0, &y0, &x1, &y1) == GifTrim_None);
    }

    /* ---- The background comes from the border, not the bulk of the image ----
       A sprite whose body is the majority color must not nominate itself as
       background: sample the ring, and the thin border still wins. */
    {
        Frames f(10, 10, 1);
        f.fill(0, 200, 30, 30, 255);              /* body color everywhere */
        for (int i = 0; i < 10; i++) {            /* one-pixel navy ring */
            f.set(0, i, 0, 0, 20, 44, 255);
            f.set(0, i, 9, 0, 20, 44, 255);
            f.set(0, 0, i, 0, 20, 44, 255);
            f.set(0, 9, i, 0, 20, 44, 255);
        }

        unsigned char bg[3] = {0, 0, 0};
        CHECK(GifBorderBackgroundColor(f.data(), 10, 10, 1, bg));
        CHECK(bg[0] == 0 && bg[1] == 20 && bg[2] == 44);

        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        GifTrimBasis basis = GifFramesContentBBox(f.data(), 10, 10, 1, 0, bg,
                                                  &x0, &y0, &x1, &y1);
        CHECK(basis == GifTrim_Background);
        CHECK(x0 == 1 && y0 == 1 && x1 == 8 && y1 == 8);
    }

    /* ---- Degenerate inputs don't crash or claim a box ---- */
    {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        CHECK(GifFramesContentBBox(NULL, 4, 4, 1, 0, NULL, &x0, &y0, &x1, &y1) == GifTrim_None);
        Frames f(4, 4, 1);
        CHECK(GifFramesContentBBox(f.data(), 0, 4, 1, 0, NULL, &x0, &y0, &x1, &y1) == GifTrim_None);
        CHECK(GifFramesContentBBox(f.data(), 4, 4, 0, 0, NULL, &x0, &y0, &x1, &y1) == GifTrim_None);
        unsigned char bg[3];
        CHECK(!GifBorderBackgroundColor(NULL, 4, 4, 1, bg));
    }

    if (g_fails == 0) {
        std::printf("gif_trim_test: all checks passed\n");
        return 0;
    }
    std::printf("gif_trim_test: %d failure(s)\n", g_fails);
    return 1;
}
