/*************************************************************
 * platform/digitize_matte.cpp
 * Implementation of the backdrop key declared in digitize_matte.h.
 * See that header for why the decontamination pass is the point of it.
 *************************************************************/
#include "digitize_matte.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace {

inline int clamp_u8(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

inline int clamp_range(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* Which channel a saturated key leads with, or -1 when the key is close
   enough to neutral that "the key's hue" is not a meaningful thing to pull
   out of anything. A white or black backdrop lands here, and despill on such
   a key would just desaturate the actor. */
int dominant_key_channel(const MatteParams &p)
{
    const int c[3] = { (int)p.key_r, (int)p.key_g, (int)p.key_b };
    int hi = 0;
    for (int i = 1; i < 3; i++) if (c[i] > c[hi]) hi = i;

    int second = -1;
    for (int i = 0; i < 3; i++) {
        if (i == hi) continue;
        if (second < 0 || c[i] > c[second]) second = i;
    }
    /* Needs a real lead over the runner-up, or it is a gray. */
    if (c[hi] - c[second] < 24) return -1;
    return hi;
}

} /* namespace */

MatteParams MatteParamsDefault(void)
{
    MatteParams p;
    p.key_r = 255; p.key_g = 255; p.key_b = 255;
    p.key_tolerance   = 12;
    p.key_softness    = 48;
    p.decontaminate   = true;
    p.despill         = false;
    p.despill_strength = 100;
    p.alpha_threshold = 128;
    p.shrink          = 0;
    return p;
}

void MatteExtract(const unsigned char *rgba, int w, int h,
                  const MatteParams &p,
                  unsigned char *out_rgb,
                  unsigned char *out_alpha,
                  MatteStats *stats)
{
    MatteStats st;
    std::memset(&st, 0, sizeof(st));
    if (stats) *stats = st;
    if (!rgba || !out_rgb || !out_alpha || w <= 0 || h <= 0) return;

    const int t0 = p.key_tolerance < 0 ? 0 : p.key_tolerance;
    const int soft = p.key_softness < 0 ? 0 : p.key_softness;
    const int t1 = t0 + soft;

    const int kr = p.key_r, kg = p.key_g, kb = p.key_b;
    const int spill_ch = (p.despill && p.despill_strength > 0)
                         ? dominant_key_channel(p) : -1;
    const int spill_keep = 100 - clamp_range(p.despill_strength, 0, 100);

    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++) {
        const unsigned char *src = rgba + i * 4;
        int r = src[0], g = src[1], b = src[2];
        const int src_a = src[3];

        /* Distance from the backdrop drives coverage. Squared distances are
           compared against squared thresholds so the common case never takes
           a square root. */
        const int dr = r - kr, dg = g - kg, db = b - kb;
        const int d2 = dr * dr + dg * dg + db * db;

        int a;
        if (d2 <= t0 * t0) {
            a = 0;
        } else if (t1 <= t0 || d2 >= t1 * t1) {
            a = 255;
        } else {
            const double d = std::sqrt((double)d2);
            a = (int)((d - t0) * 255.0 / (double)(t1 - t0) + 0.5);
            a = clamp_range(a, 0, 255);
        }
        const int key_a = a;

        /* Alpha the file already carried still applies: the key can only take
           coverage away, never hand it back. */
        if (src_a < 255) a = (a * src_a + 127) / 255;

        if (key_a == 0)        st.keyed++;
        else if (key_a == 255) st.opaque++;
        else                   st.partial++;

        if (a == 0) {
            out_alpha[i] = 0;
            out_rgb[i * 3 + 0] = 0;
            out_rgb[i * 3 + 1] = 0;
            out_rgb[i * 3 + 2] = 0;
            continue;
        }

        /* Undo the blend against the backdrop: F = (C - (1-a)*B) / a. Uses
           the key's own coverage, since that is the blend being reversed —
           alpha the file arrived with describes a composite this pass knows
           nothing about. */
        if (p.decontaminate && key_a > 0 && key_a < 255) {
            const double af = key_a / 255.0;
            const int nr = clamp_u8((int)((r - (1.0 - af) * kr) / af + 0.5));
            const int ng = clamp_u8((int)((g - (1.0 - af) * kg) / af + 0.5));
            const int nb = clamp_u8((int)((b - (1.0 - af) * kb) / af + 0.5));
            if (nr != r || ng != g || nb != b) st.decontam++;
            r = nr; g = ng; b = nb;
        }

        /* Despill: a saturated backdrop bounces into the actor, leaving the
           key's channel riding above the other two. Pull it back down to what
           the rest of the pixel supports. */
        if (spill_ch >= 0) {
            int ch[3] = { r, g, b };
            int limit = -1;
            for (int c = 0; c < 3; c++)
                if (c != spill_ch && ch[c] > limit) limit = ch[c];
            if (ch[spill_ch] > limit) {
                const int excess = ch[spill_ch] - limit;
                ch[spill_ch] = limit + (excess * spill_keep) / 100;
                st.despilled++;
                r = ch[0]; g = ch[1]; b = ch[2];
            }
        }

        out_alpha[i] = (unsigned char)a;
        out_rgb[i * 3 + 0] = (unsigned char)r;
        out_rgb[i * 3 + 1] = (unsigned char)g;
        out_rgb[i * 3 + 2] = (unsigned char)b;
    }

    if (stats) *stats = st;
}

