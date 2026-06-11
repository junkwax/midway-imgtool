/*************************************************************
 * platform/ui_state.cpp
 * Definitions for the overlay's shared file-scope state declared in
 * ui_internal.h. See that header for rationale (Phase B of the split).
 *
 * Holds only state definitions (no logic), so the UI rendering code can move
 * into sibling translation units that share this state via ui_internal.h.
 *************************************************************/
#include "ui_internal.h"
#include "ui_canvas.h"
#include "mk2_hitbox.h"

bool g_anipoint_drag1 = false;
bool g_anipoint_drag2 = false;
bool g_show_mk2 = false;
WorldViewState &g_world_state = WorldView();
mk2::Document g_mk2_doc;
int g_mk2_drag_corner = -1;
bool g_selection_add_drag = false;


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

/* ---- Clipboard, transform and hitbox shared state ---- */
CopiedImage g_clipboard = {false};
PastedImage g_pasted = {false};
FreeTransform g_xform = {false, true, 0,0,0,0, 0,0,0,0, 0.0f,0.0f, TransformHandle::None, 0,0, 0,0,0,0, 0.0f, 1.0f};
PasteBlendMode g_paste_blend_mode = PasteBlendMode::Normal;
int g_paste_opacity = 100;
int g_hitbox_x = 0;
int g_hitbox_y = 0;
int g_hitbox_w = 32;
int g_hitbox_h = 32;
int g_hitbox_drag_corner = -1;

bool g_show_auto_chop = false;
int g_chop_mode = 0; // AutoChopMode_BestHorizontal
int g_chop_w = 64;
int g_chop_h = 64;
bool g_chop_trim = true;

SnapBBox g_snap_bbox = {false, 0,0,0,0, -1};
bool g_snap_hit_x = false;
bool g_snap_hit_y = false;
int  g_snap_guide_x = 0;
int  g_snap_guide_y = 0;

WorldMarkedSequenceState &g_world_marked_state = WorldMarkedState();
bool g_show_dma_comp = false;


