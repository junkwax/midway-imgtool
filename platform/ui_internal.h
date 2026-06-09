/*************************************************************
 * platform/ui_internal.h
 * Shared internal declarations for the ImGui overlay.
 *
 * Phase B of the overlay split (refactoring_plan.md): the overlay's file-scope
 * state is being moved out of the imgui_overlay.cpp monolith into ui_state.cpp
 * and declared here as `extern`, so UI code split into sibling translation
 * units (Phase C: ui_canvas, ui_palette, ...) can share it.
 *
 * This header is internal to the overlay implementation; it is NOT part of the
 * public imgui_overlay.h C API. Migration is incremental — globals are added
 * here one cohesive group at a time, building green after each.
 *************************************************************/
#pragma once
#include <SDL.h>

/* ---- SDL state ---- */
extern SDL_Window   *g_imgui_window;
extern SDL_Renderer *g_imgui_renderer;
extern SDL_Texture  *g_canvas_texture;   /* VGA plane tex — init compat, not displayed */
extern bool          g_icon_font_loaded; /* glyph icons vs short text-label fallback */

/* Per-image render texture — rebuilt when selected image or palette changes.
   (g_img_tex_idx is defined in globals.c, not here.) */
extern SDL_Texture  *g_img_texture;
extern int           g_img_tex_w;
extern int           g_img_tex_h;

/* ---- Zoom / Pan ---- */
extern float g_zoom;
extern float g_pan_x;
extern float g_pan_y;
extern bool  g_zoom_fit;
extern bool  g_zoom_reset;
extern float g_zoom_wheel_accum;
extern int   g_zoom_pending_steps;
extern bool  g_zoom_pending_fit;
extern unsigned char *g_pixel_undo;
extern int            g_pixel_undo_img;  /* -2 = never built */
