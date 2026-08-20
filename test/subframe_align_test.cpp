/*************************************************************
 * test/subframe_align_test.cpp
 *
 * Unit coverage for platform/subframe_align.cpp: recovering where a chopped
 * subframe really sits inside its parent, and the anipoint that placement
 * implies. Models the case a parent moved without its children — the stored
 * offset still looks plausible, but the art disagrees.
 *************************************************************/
#include "subframe_align.h"

#include <cstdio>
#include <vector>

static int g_fails = 0;
#define CHECK(cond) do { if (!(cond)) { \
    std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    g_fails++; } } while (0)

/* Indexed bitmap with imgtool's 4-byte-aligned stride. */
struct Bitmap {
    std::vector<unsigned char> px;
    int w, h, stride;

    Bitmap(int width, int height) : w(width), h(height)
    {
        stride = (width + 3) & ~3;
        px.assign((size_t)stride * (size_t)height, 0);
    }
    unsigned char &at(int x, int y) { return px[(size_t)y * (size_t)stride + (size_t)x]; }
    StampBuf view() const
    {
        StampBuf b;
        b.pixels = px.data();
        b.w = w; b.h = h; b.stride = stride;
        return b;
    }
};

/* Deliberately non-linear. A pattern like (x*7 + y*13) % 200 repeats exactly
   under the shift (dx,dy) = (13,-7), which makes two placements score
   identically and tests a tie rather than the matcher. */
static unsigned char noise_at(int x, int y)
{
    unsigned int v = (unsigned int)x * 2654435761u ^ (unsigned int)y * 2246822519u;
    v ^= v >> 15;
    v *= 2246822519u;
    v ^= v >> 13;
    return (unsigned char)((v % 200u) + 1u);
}

/* A parent with a recognisable blob, and the child cut out of it at (ox,oy). */
static void build_pair(Bitmap &parent, Bitmap &child, int ox, int oy)
{
    for (int y = 0; y < parent.h; y++)
        for (int x = 0; x < parent.w; x++)
            parent.at(x, y) = noise_at(x, y);
    for (int y = 0; y < child.h; y++)
        for (int x = 0; x < child.w; x++)
            child.at(x, y) = parent.at(x + ox, y + oy);
}

int main(void)
{
    /* ---- 1. Correct art, correct anipoints: offset comes back unchanged. */
    {
        Bitmap parent(40, 30), child(11, 9);
        build_pair(parent, child, 6, 4);
        /* parent anipoint (100, 50); child chopped at (6,4) => (94, 46). */
        SubframeAlign a = subframe_align_from_parent(parent.view(), 100, 50,
                                                     child.view(), 94, 46);
        CHECK(a.valid);
        CHECK(a.exact);
        CHECK(a.off_x == 6 && a.off_y == 4);
        CHECK(a.new_anix == 94 && a.new_aniy == 46);
        CHECK(subframe_align_score(a) == 1.0f);
    }

    /* ---- 2. Parent moved without its children: the stored offset is wrong,
       the art is not. Recalculating must recover the real placement. */
    {
        Bitmap parent(40, 30), child(11, 9);
        build_pair(parent, child, 6, 4);
        /* Parent anipoint shifted by (0,-8); the child never followed, so its
           anipoint still reads as though the parent had not moved. */
        int moved_parent_ax = 100, moved_parent_ay = 42;
        SubframeAlign a = subframe_align_from_parent(parent.view(),
                                                     moved_parent_ax,
                                                     moved_parent_ay,
                                                     child.view(), 94, 46);
        CHECK(a.valid);
        CHECK(a.exact);
        CHECK(a.off_x == 6 && a.off_y == 4);
        /* The corrected child anipoint tracks the parent's new position. */
        CHECK(a.new_anix == 94 && a.new_aniy == 38);

        int cx = 0, cy = 0;
        subframe_claimed_offset(moved_parent_ax, moved_parent_ay, 94, 46, &cx, &cy);
        CHECK(cx == 6 && cy == -4);          /* what the file wrongly claimed */
        CHECK(cy != a.off_y);                /* and it disagreed with the art */
    }

    /* ---- 3. Partly repainted child still lands on the best placement. */
    {
        Bitmap parent(40, 30), child(11, 9);
        build_pair(parent, child, 5, 7);
        for (int x = 0; x < child.w; x++) child.at(x, 0) = 255; /* overpaint */
        SubframeAlign a = subframe_align_from_parent(parent.view(), 0, 0,
                                                     child.view(), 0, 0);
        CHECK(a.valid);
        CHECK(!a.exact);
        CHECK(a.off_x == 5 && a.off_y == 7);
        CHECK(subframe_align_score(a) > 0.8f);
    }

    /* ---- 4. A fully transparent child cannot be placed. */
    {
        Bitmap parent(20, 20), child(4, 4);
        for (int y = 0; y < parent.h; y++)
            for (int x = 0; x < parent.w; x++) parent.at(x, y) = 9;
        SubframeAlign a = subframe_align_from_parent(parent.view(), 0, 0,
                                                     child.view(), 0, 0);
        CHECK(!a.valid);
        CHECK(subframe_align_score(a) == 0.0f);
    }

    /* ---- 5. Child bigger than its parent: falls back, never crashes. */
    {
        Bitmap parent(8, 8), child(20, 20);
        for (int y = 0; y < parent.h; y++)
            for (int x = 0; x < parent.w; x++) parent.at(x, y) = 3;
        for (int y = 0; y < child.h; y++)
            for (int x = 0; x < child.w; x++) child.at(x, y) = 3;
        SubframeAlign a = subframe_align_from_parent(parent.view(), 10, 10,
                                                     child.view(), 0, 0);
        CHECK(a.total > 0);
    }

    /* ---- 6. Idempotent: recalculating an already-correct child is a no-op. */
    {
        Bitmap parent(32, 24), child(9, 9);
        build_pair(parent, child, 3, 11);
        SubframeAlign first = subframe_align_from_parent(parent.view(), 7, -5,
                                                         child.view(), 4, -16);
        CHECK(first.valid && first.exact);
        SubframeAlign again = subframe_align_from_parent(parent.view(), 7, -5,
                                                         child.view(),
                                                         first.new_anix,
                                                         first.new_aniy);
        CHECK(again.valid && again.exact);
        CHECK(again.new_anix == first.new_anix);
        CHECK(again.new_aniy == first.new_aniy);
    }

    if (g_fails == 0) std::printf("subframe_align_test: all checks passed\n");
    else std::fprintf(stderr, "subframe_align_test: %d failure(s)\n", g_fails);
    return g_fails ? 1 : 0;
}
