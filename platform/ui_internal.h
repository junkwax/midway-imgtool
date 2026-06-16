#pragma once
#include <SDL.h>
#include <imgui.h>
#include <cstddef>
#include <vector>
#include "document.h"   /* g_doc, Document::dirty */

static const float PALETTE_H   = 112.0f;
static const float TIMELINE_H  = 108.0f;

/* ---- Dirty marking ----
   Single entry point for flagging the active document unsaved. Shared so any
   module (not just the overlay) can mark edits; the palette-usage cache it
   invalidates still lives in the overlay. Use this instead of touching
   g_doc->dirty directly so future side-effects live in one place. */
#include <string>
#include <vector>

struct SeqScrLayoutInfo {
    bool far_model;
    int record_size;
    int entry_size;
    int entry_index_off;
    int entry_ticks_off;
    int entry_dx_off;
    int entry_dy_off;
    int entry_spare1_off;
    int startx_off;
    int starty_off;
};

struct SeqScrRecordView {
    int index;
    bool script;
    size_t offset;
    size_t entries_offset;
    int flags;
    int num;
    int startx;
    int starty;
    bool truncated;
    char name[17];
};

SeqScrLayoutInfo SeqScrLayout(void);
unsigned short SeqScrReadU16(const unsigned char *p);
short SeqScrReadI16(const unsigned char *p);
bool SeqScrBuildRecords(std::vector<SeqScrRecordView> &records,
                        bool *truncated_out);
const char *SeqScrRecordTypeLabel(const SeqScrRecordView &rec);
const char *SeqScrEntryTargetName(const SeqScrRecordView &rec,
                                  int entry_index,
                                  const std::vector<SeqScrRecordView> &records);

void InvalidatePaletteUsage(void);
inline void mark_dirty(void) { g_doc->dirty = true; InvalidatePaletteUsage(); }
void ClearAll(void);
#define g_dirty (g_doc->dirty)

/* ---- ASM Animation Viewer State ---- */
struct AsmAnimFrame {
    std::vector<std::string> piece_syms; /* sprite-piece symbols composing this frame */
    std::vector<int>         piece_img;  /* resolved IMG index per piece, -1 if missing */
    std::vector<Document*>   piece_doc;  /* doc each piece resolved against (parallel) */
    int  dx = 0, dy = 0;                 /* cumulative ani_adjustx/xy offset at this frame */
    bool mirror = false;                 /* ani_flip state at this frame */
};
struct AsmAnim {
    std::string              label;      /* e.g. a_rdstance */
    std::string              name;       /* friendly name from the anitab comment, or label */
    std::vector<AsmAnimFrame> frames;
    std::vector<std::string> control;    /* control opcodes encountered (ani_jump, etc.) */
    int                      missing;    /* unresolved piece count */
};

extern bool g_openimg_for_asm;
extern bool g_openimg_for_opp;
extern std::vector<AsmAnim> g_asm_anims;
extern int g_asm_anim_sel;
extern Document *g_asm_opp_doc;
extern int g_asm_opp_doc_idx;
extern int g_asm_opp_sel;
extern std::vector<AsmAnim> g_asm_opp_anims;
extern bool g_asm_dialog_opponent;
extern bool g_show_asm_anim;

void AsmAnimSelect(int i);
void AsmResolveAnimAgainstDoc(AsmAnim &a, Document *doc);
bool LoadAsmOpponent(const char *path);
bool LoadAsmAnimations(const char *path);

/* ---- Shared overlay state & functions ---- */
extern int g_doc_tab_select_request;
void ResetPerDocumentUiState(bool clear_pixel_clipboard = false);
void Mk2AutoSelectFromImg(void);

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

/* ---- Toolbar icon glyphs ----
   Material Symbols Sharp codepoints (UTF-8). Codepoints are stable across the
   Material Symbols family — see https://fonts.google.com/icons. When
   g_icon_font_loaded is false the UI falls back to the *_TXT strings. */