int MatteBinarize(const unsigned char *alpha, int w, int h,
                  int threshold, int shrink, unsigned char *out_mask)
{
    if (!alpha || !out_mask || w <= 0 || h <= 0) return 0;

    const int thr = clamp_range(threshold, 0, 255);
    const size_t n = (size_t)w * (size_t)h;
    for (size_t i = 0; i < n; i++)
        out_mask[i] = (unsigned char)(alpha[i] >= thr ? 1 : 0);

    /* Erode with a 4-neighbour kernel; off the edge of the image counts as
       transparent, so the border erodes inward like everything else. */
    if (shrink > 0) {
        std::vector<unsigned char> prev(n);
        for (int pass = 0; pass < shrink; pass++) {
            std::memcpy(prev.data(), out_mask, n);
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    const size_t i = (size_t)y * w + x;
                    if (!prev[i]) continue;
                    const bool keep =
                        x > 0     && prev[i - 1] &&
                        x < w - 1 && prev[i + 1] &&
                        y > 0     && prev[i - w] &&
                        y < h - 1 && prev[i + w];
                    if (!keep) out_mask[i] = 0;
                }
            }
        }
    }

    int kept = 0;
    for (size_t i = 0; i < n; i++) if (out_mask[i]) kept++;
    return kept;
}

void MatteDownscale(const unsigned char *rgb, const unsigned char *alpha,
                    int sw, int sh,
                    unsigned char *out_rgb, unsigned char *out_alpha,
                    int dw, int dh)
{
    if (!rgb || !alpha || !out_rgb || !out_alpha) return;
    if (sw <= 0 || sh <= 0 || dw <= 0 || dh <= 0) return;

    const double xs = (double)sw / (double)dw;
    const double ys = (double)sh / (double)dh;

    for (int dy = 0; dy < dh; dy++) {
        const double y0 = dy * ys, y1 = y0 + ys;
        const int sy0 = (int)y0;
        int sy1 = (int)std::ceil(y1);
        if (sy1 > sh) sy1 = sh;

        for (int dx = 0; dx < dw; dx++) {
            const double x0 = dx * xs, x1 = x0 + xs;
            const int sx0 = (int)x0;
            int sx1 = (int)std::ceil(x1);
            if (sx1 > sw) sx1 = sw;

            /* cov_sum weights alpha by area; a_sum weights color by area*alpha
               so a transparent source pixel contributes none of its color. */
            double cov_sum = 0.0, a_sum = 0.0;
            double r_sum = 0.0, g_sum = 0.0, b_sum = 0.0;

            for (int sy = sy0; sy < sy1; sy++) {
                double wy = 1.0;
                if (sy < y0)     wy -= (y0 - sy);
                if (sy + 1 > y1) wy -= (sy + 1 - y1);
                if (wy <= 0.0) continue;

                for (int sx = sx0; sx < sx1; sx++) {
                    double wx = 1.0;
                    if (sx < x0)     wx -= (x0 - sx);
                    if (sx + 1 > x1) wx -= (sx + 1 - x1);
                    if (wx <= 0.0) continue;

                    const size_t si = (size_t)sy * sw + sx;
                    const double cov = wx * wy;
                    const double av  = alpha[si] * cov;
                    cov_sum += cov;
                    a_sum   += av;
                    r_sum   += rgb[si * 3 + 0] * av;
                    g_sum   += rgb[si * 3 + 1] * av;
                    b_sum   += rgb[si * 3 + 2] * av;
                }
            }

            const size_t di = (size_t)dy * dw + dx;
            if (cov_sum <= 0.0) {
                out_alpha[di] = 0;
                out_rgb[di * 3 + 0] = 0;
                out_rgb[di * 3 + 1] = 0;
                out_rgb[di * 3 + 2] = 0;
                continue;
            }
            out_alpha[di] = (unsigned char)clamp_u8((int)(a_sum / cov_sum + 0.5));
            if (a_sum <= 0.0) {
                out_rgb[di * 3 + 0] = 0;
                out_rgb[di * 3 + 1] = 0;
                out_rgb[di * 3 + 2] = 0;
            } else {
                out_rgb[di * 3 + 0] = (unsigned char)clamp_u8((int)(r_sum / a_sum + 0.5));
                out_rgb[di * 3 + 1] = (unsigned char)clamp_u8((int)(g_sum / a_sum + 0.5));
                out_rgb[di * 3 + 2] = (unsigned char)clamp_u8((int)(b_sum / a_sum + 0.5));
            }
        }
    }
}
