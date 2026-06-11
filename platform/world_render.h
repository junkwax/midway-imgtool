/*************************************************************
 * platform/world_render.h
 * Sprite -> SDL texture rendering for the World View / composite previews.
 *
 * Extracted from the overlay split. These helpers turn an indexed IMG (in a
 * given Document's palette) into an ARGB SDL texture for
 * ImGui to draw, and manage the per-frame lifetime of those scratch textures.
 * Coupled to the SDL renderer and the active palette, but free of ImGui/panel
 * logic — the World View and composite-preview panels call in here.
 *************************************************************/
#pragma once
#include <SDL.h>
#include "document.h"    /* Document */
#include "img_format.h"  /* IMG, PAL */

/* Image/palette at index `idx` in `doc`'s linked lists, or NULL. */
IMG *doc_get_img(Document *doc, int idx);
PAL *doc_get_pal(Document *doc, int idx);

/* Build an ARGB streaming texture for `img` rendered in `doc`'s palette at the
   given alpha (index 0 is transparent). The texture is owned by the world-render
   scratch pool and freed by ClearWorldTempTextures(); do not free it directly.
   Returns NULL on failure. */
SDL_Texture *BuildWorldSpriteTexture(Document *doc, IMG *img, unsigned char alpha);

/* Destroy every texture handed out by BuildWorldSpriteTexture since the last
   call. Invoke once per frame after the world/composite draw is done. */
void ClearWorldTempTextures(void);
