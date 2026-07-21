extern int g_img_tex_idx;
#include "mk2_fatality.h"
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
float g_zoom_effective = 1.0f;
/* Sticky zoom the user explicitly chose (via wheel/+- steps). Carried across
   sprite/palette selection changes so the view stays at the last zoom instead
   of snapping back to the half-fit default. 0 = none (use the default). A
   "fit" action clears it. */
float g_zoom_user_pref = 0.0f;
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
bool g_paste_smooth_resize = true;
bool g_cookie_cut_mode = false;
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
bool g_world_marked_panel_docked = false;
bool g_show_dma_comp = false;

ImageListSort g_image_list_sort = ImageListSort::Original;
bool g_image_list_sort_desc = false;
bool g_show_debug = false;
bool g_show_about = false;
bool g_show_help = false;
bool g_show_seqscr_editor = false;
const float TOOLBAR_W = 76.0f;
const float PANEL_W = 280.0f;

float g_play_speed = 12.0f;
float g_play_timer = 0.0f;
unsigned int g_timeline_built_for_imgcnt = 0;
bool g_timeline_pingpong = false;
int g_timeline_play_dir = 1;
bool g_timeline_onion = false;

bool g_show_new_blank_dialog = false;
bool g_show_restore_regex = false;
bool g_show_histogram = false;
L2Report g_load2_report = {};
int g_load2_selected_idx = -1;
bool g_show_load2_verify = false;
bool g_show_mk2_fatality = false;
bool g_request_save_world_asm = false;
bool g_request_save_world_project = false;
bool g_request_load_world_project = false;
bool g_request_load_asm = false;
bool g_request_load_opp_asm = false;
bool g_request_asm_autoload = false;
bool g_request_asm_opp_autoload = false;
bool g_request_locate_img = false;
bool g_request_locate_opp_img = false;

bool g_show_new_img_confirm = false;
bool g_pending_quit = false;

/* ---- Shared modal pending action state ---- */
bool          g_show_unsaved_confirm = false;
PendingAction g_pending_action = PendingAction::None;
std::string   g_pending_action_path;
int           g_pending_tab_index = -1;
bool          g_show_delete_images_confirm = false;
char          g_pending_delete_parent_name[16] = {0};
std::vector<int> g_pending_delete_base_indices;
std::vector<int> g_pending_delete_subframe_indices;

/* ---- Shared drift texture & ASM doc variables ---- */
SDL_Texture  *g_load2_drift_tex = NULL;
int           g_load2_drift_tex_w = 0;
int           g_load2_drift_tex_h = 0;
Document     *g_asm_anim_doc = NULL;
int           g_asm_anim_doc_idx = -1;
bool          g_asm_lane_enabled = false;
bool          g_asm_opp_enabled = false;
bool          g_request_animation_sidebar = false;

int g_last_delete_removed_palettes = 0;
bool g_show_mk2_unsaved_confirm = false;
bool g_show_mk2_fatality_unsaved_confirm = false;

mk2fatal::Document g_mk2_fatality_doc;
bool g_mk2_fatality_status_sticky = false;
std::string g_mk2_fatality_status;

int g_mk2_char_idx = 0;
int g_mk2_move_idx = 0;
char g_mk2_path[1024] = "";


/* ---- Pixel Undo/Redo State ---- */
const size_t kPixelHistMax = 32;
std::vector<PixelHist> g_pixel_hist;
std::vector<PixelHist> g_pixel_redo;
unsigned int g_undo_seq = 0;

void pixel_hist_free(PixelHist *e) {
    if (e->data) free(e->data);
    e->data = NULL;
}

