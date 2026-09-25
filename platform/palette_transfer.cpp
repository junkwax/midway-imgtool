/*************************************************************
 * platform/palette_transfer.cpp
 * Ramp-paired slot map declared in palette_transfer.h. See that header for
 * why a pixel only ever searches its paired block.
 *************************************************************/
#include "palette_transfer.h"

#include <cmath>
#include <cstring>
#include <algorithm>
#include <vector>

namespace {

struct Rgb8 { int r, g, b; };

/* 5-bit to 8-bit by bit replication (31 -> 255), the same expansion
   img_format.h's pal_word_to_rgb8 and ramp_remap.cpp use. */
inline Rgb8 rgb_of(unsigned short w)
{
    const int r5 = (w >> 10) & 0x1F, g5 = (w >> 5) & 0x1F, b5 = w & 0x1F;
    return { (r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2) };
}

inline double luma_of(const Rgb8 &c)
{
    return 0.30 * c.r + 0.59 * c.g + 0.11 * c.b;
}

/* Opponent-axis chroma: which way the color leans, independent of how
   bright it is. Magnitude below kGrayFloor carries no usable hue. */
struct Chroma { double a, b, mag; };
const double kGrayFloor = 14.0;

inline Chroma chroma_of(double r, double g, double b)
{
    Chroma c;
    c.a = r - g;
    c.b = 0.5 * (r + g) - b;
    c.mag = std::sqrt(c.a * c.a + c.b * c.b);
    return c;
}

struct BlockSummary {
    Chroma chroma;
    double mean_luma;
    bool valid;
};

int clamp_numc(int numc) { return numc < 0 ? 0 : (numc > 256 ? 256 : numc); }

/* Clamp a block to [1, numc): slot 0 is transparent and never part of a ramp. */
bool clip_block(const TransferBlock &in, int numc, int *start, int *end)
{
    int s = in.start < 1 ? 1 : in.start;
    int e = in.start + in.count;
    if (e > numc) e = numc;
    if (s >= e) return false;
    *start = s; *end = e;
    return true;
}

BlockSummary summarize(const unsigned short *words, int numc, const TransferBlock &blk)
{
    BlockSummary s{};
    int st, en;
    if (!clip_block(blk, numc, &st, &en)) return s;
    double r = 0, g = 0, b = 0, l = 0;
    for (int i = st; i < en; i++) {
        Rgb8 c = rgb_of(words[i]);
        r += c.r; g += c.g; b += c.b; l += luma_of(c);
    }
    const double n = (double)(en - st);
    s.chroma = chroma_of(r / n, g / n, b / n);
    s.mean_luma = l / n;
    s.valid = true;
    return s;
}

double pair_cost(const BlockSummary &src, const BlockSummary &dst)
{
    const double dl = std::fabs(src.mean_luma - dst.mean_luma);
    const bool sg = src.chroma.mag < kGrayFloor;
    const bool dg = dst.chroma.mag < kGrayFloor;
    if (sg && dg) return dl;
    if (sg != dg) return 3000.0 + dl;
    const double cosv = (src.chroma.a * dst.chroma.a + src.chroma.b * dst.chroma.b) /
                        (src.chroma.mag * dst.chroma.mag);
    return (1.0 - cosv) * 1000.0 + dl;
}

int nearest_rgb(const Rgb8 &c, const unsigned short *dst_words, int dst_numc)
{
    int best = 0;
    long best_d = -1;
    for (int j = 1; j < dst_numc; j++) {
        Rgb8 d = rgb_of(dst_words[j]);
        const long dr = c.r - d.r, dg = c.g - d.g, db = c.b - d.b;
        const long dist = 3 * dr * dr + 4 * dg * dg + 2 * db * db;
        if (best_d < 0 || dist < best_d) { best_d = dist; best = j; }
    }
    return best;
}

} /* namespace */

void SuggestTransferPairs(const unsigned short *src_words, int src_numc,
                          const TransferBlock *src_blocks, int src_count,
                          const unsigned short *dst_words, int dst_numc,
                          const TransferBlock *dst_blocks, int dst_count,
                          int *out_dst_for_src)
{
    if (!out_dst_for_src || src_count <= 0) return;
    src_numc = clamp_numc(src_numc);
    dst_numc = clamp_numc(dst_numc);

    for (int i = 0; i < src_count; i++) {
        out_dst_for_src[i] = -1;
        if (!src_words || !src_blocks || !dst_words || !dst_blocks) continue;
        BlockSummary s = summarize(src_words, src_numc, src_blocks[i]);
        if (!s.valid) continue;
        double best = 0.0;
        for (int j = 0; j < dst_count; j++) {
            BlockSummary d = summarize(dst_words, dst_numc, dst_blocks[j]);
            if (!d.valid) continue;
            const double cost = pair_cost(s, d);
            if (out_dst_for_src[i] < 0 || cost < best) { best = cost; out_dst_for_src[i] = j; }
        }
    }
}

