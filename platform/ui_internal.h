#pragma once
#include <SDL.h>
#include <vector>
#include "document.h"   /* g_doc, Document::dirty */

static const float PALETTE_H   = 112.0f;
static const float TIMELINE_H  = 108.0f;

/* ---- Dirty marking ----
   Single entry point for flagging the active document unsaved. Shared so any
   module (not just the overlay) can mark edits; the palette-usage cache it
   invalidates still lives in the overlay. Use this instead of touching
   g_doc->dirty directly so future side-effects live in one place. */
void InvalidatePaletteUsage(void);
inline void mark_dirty(void) { g_doc->dirty = true; InvalidatePaletteUsage(); }

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
enum class FileDialogMode {
    OpenImg, AppendImg, OpenLod, SaveImg, ExportTga, LoadLbm, SaveLbm,
    SaveMarkedLbm, LoadTga, SaveTga, ImportPng, ImportPngMatch,
    ImportSpriteSheetMatch, ImportGif, ExportPng, ExportPalette,
    ImportPalette, WriteAniLst, WriteTbl, WriteIrw, LoadAsmAnim, SaveAsmAnim
};
extern bool g_palette_export_act;
void OpenFileDialog(FileDialogMode mode);

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