#define ICON_OPEN     "\xEE\x8B\x88"     /* U+E2C8 folder_open */
#define ICON_FOLDER   "\xEE\x8B\x87"     /* U+E2C7 folder */
#define ICON_IMAGE    "\xEE\x8F\xB4"     /* U+E3F4 image */
#define ICON_VIS      "\xEE\xA3\xB4"     /* U+E8F4 visibility */
#define ICON_SAVE     "\xEE\x85\xA1"     /* U+E161 save */
#define ICON_MARK     "\xEE\xA0\xB4"     /* U+E834 check_box — reads as 'this sprite is checked/marked' */
#define ICON_MARK_ALL "\xEE\x85\xA2"     /* U+E162 select_all */
#define ICON_CLEAR    "\xEE\xA0\xB5"     /* U+E835 check_box_outline_blank — paired visually with ICON_MARK */
#define ICON_POINTS   "\xEE\x86\xB3"     /* U+E1B3 gps_fixed — concentric registration target */
#define ICON_HITBOX   "\xEE\x87\xA6"     /* U+E1E6 activity_zone */
#define ICON_MARQUEE  "\xEE\xBD\x92"     /* U+EF52 highlight_alt — dashed-rect marquee */
#define ICON_UNDO     "\xEE\x85\xA6"     /* U+E166 undo */
#define ICON_REDO     "\xEE\x85\x9A"     /* U+E15A redo */
#define ICON_RESIZE   "\xEE\xA1\x9B"     /* U+E85B aspect_ratio */
#define ICON_ZOOM_IN  "\xEE\xA3\xBF"     /* U+E8FF zoom_in */
#define ICON_ZOOM_OUT "\xEE\xA4\x80"     /* U+E900 zoom_out */
#define ICON_LOCK     "\xEE\xA2\x97"     /* U+E897 lock */
#define ICON_UNLOCK   "\xEE\xA2\x98"     /* U+E898 lock_open */
#define ICON_SUBFRAME "\xEE\x97\x9A"     /* U+E5DA subdirectory_arrow_right */

#define ICON_OPEN_TXT     "Op"
#define ICON_FOLDER_TXT   "D "
#define ICON_IMAGE_TXT    "I "
#define ICON_VIS_TXT      "V "
#define ICON_SAVE_TXT     "Sv"
#define ICON_MARK_TXT     "Mk"
#define ICON_MARK_ALL_TXT "MA"
#define ICON_CLEAR_TXT    "CM"
#define ICON_POINTS_TXT   "Pt"
#define ICON_HITBOX_TXT   "Hb"
#define ICON_MARQUEE_TXT  "[]"
#define ICON_UNDO_TXT     "Uz"
#define ICON_REDO_TXT     "Ry"
#define ICON_RESIZE_TXT   "Sz"
#define ICON_ZOOM_IN_TXT  "Z+"
#define ICON_ZOOM_OUT_TXT "Z-"
#define ICON_LOCK_TXT     "Lk"
#define ICON_UNLOCK_TXT   "Un"
#define ICON_SUBFRAME_TXT "|-"

/* ---- Zoom / Pan ---- */
extern float g_zoom;
extern float g_pan_x;
extern float g_pan_y;
extern bool  g_zoom_fit;
extern bool  g_zoom_reset;
extern float g_zoom_effective;
extern float g_zoom_user_pref;
extern float g_zoom_wheel_accum;
extern int   g_zoom_pending_steps;
extern bool  g_zoom_pending_fit;
extern unsigned char *g_pixel_undo;
extern int            g_pixel_undo_img;  /* -2 = never built */
static const float ZOOM_MAX = 128.0f;    /* max canvas/preview zoom factor */

/* ---- Undo system (geometry / metadata snapshots) ---- */
#define UNDO_STACK_SIZE 32
struct EditSnapshot {
    unsigned int   seq;
    int            image_idx;
    unsigned short anix, aniy;
    unsigned short anix2, aniy2, aniz2;
    unsigned short w, h;
    unsigned short palnum;
    unsigned short flags;
    int            hitbox_x, hitbox_y, hitbox_w, hitbox_h;
};
extern EditSnapshot g_undo[UNDO_STACK_SIZE];
extern int          g_undo_idx;
extern int          g_undo_count;

