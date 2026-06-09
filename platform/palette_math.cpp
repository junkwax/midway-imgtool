/*************************************************************
 * platform/palette_math.cpp
 * Implementation of the pure palette-word color math helpers
 * declared in palette_math.h. See that header for rationale.
 *************************************************************/
#include "palette_math.h"

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
