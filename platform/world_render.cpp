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

void ClearWorldTempTextures(void)
{
    for (SDL_Texture *tex : g_world_temp_textures) {
        if (tex) SDL_DestroyTexture(tex);
    }
    g_world_temp_textures.clear();
}