/* ---- Grid selection tool (for copy/paste) ---- */
struct GridSelection {
    bool active;        /* a selection rectangle exists and should be drawn */
    bool dragging;      /* user is currently click-dragging the rect's far corner */
    int x1, y1;         /* start coords (pixels) */
    int x2, y2;         /* end coords (pixels) */

    bool is_mask;       /* if true, pixel_mask is used instead of just the bounding box */
    int mask_w, mask_h; /* dimensions of the mask */
    std::vector<bool> pixel_mask; /* the actual selected pixels */
};
extern GridSelection g_grid_sel;
bool selection_contains_pixel(IMG *img, int x, int y);

/* ---- Palette Clipboard & Editor ---- */
struct CopiedPalette {
    bool           valid;
    unsigned short numc;
    unsigned char  bitspix;
    char           n_s[10];
    unsigned char *data;     /* numc * 2 bytes, malloc'd */
};
extern CopiedPalette g_pal_clipboard;

extern int  g_sel_color;
extern bool g_palette_selection[256];
extern int  g_isolate_color;
extern bool g_palette_nav;
extern unsigned char g_palette_baseline[512];
extern int g_palette_baseline_nc;
extern bool g_palette_drag_undo_active;
extern unsigned int g_palette_sync_serial;
void InvalidatePaletteSync(void);
void ApplyPalette(int pal_idx);

extern int g_hue_slider;
extern int g_sat_slider;
extern int g_light_slider;
extern int g_hue_last;
extern int g_sat_last;
extern int g_light_last;

/* ---- File Dialog ---- */
#include "ui_modals.h"
extern bool g_palette_export_act;

/* ---- Right Panel Palette Editor ---- */
void DrawRightPanelPaletteEditor(float panel_h);
void ApplyVariantToSelection(void);
void ApplySelectionRemapToMatchingSprites(void);
void SplitSelectionToOverlayFrame(bool clear_source);
void OpenRenamePalette(int idx);

/* ---- Tools & State ---- */
enum class ActiveTool { None, Pencil, PaintBucket, VariantPaint, Marquee, MagicWand, BackgroundEraser, CloneStamp, SmartRemap, Lasso, Eyedropper };
extern ActiveTool g_active_tool;
extern int g_pencil_brush;
extern int g_variant_brush;
extern int g_bucket_tolerance;
extern bool g_bucket_contiguous;
extern int g_wand_tolerance;
extern bool g_wand_contiguous;
extern int g_clone_brush;
extern bool g_clone_source_set;
extern int g_clone_src_x;
extern int g_clone_src_y;
extern bool g_clone_offset_set;
extern int g_clone_dx;
extern int g_clone_dy;
extern int g_remap_target_color;
extern int g_remap_tolerance;
extern int g_eraser_tolerance;
extern bool g_eraser_contiguous;
extern bool g_eraser_defringe;
extern std::vector<std::pair<int,int>> g_lasso_points;
extern bool g_show_points;
extern bool g_show_hitbox;

bool CanUndo(void);
bool CanRedo(void);
void DoUndo(void);
void DoRedo(void);
void OpenResizeSpriteDialog(void);
void OpenBulkResizeDialog(void);
void MakeDerivedImageName(const char *base, const char *suffix, char out[16]);

/* Shared Resizing State */
extern bool g_show_resize_sprite;
extern int  g_resize_source_idx;
extern int  g_resize_source_w;
extern int  g_resize_source_h;
extern int  g_resize_w;
extern int  g_resize_h;
extern int  g_resize_scale_x;
extern int  g_resize_scale_y;
extern bool g_resize_lock_aspect;
extern int  g_resize_mode;
extern bool g_resize_trim_bounds;
extern bool g_show_bulk_resize;
extern int  g_bulk_resize_scale_x;
extern int  g_bulk_resize_scale_y;
extern bool g_bulk_resize_lock_aspect;
extern int  g_bulk_resize_mode;
extern bool g_bulk_resize_trim_bounds;



