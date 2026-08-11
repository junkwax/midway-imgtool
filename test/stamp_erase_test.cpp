/*************************************************************
 * test/stamp_erase_test.cpp
 *
 * Unit coverage for platform/stamp_erase.cpp: locating a copied object inside
 * a frame by its pixels alone, and subtracting it without disturbing whatever
 * was painted over the top. Models the UMK3FIRE case — a skeleton reused
 * verbatim under fourteen different flames. No UI/SDL dependencies.
 *************************************************************/
#include "stamp_erase.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* A small indexed bitmap with an imgtool-style 4-byte-aligned stride, so the
   tests exercise the same stride != width case the real IMG buffers have. */
struct Bitmap {
    std::vector<unsigned char> px;
    int w, h, stride;

    Bitmap(int width, int height) : w(width), h(height)
    {
        stride = (width + 3) & ~3;
        px.assign((size_t)stride * (size_t)height, 0);
    }
    unsigned char &at(int x, int y) { return px[(size_t)y * (size_t)stride + (size_t)x]; }
    unsigned char get(int x, int y) const { return px[(size_t)y * (size_t)stride + (size_t)x]; }
    StampBuf view() const
    {
        StampBuf b;
        b.pixels = px.data();
        b.w = w; b.h = h; b.stride = stride;
        return b;
    }
};

/* A 5x4 "skeleton": every index distinct, index 0 transparent so the object
   has a real silhouette rather than being a rectangle. The indices are
   deliberately scattered rather than sequential — a run of consecutive values
   would let a one-pixel horizontal shift of a recolored copy line up with the
   original by accident, which is a property of the fixture, not the matcher. */
static Bitmap MakeStamp(void)
{
    Bitmap s(5, 4);
    const unsigned char rows[4][5] = {
        {  0, 31,  7, 19,  0 },
        { 12, 44,  3, 27,  9 },
        {  0, 21, 15,  5,  0 },
        { 38,  0, 11,  0, 24 },
    };
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 5; x++)
            s.at(x, y) = rows[y][x];
    return s;
}

/* Paste the stamp into a bigger frame at (dx, dy), on a background of a color
   the stamp never uses. */
static Bitmap MakeFrameWithStamp(int w, int h, const Bitmap &stamp,
                                 int dx, int dy, unsigned char backdrop)
{
    Bitmap f(w, h);
    if (backdrop) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                f.at(x, y) = backdrop;
    }
    for (int y = 0; y < stamp.h; y++)
        for (int x = 0; x < stamp.w; x++) {
            unsigned char v = stamp.get(x, y);
            if (!v) continue;
            int tx = dx + x, ty = dy + y;
            if (tx < 0 || ty < 0 || tx >= w || ty >= h) continue;
            f.at(tx, ty) = v;
        }
    return f;
}

