/*************************************************************
 * platform/paint_tools.cpp
 * See paint_tools.h for why everything here round-trips through RGB.
 *************************************************************/
#include "paint_tools.h"
#include "palette_math.h"

#include <vector>
#include <cstring>

namespace {

struct Rgb { int r, g, b; };

inline unsigned short StrideOf(const IMG *img)
{
    return (unsigned short)((img->w + 3) & ~3);
}

/* Palette colours expanded to 8-bit once per stroke. Doing this per pixel
   would re-unpack the same 5-bit fields thousands of times per drag. */
struct PaletteCache {
    int count = 0;
    Rgb rgb[256] = {};
    unsigned short word[256] = {};

    void build(const PAL *pal)
    {
        count = 0;
        if (!pal || !pal->data_p) return;
        int n = (int)pal->numc;
        if (n > 256) n = 256;
        const unsigned char *d = (const unsigned char *)pal->data_p;
        for (int i = 0; i < n; i++) {
            unsigned short w = (unsigned short)(d[i * 2] | (d[i * 2 + 1] << 8));
            word[i] = w;
            /* 5-bit channels scaled to 8-bit; <<3 | >>2 keeps white at 255. */
            int r5 = (w >> 10) & 0x1F, g5 = (w >> 5) & 0x1F, b5 = w & 0x1F;
            rgb[i].r = (r5 << 3) | (r5 >> 2);
            rgb[i].g = (g5 << 3) | (g5 >> 2);
            rgb[i].b = (b5 << 3) | (b5 >> 2);
        }
        count = n;
    }
};

/* Nearest opaque index for an 8-bit RGB triple. Index 0 is transparent and is
   never returned, so blending can't accidentally punch a hole. */
unsigned char NearestIndex(const PaletteCache &pc, int r, int g, int b)
{
    if (pc.count <= 1) return 1;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    int best = 1;
    long best_d = -1;
    for (int i = 1; i < pc.count; i++) {
        long dr = r - pc.rgb[i].r, dg = g - pc.rgb[i].g, db = b - pc.rgb[i].b;
        long d = dr * dr + dg * dg + db * db;
        if (best_d < 0 || d < best_d) { best_d = d; best = i; }
    }
    return (unsigned char)best;
}

inline bool InBounds(const IMG *img, int x, int y)
{
    return x >= 0 && y >= 0 && x < (int)img->w && y < (int)img->h;
}

/* Pixels of the brush disc that lie inside the image. radius 1 = one pixel. */
void CollectDisc(const IMG *img, int cx, int cy, int radius,
                 std::vector<std::pair<int,int>> &out)
{
    out.clear();
    int r = radius > 0 ? radius : 1;
    int rr = (r - 1) * (r - 1);
    for (int dy = -(r - 1); dy <= (r - 1); dy++) {
        for (int dx = -(r - 1); dx <= (r - 1); dx++) {
            if (r > 1 && dx * dx + dy * dy > rr) continue;
            int x = cx + dx, y = cy + dy;
            if (InBounds(img, x, y)) out.push_back({x, y});
        }
    }
}

} /* namespace */

int PaintBlurStamp(IMG *img, const PAL *pal, int cx, int cy,
                   int radius, int strength)
{
    if (!img || !img->data_p || img->w <= 0 || img->h <= 0) return 0;
    PaletteCache pc;
    pc.build(pal);
    if (pc.count <= 1) return 0;
    if (strength < 1) strength = 1;
    if (strength > 100) strength = 100;

    unsigned short stride = StrideOf(img);
    unsigned char *data = (unsigned char *)img->data_p;

    std::vector<std::pair<int,int>> disc;
    CollectDisc(img, cx, cy, radius, disc);
    if (disc.empty()) return 0;

    /* Sample from a snapshot so pixels blurred earlier in the stroke don't
       feed back into their neighbours and smear directionally. */
    std::vector<unsigned char> src(disc.size());
    for (size_t i = 0; i < disc.size(); i++)
        src[i] = data[disc[i].second * stride + disc[i].first];

    int changed = 0;
    for (size_t i = 0; i < disc.size(); i++) {
        int x = disc[i].first, y = disc[i].second;
        unsigned char here = src[i];
        if (here == 0) continue;              /* don't grow the silhouette */

        long sr = 0, sg = 0, sb = 0;
        int n = 0;
        for (int oy = -1; oy <= 1; oy++) {
            for (int ox = -1; ox <= 1; ox++) {
                int nx = x + ox, ny = y + oy;
                if (!InBounds(img, nx, ny)) continue;
                unsigned char ci = data[ny * stride + nx];
                if (ci == 0 || ci >= pc.count) continue;  /* transparent neighbours don't dilute */
                sr += pc.rgb[ci].r; sg += pc.rgb[ci].g; sb += pc.rgb[ci].b;
                n++;
            }
        }
        if (n == 0) continue;
        int ar = (int)(sr / n), ag = (int)(sg / n), ab = (int)(sb / n);

        const Rgb &orig = pc.rgb[here < pc.count ? here : 0];
        int mr = orig.r + (ar - orig.r) * strength / 100;
        int mg = orig.g + (ag - orig.g) * strength / 100;
        int mb = orig.b + (ab - orig.b) * strength / 100;

        unsigned char out = NearestIndex(pc, mr, mg, mb);
        if (out != here) { data[y * stride + x] = out; changed++; }
    }
    return changed;
}