/* ---- Clipboard, transform and hitbox shared state ---- */
struct CopiedImage {
    bool           valid;
    unsigned short w, h;
    void          *data_p;  /* pixel data */
    unsigned short stride;  /* bytes per row */
    bool           has_meta;
    bool           has_opaque;
    bool           from_cut;
    int            origin_x, origin_y; /* source-local top-left after tight crop */
    unsigned short palnum;
    unsigned short anix, aniy;
    unsigned short anix2, aniy2, aniz2;
    unsigned short opals;
    bool           has_opaltbl;
    unsigned char  opaltbl[16];
    bool           has_palette;
    unsigned short palette_numc;
    unsigned char  palette_data[512];
    char           source_name[16];
    char           src_filename[16];
};

struct PastedImage {
    bool active;        /* paste is active and can be moved */
    int paste_x, paste_y;  /* top-left corner where paste will go */
    bool dragging;      /* user is dragging the paste boundary */
    float drag_start_mx, drag_start_my;
    int drag_start_px, drag_start_py;
};

enum class PasteBlendMode {
    Normal = 0,
    Dissolve,
    Darken,
    Multiply,
    ColorBurn,
    LinearBurn,
    Lighten,
    Screen,
    ColorDodge,
    Overlay,
    SoftLight,
    HardLight,
    Difference,
    Exclusion
};

enum class TransformHandle {
    None,
    Move,
    Rotate,
    TL, T, TR,
    L,      R,
    BL, B, BR
};

struct FreeTransform {
    bool         active;
    bool         aspect_locked;   /* persistent — chain icon toggles */
    /* The "live" rect we render during the drag. */
    int          rx, ry, rw, rh;
    /* Snapshot taken when Ctrl+T was pressed — used to compute the scale
       factor for the final nearest-neighbor resample, and to revert on Esc. */
    int          start_x, start_y, start_w, start_h;
    float        angle_deg;
    float        start_angle_deg;
    /* Per-drag state. */
    TransformHandle handle;
    float        drag_mx, drag_my;
    int          drag_rx, drag_ry, drag_rw, drag_rh;
    float        drag_angle_deg;
    /* Reference aspect ratio captured at drag-start (w / h). */
    float        ref_aspect;
};

enum class SpriteResizeMode {
    IndexNearest = 0,
    MaxQuality = 1,
    QualitySmallBytes = 2
};

enum class SpriteTransformOp {
    FlipHorizontal = 0,
    FlipVertical,
    Rotate90CW,
    Rotate90CCW,
    Rotate180
};


enum AutoChopMode {
    AutoChopMode_BestHorizontal = 0,
    AutoChopMode_BestVertical,
    AutoChopMode_ManualGrid
};

struct AutoChopPiecePreview {
    int cell_x, cell_y, cell_w, cell_h;
    int out_x, out_y, out_w, out_h;
    int piece_no;
    int opaque_pixels;
    long long uncomp_bits;
    long long zcom_bits;
};

struct AutoChopPreview {
    std::vector<AutoChopPiecePreview> pieces;
    int target_count;
    int raw_cells;
    int empty_cells;
    int bpp;
    long long src_uncomp_bits;
    long long src_zcom_bits;
    long long split_uncomp_bits;
    long long split_zcom_bits;
    bool best_split_valid;
    bool best_split_vertical;
    int best_split_pos;
};

extern CopiedImage g_clipboard;
extern PastedImage g_pasted;
extern FreeTransform g_xform;
extern PasteBlendMode g_paste_blend_mode;
extern int g_paste_opacity;
extern int g_hitbox_x, g_hitbox_y, g_hitbox_w, g_hitbox_h;
extern int g_hitbox_drag_corner;

extern bool g_show_auto_chop;
extern int g_chop_mode;
extern int g_chop_w;
extern int g_chop_h;
extern bool g_chop_trim;

