/*************************************************************
 * platform/image_ops.cpp
 * Implementation of the pure indexed-image analysis helpers declared in
 * image_ops.h. See that header for rationale.
 *************************************************************/
#include "image_ops.h"
#include "palette_math.h"   /* pal_word_or_black, PaletteColorDistance5 */

#include <cstdlib>          /* abs */

int StrokeWordLuma8(unsigned short w)
{
    int r = (int)((w >> 10) & 0x1F) * 255 / 31;
    int g = (int)((w >>  5) & 0x1F) * 255 / 31;
    int b = (int)( w        & 0x1F) * 255 / 31;
    return (r * 54 + g * 183 + b * 19) >> 8;
}

bool EdgeBufferTransparent(const unsigned char *buf, int w, int h,
                           int stride, int x, int y)
{
    if (x < 0 || x >= w || y < 0 || y >= h) return true;
    return buf[(size_t)y * stride + x] == 0;
}

int EdgeBufferTransparentNeighbors(const unsigned char *buf, int w, int h,
                                   int stride, int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            if (EdgeBufferTransparent(buf, w, h, stride, x + dx, y + dy)) n++;
        }
    }
    return n;
}

bool EdgeColorStrongVariant(PAL *pal, unsigned char edge_ci,
                            unsigned char cand_ci)
{
    if (!pal || edge_ci == 0 || cand_ci == 0 || edge_ci == cand_ci) return false;
    unsigned short ew = pal_word_or_black(pal, edge_ci);
    unsigned short cw = pal_word_or_black(pal, cand_ci);
    int dist = PaletteColorDistance5(ew, cw);
    int dl = StrokeWordLuma8(ew) - StrokeWordLuma8(cw);
    if (dl < 0) dl = -dl;
    return dist >= 10 || dl >= 22;
}

bool FindInwardEdgeReplacement(const unsigned char *src,
                               int w, int h, int stride,
                               int x, int y, PAL *pal,
                               unsigned char edge_ci,
                               int max_depth,
                               unsigned char *out_ci)
{
    if (out_ci) *out_ci = 0;
    if (!src || !pal || !pal->data_p || edge_ci == 0) return false;
    if (max_depth < 1) max_depth = 1;
    if (max_depth > 2) max_depth = 2;

    int inward_x = 0;
    int inward_y = 0;
    int edge_trans = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            if (EdgeBufferTransparent(src, w, h, stride, x + dx, y + dy)) {
                inward_x -= dx;
                inward_y -= dy;
                edge_trans++;
            }
        }
    }
    if (edge_trans <= 0) return false;

    auto sign = [](int v) -> int { return (v > 0) ? 1 : (v < 0 ? -1 : 0); };
    int step_x = sign(inward_x);
    int step_y = sign(inward_y);
    if (step_x == 0 && step_y == 0) {
        step_x = 0;
        step_y = 1;
    }

    struct Cand {
        unsigned char ci;
        int score;
        bool strong;
    };
    Cand best = {0, 0x7FFFFFFF, false};

    auto consider = [&](int cx, int cy, int depth, int lateral) {
        if (cx < 0 || cx >= w || cy < 0 || cy >= h) return;
        unsigned char ci = src[(size_t)cy * stride + cx];
        if (ci == 0 || ci == edge_ci) return;

        int cand_trans = EdgeBufferTransparentNeighbors(src, w, h, stride, cx, cy);
        if (cand_trans > edge_trans && depth > 1) return;

        bool strong = EdgeColorStrongVariant(pal, edge_ci, ci);
        int dist = PaletteColorDistance5(pal_word_or_black(pal, edge_ci),
                                         pal_word_or_black(pal, ci));
        int score = depth * 100 + lateral * 18 - (strong ? 35 : 0);
        if (!strong) score += 40;
        if (cand_trans > 0) score += cand_trans * 6;
        score -= (dist > 40) ? 12 : dist / 4;

        if ((strong && !best.strong) ||
            (strong == best.strong && score < best.score)) {
            best.ci = ci;
            best.score = score;
            best.strong = strong;
        }
    };

    for (int depth = 1; depth <= max_depth; depth++) {
        int bx = x + step_x * depth;
        int by = y + step_y * depth;
        consider(bx, by, depth, 0);
        if (step_x == 0) {
            consider(bx - 1, by, depth, 1);
            consider(bx + 1, by, depth, 1);
        } else if (step_y == 0) {
            consider(bx, by - 1, depth, 1);
            consider(bx, by + 1, depth, 1);
        } else {
            consider(x + step_x * depth, y, depth, 1);
            consider(x, y + step_y * depth, depth, 1);
        }
        if (best.ci != 0 && best.strong && depth >= 1) break;
    }

    if (best.ci == 0) {
        for (int radius = 1; radius <= max_depth; radius++) {
            for (int dy = -radius; dy <= radius; dy++) {
                for (int dx = -radius; dx <= radius; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    if (dx * inward_x + dy * inward_y <= 0) continue;
                    consider(x + dx, y + dy, radius + 2, abs(dx) + abs(dy));
                }
            }
            if (best.ci != 0) break;
        }
    }

    if (best.ci == 0) return false;
    if (out_ci) *out_ci = best.ci;
    return true;
}