int BuildPaletteTransferMap(const unsigned short *src_words, int src_numc,
                            const TransferBlock *src_blocks, int src_count,
                            const unsigned short *dst_words, int dst_numc,
                            const TransferBlock *dst_blocks, int dst_count,
                            const int *dst_for_src, int mode,
                            unsigned char out_map[256])
{
    if (!out_map) return 0;
    std::memset(out_map, 0, 256);
    src_numc = clamp_numc(src_numc);
    dst_numc = clamp_numc(dst_numc);
    if (!src_words || !dst_words || src_numc <= 1 || dst_numc <= 1) return 0;

    bool done[256] = {};
    int paired = 0;

    for (int bi = 0; src_blocks && dst_blocks && dst_for_src && bi < src_count; bi++) {
        const int di = dst_for_src[bi];
        if (di < 0 || di >= dst_count) continue;
        int ss, se, ds, de;
        if (!clip_block(src_blocks[bi], src_numc, &ss, &se)) continue;
        if (!clip_block(dst_blocks[di], dst_numc, &ds, &de)) continue;

        double s_lo = 1e9, s_hi = -1e9, d_lo = 1e9, d_hi = -1e9;
        for (int i = ss; i < se; i++) {
            const double l = luma_of(rgb_of(src_words[i]));
            if (l < s_lo) s_lo = l;
            if (l > s_hi) s_hi = l;
        }
        for (int j = ds; j < de; j++) {
            const double l = luma_of(rgb_of(dst_words[j]));
            if (l < d_lo) d_lo = l;
            if (l > d_hi) d_hi = l;
        }
        const double s_rng = s_hi - s_lo, d_rng = d_hi - d_lo;

        for (int i = ss; i < se; i++) {
            if (done[i]) continue;   /* overlapping blocks: first one wins */
            const double sl = luma_of(rgb_of(src_words[i]));
            const double st = s_rng > 0.0 ? (sl - s_lo) / s_rng : 0.5;
            int best = ds;
            double best_d = -1.0, best_tie = 0.0;
            for (int j = ds; j < de; j++) {
                const double dl = luma_of(rgb_of(dst_words[j]));
                const double abs_d = std::fabs(dl - sl);
                double d = abs_d;
                if (mode == kPaletteTransferRelative) {
                    const double dt = d_rng > 0.0 ? (dl - d_lo) / d_rng : 0.5;
                    d = std::fabs(dt - st);
                }
                if (best_d < 0.0 || d < best_d - 1e-9 ||
                    (std::fabs(d - best_d) <= 1e-9 && abs_d < best_tie)) {
                    best_d = d; best_tie = abs_d; best = j;
                }
            }
            out_map[i] = (unsigned char)best;
            done[i] = true;
            paired++;
        }
    }

    for (int i = 1; i < src_numc; i++)
        if (!done[i]) out_map[i] = (unsigned char)nearest_rgb(rgb_of(src_words[i]), dst_words, dst_numc);
    return paired;
}

int ApplyPaletteTransferMap(unsigned char *pixels, int w, int h, int stride,
                            const unsigned char map[256])
{
    if (!pixels || !map || w <= 0 || h <= 0 || stride < w) return 0;
    int changed = 0;
    for (int y = 0; y < h; y++) {
        unsigned char *row = pixels + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            const unsigned char m = map[row[x]];
            if (m != row[x]) { row[x] = m; changed++; }
        }
    }
    return changed;
}

namespace {

/* Sort `n` colors brightest-first (ties keep list order) and give each the
   middle of a span of [0,1] proportional to its weight. */
void coverage_mids(const unsigned short *words, const int *slots, const double *weight,
                   int n, std::vector<int> &order, std::vector<double> &mid)
{
    order.resize(n);
    for (int i = 0; i < n; i++) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) {
        return luma_of(rgb_of(words[slots[a]])) > luma_of(rgb_of(words[slots[b]]));
    });
    double total = 0;
    for (int i = 0; i < n; i++) total += weight ? std::max(0.0, weight[i]) : 0.0;
    /* A shade no sprite uses still gets a sliver, so it lands between its
       neighbours instead of on top of one. */
    const double eps = total > 0 ? total * 1e-6 : 1.0;
    std::vector<double> w(n);
    double sum = 0;
    for (int i = 0; i < n; i++) {
        w[i] = (weight ? std::max(0.0, weight[order[i]]) : 0.0) + eps;
        sum += w[i];
    }
    mid.resize(n);
    double acc = 0;
    for (int i = 0; i < n; i++) { mid[i] = (acc + w[i] * 0.5) / sum; acc += w[i]; }
}

} /* namespace */