void xform_begin(void);
void xform_cancel(void);
void xform_commit(void);
void apply_pasted_region(void);
bool SelectedImageWillAutoChop(void);
bool BuildBestAutoSplitPreviewForImage(const IMG *img, bool vertical, AutoChopPreview *out);
bool BuildAutoChopPreviewForImage(const IMG *img, AutoChopPreview *out);
void DrawAutoChopPreviewRects(ImDrawList *dl, const AutoChopPreview &out, ImVec2 img_pos, float sx, float sy, bool fill);
bool TransformSelectedSprite(SpriteTransformOp op);
bool ResizeSelectedSprite(int nw, int nh, SpriteResizeMode mode, bool trim_bounds);
int AutoCalculateTimelineAnipointsFromLock(void);
void DrawResizeSpriteDialog(void);
void DrawBulkResizeDialog(void);
void DrawSpriteTransformMenuItems(void);

void resize_sync_scale_from_dims(void);
void resize_sync_dims_from_scale(void);
int BulkResizeMarkedSprites(int scale_x, int scale_y, SpriteResizeMode mode, bool trim_bounds);

int clamp_int(int v, int lo, int hi);
int round_to_int(double v);


/* ---- Shared canvas variables and functions ---- */
struct WorldViewState;
namespace mk2 {
    struct Document;
}
namespace mk2fatal {
    struct Document;
}

extern bool g_anipoint_drag1;
extern bool g_anipoint_drag2;
extern bool g_show_mk2;
extern WorldViewState &g_world_state;
extern mk2::Document g_mk2_doc;
extern int g_mk2_drag_corner;
extern bool g_selection_add_drag;

struct SnapBBox {
    bool valid;
    int min_x, min_y, max_x, max_y;
    int img_idx;        /* which image the bbox was computed from */
};
extern SnapBBox g_snap_bbox;
extern bool g_snap_hit_x;
extern bool g_snap_hit_y;
extern int  g_snap_guide_x;
extern int  g_snap_guide_y;

int Mk2CurrentRecord(void);
void selection_begin_add_drag(int sw, int sh, bool add);
void selection_finish_add_drag(int sw, int sh);
bool BuildClipboardPaletteMap(const PAL *target_pal, unsigned char map[256]);
bool paste_preview_rgba(unsigned char src_ci, unsigned char dst_ci, const PAL *target_pal, const unsigned char pal_map[256], bool remap_palette, int x, int y, int *r, int *g, int *b, int *a);
void undo_push(void);
extern const PasteBlendMode k_paste_blend_modes[14];
const char *PasteBlendModeName(PasteBlendMode mode);

struct VariantPaintResult {
    int pixels;
    int slots;
    int skipped_transparent;
    int skipped_no_slot;
};
VariantPaintResult ApplyVariantBrush(IMG *img, int cx, int cy, int brush);
void SmartErase(IMG *img, int sx, int sy, int tolerance, bool contiguous, bool defringe);
void FloodFill(IMG *img, int sx, int sy, unsigned char new_color);

struct WorldMarkedSequenceState;
extern WorldMarkedSequenceState &g_world_marked_state;
extern bool g_world_marked_panel_docked;
extern bool g_show_dma_comp;
void pixel_hist_push_stroke(void);
int PaintBucketFill(IMG *img, int sx, int sy, unsigned char new_color, int tolerance, bool contiguous);
bool DrawWorldMarkedTabs(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io);
void DrawWorldMarkedTimelinePanel(void);
bool WorldLoadSeqScrRecord(int record_index);

enum class ImageListSort { Original = 0, Name, Size };
extern ImageListSort g_image_list_sort;
extern bool g_image_list_sort_desc;
extern bool g_show_debug;
extern bool g_show_about;
extern bool g_show_help;
extern bool g_show_seqscr_editor;
extern const float TOOLBAR_W;
extern const float PANEL_W;

