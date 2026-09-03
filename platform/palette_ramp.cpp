/*************************************************************
 * platform/palette_ramp.cpp
 * Implementation of the material ramp fitters declared in palette_ramp.h.
 * See that header for the shipped-palette evidence behind the two fits.
 *************************************************************/
#include "palette_ramp.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace {

inline void unpack15(unsigned short w, int &r5, int &g5, int &b5)
{
    r5 = (w >> 10) & 0x1F;
    g5 = (w >>  5) & 0x1F;
    b5 =  w        & 0x1F;
}

inline int clamp5(int v)
{
    if (v < 0) return 0;
    if (v > 31) return 31;
    return v;
}

/* One sample decoded to floating RGB, alongside its weight. */
struct Pt { double r, g, b; double w; };

std::vector<Pt> decode(const RampSample *samples, int n)
{
    std::vector<Pt> pts;
    pts.reserve(n > 0 ? n : 0);
    for (int i = 0; i < n; i++) {
        if (samples[i].weight == 0) continue;
        int r, g, b;
        unpack15(samples[i].word, r, g, b);
        pts.push_back({ (double)r, (double)g, (double)b, (double)samples[i].weight });
    }
    return pts;
}

/* Weighted mean color and total weight. */
void weighted_mean(const std::vector<Pt> &pts, double mu[3], double *total_w)
{
    double sr = 0, sg = 0, sb = 0, sw = 0;
    for (const auto &p : pts) { sr += p.r * p.w; sg += p.g * p.w; sb += p.b * p.w; sw += p.w; }
    if (sw <= 0.0) { mu[0] = mu[1] = mu[2] = 0.0; if (total_w) *total_w = 0.0; return; }
    mu[0] = sr / sw; mu[1] = sg / sw; mu[2] = sb / sw;
    if (total_w) *total_w = sw;
}

/* Principal axis of the weighted population by power iteration on the
   weighted covariance matrix. Falls back to the luma direction when the
   population is a single point (zero covariance) — a degenerate axis is
   still a usable one for a flat, single-color material. Sign is fixed so
   the axis points toward increasing luminance, matching the luminance
   ordering every shipped ramp in palette_ramp.h's evidence uses. */
void principal_axis(const std::vector<Pt> &pts, const double mu[3], double v[3])
{
    double C[3][3] = {{0,0,0},{0,0,0},{0,0,0}};
    double W = 0.0;
    for (const auto &p : pts) {
        const double d[3] = { p.r - mu[0], p.g - mu[1], p.b - mu[2] };
        W += p.w;
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                C[a][b] += p.w * d[a] * d[b];
    }
    if (W > 0.0)
        for (int a = 0; a < 3; a++)
            for (int b = 0; b < 3; b++)
                C[a][b] /= W;

    v[0] = v[1] = v[2] = 1.0;
    bool degenerate = true;
    for (int it = 0; it < 64; it++) {
        double nv[3];
        for (int a = 0; a < 3; a++)
            nv[a] = C[a][0] * v[0] + C[a][1] * v[1] + C[a][2] * v[2];
        const double n = std::sqrt(nv[0]*nv[0] + nv[1]*nv[1] + nv[2]*nv[2]);
        if (n < 1e-9) { degenerate = true; break; }
        degenerate = false;
        v[0] = nv[0] / n; v[1] = nv[1] / n; v[2] = nv[2] / n;
    }
    if (degenerate) { v[0] = 0.30; v[1] = 0.59; v[2] = 0.11; }

    const double luma = 0.30 * v[0] + 0.59 * v[1] + 0.11 * v[2];
    if (luma < 0.0) { v[0] = -v[0]; v[1] = -v[1]; v[2] = -v[2]; }
}

