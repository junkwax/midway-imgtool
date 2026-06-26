/*************************************************************
 * platform/sprite_resize_ops.cpp
 * Indexed-pixel resampling helpers for full-sprite resize.
 *************************************************************/
#include "sprite_resize_ops.h"

#include <cmath>

void BuildResizePalette(PAL *pal, const ResizeRgb fallback_rgb[256],
                        ResizeRgb out[256])
{
    for (int i = 0; i < 256; i++) {
        if (fallback_rgb) {
            out[i] = fallback_rgb[i];
        } else {
            out[i].r = 0;
            out[i].g = 0;
            out[i].b = 0;
        }
    }
    if (!pal || !pal->data_p) return;
    int n = pal->numc;
    if (n > 256) n = 256;
    const unsigned char *pd = (const unsigned char *)pal->data_p;
    for (int i = 0; i < n; i++)
        pal_word_to_rgb8(pd + i * 2, &out[i].r, &out[i].g, &out[i].b);
}

static unsigned char nearest_palette_color(const ResizeRgb pal_rgb[256],
                                           int pal_count,
                                           double r, double g, double b)
{
    if (pal_count > 256) pal_count = 256;
    if (pal_count <= 1) return 1;
    int best = 1;
    double best_d = 1.0e30;
    for (int i = 1; i < pal_count; i++) {
        double dr = r - pal_rgb[i].r;
        double dg = g - pal_rgb[i].g;
        double db = b - pal_rgb[i].b;
        double d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return (unsigned char)best;
}

static void accum_source_pixel(const unsigned char *src, int stride, int x, int y,
                               const ResizeRgb pal_rgb[256], double weight,
                               double *r, double *g, double *b, double *a)
{
    if (weight <= 0.0) return;
    unsigned char ci = src[y * stride + x];
    if (ci == 0) return;
    *a += weight;
    *r += weight * (double)pal_rgb[ci].r;
    *g += weight * (double)pal_rgb[ci].g;
    *b += weight * (double)pal_rgb[ci].b;
}

unsigned char *ResizeSpritePixelsNearest(const IMG *img, int nw, int nh,
                                         unsigned int *out_stride)
{
    int sw = img->w, sh = img->h;
    int src_stride = (sw + 3) & ~3;
    unsigned int dst_stride = ((unsigned int)nw + 3) & ~3u;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)dst_stride * nh);
    if (!dst) return NULL;
    const unsigned char *src = (const unsigned char *)img->data_p;

    for (int dy = 0; dy < nh; dy++) {
        int sy = (int)(((long long)dy * sh + sh / 2) / nh);
        if (sy >= sh) sy = sh - 1;
        const unsigned char *srow = src + sy * src_stride;
        unsigned char *drow = dst + dy * dst_stride;
        for (int dx = 0; dx < nw; dx++) {
            int sx = (int)(((long long)dx * sw + sw / 2) / nw);
            if (sx >= sw) sx = sw - 1;
            drow[dx] = srow[sx];
        }
    }
    *out_stride = dst_stride;
    return dst;
}

unsigned char *ResizeSpritePixelsQuality(const IMG *img, PAL *pal,
                                         const ResizeRgb fallback_rgb[256],
                                         int nw, int nh, bool optimize_bytes,
                                         unsigned int *out_stride)
{
    if (!pal || !pal->data_p || pal->numc <= 1)
        return ResizeSpritePixelsNearest(img, nw, nh, out_stride);

    int sw = img->w, sh = img->h;
    int src_stride = (sw + 3) & ~3;
    ResizeRgb pal_rgb[256];
    BuildResizePalette(pal, fallback_rgb, pal_rgb);

    unsigned char *dst = ResizeIndexedPixelsQuality((const unsigned char *)img->data_p,
                                                    src_stride, sw, sh,
                                                    pal_rgb, pal->numc,
                                                    nw, nh, optimize_bytes, out_stride);
    return dst ? dst : ResizeSpritePixelsNearest(img, nw, nh, out_stride);
}

