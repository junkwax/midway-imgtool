/*************************************************************
 * platform/world_render.cpp
 * Sprite -> SDL texture rendering declared in world_render.h.
 *************************************************************/
#include "world_render.h"
#include "ui_internal.h"  /* g_imgui_renderer */
#include "shim_vid.h"     /* g_palette */

#include <vector>

/* Scratch textures handed out this frame; freed by ClearWorldTempTextures(). */
static std::vector<SDL_Texture *> g_world_temp_textures;

IMG *doc_get_img(Document *doc, int idx)
{
    if (!doc || idx < 0) return NULL;
    IMG *img = (IMG *)doc->img_p;
    for (int i = 0; i < idx && img; i++) img = (IMG *)img->nxt_p;
    return img;
}

PAL *doc_get_pal(Document *doc, int idx)
{
    if (!doc || idx < 0) return NULL;
    PAL *pal = (PAL *)doc->pal_p;
    for (int i = 0; i < idx && pal; i++) pal = (PAL *)pal->nxt_p;
    return pal;
}

SDL_Texture *BuildWorldSpriteTexture(Document *doc, IMG *img, unsigned char alpha)
{
    if (!g_imgui_renderer || !doc || !img || !img->data_p || img->w == 0 || img->h == 0)
        return NULL;

    SDL_Texture *tex = SDL_CreateTexture(g_imgui_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        img->w, img->h);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    void *pixels; int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    PAL *pal = doc_get_pal(doc, img->palnum);
    const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : NULL;
    int pal_colors = pal ? (int)pal->numc : 0;
    int stride = (img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char ci = src[y * stride + x];
            Uint32 r = 200, g = 200, b = 200;
            if (pd && ci < pal_colors) {
                unsigned short w15 = (unsigned short)(pd[ci * 2] | (pd[ci * 2 + 1] << 8));
                r = (((w15 >> 10) & 0x1F) << 3);
                g = (((w15 >>  5) & 0x1F) << 3);
                b = (( w15        & 0x1F) << 3);
            } else if (doc == g_doc) {
                SDL_Color c = g_palette[ci];
                r = c.r; g = c.g; b = c.b;
            }
            Uint32 a = (ci == 0) ? 0u : (Uint32)alpha;
            dst[y * (pitch / 4) + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SDL_UnlockTexture(tex);
    g_world_temp_textures.push_back(tex);
    return tex;
}

/* Resolve one palette index to 8-bit RGB using the same rules as the texture
   path: the owning doc's palette word first, then the live VGA palette when the
   sprite belongs to the active document, then a neutral grey. */
static void world_index_to_rgb(Document *doc, const unsigned char *pd,
                               int pal_colors, unsigned char ci,
                               unsigned char *r, unsigned char *g,
                               unsigned char *b)
{
    if (pd && ci < pal_colors) {
        unsigned short w15 = (unsigned short)(pd[ci * 2] | (pd[ci * 2 + 1] << 8));
        *r = (unsigned char)(((w15 >> 10) & 0x1F) << 3);
        *g = (unsigned char)(((w15 >>  5) & 0x1F) << 3);
        *b = (unsigned char)(( w15        & 0x1F) << 3);
    } else if (doc == g_doc) {
        SDL_Color c = g_palette[ci];
        *r = c.r; *g = c.g; *b = c.b;
    } else {
        *r = 200; *g = 200; *b = 200;
    }
}

int WorldBlitSpriteRgba(Document *doc, IMG *img, unsigned char alpha,
                        bool mirror_x, bool mirror_y,
                        int dst_x, int dst_y,
                        unsigned char *rgba, int rgba_w, int rgba_h)
{
    if (!doc || !img || !img->data_p || img->w == 0 || img->h == 0)
        return 0;
    if (!rgba || rgba_w <= 0 || rgba_h <= 0) return 0;

    PAL *pal = doc_get_pal(doc, img->palnum);
    const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : NULL;
    int pal_colors = pal ? (int)pal->numc : 0;
    int sw = (int)img->w;
    int sh = (int)img->h;
    int stride = (sw + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;

    /* Clip the sprite rect against the buffer once instead of per pixel. */
    int x0 = dst_x < 0 ? -dst_x : 0;
    int y0 = dst_y < 0 ? -dst_y : 0;
    int x1 = sw, y1 = sh;
    if (dst_x + x1 > rgba_w) x1 = rgba_w - dst_x;
    if (dst_y + y1 > rgba_h) y1 = rgba_h - dst_y;
    if (x0 >= x1 || y0 >= y1) return 0;

    int written = 0;
    for (int y = y0; y < y1; y++) {
        int sy = mirror_y ? (sh - 1 - y) : y;
        unsigned char *drow = rgba + (size_t)(dst_y + y) * rgba_w * 4;
        for (int x = x0; x < x1; x++) {
            int sx = mirror_x ? (sw - 1 - x) : x;
            unsigned char ci = src[sy * stride + sx];
            if (ci == 0) continue;   /* index 0 is the transparent key */

            unsigned char r, g, b;
            world_index_to_rgb(doc, pd, pal_colors, ci, &r, &g, &b);

            unsigned char *d = drow + (size_t)(dst_x + x) * 4;
            if (alpha >= 255 || d[3] == 0) {
                d[0] = r; d[1] = g; d[2] = b;
                d[3] = (d[3] > alpha) ? d[3] : alpha;
            } else {
                /* Straight source-over in 8-bit; matches how the renderer
                   blends a lane's translucent sprite onto what's under it. */
                int a = alpha;
                int inv = 255 - a;
                d[0] = (unsigned char)((r * a + d[0] * inv) / 255);
                d[1] = (unsigned char)((g * a + d[1] * inv) / 255);
                d[2] = (unsigned char)((b * a + d[2] * inv) / 255);
                int da = d[3] + a - (d[3] * a) / 255;
                d[3] = (unsigned char)(da > 255 ? 255 : da);
            }
            written++;
        }
    }
    return written;
}

void ClearWorldTempTextures(void)
{
    for (SDL_Texture *tex : g_world_temp_textures) {
        if (tex) SDL_DestroyTexture(tex);
    }
    g_world_temp_textures.clear();
}
