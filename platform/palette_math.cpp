/*************************************************************
 * platform/palette_math.cpp
 * Implementation of the pure palette-word color math helpers
 * declared in palette_math.h. See that header for rationale.
 *************************************************************/
#include "palette_math.h"

int PaletteBppForColorCount(int numc)
{
    /* Highest index that must be representable is numc - 1, so this is the
       smallest b with (1 << b) >= numc. Anything at or below 2 colors still
       needs a whole bit per pixel. */
    if (numc <= 2) return 1;
    if (numc > 256) numc = 256;
    int bpp = 1;
    while ((1 << bpp) < numc) bpp++;
    return bpp;
}

int PaletteColorCountForBpp(int bpp)
{
    if (bpp < 1) bpp = 1;
    if (bpp > 8) bpp = 8;
    return 1 << bpp;
}

bool PaletteBppTooSmall(int bitspix, int numc)
{
    if (numc <= 0) return false;
    if (bitspix < 1 || bitspix > 8) return true;
    return PaletteColorCountForBpp(bitspix) < numc;
}

unsigned short palette_word_at(const unsigned char *data, int idx)
{
    return (unsigned short)(data[idx * 2] | (data[idx * 2 + 1] << 8));
}

int palette_word_distance_sq(unsigned short a, unsigned short b)
{
    int ar = (a >> 10) & 0x1F, ag = (a >> 5) & 0x1F, ab = a & 0x1F;
    int br = (b >> 10) & 0x1F, bg = (b >> 5) & 0x1F, bb = b & 0x1F;
    int dr = ar - br, dg = ag - bg, db = ab - bb;
    return dr * dr + dg * dg + db * db;
}

unsigned char nearest_palette_index_for_word(unsigned short src_word,
                                             const PAL *target_pal)
{
    if (!target_pal || !target_pal->data_p || target_pal->numc <= 1) return 0;
    const unsigned char *td = (const unsigned char *)target_pal->data_p;
    int n = target_pal->numc;
    if (n > 256) n = 256;

    int best_idx = 1;
    int best_dist = 0x7FFFFFFF;
    for (int i = 1; i < n; i++) {
        int dist = palette_word_distance_sq(src_word, palette_word_at(td, i));
        if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
            if (dist == 0) break;
        }
    }
    return (unsigned char)best_idx;
}

unsigned short pal_word_or_black(PAL *pal, int idx)
{
    if (!pal || !pal->data_p || idx < 0 || idx >= (int)pal->numc || idx >= 256) return 0;
    const unsigned char *pd = (const unsigned char *)pal->data_p;
    return (unsigned short)(pd[idx * 2] | (pd[idx * 2 + 1] << 8));
}

unsigned short rgb_to_word15(unsigned char r, unsigned char g, unsigned char b)
{
    unsigned char tmp[2];
    rgb8_to_pal_word(r, g, b, tmp);
    return (unsigned short)(tmp[0] | (tmp[1] << 8));
}

int PaletteColorDistance5(unsigned short a, unsigned short b)
{
    int ar = (a >> 10) & 0x1F;
    int ag = (a >>  5) & 0x1F;
    int ab =  a        & 0x1F;
    int br = (b >> 10) & 0x1F;
    int bg = (b >>  5) & 0x1F;
    int bb =  b        & 0x1F;
    int dr = ar - br;
    int dg = ag - bg;
    int db = ab - bb;
    return dr * dr + dg * dg + db * db;
}

int PaletteColorDistance5W(unsigned short a, unsigned short b, bool perceptual)
{
    int dr = ((a >> 10) & 0x1F) - ((b >> 10) & 0x1F);
    int dg = ((a >>  5) & 0x1F) - ((b >>  5) & 0x1F);
    int db = ( a        & 0x1F) - ( b        & 0x1F);
    if (perceptual)
        return 30 * dr * dr + 59 * dg * dg + 11 * db * db;
    return dr * dr + dg * dg + db * db;
}

int FindNearestMergedSlot(const PAL *target, int base_count,
                          const unsigned short *added, int added_count,
                          unsigned short color_word, bool perceptual)
{
    int best = 0;
    int best_dist = 0x7FFFFFFF;
    if (target && target->data_p) {
        const unsigned char *td = (const unsigned char *)target->data_p;
        if (base_count > 256) base_count = 256;
        for (int i = 1; i < base_count; i++) {
            unsigned short w = (unsigned short)(td[i * 2] | (td[i * 2 + 1] << 8));
            int dist = PaletteColorDistance5W(color_word, w, perceptual);
            if (dist < best_dist) { best_dist = dist; best = i; if (dist == 0) return best; }
        }
    }
    for (int j = 0; j < added_count; j++) {
        int dist = PaletteColorDistance5W(color_word, added[j], perceptual);
        if (dist < best_dist) { best_dist = dist; best = base_count + j; if (dist == 0) return best; }
    }
    return best;
}

int FindNearestPaletteSlot(const PAL *pal, unsigned short color_word)
{
    if (!pal || !pal->data_p || pal->numc <= 1) return 0;

    const unsigned char *pd = (const unsigned char *)pal->data_p;
    int count = pal->numc;
    if (count > 256) count = 256;

    int best = 1;
    int best_dist = 0x7FFFFFFF;
    for (int i = 1; i < count; i++) {
        unsigned short w = (unsigned short)(pd[i * 2] | (pd[i * 2 + 1] << 8));
        int dist = PaletteColorDistance5(color_word, w);
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
            if (dist == 0) break;
        }
    }
    return best;
}