unsigned char *ResizeIndexedPixelsQuality(const unsigned char *src, int src_stride,
                                          int sw, int sh,
                                          const ResizeRgb pal_rgb[256], int pal_count,
                                          int nw, int nh, bool optimize_bytes,
                                          unsigned int *out_stride)
{
    if (pal_count <= 1) return NULL;
    if (pal_count > 256) pal_count = 256;

    unsigned int dst_stride = ((unsigned int)nw + 3) & ~3u;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)dst_stride * nh);
    if (!dst) return NULL;

    int alpha_threshold = optimize_bytes ? 176 : 96;

    bool pure_upscale = (nw >= sw && nh >= sh);
    for (int dy = 0; dy < nh; dy++) {
        unsigned char *drow = dst + dy * dst_stride;
        for (int dx = 0; dx < nw; dx++) {
            double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
            double total = 0.0;

            if (pure_upscale) {
                double sx = ((double)dx + 0.5) * (double)sw / (double)nw - 0.5;
                double sy = ((double)dy + 0.5) * (double)sh / (double)nh - 0.5;
                int x0 = (int)floor(sx);
                int y0 = (int)floor(sy);
                double tx = sx - (double)x0;
                double ty = sy - (double)y0;
                for (int yy = 0; yy <= 1; yy++) {
                    int syi = y0 + yy;
                    if (syi < 0) syi = 0;
                    if (syi >= sh) syi = sh - 1;
                    double wy = yy ? ty : (1.0 - ty);
                    for (int xx = 0; xx <= 1; xx++) {
                        int sxi = x0 + xx;
                        if (sxi < 0) sxi = 0;
                        if (sxi >= sw) sxi = sw - 1;
                        double wx = xx ? tx : (1.0 - tx);
                        double w = wx * wy;
                        total += w;
                        accum_source_pixel(src, src_stride, sxi, syi, pal_rgb, w,
                                           &r, &g, &b, &a);
                    }
                }
            } else {
                double x0 = (double)dx * (double)sw / (double)nw;
                double x1 = (double)(dx + 1) * (double)sw / (double)nw;
                double y0 = (double)dy * (double)sh / (double)nh;
                double y1 = (double)(dy + 1) * (double)sh / (double)nh;
                int ix0 = (int)floor(x0);
                int ix1 = (int)ceil(x1);
                int iy0 = (int)floor(y0);
                int iy1 = (int)ceil(y1);
                for (int syi = iy0; syi < iy1; syi++) {
                    if (syi < 0 || syi >= sh) continue;
                    double oy0 = (y0 > (double)syi) ? y0 : (double)syi;
                    double oy1 = (y1 < (double)syi + 1.0) ? y1 : (double)syi + 1.0;
                    double wy = oy1 - oy0;
                    if (wy <= 0.0) continue;
                    for (int sxi = ix0; sxi < ix1; sxi++) {
                        if (sxi < 0 || sxi >= sw) continue;
                        double ox0 = (x0 > (double)sxi) ? x0 : (double)sxi;
                        double ox1 = (x1 < (double)sxi + 1.0) ? x1 : (double)sxi + 1.0;
                        double wx = ox1 - ox0;
                        if (wx <= 0.0) continue;
                        double w = wx * wy;
                        total += w;
                        accum_source_pixel(src, src_stride, sxi, syi, pal_rgb, w,
                                           &r, &g, &b, &a);
                    }
                }
            }

            if (total <= 0.0 || a <= 0.0 ||
                (a * 255.0 / total) < (double)alpha_threshold) {
                drow[dx] = 0;
            } else {
                drow[dx] = nearest_palette_color(pal_rgb, pal_count,
                                                 r / a, g / a, b / a);
            }
        }
    }

    *out_stride = dst_stride;
    return dst;
}

unsigned char SampleIndexedBilinear(const unsigned char *src, int src_stride,
                                    int sw, int sh,
                                    const ResizeRgb pal_rgb[256], int pal_count,
                                    double ux, double uy, bool optimize_bytes)
{
    if (pal_count <= 1) return 0;
    if (pal_count > 256) pal_count = 256;
    if (ux < 0.0 || uy < 0.0 || ux >= (double)sw || uy >= (double)sh) return 0;

    int alpha_threshold = optimize_bytes ? 176 : 96;
    double sx = ux - 0.5;
    double sy = uy - 0.5;
    int x0 = (int)floor(sx);
    int y0 = (int)floor(sy);
    double tx = sx - (double)x0;
    double ty = sy - (double)y0;

    double r = 0.0, g = 0.0, b = 0.0, a = 0.0;
    for (int yy = 0; yy <= 1; yy++) {
        int syi = y0 + yy;
        if (syi < 0) syi = 0;
        if (syi >= sh) syi = sh - 1;
        double wy = yy ? ty : (1.0 - ty);
        for (int xx = 0; xx <= 1; xx++) {
            int sxi = x0 + xx;
            if (sxi < 0) sxi = 0;
            if (sxi >= sw) sxi = sw - 1;
            double wx = xx ? tx : (1.0 - tx);
            accum_source_pixel(src, src_stride, sxi, syi, pal_rgb, wx * wy,
                               &r, &g, &b, &a);
        }
    }

    if (a <= 0.0 || a * 255.0 < (double)alpha_threshold) return 0;
    return nearest_palette_color(pal_rgb, pal_count, r / a, g / a, b / a);
}