/* Project every point onto the axis (relative to mu), sorted ascending. */
std::vector<std::pair<double, const Pt *>> project_sorted(const std::vector<Pt> &pts,
                                                           const double mu[3],
                                                           const double v[3])
{
    std::vector<std::pair<double, const Pt *>> proj;
    proj.reserve(pts.size());
    for (const auto &p : pts) {
        const double t = (p.r - mu[0]) * v[0] + (p.g - mu[1]) * v[1] + (p.b - mu[2]) * v[2];
        proj.emplace_back(t, &p);
    }
    std::sort(proj.begin(), proj.end(),
             [](const auto &a, const auto &b) { return a.first < b.first; });
    return proj;
}

/* Weighted percentile of the sorted projection: the axis position at which
   cumulative weight first reaches `q` of the total. */
double weighted_percentile(const std::vector<std::pair<double, const Pt *>> &proj,
                           double total_w, double q)
{
    if (proj.empty()) return 0.0;
    const double target = q * total_w;
    double acc = 0.0;
    for (const auto &e : proj) {
        acc += e.second->w;
        if (acc >= target) return e.first;
    }
    return proj.back().first;
}

RampColor to_ramp_color(const double mu[3], const double v[3], double t)
{
    RampColor c;
    c.r = (unsigned char)clamp5((int)std::lround(mu[0] + v[0] * t));
    c.g = (unsigned char)clamp5((int)std::lround(mu[1] + v[1] * t));
    c.b = (unsigned char)clamp5((int)std::lround(mu[2] + v[2] * t));
    return c;
}

int dedup_consecutive(const RampColor *in, int n, RampColor *out)
{
    int k = 0;
    for (int i = 0; i < n; i++) {
        if (k > 0 && out[k - 1].r == in[i].r && out[k - 1].g == in[i].g && out[k - 1].b == in[i].b)
            continue;
        out[k++] = in[i];
    }
    return k;
}

} /* namespace */

unsigned short RampColorWord(RampColor c)
{
    return (unsigned short)(((c.r & 0x1F) << 10) | ((c.g & 0x1F) << 5) | (c.b & 0x1F));
}

int FitRampLinear(const RampSample *samples, int n, int count,
                  double lo_pct, double hi_pct, RampColor *out)
{
    if (!samples || n <= 0 || count <= 0 || !out) return 0;

    std::vector<Pt> pts = decode(samples, n);
    if (pts.empty()) return 0;

    double mu[3], total_w;
    weighted_mean(pts, mu, &total_w);
    double v[3];
    principal_axis(pts, mu, v);

    auto proj = project_sorted(pts, mu, v);
    const double tlo = weighted_percentile(proj, total_w, std::min(lo_pct, hi_pct));
    const double thi = weighted_percentile(proj, total_w, std::max(lo_pct, hi_pct));

    std::vector<RampColor> raw(count);
    for (int k = 0; k < count; k++) {
        const double t = (count == 1) ? tlo : tlo + (thi - tlo) * k / (double)(count - 1);
        raw[k] = to_ramp_color(mu, v, t);
    }
    return dedup_consecutive(raw.data(), count, out);
}

