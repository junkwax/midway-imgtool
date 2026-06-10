/*************************************************************
 * platform/ui_canvas.h
 * Canvas/World-View rendering helpers split from imgui_overlay.cpp.
 *
 * Phase C of the overlay split. This first slice owns the single-sprite
 * World View canvas branch; the larger marked-tab World View panel remains
 * in the overlay until its own dependency closure is small enough to move.
 *************************************************************/
#pragma once

#include <SDL.h>
#include <imgui.h>
#include "img_format.h"  /* IMG */

/* Draw the single-sprite World View canvas into the current ImGui window.
   Returns true when it consumed/reserved the canvas area. */
bool DrawWorldViewSingleSprite(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io,
                               IMG *img, SDL_Texture *img_texture,
                               int image_idx, int image_count,
                               int world_w, int world_h,
                               int world_origin_x, int world_origin_y,
                               bool onion_enabled, bool mirror_active);

/* Destroy module-owned transient/cached canvas textures. */
void ClearCanvasUiTextures(void);
