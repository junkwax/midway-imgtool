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

/* ---- Undo system ---- */
EditSnapshot g_undo[UNDO_STACK_SIZE];
int          g_undo_idx   = -1;
int          g_undo_count =  0;

GridSelection g_grid_sel = {false, false, 0, 0, 0, 0, false, 0, 0, {}};

/* ---- Palette Clipboard & Editor ---- */
CopiedPalette g_pal_clipboard = {false, 0, 0, {0}, NULL};

int  g_sel_color   = 0;
bool g_palette_selection[256] = {false};
int  g_isolate_color = -1;
bool g_palette_nav   = false;
unsigned char g_palette_baseline[512] = {0};
int g_palette_baseline_nc = 0;
bool g_palette_drag_undo_active = false;
unsigned int g_palette_sync_serial = 1;

int g_hue_slider = 0;
int g_sat_slider = 0;
int g_light_slider = 0;
int g_hue_last = 0;
int g_sat_last = 0;
int g_light_last = 0;

bool g_palette_export_act = false;

/* ---- Tools & State ---- */
ActiveTool g_active_tool = ActiveTool::None;
int g_pencil_brush = 1;
int g_variant_brush = 1;
int g_bucket_tolerance = 0;
bool g_bucket_contiguous = true;
int g_wand_tolerance = 0;
bool g_wand_contiguous = true;
int g_clone_brush = 1;
bool g_clone_source_set = false;
int g_clone_src_x = 0;
int g_clone_src_y = 0;
bool g_clone_offset_set = false;
int g_clone_dx = 0;
int g_clone_dy = 0;
int g_remap_target_color = -1;
int g_remap_tolerance = 0;
int g_eraser_tolerance = 0;
bool g_eraser_contiguous = true;
bool g_eraser_defringe = true;
std::vector<std::pair<int,int>> g_lasso_points;
bool g_show_points = true;
bool g_show_hitbox = false;