int PaintSmudgeStamp(IMG *img, const PAL *pal, int px, int py,
                     int cx, int cy, int radius, int strength)
{
    if (!img || !img->data_p || img->w <= 0 || img->h <= 0) return 0;
    PaletteCache pc;
    pc.build(pal);
    if (pc.count <= 1) return 0;
    if (strength < 1) strength = 1;
    if (strength > 100) strength = 100;

    int mvx = cx - px, mvy = cy - py;
    if (mvx == 0 && mvy == 0) return 0;   /* nothing to drag */

    unsigned short stride = StrideOf(img);
    unsigned char *data = (unsigned char *)img->data_p;

    std::vector<std::pair<int,int>> disc;
    CollectDisc(img, cx, cy, radius, disc);
    if (disc.empty()) return 0;

    /* Read the trailing colours before writing anything, so the whole stamp
       pulls from where the brush was, not from itself mid-update. */
    std::vector<int> pulled(disc.size(), -1);
    for (size_t i = 0; i < disc.size(); i++) {
        int sx = disc[i].first - mvx, sy = disc[i].second - mvy;
        if (!InBounds(img, sx, sy)) continue;
        pulled[i] = data[sy * stride + sx];
    }

    int changed = 0;
    for (size_t i = 0; i < disc.size(); i++) {
        int from = pulled[i];
        if (from <= 0 || from >= pc.count) continue;   /* nothing (or transparency) to carry */
        int x = disc[i].first, y = disc[i].second;
        unsigned char here = data[y * stride + x];
        if (here == 0) continue;                        /* don't paint outside the sprite */

        const Rgb &a = pc.rgb[here < pc.count ? here : 0];
        const Rgb &b = pc.rgb[from];
        int mr = a.r + (b.r - a.r) * strength / 100;
        int mg = a.g + (b.g - a.g) * strength / 100;
        int mb = a.b + (b.b - a.b) * strength / 100;

        unsigned char out = NearestIndex(pc, mr, mg, mb);
        if (out != here) { data[y * stride + x] = out; changed++; }
    }
    return changed;
}

/* Shared core: clear `region`, then diffuse colour inward from its border.
   Each pass averages every still-empty cell over whatever filled neighbours it
   has, so colour creeps in from the rim. Cells that never acquire a neighbour
   (the region touches only transparency) stay transparent. */
static int ContentAwareEraseRegion(IMG *img, const PAL *pal,
                                   const std::vector<std::pair<int,int>> &region,
                                   int passes)
{
    if (!img || !img->data_p || region.empty()) return 0;
    PaletteCache pc;
    pc.build(pal);
    if (pc.count <= 1) return 0;
    if (passes < 1) passes = 1;
    if (passes > 64) passes = 64;

    unsigned short stride = StrideOf(img);
    unsigned char *data = (unsigned char *)img->data_p;

    /* Mark membership so neighbour lookups can tell "inside the hole" from
       "outside, keep as reference". */
    std::vector<unsigned char> inside((size_t)img->w * (size_t)img->h, 0);
    for (const auto &p : region)
        inside[(size_t)p.second * (size_t)img->w + (size_t)p.first] = 1;

    /* Working colour buffer for the hole: -1 = still empty. */
    std::vector<int> fill(region.size(), -1);

    for (int pass = 0; pass < passes; pass++) {
        bool any = false;
        std::vector<int> next = fill;
        for (size_t i = 0; i < region.size(); i++) {
            if (fill[i] >= 0) continue;
            int x = region[i].first, y = region[i].second;
            long sr = 0, sg = 0, sb = 0;
            int n = 0;
            for (int oy = -1; oy <= 1; oy++) {
                for (int ox = -1; ox <= 1; ox++) {
                    if (!ox && !oy) continue;
                    int nx = x + ox, ny = y + oy;
                    if (!InBounds(img, nx, ny)) continue;
                    size_t ni = (size_t)ny * (size_t)img->w + (size_t)nx;
                    int ci;
                    if (inside[ni]) {
                        /* Another hole cell: usable only once it has been filled
                           by an earlier pass. */
                        int found = -1;
                        for (size_t k = 0; k < region.size(); k++) {
                            if (region[k].first == nx && region[k].second == ny) {
                                found = fill[k];
                                break;
                            }
                        }
                        ci = found;
                    } else {
                        ci = data[ny * stride + nx];
                        if (ci == 0) ci = -1;   /* transparency is not a colour */
                    }
                    if (ci <= 0 || ci >= pc.count) continue;
                    sr += pc.rgb[ci].r; sg += pc.rgb[ci].g; sb += pc.rgb[ci].b;
                    n++;
                }
            }
            if (n == 0) continue;
            next[i] = NearestIndex(pc, (int)(sr / n), (int)(sg / n), (int)(sb / n));
            any = true;
        }
        fill.swap(next);
        if (!any) break;
    }

    int changed = 0;
    for (size_t i = 0; i < region.size(); i++) {
        int x = region[i].first, y = region[i].second;
        unsigned char out = (fill[i] > 0) ? (unsigned char)fill[i] : 0;
        if (data[y * stride + x] != out) { data[y * stride + x] = out; changed++; }
    }
    return changed;
}

int PaintContentAwareErase(IMG *img, const PAL *pal, int cx, int cy,
                           int radius, int passes)
{
    if (!img || !img->data_p) return 0;
    std::vector<std::pair<int,int>> disc;
    CollectDisc(img, cx, cy, radius, disc);
    return ContentAwareEraseRegion(img, pal, disc, passes);
}

int PaintContentAwareEraseRect(IMG *img, const PAL *pal,
                               int x0, int y0, int x1, int y1, int passes)
{
    if (!img || !img->data_p) return 0;
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    std::vector<std::pair<int,int>> rect;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++)
            if (InBounds(img, x, y)) rect.push_back({x, y});
    return ContentAwareEraseRegion(img, pal, rect, passes);
}