int main(void)
{
    Bitmap stamp = MakeStamp();
    const int stamp_opaque = 14;   /* 20 cells minus 6 transparent ones */

    CHECK(StampOpaqueCount(stamp.view()) == stamp_opaque);

    /* ---- Auto search finds the object with no alignment hint ---- */
    {
        Bitmap frame = MakeFrameWithStamp(30, 20, stamp, 17, 11, 0);
        StampMatch m = StampSearchAuto(stamp.view(), frame.view(), 0);
        CHECK(m.valid);
        CHECK(m.dx == 17 && m.dy == 11);
        CHECK(m.matched == stamp_opaque);
        CHECK(StampMatchScore(m) > 0.999f);
    }

    /* ---- The flame case: something painted over part of the object ----
       Two of the object's pixels are covered. The object is still located, the
       score reports the damage, and the erase leaves the covering pixels
       exactly where they were. */
    {
        Bitmap frame = MakeFrameWithStamp(24, 16, stamp, 5, 6, 0);
        frame.at(6, 7) = 99;      /* was index 44 */
        frame.at(8, 8) = 98;      /* was index 5 */

        StampMatch m = StampSearchAuto(stamp.view(), frame.view(), 0);
        CHECK(m.valid);
        CHECK(m.dx == 5 && m.dy == 6);
        CHECK(m.matched == stamp_opaque - 2);
        CHECK(m.covered == stamp_opaque);

        int cleared = StampEraseAt(frame.px.data(), frame.w, frame.h,
                                   frame.stride, stamp.view(), m.dx, m.dy);
        CHECK(cleared == stamp_opaque - 2);
        CHECK(frame.get(6, 7) == 99);   /* flame survives */
        CHECK(frame.get(8, 8) == 98);
        /* Everything the object contributed is gone. */
        int leftovers = 0;
        for (int y = 0; y < frame.h; y++)
            for (int x = 0; x < frame.w; x++)
                if (frame.get(x, y) != 0 && frame.get(x, y) != 99 &&
                    frame.get(x, y) != 98)
                    leftovers++;
        CHECK(leftovers == 0);
    }

    /* ---- Heavy occlusion: three quarters of the object repainted ----
       The real UMK3FIRE case. The flame does not sit politely on top of the
       skeleton in a couple of places; it recolors most of it, and only about a
       quarter of the object survives byte-identical. The placement still has
       to come out exact, because one pixel off erases nothing at all.

       Everything here is deterministic — a fixed LCG, no rand() — so a
       regression is reproducible rather than flaky. */
    {
        const int sw = 24, sh = 24;
        Bitmap big(sw, sh);
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                if ((x * 3 + y * 5) % 11 == 0) continue;   /* holes: silhouette */
                big.at(x, y) = (unsigned char)(1 + ((x * 7 + y * 13) % 60));
            }
        int big_opaque = StampOpaqueCount(big.view());
        CHECK(big_opaque > 400);

        /* Frame background is noise from a band the object never uses, so a
           false hit cannot come from the backdrop. */
        Bitmap frame(64, 48);
        unsigned int seed = 12345u;
        for (int y = 0; y < frame.h; y++)
            for (int x = 0; x < frame.w; x++) {
                seed = seed * 1103515245u + 12345u;
                frame.at(x, y) = (unsigned char)(100 + ((seed >> 16) % 60));
            }
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                unsigned char v = big.get(x, y);
                if (v) frame.at(17 + x, 11 + y) = v;
            }
        /* Paint the "flame" over ~72% of it, in that same foreign band. */
        int survivors = 0;
        for (int y = 0; y < sh; y++)
            for (int x = 0; x < sw; x++) {
                if (!big.get(x, y)) continue;
                seed = seed * 1103515245u + 12345u;
                if (((seed >> 16) % 100) < 72)
                    frame.at(17 + x, 11 + y) = (unsigned char)(100 + ((seed >> 8) % 60));
                else
                    survivors++;
            }
        CHECK(survivors > 80);   /* enough left to be findable at all */

        StampMatch m = StampSearchAuto(big.view(), frame.view(), 0);
        CHECK(m.valid);
        CHECK(m.dx == 17 && m.dy == 11);
        CHECK(m.matched == survivors);

        /* And the subtraction takes exactly the survivors, leaving the flame. */
        int cleared = StampEraseAt(frame.px.data(), frame.w, frame.h,
                                   frame.stride, big.view(), m.dx, m.dy);
        CHECK(cleared == survivors);
    }

    /* ---- Erase never touches a pixel the stamp does not cover ---- */
    {
        Bitmap frame = MakeFrameWithStamp(24, 16, stamp, 5, 6, 200);
        /* Backdrop index 200 fills the frame; the stamp's transparent corners
           must leave it alone. */
        int cleared = StampEraseAt(frame.px.data(), frame.w, frame.h,
                                   frame.stride, stamp.view(), 5, 6);
        CHECK(cleared == stamp_opaque);
        CHECK(frame.get(5, 6) == 200);        /* stamp corner is transparent */
        CHECK(frame.get(9, 6) == 200);
        CHECK(frame.get(0, 0) == 200);        /* well outside the stamp */
        CHECK(frame.get(6, 6) == 0);          /* stamp pixel 31, cleared */
    }

    /* ---- Object absent: the search reports a poor score, not a false hit ---- */
    {
        Bitmap frame(20, 12);
        for (int y = 0; y < frame.h; y++)
            for (int x = 0; x < frame.w; x++)
                frame.at(x, y) = (unsigned char)(200 + ((x + y) & 3));
        StampMatch m = StampSearchAuto(stamp.view(), frame.view(), 0);
        /* None of the stamp's indices exist here at all, so there is not even
           a candidate placement. */
        CHECK(!m.valid);
        CHECK(StampMatchScore(m) == 0.0f);
    }

    /* ---- A near-miss must not read as a match ----
       Same silhouette, every index recolored: the shapes line up but the colors
       do not, which is precisely what separates "this is the object" from
       "this is a different frame of the same character". */
    {
        Bitmap other = MakeStamp();
        for (int y = 0; y < other.h; y++)
            for (int x = 0; x < other.w; x++)
                if (other.get(x, y)) other.at(x, y) = (unsigned char)(other.get(x, y) + 1);
        Bitmap frame = MakeFrameWithStamp(24, 16, other, 4, 4, 0);
        StampMatch m = StampSearchAuto(stamp.view(), frame.view(), 0);
        if (m.valid)
            CHECK(StampMatchScore(m) < 0.5f);
    }

    /* ---- Two copies in one frame: the intact one wins ----
       A window wide enough to reach both makes the scorer choose between them
       rather than stopping at the first thing that looks close. */
    {
        Bitmap frame = MakeFrameWithStamp(30, 16, stamp, 4, 4, 0);
        for (int y = 0; y < stamp.h; y++)
            for (int x = 0; x < stamp.w; x++) {
                unsigned char v = stamp.get(x, y);
                if (v) frame.at(12 + x, 4 + y) = v;
            }
        frame.at(5, 4) = 90;    /* damage three pixels of the first copy */
        frame.at(6, 4) = 91;
        frame.at(4, 5) = 92;

        StampMatch win = StampSearchWindow(stamp.view(), frame.view(), 8, 4, 8);
        CHECK(win.valid);
        CHECK(win.dx == 12 && win.dy == 4);
        CHECK(win.matched == stamp_opaque);

        StampMatch aut = StampSearchAuto(stamp.view(), frame.view(), 0);
        CHECK(aut.valid);
        CHECK(aut.dx == 12 && aut.dy == 4);
        CHECK(aut.matched == stamp_opaque);
    }

    /* ---- Partly off the edge: found, and only the visible part is cleared ---- */
    {
        Bitmap frame = MakeFrameWithStamp(12, 10, stamp, 9, 3, 0);
        StampMatch m = StampSearchAuto(stamp.view(), frame.view(), 0);
        CHECK(m.valid);
        CHECK(m.dx == 9 && m.dy == 3);
        CHECK(m.covered < stamp_opaque);      /* two columns fell off */
        CHECK(m.matched == m.covered);
        int cleared = StampEraseAt(frame.px.data(), frame.w, frame.h,
                                   frame.stride, stamp.view(), m.dx, m.dy);
        CHECK(cleared == m.covered);
    }

    /* ---- Windowed search around a hint ---- */
    {
        Bitmap frame = MakeFrameWithStamp(30, 20, stamp, 12, 8, 0);
        StampMatch near_hit = StampSearchWindow(stamp.view(), frame.view(),
                                                14, 6, 4);
        CHECK(near_hit.valid);
        CHECK(near_hit.dx == 12 && near_hit.dy == 8);
        CHECK(near_hit.matched == stamp_opaque);

        /* Too far away to be inside the window: the best placement in range
           scores badly rather than silently jumping to the real one. */
        StampMatch far_miss = StampSearchWindow(stamp.view(), frame.view(),
                                                0, 0, 2);
        CHECK(far_miss.valid);
        CHECK(far_miss.matched < stamp_opaque);
    }

    /* ---- Exact placement scoring ---- */
    {
        Bitmap frame = MakeFrameWithStamp(30, 20, stamp, 7, 5, 0);
        StampMatch exact = StampScoreAt(stamp.view(), frame.view(), 7, 5);
        CHECK(exact.valid);
        CHECK(exact.matched == stamp_opaque && exact.total == stamp_opaque);
        StampMatch off_by_one = StampScoreAt(stamp.view(), frame.view(), 8, 5);
        CHECK(off_by_one.valid);
        CHECK(off_by_one.matched < stamp_opaque);
    }

    /* ---- Degenerate inputs are refused, not crashed on ---- */
    {
        StampBuf null_buf; null_buf.pixels = NULL; null_buf.w = 4; null_buf.h = 4; null_buf.stride = 4;
        Bitmap frame(8, 8);
        CHECK(!StampSearchAuto(null_buf, frame.view(), 0).valid);
        CHECK(!StampScoreAt(null_buf, frame.view(), 0, 0).valid);
        CHECK(StampEraseAt(frame.px.data(), frame.w, frame.h, frame.stride,
                           null_buf, 0, 0) == 0);
        CHECK(StampOpaqueCount(null_buf) == 0);

        Bitmap empty(6, 6);   /* all transparent */
        CHECK(!StampSearchAuto(empty.view(), frame.view(), 0).valid);
        CHECK(!StampSearchWindow(empty.view(), frame.view(), 0, 0, 3).valid);
    }

    if (g_fails == 0) {
        std::printf("stamp_erase_test: all checks passed\n");
        return 0;
    }
    std::fprintf(stderr, "stamp_erase_test: %d failure(s)\n", g_fails);
    return 1;
}
