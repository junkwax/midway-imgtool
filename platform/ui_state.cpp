/*************************************************************
 * platform/ui_state.cpp
 * Definitions for the overlay's shared file-scope state declared in
 * ui_internal.h. See that header for rationale (Phase B of the split).
 *
 * Holds only state definitions (no logic), so the UI rendering code can move
 * into sibling translation units that share this state via ui_internal.h.
 *************************************************************/
#include "ui_internal.h"

/* ---- SDL state ---- */
SDL_Window   *g_imgui_window   = NULL;
SDL_Renderer *g_imgui_renderer = NULL;
SDL_Texture  *g_canvas_texture = NULL;
bool          g_icon_font_loaded = false;

SDL_Texture  *g_img_texture    = NULL;
int           g_img_tex_w      = 0;
int           g_img_tex_h      = 0;

/* ---- Zoom / Pan ---- */
float g_zoom       = 1.0f;
float g_pan_x      = 0.0f;
float g_pan_y      = 0.0f;
bool  g_zoom_fit   = true;
bool  g_zoom_reset = true;
float g_zoom_wheel_accum   = 0.0f;
int   g_zoom_pending_steps = 0;
bool  g_zoom_pending_fit   = false;
unsigned char *g_pixel_undo     = NULL;
int            g_pixel_undo_img = -1;  /* -2 = never built */