float DrawDocumentTabBar(float y, float sw);
void rebuild_img_texture(IMG *img);
void OpenRenameImage(void);
void DuplicateImage(void);
void DrawSpriteTransformMenuItems(void);
void RequestDeleteImage(int idx);
int CountMarkedImages(void);
void RequestDeleteMarkedImages(void);

void LabeledValue(const char *label, const char *fmt, ...);
bool AnimPointSliderInt(const char *label, int *value, int min_value, int max_value);
std::string sprite_family_key(const std::string &name);
std::string sprite_family_regex_pattern(const std::string &name);
int PushAnipointsToMatchingOpenTabs(const IMG *src, int *matched_count, int *doc_count, std::string *pattern_out);
void MirrorMarkedAnipointsToReverseWithToast(void);
int AutoCalculateTimelineAnipointsFromLock(void);

extern float g_play_speed;
extern float g_play_timer;
extern unsigned int g_timeline_built_for_imgcnt;
extern bool g_timeline_pingpong;
extern int g_timeline_play_dir;

void DrawRenameDialog(void);
void DrawLoad2VerifyDialog(void);
void DrawPaletteMergeQualityDialog(void);
void DrawSpriteLayerPanel(void);
void DrawAsmAnimWindow(void);
void DrawPaletteHistogramDialog(void);
void DrawPaletteReduceDialog(void);
void DrawMk2HitboxWindow(void);
void DrawMk2FatalityWindow(void);
void DrawAutoChopDialog(void);
void DrawResizeSpriteDialog(void);
void DrawBulkResizeDialog(void);
void DrawBulkRestoreRegexDialog(void);
void OpenOpacityGradientDialog(void);
void DrawOpacityGradientDialog(void);
void DrawDeleteImagesConfirm(void);
void DrawSeqScrEditorWindow(void);
void DrawDebugInfoModal(void);
void DrawNewImgConfirm(void);
void DrawNewBlankImageDialog(void);
void DrawHelpModal(void);
void DrawAboutModal(void);
void DrawTransientToast(float delta_time);
void DrawVerboseLogWindow(void);
void finish_sequence_anipoint_edit_if_idle(void);

extern bool g_show_new_blank_dialog;
extern bool g_show_restore_regex;
extern bool g_show_histogram;
#include "load2_verify.h"
extern L2Report g_load2_report;
extern int g_load2_selected_idx;
extern bool g_show_load2_verify;
extern bool g_show_mk2_fatality;
extern bool g_request_save_world_asm;
extern bool g_request_load_asm;
extern bool g_request_load_opp_asm;
extern bool g_request_asm_autoload;
extern bool g_request_asm_opp_autoload;
extern bool g_request_locate_img;
extern bool g_request_locate_opp_img;

void DrawUnsavedChangesConfirm(void);
void DrawMk2UnsavedChangesConfirm(void);
void DrawMk2FatalityUnsavedChangesConfirm(void);
void StripMarkedImages(int max_transparent_neighbors, int specific_color = -1);
void DitherReplaceMarkedImages(int specific_color);
void OpenRenameMarkedImages(void);
void OpenAutoChopDialog(void);
void OpenBulkResizeDialog(void);
void CopySelectionToNewImage(void);
void CutSelectionToNewImage(void);
void PasteClipboardAsNewImage(void);
void AsmProcessAutoload(void);
void AsmProcessOppAutoload(void);

void copy_image(bool cut);
void paste_image(void);
void select_all(void);
void deselect_all(void);
void invert_selection(void);
void TogglePointTable(void);
void MoveImageUp(void);
void MoveImageDown(void);
void flip_clipboard_horizontal(void);
void flip_clipboard_vertical(void);
void drop_paste_to_layer(void);
void imgtool_toggle_timeline_play(void);
void LeastSquaresReduceMarked(void); /* definition has no args, body has them locally */
void SetIDFromSecondList(void);
void SwitchImageList(void);
void ClearExtraData(void);
int RemoveHardStrokeFromTargets(int max_width);
int ApplyMarkedLikenessToSelected(void);

extern bool g_show_new_img_confirm;
extern bool g_pending_quit;