int FitRampLloyd(const RampSample *samples, int n, int count,
                 double lo_pct, double hi_pct, int max_iters,
                 RampColor *out)
{
    if (!samples || n <= 0 || count <= 0 || !out) return 0;
    if (max_iters <= 0) max_iters = 32;

    std::vector<Pt> pts = decode(samples, n);
    if (pts.empty()) return 0;

    double mu[3], total_w;
    weighted_mean(pts, mu, &total_w);
    double v[3];
    principal_axis(pts, mu, v);

    auto proj = project_sorted(pts, mu, v);
    const double tlo = weighted_percentile(proj, total_w, std::min(lo_pct, hi_pct));
    const double thi = weighted_percentile(proj, total_w, std::max(lo_pct, hi_pct));

    std::vector<double> centroid_t(count);
    for (int k = 0; k < count; k++)
        centroid_t[k] = (count == 1) ? tlo : tlo + (thi - tlo) * k / (double)(count - 1);

    std::vector<int> assign(proj.size(), -1);
    std::vector<bool> has_sample(count, false);

    for (int iter = 0; iter < max_iters; iter++) {
        bool changed = false;
        for (size_t i = 0; i < proj.size(); i++) {
            const double t = proj[i].first;
            int best = 0;
            double best_d = std::fabs(t - centroid_t[0]);
            for (int k = 1; k < count; k++) {
                const double d = std::fabs(t - centroid_t[k]);
                if (d < best_d) { best_d = d; best = k; }
            }
            if (assign[i] != best) { assign[i] = best; changed = true; }
        }

        std::vector<double> sum_t(count, 0.0), sum_r(count, 0.0), sum_g(count, 0.0), sum_b(count, 0.0), sum_w(count, 0.0);
        for (size_t i = 0; i < proj.size(); i++) {
            const int k = assign[i];
            const Pt &p = *proj[i].second;
            sum_t[k] += proj[i].first * p.w;
            sum_r[k] += p.r * p.w;
            sum_g[k] += p.g * p.w;
            sum_b[k] += p.b * p.w;
            sum_w[k] += p.w;
        }
        for (int k = 0; k < count; k++) {
            has_sample[k] = sum_w[k] > 0.0;
            if (has_sample[k]) centroid_t[k] = sum_t[k] / sum_w[k];
        }

        if (!changed) {
            std::vector<RampColor> raw;
            raw.reserve(count);
            for (int k = 0; k < count; k++) {
                if (!has_sample[k]) continue;
                RampColor c;
                c.r = (unsigned char)clamp5((int)std::lround(sum_r[k] / sum_w[k]));
                c.g = (unsigned char)clamp5((int)std::lround(sum_g[k] / sum_w[k]));
                c.b = (unsigned char)clamp5((int)std::lround(sum_b[k] / sum_w[k]));
                raw.push_back(c);
            }
            return dedup_consecutive(raw.data(), (int)raw.size(), out);
        }
    }

    /* Ran out of iterations without settling: emit the last computed means
       for centroids that ended up owning a sample. */
    std::vector<double> sum_r(count, 0.0), sum_g(count, 0.0), sum_b(count, 0.0), sum_w(count, 0.0);
    for (size_t i = 0; i < proj.size(); i++) {
        const int k = assign[i];
        const Pt &p = *proj[i].second;
        sum_r[k] += p.r * p.w; sum_g[k] += p.g * p.w; sum_b[k] += p.b * p.w; sum_w[k] += p.w;
    }
    std::vector<RampColor> raw;
    raw.reserve(count);
    for (int k = 0; k < count; k++) {
        if (sum_w[k] <= 0.0) continue;
        RampColor c;
        c.r = (unsigned char)clamp5((int)std::lround(sum_r[k] / sum_w[k]));
        c.g = (unsigned char)clamp5((int)std::lround(sum_g[k] / sum_w[k]));
        c.b = (unsigned char)clamp5((int)std::lround(sum_b[k] / sum_w[k]));
        raw.push_back(c);
    }
    return dedup_consecutive(raw.data(), (int)raw.size(), out);
}

double RampPopulationRMS(const RampSample *samples, int n,
                         const RampColor *ramp, int count)
{
    if (!samples || n <= 0 || !ramp || count <= 0) return 0.0;

    double se = 0.0, tw = 0.0;
    for (int i = 0; i < n; i++) {
        if (samples[i].weight == 0) continue;
        int r, g, b;
        unpack15(samples[i].word, r, g, b);

        double best = -1.0;
        for (int k = 0; k < count; k++) {
            const double dr = r - ramp[k].r, dg = g - ramp[k].g, db = b - ramp[k].b;
            const double d2 = dr * dr + dg * dg + db * db;
            if (best < 0.0 || d2 < best) best = d2;
        }
        se += best * samples[i].weight;
        tw += samples[i].weight;
    }
    if (tw <= 0.0) return 0.0;
    return std::sqrt(se / tw);
}