bool pixel_hist_capture_img(int img_idx, PixelHist *out, bool full_state) {
    IMG *img = (img_idx >= 0) ? get_img(img_idx) : NULL;
    if (!img || !img->data_p) return false;
    unsigned short stride = (img->w + 3) & ~3;
    unsigned int sz = (unsigned int)stride * img->h;
    unsigned char *buf = (unsigned char *)malloc(sz);
    if (!buf) return false;
    memcpy(buf, img->data_p, sz);
    out->img_idx = img_idx;
    out->full_state = full_state;
    out->w = img->w; out->h = img->h;
    out->anix = img->anix; out->aniy = img->aniy;
    out->anix2 = img->anix2; out->aniy2 = img->aniy2; out->aniz2 = img->aniz2;
    out->palnum = img->palnum;
    out->flags = img->flags;
    out->opals = img->opals;
    out->size = sz;
    out->data = buf;
    return true;
}

bool pixel_hist_capture(PixelHist *out, bool full_state) {
    return pixel_hist_capture_img(g_doc->ilselected, out, full_state);
}


bool pixel_hist_restore(const PixelHist *e) {
    if (!e || !e->data || e->size == 0) return false;
    IMG *img = get_img(e->img_idx);
    if (!img) return false;
    if (e->full_state) {
        unsigned char *buf = (unsigned char *)malloc(e->size);
        if (!buf) return false;
        memcpy(buf, e->data, e->size);
        free(img->data_p);
        img->data_p = buf;
        img->w = e->w; img->h = e->h;
        img->anix = e->anix; img->aniy = e->aniy;
        img->anix2 = e->anix2; img->aniy2 = e->aniy2; img->aniz2 = e->aniz2;
        img->palnum = e->palnum;
        img->flags = e->flags;
        img->opals = e->opals;
        g_zoom_reset = true;
    } else {
        if (!img->data_p || img->w != e->w || img->h != e->h) return false;
        unsigned int cur_sz = (unsigned int)((img->w + 3) & ~3) * img->h;
        if (cur_sz != e->size) return false;
        memcpy(img->data_p, e->data, e->size);
    }
    g_doc->ilselected = e->img_idx;
    g_img_tex_idx = -2;
    return true;
}

void pixel_hist_push_stroke(void) {
    PixelHist e = {};
    if (!pixel_hist_capture(&e)) return;
    e.seq = ++g_undo_seq;
    if (g_pixel_hist.size() >= kPixelHistMax) {
        pixel_hist_free(&g_pixel_hist.front());
        g_pixel_hist.erase(g_pixel_hist.begin());
    }
    g_pixel_hist.push_back(e);
    for (auto &r : g_pixel_redo) pixel_hist_free(&r);
    g_pixel_redo.clear();
    ClearDocumentRedoStack();
}


void ClearPixelHistoryStacks(void)
{
    for (auto &e : g_pixel_hist) pixel_hist_free(&e);
    for (auto &e : g_pixel_redo) pixel_hist_free(&e);
    g_pixel_hist.clear();
    g_pixel_redo.clear();
}


bool push_pixel_history_entry(PixelHist *snap)
{
    if (!snap || !snap->data) return false;
    if (snap->seq == 0) snap->seq = ++g_undo_seq;
    if (g_pixel_hist.size() >= kPixelHistMax) {
        pixel_hist_free(&g_pixel_hist.front());
        g_pixel_hist.erase(g_pixel_hist.begin());
    }
    g_pixel_hist.push_back(*snap);
    snap->data = NULL;
    for (auto &redo : g_pixel_redo) pixel_hist_free(&redo);
    g_pixel_redo.clear();
    ClearDocumentRedoStack();
    return true;
}

/* Shared Resizing State */
bool g_show_resize_sprite = false;
int  g_resize_source_idx = -1;
int  g_resize_source_w = 0;
int  g_resize_source_h = 0;
int  g_resize_w = 32;
int  g_resize_h = 32;
int  g_resize_scale_x = 100;
int  g_resize_scale_y = 100;
bool g_resize_lock_aspect = true;
int  g_resize_mode = 0;
bool g_resize_trim_bounds = false;
bool g_show_bulk_resize = false;
int  g_bulk_resize_scale_x = 100;
int  g_bulk_resize_scale_y = 100;
bool g_bulk_resize_lock_aspect = true;
int  g_bulk_resize_mode = 0;
bool g_bulk_resize_trim_bounds = false;