void BorrowRampByCoverage(const unsigned short *dst_words, const int *dst_slots,
                          const double *dst_weight, int dst_n,
                          const unsigned short *ref_words, const int *ref_slots,
                          const double *ref_weight, int ref_n,
                          unsigned short *out_words)
{
    if (!dst_words || !dst_slots || !ref_words || !ref_slots || !out_words ||
        dst_n <= 0 || ref_n <= 0) return;

    /* Pool reference slots that hold the same color into one shade. */
    std::vector<unsigned short> rw;
    std::vector<double> rwt;
    for (int i = 0; i < ref_n; i++) {
        const unsigned short w = ref_words[ref_slots[i]];
        const double wt = ref_weight ? ref_weight[i] : 0.0;
        size_t k = 0;
        while (k < rw.size() && rw[k] != w) k++;
        if (k == rw.size()) { rw.push_back(w); rwt.push_back(wt); }
        else rwt[k] += wt;
    }
    std::vector<int> ridx(rw.size());
    for (size_t i = 0; i < ridx.size(); i++) ridx[i] = (int)i;
    std::vector<int> rorder, dorder;
    std::vector<double> rmid, dmid;
    coverage_mids(rw.data(), ridx.data(), ref_weight ? rwt.data() : nullptr,
                  (int)rw.size(), rorder, rmid);
    coverage_mids(dst_words, dst_slots, dst_weight, dst_n, dorder, dmid);

    struct Shade { double q; Rgb8 c; };
    std::vector<Shade> ref(rw.size());
    for (size_t i = 0; i < ref.size(); i++) ref[i] = { rmid[i], rgb_of(rw[rorder[i]]) };

    for (int i = 0; i < dst_n; i++) {
        const int slot = dst_slots[dorder[i]];
        if (slot < 1) continue;
        const double q = dmid[i];
        double r = ref.back().c.r, g = ref.back().c.g, b = ref.back().c.b;
        if (q <= ref.front().q) {
            r = ref.front().c.r; g = ref.front().c.g; b = ref.front().c.b;
        } else {
            for (size_t k = 1; k < ref.size(); k++) {
                if (q > ref[k].q) continue;
                const Rgb8 &a = ref[k - 1].c, &z = ref[k].c;
                const double f = (q - ref[k - 1].q) / (ref[k].q - ref[k - 1].q);
                r = a.r + (z.r - a.r) * f;
                g = a.g + (z.g - a.g) * f;
                b = a.b + (z.b - a.b) * f;
                break;
            }
        }
        const int r5 = (int)std::lround(r * 31.0 / 255.0);
        const int g5 = (int)std::lround(g * 31.0 / 255.0);
        const int b5 = (int)std::lround(b * 31.0 / 255.0);
        out_words[slot] = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
    }
}

int CountSharedSlots(const unsigned short *a, const unsigned short *b, int numc)
{
    if (!a || !b) return 0;
    numc = clamp_numc(numc);
    int n = 0;
    for (int i = 1; i < numc; i++) n += a[i] == b[i];
    return n;
}

bool IsCostumeSibling(const unsigned short *a, int a_numc,
                      const unsigned short *b, int b_numc, int first, int last)
{
    if (!a || !b || a_numc != b_numc) return false;
    const int numc = clamp_numc(a_numc);
    int outside = 0, same = 0;
    for (int i = 1; i < numc; i++) {
        if (i >= first && i <= last) continue;
        outside++;
        same += a[i] == b[i];
    }
    return outside > 0 && same * 2 >= outside;
}

bool FindCostumeRun(const unsigned short *words, const unsigned short *const *siblings, int nsib,
                    int numc, int *out_first, int *out_last)
{
    if (!words || !siblings || nsib <= 0 || !out_first || !out_last) return false;
    numc = clamp_numc(numc);
    std::vector<char> diff(numc + 2, 0);
    for (int k = 0; k < nsib; k++)
        if (siblings[k])
            for (int s = 1; s < numc; s++) diff[s] |= words[s] != siblings[k][s];
    int best_s = -1, best_e = -2;
    auto differs = [&](int s) { return s < numc && diff[s]; };
    for (int i = 1; i < numc;) {
        if (!differs(i)) { i++; continue; }
        /* One shared slot with differing ones on both sides is still inside
           the ramp. */
        int j = i;
        while (differs(j + 1) || differs(j + 2)) j += differs(j + 1) ? 1 : 2;
        if (j - i > best_e - best_s) { best_s = i; best_e = j; }
        i = j + 1;
    }
    if (best_s < 0) return false;
    /* A flat fill (3+ identical colors) at either end is not part of the ramp. */
    int run = 1;
    while (best_e - run >= best_s && words[best_e - run] == words[best_e]) run++;
    if (run >= 3 && best_e - run >= best_s) best_e -= run;
    run = 1;
    while (best_s + run <= best_e && words[best_s + run] == words[best_s]) run++;
    if (run >= 3 && best_s + run <= best_e) best_s += run;
    *out_first = best_s;
    *out_last = best_e;
    return true;
}
