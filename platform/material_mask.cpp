/*************************************************************
 * platform/material_mask.cpp
 * Implementation of the chrominance-direction segmentation declared in
 * material_mask.h. See that header for why direction, not raw color
 * distance, is the right test.
 *************************************************************/
#include "material_mask.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

struct Chroma { double r, g, b, mag; };

/* Deviation from gray: subtracting the simple (unweighted) mean puts this
   vector exactly in the plane orthogonal to the gray axis, so a uniform
   brightness shift leaves it untouched and a shading SCALE (the case that
   actually matters — see the file header) only rescales its magnitude, never
   its direction. */
inline Chroma chroma_of(int r, int g, int b)
{
    const double y = (r + g + b) / 3.0;
    Chroma c;
    c.r = r - y; c.g = g - y; c.b = b - y;
    c.mag = std::sqrt(c.r * c.r + c.g * c.g + c.b * c.b);
    return c;
}

/* Cosine similarity, epsilon-guarded so a fully gray (mag == 0) pixel or
   reference returns a defined 0 (orthogonal) rather than dividing by zero.
   Used both as the tolerance test's input and, for pixels the noise floor
   already accepts unconditionally, as a tie-break signal — see
   ClassifyMaterialsByHue. */
inline double cos_similarity(const Chroma &a, const Chroma &b)
{
    const double denom = a.mag * b.mag;
    if (denom < 1e-9) return 0.0;
    return (a.r * b.r + a.g * b.g + a.b * b.b) / denom;
}

inline bool hue_matches(const Chroma &candidate, const Chroma &ref,
                        double cos_threshold, int noise_floor)
{
    if (candidate.mag < noise_floor) return true;
    return cos_similarity(candidate, ref) >= cos_threshold;
}

} /* namespace */

MaterialSeedParams MaterialSeedParamsDefault(void)
{
    MaterialSeedParams p;
    p.tolerance_deg = 30.0;
    p.noise_floor   = 14;
    return p;
}

int FloodFillMaterial(const unsigned char *rgb, const unsigned char *alpha,
                      int w, int h, int seed_x, int seed_y,
                      const MaterialSeedParams &p,
                      unsigned char material_id, unsigned char *out_mask)
{
    if (!rgb || !alpha || !out_mask || w <= 0 || h <= 0) return 0;
    if (seed_x < 0 || seed_x >= w || seed_y < 0 || seed_y >= h) return 0;

    const int seed_i = seed_y * w + seed_x;
    if (alpha[seed_i] == 0) return 0;

    const bool erasing = (material_id == 0);
    const unsigned char erase_target = out_mask[seed_i];
    if (erasing && erase_target == 0) return 0;

    Chroma seed_c{};
    double cos_threshold = -2.0;   /* always passes when erasing (unused) */
    if (!erasing) {
        seed_c = chroma_of(rgb[seed_i * 3 + 0], rgb[seed_i * 3 + 1], rgb[seed_i * 3 + 2]);
        const double clamped_deg = p.tolerance_deg < 0.0 ? 0.0
                                  : (p.tolerance_deg > 180.0 ? 180.0 : p.tolerance_deg);
        cos_threshold = std::cos(clamped_deg * 3.14159265358979323846 / 180.0);
        if (out_mask[seed_i] != 0) return 0;   /* seed already claimed */
    }

    struct Pt { int x, y; };
    std::vector<Pt> stack;
    stack.reserve(4096);
    stack.push_back({ seed_x, seed_y });

    int changed = 0;
    while (!stack.empty()) {
        const Pt pt = stack.back();
        stack.pop_back();
        if (pt.x < 0 || pt.x >= w || pt.y < 0 || pt.y >= h) continue;

        const int i = pt.y * w + pt.x;
        if (erasing) {
            if (out_mask[i] != erase_target) continue;
            out_mask[i] = 0;
        } else {
            if (alpha[i] == 0 || out_mask[i] != 0) continue;
            const Chroma c = chroma_of(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);
            if (!hue_matches(c, seed_c, cos_threshold, p.noise_floor)) continue;
            out_mask[i] = material_id;
        }
        changed++;

        stack.push_back({ pt.x + 1, pt.y });
        stack.push_back({ pt.x - 1, pt.y });
        stack.push_back({ pt.x, pt.y + 1 });
        stack.push_back({ pt.x, pt.y - 1 });
    }
    return changed;
}

int ClassifyMaterialsByHue(const unsigned char *rgb, const unsigned char *alpha,
                           int w, int h,
                           const MaterialRef *refs, int ref_count,
                           const MaterialSeedParams &p,
                           unsigned char *out_mask)
{
    if (!rgb || !alpha || !out_mask || w <= 0 || h <= 0) return 0;
    const size_t n = (size_t)w * (size_t)h;
    std::memset(out_mask, 0, n);
    if (!refs || ref_count <= 0) return 0;

    const double clamped_deg = p.tolerance_deg < 0.0 ? 0.0
                              : (p.tolerance_deg > 180.0 ? 180.0 : p.tolerance_deg);
    const double cos_threshold = std::cos(clamped_deg * 3.14159265358979323846 / 180.0);

    std::vector<Chroma> ref_chroma(ref_count);
    for (int k = 0; k < ref_count; k++)
        ref_chroma[k] = chroma_of(refs[k].r, refs[k].g, refs[k].b);

    int classified = 0;
    for (size_t i = 0; i < n; i++) {
        if (alpha[i] == 0) continue;
        const Chroma c = chroma_of(rgb[i * 3 + 0], rgb[i * 3 + 1], rgb[i * 3 + 2]);

        int best = -1;
        double best_score = -2.0;
        for (int k = 0; k < ref_count; k++) {
            if (!hue_matches(c, ref_chroma[k], cos_threshold, p.noise_floor)) continue;
            const double score = cos_similarity(c, ref_chroma[k]);
            if (score > best_score) { best_score = score; best = k; }
        }
        if (best >= 0) {
            out_mask[i] = refs[best].id;
            classified++;
        }
    }
    return classified;
}

bool MaterialMeanColor(const unsigned char *rgb, const unsigned char *mask,
                       int w, int h, unsigned char material_id,
                       unsigned char *out_r, unsigned char *out_g, unsigned char *out_b)
{
    if (!rgb || !mask || w <= 0 || h <= 0) return false;
    const size_t n = (size_t)w * (size_t)h;

    double sr = 0, sg = 0, sb = 0;
    long count = 0;
    for (size_t i = 0; i < n; i++) {
        if (mask[i] != material_id) continue;
        sr += rgb[i * 3 + 0]; sg += rgb[i * 3 + 1]; sb += rgb[i * 3 + 2];
        count++;
    }
    if (count == 0) return false;

    if (out_r) *out_r = (unsigned char)std::lround(sr / count);
    if (out_g) *out_g = (unsigned char)std::lround(sg / count);
    if (out_b) *out_b = (unsigned char)std::lround(sb / count);
    return true;
}
