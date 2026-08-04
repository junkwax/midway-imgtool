/*************************************************************
 * test/canvas_ops_test.cpp
 *
 * Unit coverage for platform/canvas_ops.cpp: canvas re-framing geometry, the
 * content nudge clamp, selection clearing, and the cross-palette paste color
 * import planner. No UI/SDL dependencies.
 *************************************************************/
#include "canvas_ops.h"

#include <cstdio>
#include <cstring>

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
    /* ---- CanvasAnchorOffset ---- */
    {
        int dx = -1, dy = -1;

        CanvasAnchorOffset(4, 4, 8, 8, CanvasAnchor_TopLeft, &dx, &dy);
        CHECK(dx == 0 && dy == 0);

        CanvasAnchorOffset(4, 4, 8, 8, CanvasAnchor_BottomRight, &dx, &dy);
        CHECK(dx == 4 && dy == 4);

        CanvasAnchorOffset(4, 4, 8, 8, CanvasAnchor_Center, &dx, &dy);
        CHECK(dx == 2 && dy == 2);

        /* Odd growth: the extra pixel goes right/bottom, so the art sits one
           pixel toward the top-left of exact center. */
        CanvasAnchorOffset(4, 4, 7, 7, CanvasAnchor_Center, &dx, &dy);
        CHECK(dx == 1 && dy == 1);

        /* Cropping back undoes the grow exactly rather than shaving a column
           off the art: 4 -> 7 parks at +1, so 7 -> 4 must come back at -1. */
        CanvasAnchorOffset(7, 7, 4, 4, CanvasAnchor_Center, &dx, &dy);
        CHECK(dx == -1 && dy == -1);

        CanvasAnchorOffset(4, 4, 8, 8, CanvasAnchor_TopCenter, &dx, &dy);
        CHECK(dx == 2 && dy == 0);

        CanvasAnchorOffset(4, 4, 8, 8, CanvasAnchor_MiddleLeft, &dx, &dy);
        CHECK(dx == 0 && dy == 2);

        /* Out-of-range anchors fall back to top-left rather than reading junk. */
        CanvasAnchorOffset(4, 4, 8, 8, 99, &dx, &dy);
        CHECK(dx == 0 && dy == 0);
    }

    /* ---- CanvasBlitIndexedOffset: grow, keeping pixels byte-identical ---- */
    {
        const unsigned char src[4 * 2] = {
            1, 2, 0, 0,
            3, 4, 0, 0,
        };
        unsigned char dst[8 * 4];
        memset(dst, 0, sizeof(dst));
        CanvasBlitIndexedOffset(src, 2, 2, 4, dst, 6, 4, 8, 2, 1);

        CHECK(dst[1 * 8 + 2] == 1);
        CHECK(dst[1 * 8 + 3] == 2);
        CHECK(dst[2 * 8 + 2] == 3);
        CHECK(dst[2 * 8 + 3] == 4);
        /* Everything outside the blit stays transparent. */
        CHECK(dst[0 * 8 + 0] == 0);
        CHECK(dst[3 * 8 + 5] == 0);
    }

    /* ---- CanvasBlitIndexedOffset: negative offset clips instead of wrapping ---- */
    {
        const unsigned char src[4 * 2] = {
            1, 2, 0, 0,
            3, 4, 0, 0,
        };
        unsigned char dst[4 * 2];
        memset(dst, 0, sizeof(dst));
        CanvasBlitIndexedOffset(src, 2, 2, 4, dst, 2, 2, 4, -1, -1);

        CHECK(dst[0 * 4 + 0] == 4);  /* only the bottom-right pixel survives */
        CHECK(dst[0 * 4 + 1] == 0);
        CHECK(dst[1 * 4 + 0] == 0);
    }

    /* ---- CanvasIndexedContentBounds ---- */
    {
        const unsigned char buf[4 * 4] = {
            0, 0, 0, 0,
            0, 0, 5, 0,
            0, 7, 0, 0,
            0, 0, 0, 0,
        };
        int x0 = -1, y0 = -1, x1 = -1, y1 = -1;
        CHECK(CanvasIndexedContentBounds(buf, 4, 4, 4, &x0, &y0, &x1, &y1));
        CHECK(x0 == 1 && y0 == 1 && x1 == 2 && y1 == 2);

        const unsigned char empty[4 * 4] = {0};
        CHECK(!CanvasIndexedContentBounds(empty, 4, 4, 4, &x0, &y0, &x1, &y1));
        /* Outputs untouched on the empty path. */
        CHECK(x0 == 1 && y0 == 1);
    }

    /* ---- CanvasClampContentNudge ---- */
    {
        /* Art at x 2..5 inside a 10-wide canvas: 2 px of room left, 4 right. */
        int dx = -5, dy = 0;
        CanvasClampContentNudge(2, 2, 5, 5, 10, 10, &dx, &dy);
        CHECK(dx == -2);

        dx = 9; dy = 0;
        CanvasClampContentNudge(2, 2, 5, 5, 10, 10, &dx, &dy);
        CHECK(dx == 4);

        /* Inside the allowance, the nudge passes through untouched. */
        dx = -1; dy = 3;
        CanvasClampContentNudge(2, 2, 5, 5, 10, 10, &dx, &dy);
        CHECK(dx == -1 && dy == 3);

        /* Art flush against both edges cannot move at all. */
        dx = -1; dy = 1;
        CanvasClampContentNudge(0, 0, 9, 9, 10, 10, &dx, &dy);
        CHECK(dx == 0 && dy == 0);
    }

    /* ---- CanvasClearIndexedRect: plain rect ---- */
    {
        unsigned char buf[4 * 4] = {
            1, 1, 1, 1,
            1, 1, 1, 1,
            1, 1, 1, 1,
            1, 1, 1, 1,
        };
        int n = CanvasClearIndexedRect(buf, 4, 4, 4, 1, 1, 2, 2, NULL, 0, 0);
        CHECK(n == 4);
        CHECK(buf[1 * 4 + 1] == 0 && buf[2 * 4 + 2] == 0);
        CHECK(buf[0 * 4 + 0] == 1 && buf[3 * 4 + 3] == 1);

        /* Already-transparent pixels are not counted, and the rect is clipped
           to the buffer rather than running off it. */
        n = CanvasClearIndexedRect(buf, 4, 4, 4, 1, 1, 99, 99, NULL, 0, 0);
        CHECK(n == 5);  /* the 3x3 corner minus the 4 already cleared */
    }

    /* ---- CanvasClearIndexedRect: mask erases its real shape ---- */
    {
        unsigned char buf[4 * 4] = {
            1, 1, 1, 1,
            1, 1, 1, 1,
            1, 1, 1, 1,
            1, 1, 1, 1,
        };
        unsigned char mask[4 * 4] = {0};
        mask[1 * 4 + 1] = 1;
        mask[2 * 4 + 2] = 1;
        int n = CanvasClearIndexedRect(buf, 4, 4, 4, 0, 0, 3, 3, mask, 4, 4);
        CHECK(n == 2);
        CHECK(buf[1 * 4 + 1] == 0 && buf[2 * 4 + 2] == 0);
        CHECK(buf[1 * 4 + 2] == 1);  /* inside the rect, outside the mask */
    }

    /* ---- CanvasCollectUsedIndices ---- */
    {
        const unsigned char buf[4 * 2] = {
            0, 9, 0, 0,   /* the trailing stride bytes must not be counted */
            4, 4, 0, 0,
        };
        bool used[256] = {false};
        CanvasCollectUsedIndices(buf, 2, 2, 4, used);
        CHECK(used[0] && used[9] && used[4]);
        CHECK(!used[1] && !used[255]);
    }

    /* ---- PlanPaletteColorImport: append into the depth's spare indices ----
       The motivating case: a 6bpp palette addressing 64 slots but holding only
       4 colors, and a paste needing two colors it doesn't have. */
    {
        unsigned char src[512] = {0};
        put_word(src, 0, 0);
        put_word(src, 1, mkword(31, 0, 0));   /* red   — already in the target */
        put_word(src, 2, mkword(0, 31, 0));   /* green — missing */
        put_word(src, 3, mkword(0, 0, 31));   /* blue  — missing, but unused */

        unsigned char dst[512] = {0};
        put_word(dst, 0, 0);
        put_word(dst, 1, mkword(31, 0, 0));
        put_word(dst, 2, mkword(10, 10, 10));
        put_word(dst, 3, mkword(20, 20, 20));

        bool used[256] = {false};
        used[0] = used[1] = used[2] = true;   /* index 3 is never drawn */

        PaletteImportPlan plan =
            PlanPaletteColorImport(src, 4, used, dst, 4, 64, NULL);

        CHECK(plan.matched == 1);             /* red */
        CHECK(plan.unmatched == 0);
        CHECK(plan.added.size() == 1);        /* green only; blue is unused */
        if (plan.added.size() == 1) {
            CHECK(plan.added[0].src_index == 2);
            CHECK(plan.added[0].dst_index == 4);   /* appended past numc */
            CHECK(plan.added[0].word == mkword(0, 31, 0));
        }
        CHECK(plan.new_numc == 5);
    }

    /* ---- PlanPaletteColorImport: full palette reuses only free holes ---- */
    {
        unsigned char src[512] = {0};
        put_word(src, 1, mkword(1, 2, 3));
        put_word(src, 2, mkword(4, 5, 6));
        put_word(src, 3, mkword(7, 8, 9));

        unsigned char dst[512] = {0};
        for (int i = 1; i < 4; i++) put_word(dst, i, mkword(30, 30, i));

        bool used[256] = {false};
        used[1] = used[2] = used[3] = true;

        /* Capacity == numc, so there is nothing to append into: only index 2
           may be overwritten. */
        bool freeslot[256] = {false};
        freeslot[2] = true;

        PaletteImportPlan plan =
            PlanPaletteColorImport(src, 4, used, dst, 4, 4, freeslot);

        CHECK(plan.added.size() == 1);
        if (plan.added.size() == 1) {
            CHECK(plan.added[0].dst_index == 2);
            CHECK(plan.added[0].src_index == 1);   /* first missing color wins */
        }
        CHECK(plan.unmatched == 2);
        CHECK(plan.new_numc == 4);                 /* reuse never grows */
    }

    /* ---- PlanPaletteColorImport: no room at all leaves everything to the
       existing nearest-color remap ---- */
    {
        unsigned char src[512] = {0};
        put_word(src, 1, mkword(1, 2, 3));

        unsigned char dst[512] = {0};
        put_word(dst, 1, mkword(30, 30, 30));

        bool used[256] = {false};
        used[1] = true;

        PaletteImportPlan plan =
            PlanPaletteColorImport(src, 2, used, dst, 2, 2, NULL);
        CHECK(plan.added.empty());
        CHECK(plan.unmatched == 1);
        CHECK(plan.new_numc == 2);
    }

    /* ---- PlanPaletteColorImport: duplicate source colors share one slot ---- */
    {
        unsigned char src[512] = {0};
        put_word(src, 1, mkword(5, 5, 5));
        put_word(src, 2, mkword(5, 5, 5));

        unsigned char dst[512] = {0};
        put_word(dst, 1, mkword(31, 31, 31));

        bool used[256] = {false};
        used[1] = used[2] = true;

        PaletteImportPlan plan =
            PlanPaletteColorImport(src, 3, used, dst, 2, 16, NULL);
        CHECK(plan.added.size() == 1);
        CHECK(plan.matched == 1);      /* the second one finds the first */
        CHECK(plan.new_numc == 3);
    }

    /* Index 0 is transparent and is never imported, even when the source
       palette's slot 0 holds a real color. */
    {
        unsigned char src[512] = {0};
        put_word(src, 0, mkword(9, 9, 9));

        unsigned char dst[512] = {0};
        put_word(dst, 1, mkword(31, 31, 31));

        bool used[256] = {false};
        used[0] = true;

        PaletteImportPlan plan =
            PlanPaletteColorImport(src, 2, used, dst, 2, 16, NULL);
        CHECK(plan.added.empty());
        CHECK(plan.matched == 0 && plan.unmatched == 0);
    }

    if (g_fails == 0) {
        std::printf("canvas_ops_test: all checks passed\n");
        return 0;
    }
    std::printf("canvas_ops_test: %d failure(s)\n", g_fails);
    return 1;
}
