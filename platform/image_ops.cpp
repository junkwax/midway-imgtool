/*************************************************************
 * platform/image_ops.cpp
 * Implementation of the pure indexed-image analysis helpers declared in
 * image_ops.h. See that header for rationale.
 *************************************************************/
#include "image_ops.h"
#include "palette_math.h"   /* pal_word_or_black, PaletteColorDistance5 */

#include <cstdlib>          /* abs */
#include <cstring>          /* memcpy */
#include <vector>

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

static int cleanup_clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int cleanup_square(int v)
{
    return v * v;
}

static int cleanup_abs(int v)
{
    return v < 0 ? -v : v;
}

static int cleanup_chebyshev(int dx, int dy)
{
    int ax = cleanup_abs(dx);
    int ay = cleanup_abs(dy);
    return ax > ay ? ax : ay;
}

static bool cleanup_palette_word(PAL *pal, unsigned char ci, unsigned short *out)
{
    if (out) *out = 0;
    if (!pal || !pal->data_p || ci == 0) return false;
    if ((int)ci >= (int)pal->numc || (int)ci >= 256) return false;
    if (out) *out = pal_word_or_black(pal, ci);
    return true;
}

int CleanupSpriteArtifacts(unsigned char *pixels, int w, int h, int stride,
                           PAL *pal, const SpriteCleanupOptions *options,
                           bool apply)
{
    if (!pixels || w <= 0 || h <= 0 || stride < w) return 0;

    SpriteCleanupOptions opt;
    if (options) opt = *options;
    opt.search_radius = cleanup_clamp_int(opt.search_radius, 1, 8);
    opt.similarity_distance = cleanup_clamp_int(opt.similarity_distance, 0, 64);
    opt.min_similar_neighbors = cleanup_clamp_int(opt.min_similar_neighbors, 0, 64);
    opt.min_replacement_neighbors = cleanup_clamp_int(opt.min_replacement_neighbors, 1, 64);
    opt.outlier_distance = cleanup_clamp_int(opt.outlier_distance, 0, 64);

    const int sim_dist_sq = cleanup_square(opt.similarity_distance);
    const int outlier_dist_sq = cleanup_square(opt.outlier_distance);
    const size_t bytes = (size_t)stride * (size_t)h;

    std::vector<unsigned char> src(bytes);
    memcpy(src.data(), pixels, bytes);

    struct Write {
        int off;
        unsigned char ci;
    };
    std::vector<Write> writes;

    struct ReplacementStats {
        int count;
        int near_count;
        int closest_manhattan;
    };

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int off = y * stride + x;
            unsigned char self_ci = src[(size_t)off];
            if (self_ci == 0) continue;

            unsigned short self_word = 0;
            bool have_self_word = cleanup_palette_word(pal, self_ci, &self_word);

            ReplacementStats repl[256];
            for (int i = 0; i < 256; i++) {
                repl[i].count = 0;
                repl[i].near_count = 0;
                repl[i].closest_manhattan = 0x7FFFFFFF;
            }

            int similar_neighbors = 0;
            int immediate_opaque = 0;
            int immediate_transparent = 0;

            for (int dy = -opt.search_radius; dy <= opt.search_radius; dy++) {
                for (int dx = -opt.search_radius; dx <= opt.search_radius; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx;
                    int ny = y + dy;
                    int ring = cleanup_chebyshev(dx, dy);
                    bool immediate = (ring == 1);

                    if (nx < 0 || nx >= w || ny < 0 || ny >= h) {
                        if (immediate) immediate_transparent++;
                        continue;
                    }

                    unsigned char ni = src[(size_t)ny * stride + nx];
                    if (ni == 0) {
                        if (immediate) immediate_transparent++;
                        continue;
                    }

                    if (immediate) immediate_opaque++;

                    if (ni == self_ci) {
                        similar_neighbors++;
                    } else if (have_self_word) {
                        unsigned short nw = 0;
                        if (cleanup_palette_word(pal, ni, &nw) &&
                            PaletteColorDistance5(self_word, nw) <= sim_dist_sq) {
                            similar_neighbors++;
                        }
                    }

                    if (ni != self_ci) {
                        ReplacementStats &rs = repl[ni];
                        rs.count++;
                        if (immediate) rs.near_count++;
                        int manhattan = cleanup_abs(dx) + cleanup_abs(dy);
                        if (manhattan < rs.closest_manhattan)
                            rs.closest_manhattan = manhattan;
                    }
                }
            }

            if (similar_neighbors >= opt.min_similar_neighbors) continue;

            bool have_best = false;
            unsigned char best_ci = 0;
            int best_score = -0x7FFFFFFF;

            for (int ci = 1; ci < 256; ci++) {
                if (repl[ci].count <= 0) continue;
                if (repl[ci].near_count == 0 &&
                    repl[ci].count < opt.min_replacement_neighbors)
                    continue;

                unsigned short repl_word = 0;
                if (!cleanup_palette_word(pal, (unsigned char)ci, &repl_word))
                    continue;

                int color_dist = have_self_word ?
                    PaletteColorDistance5(self_word, repl_word) : outlier_dist_sq;
                if (have_self_word && color_dist < outlier_dist_sq) continue;

                int support = repl[ci].count + repl[ci].near_count * 2;
                if (support < opt.min_replacement_neighbors) continue;

                int score = support * 100;
                score -= repl[ci].closest_manhattan * 8;
                if (color_dist > outlier_dist_sq * 2) score += 20;
                if (!have_best || score > best_score) {
                    have_best = true;
                    best_ci = (unsigned char)ci;
                    best_score = score;
                }
            }

            if (opt.allow_transparent_replacement &&
                immediate_transparent >= 6 && immediate_opaque <= 2) {
                int transparent_score = immediate_transparent * 120 - immediate_opaque * 40;
                if (!have_best || immediate_opaque == 0 || transparent_score > best_score) {
                    have_best = true;
                    best_ci = 0;
                    best_score = transparent_score;
                }
            }

            if (have_best && best_ci != self_ci)
                writes.push_back({off, best_ci});
        }
    }

    if (apply) {
        for (const Write &wr : writes)
            pixels[wr.off] = wr.ci;
    }

    return (int)writes.size();
}
