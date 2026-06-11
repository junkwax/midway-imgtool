/*************************************************************
 * platform/color_ops.cpp
 * Implementation of pure color operations declared in color_ops.h.
 *************************************************************/
#include "color_ops.h"

#include "img_format.h"

#include <cstring>

static unsigned char clamp_float_to_u8(float v)
{
    int i = (int)(v * 255.0f + 0.5f);
    if (i < 0) i = 0;
    if (i > 255) i = 255;
    return (unsigned char)i;
}

static float hue2rgb(float p, float q, float t)
{
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

static void write_rgb(unsigned char *out_rgb, int idx,
                      unsigned char r, unsigned char g, unsigned char b)
{
    if (!out_rgb) return;
    out_rgb[idx * 3 + 0] = r;
    out_rgb[idx * 3 + 1] = g;
    out_rgb[idx * 3 + 2] = b;
}

void HslAdjustPaletteWordsFromBaseline(const unsigned char *baseline_words,
                                       int count,
                                       const bool *selected_mask,
                                       int hue_deg,
                                       int sat_pct,
                                       int light_pct,
                                       unsigned char *out_words,
                                       unsigned char *out_rgb)
{
    if (!baseline_words || count <= 0) return;
    if (count > 256) count = 256;

    float dh = (float)hue_deg / 360.0f;
    float ds = (float)sat_pct / 100.0f;
    float dl = (float)light_pct / 100.0f;

    bool any_selected = false;
    if (selected_mask) {
        for (int i = 0; i < count; i++) {
            if (selected_mask[i]) {
                any_selected = true;
                break;
            }
        }
    }

    for (int i = 0; i < count; i++) {
        if (any_selected && !selected_mask[i]) {
            unsigned char r, g, b;
            pal_word_to_rgb8(baseline_words + i * 2, &r, &g, &b);
            if (out_words) memcpy(out_words + i * 2, baseline_words + i * 2, 2);
            write_rgb(out_rgb, i, r, g, b);
            continue;
        }

        unsigned char br, bg, bb;
        pal_word_to_rgb8(baseline_words + i * 2, &br, &bg, &bb);
        float r = (float)br / 255.0f;
        float g = (float)bg / 255.0f;
        float b = (float)bb / 255.0f;

        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float l = (mx + mn) * 0.5f;

        float h = 0.0f;
        float s = 0.0f;
        if (mx != mn) {
            float d = mx - mn;
            s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
            if (r == mx)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
            else if (g == mx) h = (b - r) / d + 2.0f;
            else              h = (r - g) / d + 4.0f;
            h /= 6.0f;
        }

        h += dh;
        while (h < 0.0f) h += 1.0f;
        while (h >= 1.0f) h -= 1.0f;

        s += ds;
        if (s < 0.0f) s = 0.0f;
        if (s > 1.0f) s = 1.0f;

        l += dl;
        if (l < 0.0f) l = 0.0f;
        if (l > 1.0f) l = 1.0f;

        if (s < 0.0001f) {
            r = g = b = l;
        } else {
            float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
            float p = 2.0f * l - q;
            r = hue2rgb(p, q, h + 1.0f / 3.0f);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f / 3.0f);
        }

        unsigned char ri = clamp_float_to_u8(r);
        unsigned char gi = clamp_float_to_u8(g);
        unsigned char bi = clamp_float_to_u8(b);

        if (out_words) rgb8_to_pal_word(ri, gi, bi, out_words + i * 2);
        write_rgb(out_rgb, i, ri, gi, bi);
    }
}