/* ---- Shared modal pending action state ---- */
enum class PendingAction { None, Quit, OpenDialog, OpenPath, OpenLodDialog, CloseTab };
extern bool          g_show_unsaved_confirm;
extern PendingAction g_pending_action;
extern std::string   g_pending_action_path;
extern int           g_pending_tab_index;
extern bool          g_show_delete_images_confirm;
extern char          g_pending_delete_parent_name[16];
extern std::vector<int> g_pending_delete_base_indices;
extern std::vector<int> g_pending_delete_subframe_indices;

void AddNewBlankImage(int w = 32, int h = 32);

/* ---- Shared drift texture & ASM doc variables ---- */
extern SDL_Texture  *g_load2_drift_tex;
extern int           g_load2_drift_tex_w;
extern int           g_load2_drift_tex_h;
extern Document     *g_asm_anim_doc;
extern int           g_asm_anim_doc_idx;
extern bool          g_asm_lane_enabled;
extern bool          g_asm_opp_enabled;

void update_drift_texture(IMG *img);
void Mk2SelectRecord(int rec_idx);

struct SpriteLayer;
void flip_layer_horizontal(SpriteLayer *L);
void flip_layer_vertical(SpriteLayer *L);
void flatten_img_layer(IMG *img);
void delete_img_layer(IMG *img);

/* ---- AutoChop / AutoSplit target structures ---- */
struct AutoSplitTargetSummary {
    AutoChopPreview selected_preview;
    int target_count;
    int split_count;
    int skipped_count;
    int bpp;
    long long src_zcom_bits;
    long long split_zcom_bits;
};

extern int g_last_delete_removed_palettes;
extern bool g_show_mk2_unsaved_confirm;
extern bool g_show_mk2_fatality_unsaved_confirm;
extern const char *g_help_text;

extern mk2fatal::Document g_mk2_fatality_doc;
extern bool g_mk2_fatality_status_sticky;
extern std::string g_mk2_fatality_status;

void BuildAutoChopTargetSummary(AutoChopPreview *out);
void BuildAutoSplitTargetSummary(bool vertical, AutoSplitTargetSummary *summary);
void AutoChopSetThreeBandSize(void);
int ApplyBestAutoSplitToTargets(bool vertical);
void NormalizeImageDeleteIndices(std::vector<int> *indices);
int DeleteImagesByIndices(std::vector<int> indices);
void ClearPendingImageDelete(void);
int FindDirtyDocumentIndex(void);

static const int k_auto_split_min_side = 5;
extern int g_mk2_char_idx;
extern int g_mk2_move_idx;
extern char g_mk2_path[1024];

/* ---- Pixel History Undo/Redo ---- */
struct PixelHist {
    unsigned int   seq;         /* global undo ordering across stack types */
    int            img_idx;     /* index into the IMG list at the time */
    bool           full_state;  /* true for geometry-changing operations */
    unsigned short w, h;        /* sentinels for stale-redo protection */
    unsigned short anix, aniy;
    unsigned short anix2, aniy2, aniz2;
    unsigned short palnum;
    unsigned short flags;
    unsigned short opals;
    unsigned int   size;        /* bytes in `data` */
    unsigned char *data;        /* owned malloc()'d buffer */
};
extern const size_t kPixelHistMax;
extern std::vector<PixelHist> g_pixel_hist;
extern std::vector<PixelHist> g_pixel_redo;
extern unsigned int g_undo_seq;
void ClearDocumentRedoStack(void);
void ClearDocumentHistoryStacks(void);
void pixel_hist_free(PixelHist *e);
bool pixel_hist_capture_img(int img_idx, PixelHist *out, bool full_state = false);
bool pixel_hist_capture(PixelHist *out, bool full_state = false);
bool pixel_hist_restore(const PixelHist *e);
void pixel_hist_push_stroke(void);
void ClearPixelHistoryStacks(void);
bool push_pixel_history_entry(PixelHist *snap);
void unlink_and_free_img(IMG *victim);
