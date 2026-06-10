/*************************************************************
 * platform/imgui_overlay.cpp
 * Fixed Adobe/GIMP-style ImGui UI overlay
 * Layout: menu bar + left toolbar + center canvas + right panel strip + bottom palette
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL.h>
#include <cstdint>
#include <cstring>
#include <string>
#include <cstdio>
#include <cstdarg>
#include <cstdlib>
#include <cctype>
#include <cfloat>
#include <vector>
#include <algorithm>
#include <functional>
#include <regex>
#include <unordered_map>
#include <cmath>
#include "compat.h"
#ifdef _WIN32
#include <shlobj.h>
#include <commdlg.h>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "comdlg32.lib")
#endif
#include "img_format.h"
#include "color_ops.h"
#include "image_ops.h"
#include "palette_math.h"
#include "ui_internal.h"
#include "ui_canvas.h"
#include "ui_timeline.h"
#include "world_render.h"
#include "anipoint.h"
#include "anipoint_edit.h"
#include "img_util.h"
#include "sprite_resize_ops.h"
#include "img_io.h"
#include "imgui_overlay.h"
#include "load2_verify.h"
#include "lod_parser.h"
#include "mk2_hitbox.h"
#include "mk2_fatality.h"

/* PPP setting from img_io.cpp — used to drive the verifier modal. */

/* Document-state globals are reached through g_doc->X (see document.h),
   pulled in via img_format.h below. App-wide externs live here. */
extern "C" {
extern char             exe_dir[];
extern struct SDL_Color g_palette[256];
}

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

static LONG WINAPI CrashHandlerExceptionFilter(EXCEPTION_POINTERS* pExceptionPointers) {
    HANDLE hFile = CreateFileA("crashdump.dmp", GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION dumpInfo;
        dumpInfo.ThreadId = GetCurrentThreadId();
        dumpInfo.ExceptionPointers = pExceptionPointers;
        dumpInfo.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile, MiniDumpNormal, &dumpInfo, NULL, NULL);
        CloseHandle(hFile);
    }
    MessageBoxA(NULL, "The application has crashed.\nA crashdump.dmp file has been generated.", "Fatal Error", MB_ICONERROR | MB_OK);
    return EXCEPTION_EXECUTE_HANDLER;
}
#else
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <limits.h>
#endif

static void PosixCrashHandler(int sig) {
    fprintf(stderr, "CRASH: Fatal signal %d received!\n", sig);
    exit(1);
}
#endif

/* SDL state (g_imgui_window/renderer, g_canvas_texture) and the icon-font flag
   g_icon_font_loaded now live in ui_state.cpp; see ui_internal.h. */

/* Toolbar icon glyph macros (ICON_* / ICON_*_TXT) now live in ui_internal.h. */

/* Per-image render texture state (g_img_texture / _w / _h) lives in
   ui_state.cpp. g_img_tex_idx is defined in globals.c. */
extern int           g_img_tex_idx;

/* ---- Zoom / Pan ---- */
/* Mutable zoom/pan + pixel-undo state and ZOOM_MAX live in ui_state.cpp /
   ui_internal.h. */

/* Zoom/pan helpers now live in ui_canvas.{h,cpp}. */

/* Per-stroke pixel history. Stack of pre-stroke snapshots: each entry
   captures the full pixel buffer of one image at the moment a paint
   stroke began. Push on the first frame of mouse-down for any pixel
   tool (Pencil, Clone Stamp, Smart Eraser, Smart Remap, Flood Fill);
   Ctrl+Z pops the most recent entry, swapping current pixels with the
   snapshot and pushing the prior state onto the redo stack. */
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
static std::vector<PixelHist> g_pixel_hist;   /* undo stack */
static std::vector<PixelHist> g_pixel_redo;   /* redo stack */
static const size_t kPixelHistMax = 32;
static unsigned int g_undo_seq = 0;
static void ClearDocumentRedoStack(void);
static bool push_pixel_history_entry(PixelHist *snap);
/* Free the buffer inside a PixelHist (caller still owns the vector slot). */
static inline void pixel_hist_free(PixelHist *e) {
    if (e->data) free(e->data);
    e->data = NULL;
}
/* Capture the current pixel buffer of an image into `out`.
   Returns true if `out` now owns a valid snapshot. */
static bool pixel_hist_capture_img(int img_idx, PixelHist *out, bool full_state = false) {
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
static bool pixel_hist_capture(PixelHist *out, bool full_state = false) {
    return pixel_hist_capture_img(g_doc->ilselected, out, full_state);
}

static bool pixel_hist_restore(const PixelHist *e) {
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
/* Stroke-begin: push a fresh pre-stroke snapshot. Drops the oldest entry
   if the stack is full; clears the redo stack since a new edit branch
   invalidates any pending redo. */
static void pixel_hist_push_stroke(void) {
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

/* ---- Layout constants ---- */
static const float TOOLBAR_W   = 76.0f;
static const float PANEL_W     = 280.0f;
static const float PALETTE_H   = 112.0f;
static const float TIMELINE_H  = 108.0f;

/* ---- Undo system ---- */
/* EditSnapshot, UNDO_STACK_SIZE, and g_undo[]/g_undo_idx/g_undo_count moved to
   ui_internal.h / ui_state.cpp. */

/* ---- Clipboard (pixel data only) ---- */
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
    bool           has_palette;
    unsigned short palette_numc;
    unsigned char  palette_data[512];
    char           source_name[16];
    char           src_filename[16];
};
static CopiedImage g_clipboard = {false};

static void ClearPixelClipboard(void)
{
    if (g_clipboard.data_p) free(g_clipboard.data_p);
    memset(&g_clipboard, 0, sizeof(g_clipboard));
}

static bool BuildClipboardPaletteMap(const PAL *target_pal, unsigned char map[256])
{
    for (int i = 0; i < 256; i++) map[i] = (unsigned char)i;
    if (!g_clipboard.has_palette || !target_pal || !target_pal->data_p)
        return false;

    int src_n = g_clipboard.palette_numc;
    int dst_n = target_pal->numc;
    if (src_n > 256) src_n = 256;
    if (dst_n > 256) dst_n = 256;
    if (src_n <= 0 || dst_n <= 0) return false;

    const unsigned char *td = (const unsigned char *)target_pal->data_p;
    if (src_n == dst_n &&
        memcmp(g_clipboard.palette_data, td, (size_t)src_n * 2u) == 0)
        return false;

    map[0] = 0;
    for (int i = 1; i < 256; i++) {
        if (i < src_n) {
            unsigned short src_word = palette_word_at(g_clipboard.palette_data, i);
            map[i] = nearest_palette_index_for_word(src_word, target_pal);
        } else {
            map[i] = (i < dst_n) ? (unsigned char)i : 0;
        }
    }
    return true;
}

/* ---- Palette clipboard ----
   Holds a copy of a palette's color data (15-bit packed words) plus its name,
   so the user can copy a palette in one .IMG and paste it after opening
   another. Lives outside g_clipboard (pixel data) so the two clipboards don't
   stomp on each other. Storage is plain malloc, not the LIB pool, because it
   must survive a File->Open which resets the pool. */
struct CopiedPalette {
    bool           valid;
    unsigned short numc;
    unsigned char  bitspix;
    char           n_s[10];
    unsigned char *data;     /* numc * 2 bytes, malloc'd */
};
static CopiedPalette g_pal_clipboard = {false, 0, 0, {0}, NULL};

/* ---- Editor state ---- */
static int  g_sel_color   = 0;
static bool g_palette_selection[256] = {false};
static bool g_show_points = true;
static bool g_show_hitbox = false;
static bool g_show_dma_comp = false;
static int  g_hitbox_x = 0, g_hitbox_y = 0, g_hitbox_w = 32, g_hitbox_h = 32;
static int  g_hitbox_drag_corner = -1;

/* Rename dialog — handles image/palette/marked-images. The rename target
   determines the popup title, max name length, and where the new name lands
   on OK. For RenameTarget::MarkedImages, an "+" prefix means "prepend the
   typed string to the existing name"; otherwise the typed string becomes the
   base and a numeric suffix is appended (1, 2, 3 ...). */
enum class RenameTarget { Image, Palette, MarkedImages };
static bool         g_show_rename = false;
static RenameTarget g_rename_target = RenameTarget::Palette;
static int          g_rename_idx = -1;
static char         g_rename_buf[20] = {0};
static bool         g_rename_tail_existing = false;
static int          g_rename_start_number = 1;

enum class ImageListSort { Original = 0, Name, Size };
static ImageListSort g_image_list_sort = ImageListSort::Original;
static bool          g_image_list_sort_desc = false;

/* Unsaved changes confirmation */
static bool g_show_unsaved_confirm = false;
/* Deferred action that the unsaved-changes dialog should run after the user
   picks Save or Discard. One of: quit, open-file-dialog, open-specific-path. */
enum class PendingAction { None, Quit, OpenDialog, OpenPath, OpenLodDialog, CloseTab };
static PendingAction g_pending_action      = PendingAction::None;
static std::string   g_pending_action_path; /* only used when action == OpenPath */
static int           g_pending_tab_index = -1;
static int           g_doc_tab_select_request = 0;
static bool g_pending_quit = false;
#define g_dirty (g_doc->dirty)

/* Anipoint drag state, hoisted to file scope so the pencil branch can
   gate on it (the anipoint render block runs *after* the pencil block,
   so widget_consumed_click is too late to suppress the first paint
   frame of a drag). */
static bool g_anipoint_drag1 = false;
static bool g_anipoint_drag2 = false;
/* g_sequence_anipoint_undo_active + the sequence anipoint setters live in
   anipoint_edit.{h,cpp}. */

/* mark_dirty() and the InvalidatePaletteUsage() declaration now live in
   ui_internal.h (shared service). The g_dirty macro above stays for overlay
   reads/clears; InvalidatePaletteUsage's definition stays below. */

/* Two-column label/value renderer for the Properties panel. The value column
   is positioned by ImGui::SameLine(col_x) instead of by padding the label
   with spaces, so re-labeling a row doesn't break alignment. col_x is the
   pixel offset within the current window — 90 px lines up roughly with the
   pre-existing 13-character indent at the default font. */
static void LabeledValue(const char *label, const char *fmt, ...)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

static bool AnimPointSliderInt(const char *label, int *value, int min_value, int max_value)
{
    ImGui::SetNextItemWidth(-1);
    bool changed = ImGui::DragInt(label, value, 1.0f, min_value, max_value);
    bool selected = ImGui::IsItemActive() || ImGui::IsItemFocused();
    ImGui::SetItemKeyOwner(ImGuiKey_LeftArrow);
    ImGui::SetItemKeyOwner(ImGuiKey_RightArrow);

    ImGuiIO &io = ImGui::GetIO();
    if (selected && !changed && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
        int delta = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow, true))  delta--;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow, true)) delta++;
        if (delta != 0) {
            int next = *value + delta;
            if (next < min_value) next = min_value;
            if (next > max_value) next = max_value;
            if (next != *value) {
                *value = next;
                changed = true;
            }
        }
    }
    return changed;
}

/* New IMG / Add Palette confirmations */
static bool g_show_new_img_confirm = false;
/* New-image dialog state. Width/height persist between opens so the user
   doesn't have to retype after a stream of additions at the same size. */
static bool g_show_new_blank_dialog = false;
static int  g_new_blank_w = 32;
static int  g_new_blank_h = 32;

/* Keyboard navigation focus: up/down arrows navigate palettes when true */
static bool g_palette_nav = false;

/* Deferred sprite delete confirmation. When a parent sprite has inferred
   subframes, the delete path pauses here so the user can choose parent-only
   or parent+children without losing the normal one-key delete workflow. */
static bool g_show_delete_images_confirm = false;
static std::vector<int> g_pending_delete_base_indices;
static std::vector<int> g_pending_delete_subframe_indices;
static char g_pending_delete_parent_name[16] = {0};

/* Hue shift slider state (reset on palette change) */
static int g_hue_slider = 0;
static int g_hue_last = 0;
/* Saturation: -100..+100 maps to a multiplicative scale (e.g. -100 = grayscale,
   +100 = saturation doubled and clamped). Lightness: -100..+100 maps to an
   additive shift in HSL L space (e.g. +50 lifts everything halfway to white). */
static int g_sat_slider = 0;
static int g_sat_last = 0;
static int g_light_slider = 0;
static int g_light_last = 0;
static unsigned char g_palette_baseline[512];
static int g_palette_baseline_nc = 0;
static bool g_palette_drag_undo_active = false;
static unsigned int g_palette_sync_serial = 1;

static void InvalidatePaletteSync(void)
{
    g_palette_sync_serial++;
    if (g_palette_sync_serial == 0) g_palette_sync_serial = 1;
}

/* Grid selection tool (for copy/paste) */
struct GridSelection {
    bool active;        /* a selection rectangle exists and should be drawn */
    bool dragging;      /* user is currently click-dragging the rect's far corner */
    int x1, y1;         /* start coords (pixels) */
    int x2, y2;         /* end coords (pixels) */

    bool is_mask;       /* if true, pixel_mask is used instead of just the bounding box */
    int mask_w, mask_h; /* dimensions of the mask */
    std::vector<bool> pixel_mask; /* the actual selected pixels */
};
static GridSelection g_grid_sel = {false, false, 0, 0, 0, 0, false, 0, 0, {}};
static bool g_selection_add_drag = false;
static int  g_selection_add_mask_w = 0;
static int  g_selection_add_mask_h = 0;
static std::vector<bool> g_selection_add_mask;

/* Active tool state */
enum class ActiveTool { None, Pencil, PaintBucket, VariantPaint, Marquee, MagicWand, BackgroundEraser, CloneStamp, SmartRemap, Lasso, Eyedropper };
static ActiveTool g_active_tool = ActiveTool::None;

/* Clone Stamp state */
static bool g_clone_source_set = false;
static int  g_clone_src_x = 0;
static int  g_clone_src_y = 0;
static bool g_clone_offset_set = false;
static int  g_clone_dx = 0;
static int  g_clone_dy = 0;

/* Smart Remap state */
static int g_remap_target_color = -1;
static int g_remap_tolerance = 0;       /* 0..16, palette-index distance */
static int  g_bucket_tolerance = 0;     /* 0..16, palette-index distance */
static bool g_bucket_contiguous = true; /* true = flood region; false = replace all matching pixels */

/* Color isolation view: when set to a palette index >=0, the canvas dims
   every pixel that isn't that index so the user can see exactly where a
   particular color lives in the sprite. -1 = isolation off. */
static int g_isolate_color = -1;

/* Timeline thumbnail cache (TimelineThumb, g_thumb_cache, EnsureThumb,
   InvalidateThumb, ClearTimelineThumbCache) now lives in ui_timeline.{h,cpp}. */

/* Timeline state (lifted to file scope so keyboard shortcuts and onion-skin
   can address it). g_timeline_built_for_imgcnt drives stale-index pruning. */
/* g_is_playing lives in ui_timeline.{h,cpp}. */
static float            g_play_speed = 12.0f; /* fps */
static float            g_play_timer = 0.0f;
/* g_timeline_frames / g_timeline_holds / g_timeline_play_idx and the frame-model
   operations now live in ui_timeline.{h,cpp}. */
static unsigned int     g_timeline_built_for_imgcnt = 0;
static bool             g_timeline_onion = false;  /* prev/next frame ghosting */
static bool             g_timeline_pingpong = false; /* play forward then reverse, looping */
static int              g_timeline_play_dir = 1;     /* +1 forward, -1 reverse (used when pingpong) */
/* g_timeline_composite[2] / _locked[2] / _drag_slot live in ui_timeline.{h,cpp}
   along with the selection + playback logic. The drag mouse/anipoint state below
   stays here with the composite-preview rendering. */
/* Composite-preview drag state lives in ui_timeline.cpp with the preview. */

/* Composite selection helpers (ClearTimelineCompositeSelection,
   CompactTimelineCompositeSelection, PruneTimelineCompositeSelection,
   TimelineCompositeSlot, TimelineCompositeReady, TimelineAnyCompositeLocked)
   now live in ui_timeline.cpp. */

/* DrawTimelineCompositeLockToggle now lives in ui_timeline.cpp.

   Timeline frame-model operations (TimelineFramePosition, WrapTimelinePosition,
   ClampTimelineHold, EnsureTimelineHolds, TimelinePushFrame, TimelineSetFrames,
   TimelineClearFrames, TimelineHoldAt, TimelineSetHoldAt, TimelineSwapFrames,
   TimelineMoveFrame) now live in ui_timeline.cpp. */

/* AdvanceTimelineComposite, StepTimelinePlayhead, and ToggleTimelineCompositeFrame
   now live in ui_timeline.cpp. */

void imgtool_toggle_timeline_play(void)
{
    if (g_timeline_frames.empty()) return;
    g_is_playing = !g_is_playing;
    if (g_is_playing) {
        if (TimelineCompositeReady()) {
            int p0 = TimelineFramePosition(g_timeline_composite[0]);
            if (p0 >= 0) g_timeline_play_idx = p0;
        }
        g_play_timer = 0.0f;
        g_doc->ilselected = g_timeline_frames[g_timeline_play_idx];
        g_img_tex_idx = -2;
    }
}

/* Magic Wand state */
static int  g_wand_tolerance = 0;       /* 0..64, palette-index distance */
static bool g_wand_contiguous = true;   /* false = select all matching pixels globally */

/* Background Eraser ("Smart Eraser") state */
static int  g_eraser_tolerance = 0;     /* 0..16, palette-index distance from clicked chroma */
static bool g_eraser_contiguous = true; /* true = flood from click; false = global by color */
static bool g_eraser_defringe = true;   /* if true, also nudges edge pixels next to erased area */

/* Lasso state — freehand polygon being drawn this stroke */
static std::vector<std::pair<int,int>> g_lasso_points;

/* Clone Stamp brush */
static int g_clone_brush = 1;           /* radius+1; 1 = single pixel, 3/5/7 = round brushes */
static int g_pencil_brush = 1;          /* same convention as g_clone_brush — 1 = single pixel */
static int g_variant_brush = 1;         /* same convention as pencil brush */

/* Snap-to-content cached bbox (recomputed when Shift goes down on a paste drag) */
struct SnapBBox {
    bool valid;
    int min_x, min_y, max_x, max_y;
    int img_idx;        /* which image the bbox was computed from */
};
static SnapBBox g_snap_bbox = {false, 0,0,0,0, -1};
static bool g_snap_hit_x = false;       /* set during drag when an X snap fires; for guide draw */
static bool g_snap_hit_y = false;
static int  g_snap_guide_x = 0;
static int  g_snap_guide_y = 0;

/* Pasted image placement (with move feedback) */
struct PastedImage {
    bool active;        /* paste is active and can be moved */
    int paste_x, paste_y;  /* top-left corner where paste will go */
    bool dragging;      /* user is dragging the paste boundary */
    float drag_start_mx, drag_start_my;
    int drag_start_px, drag_start_py;
};
static PastedImage g_pasted = {false};

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
static PasteBlendMode g_paste_blend_mode = PasteBlendMode::Normal;
static int            g_paste_opacity = 100;

/* Free Transform (Ctrl+T) state. Engaged only while a paste is floating.
   The eight handles re-scale the floating clipboard; the user confirms with
   Enter/Ctrl+T/click-outside, or cancels with Esc (reverts to pre-transform
   size). Aspect ratio is locked by default; the chain icon toggles the lock
   for the rest of the session, Shift inverts it temporarily for one drag. */
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
static FreeTransform g_xform = {false, true, 0,0,0,0, 0,0,0,0, 0.0f,0.0f, TransformHandle::None, 0,0, 0,0,0,0, 0.0f, 1.0f};

static void ApplyPalette(int pal_idx);
static void save_palette_baseline(void);
static void reset_palette_adjust_sliders(void);
static void commit_palette_adjustments(void);
void undo_push(void);
static void xform_begin(void);  /* forward decl — used by paste_image */
static int  FindDirtyDocumentIndex(void);
static bool HasDirtyDocuments(void);
static int  BuildMergeAdditions(int target_idx, int target_base_numc, bool grow,
                                unsigned short added[256], int *overflow_out);
static void MergeMarkedPalettes(bool force_quality_merge = false);
static bool LoadAsmAnimations(const char *path);
static bool LoadAsmOpponent(const char *path);
static void AsmAnimSelect(int i);
static bool clipboard_secondary_anipoint_in_use(void);
static void MakeDerivedImageName(const char *base, const char *suffix, char out[16]);

struct DocSnapshot {
    unsigned int seq;
    Document     doc;
    int          hitbox_x, hitbox_y, hitbox_w, hitbox_h;
    int          sel_color;
    bool         palette_selection[256];
};

static std::vector<DocSnapshot *> g_doc_hist;
static std::vector<DocSnapshot *> g_doc_redo;
static const size_t kDocHistMax = 16;

static size_t ImgPixelBytes(unsigned short w, unsigned short h)
{
    return (size_t)(((unsigned int)w + 3u) & ~3u) * (size_t)h;
}

static bool CloneBytes(const void *src, size_t n, void **out)
{
    if (!out) return false;
    *out = NULL;
    if (!src || n == 0) return true;
    void *dst = malloc(n);
    if (!dst) return false;
    memcpy(dst, src, n);
    *out = dst;
    return true;
}

static void FreeImgChainForSnapshot(void *head)
{
    IMG *img = (IMG *)head;
    while (img) {
        IMG *next = (IMG *)img->nxt_p;
        FreeImg(img);
        img = next;
    }
}

static void FreePalChainForSnapshot(void *head)
{
    PAL *pal = (PAL *)head;
    while (pal) {
        PAL *next = (PAL *)pal->nxt_p;
        FreePal(pal);
        pal = next;
    }
}

static bool CloneImgChainForSnapshot(const IMG *src, void **out)
{
    if (!out) return false;
    *out = NULL;
    IMG *head = NULL;
    IMG *tail = NULL;

    while (src) {
        IMG *dst = (IMG *)calloc(1, sizeof(IMG));
        if (!dst) {
            FreeImgChainForSnapshot(head);
            return false;
        }

        *dst = *src;
        dst->nxt_p = NULL;
        dst->data_p = NULL;
        dst->pttbl_p = NULL;
        dst->baseline_p = NULL;
        dst->temp = NULL;
        dst->layer_p = NULL;

        if (!CloneBytes(src->data_p, ImgPixelBytes(src->w, src->h), &dst->data_p) ||
            !CloneBytes(src->pttbl_p, 40, &dst->pttbl_p) ||
            !CloneBytes(src->layer_p, LayerBlockBytesFromHeader(src->layer_p),
                        &dst->layer_p)) {
            FreeImg(dst);
            FreeImgChainForSnapshot(head);
            return false;
        }

        unsigned short bw = src->baseline_w ? src->baseline_w : src->w;
        unsigned short bh = src->baseline_h ? src->baseline_h : src->h;
        if (!CloneBytes(src->baseline_p, ImgPixelBytes(bw, bh), &dst->baseline_p)) {
            FreeImg(dst);
            FreeImgChainForSnapshot(head);
            return false;
        }

        if (tail) tail->nxt_p = dst;
        else head = dst;
        tail = dst;
        src = (const IMG *)src->nxt_p;
    }

    *out = head;
    return true;
}

static bool ClonePalChainForSnapshot(const PAL *src, void **out)
{
    if (!out) return false;
    *out = NULL;
    PAL *head = NULL;
    PAL *tail = NULL;

    while (src) {
        PAL *dst = (PAL *)calloc(1, sizeof(PAL));
        if (!dst) {
            FreePalChainForSnapshot(head);
            return false;
        }

        *dst = *src;
        dst->nxt_p = NULL;
        dst->data_p = NULL;
        dst->temp = NULL;

        if (!CloneBytes(src->data_p, (size_t)src->numc * 2u, &dst->data_p)) {
            FreePal(dst);
            FreePalChainForSnapshot(head);
            return false;
        }

        if (tail) tail->nxt_p = dst;
        else head = dst;
        tail = dst;
        src = (const PAL *)src->nxt_p;
    }

    *out = head;
    return true;
}

static void FreeDocSnapshot(DocSnapshot *snap)
{
    if (!snap) return;
    FreeImgChainForSnapshot(snap->doc.img_p);
    FreeImgChainForSnapshot(snap->doc.img2_p);
    FreePalChainForSnapshot(snap->doc.pal_p);
    free(snap->doc.scrseqmem_p);
    free(snap);
}

static DocSnapshot *CaptureDocSnapshot(unsigned int seq)
{
    if (!g_doc) return NULL;
    DocSnapshot *snap = (DocSnapshot *)calloc(1, sizeof(DocSnapshot));
    if (!snap) return NULL;

    snap->seq = seq;
    snap->doc = *g_doc;
    snap->doc.img_p = NULL;
    snap->doc.img2_p = NULL;
    snap->doc.pal_p = NULL;
    snap->doc.scrseqmem_p = NULL;

    if (!CloneImgChainForSnapshot((const IMG *)g_doc->img_p, &snap->doc.img_p) ||
        !CloneImgChainForSnapshot((const IMG *)g_doc->img2_p, &snap->doc.img2_p) ||
        !ClonePalChainForSnapshot((const PAL *)g_doc->pal_p, &snap->doc.pal_p) ||
        !CloneBytes(g_doc->scrseqmem_p, g_doc->scrseqbytes, &snap->doc.scrseqmem_p)) {
        FreeDocSnapshot(snap);
        return NULL;
    }

    snap->hitbox_x = g_hitbox_x;
    snap->hitbox_y = g_hitbox_y;
    snap->hitbox_w = g_hitbox_w;
    snap->hitbox_h = g_hitbox_h;
    snap->sel_color = g_sel_color;
    memcpy(snap->palette_selection, g_palette_selection,
           sizeof(snap->palette_selection));
    return snap;
}

static void ClearDocumentRedoStack(void)
{
    for (DocSnapshot *snap : g_doc_redo)
        FreeDocSnapshot(snap);
    g_doc_redo.clear();
}

static void ClearDocumentHistoryStacks(void)
{
    for (DocSnapshot *snap : g_doc_hist)
        FreeDocSnapshot(snap);
    g_doc_hist.clear();
    ClearDocumentRedoStack();
}

static bool RestoreDocSnapshot(DocSnapshot *snap)
{
    if (!snap || !g_doc) return false;

    FreeImgChainForSnapshot(g_doc->img_p);
    FreeImgChainForSnapshot(g_doc->img2_p);
    FreePalChainForSnapshot(g_doc->pal_p);
    free(g_doc->scrseqmem_p);

    Document restored = snap->doc;
    snap->doc.img_p = NULL;
    snap->doc.img2_p = NULL;
    snap->doc.pal_p = NULL;
    snap->doc.scrseqmem_p = NULL;
    *g_doc = restored;

    if (g_doc->ilselected >= (int)g_doc->imgcnt)
        g_doc->ilselected = g_doc->imgcnt ? (int)g_doc->imgcnt - 1 : -1;
    if (g_doc->plselected >= (int)g_doc->palcnt)
        g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;

    g_hitbox_x = snap->hitbox_x;
    g_hitbox_y = snap->hitbox_y;
    g_hitbox_w = snap->hitbox_w;
    g_hitbox_h = snap->hitbox_h;
    g_sel_color = snap->sel_color;
    if (g_sel_color < 0) g_sel_color = 0;
    if (g_sel_color > 255) g_sel_color = 255;
    memcpy(g_palette_selection, snap->palette_selection,
           sizeof(g_palette_selection));

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    ClearTimelineThumbCache();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_palette_nav = false;
    InvalidatePaletteSync();
    mark_dirty();
    return true;
}

bool doc_undo_push(void)   /* non-static: also used by img_io.cpp crop ops */
{
    DocSnapshot *snap = CaptureDocSnapshot(++g_undo_seq);
    mark_dirty();
    if (!snap) return false;

    if (g_doc_hist.size() >= kDocHistMax) {
        FreeDocSnapshot(g_doc_hist.front());
        g_doc_hist.erase(g_doc_hist.begin());
    }
    g_doc_hist.push_back(snap);
    ClearDocumentRedoStack();
    for (auto &r : g_pixel_redo) pixel_hist_free(&r);
    g_pixel_redo.clear();
    if (g_undo_idx < g_undo_count - 1)
        g_undo_count = g_undo_idx + 1;
    return true;
}

/* Palette downscale preview modal. Reduces the selected palette to a bpp
   color budget (index 0 included) and remaps sprites that use it on OK. */
static bool         g_show_palette_reduce = false;
static int          g_palette_reduce_bpp = 7;
static int          g_palette_reduce_preview_idx = -1;
static SDL_Texture *g_palette_reduce_orig_tex = NULL;
static SDL_Texture *g_palette_reduce_new_tex = NULL;
static int          g_palette_reduce_tex_w = 0;
static int          g_palette_reduce_tex_h = 0;
static int          g_palette_reduce_tex_img = -1;
static int          g_palette_reduce_tex_pal = -1;
static int          g_palette_reduce_tex_bpp = 0;

struct PaletteMergeQuality {
    int target_idx;
    char target_name[16];
    int source_palettes;
    int remapped_images;
    int affected_pixels;
    int exact_pixels;
    int color_drift_pixels;
    int transparent_drift_pixels;
    int invalid_pixels;
    int ppp_warning_images;
    long long total_dist;
    int max_dist;
    int max_src_slot;
    int max_dst_slot;
    char max_palette[16];
    char max_image[16];
    /* Merge-mode options the quality was computed under, plus the resulting
       grow plan (colors appended to the target's free slots).  Stored here so
       the preview and the commit use the exact same plan the dialog showed. */
    bool opt_grow;
    bool opt_perceptual;
    int target_base_numc;          /* target->numc before any growth */
    int colors_added;              /* distinct used source colors appended */
    int colors_overflow;           /* wanted to add but no free slots left */
    unsigned short added_words[256];
};
static bool g_show_palette_merge_quality = false;
static PaletteMergeQuality g_palette_merge_quality = {};
static bool g_palette_merge_preview_only = false;
/* Merge-mode toggles, persisted across invocations. Grow defaults on so the
   merge is lossless whenever the target has free slots. */
static bool g_merge_opt_grow = true;
static bool g_merge_opt_perceptual = false;

static void ClearPaletteReducePreviewTextures(void)
{
    if (g_palette_reduce_orig_tex) {
        SDL_DestroyTexture(g_palette_reduce_orig_tex);
        g_palette_reduce_orig_tex = NULL;
    }
    if (g_palette_reduce_new_tex) {
        SDL_DestroyTexture(g_palette_reduce_new_tex);
        g_palette_reduce_new_tex = NULL;
    }
    g_palette_reduce_tex_w = 0;
    g_palette_reduce_tex_h = 0;
    g_palette_reduce_tex_img = -1;
    g_palette_reduce_tex_pal = -1;
    g_palette_reduce_tex_bpp = 0;
}

static void ClearPixelHistoryStacks(void)
{
    for (auto &e : g_pixel_hist) pixel_hist_free(&e);
    for (auto &e : g_pixel_redo) pixel_hist_free(&e);
    g_pixel_hist.clear();
    g_pixel_redo.clear();
}

static void ResetPerDocumentUiState(bool clear_pixel_clipboard = false)
{
    g_undo_count = 0;
    g_undo_idx = -1;
    g_undo_seq = 0;
    ClearPixelHistoryStacks();
    ClearDocumentHistoryStacks();
    ClearTimelineThumbCache();
    TimelineClearFrames();
    g_timeline_built_for_imgcnt = 0;
    g_is_playing = false;
    g_play_timer = 0.0f;
    g_timeline_play_dir = 1;
    ClearTimelineCompositeSelection();

    if (clear_pixel_clipboard) ClearPixelClipboard();
    g_grid_sel.active = false;
    g_grid_sel.dragging = false;
    g_lasso_points.clear();
    g_active_tool = ActiveTool::None;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    g_xform.handle = TransformHandle::None;
    g_palette_nav = false;
    g_clone_source_set = false;
    g_clone_offset_set = false;
    g_remap_target_color = -1;
    g_show_palette_reduce = false;
    g_palette_reduce_preview_idx = -1;
    ClearPaletteReducePreviewTextures();
    g_snap_bbox.valid = false;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    g_palette_baseline_nc = 0;
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    InvalidatePaletteSync();
}

/* ---- Histogram state ---- */
static bool  g_show_histogram = false;
static bool  g_show_load2_verify = false;
static L2Report g_load2_report;

/* ---- Selected palette usage state ----
   Live cache for the bottom swatch strip. Counts visible sprite pixels only
   (w*h), not DMA stride padding, across images using g_doc->plselected. */
static unsigned long long g_palette_usage_counts[256] = {0};
static unsigned int       g_palette_usage_serial = 1;
static unsigned int       g_palette_usage_built_serial = 0;
static Document          *g_palette_usage_doc = NULL;
static void              *g_palette_usage_img_head = NULL;
static unsigned int       g_palette_usage_imgcnt_seen = 0;
static int                g_palette_usage_pal_idx = -2;
static int                g_palette_usage_pal_numc = 0;
static int                g_palette_usage_img_count = 0;
static int                g_palette_usage_used_colors = 0;   /* excludes #0 */
static int                g_palette_usage_unused_colors = 0; /* excludes #0 */
static int                g_palette_usage_low_colors = 0;    /* excludes #0 */
static int                g_palette_usage_low_threshold = 8;

/* ---- MK2 strike-table editor state ---- */
static bool          g_show_mk2 = false;
static mk2::Document g_mk2_doc;
static char          g_mk2_path[1024] = "";   /* user picks via Browse or types directly */
static std::string   g_mk2_status;        /* last load/save message */
static bool          g_mk2_status_sticky = false; /* errors stay until next action; success messages clear on edit */
static int           g_mk2_char_idx = 0;  /* selected char-table index */
static int           g_mk2_move_idx = 0;  /* selected move index within that table */
static int           g_mk2_drag_corner = -1; /* canvas overlay corner drag (0..3) */
static char          g_mk2_search[64] = ""; /* filter for the move list */

/* ---- MK2 fatality lab state ---- */
static bool                 g_show_mk2_fatality = false;
static mk2fatal::Document   g_mk2_fatality_doc;
static char                 g_mk2_fatality_root[1024] = "..\\mk2-main";
static std::string          g_mk2_fatality_status;
static bool                 g_mk2_fatality_status_sticky = false;
static int                  g_mk2_fatality_command_idx = 0;
static int                  g_mk2_fatality_combo_idx = 0;
static int                  g_mk2_fatality_anim_idx = 0;
static int                  g_mk2_fatality_selected_line = 0;
static char                 g_mk2_fatality_filter[96] = "";
static char                 g_mk2_fatality_insert_anim[256] = "\t.long\t0";
static char                 g_mk2_fatality_insert_combo[256] = "\t.word\tsw_right";
static bool                 g_mk2_fatality_body_only = false;
static mk2fatal::AssetPlan  g_mk2_fatality_plan;
static std::string          g_mk2_fatality_stage_status;
static int                  g_mk2_fatality_fighter_idx = 0;
static int                  g_mk2_fatality_selected_fatality = 0;
static int                  g_mk2_fatality_attacker_anim_idx = 0;
static int                  g_mk2_fatality_victim_anim_idx = 0;
static float                g_mk2_fatality_preview_fps = 8.0f;

/* Resolve the current MK2 record index, or -1 if no valid selection. */
static int Mk2CurrentRecord(void) {
    if (g_mk2_char_idx < 0 || g_mk2_char_idx >= (int)g_mk2_doc.char_tables.size()) return -1;
    const auto &moves = g_mk2_doc.char_tables[g_mk2_char_idx].moves;
    if (g_mk2_move_idx < 0 || g_mk2_move_idx >= (int)moves.size()) return -1;
    return mk2::find_record(&g_mk2_doc, moves[g_mk2_move_idx].c_str());
}
/* Heuristically map an IMG filename to an MK2 character code based on
   the data/ filename prefixes seen in the mk2-main tree. Returns NULL
   when the filename doesn't match a known fighter; callers should leave
   the panel selection alone in that case rather than guessing. */
static const char *Mk2CharFromImgName(const char *fname)
{
    if (!fname || !fname[0]) return NULL;
    /* Uppercase copy of just the basename (strip path + extension). */
    char up[64];
    const char *p = fname;
    /* Strip any preceding directory components — backslash and slash. */
    for (const char *s = fname; *s; s++)
        if (*s == '\\' || *s == '/') p = s + 1;
    size_t n = 0;
    while (p[n] && p[n] != '.' && n < sizeof(up) - 1) {
        up[n] = (char)toupper((unsigned char)p[n]);
        n++;
    }
    up[n] = '\0';
    if (n == 0) return NULL;

    /* Prefix match table — most specific first. Codes match the two-
       letter labels in MKSTK.ASM's character tables (e.g. lk_strikes,
       jc_strikes); no character names are encoded here. */
    struct { const char *prefix; const char *code; } map[] = {
        { "CAGE",     "jc" },
        { "HATHED",   "hh" },
        { "KANG",     "lk" },
        { "NINJAS",   "nj" },
        { "RAID",     "rd" },
        { "TSUNG",    "st" },
        { "JAXPRO",   "jx" },
        { "NUJAX",    "jx" },
        { "MKJXARMS", "jx" },
        { "KAT",      "fn" },
        { "BIGGORO",  "go" },
        { "GOROSIZE", "go" },
    };
    for (const auto &m : map)
        if (strncmp(up, m.prefix, strlen(m.prefix)) == 0) return m.code;
    return NULL;
}

/* Best-effort: when an IMG finishes loading, if its filename maps to a
   known MK2 fighter and the MK2 doc has been loaded, jump the panel to
   that character. Silent no-op when nothing matches. */
static void Mk2AutoSelectFromImg(void)
{
    if (g_mk2_doc.char_tables.empty()) return;
    const char *code = Mk2CharFromImgName(g_doc->fname_s);
    if (!code) return;
    int ci = mk2::find_char_table(&g_mk2_doc, code);
    if (ci < 0) return;
    g_mk2_char_idx = ci;
    g_mk2_move_idx = 0;
}

/* Move the char/move selection to whichever character table contains
   the given record index, so the user sees the result of an undo/redo. */
static void Mk2SelectRecord(int rec_idx) {
    if (rec_idx < 0 || rec_idx >= (int)g_mk2_doc.records.size()) return;
    const std::string &lbl = g_mk2_doc.records[rec_idx].label;
    for (int ci = 0; ci < (int)g_mk2_doc.char_tables.size(); ci++) {
        const auto &mv = g_mk2_doc.char_tables[ci].moves;
        for (int mi = 0; mi < (int)mv.size(); mi++) {
            if (mv[mi] == lbl) {
                g_mk2_char_idx = ci;
                g_mk2_move_idx = mi;
                return;
            }
        }
    }
}

/* ---- World View mode (DOS-tool-style anipoint alignment workspace) ----
 * When on, renders the sprite inside a fixed black canvas at
 * (world_origin - anix, world_origin - aniy). Left-drag the sprite
 * to update its anipoint (drag direction is "sprite follows cursor"
 * which means anix/aniy DECREASE as you drag right/down). Use up/down
 * arrows to flick through frames — origin stays put so you can
 * eyeball whether anipoints line up across frames. */
static WorldViewState &g_world_state = WorldView();
/* Marked World View slot constants now live in ui_canvas.h. */
static WorldMarkedSequenceState &g_world_marked_state = WorldMarkedState();
/* g_world_temp_textures + ClearWorldTempTextures + BuildWorldSpriteTexture +
   doc_get_pal now live in world_render.{h,cpp}. */
static int   g_load2_selected_idx = -1;          /* index into g_load2_report.issues */
static SDL_Texture *g_load2_drift_tex = NULL;
static int   g_load2_drift_tex_w = 0, g_load2_drift_tex_h = 0;

/* ---- ASM animation viewer ----
   Parses a MK2 per-character ASM (e.g. MKRD.ASM) and lets the user inspect /
   play the animations it defines against the currently loaded IMG. An anim
   like a_rdstance is a list of frame-GROUP labels; each group lists sprite
   piece symbols (RNSTANCE1A,...,0) that resolve to IMG frames by name. */
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
static std::vector<AsmAnim> g_asm_anims;
static int          g_asm_anim_sel = -1;
static std::string  g_asm_anim_file;
static bool         g_show_asm_anim = false;
static bool         g_asm_anim_play = true;
static float        g_asm_anim_fps = 12.0f;
static float        g_asm_anim_timer = 0.0f;
static int          g_asm_anim_frame = 0;
static SDL_Texture *g_asm_anim_tex = NULL;
static int          g_asm_anim_tex_w = 0, g_asm_anim_tex_h = 0;
static int          g_asm_anim_canvas_w = 0, g_asm_anim_canvas_h = 0;
static int          g_asm_anim_minx = 0, g_asm_anim_miny = 0; /* anipoint-anchored bbox origin */
static int          g_asm_anim_last_drawn = -1;
static Document    *g_asm_anim_doc = NULL;      /* doc the anim's frames resolved against */
static int          g_asm_anim_doc_idx = -1;
static bool         g_asm_lane_enabled = false; /* show the selected anim as a World View lane */
static bool         g_request_save_world_asm = false; /* deferred: open Save ASM dialog */
static bool         g_request_load_asm = false;       /* deferred: open Load ASM dialog */
/* Fatality opponent: a second ASM instance drawn in the opponent lane. */
static std::vector<AsmAnim> g_asm_opp_anims;
static int          g_asm_opp_sel = -1;
static std::string  g_asm_opp_file;
static Document    *g_asm_opp_doc = NULL;
static int          g_asm_opp_doc_idx = -1;
static bool         g_asm_opp_enabled = false;
static bool         g_request_load_opp_asm = false;   /* deferred: open opponent ASM dialog */
static bool         g_asm_dialog_opponent = false;    /* LoadAsmAnim dialog targets opponent */
static bool         g_request_locate_img = false;     /* deferred: prompt for the ASM's IMG */
static bool         g_openimg_for_asm = false;        /* next OpenImg re-resolves the ASM viewer */
static bool         g_request_locate_opp_img = false; /* deferred: prompt for the opponent IMG */
static bool         g_openimg_for_opp = false;        /* next OpenImg re-resolves the opponent */
static bool         g_request_asm_autoload = false;   /* deferred: open IMGs for the player anim */
static bool         g_request_asm_opp_autoload = false; /* deferred: open IMGs for opponent anim */
static void AsmResolveAnimAgainstDoc(AsmAnim &a, Document *doc);

/* WorldMarkedRestart and StepWorldMarkedSequence now live in ui_canvas.{h,cpp}. */

/* World marked clamp and tick helpers now live in ui_canvas.{h,cpp}. */

/* World decap-name parsing now lives in ui_canvas.{h,cpp}. */
/* World dummy-decap order/reset helpers now live in ui_canvas.{h,cpp}. */

/* World marked ASM string helpers now live in ui_canvas.{h,cpp}. */

/* doc_get_img now lives in world_render.{h,cpp}. */

/* img_name_string now lives in img_util.{h,cpp}. */

/* WorldMarkedBuildSingleFrameLane now lives in ui_canvas.{h,cpp}. */

/* WorldMarkedClearSequenceState and WorldMarkedSyncSequenceOverride now live
   in ui_canvas.{h,cpp}. */

/* World marked sequence edit helpers now live in ui_canvas.{h,cpp}. */

static std::string regex_escape(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (char ch : s) {
        switch (ch) {
            case '\\': case '.': case '^': case '$': case '|':
            case '(': case ')': case '[': case ']': case '{':
            case '}': case '*': case '+': case '?':
                out.push_back('\\');
                break;
            default:
                break;
        }
        out.push_back(ch);
    }
    return out;
}

static std::string sprite_family_key(const std::string &name)
{
    return (name.size() > 2) ? name.substr(2) : name;
}

static std::string sprite_family_regex_pattern(const std::string &name)
{
    if (name.size() > 2)
        return std::string("^..") + regex_escape(name.substr(2)) + "$";
    return std::string("^") + regex_escape(name) + "$";
}

static int PushAnipointsToMatchingOpenTabs(const IMG *src, int *matched_count, int *doc_count, std::string *pattern_out)
{
    if (matched_count) *matched_count = 0;
    if (doc_count) *doc_count = 0;
    if (pattern_out) pattern_out->clear();
    if (!src) return 0;

    std::string src_name = img_name_string(src);
    if (src_name.empty()) return 0;

    std::string pattern = sprite_family_regex_pattern(src_name);
    if (pattern_out) *pattern_out = pattern;

    std::regex name_re(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
    int changed = 0;
    int matched = 0;
    int docs_changed = 0;

    for (int tab = 0; tab < document_tab_count(); tab++) {
        Document *doc = document_get(tab);
        if (!doc) continue;

        bool doc_touched = false;
        for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p) {
            std::string name = img_name_string(img);
            if (name.empty() || !std::regex_match(name, name_re)) continue;

            matched++;
            if (img == src) continue;
            if (img->anix  == src->anix  && img->aniy  == src->aniy &&
                img->anix2 == src->anix2 && img->aniy2 == src->aniy2 &&
                img->aniz2 == src->aniz2)
                continue;

            img->anix  = src->anix;
            img->aniy  = src->aniy;
            img->anix2 = src->anix2;
            img->aniy2 = src->aniy2;
            img->aniz2 = src->aniz2;
            changed++;
            doc_touched = true;
        }

        if (doc_touched) {
            doc->dirty = true;
            docs_changed++;
        }
    }

    if (matched_count) *matched_count = matched;
    if (doc_count) *doc_count = docs_changed;
    return changed;
}

static int CountMarkedImages(void)
{
    int count = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p)
        if (img->flags & 1) count++;
    return count;
}

/* InferSubframeParentName, trim_sprite_name, ascii_iequals, and
   strip_trailing_sequence_digits now live in anipoint.{h,cpp}. */

/* anipoint_sequence_parent_name, same_anipoint_sequence,
   begin_sequence_anipoint_edit, finish_sequence_anipoint_edit_if_idle,
   apply_anipoint_delta_to_sequence, and set_primary/secondary_anipoint_with_sequence
   now live in anipoint_edit.{h,cpp}. */

static void MirrorMarkedAnipointsToReverseWithToast(void)
{
    int marked = CountMarkedImages();
    int changed = MirrorMarkedAnipointsToReverse();
    if (changed > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mirrored anipoints on %d marked sprite%s.",
                 changed, changed == 1 ? "" : "s");
    } else if (marked > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No marked anipoints moved; X values are centered.");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark sprites first, then mirror anipoints.");
    }
    g_restore_msg_timer = 4.0f;
}

/* doc_get_pal now lives in world_render.{h,cpp}. */

/* WorldCollectMarkedFrames now lives in ui_canvas.{h,cpp}. */

/* BuildWorldSpriteTexture now lives in world_render.{h,cpp}. */

/* DrawTimelineCompositePreview now lives in ui_timeline.{h,cpp} — its last
   blockers (world_render, anipoint, anipoint_edit) are all extracted. */

static bool DrawWorldMarkedTabs(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    std::vector<WorldMarkedAsmLaneInput> asm_lanes;
    asm_lanes.reserve(2);
    auto add_asm_lane = [&](std::vector<AsmAnim> &anims, bool enabled, int sel,
                            int slot_id, Document *doc, int doc_idx) {
        if (!enabled || sel < 0 || sel >= (int)anims.size() || !doc) return;
        AsmAnim &a = anims[sel];
        if (a.frames.empty()) return;
        WorldMarkedAsmLaneInput input = {};
        input.enabled = true;
        input.slot_id = slot_id;
        input.doc = doc;
        input.doc_idx = doc_idx;
        input.name = a.name.c_str();
        input.frames.reserve(a.frames.size());
        for (const AsmAnimFrame &fr : a.frames) {
            WorldAsmLaneFrame view = {};
            view.piece_img = &fr.piece_img;
            view.piece_doc = &fr.piece_doc;
            view.dx = fr.dx;
            view.dy = fr.dy;
            view.mirror = fr.mirror;
            input.frames.push_back(view);
        }
        asm_lanes.push_back(input);
    };
    add_asm_lane(g_asm_anims, g_asm_lane_enabled, g_asm_anim_sel,
                 kWorldAsmSlot, g_asm_anim_doc, g_asm_anim_doc_idx);
    add_asm_lane(g_asm_opp_anims, g_asm_opp_enabled, g_asm_opp_sel,
                 kWorldAsmOpponentSlot, g_asm_opp_doc, g_asm_opp_doc_idx);

    IMG *selected_img = get_img(g_doc ? g_doc->ilselected : -1);
    WorldMarkedTabsResult tabs_result =
        WorldDrawMarkedTabs(g_world_marked_state, g_world_state,
                            avail, img_pos, io.DeltaTime,
                            document_active_index(), selected_img, asm_lanes);
    if (!tabs_result.drew)
        return false;

    WorldMarkedPanelAction panel_action = tabs_result.panel.header;
    if (panel_action.copied_asm) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Copied World View ASM for %d lane%s.",
                 panel_action.copied_lane_count,
                 panel_action.copied_lane_count == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    if (panel_action.request_save_asm)
        g_request_save_world_asm = true;   /* dialog opened in main loop */
    if (panel_action.request_load_asm) {
        g_show_asm_anim = true;
        g_request_load_asm = true;
    }
    if (panel_action.dummy_assigned) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Assigned dummy body to [%d] %sDECAP.",
                 g_world_marked_state.dummy_decap_doc_idx,
                 g_world_marked_state.dummy_decap_prefix.c_str());
        g_restore_msg_timer = 4.0f;
    } else if (panel_action.dummy_assign_failed) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Select a DECAP body frame/piece first.");
        g_restore_msg_timer = 4.0f;
    }
    if (tabs_result.panel.thumb_click.clicked) {
        if (tabs_result.panel.thumb_click.doc_idx != document_active_index()) {
            document_set_active(tabs_result.panel.thumb_click.doc_idx);
            ResetPerDocumentUiState(false);
            g_doc_tab_select_request = tabs_result.panel.thumb_click.doc_idx;
        }
        g_doc->ilselected = tabs_result.panel.thumb_click.img_idx;
        g_zoom_reset = true;
    }
    if (tabs_result.panel.copied_popup_asm) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Copied World View ASM.");
        g_restore_msg_timer = 4.0f;
    }
    return true;
}

/* Build a per-row drift overlay texture for the given image. Renders the
 * sprite's current pixels at full color, then tints rows where the
 * baseline-vs-current zero-shape differs in semi-transparent red so the
 * user can see exactly which scanlines will shift LOAD2's destbits. */
static void update_drift_texture(IMG *img)
{
    int baseline_w = img ? (img->baseline_w ? (int)img->baseline_w : (int)img->w) : 0;
    int baseline_h = img ? (img->baseline_h ? (int)img->baseline_h : (int)img->h) : 0;
    if (!img || !img->data_p || !img->baseline_p || img->w == 0 || img->h == 0 ||
        baseline_w != (int)img->w || baseline_h != (int)img->h) {
        if (g_load2_drift_tex) { SDL_DestroyTexture(g_load2_drift_tex); g_load2_drift_tex = NULL; }
        g_load2_drift_tex_w = g_load2_drift_tex_h = 0;
        return;
    }
    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;

    if (!g_load2_drift_tex || g_load2_drift_tex_w != w || g_load2_drift_tex_h != h) {
        if (g_load2_drift_tex) SDL_DestroyTexture(g_load2_drift_tex);
        g_load2_drift_tex = SDL_CreateTexture(g_imgui_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureBlendMode(g_load2_drift_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_load2_drift_tex, SDL_ScaleModeNearest);
        g_load2_drift_tex_w = w;
        g_load2_drift_tex_h = h;
    }
    void *pixels; int pitch;
    if (SDL_LockTexture(g_load2_drift_tex, NULL, &pixels, &pitch) != 0) return;

    const unsigned char *cur  = (const unsigned char *)img->data_p;
    const unsigned char *base = (const unsigned char *)img->baseline_p;
    Uint32 *dst = (Uint32 *)pixels;

    for (int y = 0; y < h; y++) {
        /* Compute lead/trail counts on baseline and current to decide if
         * this row drifts. Match load2_verify.cpp count_row_zeros exactly. */
        int bl = 0, bt = 0, cl = 0, ct = 0;
        const unsigned char *brow = base + y * stride;
        const unsigned char *crow = cur  + y * stride;
        while (bl < w && brow[bl] == 0) bl++;
        if (bl < w) { int x = w - 1; while (x >= bl && brow[x] == 0) { bt++; x--; } }
        while (cl < w && crow[cl] == 0) cl++;
        if (cl < w) { int x = w - 1; while (x >= cl && crow[x] == 0) { ct++; x--; } }
        bool row_drifts = (bl != cl) || (bt != ct);

        for (int x = 0; x < w; x++) {
            unsigned char ci = cur[y * stride + x];
            SDL_Color c = g_palette[ci];
            Uint32 r = c.r, g = c.g, b = c.b;
            Uint32 a = (ci == 0) ? 0x00u : 0xFFu;

            if (row_drifts) {
                /* Tint: blend toward red. Transparent pixels get a faint
                 * red rectangle so empty rows still show their drift. */
                if (a == 0) { r = 200; g = 40; b = 40; a = 90; }
                else { r = (r + 510) / 3; g = g / 3; b = b / 3; }
            }
            dst[y * (pitch / 4) + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SDL_UnlockTexture(g_load2_drift_tex);
}
static float g_histogram_data[256] = {0};
static float g_histogram_max = 0.0f;
static int   g_histogram_img_count = 0;

/* ---- Bulk Restore Regex state ---- */
static bool g_show_restore_regex = false;
static bool g_show_auto_chop = false;
enum AutoChopMode {
    AutoChopMode_BestHorizontal = 0,
    AutoChopMode_BestVertical,
    AutoChopMode_ManualGrid
};
static int  g_chop_mode = AutoChopMode_BestHorizontal;
static const int k_auto_split_min_side = 5;
static int  g_chop_w = 64;
static int  g_chop_h = 64;
static bool g_chop_trim = true;

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

static IMG *AutoChopPrimaryTarget(int *out_idx = NULL)
{
    int marked = CountMarkedImages();
    if (g_doc->ilselected >= 0) {
        IMG *selected = get_img(g_doc->ilselected);
        if (selected && (marked == 0 || (selected->flags & 1))) {
            if (out_idx) *out_idx = g_doc->ilselected;
            return selected;
        }
    }

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (marked == 0 || (img->flags & 1)) {
            if (out_idx) *out_idx = idx;
            return img;
        }
    }

    if (out_idx) *out_idx = -1;
    return NULL;
}

static void AutoChopSetThreeBandSize(void)
{
    int idx = -1;
    IMG *img = AutoChopPrimaryTarget(&idx);
    if (!img || img->w == 0 || img->h == 0) return;

    g_chop_mode = AutoChopMode_ManualGrid;
    g_chop_w = (int)img->w;
    g_chop_h = ((int)img->h + 2) / 3;
    if (g_chop_h < 1) g_chop_h = 1;

    if (idx >= 0)
        g_doc->ilselected = idx;
}

static void OpenAutoChopDialog(void)
{
    AutoChopSetThreeBandSize();
    g_show_auto_chop = true;
}

static void AutoChopPreviewClear(AutoChopPreview *p)
{
    if (!p) return;
    p->pieces.clear();
    p->target_count = 0;
    p->raw_cells = 0;
    p->empty_cells = 0;
    p->bpp = 0;
    p->src_uncomp_bits = 0;
    p->src_zcom_bits = 0;
    p->split_uncomp_bits = 0;
    p->split_zcom_bits = 0;
    p->best_split_valid = false;
    p->best_split_vertical = false;
    p->best_split_pos = 0;
}

static int AutoChopBppForImage(const IMG *img)
{
    PAL *pal = img ? get_pal((int)img->palnum) : NULL;
    if (g_load2_ppp > 0 && g_load2_ppp <= 8) {
        int bpp = g_load2_ppp;
        if (pal && pal->numc > (unsigned short)(1u << g_load2_ppp) &&
            pal->bitspix > 0 && pal->bitspix <= 8)
            bpp = (int)pal->bitspix;
        return bpp;
    }

    if (pal) {
        if (pal->bitspix > 0 && pal->bitspix <= 8) return (int)pal->bitspix;
        int colors = pal->numc > 0 ? (int)pal->numc : 1;
        int bpp = 1;
        while (bpp < 8 && (1 << bpp) < colors) bpp++;
        return bpp;
    }
    return 8;
}

static long long EstimateZcomBitsForRect(const IMG *img,
                                         int rx, int ry, int rw, int rh,
                                         int bpp)
{
    if (!img || !img->data_p || rw <= 0 || rh <= 0 || bpp <= 0) return 0;

    long long uncompressed = (long long)rw * (long long)rh * (long long)bpp;
    if (rw <= 10) return uncompressed; /* LOAD2 disables ZCOM at this width. */

    int stride = ((int)img->w + 3) & ~3;
    const unsigned char *pixels = (const unsigned char *)img->data_p;
    std::vector<int> zlc((size_t)rh);
    std::vector<int> ztc((size_t)rh);
    long long lead_left[4] = {0, 0, 0, 0};
    long long trail_left[4] = {0, 0, 0, 0};

    for (int y = 0; y < rh; y++) {
        const unsigned char *row = pixels + (ry + y) * stride + rx;
        int lead = 0;
        while (lead < rw && row[lead] == 0) lead++;

        int trail = 0;
        if (lead < rw) {
            int x = rw - 1;
            while (x >= lead && row[x] == 0) { trail++; x--; }
        }
        zlc[(size_t)y] = lead;
        ztc[(size_t)y] = trail;

        for (int k = 0; k < 4; k++) {
            int factor = 1 << k;
            int q = lead / factor; if (q > 15) q = 15;
            lead_left[k] += lead - q * factor;
            q = trail / factor; if (q > 15) q = 15;
            trail_left[k] += trail - q * factor;
        }
    }

    int lm = 0, tm = 0;
    for (int k = 1; k < 4; k++) {
        if (lead_left[k] < lead_left[lm]) lm = k;
        if (trail_left[k] < trail_left[tm]) tm = k;
    }
    int lm_mult = 1 << lm;
    int tm_mult = 1 << tm;

    long long bits = (long long)rh * 8LL; /* one packed lead/trail byte per row */
    for (int y = 0; y < rh; y++) {
        int qzl = zlc[(size_t)y] / lm_mult; if (qzl > 15) qzl = 15;
        int qzt = ztc[(size_t)y] / tm_mult; if (qzt > 15) qzt = 15;
        int visible = rw - qzl * lm_mult - qzt * tm_mult;
        if (visible < 0) visible = 0;
        bits += (long long)visible * (long long)bpp;
    }

    return bits < uncompressed ? bits : uncompressed;
}

static bool BuildAutoChopPreviewForImage(const IMG *img, AutoChopPreview *out)
{
    if (!out) return false;
    AutoChopPreviewClear(out);
    if (!img || !img->data_p || img->w == 0 || img->h == 0 ||
        g_chop_w <= 0 || g_chop_h <= 0)
        return false;

    out->target_count = 1;
    out->bpp = AutoChopBppForImage(img);
    out->src_uncomp_bits = (long long)img->w * (long long)img->h * out->bpp;
    out->src_zcom_bits = EstimateZcomBitsForRect(img, 0, 0, img->w, img->h,
                                                 out->bpp);

    int rows = ((int)img->h + g_chop_h - 1) / g_chop_h;
    int cols = ((int)img->w + g_chop_w - 1) / g_chop_w;
    int stride = ((int)img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    int piece_no = 0;

    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            int cell_x = c * g_chop_w;
            int cell_y = r * g_chop_h;
            int cell_w = g_chop_w;
            int cell_h = g_chop_h;
            if (cell_x + cell_w > (int)img->w) cell_w = (int)img->w - cell_x;
            if (cell_y + cell_h > (int)img->h) cell_h = (int)img->h - cell_y;
            out->raw_cells++;

            int min_x = cell_w, min_y = cell_h;
            int max_x = -1, max_y = -1;
            int opaque = 0;
            for (int y = 0; y < cell_h; y++) {
                for (int x = 0; x < cell_w; x++) {
                    if (src[(cell_y + y) * stride + (cell_x + x)] != 0) {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                        opaque++;
                    }
                }
            }

            if (max_x < min_x) {
                out->empty_cells++;
                continue;
            }

            if (!g_chop_trim) {
                min_x = 0; min_y = 0;
                max_x = cell_w - 1; max_y = cell_h - 1;
            }

            AutoChopPiecePreview piece = {};
            piece.cell_x = cell_x;
            piece.cell_y = cell_y;
            piece.cell_w = cell_w;
            piece.cell_h = cell_h;
            piece.out_x = cell_x + min_x;
            piece.out_y = cell_y + min_y;
            piece.out_w = max_x - min_x + 1;
            piece.out_h = max_y - min_y + 1;
            piece.piece_no = piece_no++;
            piece.opaque_pixels = opaque;
            piece.uncomp_bits = (long long)piece.out_w *
                                (long long)piece.out_h *
                                (long long)out->bpp;
            piece.zcom_bits = EstimateZcomBitsForRect(img, piece.out_x,
                                                      piece.out_y,
                                                      piece.out_w,
                                                      piece.out_h,
                                                      out->bpp);
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        }
    }

    return true;
}

static bool BuildAutoSplitPieceForRect(const IMG *img, int rx, int ry,
                                       int rw, int rh, int piece_no, int bpp,
                                       AutoChopPiecePreview *piece)
{
    if (!img || !img->data_p || !piece || rw <= 0 || rh <= 0) return false;

    int stride = ((int)img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    int min_x = rw, min_y = rh;
    int max_x = -1, max_y = -1;
    int opaque = 0;

    for (int y = 0; y < rh; y++) {
        for (int x = 0; x < rw; x++) {
            if (src[(ry + y) * stride + (rx + x)] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
                opaque++;
            }
        }
    }

    if (max_x < min_x) return false;

    if (!g_chop_trim) {
        min_x = 0; min_y = 0;
        max_x = rw - 1; max_y = rh - 1;
    }

    *piece = {};
    piece->cell_x = rx;
    piece->cell_y = ry;
    piece->cell_w = rw;
    piece->cell_h = rh;
    piece->out_x = rx + min_x;
    piece->out_y = ry + min_y;
    piece->out_w = max_x - min_x + 1;
    piece->out_h = max_y - min_y + 1;
    piece->piece_no = piece_no;
    piece->opaque_pixels = opaque;
    piece->uncomp_bits = (long long)piece->out_w *
                         (long long)piece->out_h *
                         (long long)bpp;
    piece->zcom_bits = EstimateZcomBitsForRect(img, piece->out_x,
                                               piece->out_y,
                                               piece->out_w,
                                               piece->out_h,
                                               bpp);
    return true;
}

static bool BuildAutoSplitPreviewForImageAt(const IMG *img, bool vertical,
                                            int split_pos,
                                            AutoChopPreview *out)
{
    if (!out) return false;
    AutoChopPreviewClear(out);
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int w = (int)img->w;
    int h = (int)img->h;
    if (vertical) {
        if (split_pos <= k_auto_split_min_side ||
            w - split_pos <= k_auto_split_min_side)
            return false;
    } else {
        if (split_pos <= k_auto_split_min_side ||
            h - split_pos <= k_auto_split_min_side)
            return false;
    }

    out->target_count = 1;
    out->raw_cells = 2;
    out->bpp = AutoChopBppForImage(img);
    out->src_uncomp_bits = (long long)w * (long long)h * out->bpp;
    out->src_zcom_bits = EstimateZcomBitsForRect(img, 0, 0, w, h, out->bpp);
    out->best_split_valid = true;
    out->best_split_vertical = vertical;
    out->best_split_pos = split_pos;

    AutoChopPiecePreview piece = {};
    if (vertical) {
        if (BuildAutoSplitPieceForRect(img, 0, 0, split_pos, h, 0,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
        if (BuildAutoSplitPieceForRect(img, split_pos, 0, w - split_pos, h, 1,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
    } else {
        if (BuildAutoSplitPieceForRect(img, 0, 0, w, split_pos, 0,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
        if (BuildAutoSplitPieceForRect(img, 0, split_pos, w, h - split_pos, 1,
                                       out->bpp, &piece)) {
            out->split_uncomp_bits += piece.uncomp_bits;
            out->split_zcom_bits += piece.zcom_bits;
            out->pieces.push_back(piece);
        } else {
            out->empty_cells++;
        }
    }

    return out->pieces.size() == 2;
}

static bool BuildBestAutoSplitPreviewForImage(const IMG *img, bool vertical,
                                              AutoChopPreview *out)
{
    if (!out) return false;
    AutoChopPreviewClear(out);
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int dim = vertical ? (int)img->w : (int)img->h;
    int first = k_auto_split_min_side + 1;
    int last = dim - (k_auto_split_min_side + 1);
    if (first > last) return false;

    bool have_best = false;
    AutoChopPreview best;
    for (int split = first; split <= last; split++) {
        AutoChopPreview trial;
        if (!BuildAutoSplitPreviewForImageAt(img, vertical, split, &trial))
            continue;
        if (!have_best ||
            trial.split_zcom_bits < best.split_zcom_bits ||
            (trial.split_zcom_bits == best.split_zcom_bits &&
             std::abs(split - dim / 2) < std::abs(best.best_split_pos - dim / 2))) {
            best = trial;
            have_best = true;
        }
    }

    if (!have_best) return false;
    *out = best;
    return true;
}

static bool SelectedImageWillAutoChop(void)
{
    if (g_doc->ilselected < 0) return false;
    int marked = CountMarkedImages();
    if (marked == 0) return true;
    IMG *img = get_img(g_doc->ilselected);
    return img && (img->flags & 1);
}

static void BuildAutoChopTargetSummary(AutoChopPreview *out)
{
    if (!out) return;
    AutoChopPreviewClear(out);
    int marked = CountMarkedImages();
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0)
                                 : (idx == g_doc->ilselected);
        if (!target) continue;

        AutoChopPreview one;
        if (!BuildAutoChopPreviewForImage(img, &one)) continue;
        out->target_count += one.target_count;
        out->raw_cells += one.raw_cells;
        out->empty_cells += one.empty_cells;
        out->src_uncomp_bits += one.src_uncomp_bits;
        out->src_zcom_bits += one.src_zcom_bits;
        out->split_uncomp_bits += one.split_uncomp_bits;
        out->split_zcom_bits += one.split_zcom_bits;
        if (out->bpp == 0) out->bpp = one.bpp;
        else if (out->bpp != one.bpp) out->bpp = -1;
        out->pieces.insert(out->pieces.end(), one.pieces.begin(), one.pieces.end());
    }
}

struct AutoChopTargetRef {
    IMG *img;
    int idx;
};

static void CollectAutoChopTargets(std::vector<AutoChopTargetRef> &targets)
{
    targets.clear();
    int marked = CountMarkedImages();
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0)
                                 : (idx == g_doc->ilselected);
        if (target) targets.push_back({img, idx});
    }
}

struct AutoSplitTargetSummary {
    AutoChopPreview selected_preview;
    int target_count;
    int split_count;
    int skipped_count;
    int bpp;
    long long src_zcom_bits;
    long long split_zcom_bits;
};

static void AutoSplitTargetSummaryClear(AutoSplitTargetSummary *s)
{
    if (!s) return;
    AutoChopPreviewClear(&s->selected_preview);
    s->target_count = 0;
    s->split_count = 0;
    s->skipped_count = 0;
    s->bpp = 0;
    s->src_zcom_bits = 0;
    s->split_zcom_bits = 0;
}

static void BuildAutoSplitTargetSummary(bool vertical,
                                        AutoSplitTargetSummary *summary)
{
    if (!summary) return;
    AutoSplitTargetSummaryClear(summary);

    std::vector<AutoChopTargetRef> targets;
    CollectAutoChopTargets(targets);
    summary->target_count = (int)targets.size();

    for (const AutoChopTargetRef &target : targets) {
        AutoChopPreview preview;
        if (!BuildBestAutoSplitPreviewForImage(target.img, vertical, &preview)) {
            summary->skipped_count++;
            continue;
        }

        if (target.idx == g_doc->ilselected)
            summary->selected_preview = preview;
        summary->split_count++;
        summary->src_zcom_bits += preview.src_zcom_bits;
        summary->split_zcom_bits += preview.split_zcom_bits;
        if (summary->bpp == 0) summary->bpp = preview.bpp;
        else if (summary->bpp != preview.bpp) summary->bpp = -1;
    }
}

static void UnlinkAllocatedImage(IMG *img)
{
    if (!img) return;
    IMG *prev = NULL;
    IMG *cur = (IMG *)g_doc->img_p;
    while (cur && cur != img) {
        prev = cur;
        cur = (IMG *)cur->nxt_p;
    }
    if (cur == img) {
        if (prev) prev->nxt_p = cur->nxt_p;
        else g_doc->img_p = cur->nxt_p;
        if (g_doc->imgcnt > 0) g_doc->imgcnt--;
    }
    FreeImg(img);
}

static bool CreateAutoSplitPiece(IMG *master,
                                 const AutoChopPiecePreview &piece,
                                 const char *suffix)
{
    if (!master || !master->data_p || piece.out_w <= 0 || piece.out_h <= 0)
        return false;

    IMG *child = AllocImg();
    if (!child) return false;

    child->w = (unsigned short)piece.out_w;
    child->h = (unsigned short)piece.out_h;
    child->palnum = master->palnum;
    child->flags = 0;
    child->opals = master->opals;
    child->anix = signed_to_img_word((int)(short)master->anix - piece.out_x);
    child->aniy = signed_to_img_word((int)(short)master->aniy - piece.out_y);
    clear_secondary_anipoint(child);

    int src_stride = ((int)master->w + 3) & ~3;
    int dst_stride = (piece.out_w + 3) & ~3;
    child->data_p = PoolAlloc((size_t)dst_stride * (size_t)piece.out_h);
    if (!child->data_p) {
        UnlinkAllocatedImage(child);
        return false;
    }

    const unsigned char *src = (const unsigned char *)master->data_p;
    unsigned char *dst = (unsigned char *)child->data_p;
    for (int y = 0; y < piece.out_h; y++) {
        memcpy(dst + y * dst_stride,
               src + (piece.out_y + y) * src_stride + piece.out_x,
               piece.out_w);
    }

    strncpy(child->src_filename, master->src_filename,
            sizeof(child->src_filename) - 1);
    child->src_filename[sizeof(child->src_filename) - 1] = '\0';
    MakeDerivedImageName(master->n_s, suffix, child->n_s);
    return true;
}

static int ApplyBestAutoSplitToTargets(bool vertical)
{
    std::vector<AutoChopTargetRef> targets;
    CollectAutoChopTargets(targets);
    if (targets.empty()) return 0;

    struct PendingSplit {
        IMG *img;
        int idx;
        AutoChopPreview preview;
    };
    std::vector<PendingSplit> pending;
    for (const AutoChopTargetRef &target : targets) {
        AutoChopPreview preview;
        if (BuildBestAutoSplitPreviewForImage(target.img, vertical, &preview))
            pending.push_back({target.img, target.idx, preview});
    }
    if (pending.empty()) return 0;

    if (!doc_undo_push()) return 0;

    int created = 0;
    int first_created_idx = -1;
    for (const PendingSplit &plan : pending) {
        int local_created = 0;
        for (int i = 0; i < (int)plan.preview.pieces.size(); i++) {
            char suffix[4];
            snprintf(suffix, sizeof(suffix), "%c", 'A' + i);
            if (CreateAutoSplitPiece(plan.img, plan.preview.pieces[(size_t)i], suffix)) {
                local_created++;
                created++;
                if (first_created_idx < 0)
                    first_created_idx = (int)g_doc->imgcnt - 1;
            }
        }
        if (local_created > 0) {
            plan.img->flags &= ~1;
            InvalidateThumb(plan.idx);
        }
    }

    if (created > 0) {
        if (first_created_idx >= 0) g_doc->ilselected = first_created_idx;
        g_img_tex_idx = -2;
        g_zoom_reset = true;
        mark_dirty();
    }
    return created;
}

static void AutoChopPieceLabel(const AutoChopPiecePreview &piece,
                               char *buf, size_t buf_sz)
{
    if (!buf || buf_sz == 0) return;
    if (piece.piece_no < 26)
        snprintf(buf, buf_sz, "%c", 'A' + piece.piece_no);
    else
        snprintf(buf, buf_sz, "%02d", piece.piece_no + 1);
}

static void DrawAutoChopPreviewRects(ImDrawList *dl,
                                     const AutoChopPreview &preview,
                                     ImVec2 img_pos, float sx, float sy,
                                     bool foreground)
{
    if (!dl || preview.pieces.empty()) return;

    for (const AutoChopPiecePreview &piece : preview.pieces) {
        ImVec2 cell_min(img_pos.x + piece.cell_x * sx,
                        img_pos.y + piece.cell_y * sy);
        ImVec2 cell_max(img_pos.x + (piece.cell_x + piece.cell_w) * sx,
                        img_pos.y + (piece.cell_y + piece.cell_h) * sy);
        ImVec2 out_min(img_pos.x + piece.out_x * sx,
                       img_pos.y + piece.out_y * sy);
        ImVec2 out_max(img_pos.x + (piece.out_x + piece.out_w) * sx,
                       img_pos.y + (piece.out_y + piece.out_h) * sy);

        if (!foreground) {
            ImU32 fill = g_chop_trim ? IM_COL32(30, 190, 115, 58)
                                     : IM_COL32(255, 185, 45, 44);
            dl->AddRectFilled(out_min, out_max, fill);
            continue;
        }

        if (g_chop_trim) {
            dl->AddRect(cell_min, cell_max, IM_COL32(255, 210, 80, 120),
                        0.0f, 0, 1.0f);
        }
        ImU32 halo = IM_COL32(0, 0, 0, 210);
        ImU32 col = g_chop_trim ? IM_COL32(55, 240, 150, 245)
                                : IM_COL32(255, 190, 65, 245);
        dl->AddRect(ImVec2(out_min.x - 1.0f, out_min.y - 1.0f),
                    ImVec2(out_max.x + 1.0f, out_max.y + 1.0f),
                    halo, 0.0f, 0, 3.0f);
        dl->AddRect(out_min, out_max, col, 0.0f, 0, 1.5f);

        if ((out_max.x - out_min.x) >= 14.0f &&
            (out_max.y - out_min.y) >= 12.0f) {
            char label[8];
            AutoChopPieceLabel(piece, label, sizeof(label));
            ImVec2 text_pos(out_min.x + 3.0f, out_min.y + 2.0f);
            dl->AddText(ImVec2(text_pos.x + 1.0f, text_pos.y + 1.0f),
                        halo, label);
            dl->AddText(text_pos, col, label);
        }
    }
}

static char g_restore_regex_buf[256] = "^(.+)[A-Z]$";
static std::vector<BulkRestoreMatch> g_restore_matches;
static bool g_restore_regex_tested = false;
static bool g_restore_regex_error = false;
/* Mode: 0 = Replace (overwrite child bbox with parent pixels — clobbers
 * hand-tuned per-piece details). 1 = Diff (only propagate the user's
 * edits to the master, leaving every untouched pixel alone). Diff is the
 * right choice when adding a small detail to a master sprite. */
static int g_restore_diff_mode = 1;
static int g_last_delete_removed_palettes = 0;

static void NormalizeImageDeleteIndices(std::vector<int> *indices)
{
    if (!indices) return;
    indices->erase(std::remove_if(indices->begin(), indices->end(),
        [](int idx) { return idx < 0 || (unsigned int)idx >= g_doc->imgcnt; }),
        indices->end());
    std::sort(indices->begin(), indices->end());
    indices->erase(std::unique(indices->begin(), indices->end()), indices->end());
}

static void RemapTimelineAfterImageDelete(const std::vector<int> &deleted)
{
    if (deleted.empty()) return;

    auto remap_index = [&](int idx) {
        if (idx < 0) return -1;
        if (std::binary_search(deleted.begin(), deleted.end(), idx)) return -1;
        int shift = (int)(std::lower_bound(deleted.begin(), deleted.end(), idx) - deleted.begin());
        return idx - shift;
    };

    EnsureTimelineHolds();
    std::vector<int> remapped_frames;
    std::vector<int> remapped_holds;
    remapped_frames.reserve(g_timeline_frames.size());
    remapped_holds.reserve(g_timeline_holds.size());
    for (size_t i = 0; i < g_timeline_frames.size(); i++) {
        int idx = remap_index(g_timeline_frames[i]);
        if (idx < 0) continue;
        remapped_frames.push_back(idx);
        remapped_holds.push_back(g_timeline_holds[i]);
    }
    g_timeline_frames.swap(remapped_frames);
    g_timeline_holds.swap(remapped_holds);

    for (int i = 0; i < 2; i++) {
        g_timeline_composite[i] = remap_index(g_timeline_composite[i]);
        if (g_timeline_composite[i] < 0)
            g_timeline_composite_locked[i] = false;
    }
    CompactTimelineCompositeSelection();
    if (g_timeline_composite[0] < 0 || g_timeline_composite[1] < 0)
        ClearTimelineCompositeSelection();

    if (g_timeline_play_idx >= (int)g_timeline_frames.size())
        g_timeline_play_idx = 0;
    g_timeline_built_for_imgcnt = g_doc->imgcnt;
    ClearTimelineThumbCache();
}

static int DeleteImagesByIndices(std::vector<int> indices)
{
    g_last_delete_removed_palettes = 0;
    NormalizeImageDeleteIndices(&indices);
    if (indices.empty()) return 0;

    std::vector<int> candidate_palettes;
    candidate_palettes.reserve(indices.size());
    for (int delete_idx : indices) {
        IMG *img = get_img(delete_idx);
        if (img) candidate_palettes.push_back((int)img->palnum);
    }
    std::sort(candidate_palettes.begin(), candidate_palettes.end());
    candidate_palettes.erase(std::unique(candidate_palettes.begin(), candidate_palettes.end()),
                             candidate_palettes.end());

    doc_undo_push();

    IMG *prev = NULL;
    IMG *curr = (IMG *)g_doc->img_p;
    int idx = 0;
    int deleted_count = 0;
    int deleted_before_sel = 0;
    bool sel_was_deleted = false;
    int old_sel = g_doc->ilselected;

    while (curr) {
        bool delete_this = std::binary_search(indices.begin(), indices.end(), idx);
        if (delete_this) {
            IMG *to_delete = curr;
            if (prev) prev->nxt_p = curr->nxt_p;
            else g_doc->img_p = curr->nxt_p;
            curr = (IMG *)curr->nxt_p;
            g_doc->imgcnt--;
            deleted_count++;

            if (idx < old_sel) deleted_before_sel++;
            else if (idx == old_sel) sel_was_deleted = true;

            if (to_delete->data_p) free(to_delete->data_p);
            if (to_delete->pttbl_p) free(to_delete->pttbl_p);
            if (to_delete->baseline_p) free(to_delete->baseline_p);
            free(to_delete);
        } else {
            prev = curr;
            curr = (IMG *)curr->nxt_p;
        }
        idx++;
    }

    if (g_doc->imgcnt == 0) {
        g_doc->ilselected = -1;
    } else {
        int new_sel = old_sel - deleted_before_sel;
        if (sel_was_deleted && new_sel >= (int)g_doc->imgcnt)
            new_sel = (int)g_doc->imgcnt - 1;
        if (new_sel < 0) new_sel = 0;
        if (new_sel >= (int)g_doc->imgcnt) new_sel = (int)g_doc->imgcnt - 1;
        g_doc->ilselected = new_sel;
    }

    RemapTimelineAfterImageDelete(indices);
    int deleted_palettes = 0;
    if (!candidate_palettes.empty() && g_doc->palcnt > 0) {
        std::vector<unsigned char> used((size_t)g_doc->palcnt, 0);
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            int pal_idx = (int)img->palnum;
            if (pal_idx >= 0 && (unsigned int)pal_idx < g_doc->palcnt)
                used[(size_t)pal_idx] = 1;
        }

        std::sort(candidate_palettes.begin(), candidate_palettes.end(), std::greater<int>());
        for (int pal_idx : candidate_palettes) {
            if (pal_idx < 0 || (unsigned int)pal_idx >= g_doc->palcnt) continue;
            if (used[(size_t)pal_idx]) continue;

            PAL *prev_pal = NULL;
            PAL *pal = (PAL *)g_doc->pal_p;
            for (int i = 0; pal && i < pal_idx; i++) {
                prev_pal = pal;
                pal = (PAL *)pal->nxt_p;
            }
            if (!pal) continue;

            if (prev_pal) prev_pal->nxt_p = pal->nxt_p;
            else g_doc->pal_p = pal->nxt_p;
            FreePal(pal);
            g_doc->palcnt--;
            deleted_palettes++;

            for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > pal_idx)
                    img->palnum--;
            }
            if (g_doc->plselected == pal_idx)
                g_doc->plselected = -1;
            else if (g_doc->plselected > pal_idx)
                g_doc->plselected--;
        }
    }
    g_last_delete_removed_palettes = deleted_palettes;

    IMG *sel = get_img(g_doc->ilselected);
    if (sel && (unsigned int)sel->palnum < g_doc->palcnt)
        g_doc->plselected = (int)sel->palnum;
    else if (g_doc->palcnt == 0)
        g_doc->plselected = -1;
    else if (g_doc->plselected < 0 || (unsigned int)g_doc->plselected >= g_doc->palcnt)
        g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_palette_nav = false;
    if (deleted_palettes > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Deleted %d sprite%s and %d now-unused palette%s.",
                 deleted_count, deleted_count == 1 ? "" : "s",
                 deleted_palettes, deleted_palettes == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    return deleted_count;
}

static int DeleteImage(int idx)
{
    std::vector<int> indices;
    indices.push_back(idx);
    return DeleteImagesByIndices(indices);
}

static int DeleteMarkedImages(void)
{
    std::vector<int> indices;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img->flags & 1) indices.push_back(idx);
    }
    return DeleteImagesByIndices(indices);
}

static void CollectSubframeIndicesForParent(int parent_idx, std::vector<int> *out)
{
    if (!out) return;
    out->clear();
    IMG *parent = get_img(parent_idx);
    if (!parent) return;

    std::string parent_name = img_name_string(parent);
    if (parent_name.empty()) return;
    std::string parent_src = parent->src_filename[0] ? parent->src_filename : "Workspace";

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (idx == parent_idx) continue;
        std::string src = img->src_filename[0] ? img->src_filename : "Workspace";
        if (src != parent_src) continue;
        std::string child_name = img_name_string(img);
        if (InferSubframeParentName(child_name.c_str()) == parent_name)
            out->push_back(idx);
    }
}

static void CollectExtraSubframeIndices(const std::vector<int> &base_indices,
                                        std::vector<int> *extra_indices,
                                        int *parent_count)
{
    if (extra_indices) extra_indices->clear();
    if (parent_count) *parent_count = 0;
    if (!extra_indices || base_indices.empty()) return;

    std::vector<int> base = base_indices;
    NormalizeImageDeleteIndices(&base);

    for (int idx : base) {
        std::vector<int> children;
        CollectSubframeIndicesForParent(idx, &children);

        bool parent_has_extra = false;
        for (int child_idx : children) {
            if (std::binary_search(base.begin(), base.end(), child_idx)) continue;
            if (std::find(extra_indices->begin(), extra_indices->end(), child_idx) != extra_indices->end()) continue;
            extra_indices->push_back(child_idx);
            parent_has_extra = true;
        }
        if (parent_has_extra && parent_count) (*parent_count)++;
    }

    NormalizeImageDeleteIndices(extra_indices);
}

static void ClearPendingImageDelete(void)
{
    g_pending_delete_base_indices.clear();
    g_pending_delete_subframe_indices.clear();
    g_pending_delete_parent_name[0] = '\0';
    g_show_delete_images_confirm = false;
}

static void RequestDeleteImage(int idx)
{
    if (idx < 0 || (unsigned int)idx >= g_doc->imgcnt) return;

    std::vector<int> base;
    std::vector<int> extra;
    base.push_back(idx);
    CollectExtraSubframeIndices(base, &extra, NULL);
    if (extra.empty()) {
        DeleteImage(idx);
        return;
    }

    IMG *img = get_img(idx);
    snprintf(g_pending_delete_parent_name, sizeof(g_pending_delete_parent_name),
             "%.15s", img ? img->n_s : "sprite");
    g_pending_delete_base_indices = base;
    g_pending_delete_subframe_indices = extra;
    g_show_delete_images_confirm = true;
}

static void RequestDeleteMarkedImages(void)
{
    std::vector<int> base;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img->flags & 1) base.push_back(idx);
    }
    NormalizeImageDeleteIndices(&base);
    if (base.empty()) return;

    std::vector<int> extra;
    CollectExtraSubframeIndices(base, &extra, NULL);
    if (extra.empty()) {
        int deleted = DeleteMarkedImages();
        if (g_last_delete_removed_palettes > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d marked sprite%s and %d now-unused palette%s.",
                     deleted, deleted == 1 ? "" : "s",
                     g_last_delete_removed_palettes,
                     g_last_delete_removed_palettes == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d marked sprite%s.", deleted, deleted == 1 ? "" : "s");
        }
        g_restore_msg_timer = 4.0f;
        return;
    }

    g_pending_delete_parent_name[0] = '\0';
    g_pending_delete_base_indices = base;
    g_pending_delete_subframe_indices = extra;
    g_show_delete_images_confirm = true;
}

/* Swap two adjacent IMG nodes in the linked list. `before_a` is the node
   whose nxt_p points at `a` (or NULL if `a` is the head); `b` must equal
   a->nxt_p. After the call, the order is ...before_a -> b -> a -> b->nxt_p. */
static void swap_adjacent_img(IMG *before_a, IMG *a, IMG *b)
{
    a->nxt_p = b->nxt_p;
    b->nxt_p = a;
    if (before_a) before_a->nxt_p = b;
    else g_doc->img_p = b;
}

static void MoveImageUp(void)
{
    if (g_doc->ilselected <= 0) return;
    doc_undo_push();   /* reorders the image list — structural */

    IMG *before_prev = NULL;
    IMG *prev = (IMG *)g_doc->img_p;
    for (int i = 0; prev && i < g_doc->ilselected - 1; i++) {
        before_prev = prev;
        prev = (IMG *)prev->nxt_p;
    }
    if (!prev || !prev->nxt_p) return;
    swap_adjacent_img(before_prev, prev, (IMG *)prev->nxt_p);
    g_doc->ilselected--;
    g_img_tex_idx = -2;
}

static void MoveImageDown(void)
{
    if (g_doc->ilselected < 0) return;
    doc_undo_push();   /* reorders the image list — structural */

    IMG *before_curr = NULL;
    IMG *curr = (IMG *)g_doc->img_p;
    for (int i = 0; curr && i < g_doc->ilselected; i++) {
        before_curr = curr;
        curr = (IMG *)curr->nxt_p;
    }
    if (!curr || !curr->nxt_p) return;
    swap_adjacent_img(before_curr, curr, (IMG *)curr->nxt_p);
    g_doc->ilselected++;
    g_img_tex_idx = -2;
}

static void RemapImagePalettesAfterPaletteDelete(int deleted_idx, int fallback_idx)
{
    int img_idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        int pal_idx = (int)img->palnum;
        int new_idx = pal_idx;

        if (pal_idx == deleted_idx) {
            new_idx = fallback_idx;
        } else if (pal_idx > deleted_idx) {
            new_idx = pal_idx - 1;
        }

        if (g_doc->palcnt == 0) {
            new_idx = 0;
        } else if (new_idx < 0) {
            new_idx = 0;
        } else if ((unsigned)new_idx >= g_doc->palcnt) {
            new_idx = (int)g_doc->palcnt - 1;
        }

        if (new_idx != pal_idx) {
            img->palnum = (unsigned short)new_idx;
            InvalidateThumb(img_idx);
        }
    }
}

static void DeletePalette(void)
{
    if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) return;
    commit_palette_adjustments();
    doc_undo_push();

    int deleted_idx = g_doc->plselected;
    PAL *prev = NULL;
    PAL *curr = (PAL *)g_doc->pal_p;
    for (int i = 0; curr && i < deleted_idx; i++) {
        prev = curr;
        curr = (PAL *)curr->nxt_p;
    }
    if (!curr) return;

    if (prev) prev->nxt_p = curr->nxt_p;
    else g_doc->pal_p = curr->nxt_p;
    g_doc->palcnt--;

    int fallback_idx = -1;
    if (g_doc->palcnt > 0)
        fallback_idx = (deleted_idx < (int)g_doc->palcnt) ? deleted_idx : (int)g_doc->palcnt - 1;

    RemapImagePalettesAfterPaletteDelete(deleted_idx, fallback_idx);
    g_doc->plselected = fallback_idx;

    if (curr->data_p) free(curr->data_p);
    free(curr);

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
}

/* Assign the currently-selected palette to the currently-selected image.
   No-ops on an invalid selection on either side. */
static void SetPaletteOfSelected(void)
{
    if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) return;
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;
    doc_undo_push();
    img->palnum = (unsigned short)g_doc->plselected;
    g_img_tex_idx = -2;
}

/* Select a palette in the palette list, and commit it onto the active image
   so the choice sticks when the user switches sprites. Without the commit,
   g_doc->plselected was only a preview — moving to another sprite would overwrite
   it from img->palnum. No-ops when the image already uses this palette to
   keep the undo stack clean. */
static void SelectPalette(int idx)
{
    if (idx < 0 || (unsigned)idx >= g_doc->palcnt) return;
    commit_palette_adjustments();
    IMG *cur = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    bool assign_to_image = cur && cur->palnum != (unsigned short)idx;
    if (assign_to_image) doc_undo_push();
    g_doc->plselected = idx;
    g_palette_nav = true;
    if (assign_to_image) {
        cur->palnum = (unsigned short)idx;
        g_img_tex_idx = -2;
    }
}

/* Assign the currently-selected palette to every marked image. */
static void SetPaletteOfMarked(void)
{
    if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) return;
    bool any = false;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p)
        if (p->flags & 1) { any = true; break; }
    if (!any) return;
    doc_undo_push();
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p)
        if (p->flags & 1) p->palnum = (unsigned short)g_doc->plselected;
    g_img_tex_idx = -2;
}

/* Toggle the selected image's point table: allocate via the asm pool when
   absent (so mem_free can release it later), free when present. No "are you
   sure" — matches DeleteImage's no-confirm behavior. */
static void TogglePointTable(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;
    doc_undo_push();   /* pttbl alloc/free not captured by metadata undo */
    if (img->pttbl_p) {
        free(img->pttbl_p);
        img->pttbl_p = NULL;
    } else {
        AddPointTable(g_doc->ilselected);  /* cdecl-safe wrapper around img_pttbladd */
    }
}

/* Clear all "extra" anipt/pttbl data on every image. Mirrors ilst_clrxdata:
   clears the secondary anipoint sentinel and the contents of any attached
   PTTBL (without freeing the PTTBL itself, so toggle state is preserved). */
static void ClearExtraData(void)
{
    if (!g_doc->img_p) return;
    doc_undo_push();   /* clears anipoints + pttbl contents across all images */
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        clear_secondary_anipoint(p);
        if (p->pttbl_p) {
            /* PTTBL is 40 bytes per wmpstruc.inc: 8 dw header + 5 PTBOX
               (4 b each) + 1 PTCBOX (4 b). */
            memset(p->pttbl_p, 0, 40);
        }
    }
    g_img_tex_idx = -2;
}

/* ---- C++ ports of ASM operations ---- */

static void OpenRenameImage(void);  /* forward decl — used by DuplicateImage */

/* Flood fill helper — 4-connected stack-based fill */
static void FloodFill(IMG *img, int sx, int sy, unsigned char new_color)
{
    if (!img || !img->data_p || sx < 0 || sy < 0 || sx >= (int)img->w || sy >= (int)img->h)
        return;
    unsigned short stride = (unsigned short)((img->w + 3) & ~3);
    unsigned char *pixels = (unsigned char *)img->data_p;
    unsigned char old_color = pixels[sy * stride + sx];
    if (old_color == new_color) return;
    struct Pt { int x, y; };
    std::vector<Pt> stack; stack.reserve(4096);
    stack.push_back({sx, sy});
    while (!stack.empty()) {
        Pt p = stack.back(); stack.pop_back();
        if (p.x < 0 || p.x >= (int)img->w || p.y < 0 || p.y >= (int)img->h) continue;
        unsigned char *px = &pixels[p.y * stride + p.x];
        if (*px != old_color) continue;
        *px = new_color;
        stack.push_back({p.x + 1, p.y}); stack.push_back({p.x - 1, p.y});
        stack.push_back({p.x, p.y + 1}); stack.push_back({p.x, p.y - 1});
    }
}

static int PaintBucketFill(IMG *img, int sx, int sy, unsigned char new_color, int tolerance, bool contiguous)
{
    if (!img || !img->data_p || sx < 0 || sy < 0 || sx >= (int)img->w || sy >= (int)img->h)
        return 0;
    if (tolerance < 0) tolerance = 0;
    if (tolerance > 255) tolerance = 255;

    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pixels = (unsigned char *)img->data_p;
    unsigned char target = pixels[sy * stride + sx];
    if (target == new_color) return 0;

    auto in_range = [&](unsigned char v) {
        int d = (int)v - (int)target;
        if (d < 0) d = -d;
        return d <= tolerance;
    };

    int changed = 0;
    if (contiguous) {
        struct Pt { int x, y; };
        std::vector<Pt> stack;
        std::vector<unsigned char> seen((size_t)w * h, 0);
        stack.reserve(4096);
        stack.push_back({sx, sy});
        seen[sy * w + sx] = 1;

        while (!stack.empty()) {
            Pt p = stack.back();
            stack.pop_back();
            unsigned char *px = &pixels[p.y * stride + p.x];
            if (!in_range(*px)) continue;
            *px = new_color;
            changed++;

            const int dx[4] = {1, -1, 0, 0};
            const int dy[4] = {0, 0, 1, -1};
            for (int i = 0; i < 4; i++) {
                int nx = p.x + dx[i], ny = p.y + dy[i];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                size_t off = (size_t)ny * w + nx;
                if (seen[off]) continue;
                seen[off] = 1;
                if (in_range(pixels[ny * stride + nx]))
                    stack.push_back({nx, ny});
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                unsigned char *px = &pixels[y * stride + x];
                if (!in_range(*px)) continue;
                *px = new_color;
                changed++;
            }
        }
    }
    return changed;
}

/* Smart eraser: removes the clicked chroma color (and anything within
   `tolerance` palette-index distance of it) by setting it to index 0
   (transparency). In `contiguous` mode it floods from the click site; in
   global mode every matching pixel in the image is wiped.
   When `defringe` is set, after the chroma pass each transparent pixel
   that touches a still-opaque pixel scans its 8-neighborhood and replaces
   the opaque neighbor with the average of its own non-chroma neighbors —
   this kills the 1-pixel blue-spill halo that survives digitized actor
   bluescreen removal. */
static void SmartErase(IMG *img, int sx, int sy, int tolerance, bool contiguous, bool defringe)
{
    if (!img || !img->data_p || sx < 0 || sy < 0 ||
        sx >= (int)img->w || sy >= (int)img->h) return;
    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pix = (unsigned char *)img->data_p;
    int target = pix[sy * stride + sx];
    if (target == 0) return; /* clicked on existing transparent */

    auto in_range = [&](unsigned char v) {
        int d = (int)v - target;
        if (d < 0) d = -d;
        return d <= tolerance;
    };

    /* Mark which pixels we'll erase, so defringe can scan against the
       original neighborhood before zeroing. Multiply in size_t so the
       computation can't overflow int for pathological sprite sizes. */
    std::vector<unsigned char> kill((size_t)w * h, 0);

    if (contiguous) {
        struct Pt { int x, y; };
        std::vector<Pt> stack; stack.reserve(4096);
        stack.push_back({sx, sy});
        kill[sy * w + sx] = 1;
        while (!stack.empty()) {
            Pt p = stack.back(); stack.pop_back();
            const int dx[] = {1, -1, 0, 0};
            const int dy[] = {0, 0, 1, -1};
            for (int i = 0; i < 4; i++) {
                int nx = p.x + dx[i], ny = p.y + dy[i];
                if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                if (kill[ny * w + nx]) continue;
                if (!in_range(pix[ny * stride + nx])) continue;
                kill[ny * w + nx] = 1;
                stack.push_back({nx, ny});
            }
        }
    } else {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (in_range(pix[y * stride + x]))
                    kill[y * w + x] = 1;
            }
        }
    }

    /* Optionally compute defringe replacements before applying the kill. */
    std::vector<std::pair<int,unsigned char>> defringe_writes;
    if (defringe) {
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (kill[y * w + x]) continue;
                unsigned char self = pix[y * stride + x];
                if (self == 0) continue;
                /* Edge test: any 8-neighbor will be killed. */
                bool touches = false;
                for (int dy = -1; dy <= 1 && !touches; dy++) {
                    for (int dx = -1; dx <= 1 && !touches; dx++) {
                        if (!dx && !dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        if (kill[ny * w + nx]) touches = true;
                    }
                }
                if (!touches) continue;
                /* Average palette indices of safe (non-killed, non-zero,
                   not-itself-the-chroma) neighbors. This is a coarse proxy
                   for picking the nearest "skin/fabric" color in the
                   indexed palette — gives a much cleaner edge than just
                   leaving the blue-spill pixel alone. */
                int sum = 0, n = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (!dx && !dy) continue;
                        int nx = x + dx, ny = y + dy;
                        if (nx < 0 || nx >= w || ny < 0 || ny >= h) continue;
                        if (kill[ny * w + nx]) continue;
                        unsigned char nv = pix[ny * stride + nx];
                        if (nv == 0) continue;
                        if (in_range(nv)) continue;
                        sum += nv; n++;
                    }
                }
                if (n > 0) {
                    defringe_writes.push_back({y * stride + x, (unsigned char)(sum / n)});
                }
            }
        }
    }

    /* Apply the kill, then the defringe overrides. */
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (kill[y * w + x]) pix[y * stride + x] = 0;
        }
    }
    for (auto &w_ : defringe_writes) pix[w_.first] = w_.second;
}

/* Free all images, palettes, and sequence/script data.  Resets all counters
   and selections.  Ported from img_clearall — called before img_load. */
static void ClearAll(void)
{
    /* Delete all images (delete index 0 repeatedly, mirroring img_del(0) loop) */
    while (g_doc->img_p) {
        IMG *cur = (IMG *)g_doc->img_p;
        g_doc->img_p = cur->nxt_p;
        if (cur->data_p)  free(cur->data_p);
        if (cur->pttbl_p) free(cur->pttbl_p);
        if (cur->baseline_p) free(cur->baseline_p);
        free(cur);
    }
    g_doc->imgcnt = 0;
    g_doc->ilselected = -1;

    /* Delete all palettes */
    while (g_doc->pal_p) {
        PAL *cur = (PAL *)g_doc->pal_p;
        g_doc->pal_p = cur->nxt_p;
        if (cur->data_p) free(cur->data_p);
        free(cur);
    }
    g_doc->palcnt = 0;
    g_doc->plselected = -1;

    /* Free sequence/script memory */
    if (g_doc->scrseqmem_p) {
        free(g_doc->scrseqmem_p);
        g_doc->scrseqmem_p = NULL;
        g_doc->scrseqbytes  = 0;
    }

    g_doc->seqcnt = 0;
    g_doc->scrcnt = 0;
    g_doc->damcnt = 0;
    g_doc->ilpalloaded = -1;

    /* Reset second image list */
    if (g_doc->img2_p) {
        while (g_doc->img2_p) {
            IMG *cur = (IMG *)g_doc->img2_p;
            g_doc->img2_p = cur->nxt_p;
            if (cur->data_p)  free(cur->data_p);
            if (cur->pttbl_p) free(cur->pttbl_p);
            if (cur->baseline_p) free(cur->baseline_p);
            free(cur);
        }
    }
    g_doc->img2cnt     = 0;
    g_doc->il2selected = -1;
    g_doc->il1stprt    = 0;
    g_doc->il21stprt   = 0;

    g_img_tex_idx = -2;
    g_palette_nav = false;
    reset_palette_adjust_sliders();
    g_palette_baseline_nc = 0;
    InvalidatePaletteSync();
}

/* Swap to the alternate (second) image list.  Purely swaps globals —
   no ASM dependencies. */
static void SwitchImageList(void)
{
    void *tmp_p = g_doc->img_p;  g_doc->img_p = g_doc->img2_p;  g_doc->img2_p = tmp_p;
    unsigned int tmp_cnt = g_doc->imgcnt;  g_doc->imgcnt = g_doc->img2cnt;  g_doc->img2cnt = tmp_cnt;
    int tmp_sel = g_doc->ilselected;  g_doc->ilselected = g_doc->il2selected;  g_doc->il2selected = tmp_sel;
    unsigned int tmp_prt = g_doc->il1stprt;  g_doc->il1stprt = g_doc->il21stprt;  g_doc->il21stprt = tmp_prt;
    g_img_tex_idx = -2;
}

/* Set the selected image's PTTBL.ID to (g_doc->il2selected + 1).  If no PTTBL
   exists for the image, allocate one via the ASM memory pool. */
static void SetIDFromSecondList(void)
{
    mark_dirty();
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;

    if (!img->pttbl_p) {
        AddPointTable(g_doc->ilselected);
        img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
        if (!img || !img->pttbl_p) return;
    }

    /* PTTBL.ID is at struct offset 14 (dw aligned, pack-2) */
    unsigned char *pttbl = (unsigned char *)img->pttbl_p;
    unsigned short new_id = (unsigned short)(g_doc->il2selected + 1);
    pttbl[14] = (unsigned char)(new_id & 0xFF);
    pttbl[15] = (unsigned char)(new_id >> 8);
}

static bool ImageNameExists(const char *name)
{
    if (!name || !*name) return false;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        char existing[16];
        strncpy(existing, p->n_s, 15);
        existing[15] = '\0';
        if (strcmp(existing, name) == 0) return true;
    }
    return false;
}

static void MakeDerivedImageName(const char *base, const char *suffix, char out[16])
{
    char root[16];
    if (base && *base) {
        strncpy(root, base, 15);
        root[15] = '\0';
    } else {
        strncpy(root, "SPRITE", sizeof(root));
    }

    for (int attempt = 0; attempt < 1000; attempt++) {
        char tail[8];
        if (attempt == 0) snprintf(tail, sizeof(tail), "%s", suffix ? suffix : "");
        else              snprintf(tail, sizeof(tail), "%s%d", suffix ? suffix : "", attempt);

        size_t tail_len = strlen(tail);
        size_t budget = (tail_len < 15) ? (15 - tail_len) : 0;
        snprintf(out, 16, "%.*s%s", (int)budget, root, tail);
        if (!ImageNameExists(out)) return;
    }

    snprintf(out, 16, "%.15s", root);
}

/* Duplicate the selected image: allocate a new IMG via the ASM pool,
   copy all fields (including pixel data and PTTBL), then open the
   ImGui rename dialog so the user can name the copy. */
static void DuplicateImage(void)
{
    IMG *src = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!src) return;

    doc_undo_push();   /* adds a new image — undo must remove it */

    IMG *dst = (IMG *)AllocImg();
    if (!dst) return;

    /* Copy pixel data */
    dst->data_p = NULL;
    if (src->data_p) {
        unsigned int stride = ((unsigned int)src->w + 3) & ~3;
        unsigned int sz = stride * src->h;
        dst->data_p = malloc(sz);
        if (!dst->data_p) goto err;
        memcpy(dst->data_p, src->data_p, sz);
    }

    /* Copy point table */
    dst->pttbl_p = NULL;
    if (src->pttbl_p) {
        dst->pttbl_p = malloc(40);
        if (!dst->pttbl_p) goto err;
        memcpy(dst->pttbl_p, src->pttbl_p, 40);
    }

    /* Copy header fields */
    dst->flags  = src->flags;
    dst->anix   = src->anix;
    dst->aniy   = src->aniy;
    dst->w      = src->w;
    dst->h      = src->h;
    dst->palnum = src->palnum;
    dst->anix2  = src->anix2;
    dst->aniy2  = src->aniy2;
    dst->aniz2  = src->aniz2;
    dst->opals  = src->opals;

    strncpy(dst->src_filename, src->src_filename, sizeof(dst->src_filename) - 1);
    dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';
    MakeDerivedImageName(src->n_s, "DUP", dst->n_s);

    /* Select the new image and open rename */
    g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    OpenRenameImage();
    return;

err:
    /* Rollback: delete the newly-allocated image */
    if (dst->data_p) free(dst->data_p);
    if (dst->pttbl_p) free(dst->pttbl_p);
    /* img_alloc appended the node — unlink it */
    {
        IMG *prev = NULL;
        IMG *cur = (IMG *)g_doc->img_p;
        while (cur && cur != dst) { prev = cur; cur = (IMG *)cur->nxt_p; }
        if (cur == dst) {
            if (prev) prev->nxt_p = cur->nxt_p;
            else g_doc->img_p = cur->nxt_p;
            g_doc->imgcnt--;
        }
    }
    free(dst);
}

/* Add a new blank IMG of the given size (8bpp, transparent) to the current
 * library. Uses the currently selected palette if any, otherwise palette 0.
 * Useful when starting a new library from scratch — File → New gives you
 * an empty library, "Add" on palettes makes a blank palette, then this
 * gives you the first sprite to start drawing on.
 * Width and height are clamped to 1..1024 — the IMAGE_disk fields are 16-bit
 * but the legacy toolchain breaks well below that, and a 1024×1024 sprite is
 * already four MiB so larger sizes are unlikely to be useful for authoring. */
static void AddNewBlankImage(int w = 32, int h = 32)
{
    if (w < 1)    w = 1;
    if (w > 1024) w = 1024;
    if (h < 1)    h = 1;
    if (h > 1024) h = 1024;
    mark_dirty();
    IMG *img = AllocImg();
    if (!img) return;

    img->w        = (unsigned short)w;
    img->h        = (unsigned short)h;
    img->flags    = 0;
    img->anix     = 0;
    img->aniy     = 0;
    clear_secondary_anipoint(img);
    img->opals    = (unsigned short)-1;
    img->pttbl_p  = NULL;
    img->palnum   = (g_doc->plselected >= 0) ? (unsigned short)g_doc->plselected : 0;

    unsigned int stride = ((unsigned int)img->w + 3) & ~3;
    unsigned int sz = stride * img->h;
    img->data_p = PoolAlloc(sz);
    if (img->data_p) memset(img->data_p, 0, sz);
    img->baseline_p = PoolAlloc(sz);
    if (img->baseline_p) {
        memset(img->baseline_p, 0, sz);
        img->baseline_w = img->w;
        img->baseline_h = img->h;
    }

    static int next_id = 1;
    snprintf(img->n_s, sizeof(img->n_s), "NEW%d", next_id++);

    if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
}

/* Pick a unique 9-char name for a freshly-allocated palette. Format: PAL%02d,
   bumping the suffix until it doesn't collide with an existing palette name.
   The name field is 10 bytes including NUL, so cap the digits at 8. */
static void make_unique_pal_name(char out[10])
{
    for (int n = 1; n < 100000000; n++) {
        char cand[10];
        snprintf(cand, sizeof(cand), "PAL%d", n);
        bool clash = false;
        for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
            if (strncmp(p->n_s, cand, 10) == 0) { clash = true; break; }
        }
        if (!clash) { memcpy(out, cand, 10); return; }
    }
    out[0] = '\0';
}

/* Add a new blank 256-color palette, select it, and commit it onto the active
   image so the next pixel paint actually uses it. */
static void AddNewPalette(void)
{
    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = 256;
    pal->pad     = 0;
    make_unique_pal_name(pal->n_s);

    unsigned char *buf = (unsigned char *)PoolAlloc(512);
    if (!buf) return;
    pal->data_p = buf;
    memset(buf, 0, 512);

    g_doc->plselected = (int)g_doc->palcnt - 1;
    IMG *cur = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (cur) cur->palnum = (unsigned short)g_doc->plselected;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
}

static void DuplicatePalette(void)
{
    PAL *src = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!src || !src->data_p) return;

    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = src->flags;
    pal->bitspix = src->bitspix;
    pal->numc    = src->numc;
    pal->pad     = 0;
    make_unique_pal_name(pal->n_s);

    unsigned int col_sz = (unsigned int)pal->numc * 2;
    unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
    if (!buf) return;
    pal->data_p = buf;
    memcpy(buf, src->data_p, col_sz);

    g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
}

/* Copy the selected palette into the cross-file palette clipboard. The buffer
   is malloc'd (not pool-allocated) so it survives File->Open. */
static void CopyPaletteToClipboard(void)
{
    PAL *src = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!src || !src->data_p || src->numc == 0) return;

    unsigned int col_sz = (unsigned int)src->numc * 2;
    unsigned char *buf = (unsigned char *)malloc(col_sz);
    if (!buf) return;
    memcpy(buf, src->data_p, col_sz);

    if (g_pal_clipboard.valid && g_pal_clipboard.data) free(g_pal_clipboard.data);
    g_pal_clipboard.valid   = true;
    g_pal_clipboard.numc    = src->numc;
    g_pal_clipboard.bitspix = src->bitspix;
    memcpy(g_pal_clipboard.n_s, src->n_s, 10);
    g_pal_clipboard.data    = buf;
}

/* Paste the clipboard palette as a new palette in the current library. */
static void PastePaletteFromClipboard(void)
{
    if (!g_pal_clipboard.valid || !g_pal_clipboard.data || g_pal_clipboard.numc == 0) return;

    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = 0;
    pal->bitspix = g_pal_clipboard.bitspix ? g_pal_clipboard.bitspix : 8;
    pal->numc    = g_pal_clipboard.numc;
    pal->pad     = 0;
    memcpy(pal->n_s, g_pal_clipboard.n_s, 10);

    unsigned int col_sz = (unsigned int)pal->numc * 2;
    unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
    if (!buf) return;
    pal->data_p = buf;
    memcpy(buf, g_pal_clipboard.data, col_sz);

    if (g_doc->palcnt > 0) g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
}

/* Word at a slot of the "merged" target: original target slots [0..base_count),
   then the colors queued for appending [base_count..base_count+added_count). */
static unsigned short MergedSlotWord(const unsigned char *target_colors, int base_count,
                                     const unsigned short *added, int slot)
{
    if (slot < base_count)
        return (unsigned short)(target_colors[slot * 2] |
                                (target_colors[slot * 2 + 1] << 8));
    return added ? added[slot - base_count] : 0;
}

/* Build src_index -> target_index remap against the target as it will look
   after growth (original colors plus any queued additions).  When added_count
   is 0 this is the plain nearest-existing-color remap. */
static void BuildPaletteMergeRemap(const PAL *src, const PAL *target, int base_count,
                                   const unsigned short *added, int added_count,
                                   bool perceptual, unsigned char remap[256])
{
    memset(remap, 0, 256);
    if (!src || !target || !src->data_p || !target->data_p) return;

    int src_count = (int)src->numc;
    if (src_count > 256) src_count = 256;
    const unsigned char *src_colors = (const unsigned char *)src->data_p;
    for (int si = 1; si < src_count; si++) {
        unsigned short sw = (unsigned short)(src_colors[si * 2] |
                                             (src_colors[si * 2 + 1] << 8));
        remap[si] = (unsigned char)FindNearestMergedSlot(target, base_count,
                                                         added, added_count,
                                                         sw, perceptual);
    }
}

static bool PaletteMergeQualityHasDrift(const PaletteMergeQuality &q)
{
    return q.color_drift_pixels > 0 ||
           q.transparent_drift_pixels > 0 ||
           q.invalid_pixels > 0 ||
           q.ppp_warning_images > 0;
}

static bool MergeTargetHasColor(const unsigned char *td, int base_count, unsigned short word)
{
    for (int i = 1; i < base_count; i++) {
        unsigned short w = (unsigned short)(td[i * 2] | (td[i * 2 + 1] << 8));
        if (w == word) return true;
    }
    return false;
}

/* Decide which source colors to append to the target's free slots. Only colors
   ACTUALLY USED by a sprite assigned to a marked source palette are considered,
   so unused palette entries never consume target slots. Colors already present
   in the target (exact match) or already queued are skipped. Returns the count
   and, via overflow_out, how many distinct used colors could not be added for
   lack of room (those get approximated to the nearest existing color). */
static int BuildMergeAdditions(int target_idx, int target_base_numc, bool grow,
                               unsigned short added[256], int *overflow_out)
{
    if (overflow_out) *overflow_out = 0;
    PAL *target = (target_idx >= 0) ? get_pal(target_idx) : NULL;
    if (!grow || !target || !target->data_p) return 0;
    if (target_base_numc > 256) target_base_numc = 256;
    const unsigned char *td = (const unsigned char *)target->data_p;
    int free_slots = 256 - target_base_numc;

    int added_count = 0, overflow = 0;
    int pal_idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, pal_idx++) {
        if (!(pal->flags & 1) || pal_idx == target_idx || !pal->data_p || pal->numc == 0)
            continue;
        int src_count = (int)pal->numc;
        if (src_count > 256) src_count = 256;
        const unsigned char *sd = (const unsigned char *)pal->data_p;

        /* Which source indices are actually painted by this palette's sprites. */
        bool used[256] = {false};
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if ((int)img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0)
                continue;
            int stride = (img->w + 3) & ~3;
            const unsigned char *px = (const unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++)
                for (int x = 0; x < img->w; x++)
                    used[px[y * stride + x]] = true;
        }

        for (int si = 1; si < src_count; si++) {
            if (!used[si]) continue;
            unsigned short w = (unsigned short)(sd[si * 2] | (sd[si * 2 + 1] << 8));
            if (MergeTargetHasColor(td, target_base_numc, w)) continue;
            bool dup = false;
            for (int j = 0; j < added_count; j++)
                if (added[j] == w) { dup = true; break; }
            if (dup) continue;
            if (added_count < free_slots) added[added_count++] = w;
            else overflow++;
        }
    }
    if (overflow_out) *overflow_out = overflow;
    return added_count;
}

static bool BuildMarkedPaletteMergeQuality(PaletteMergeQuality *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->target_idx = g_doc->plselected;
    out->opt_grow = g_merge_opt_grow;
    out->opt_perceptual = g_merge_opt_perceptual;

    PAL *target = (out->target_idx >= 0) ? get_pal(out->target_idx) : NULL;
    if (!target || !target->data_p || target->numc == 0) return false;
    snprintf(out->target_name, sizeof(out->target_name), "%.9s", target->n_s);

    int base_count = (int)target->numc;
    if (base_count > 256) base_count = 256;
    out->target_base_numc = base_count;
    const unsigned char *target_colors = (const unsigned char *)target->data_p;

    /* Plan the colors growth would append to the target's free slots. */
    out->colors_added = BuildMergeAdditions(out->target_idx, base_count, out->opt_grow,
                                            out->added_words, &out->colors_overflow);
    int final_count = base_count + out->colors_added;   /* <= 256 */

    int ppp_limit = (g_load2_ppp > 0 && g_load2_ppp <= 8) ? (1 << g_load2_ppp) : 0;

    int pal_idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, pal_idx++) {
        if (!(pal->flags & 1) || pal == target || !pal->data_p || pal->numc == 0)
            continue;

        out->source_palettes++;

        unsigned char remap[256];
        BuildPaletteMergeRemap(pal, target, base_count, out->added_words,
                               out->colors_added, out->opt_perceptual, remap);

        int src_count = (int)pal->numc;
        if (src_count > 256) src_count = 256;
        const unsigned char *src_colors = (const unsigned char *)pal->data_p;

        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if ((int)img->palnum != pal_idx) continue;
            out->remapped_images++;
            if (ppp_limit > 0 && (int)pal->numc <= ppp_limit &&
                final_count > ppp_limit)
                out->ppp_warning_images++;

            if (!img->data_p || img->w == 0 || img->h == 0) continue;
            int stride = (img->w + 3) & ~3;
            const unsigned char *pixels = (const unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char ci = pixels[y * stride + x];
                    if (ci == 0) continue;
                    out->affected_pixels++;

                    if ((int)ci >= src_count) {
                        out->invalid_pixels++;
                        out->transparent_drift_pixels++;
                        continue;
                    }

                    unsigned char mapped = remap[ci];
                    if (mapped == 0 || (int)mapped >= final_count) {
                        out->transparent_drift_pixels++;
                        continue;
                    }

                    unsigned short sw = (unsigned short)(src_colors[ci * 2] |
                                                         (src_colors[ci * 2 + 1] << 8));
                    unsigned short dw = MergedSlotWord(target_colors, base_count,
                                                       out->added_words, mapped);
                    int dist = PaletteColorDistance5(sw, dw);
                    out->total_dist += dist;
                    if (dist == 0) {
                        out->exact_pixels++;
                    } else {
                        out->color_drift_pixels++;
                        if (dist > out->max_dist) {
                            out->max_dist = dist;
                            out->max_src_slot = (int)ci;
                            out->max_dst_slot = (int)mapped;
                            snprintf(out->max_palette, sizeof(out->max_palette), "%.9s", pal->n_s);
                            snprintf(out->max_image, sizeof(out->max_image), "%.15s", img->n_s);
                        }
                    }
                }
            }
        }
    }

    return out->source_palettes > 0;
}

/* Merge marked palettes into the selected palette.
   For each marked palette, each color is remapped to the closest match
   in the selected palette (Euclidean distance in 5-bit RGB space).
   All images using the marked palette are remapped and reassigned.
   Finally, the marked palettes are deleted.  Ported from plst_merge. */
static void MergeMarkedPalettes(bool force_quality_merge)
{
    PAL *sel = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!sel || !sel->data_p || sel->numc == 0) return;

    /* Count marked palettes (excluding the selected one) */
    bool any_marked = false;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p)
        if ((p->flags & 1) && p != sel) { any_marked = true; break; }
    if (!any_marked) return;

    PaletteMergeQuality quality = {};
    if (!BuildMarkedPaletteMergeQuality(&quality)) return;
    g_palette_merge_quality = quality;

    /* Always route through the preview/choice dialog: the user picks the merge
       mode (grow vs nearest, perceptual) and sees the result before committing.
       The dialog's Merge button re-enters with force_quality_merge = true. */
    if (!force_quality_merge) {
        g_palette_merge_preview_only = false;
        g_show_palette_merge_quality = true;
        return;
    }

    doc_undo_push();

    /* Clear mark on the selected palette so it survives deletion pass */
    sel->flags &= ~1;

    /* Grow the target with the planned additions before remapping, so exact
       source colors land on real target slots (zero drift). PoolAlloc is
       calloc, so realloc on data_p is safe. */
    int base_count = (int)sel->numc;
    if (base_count > 256) base_count = 256;
    if (quality.colors_added > 0) {
        int new_numc = base_count + quality.colors_added;   /* <= 256 */
        unsigned char *nd =
            (unsigned char *)realloc(sel->data_p, (size_t)new_numc * 2);
        if (nd) {
            sel->data_p = nd;
            for (int j = 0; j < quality.colors_added; j++) {
                unsigned short w = quality.added_words[j];
                nd[(base_count + j) * 2 + 0] = (unsigned char)(w & 0xFF);
                nd[(base_count + j) * 2 + 1] = (unsigned char)((w >> 8) & 0xFF);
            }
            sel->numc = (unsigned short)new_numc;
        }
    }

    /* Phase 1: build remap and remap images for each marked palette. The target
       now contains the appended colors, so a plain nearest search over it gives
       exact matches for added colors and approximations for the rest. */
    PAL *pal = (PAL *)g_doc->pal_p;
    while (pal) {
        if (!(pal->flags & 1) || pal == sel || !pal->data_p || pal->numc == 0) {
            pal = (PAL *)pal->nxt_p;
            continue;
        }

        unsigned short  src_numc   = pal->numc;

        unsigned char remap[256] = {0};
        BuildPaletteMergeRemap(pal, sel, (int)sel->numc, NULL, 0,
                               quality.opt_perceptual, remap);

        /* Find this palette's index in the linked list */
        int pal_idx = 0;
        for (PAL *q = (PAL *)g_doc->pal_p; q && q != pal; q = (PAL *)q->nxt_p) pal_idx++;

        /* Remap all images that use this palette */
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if (img->palnum != pal_idx) continue;
            img->palnum = (unsigned short)g_doc->plselected;

            if (!img->data_p || img->w == 0 || img->h == 0) continue;
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            int src_count = (int)src_numc;
            if (src_count > 256) src_count = 256;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *px = pixels + y * stride + x;
                    if (*px != 0) *px = ((int)*px < src_count) ? remap[*px] : 0;
                }
            }
        }

        pal = (PAL *)pal->nxt_p;
    }

    /* Phase 2: delete marked palettes and fix up image palette indices */
    PAL *prev = NULL;
    PAL *cur  = (PAL *)g_doc->pal_p;
    int del_idx = 0;

    while (cur) {
        if (cur->flags & 1) {
            PAL *to_del = cur;
            /* Unlink */
            if (prev) prev->nxt_p = cur->nxt_p;
            else g_doc->pal_p = cur->nxt_p;
            cur = (PAL *)cur->nxt_p;
            g_doc->palcnt--;

            /* Any image pointing to palette index > del_idx shifts down */
            for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > del_idx) img->palnum--;
            }
            /* g_doc->plselected also shifts if the deleted palette was before it */
            if ((int)g_doc->plselected > del_idx) g_doc->plselected--;

            if (to_del->data_p) free(to_del->data_p);
            free(to_del);
            /* del_idx stays the same: next palette slid into this position */
        } else {
            prev = cur;
            cur = (PAL *)cur->nxt_p;
            del_idx++;
        }
    }

    /* Fix selection if it went out of bounds */
    if ((unsigned)g_doc->plselected >= g_doc->palcnt)
        g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;

    char added_note[48] = "";
    if (quality.colors_added > 0)
        snprintf(added_note, sizeof(added_note), " (+%d color%s)",
                 quality.colors_added, quality.colors_added == 1 ? "" : "s");

    int drift_pixels = quality.color_drift_pixels +
                       quality.transparent_drift_pixels;
    if (drift_pixels > 0) {
        double max_drift = sqrt((double)quality.max_dist);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Merged %d palette%s%s; quality drift on %d/%d pixel%s (max %.1f).",
                 quality.source_palettes, quality.source_palettes == 1 ? "" : "s",
                 added_note, drift_pixels, quality.affected_pixels,
                 drift_pixels == 1 ? "" : "s", max_drift);
    } else if (quality.ppp_warning_images > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Merged %d palette%s%s; no visual drift, PPP warning on %d image%s.",
                 quality.source_palettes, quality.source_palettes == 1 ? "" : "s",
                 added_note, quality.ppp_warning_images,
                 quality.ppp_warning_images == 1 ? "" : "s");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Merged %d palette%s%s; quality check OK (no visual drift).",
                 quality.source_palettes, quality.source_palettes == 1 ? "" : "s",
                 added_note);
    }
    g_restore_msg_timer = 5.0f;
}

static void OpenPaletteMergePreview(void)
{
    PaletteMergeQuality quality = {};
    if (!BuildMarkedPaletteMergeQuality(&quality)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark one source palette, then select the target palette.");
        g_restore_msg_timer = 4.0f;
        return;
    }
    g_palette_merge_quality = quality;
    g_palette_merge_preview_only = true;
    g_show_palette_merge_quality = true;
}

static void DrawPaletteMergeMappingPreview(const PaletteMergeQuality &q)
{
    PAL *target = get_pal(q.target_idx);
    if (!target || !target->data_p) return;
    int base_count = q.target_base_numc;
    if (base_count > 256) base_count = 256;
    int final_count = base_count + q.colors_added;       /* after growth */
    const unsigned char *target_data = (const unsigned char *)target->data_p;

    ImGui::TextDisabled("Swatch top = source color, bottom = mapped target color "
                        "(green border = added as a new color).");
    ImGui::BeginChild("##pal_merge_preview", ImVec2(560, 210), true);
    int pal_idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, pal_idx++) {
        if (!(pal->flags & 1) || pal_idx == q.target_idx ||
            !pal->data_p || pal->numc <= 1)
            continue;

        ImGui::Text("%.9s -> %.9s", pal->n_s, q.target_name);
        unsigned char remap[256];
        BuildPaletteMergeRemap(pal, target, base_count, q.added_words,
                               q.colors_added, q.opt_perceptual, remap);
        const unsigned char *src_data = (const unsigned char *)pal->data_p;
        int src_count = (int)pal->numc;
        if (src_count > 256) src_count = 256;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 base = ImGui::GetCursorScreenPos();
        const float sw = 14.0f;
        const float gap = 2.0f;
        const int cols = 16;
        int shown = src_count - 1;
        int rows = (shown + cols - 1) / cols;
        if (rows < 1) rows = 1;

        for (int si = 1; si < src_count; si++) {
            int k = si - 1;
            int row = k / cols;
            int col = k % cols;
            ImVec2 p0(base.x + col * (sw + gap), base.y + row * (sw + gap));
            ImVec2 p1(p0.x + sw, p0.y + sw);

            unsigned char sr = 0, sg = 0, sb = 0;
            pal_word_to_rgb8(src_data + si * 2, &sr, &sg, &sb);
            unsigned char mapped = remap[si];
            bool valid = mapped > 0 && (int)mapped < final_count;
            bool is_added = valid && (int)mapped >= base_count;

            unsigned short src_word =
                (unsigned short)(src_data[si * 2] | (src_data[si * 2 + 1] << 8));
            unsigned short dst_word = valid
                ? MergedSlotWord(target_data, base_count, q.added_words, mapped)
                : 0;
            unsigned char dst_bytes[2] = {
                (unsigned char)(dst_word & 0xFF), (unsigned char)(dst_word >> 8) };
            unsigned char dr = 0, dg = 0, db = 0;
            if (valid) pal_word_to_rgb8(dst_bytes, &dr, &dg, &db);

            dl->AddRectFilled(p0, p1, IM_COL32(sr, sg, sb, 255));
            dl->AddRectFilled(ImVec2(p0.x, p0.y + sw - 4.0f), p1,
                              valid ? IM_COL32(dr, dg, db, 255)
                                    : IM_COL32(255, 0, 0, 255));

            int dist = valid ? PaletteColorDistance5(src_word, dst_word) : 9999;
            ImU32 border = is_added  ? IM_COL32(80, 200, 120, 255)
                          : dist == 0 ? IM_COL32(70, 90, 110, 220)
                          : dist < 36 ? IM_COL32(255, 190, 60, 255)
                                      : IM_COL32(255, 80, 80, 255);
            dl->AddRect(p0, p1, border);

            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(pal_idx * 1000 + si);
            ImGui::InvisibleButton("##map", ImVec2(sw, sw));
            if (ImGui::IsItemHovered()) {
                if (is_added) {
                    ImGui::SetTooltip("%.9s #%d -> %.9s #%d (added, exact)",
                                      pal->n_s, si, q.target_name, (int)mapped);
                } else if (valid) {
                    ImGui::SetTooltip("%.9s #%d -> %.9s #%d\nRGB drift %.2f",
                                      pal->n_s, si, q.target_name, (int)mapped,
                                      sqrt((double)dist));
                } else {
                    ImGui::SetTooltip("%.9s #%d would map to transparent/invalid",
                                      pal->n_s, si);
                }
            }
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(cols * (sw + gap), rows * (sw + gap)));
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

static bool PalettesAreIdentical(const PAL *a, const PAL *b)
{
    if (!a || !b || !a->data_p || !b->data_p) return false;
    if (a->bitspix != b->bitspix || a->numc != b->numc) return false;
    if (a->numc == 0) return false;
    return memcmp(a->data_p, b->data_p, (size_t)a->numc * 2) == 0;
}

static int MergeDuplicatePalettes(void)
{
    int n_pals = count_pals();
    if (n_pals < 2) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No duplicate palettes found.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<PAL *> pals;
    std::vector<int> duplicate_to;
    pals.reserve(n_pals);
    duplicate_to.assign(n_pals, -1);
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p)
        pals.push_back(p);

    int duplicates = 0;
    for (int i = 0; i < (int)pals.size(); i++) {
        for (int j = 0; j < i; j++) {
            if (PalettesAreIdentical(pals[i], pals[j])) {
                duplicate_to[i] = j;
                duplicates++;
                break;
            }
        }
    }

    if (duplicates == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No duplicate palettes found.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    doc_undo_push();

    int remapped_images = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        int pal_idx = (int)img->palnum;
        if (pal_idx >= 0 && pal_idx < (int)duplicate_to.size() && duplicate_to[pal_idx] >= 0) {
            img->palnum = (unsigned short)duplicate_to[pal_idx];
            remapped_images++;
        }
    }
    if (g_doc->plselected >= 0 && g_doc->plselected < (int)duplicate_to.size() &&
        duplicate_to[g_doc->plselected] >= 0)
        g_doc->plselected = duplicate_to[g_doc->plselected];

    PAL *prev = NULL;
    PAL *cur = (PAL *)g_doc->pal_p;
    int original_idx = 0;
    int current_idx = 0;
    while (cur) {
        if (original_idx < (int)duplicate_to.size() && duplicate_to[original_idx] >= 0) {
            PAL *to_del = cur;
            if (prev) prev->nxt_p = cur->nxt_p;
            else g_doc->pal_p = cur->nxt_p;
            cur = (PAL *)cur->nxt_p;
            g_doc->palcnt--;

            for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > current_idx) img->palnum--;
            }
            if ((int)g_doc->plselected > current_idx) g_doc->plselected--;

            FreePal(to_del);
        } else {
            prev = cur;
            cur = (PAL *)cur->nxt_p;
            current_idx++;
        }
        original_idx++;
    }

    if ((unsigned)g_doc->plselected >= g_doc->palcnt)
        g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;
    ApplyPalette(g_doc->plselected);
    g_img_tex_idx = -2;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Merged %d duplicate palette%s into first match%s; quality check OK.",
             duplicates, duplicates == 1 ? "" : "s",
             remapped_images ? "" : " (no sprites remapped)");
    g_restore_msg_timer = 4.0f;
    return duplicates;
}

static int FindMarkedPaletteExcept(int except_idx)
{
    int idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, idx++) {
        if (idx != except_idx && (pal->flags & 1))
            return idx;
    }
    return -1;
}

static int InheritSelectedPaletteFromMarked(void)
{
    int target_idx = g_doc->plselected;
    PAL *target = (target_idx >= 0) ? get_pal(target_idx) : NULL;
    int source_idx = FindMarkedPaletteExcept(target_idx);
    PAL *source = (source_idx >= 0) ? get_pal(source_idx) : NULL;

    if (!target || !target->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a target palette first.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }
    if (!source || !source->data_p || source->numc <= 1) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark one source palette, then select the palette to inherit into.");
        g_restore_msg_timer = 5.0f;
        return 0;
    }

    commit_palette_adjustments();

    unsigned char remap[256] = {0};
    const unsigned char *target_data = (const unsigned char *)target->data_p;
    int target_colors = target->numc;
    if (target_colors > 256) target_colors = 256;
    for (int i = 1; i < target_colors; i++) {
        unsigned short w = (unsigned short)(target_data[i * 2] | (target_data[i * 2 + 1] << 8));
        remap[i] = (unsigned char)FindNearestPaletteSlot(source, w);
    }

    int source_colors = source->numc;
    if (source_colors > 256) source_colors = 256;
    size_t source_bytes = (size_t)source_colors * 2;
    void *new_data = malloc(source_bytes);
    if (!new_data) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not allocate inherited palette data.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }
    memcpy(new_data, source->data_p, source_bytes);

    doc_undo_push();

    int pixels_changed = 0;
    int images_touched = 0;
    int img_idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        if ((int)img->palnum != target_idx) continue;
        images_touched++;
        if (img->data_p && img->w > 0 && img->h > 0) {
            int stride = (img->w + 3) & ~3;
            unsigned char *pix = (unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *p = pix + y * stride + x;
                    unsigned char mapped = (*p < target_colors) ? remap[*p] : 0;
                    if (*p != 0 && *p != mapped) {
                        *p = mapped;
                        pixels_changed++;
                    }
                }
            }
        }
        InvalidateThumb(img_idx);
    }

    free(target->data_p);
    target->data_p = new_data;
    target->numc = (unsigned short)source_colors;
    target->bitspix = source->bitspix;
    target->pad = source->pad;

    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    if (g_sel_color >= (int)target->numc)
        g_sel_color = target->numc > 1 ? (int)target->numc - 1 : 0;
    ApplyPalette(target_idx);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Inherited %.9s into %.9s: %d image%s, %d pixel%s remapped.",
             source->n_s, target->n_s,
             images_touched, images_touched == 1 ? "" : "s",
             pixels_changed, pixels_changed == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    return pixels_changed;
}

static void OpenRenameImage(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;
    g_rename_target = RenameTarget::Image;
    g_rename_idx = g_doc->ilselected;
    strncpy(g_rename_buf, img->n_s, 15);
    g_rename_buf[15] = '\0';
    g_show_rename = true;
}

static void OpenRenamePalette(int idx)
{
    PAL *pal = get_pal(idx);
    if (!pal) return;
    g_rename_target = RenameTarget::Palette;
    g_rename_idx = idx;
    strncpy(g_rename_buf, pal->n_s, 9);
    g_rename_buf[9] = '\0';
    g_show_rename = true;
}

static void OpenRenameMarkedImages(void)
{
    /* Find first marked image to seed the buffer with its name. */
    IMG *first = NULL;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (p->flags & 1) { first = p; break; }
    }
    if (!first) return;
    g_rename_target = RenameTarget::MarkedImages;
    g_rename_idx = -1;
    strncpy(g_rename_buf, first->n_s, 12);
    g_rename_buf[12] = '\0';
    g_rename_tail_existing = false;
    g_rename_start_number = 1;
    g_show_rename = true;
}

static void ApplyMarkedImageRename(const char *base)
{
    if (!base || (!*base && !g_rename_tail_existing)) return;
    doc_undo_push();
    bool prepend = (base[0] == '+') && !g_rename_tail_existing;
    const char *core = prepend ? base + 1 : base;
    int n = g_rename_start_number;
    if (n < 0) n = 0;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        if (!(p->flags & 1)) continue;
        char old[16];
        strncpy(old, p->n_s, 15); old[15] = '\0';
        if (g_rename_tail_existing) {
            snprintf(p->n_s, sizeof(p->n_s), "%s%s%d", old, core, n);
        } else if (prepend) {
            snprintf(p->n_s, sizeof(p->n_s), "%s%s", core, old);
        } else {
            snprintf(p->n_s, sizeof(p->n_s), "%s%d", core, n);
        }
        n++;
        p->n_s[15] = '\0';
    }
}

/* Declared in ui_internal.h so mark_dirty() (now shared) can call it. */
void InvalidatePaletteUsage(void)
{
    g_palette_usage_serial++;
    if (g_palette_usage_serial == 0) {
        g_palette_usage_serial = 1;
        g_palette_usage_built_serial = 0;
    }
}

static void BuildSelectedPaletteUsage(void)
{
    int pal_idx = g_doc ? g_doc->plselected : -1;
    PAL *pal = (pal_idx >= 0) ? get_pal(pal_idx) : NULL;
    int pal_numc = (pal && pal->data_p) ? (int)pal->numc : 0;
    if (pal_numc < 0) pal_numc = 0;
    if (pal_numc > 256) pal_numc = 256;

    bool needs_rebuild =
        g_palette_usage_doc != g_doc ||
        g_palette_usage_img_head != (g_doc ? g_doc->img_p : NULL) ||
        g_palette_usage_imgcnt_seen != (g_doc ? g_doc->imgcnt : 0) ||
        g_palette_usage_pal_idx != pal_idx ||
        g_palette_usage_pal_numc != pal_numc ||
        g_palette_usage_built_serial != g_palette_usage_serial;
    if (!needs_rebuild) return;

    memset(g_palette_usage_counts, 0, sizeof(g_palette_usage_counts));
    g_palette_usage_doc = g_doc;
    g_palette_usage_img_head = g_doc ? g_doc->img_p : NULL;
    g_palette_usage_imgcnt_seen = g_doc ? g_doc->imgcnt : 0;
    g_palette_usage_pal_idx = pal_idx;
    g_palette_usage_pal_numc = pal_numc;
    g_palette_usage_img_count = 0;
    g_palette_usage_used_colors = 0;
    g_palette_usage_unused_colors = 0;
    g_palette_usage_low_colors = 0;
    g_palette_usage_built_serial = g_palette_usage_serial;

    if (!g_doc || pal_idx < 0 || !pal || !pal->data_p || pal_numc <= 0)
        return;

    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if ((int)img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0)
            continue;

        g_palette_usage_img_count++;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pixels = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            const unsigned char *row = pixels + y * stride;
            for (int x = 0; x < img->w; x++)
                g_palette_usage_counts[row[x]]++;
        }
    }

    if (g_palette_usage_low_threshold < 1) g_palette_usage_low_threshold = 1;
    for (int i = 1; i < pal_numc; i++) {
        unsigned long long count = g_palette_usage_counts[i];
        if (count == 0) {
            g_palette_usage_unused_colors++;
        } else {
            g_palette_usage_used_colors++;
            if (count <= (unsigned long long)g_palette_usage_low_threshold)
                g_palette_usage_low_colors++;
        }
    }
}

static int FindNearestUsedPaletteSlotForUsage(int color_idx, int *dist_out)
{
    if (dist_out) *dist_out = 0;
    BuildSelectedPaletteUsage();

    PAL *pal = (g_palette_usage_pal_idx >= 0) ? get_pal(g_palette_usage_pal_idx) : NULL;
    if (!pal || !pal->data_p ||
        color_idx <= 0 || color_idx >= g_palette_usage_pal_numc)
        return -1;

    const unsigned char *pd = (const unsigned char *)pal->data_p;
    unsigned short target =
        (unsigned short)(pd[color_idx * 2] | (pd[color_idx * 2 + 1] << 8));
    int best = -1;
    int best_dist = 0x7FFFFFFF;

    for (int i = 1; i < g_palette_usage_pal_numc; i++) {
        if (i == color_idx || g_palette_usage_counts[i] == 0)
            continue;
        unsigned short w = (unsigned short)(pd[i * 2] | (pd[i * 2 + 1] << 8));
        int dist = PaletteColorDistance5(target, w);
        if (dist < best_dist) {
            best = i;
            best_dist = dist;
            if (dist == 0) break;
        }
    }

    if (dist_out && best >= 0) *dist_out = best_dist;
    return best;
}

static void CalculatePaletteHistogram()
{
    memset(g_histogram_data, 0, sizeof(g_histogram_data));
    g_histogram_max = 0.0f;
    g_histogram_img_count = 0;

    BuildSelectedPaletteUsage();
    g_histogram_img_count = g_palette_usage_img_count;
    for (int i = 0; i < 256; i++)
        g_histogram_data[i] = (float)g_palette_usage_counts[i];

    /* Find max (skipping index 0, so transparent background doesn't dwarf
       the chart). */
    for (int i = 1; i < 256; i++) {
        if (g_histogram_data[i] > g_histogram_max) {
            g_histogram_max = g_histogram_data[i];
        }
    }
    if (g_histogram_max == 0.0f) g_histogram_max = 1.0f;
}

struct PaletteCleanupResult {
    int removed;
    int moved;
    int sorted;
    bool changed;
};

struct PaletteSortColor {
    int old_idx;
    int r, g, b;
    int luma;
    int sat;
    int hue;
    int family;
};

static int palette_sort_hue(int r, int g, int b)
{
    int maxv = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int minv = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int delta = maxv - minv;
    if (delta == 0) return 0;

    int h;
    if (maxv == r) {
        h = 256 * (g - b) / delta;
        if (h < 0) h += 1536;
    } else if (maxv == g) {
        h = 512 + 256 * (b - r) / delta;
    } else {
        h = 1024 + 256 * (r - g) / delta;
    }
    if (h < 0) h += 1536;
    if (h >= 1536) h -= 1536;
    return h;
}

static std::vector<int> BuildGradientPaletteOrder(PAL *pal, const bool used[256])
{
    std::vector<PaletteSortColor> colors;
    if (!pal || !pal->data_p) return {};

    int max_colors = pal->numc;
    if (max_colors > 256) max_colors = 256;
    const unsigned char *pd = (const unsigned char *)pal->data_p;

    for (int i = 1; i < max_colors; i++) {
        if (!used[i]) continue;
        unsigned char r, g, b;
        pal_word_to_rgb8(pd + i * 2, &r, &g, &b);
        PaletteSortColor c;
        c.old_idx = i;
        c.r = r;
        c.g = g;
        c.b = b;
        c.luma = c.r * 54 + c.g * 183 + c.b * 19;
        int maxv = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
        int minv = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
        c.sat = maxv - minv;
        c.hue = palette_sort_hue(c.r, c.g, c.b);
        c.family = (c.sat <= 18) ? 0 : 1 + ((c.hue + 32) % 1536) / 64;
        colors.push_back(c);
    }

    std::sort(colors.begin(), colors.end(), [](const PaletteSortColor& a, const PaletteSortColor& b) {
        if (a.luma != b.luma) return a.luma < b.luma;
        if (a.sat != b.sat) return a.sat < b.sat;
        if (a.family != b.family) return a.family < b.family;
        if (a.hue != b.hue) return a.hue < b.hue;
        if (a.r != b.r) return a.r < b.r;
        if (a.g != b.g) return a.g < b.g;
        if (a.b != b.b) return a.b < b.b;
        return a.old_idx < b.old_idx;
    });

    std::vector<int> order;
    order.reserve(colors.size());
    for (const PaletteSortColor& c : colors)
        order.push_back(c.old_idx);
    return order;
}

static int BuildPaletteUsedMask(int pal_idx, int old_numc, bool used[256])
{
    memset(used, 0, sizeof(bool) * 256);
    used[0] = true; /* Transparent index 0 is always preserved. */
    if (old_numc > 256) old_numc = 256;

    int referenced_pixels = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        unsigned short stride = (img->w + 3) & ~3;
        unsigned char *pixels = (unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char idx = pixels[y * stride + x];
                if (idx < old_numc) used[idx] = true;
                referenced_pixels++;
            }
        }
    }

    /* If this palette is only being previewed and no sprite references it,
       sort the palette itself instead of producing a blank cleaned copy. */
    if (referenced_pixels == 0) {
        for (int i = 1; i < old_numc; i++) used[i] = true;
    }
    return referenced_pixels;
}

static PaletteCleanupResult DeleteUnusedPaletteColors()
{
    PaletteCleanupResult result = {0, 0, 0, false};
    if (g_doc->plselected < 0) return result;
    PAL *pal = get_pal(g_doc->plselected);
    if (!pal || !pal->data_p) return result;

    bool used[256] = {false};
    used[0] = true; /* Transparent index 0 is always preserved */
    int old_numc = pal->numc;
    if (old_numc > 256) old_numc = 256;

    /* 1. Find used colors across all images sharing this palette. */
    BuildPaletteUsedMask(g_doc->plselected, old_numc, used);

    /* 2. Reorder used colors into visible brightness ramps. Brightness is
       the primary key so near dark variants stay adjacent even when one has
       a slight blue tint, e.g. #060606 next to #070707. */
    std::vector<int> order = BuildGradientPaletteOrder(pal, used);
    int new_numc = (int)order.size() + 1;
    result.sorted = (int)order.size();

    result.removed = old_numc - new_numc;
    if (result.removed < 0) result.removed = 0;
    for (int i = 0; i < (int)order.size(); i++) {
        if (order[i] != i + 1) result.moved++;
    }

    if (result.removed == 0 && result.moved == 0) return result;
    result.changed = true;
    doc_undo_push();

    /* 3. Build remap table */
    unsigned char remap[256] = {0};
    for (int i = 0; i < (int)order.size(); i++) {
        remap[order[i]] = (unsigned char)(i + 1);
    }
    if (g_sel_color > 0 && g_sel_color < 256)
        g_sel_color = remap[g_sel_color] ? remap[g_sel_color] : 0;

    /* 4. Rewrite palette colors in the new order. */
    unsigned char *colors = (unsigned char *)pal->data_p;
    unsigned char old_colors[512] = {0};
    memcpy(old_colors, colors, (size_t)old_numc * 2);
    for (int i = 0; i < (int)order.size(); i++) {
        int old_idx = order[i];
        int new_idx = i + 1;
        colors[new_idx * 2 + 0] = old_colors[old_idx * 2 + 0];
        colors[new_idx * 2 + 1] = old_colors[old_idx * 2 + 1];
    }

    /* Clear remaining colors to black */
    for (int i = new_numc; i < old_numc; i++) {
        colors[i * 2 + 0] = 0;
        colors[i * 2 + 1] = 0;
    }

    pal->numc = (unsigned short)new_numc;

    /* 5. Remap pixel indices in all affected images */
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if (img->palnum == g_doc->plselected && img->data_p && img->w > 0 && img->h > 0) {
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *idx = &pixels[y * stride + x];
                    *idx = remap[*idx];
                }
            }
        }
        img = (IMG *)img->nxt_p;
    }

    /* 6. Update global SDL palette so the UI reflects changes instantly */
    ApplyPalette(g_doc->plselected);

    /* 7. Force canvas texture rebuild */
    g_img_tex_idx = -2;
    mark_dirty();
    return result;
}

static void CreateCleanedPaletteCopy(void)
{
    commit_palette_adjustments();

    int src_idx = g_doc->plselected;
    PAL *src = (src_idx >= 0) ? get_pal(src_idx) : NULL;
    if (!src || !src->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No palette selected to clean-copy.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int old_numc = src->numc;
    if (old_numc > 256) old_numc = 256;
    if (old_numc <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Selected palette has no colors.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    bool used[256];
    int referenced_pixels = BuildPaletteUsedMask(src_idx, old_numc, used);
    std::vector<int> order = BuildGradientPaletteOrder(src, used);
    int new_numc = (int)order.size() + 1;
    if (new_numc <= 1) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No active colors to clean-copy.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int moved = 0;
    for (int i = 0; i < (int)order.size(); i++)
        if (order[i] != i + 1) moved++;

    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = src->flags;
    pal->bitspix = src->bitspix;
    pal->numc    = (unsigned short)new_numc;
    pal->pad     = 0;
    make_unique_pal_name(pal->n_s);

    unsigned char *buf = (unsigned char *)PoolAlloc((size_t)new_numc * 2);
    if (!buf) return;
    pal->data_p = buf;

    const unsigned char *old_colors = (const unsigned char *)src->data_p;
    buf[0] = old_colors[0];
    buf[1] = old_colors[1];
    for (int i = 0; i < (int)order.size(); i++) {
        int old_idx = order[i];
        int new_idx = i + 1;
        buf[new_idx * 2 + 0] = old_colors[old_idx * 2 + 0];
        buf[new_idx * 2 + 1] = old_colors[old_idx * 2 + 1];
    }

    g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    save_palette_baseline();
    g_img_tex_idx = -2;
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Created clean palette copy: sorted %d colors, moved %d%s.",
             (int)order.size(), moved,
             referenced_pixels == 0 ? " (palette-only)" : "");
    g_restore_msg_timer = 5.0f;
}

static void CleanupSelectedPalette(void)
{
    commit_palette_adjustments();
    PaletteCleanupResult r = DeleteUnusedPaletteColors();
    if (r.changed) {
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        save_palette_baseline();
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Cleaned %d unused, sorted %d active color%s, moved %d.",
                 r.removed, r.sorted, r.sorted == 1 ? "" : "s", r.moved);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Palette already sorted (%d active color%s).",
                 r.sorted, r.sorted == 1 ? "" : "s");
    }
    g_restore_msg_timer = 4.0f;
}

struct PaletteReduceColor {
    int old_idx;
    unsigned char r, g, b;
    double weight;
};

struct PaletteReduceBox {
    std::vector<int> colors;
    int rmin, rmax, gmin, gmax, bmin, bmax;
    double weight;
};

struct PaletteReduceRep {
    unsigned char r, g, b;
    int old_min;
    int luma;
    int sat;
    int hue;
    int family;
};

struct PaletteReductionPlan {
    bool valid;
    int pal_idx;
    int target_bpp;
    int target_numc;
    int old_numc;
    int new_numc;
    int input_colors;
    int output_colors;
    int active_colors;
    int images;
    int pixels;
    int pixels_changed;
    int colors_merged;
    bool quantized;
    unsigned char remap[256];
    unsigned char data[512];
    char error[128];
};

static int palette_reduce_target_numc(int bpp)
{
    if (bpp < 4) bpp = 4;
    if (bpp > 8) bpp = 8;
    return 1 << bpp;
}

static void palette_reduce_update_rep_fields(PaletteReduceRep *rep)
{
    if (!rep) return;
    rep->luma = rep->r * 54 + rep->g * 183 + rep->b * 19;
    int maxv = rep->r > rep->g ? (rep->r > rep->b ? rep->r : rep->b) : (rep->g > rep->b ? rep->g : rep->b);
    int minv = rep->r < rep->g ? (rep->r < rep->b ? rep->r : rep->b) : (rep->g < rep->b ? rep->g : rep->b);
    rep->sat = maxv - minv;
    rep->hue = palette_sort_hue(rep->r, rep->g, rep->b);
    rep->family = (rep->sat <= 18) ? 0 : 1 + ((rep->hue + 32) % 1536) / 64;
}

static void palette_reduce_compute_box(PaletteReduceBox *box,
                                       const std::vector<PaletteReduceColor> &colors)
{
    if (!box || box->colors.empty()) return;
    const PaletteReduceColor &first = colors[box->colors[0]];
    box->rmin = box->rmax = first.r;
    box->gmin = box->gmax = first.g;
    box->bmin = box->bmax = first.b;
    box->weight = 0.0;

    for (int ci : box->colors) {
        const PaletteReduceColor &c = colors[ci];
        if (c.r < box->rmin) box->rmin = c.r;
        if (c.r > box->rmax) box->rmax = c.r;
        if (c.g < box->gmin) box->gmin = c.g;
        if (c.g > box->gmax) box->gmax = c.g;
        if (c.b < box->bmin) box->bmin = c.b;
        if (c.b > box->bmax) box->bmax = c.b;
        box->weight += c.weight;
    }
}

static double palette_reduce_box_score(const PaletteReduceBox &box)
{
    if (box.colors.size() <= 1) return -1.0;
    int rr = box.rmax - box.rmin;
    int gr = box.gmax - box.gmin;
    int br = box.bmax - box.bmin;
    int range = rr > gr ? (rr > br ? rr : br) : (gr > br ? gr : br);
    if (range <= 0) return -1.0;
    return (double)range * (box.weight > 1.0 ? box.weight : 1.0);
}

static bool palette_reduce_split_box(std::vector<PaletteReduceBox> &boxes,
                                     const std::vector<PaletteReduceColor> &colors,
                                     int box_idx)
{
    if (box_idx < 0 || box_idx >= (int)boxes.size()) return false;
    PaletteReduceBox box = boxes[box_idx];
    if (box.colors.size() <= 1) return false;

    int rr = box.rmax - box.rmin;
    int gr = box.gmax - box.gmin;
    int br = box.bmax - box.bmin;
    int channel = 0;
    if (gr >= rr && gr >= br) channel = 1;
    else if (br >= rr && br >= gr) channel = 2;

    std::stable_sort(box.colors.begin(), box.colors.end(),
        [&](int ai, int bi) {
            const PaletteReduceColor &a = colors[ai];
            const PaletteReduceColor &b = colors[bi];
            int av = channel == 0 ? a.r : (channel == 1 ? a.g : a.b);
            int bv = channel == 0 ? b.r : (channel == 1 ? b.g : b.b);
            if (av != bv) return av < bv;
            return a.old_idx < b.old_idx;
        });

    double half = box.weight * 0.5;
    double acc = 0.0;
    int split = (int)box.colors.size() / 2;
    for (int i = 0; i < (int)box.colors.size(); i++) {
        acc += colors[box.colors[i]].weight;
        if (acc >= half) {
            split = i + 1;
            break;
        }
    }
    if (split <= 0) split = 1;
    if (split >= (int)box.colors.size()) split = (int)box.colors.size() - 1;

    PaletteReduceBox a = {};
    PaletteReduceBox b = {};
    a.colors.assign(box.colors.begin(), box.colors.begin() + split);
    b.colors.assign(box.colors.begin() + split, box.colors.end());
    palette_reduce_compute_box(&a, colors);
    palette_reduce_compute_box(&b, colors);

    boxes[box_idx] = a;
    boxes.push_back(b);
    return true;
}

static void palette_reduce_make_reps(const std::vector<PaletteReduceBox> &boxes,
                                     const std::vector<PaletteReduceColor> &colors,
                                     std::vector<PaletteReduceRep> &reps)
{
    reps.clear();
    reps.reserve(boxes.size());
    for (const PaletteReduceBox &box : boxes) {
        if (box.colors.empty()) continue;
        double rs = 0.0, gs = 0.0, bs = 0.0, ws = 0.0;
        int old_min = 999;
        for (int ci : box.colors) {
            const PaletteReduceColor &c = colors[ci];
            double w = c.weight > 0.0 ? c.weight : 1.0;
            rs += (double)c.r * w;
            gs += (double)c.g * w;
            bs += (double)c.b * w;
            ws += w;
            if (c.old_idx < old_min) old_min = c.old_idx;
        }
        if (ws <= 0.0) ws = 1.0;
        int r = (int)(rs / ws + 0.5);
        int g = (int)(gs / ws + 0.5);
        int b = (int)(bs / ws + 0.5);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;

        PaletteReduceRep rep = {};
        rep.r = (unsigned char)r;
        rep.g = (unsigned char)g;
        rep.b = (unsigned char)b;
        rep.old_min = old_min;
        palette_reduce_update_rep_fields(&rep);
        reps.push_back(rep);
    }

    std::sort(reps.begin(), reps.end(), [](const PaletteReduceRep &a, const PaletteReduceRep &b) {
        if (a.luma != b.luma) return a.luma < b.luma;
        if (a.sat != b.sat) return a.sat < b.sat;
        if (a.family != b.family) return a.family < b.family;
        if (a.hue != b.hue) return a.hue < b.hue;
        return a.old_min < b.old_min;
    });
}

static int palette_reduce_nearest_rep(const std::vector<PaletteReduceRep> &reps,
                                      unsigned char r, unsigned char g, unsigned char b)
{
    int best = 0;
    int best_d = 0x7fffffff;
    for (int i = 0; i < (int)reps.size(); i++) {
        int dr = (int)r - (int)reps[i].r;
        int dg = (int)g - (int)reps[i].g;
        int db = (int)b - (int)reps[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void palette_reduce_refine_reps(std::vector<PaletteReduceRep> &reps,
                                       const std::vector<PaletteReduceColor> &colors)
{
    int n = (int)reps.size();
    if (n <= 0 || colors.empty()) return;

    for (int iter = 0; iter < 8; iter++) {
        std::vector<double> rs(n, 0.0), gs(n, 0.0), bs(n, 0.0), ws(n, 0.0);
        std::vector<int> old_min(n, 999);

        for (const PaletteReduceColor &c : colors) {
            int best = palette_reduce_nearest_rep(reps, c.r, c.g, c.b);
            double w = c.weight > 0.0 ? c.weight : 1.0;
            rs[best] += (double)c.r * w;
            gs[best] += (double)c.g * w;
            bs[best] += (double)c.b * w;
            ws[best] += w;
            if (c.old_idx < old_min[best]) old_min[best] = c.old_idx;
        }

        bool changed = false;
        for (int i = 0; i < n; i++) {
            if (ws[i] <= 0.0) continue;
            int r = (int)(rs[i] / ws[i] + 0.5);
            int g = (int)(gs[i] / ws[i] + 0.5);
            int b = (int)(bs[i] / ws[i] + 0.5);
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            if (reps[i].r != (unsigned char)r ||
                reps[i].g != (unsigned char)g ||
                reps[i].b != (unsigned char)b) {
                changed = true;
            }
            reps[i].r = (unsigned char)r;
            reps[i].g = (unsigned char)g;
            reps[i].b = (unsigned char)b;
            if (old_min[i] != 999) reps[i].old_min = old_min[i];
            palette_reduce_update_rep_fields(&reps[i]);
        }
        if (!changed) break;
    }
}

static bool BuildPaletteReductionPlan(int pal_idx, int target_bpp,
                                      PaletteReductionPlan *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (target_bpp < 4) target_bpp = 4;
    if (target_bpp > 8) target_bpp = 8;
    out->pal_idx = pal_idx;
    out->target_bpp = target_bpp;
    out->target_numc = palette_reduce_target_numc(target_bpp);

    for (int i = 0; i < 256; i++) out->remap[i] = 0;

    PAL *pal = get_pal(pal_idx);
    if (!pal || !pal->data_p) {
        snprintf(out->error, sizeof(out->error), "No palette selected.");
        return false;
    }

    int old_numc = (int)pal->numc;
    if (old_numc > 256) old_numc = 256;
    if (old_numc <= 0) {
        snprintf(out->error, sizeof(out->error), "Selected palette has no colors.");
        return false;
    }

    out->old_numc = old_numc;
    out->input_colors = old_numc;
    const unsigned char *src = (const unsigned char *)pal->data_p;

    double usage[256] = {};
    bool active[256] = {};
    active[0] = true;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        out->images++;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pixels = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char idx = pixels[y * stride + x];
                if (idx < old_numc) {
                    usage[idx] += 1.0;
                    active[idx] = true;
                }
                out->pixels++;
            }
        }
    }

    if (out->images == 0) {
        for (int i = 1; i < old_numc; i++) {
            active[i] = true;
            usage[i] = 1.0;
        }
    }

    for (int i = 1; i < old_numc; i++)
        if (active[i]) out->active_colors++;

    if (old_numc <= out->target_numc) {
        memcpy(out->data, src, (size_t)old_numc * 2);
        for (int i = 0; i < old_numc; i++) out->remap[i] = (unsigned char)i;
        out->new_numc = old_numc;
        out->output_colors = old_numc;
        out->colors_merged = 0;
    } else if (out->active_colors <= out->target_numc - 1) {
        out->data[0] = src[0];
        out->data[1] = src[1];
        out->remap[0] = 0;
        int dst_idx = 1;
        for (int i = 1; i < old_numc; i++) {
            if (!active[i]) continue;
            out->remap[i] = (unsigned char)dst_idx;
            out->data[dst_idx * 2 + 0] = src[i * 2 + 0];
            out->data[dst_idx * 2 + 1] = src[i * 2 + 1];
            dst_idx++;
        }
        out->new_numc = dst_idx;
        out->output_colors = out->new_numc;
        out->colors_merged = old_numc - out->new_numc;
    } else {
        std::vector<PaletteReduceColor> colors;
        colors.reserve((size_t)out->active_colors);
        for (int i = 1; i < old_numc; i++) {
            if (!active[i]) continue;
            unsigned char r, g, b;
            pal_word_to_rgb8(src + i * 2, &r, &g, &b);
            PaletteReduceColor c = {};
            c.old_idx = i;
            c.r = r;
            c.g = g;
            c.b = b;
            c.weight = usage[i] > 0.0 ? usage[i] : 1.0;
            colors.push_back(c);
        }

        int target_opaque = out->target_numc - 1;
        std::vector<PaletteReduceBox> boxes;
        PaletteReduceBox root = {};
        for (int i = 0; i < (int)colors.size(); i++) root.colors.push_back(i);
        palette_reduce_compute_box(&root, colors);
        boxes.push_back(root);

        while ((int)boxes.size() < target_opaque) {
            int best = -1;
            double best_score = -1.0;
            for (int i = 0; i < (int)boxes.size(); i++) {
                double score = palette_reduce_box_score(boxes[i]);
                if (score > best_score) {
                    best_score = score;
                    best = i;
                }
            }
            if (best < 0) break;
            if (!palette_reduce_split_box(boxes, colors, best)) break;
        }

        std::vector<PaletteReduceRep> reps;
        palette_reduce_make_reps(boxes, colors, reps);
        palette_reduce_refine_reps(reps, colors);
        if ((int)reps.size() > target_opaque) reps.resize(target_opaque);
        std::sort(reps.begin(), reps.end(), [](const PaletteReduceRep &a, const PaletteReduceRep &b) {
            if (a.luma != b.luma) return a.luma < b.luma;
            if (a.sat != b.sat) return a.sat < b.sat;
            if (a.family != b.family) return a.family < b.family;
            if (a.hue != b.hue) return a.hue < b.hue;
            return a.old_min < b.old_min;
        });

        out->data[0] = src[0];
        out->data[1] = src[1];
        out->remap[0] = 0;
        out->new_numc = (int)reps.size() + 1;
        out->output_colors = out->new_numc;
        out->colors_merged = old_numc - out->new_numc;
        out->quantized = true;

        for (int i = 0; i < (int)reps.size(); i++) {
            rgb8_to_pal_word(reps[i].r, reps[i].g, reps[i].b,
                             out->data + (i + 1) * 2);
        }

        for (int i = 1; i < old_numc; i++) {
            unsigned char r, g, b;
            pal_word_to_rgb8(src + i * 2, &r, &g, &b);
            int best = palette_reduce_nearest_rep(reps, r, g, b);
            out->remap[i] = (unsigned char)(best + 1);
        }
    }

    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pixels = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char idx = pixels[y * stride + x];
                unsigned char mapped = out->remap[idx];
                if (mapped != idx) out->pixels_changed++;
            }
        }
    }

    out->valid = true;
    return true;
}

static int FindPalettePreviewImage(int pal_idx, int prefer_idx)
{
    IMG *prefer = get_img(prefer_idx);
    if (prefer && prefer->palnum == pal_idx && prefer->data_p &&
        prefer->w > 0 && prefer->h > 0)
        return prefer_idx;

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img->palnum == pal_idx && img->data_p && img->w > 0 && img->h > 0)
            return idx;
    }
    return -1;
}

static int StepPalettePreviewImage(int pal_idx, int start_idx, int delta)
{
    int n = count_imgs();
    if (n <= 0) return -1;
    int idx = start_idx;
    if (idx < 0 || idx >= n) idx = delta >= 0 ? -1 : 0;
    for (int step = 0; step < n; step++) {
        idx += delta;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
        IMG *img = get_img(idx);
        if (img && img->palnum == pal_idx && img->data_p &&
            img->w > 0 && img->h > 0)
            return idx;
    }
    return -1;
}

static SDL_Texture *BuildPaletteReductionTexture(const IMG *img, const PAL *pal,
                                                 const PaletteReductionPlan *plan,
                                                 bool reduced)
{
    if (!g_imgui_renderer || !img || !img->data_p || img->w == 0 || img->h == 0)
        return NULL;

    SDL_Texture *tex = SDL_CreateTexture(g_imgui_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, img->w, img->h);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    void *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    const unsigned char *pal_data = reduced ? plan->data : (const unsigned char *)pal->data_p;
    int pal_colors = reduced ? plan->new_numc : (int)pal->numc;
    if (pal_colors > 256) pal_colors = 256;
    int stride = (img->w + 3) & ~3;
    const unsigned char *src_pixels = (const unsigned char *)img->data_p;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char ci = src_pixels[y * stride + x];
            if (reduced) ci = plan->remap[ci];
            Uint32 a = ci == 0 ? 0u : 0xFFu;
            unsigned char r = 160, g = 160, b = 160;
            if (pal_data && ci < pal_colors) {
                pal_word_to_rgb8(pal_data + ci * 2, &r, &g, &b);
            }
            dst[y * (pitch / 4) + x] =
                (a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
        }
    }
    SDL_UnlockTexture(tex);
    return tex;
}

static void RebuildPaletteReducePreviewTextures(const PaletteReductionPlan &plan)
{
    IMG *img = get_img(g_palette_reduce_preview_idx);
    PAL *pal = get_pal(plan.pal_idx);
    if (!img || !pal || !plan.valid) {
        ClearPaletteReducePreviewTextures();
        return;
    }

    if (g_palette_reduce_orig_tex && g_palette_reduce_new_tex &&
        g_palette_reduce_tex_img == g_palette_reduce_preview_idx &&
        g_palette_reduce_tex_pal == plan.pal_idx &&
        g_palette_reduce_tex_bpp == plan.target_bpp &&
        g_palette_reduce_tex_w == (int)img->w &&
        g_palette_reduce_tex_h == (int)img->h) {
        return;
    }

    ClearPaletteReducePreviewTextures();
    g_palette_reduce_orig_tex = BuildPaletteReductionTexture(img, pal, &plan, false);
    g_palette_reduce_new_tex = BuildPaletteReductionTexture(img, pal, &plan, true);
    g_palette_reduce_tex_img = g_palette_reduce_preview_idx;
    g_palette_reduce_tex_pal = plan.pal_idx;
    g_palette_reduce_tex_bpp = plan.target_bpp;
    g_palette_reduce_tex_w = img->w;
    g_palette_reduce_tex_h = img->h;
}

static void OpenPaletteReduceDialog(int bpp)
{
    commit_palette_adjustments();
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a palette first.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    if (bpp < 4) bpp = 4;
    if (bpp > 8) bpp = 8;
    g_palette_reduce_bpp = bpp;
    g_palette_reduce_preview_idx = FindPalettePreviewImage(g_doc->plselected, g_doc->ilselected);
    ClearPaletteReducePreviewTextures();
    g_show_palette_reduce = true;
}

static bool ApplyPaletteReductionPlan(const PaletteReductionPlan &plan)
{
    if (!plan.valid) return false;
    PAL *pal = get_pal(plan.pal_idx);
    if (!pal || !pal->data_p) return false;

    doc_undo_push();

    int old_numc = (int)pal->numc;
    if (old_numc > 256) old_numc = 256;
    memcpy(pal->data_p, plan.data, (size_t)plan.new_numc * 2);
    for (int i = plan.new_numc; i < old_numc; i++) {
        ((unsigned char *)pal->data_p)[i * 2 + 0] = 0;
        ((unsigned char *)pal->data_p)[i * 2 + 1] = 0;
    }
    pal->numc = (unsigned short)plan.new_numc;
    pal->bitspix = (unsigned char)plan.target_bpp;

    int img_idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        if (img->palnum != plan.pal_idx || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        int stride = (img->w + 3) & ~3;
        unsigned char *pixels = (unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char *idx = pixels + y * stride + x;
                *idx = plan.remap[*idx];
            }
        }
        InvalidateThumb(img_idx);
    }

    if (g_sel_color >= plan.new_numc)
        g_sel_color = plan.new_numc > 1 ? plan.new_numc - 1 : 0;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    ApplyPalette(plan.pal_idx);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    mark_dirty();
    return true;
}

struct VariantPaintResult {
    int pixels;
    int slots;
    int skipped_transparent;
    int skipped_no_slot;
};

static bool ensure_palette_numc(PAL *pal, int min_numc)
{
    if (!pal || min_numc <= 0 || min_numc > 256) return false;
    if (!pal->data_p) {
        pal->data_p = PoolAlloc(512);
        if (!pal->data_p) return false;
        pal->numc = (unsigned short)min_numc;
        pal->bitspix = 8;
        return true;
    }

    if (pal->numc >= min_numc) return true;

    unsigned char *old_data = (unsigned char *)pal->data_p;
    unsigned int old_bytes = (unsigned int)pal->numc * 2;
    unsigned char *new_data = (unsigned char *)malloc(512);
    if (!new_data) return false;
    memset(new_data, 0, 512);
    if (old_bytes > 0) memcpy(new_data, old_data, old_bytes);
    free(old_data);
    pal->data_p = new_data;
    pal->numc = (unsigned short)min_numc;
    if (pal->bitspix == 0) pal->bitspix = 8;
    return true;
}

static bool ensure_all_palettes_numc(int min_numc)
{
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
        if (!ensure_palette_numc(p, min_numc)) return false;
    }
    return true;
}

static void collect_library_used_indices(bool used[256])
{
    memset(used, 0, sizeof(bool) * 256);
    used[0] = true;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++)
                used[pix[y * stride + x]] = true;
    }
}

static bool slot_matches_variant_shadow(int slot, int base_idx, int target_pal_idx, unsigned short target_word)
{
    PAL *target = get_pal(target_pal_idx);
    if (!target || !target->data_p || slot >= (int)target->numc) return false;
    if (pal_word_or_black(target, slot) != target_word) return false;

    int pal_idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, pal_idx++) {
        if (pal_idx == target_pal_idx) continue;
        if (!p->data_p || slot >= (int)p->numc) return false;
        if (pal_word_or_black(p, slot) != pal_word_or_black(p, base_idx)) return false;
    }
    return true;
}

static int find_variant_shadow_slot(int base_idx, int target_pal_idx, unsigned short target_word)
{
    if (base_idx <= 0 || base_idx >= 256) return -1;
    for (int slot = 1; slot < 256; slot++) {
        if (slot == base_idx) continue;
        if (slot_matches_variant_shadow(slot, base_idx, target_pal_idx, target_word))
            return slot;
    }
    return -1;
}

static int create_variant_shadow_slot(int base_idx, int target_pal_idx, unsigned short target_word, bool *created)
{
    if (created) *created = false;
    int existing = find_variant_shadow_slot(base_idx, target_pal_idx, target_word);
    if (existing >= 0) return existing;

    bool used[256];
    collect_library_used_indices(used);
    int slot = -1;
    for (int i = 1; i < 256; i++) {
        if (!used[i]) { slot = i; break; }
    }
    if (slot < 0) return -1;
    if (!ensure_all_palettes_numc(slot + 1)) return -1;

    int pal_idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, pal_idx++) {
        unsigned short w = (pal_idx == target_pal_idx)
            ? target_word
            : pal_word_or_black(p, base_idx);
        unsigned char *pd = (unsigned char *)p->data_p;
        pd[slot * 2 + 0] = (unsigned char)(w & 0xFF);
        pd[slot * 2 + 1] = (unsigned char)(w >> 8);
    }

    if (created) *created = true;
    return slot;
}

static bool selection_contains_pixel(IMG *img, int x, int y)
{
    if (!img || !g_grid_sel.active) return false;
    int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
    int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x < x1 || x > x2 || y < y1 || y > y2) return false;
    if (!g_grid_sel.is_mask) return true;
    if (x < 0 || y < 0 || x >= g_grid_sel.mask_w || y >= g_grid_sel.mask_h) return false;
    return g_grid_sel.pixel_mask[(size_t)y * g_grid_sel.mask_w + x];
}

enum class PaletteZeroRemapMode { None, Selection, CurrentImage };

static int find_free_palette_slot_for_zero(PAL *pal, int pal_idx)
{
    if (!pal) return -1;

    int n = pal->numc;
    if (n < 0) n = 0;
    if (n < 256) return n <= 0 ? 1 : n;

    bool used[256] = {false};
    used[0] = true;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if ((int)img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++)
                used[pix[y * stride + x]] = true;
    }

    for (int i = 1; i < 256; i++)
        if (!used[i]) return i;

    unsigned short zero_word = pal_word_or_black(pal, 0);
    for (int i = 1; i < 256; i++)
        if (pal_word_or_black(pal, i) == zero_word) return i;

    return -1;
}

static int copy_palette_zero_color_to_slot(int requested_slot)
{
    int pal_idx = g_doc->plselected;
    PAL *pal = (pal_idx >= 0) ? get_pal(pal_idx) : NULL;
    if (!pal) return -1;

    int slot = requested_slot;
    if (slot < 0) slot = find_free_palette_slot_for_zero(pal, pal_idx);
    if (slot <= 0 || slot >= 256) return -1;

    commit_palette_adjustments();

    unsigned short zero_word = pal_word_or_black(pal, 0);
    int old_numc = (int)pal->numc;
    if (!ensure_palette_numc(pal, slot + 1)) return -1;
    if ((int)pal->numc != old_numc) mark_dirty();

    unsigned char *pd = (unsigned char *)pal->data_p;
    unsigned short old_word = (unsigned short)(pd[slot * 2] | (pd[slot * 2 + 1] << 8));
    if (old_word != zero_word) {
        pd[slot * 2 + 0] = (unsigned char)(zero_word & 0xFF);
        pd[slot * 2 + 1] = (unsigned char)(zero_word >> 8);
        mark_dirty();
    }

    g_sel_color = slot;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    ApplyPalette(pal_idx);
    save_palette_baseline();
    g_img_tex_idx = -2;
    return slot;
}

static int count_zero_pixels_for_remap(IMG *img, bool selection_only)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return 0;
    if (selection_only && !g_grid_sel.active) return 0;

    int count = 0;
    int stride = (img->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            if (pix[y * stride + x] == 0 &&
                (!selection_only || selection_contains_pixel(img, x, y)))
                count++;
        }
    }
    return count;
}

static int remap_zero_pixels_to_slot(IMG *img, int slot, bool selection_only)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return 0;
    if (slot <= 0 || slot >= 256) return 0;
    if (selection_only && !g_grid_sel.active) return 0;

    int changed = 0;
    int stride = (img->w + 3) & ~3;
    unsigned char *pix = (unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char *p = pix + y * stride + x;
            if (*p == 0 && (!selection_only || selection_contains_pixel(img, x, y))) {
                *p = (unsigned char)slot;
                changed++;
            }
        }
    }
    return changed;
}

static void CopyPaletteZeroToOpaqueSlot(int requested_slot = -1)
{
    doc_undo_push();
    int slot = copy_palette_zero_color_to_slot(requested_slot);
    if (slot < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No opaque palette slot is available for color #0.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Copied transparent color #0 to opaque palette index %d.", slot);
    g_restore_msg_timer = 4.0f;
}

static void CopyPaletteZeroAndRemap(PaletteZeroRemapMode mode, int requested_slot = -1)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    bool selection_only = (mode == PaletteZeroRemapMode::Selection);
    int pending = (mode == PaletteZeroRemapMode::None) ? 0
                : count_zero_pixels_for_remap(img, selection_only);

    doc_undo_push();
    int slot = copy_palette_zero_color_to_slot(requested_slot);
    if (slot < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No opaque palette slot is available for color #0.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int changed = 0;
    if (mode != PaletteZeroRemapMode::None && pending > 0) {
        changed = remap_zero_pixels_to_slot(img, slot, selection_only);
        if (changed > 0) {
            mark_dirty();
            g_img_tex_idx = -2;
        }
    }

    if (mode == PaletteZeroRemapMode::Selection) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 changed > 0
                     ? "Copied #0 to index %d and remapped %d selected transparent pixel%s."
                     : "Copied #0 to index %d; no selected #0 pixels to remap.",
                 slot, changed, changed == 1 ? "" : "s");
    } else if (mode == PaletteZeroRemapMode::CurrentImage) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 changed > 0
                     ? "Copied #0 to index %d and remapped %d transparent pixel%s."
                     : "Copied #0 to index %d; current sprite has no #0 pixels to remap.",
                 slot, changed, changed == 1 ? "" : "s");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Copied transparent color #0 to opaque palette index %d.", slot);
    }
    g_restore_msg_timer = 5.0f;
}

static VariantPaintResult ApplyVariantPaintToPixels(IMG *img, const std::vector<std::pair<int,int>>& pixels)
{
    VariantPaintResult r = {0, 0, 0, 0};
    if (!img || !img->data_p || pixels.empty()) return r;
    int target_pal_idx = img->palnum;
    PAL *target_pal = get_pal(target_pal_idx);
    if (!target_pal || !target_pal->data_p || target_pal_idx < 0) {
        r.skipped_no_slot = (int)pixels.size();
        return r;
    }
    if (g_sel_color == 0) {
        r.skipped_no_slot = (int)pixels.size();
        return r;
    }

    SDL_Color &tc = g_palette[g_sel_color];
    unsigned short target_word = rgb_to_word15(tc.r, tc.g, tc.b);
    int base_to_slot[256];
    for (int i = 0; i < 256; i++) base_to_slot[i] = -1;

    int stride = (img->w + 3) & ~3;
    unsigned char *data = (unsigned char *)img->data_p;
    for (const auto &pt : pixels) {
        int x = pt.first, y = pt.second;
        if (x < 0 || y < 0 || x >= (int)img->w || y >= (int)img->h) continue;
        unsigned char *pix = data + y * stride + x;
        int base_idx = *pix;
        if (base_idx == 0) { r.skipped_transparent++; continue; }
        if (pal_word_or_black(target_pal, base_idx) == target_word) continue;

        int slot = base_to_slot[base_idx];
        if (slot == -1) {
            bool created = false;
            slot = create_variant_shadow_slot(base_idx, target_pal_idx, target_word, &created);
            base_to_slot[base_idx] = (slot >= 0) ? slot : -2;
            if (created) r.slots++;
        }
        if (slot < 0) {
            r.skipped_no_slot++;
            continue;
        }
        if (*pix != (unsigned char)slot) {
            *pix = (unsigned char)slot;
            r.pixels++;
        }
    }

    if (r.pixels > 0 || r.slots > 0) {
        ApplyPalette(target_pal_idx);
        g_img_tex_idx = -2;
        mark_dirty();
    }
    return r;
}

static VariantPaintResult ApplyVariantBrush(IMG *img, int cx, int cy, int brush)
{
    std::vector<std::pair<int,int>> pts;
    int r = brush > 0 ? brush : 1;
    int r2 = (r - 1) * (r - 1);
    for (int by = -(r - 1); by <= (r - 1); by++) {
        for (int bx = -(r - 1); bx <= (r - 1); bx++) {
            if (r > 1 && bx * bx + by * by > r2) continue;
            pts.push_back({cx + bx, cy + by});
        }
    }
    return ApplyVariantPaintToPixels(img, pts);
}

static void ApplyVariantToSelection(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
    if (!g_grid_sel.active) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select pixels first, then apply variant paint.");
        g_restore_msg_timer = 4.0f;
        return;
    }
    if (g_sel_color == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Variant paint needs an opaque target color.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    std::vector<std::pair<int,int>> pts;
    for (int y = 0; y < img->h; y++)
        for (int x = 0; x < img->w; x++)
            if (selection_contains_pixel(img, x, y)) pts.push_back({x, y});

    doc_undo_push();
    VariantPaintResult r = ApplyVariantPaintToPixels(img, pts);
    if (r.pixels > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Variantized %d px using %d shadow slot%s.",
                 r.pixels, r.slots, r.slots == 1 ? "" : "s");
    } else if (r.skipped_no_slot > 0 && g_sel_color == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Variant paint needs an opaque target color.");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No pixels changed (%d transparent skipped).", r.skipped_transparent);
    }
    g_restore_msg_timer = 4.0f;
}

struct SelectionPropagateSample {
    int src_idx;
    int pal_idx;
    int target_idx;
    bool source_colors[256];
    std::vector<std::pair<int,int>> exact_pixels;
    std::vector<std::pair<int,int>> rel_seeds;
    int area;
    int min_x, min_y, max_x, max_y;
    double rel_cx, rel_cy;
};

struct SelectionPropagateMatch {
    int img_idx;
    std::vector<std::pair<int,int>> pixels;
};

struct SelectionComponentStats {
    int area;
    int min_x, min_y, max_x, max_y;
    long long sum_x, sum_y;
};

static bool BuildSelectionPropagateSample(SelectionPropagateSample *sample,
                                          char *err, size_t err_sz)
{
    if (err && err_sz) err[0] = '\0';
    if (!sample) return false;
    sample->src_idx = g_doc->ilselected;
    sample->pal_idx = -1;
    sample->target_idx = g_sel_color;
    memset(sample->source_colors, 0, sizeof(sample->source_colors));
    sample->exact_pixels.clear();
    sample->rel_seeds.clear();
    sample->area = 0;
    sample->min_x = sample->min_y = 0x7FFFFFFF;
    sample->max_x = sample->max_y = -1;
    sample->rel_cx = 0.0;
    sample->rel_cy = 0.0;

    IMG *src = (sample->src_idx >= 0) ? get_img(sample->src_idx) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) {
        snprintf(err, err_sz, "Select a source sprite first.");
        return false;
    }
    if (!g_grid_sel.active) {
        snprintf(err, err_sz, "Select the feature first, then propagate it.");
        return false;
    }
    if (g_sel_color <= 0 || g_sel_color >= 256) {
        snprintf(err, err_sz, "Pick a non-transparent destination swatch first.");
        return false;
    }

    sample->pal_idx = (int)src->palnum;
    PAL *pal = get_pal(sample->pal_idx);
    if (!pal || !pal->data_p) {
        snprintf(err, err_sz, "Source sprite has no usable palette.");
        return false;
    }

    int stride = (src->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)src->data_p;
    long long sum_x = 0;
    long long sum_y = 0;

    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            if (!selection_contains_pixel(src, x, y)) continue;
            unsigned char ci = pix[y * stride + x];
            if (ci == 0 || ci == (unsigned char)g_sel_color) continue;
            sample->source_colors[ci] = true;
            sample->exact_pixels.push_back({x, y});
            sum_x += x;
            sum_y += y;
            if (x < sample->min_x) sample->min_x = x;
            if (x > sample->max_x) sample->max_x = x;
            if (y < sample->min_y) sample->min_y = y;
            if (y > sample->max_y) sample->max_y = y;
        }
    }

    sample->area = (int)sample->exact_pixels.size();
    if (sample->area <= 0) {
        snprintf(err, err_sz,
                 "Selection has no source-colored opaque pixels to remap.");
        return false;
    }

    sample->rel_cx = (double)sum_x / (double)sample->area - (double)(short)src->anix;
    sample->rel_cy = (double)sum_y / (double)sample->area - (double)(short)src->aniy;

    int seed_limit = 768;
    int step = sample->area > seed_limit
        ? (sample->area + seed_limit - 1) / seed_limit
        : 1;
    sample->rel_seeds.reserve((size_t)((sample->area + step - 1) / step));
    for (int i = 0; i < sample->area; i += step) {
        int x = sample->exact_pixels[i].first;
        int y = sample->exact_pixels[i].second;
        sample->rel_seeds.push_back({x - (int)(short)src->anix,
                                     y - (int)(short)src->aniy});
    }
    return true;
}

static bool SelectionPropagateColorMatch(const SelectionPropagateSample &sample,
                                         unsigned char ci)
{
    return ci != 0 && ci != (unsigned char)sample.target_idx &&
           sample.source_colors[ci];
}

static bool FindNearestSelectionSeedPixel(IMG *img,
                                          const SelectionPropagateSample &sample,
                                          int cx, int cy, int radius,
                                          int *out_x, int *out_y)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;
    int w = img->w;
    int h = img->h;
    int stride = (w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;

    int best_x = -1;
    int best_y = -1;
    int best_d2 = 0x7FFFFFFF;
    int x0 = cx - radius; if (x0 < 0) x0 = 0;
    int y0 = cy - radius; if (y0 < 0) y0 = 0;
    int x1 = cx + radius; if (x1 >= w) x1 = w - 1;
    int y1 = cy + radius; if (y1 >= h) y1 = h - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!SelectionPropagateColorMatch(sample, pix[y * stride + x]))
                continue;
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_x = x;
                best_y = y;
                if (d2 == 0) {
                    if (out_x) *out_x = best_x;
                    if (out_y) *out_y = best_y;
                    return true;
                }
            }
        }
    }

    if (best_x < 0) return false;
    if (out_x) *out_x = best_x;
    if (out_y) *out_y = best_y;
    return true;
}

static void FloodSelectionPropagateComponent(IMG *img,
                                             const SelectionPropagateSample &sample,
                                             int sx, int sy,
                                             std::vector<unsigned char> &visited,
                                             std::vector<std::pair<int,int>> &out,
                                             SelectionComponentStats *stats)
{
    if (!img || !img->data_p || !stats) return;
    int w = img->w;
    int h = img->h;
    int stride = (w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return;
    if (visited[(size_t)sy * w + sx]) return;
    if (!SelectionPropagateColorMatch(sample, pix[sy * stride + sx])) return;

    stats->area = 0;
    stats->min_x = stats->min_y = 0x7FFFFFFF;
    stats->max_x = stats->max_y = -1;
    stats->sum_x = stats->sum_y = 0;

    std::vector<std::pair<int,int>> stack;
    stack.push_back({sx, sy});
    visited[(size_t)sy * w + sx] = 1;

    while (!stack.empty()) {
        std::pair<int,int> pt = stack.back();
        stack.pop_back();
        int x = pt.first;
        int y = pt.second;

        out.push_back(pt);
        stats->area++;
        stats->sum_x += x;
        stats->sum_y += y;
        if (x < stats->min_x) stats->min_x = x;
        if (x > stats->max_x) stats->max_x = x;
        if (y < stats->min_y) stats->min_y = y;
        if (y > stats->max_y) stats->max_y = y;

        const int dx[4] = {0, 1, 0, -1};
        const int dy[4] = {-1, 0, 1, 0};
        for (int i = 0; i < 4; i++) {
            int nx = x + dx[i];
            int ny = y + dy[i];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            size_t off = (size_t)ny * w + nx;
            if (visited[off]) continue;
            if (!SelectionPropagateColorMatch(sample, pix[ny * stride + nx]))
                continue;
            visited[off] = 1;
            stack.push_back({nx, ny});
        }
    }
}

static bool SelectionPropagateComponentLooksLikely(IMG *img,
                                                   const SelectionPropagateSample &sample,
                                                   const SelectionComponentStats &stats)
{
    if (!img || stats.area <= 0 || sample.area <= 0) return false;
    double ratio = (double)stats.area / (double)sample.area;
    if (ratio < 0.04 || ratio > 8.0) return false;

    double cx = (double)stats.sum_x / (double)stats.area - (double)(short)img->anix;
    double cy = (double)stats.sum_y / (double)stats.area - (double)(short)img->aniy;
    double dx = cx - sample.rel_cx;
    double dy = cy - sample.rel_cy;
    double dist = sqrt(dx * dx + dy * dy);

    int sample_w = sample.max_x - sample.min_x + 1;
    int sample_h = sample.max_y - sample.min_y + 1;
    if (sample_w < 1) sample_w = 1;
    if (sample_h < 1) sample_h = 1;
    double reach = (double)(sample_w > sample_h ? sample_w : sample_h) * 2.5 + 12.0;
    if (reach < 28.0) reach = 28.0;
    return dist <= reach;
}

static std::vector<std::pair<int,int>>
FindSelectionPropagationPixels(IMG *img, const SelectionPropagateSample &sample,
                               int img_idx)
{
    std::vector<std::pair<int,int>> result;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return result;

    if (img_idx == sample.src_idx) {
        result = sample.exact_pixels;
        return result;
    }

    int sample_w = sample.max_x - sample.min_x + 1;
    int sample_h = sample.max_y - sample.min_y + 1;
    int radius = (sample_w > sample_h ? sample_w : sample_h) / 2;
    if (radius < 6) radius = 6;
    if (radius > 18) radius = 18;

    int w = img->w;
    int h = img->h;
    std::vector<unsigned char> visited((size_t)w * h, 0);
    for (const auto &seed : sample.rel_seeds) {
        int ex = (int)(short)img->anix + seed.first;
        int ey = (int)(short)img->aniy + seed.second;
        int sx = 0, sy = 0;
        if (!FindNearestSelectionSeedPixel(img, sample, ex, ey, radius, &sx, &sy))
            continue;
        if (visited[(size_t)sy * w + sx])
            continue;

        std::vector<std::pair<int,int>> component;
        SelectionComponentStats stats = {};
        FloodSelectionPropagateComponent(img, sample, sx, sy,
                                         visited, component, &stats);
        if (component.empty())
            continue;
        if (!SelectionPropagateComponentLooksLikely(img, sample, stats))
            continue;
        result.insert(result.end(), component.begin(), component.end());
    }
    return result;
}

static void ApplySelectionRemapToMatchingSprites(void)
{
    SelectionPropagateSample sample = {};
    char err[160];
    if (!BuildSelectionPropagateSample(&sample, err, sizeof(err))) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", err);
        g_restore_msg_timer = 4.0f;
        return;
    }

    std::vector<SelectionPropagateMatch> matches;
    int scanned = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if ((int)img->palnum != sample.pal_idx ||
            !img->data_p || img->w == 0 || img->h == 0)
            continue;
        scanned++;
        std::vector<std::pair<int,int>> pts =
            FindSelectionPropagationPixels(img, sample, idx);
        if (!pts.empty())
            matches.push_back({idx, std::move(pts)});
    }

    if (matches.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No matching regions found in %d same-palette sprite%s.",
                 scanned, scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
        return;
    }

    if (!doc_undo_push()) return;

    PAL *pal = get_pal(sample.pal_idx);
    if (!ensure_palette_numc(pal, sample.target_idx + 1)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Could not extend palette to index %d.", sample.target_idx);
        g_restore_msg_timer = 4.0f;
        return;
    }

    int changed_pixels = 0;
    int changed_images = 0;
    for (SelectionPropagateMatch &m : matches) {
        IMG *img = get_img(m.img_idx);
        if (!img || !img->data_p) continue;
        int stride = (img->w + 3) & ~3;
        unsigned char *pix = (unsigned char *)img->data_p;
        int image_changed = 0;
        for (const auto &pt : m.pixels) {
            int x = pt.first;
            int y = pt.second;
            if (x < 0 || y < 0 || x >= (int)img->w || y >= (int)img->h)
                continue;
            unsigned char *p = pix + y * stride + x;
            if (!SelectionPropagateColorMatch(sample, *p))
                continue;
            *p = (unsigned char)sample.target_idx;
            image_changed++;
        }
        if (image_changed > 0) {
            changed_pixels += image_changed;
            changed_images++;
            InvalidateThumb(m.img_idx);
        }
    }

    if (changed_pixels > 0) {
        ApplyPalette(sample.pal_idx);
        save_palette_baseline();
        g_img_tex_idx = -2;
        mark_dirty();
    }

    if (changed_pixels > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Remapped %d likely-matching pixel%s in %d/%d sprite%s to #%d.",
                 changed_pixels, changed_pixels == 1 ? "" : "s",
                 changed_images, scanned, scanned == 1 ? "" : "s",
                 sample.target_idx);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Matching regions were already using #%d.", sample.target_idx);
    }
    g_restore_msg_timer = 5.0f;
}

/* ---- Marked Likeness Transfer ----
   One marked sprite acts as the source "look"; the selected sprite keeps its
   own silhouette and pose, but is recolored through normalized body-space
   samples from the source. This is deliberately deterministic and palette-
   native: no ML dependency, no bitmap import round-trip, undoable as one doc
   operation. */
struct LikenessBBox {
    int x1, y1, x2, y2;
    int w, h;
    float anchor_u, anchor_v;
    bool anchor_ok;
};

struct LikenessSample {
    float u, v;
    int x, y;
    unsigned char idx;
    int r, g, b, luma;
};

struct LikenessColor {
    unsigned char idx;
    float r, g, b, luma;
    bool found;
};

static bool LikenessOpaqueBBox(IMG *img, LikenessBBox *bbox)
{
    if (!img || !img->data_p || !bbox || img->w == 0 || img->h == 0)
        return false;

    int min_x = img->w, min_y = img->h, max_x = -1, max_y = -1;
    int stride = (img->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            if (pix[y * stride + x] == 0) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return false;

    bbox->x1 = min_x; bbox->y1 = min_y;
    bbox->x2 = max_x; bbox->y2 = max_y;
    bbox->w = max_x - min_x + 1;
    bbox->h = max_y - min_y + 1;

    float span_x = (bbox->w > 1) ? (float)(bbox->w - 1) : 1.0f;
    float span_y = (bbox->h > 1) ? (float)(bbox->h - 1) : 1.0f;
    bbox->anchor_u = ((float)(short)img->anix - (float)bbox->x1) / span_x;
    bbox->anchor_v = ((float)(short)img->aniy - (float)bbox->y1) / span_y;
    bbox->anchor_ok = bbox->anchor_u >= -0.25f && bbox->anchor_u <= 1.25f &&
                      bbox->anchor_v >= -0.25f && bbox->anchor_v <= 1.25f;
    return true;
}

static int LikenessWordLuma8(unsigned short word)
{
    int r = (int)((word >> 10) & 0x1F) * 255 / 31;
    int g = (int)((word >>  5) & 0x1F) * 255 / 31;
    int b = (int)( word        & 0x1F) * 255 / 31;
    return (r * 54 + g * 183 + b * 19) >> 8;
}

static void LikenessWordRgb8(unsigned short word, int *r, int *g, int *b)
{
    if (r) *r = (int)((word >> 10) & 0x1F) * 255 / 31;
    if (g) *g = (int)((word >>  5) & 0x1F) * 255 / 31;
    if (b) *b = (int)( word        & 0x1F) * 255 / 31;
}

static void LikenessBuildSamples(IMG *src, PAL *src_pal,
                                 const LikenessBBox &bbox,
                                 std::vector<LikenessSample> &samples,
                                 bool used_slots[256],
                                 int *mean_luma)
{
    samples.clear();
    memset(used_slots, 0, sizeof(bool) * 256);
    if (mean_luma) *mean_luma = 128;
    if (!src || !src->data_p || !src_pal || !src_pal->data_p) return;

    int stride = (src->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)src->data_p;
    float span_x = (bbox.w > 1) ? (float)(bbox.w - 1) : 1.0f;
    float span_y = (bbox.h > 1) ? (float)(bbox.h - 1) : 1.0f;
    long long luma_sum = 0;

    samples.reserve((size_t)bbox.w * bbox.h / 2);
    for (int y = bbox.y1; y <= bbox.y2; y++) {
        for (int x = bbox.x1; x <= bbox.x2; x++) {
            unsigned char ci = pix[y * stride + x];
            if (ci == 0) continue;
            unsigned short word = pal_word_or_black(src_pal, ci);
            int r, g, b;
            LikenessWordRgb8(word, &r, &g, &b);
            int luma = LikenessWordLuma8(word);
            LikenessSample s = {};
            s.u = ((float)x - (float)bbox.x1) / span_x;
            s.v = ((float)y - (float)bbox.y1) / span_y;
            s.x = x;
            s.y = y;
            s.idx = ci;
            s.r = r; s.g = g; s.b = b; s.luma = luma;
            samples.push_back(s);
            used_slots[ci] = true;
            luma_sum += luma;
        }
    }
    if (mean_luma && !samples.empty())
        *mean_luma = (int)(luma_sum / (long long)samples.size());
}

static float LikenessMaskScore(IMG *target, const LikenessBBox &tb,
                               const std::vector<LikenessSample> &samples,
                               bool mirror)
{
    const int G = 32;
    bool src_occ[G * G] = {};
    for (const LikenessSample &s : samples) {
        float u = mirror ? (1.0f - s.u) : s.u;
        int bx = (int)(u * (G - 1) + 0.5f);
        int by = (int)(s.v * (G - 1) + 0.5f);
        if (bx < 0) bx = 0; if (bx >= G) bx = G - 1;
        if (by < 0) by = 0; if (by >= G) by = G - 1;
        src_occ[by * G + bx] = true;
    }

    int stride = (target->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)target->data_p;
    float span_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    float score = 0.0f;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            if (pix[y * stride + x] == 0) continue;
            int bx = (int)((((float)x - tb.x1) / span_x) * (G - 1) + 0.5f);
            int by = (int)((((float)y - tb.y1) / span_y) * (G - 1) + 0.5f);
            if (bx < 0) bx = 0; if (bx >= G) bx = G - 1;
            if (by < 0) by = 0; if (by >= G) by = G - 1;
            if (src_occ[by * G + bx]) {
                score += 2.0f;
            } else {
                bool has_near_source = false;
                for (int dy = -1; dy <= 1 && !has_near_source; dy++) {
                    for (int dx = -1; dx <= 1 && !has_near_source; dx++) {
                        int nx = bx + dx, ny = by + dy;
                        if (nx < 0 || nx >= G || ny < 0 || ny >= G) continue;
                        if (src_occ[ny * G + nx]) has_near_source = true;
                    }
                }
                if (has_near_source) score += 0.75f;
            }
        }
    }
    return score;
}

static int LikenessNearestUsedSlot(PAL *pal, const bool used_slots[256],
                                   int r, int g, int b)
{
    if (!pal || !pal->data_p) return 0;
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    unsigned short desired = rgb_to_word15((unsigned char)r,
                                           (unsigned char)g,
                                           (unsigned char)b);
    int count = pal->numc;
    if (count > 256) count = 256;
    int best = 0;
    int best_dist = 0x7FFFFFFF;
    for (int i = 1; i < count; i++) {
        if (!used_slots[i]) continue;
        int dist = PaletteColorDistance5(desired, pal_word_or_black(pal, i));
        if (dist < best_dist) {
            best_dist = dist;
            best = i;
            if (dist == 0) break;
        }
    }
    if (best > 0) return best;
    return FindNearestPaletteSlot(pal, desired);
}

static bool LikenessSampleSourceColor(const std::vector<LikenessSample> &samples,
                                      const std::vector<std::vector<int>> &bins,
                                      float u, float v,
                                      LikenessColor *out)
{
    if (out) *out = {};
    if (samples.empty() || !out) return false;
    const int G = 32;
    const float radii[] = { 0.055f, 0.095f, 0.16f, 0.27f };
    for (float radius : radii) {
        float r2 = radius * radius;
        int bx0 = (int)((u - radius) * G);
        int bx1 = (int)((u + radius) * G);
        int by0 = (int)((v - radius) * G);
        int by1 = (int)((v + radius) * G);
        if (bx0 < 0) bx0 = 0; if (bx1 >= G) bx1 = G - 1;
        if (by0 < 0) by0 = 0; if (by1 >= G) by1 = G - 1;

        double wr = 0.0, wg = 0.0, wb = 0.0, wl = 0.0, wsum = 0.0;
        const LikenessSample *nearest = NULL;
        float nearest_d2 = 9999.0f;
        for (int by = by0; by <= by1; by++) {
            for (int bx = bx0; bx <= bx1; bx++) {
                const std::vector<int> &bucket = bins[by * G + bx];
                for (int si : bucket) {
                    const LikenessSample &s = samples[si];
                    float dx = (s.u - u) * 1.12f;
                    float dy = s.v - v;
                    float d2 = dx * dx + dy * dy;
                    if (d2 > r2) continue;
                    if (d2 < nearest_d2) {
                        nearest_d2 = d2;
                        nearest = &s;
                    }
                    double wt = 1.0 / (0.0009 + (double)d2);
                    wr += (double)s.r * wt;
                    wg += (double)s.g * wt;
                    wb += (double)s.b * wt;
                    wl += (double)s.luma * wt;
                    wsum += wt;
                }
            }
        }
        if (wsum > 0.0) {
            out->r = (float)(wr / wsum);
            out->g = (float)(wg / wsum);
            out->b = (float)(wb / wsum);
            out->luma = (float)(wl / wsum);
            if (nearest) {
                out->r = out->r * 0.65f + nearest->r * 0.35f;
                out->g = out->g * 0.65f + nearest->g * 0.35f;
                out->b = out->b * 0.65f + nearest->b * 0.35f;
                out->luma = out->luma * 0.65f + nearest->luma * 0.35f;
                out->idx = nearest->idx;
            }
            out->found = true;
            return true;
        }
    }

    const LikenessSample *nearest = &samples[0];
    float best_d2 = 9999.0f;
    for (const LikenessSample &s : samples) {
        float dx = (s.u - u) * 1.12f;
        float dy = s.v - v;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            nearest = &s;
        }
    }
    out->r = (float)nearest->r;
    out->g = (float)nearest->g;
    out->b = (float)nearest->b;
    out->luma = (float)nearest->luma;
    out->idx = nearest->idx;
    out->found = true;
    return true;
}

struct LikenessPartBox {
    int x1, y1, x2, y2;
    int count;
    float cx, cy;
    bool valid;
};

static int LikenessClampInt(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int LikenessAbsInt(int v)
{
    return v < 0 ? -v : v;
}

static void LikenessPaletteRgb(PAL *pal, unsigned char ci,
                               int *r, int *g, int *b, int *luma)
{
    unsigned short word = pal_word_or_black(pal, ci);
    int rr, gg, bb;
    LikenessWordRgb8(word, &rr, &gg, &bb);
    if (r) *r = rr;
    if (g) *g = gg;
    if (b) *b = bb;
    if (luma) *luma = LikenessWordLuma8(word);
}

static bool LikenessIsGoldPixel(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return false;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    return luma >= 48 && luma <= 212 &&
           r >= 82 && g >= 50 &&
           r > b + 34 && g > b + 16 &&
           r >= g - 28;
}

static bool LikenessIsHeadPixel(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return false;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;

    bool pale_hair = luma >= 108 && chroma <= 88;
    bool skin = luma >= 78 &&
                r >= g - 18 && g >= b - 34 &&
                r > b + 10 && chroma <= 132;
    bool hot_gold = LikenessIsGoldPixel(pal, ci) && b < 72 && luma < 150;
    return (pale_hair || skin) && !hot_gold;
}

static int LikenessTransparentNeighborCount(const unsigned char *pix,
                                            int w, int h, int stride,
                                            int x, int y)
{
    int n = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx;
            int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h ||
                pix[(size_t)ny * stride + nx] == 0)
                n++;
        }
    }
    return n;
}

static bool LikenessHasLocalContrast(const unsigned char *pix,
                                     int w, int h, int stride,
                                     int x, int y, PAL *pal)
{
    if (!pix || !pal) return false;
    unsigned char ci = pix[(size_t)y * stride + x];
    if (ci == 0) return false;
    unsigned short self = pal_word_or_black(pal, ci);
    int self_luma = LikenessWordLuma8(self);
    int max_dist = 0;
    int max_luma_delta = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            if (dx == 0 && dy == 0) continue;
            int nx = x + dx;
            int ny = y + dy;
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            unsigned char ni = pix[(size_t)ny * stride + nx];
            if (ni == 0 || ni == ci) continue;
            unsigned short nw = pal_word_or_black(pal, ni);
            int dist = PaletteColorDistance5(self, nw);
            int ld = self_luma - LikenessWordLuma8(nw);
            if (ld < 0) ld = -ld;
            if (dist > max_dist) max_dist = dist;
            if (ld > max_luma_delta) max_luma_delta = ld;
        }
    }
    return max_dist >= 86 || max_luma_delta >= 34;
}

static bool LikenessFindHeadPart(const unsigned char *pix,
                                 int w, int h, int stride,
                                 PAL *pal,
                                 const LikenessBBox &bbox,
                                 bool prefer_top,
                                 LikenessPartBox *out)
{
    if (out) *out = {};
    if (!pix || !pal || !out) return false;

    std::vector<unsigned char> visited((size_t)stride * h, 0);
    std::vector<int> queue;
    float best_score = -1.0f;
    float span_y = (bbox.h > 1) ? (float)(bbox.h - 1) : 1.0f;

    for (int y = bbox.y1; y <= bbox.y2; y++) {
        for (int x = bbox.x1; x <= bbox.x2; x++) {
            size_t off = (size_t)y * stride + x;
            if (visited[off] || !LikenessIsHeadPixel(pal, pix[off])) continue;

            int min_x = x, min_y = y, max_x = x, max_y = y;
            int count = 0;
            long long sum_x = 0, sum_y = 0;
            queue.clear();
            queue.push_back(y * stride + x);
            visited[off] = 1;

            for (size_t qi = 0; qi < queue.size(); qi++) {
                int qoff = queue[qi];
                int cy = qoff / stride;
                int cx = qoff - cy * stride;
                count++;
                sum_x += cx;
                sum_y += cy;
                if (cx < min_x) min_x = cx;
                if (cy < min_y) min_y = cy;
                if (cx > max_x) max_x = cx;
                if (cy > max_y) max_y = cy;

                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        if (dx == 0 && dy == 0) continue;
                        int nx = cx + dx;
                        int ny = cy + dy;
                        if (nx < bbox.x1 || nx > bbox.x2 ||
                            ny < bbox.y1 || ny > bbox.y2)
                            continue;
                        size_t noff = (size_t)ny * stride + nx;
                        if (visited[noff] || !LikenessIsHeadPixel(pal, pix[noff]))
                            continue;
                        visited[noff] = 1;
                        queue.push_back(ny * stride + nx);
                    }
                }
            }

            if (count < 3) continue;
            int bw = max_x - min_x + 1;
            int bh = max_y - min_y + 1;
            float cx = (float)sum_x / (float)count;
            float cy = (float)sum_y / (float)count;
            float norm_y = (cy - (float)bbox.y1) / span_y;
            float density = (float)count / (float)(bw * bh);
            float score = (float)count * (1.0f + density * 0.8f);
            if (prefer_top) score *= 1.0f + (1.0f - norm_y) * 1.15f;
            if (bw > bbox.w * 7 / 10 || bh > bbox.h * 5 / 10) score *= 0.45f;
            if (prefer_top && norm_y > 0.70f) score *= 0.25f;

            if (score > best_score) {
                best_score = score;
                out->x1 = min_x; out->y1 = min_y;
                out->x2 = max_x; out->y2 = max_y;
                out->count = count;
                out->cx = cx; out->cy = cy;
                out->valid = true;
            }
        }
    }
    return out->valid;
}

static void LikenessExpandPartBox(LikenessPartBox *box,
                                  int w, int h, int px, int py)
{
    if (!box || !box->valid) return;
    box->x1 = LikenessClampInt(box->x1 - px, 0, w - 1);
    box->x2 = LikenessClampInt(box->x2 + px, 0, w - 1);
    box->y1 = LikenessClampInt(box->y1 - py, 0, h - 1);
    box->y2 = LikenessClampInt(box->y2 + py, 0, h - 1);
}

static bool LikenessPointInPartBox(const LikenessPartBox &box, int x, int y)
{
    return box.valid &&
           x >= box.x1 && x <= box.x2 &&
           y >= box.y1 && y <= box.y2;
}

static unsigned char LikenessNearestOpaqueInBox(const unsigned char *pix,
                                                int w, int h, int stride,
                                                const LikenessPartBox &box,
                                                int x, int y)
{
    if (!pix || !box.valid) return 0;
    x = LikenessClampInt(x, box.x1, box.x2);
    y = LikenessClampInt(y, box.y1, box.y2);
    unsigned char direct = pix[(size_t)y * stride + x];
    if (direct != 0) return direct;

    int max_radius = box.x2 - box.x1;
    int bh = box.y2 - box.y1;
    if (bh > max_radius) max_radius = bh;
    if (max_radius > 4) max_radius = 4;
    for (int r = 1; r <= max_radius; r++) {
        unsigned char best = 0;
        int best_d2 = 0x7FFFFFFF;
        for (int dy = -r; dy <= r; dy++) {
            for (int dx = -r; dx <= r; dx++) {
                int sx = x + dx;
                int sy = y + dy;
                if (sx < box.x1 || sx > box.x2 ||
                    sy < box.y1 || sy > box.y2 ||
                    sx < 0 || sy < 0 || sx >= w || sy >= h)
                    continue;
                unsigned char ci = pix[(size_t)sy * stride + sx];
                if (ci == 0) continue;
                int d2 = dx * dx + dy * dy;
                if (d2 < best_d2) {
                    best_d2 = d2;
                    best = ci;
                }
            }
        }
        if (best != 0) return best;
    }
    return 0;
}

static LikenessPartBox LikenessProjectPartBox(const LikenessPartBox &src_box,
                                              const LikenessBBox &sb,
                                              const LikenessBBox &tb,
                                              bool mirror,
                                              int target_w, int target_h)
{
    LikenessPartBox out = {};
    if (!src_box.valid) return out;
    float sxspan = (sb.w > 1) ? (float)(sb.w - 1) : 1.0f;
    float syspan = (sb.h > 1) ? (float)(sb.h - 1) : 1.0f;
    float txspan = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float tyspan = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;

    float u1 = ((float)src_box.x1 - (float)sb.x1) / sxspan;
    float u2 = ((float)src_box.x2 - (float)sb.x1) / sxspan;
    if (mirror) {
        float mu1 = 1.0f - u2;
        float mu2 = 1.0f - u1;
        u1 = mu1; u2 = mu2;
    }
    float v1 = ((float)src_box.y1 - (float)sb.y1) / syspan;
    float v2 = ((float)src_box.y2 - (float)sb.y1) / syspan;

    if (sb.anchor_ok && tb.anchor_ok) {
        float src_anchor_u = mirror ? (1.0f - sb.anchor_u) : sb.anchor_u;
        float du = 0.28f * (src_anchor_u - tb.anchor_u);
        float dv = 0.24f * (sb.anchor_v - tb.anchor_v);
        u1 -= du; u2 -= du;
        v1 -= dv; v2 -= dv;
    }

    out.x1 = LikenessClampInt((int)(tb.x1 + u1 * txspan + 0.5f), 0, target_w - 1);
    out.x2 = LikenessClampInt((int)(tb.x1 + u2 * txspan + 0.5f), 0, target_w - 1);
    out.y1 = LikenessClampInt((int)(tb.y1 + v1 * tyspan + 0.5f), 0, target_h - 1);
    out.y2 = LikenessClampInt((int)(tb.y1 + v2 * tyspan + 0.5f), 0, target_h - 1);
    if (out.x1 > out.x2) { int t = out.x1; out.x1 = out.x2; out.x2 = t; }
    if (out.y1 > out.y2) { int t = out.y1; out.y1 = out.y2; out.y2 = t; }
    out.count = src_box.count;
    out.cx = (out.x1 + out.x2) * 0.5f;
    out.cy = (out.y1 + out.y2) * 0.5f;
    out.valid = out.x2 >= out.x1 && out.y2 >= out.y1;
    return out;
}

static bool LikenessProjectSourceToTarget(float raw_u, float raw_v,
                                          const LikenessBBox &sb,
                                          const LikenessBBox &tb,
                                          bool mirror,
                                          float *out_u, float *out_v)
{
    float u = mirror ? (1.0f - raw_u) : raw_u;
    float v = raw_v;
    if (sb.anchor_ok && tb.anchor_ok) {
        float src_anchor_u = mirror ? (1.0f - sb.anchor_u) : sb.anchor_u;
        u -= 0.28f * (src_anchor_u - tb.anchor_u);
        v -= 0.24f * (sb.anchor_v - tb.anchor_v);
    }
    if (u < -0.08f || u > 1.08f || v < -0.08f || v > 1.08f)
        return false;
    if (u < 0.0f) u = 0.0f; if (u > 1.0f) u = 1.0f;
    if (v < 0.0f) v = 0.0f; if (v > 1.0f) v = 1.0f;
    if (out_u) *out_u = u;
    if (out_v) *out_v = v;
    return true;
}

static int LikenessMaskMismatch(const unsigned char *src_pix,
                                int sw, int sh, int ss, int sx, int sy,
                                const unsigned char *dst_pix,
                                int dw, int dh, int ds, int dx0, int dy0)
{
    int mismatch = 0;
    for (int dy = -1; dy <= 1; dy++) {
        for (int dx = -1; dx <= 1; dx++) {
            bool so = false;
            bool do_ = false;
            int ax = sx + dx, ay = sy + dy;
            int bx = dx0 + dx, by = dy0 + dy;
            if (ax >= 0 && ay >= 0 && ax < sw && ay < sh)
                so = src_pix[(size_t)ay * ss + ax] != 0;
            if (bx >= 0 && by >= 0 && bx < dw && by < dh)
                do_ = dst_pix[(size_t)by * ds + bx] != 0;
            if (so != do_) mismatch++;
        }
    }
    return mismatch;
}

static int LikenessApplyHeadPatch(const unsigned char *source_pix,
                                  int sw, int sh, int ss,
                                  const unsigned char *target_original,
                                  int tw, int th, int ts,
                                  unsigned char *target_pix,
                                  PAL *source_pal, PAL *target_pal,
                                  const LikenessBBox &sb,
                                  const LikenessBBox &tb,
                                  bool mirror,
                                  std::vector<unsigned char> &patch_mask,
                                  LikenessPartBox *source_head_out)
{
    LikenessPartBox source_head = {};
    LikenessPartBox target_head = {};
    if (!LikenessFindHeadPart(source_pix, sw, sh, ss, source_pal, sb,
                              true, &source_head))
        return 0;

    if (!LikenessFindHeadPart(target_original, tw, th, ts, target_pal, tb,
                              false, &target_head))
        target_head = LikenessProjectPartBox(source_head, sb, tb, mirror, tw, th);

    if (!target_head.valid) return 0;
    if (source_head_out) *source_head_out = source_head;

    LikenessExpandPartBox(&source_head, sw, sh, 1, 1);
    LikenessExpandPartBox(&target_head, tw, th, 1, 1);

    float source_span_x = (source_head.x2 > source_head.x1)
        ? (float)(source_head.x2 - source_head.x1) : 1.0f;
    float source_span_y = (source_head.y2 > source_head.y1)
        ? (float)(source_head.y2 - source_head.y1) : 1.0f;
    float target_span_x = (target_head.x2 > target_head.x1)
        ? (float)(target_head.x2 - target_head.x1) : 1.0f;
    float target_span_y = (target_head.y2 > target_head.y1)
        ? (float)(target_head.y2 - target_head.y1) : 1.0f;

    int changed = 0;
    for (int y = target_head.y1; y <= target_head.y2; y++) {
        for (int x = target_head.x1; x <= target_head.x2; x++) {
            int off = y * ts + x;
            if (target_original[(size_t)off] == 0) continue;

            float rx = ((float)x - (float)target_head.x1) / target_span_x;
            float ry = ((float)y - (float)target_head.y1) / target_span_y;
            float sx_f = mirror
                ? (float)source_head.x2 - rx * source_span_x
                : (float)source_head.x1 + rx * source_span_x;
            float sy_f = (float)source_head.y1 + ry * source_span_y;
            int sx = LikenessClampInt((int)(sx_f + 0.5f), 0, sw - 1);
            int sy = LikenessClampInt((int)(sy_f + 0.5f), 0, sh - 1);
            unsigned char ci = LikenessNearestOpaqueInBox(source_pix, sw, sh, ss,
                                                          source_head, sx, sy);
            if (ci == 0) continue;
            if (target_pix[off] != ci) {
                target_pix[off] = ci;
                changed++;
            }
            patch_mask[(size_t)off] = 1;
        }
    }
    return changed;
}

static int LikenessApplyDetailPatches(const unsigned char *source_pix,
                                      int sw, int sh, int ss,
                                      const unsigned char *target_original,
                                      int tw, int th, int ts,
                                      unsigned char *target_pix,
                                      PAL *source_pal, PAL *target_pal,
                                      const LikenessBBox &sb,
                                      const LikenessBBox &tb,
                                      bool mirror,
                                      const LikenessPartBox &source_head,
                                      std::vector<unsigned char> &patch_mask)
{
    if (!source_pix || !target_original || !target_pix ||
        !source_pal || !target_pal)
        return 0;

    float sspan_x = (sb.w > 1) ? (float)(sb.w - 1) : 1.0f;
    float sspan_y = (sb.h > 1) ? (float)(sb.h - 1) : 1.0f;
    float tspan_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float tspan_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int search_radius = 2 + ((tb.w > tb.h ? tb.w : tb.h) / 46);
    if (search_radius < 2) search_radius = 2;
    if (search_radius > 5) search_radius = 5;

    std::vector<int> best_score((size_t)ts * th, 0x7FFFFFFF);
    int changed = 0;

    for (int sy = sb.y1; sy <= sb.y2; sy++) {
        for (int sx = sb.x1; sx <= sb.x2; sx++) {
            int soff = sy * ss + sx;
            unsigned char ci = source_pix[(size_t)soff];
            if (ci == 0) continue;
            if (LikenessPointInPartBox(source_head, sx, sy)) continue;

            float raw_u = ((float)sx - (float)sb.x1) / sspan_x;
            float raw_v = ((float)sy - (float)sb.y1) / sspan_y;
            bool gold = LikenessIsGoldPixel(source_pal, ci);
            bool headish = LikenessIsHeadPixel(source_pal, ci);
            bool contrast = LikenessHasLocalContrast(source_pix, sw, sh, ss,
                                                     sx, sy, source_pal);
            bool edge = LikenessTransparentNeighborCount(source_pix, sw, sh, ss,
                                                         sx, sy) > 0;
            if (!(gold && (raw_v < 0.80f || contrast || edge)) &&
                !(headish && contrast && raw_v < 0.70f) &&
                !(contrast && raw_v < 0.72f && edge))
                continue;

            float tu, tv;
            if (!LikenessProjectSourceToTarget(raw_u, raw_v, sb, tb, mirror,
                                               &tu, &tv))
                continue;
            int tx0 = LikenessClampInt((int)(tb.x1 + tu * tspan_x + 0.5f),
                                       0, tw - 1);
            int ty0 = LikenessClampInt((int)(tb.y1 + tv * tspan_y + 0.5f),
                                       0, th - 1);

            int best_off = -1;
            int best = 0x7FFFFFFF;
            for (int dy = -search_radius; dy <= search_radius; dy++) {
                for (int dx = -search_radius; dx <= search_radius; dx++) {
                    int tx = tx0 + dx;
                    int ty = ty0 + dy;
                    if (tx < 0 || ty < 0 || tx >= tw || ty >= th) continue;
                    int toff = ty * ts + tx;
                    if (target_original[(size_t)toff] == 0) continue;
                    if (patch_mask[(size_t)toff] == 1) continue;

                    int dist2 = dx * dx + dy * dy;
                    int mismatch = LikenessMaskMismatch(source_pix, sw, sh, ss,
                                                        sx, sy,
                                                        target_original,
                                                        tw, th, ts, tx, ty);
                    unsigned char old_ci = target_original[(size_t)toff];
                    bool target_warm = LikenessIsGoldPixel(target_pal, old_ci) ||
                                       LikenessIsHeadPixel(target_pal, old_ci);
                    int target_edge = LikenessTransparentNeighborCount(
                        target_original, tw, th, ts, tx, ty);

                    int score = dist2 * 7 + mismatch * 8;
                    if (gold && target_warm) score -= 10;
                    else if (gold) score += 4;
                    if (contrast) score -= 4;
                    if (target_edge > 0) score -= 3;

                    if (score < best) {
                        best = score;
                        best_off = toff;
                    }
                }
            }

            if (best_off < 0 || best > 70 || best >= best_score[(size_t)best_off])
                continue;
            best_score[(size_t)best_off] = best;
            if (target_pix[best_off] != ci) {
                target_pix[best_off] = ci;
                changed++;
            }
            patch_mask[(size_t)best_off] = 2;
        }
    }
    return changed;
}

static int LikenessBlendPatchEdges(unsigned char *target_pix,
                                   const unsigned char *target_original,
                                   int w, int h, int stride,
                                   PAL *source_pal,
                                   const bool used_slots[256],
                                   const std::vector<unsigned char> &patch_mask)
{
    if (!target_pix || !target_original || !source_pal || !used_slots)
        return 0;

    struct BlendWrite { int off; unsigned char ci; };
    std::vector<BlendWrite> writes;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            int off = y * stride + x;
            if (target_original[(size_t)off] == 0 || patch_mask[(size_t)off])
                continue;
            unsigned char old_ci = target_pix[off];
            if (old_ci == 0) continue;

            int patch_neighbors = 0;
            int rsum = 0, gsum = 0, bsum = 0;
            for (int dy = -1; dy <= 1; dy++) {
                for (int dx = -1; dx <= 1; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    int noff = ny * stride + nx;
                    if (!patch_mask[(size_t)noff]) continue;
                    unsigned char pci = target_pix[noff];
                    if (pci == 0) continue;
                    int pr, pg, pb;
                    LikenessPaletteRgb(source_pal, pci, &pr, &pg, &pb, NULL);
                    rsum += pr; gsum += pg; bsum += pb;
                    patch_neighbors++;
                }
            }
            if (patch_neighbors < 2) continue;

            int sr, sg, sbc;
            LikenessPaletteRgb(source_pal, old_ci, &sr, &sg, &sbc, NULL);
            int ar = rsum / patch_neighbors;
            int ag = gsum / patch_neighbors;
            int ab = bsum / patch_neighbors;
            int mix = patch_neighbors >= 4 ? 112 : 78; /* 0..256 */
            int nr = (sr * (256 - mix) + ar * mix) >> 8;
            int ng = (sg * (256 - mix) + ag * mix) >> 8;
            int nb = (sbc * (256 - mix) + ab * mix) >> 8;
            int ci = LikenessNearestUsedSlot(source_pal, used_slots, nr, ng, nb);
            if (ci > 0 && ci != old_ci)
                writes.push_back({off, (unsigned char)ci});
        }
    }

    int changed = 0;
    for (const BlendWrite &wr : writes) {
        if (target_pix[wr.off] != wr.ci) {
            target_pix[wr.off] = wr.ci;
            changed++;
        }
    }
    return changed;
}

enum LikenessMaterial {
    LK_MATERIAL_ANY = 0,
    LK_MATERIAL_HEAD,
    LK_MATERIAL_SKIN,
    LK_MATERIAL_BLUE,
    LK_MATERIAL_LOWER,
    LK_MATERIAL_BLACK
};

struct LikenessActorStats {
    int opaque;
    int remapped;
    int accents;
    int cleaned;
    int edge_darkened;
    int head_pixels;
    int skin_pixels;
};

static int LikenessClampByte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static int LikenessRgbLuma8(int r, int g, int b)
{
    return (r * 54 + g * 183 + b * 19) >> 8;
}

static bool LikenessIsBadGreenRgb(int r, int g, int b, int luma)
{
    (void)luma;
    return g > 65 && g > r + 7 && g > b + 18 &&
           (r < 105 || b < 55);
}

static bool LikenessIsBadGreenPixel(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return false;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    return LikenessIsBadGreenRgb(r, g, b, luma);
}

static bool LikenessIsBlackRgb(int r, int g, int b, int luma)
{
    return luma <= 34 || (r < 44 && g < 44 && b < 54);
}

static bool LikenessIsBlueRgb(int r, int g, int b, int luma)
{
    if (LikenessIsBadGreenRgb(r, g, b, luma)) return false;
    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;
    return luma >= 14 && luma <= 184 &&
           b >= r + 4 && b >= g - 12 &&
           (b >= 46 || chroma >= 26);
}

static bool LikenessIsLowerRgb(int r, int g, int b, int luma)
{
    if (LikenessIsBadGreenRgb(r, g, b, luma)) return false;
    if (luma < 28 || luma > 214) return false;
    return r >= 54 && g >= 34 &&
           r > b + 22 && g > b + 7 &&
           r >= g - 38;
}

static bool LikenessIsSkinRgb(int r, int g, int b, int luma)
{
    if (LikenessIsBadGreenRgb(r, g, b, luma) ||
        LikenessIsBlueRgb(r, g, b, luma))
        return false;

    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;
    bool warm_skin = luma >= 58 &&
                     r >= g - 28 && g >= b - 42 &&
                     r > b + 7 && chroma <= 140;
    bool pale_hair = luma >= 102 &&
                     chroma <= 86 &&
                     r >= b - 18 && g >= b - 24;
    bool goldish = LikenessIsLowerRgb(r, g, b, luma) &&
                   b < 96 && r > b + 34 && g > b + 18;
    return (warm_skin || pale_hair) && !goldish;
}

static bool LikenessIsHeadRgb(int r, int g, int b, int luma)
{
    int mn = r < g ? r : g; if (b < mn) mn = b;
    int mx = r > g ? r : g; if (b > mx) mx = b;
    int chroma = mx - mn;
    bool grey_hair = luma >= 94 && chroma <= 78 &&
                     !LikenessIsBlueRgb(r, g, b, luma) &&
                     !LikenessIsLowerRgb(r, g, b, luma);
    return LikenessIsSkinRgb(r, g, b, luma) || grey_hair;
}

static bool LikenessPaletteSlotMatchesMaterial(PAL *pal, unsigned char ci,
                                               LikenessMaterial material)
{
    if (!pal || ci == 0) return false;
    if (material == LK_MATERIAL_ANY) return true;

    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    switch (material) {
    case LK_MATERIAL_HEAD:
        return LikenessIsHeadPixel(pal, ci) || LikenessIsHeadRgb(r, g, b, luma);
    case LK_MATERIAL_SKIN:
        return LikenessIsHeadRgb(r, g, b, luma) ||
               LikenessIsSkinRgb(r, g, b, luma);
    case LK_MATERIAL_BLUE:
        return LikenessIsBlueRgb(r, g, b, luma) ||
               (LikenessIsBlackRgb(r, g, b, luma) && b >= r - 4);
    case LK_MATERIAL_LOWER:
        return LikenessIsGoldPixel(pal, ci) ||
               LikenessIsLowerRgb(r, g, b, luma);
    case LK_MATERIAL_BLACK:
        return LikenessIsBlackRgb(r, g, b, luma);
    default:
        return true;
    }
}

static LikenessMaterial LikenessClassifyPaletteSlot(PAL *pal, unsigned char ci)
{
    if (!pal || ci == 0) return LK_MATERIAL_ANY;
    int r, g, b, luma;
    LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
    if (LikenessIsBlackRgb(r, g, b, luma)) return LK_MATERIAL_BLACK;
    if (LikenessPaletteSlotMatchesMaterial(pal, ci, LK_MATERIAL_SKIN))
        return LK_MATERIAL_SKIN;
    if (LikenessPaletteSlotMatchesMaterial(pal, ci, LK_MATERIAL_BLUE))
        return LK_MATERIAL_BLUE;
    if (LikenessPaletteSlotMatchesMaterial(pal, ci, LK_MATERIAL_LOWER))
        return LK_MATERIAL_LOWER;
    return LK_MATERIAL_ANY;
}

static int LikenessNearestMaterialSlot(PAL *pal, const bool used_slots[256],
                                       LikenessMaterial material,
                                       int r, int g, int b,
                                       int prefer_r = -1,
                                       int prefer_g = -1,
                                       int prefer_b = -1,
                                       bool forbid_bad_green = false)
{
    if (!pal || !pal->data_p || !used_slots) return 0;
    r = LikenessClampByte(r);
    g = LikenessClampByte(g);
    b = LikenessClampByte(b);
    int want_luma = LikenessRgbLuma8(r, g, b);
    int prefer_luma = (prefer_r >= 0 && prefer_g >= 0 && prefer_b >= 0)
        ? LikenessRgbLuma8(prefer_r, prefer_g, prefer_b)
        : -1;

    int count = pal->numc;
    if (count > 256) count = 256;
    int pass_count = forbid_bad_green ? 2 : 4;
    for (int pass = 0; pass < pass_count; pass++) {
        bool require_material = (pass == 0 || pass == 2) &&
                                material != LK_MATERIAL_ANY;
        bool avoid_green = forbid_bad_green || pass < 2;
        int best = 0;
        double best_score = DBL_MAX;
        for (int i = 1; i < count; i++) {
            if (!used_slots[i]) continue;
            int pr, pg, pb, pl;
            LikenessPaletteRgb(pal, (unsigned char)i, &pr, &pg, &pb, &pl);
            bool bad_green = LikenessIsBadGreenRgb(pr, pg, pb, pl);
            if (avoid_green && bad_green) continue;
            bool material_match =
                LikenessPaletteSlotMatchesMaterial(pal, (unsigned char)i, material);
            if (require_material && !material_match) continue;

            int dr = pr - r, dg = pg - g, db = pb - b;
            int dl = pl - want_luma; if (dl < 0) dl = -dl;
            double score = (double)dl * 6.0 +
                           (double)(dr * dr + dg * dg + db * db) * 0.020;
            if (prefer_luma >= 0) {
                int pdr = pr - prefer_r, pdg = pg - prefer_g, pdb = pb - prefer_b;
                int pdl = pl - prefer_luma; if (pdl < 0) pdl = -pdl;
                score += (double)(pdr * pdr + pdg * pdg + pdb * pdb) * 0.010;
                score += (double)pdl * 0.75;
            }
            if (!require_material && material != LK_MATERIAL_ANY && !material_match)
                score += 160.0;
            if (material == LK_MATERIAL_BLACK)
                score += (double)pl * 2.5;
            if (bad_green)
                score += 9000.0;

            if (score < best_score) {
                best_score = score;
                best = i;
            }
        }
        if (best > 0) return best;
    }

    if (forbid_bad_green) return 0;
    return LikenessNearestUsedSlot(pal, used_slots, r, g, b);
}

static bool LikenessSourceSampleMatches(const LikenessSample &s,
                                        PAL *source_pal,
                                        LikenessMaterial material,
                                        const LikenessPartBox *source_head,
                                        bool strict_head)
{
    if (s.idx == 0) return false;
    if (material != LK_MATERIAL_ANY &&
        LikenessIsBadGreenRgb(s.r, s.g, s.b, s.luma))
        return false;

    if (material == LK_MATERIAL_HEAD &&
        source_head && source_head->valid && strict_head) {
        return LikenessPointInPartBox(*source_head, s.x, s.y) &&
               (LikenessIsHeadRgb(s.r, s.g, s.b, s.luma) ||
                LikenessPaletteSlotMatchesMaterial(source_pal, s.idx,
                                                   LK_MATERIAL_SKIN));
    }

    return LikenessPaletteSlotMatchesMaterial(source_pal, s.idx, material);
}

static bool LikenessSampleMaterialSource(const std::vector<LikenessSample> &samples,
                                         const std::vector<std::vector<int>> &bins,
                                         PAL *source_pal,
                                         LikenessMaterial material,
                                         const LikenessPartBox *source_head,
                                         float u, float v,
                                         LikenessColor *out,
                                         const LikenessSample **nearest_out)
{
    if (out) *out = {};
    if (nearest_out) *nearest_out = NULL;
    if (samples.empty() || !out) return false;

    const int G = 32;
    const float radii[] = { 0.052f, 0.088f, 0.145f, 0.24f, 0.39f, 0.66f };
    int attempts = (material == LK_MATERIAL_HEAD &&
                    source_head && source_head->valid) ? 2 : 1;
    for (int attempt = 0; attempt < attempts; attempt++) {
        bool strict_head = attempt == 0;
        for (float radius : radii) {
            float r2 = radius * radius;
            int bx0 = (int)((u - radius) * G);
            int bx1 = (int)((u + radius) * G);
            int by0 = (int)((v - radius) * G);
            int by1 = (int)((v + radius) * G);
            if (bx0 < 0) bx0 = 0; if (bx1 >= G) bx1 = G - 1;
            if (by0 < 0) by0 = 0; if (by1 >= G) by1 = G - 1;

            double wr = 0.0, wg = 0.0, wb = 0.0, wl = 0.0, wsum = 0.0;
            const LikenessSample *nearest = NULL;
            float nearest_d2 = 9999.0f;
            for (int by = by0; by <= by1; by++) {
                for (int bx = bx0; bx <= bx1; bx++) {
                    const std::vector<int> &bucket = bins[by * G + bx];
                    for (int si : bucket) {
                        const LikenessSample &s = samples[si];
                        if (!LikenessSourceSampleMatches(s, source_pal, material,
                                                         source_head, strict_head))
                            continue;
                        float dx = (s.u - u) * 1.10f;
                        float dy = s.v - v;
                        float d2 = dx * dx + dy * dy;
                        if (d2 > r2) continue;
                        if (d2 < nearest_d2) {
                            nearest_d2 = d2;
                            nearest = &s;
                        }
                        double wt = 1.0 / (0.0008 + (double)d2);
                        wr += (double)s.r * wt;
                        wg += (double)s.g * wt;
                        wb += (double)s.b * wt;
                        wl += (double)s.luma * wt;
                        wsum += wt;
                    }
                }
            }
            if (wsum > 0.0) {
                out->r = (float)(wr / wsum);
                out->g = (float)(wg / wsum);
                out->b = (float)(wb / wsum);
                out->luma = (float)(wl / wsum);
                if (nearest) {
                    out->r = out->r * 0.56f + nearest->r * 0.44f;
                    out->g = out->g * 0.56f + nearest->g * 0.44f;
                    out->b = out->b * 0.56f + nearest->b * 0.44f;
                    out->luma = out->luma * 0.56f + nearest->luma * 0.44f;
                    out->idx = nearest->idx;
                }
                out->found = true;
                if (nearest_out) *nearest_out = nearest;
                return true;
            }
        }
    }

    const LikenessSample *nearest = NULL;
    float best_d2 = 9999.0f;
    for (const LikenessSample &s : samples) {
        if (!LikenessSourceSampleMatches(s, source_pal, material,
                                         source_head, false))
            continue;
        float dx = (s.u - u) * 1.10f;
        float dy = s.v - v;
        float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            nearest = &s;
        }
    }
    if (!nearest) return false;
    out->r = (float)nearest->r;
    out->g = (float)nearest->g;
    out->b = (float)nearest->b;
    out->luma = (float)nearest->luma;
    out->idx = nearest->idx;
    out->found = true;
    if (nearest_out) *nearest_out = nearest;
    return true;
}

static int LikenessLocalSourceMeanLuma(const unsigned char *pix,
                                       int w, int h, int stride,
                                       PAL *pal,
                                       const LikenessSample *sample,
                                       LikenessMaterial material,
                                       const LikenessPartBox *source_head)
{
    if (!pix || !pal || !sample) return 128;
    int radius = (material == LK_MATERIAL_HEAD) ? 1 : 2;
    int sum = 0, count = 0;
    for (int dy = -radius; dy <= radius; dy++) {
        for (int dx = -radius; dx <= radius; dx++) {
            int x = sample->x + dx;
            int y = sample->y + dy;
            if (x < 0 || y < 0 || x >= w || y >= h) continue;
            unsigned char ci = pix[(size_t)y * stride + x];
            if (ci == 0) continue;
            int r, g, b, luma;
            LikenessPaletteRgb(pal, ci, &r, &g, &b, &luma);
            LikenessSample temp = {};
            temp.x = x; temp.y = y; temp.idx = ci;
            temp.r = r; temp.g = g; temp.b = b; temp.luma = luma;
            if (!LikenessSourceSampleMatches(temp, pal, material,
                                             source_head, false))
                continue;
            sum += luma;
            count++;
        }
    }
    return count > 0 ? sum / count : sample->luma;
}

static void LikenessBuildActorMasks(const unsigned char *target_original,
                                    int w, int h, int stride,
                                    PAL *target_pal,
                                    const LikenessBBox &tb,
                                    std::vector<unsigned char> &head_mask,
                                    std::vector<unsigned char> &skin_mask,
                                    int *head_pixels,
                                    int *skin_pixels)
{
    size_t bytes = (size_t)stride * h;
    head_mask.assign(bytes, 0);
    skin_mask.assign(bytes, 0);
    if (head_pixels) *head_pixels = 0;
    if (skin_pixels) *skin_pixels = 0;
    if (!target_original || !target_pal) return;

    LikenessPartBox target_head = {};
    LikenessFindHeadPart(target_original, w, h, stride, target_pal, tb,
                         true, &target_head);
    LikenessPartBox head_zone = target_head;
    if (head_zone.valid)
        LikenessExpandPartBox(&head_zone, w, h, 1, 1);

    float span_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * stride + x;
            unsigned char ci = target_original[off];
            if (ci == 0) continue;
            int r, g, b, luma;
            LikenessPaletteRgb(target_pal, ci, &r, &g, &b, &luma);
            float u = ((float)x - (float)tb.x1) / span_x;
            float v = ((float)y - (float)tb.y1) / span_y;
            bool headish = LikenessIsHeadRgb(r, g, b, luma) ||
                            LikenessIsHeadPixel(target_pal, ci);
            bool skinish = LikenessIsSkinRgb(r, g, b, luma);
            bool in_head_zone = LikenessPointInPartBox(head_zone, x, y);
            bool fallback_head = !head_zone.valid &&
                                 v < 0.56f && u > 0.08f && u < 0.94f;
            if (headish && (in_head_zone || fallback_head)) {
                head_mask[off] = 1;
                skin_mask[off] = 1;
            } else if (skinish && v < 0.82f &&
                       !LikenessIsBlueRgb(r, g, b, luma) &&
                       !LikenessIsLowerRgb(r, g, b, luma)) {
                skin_mask[off] = 1;
            }
        }
    }

    std::vector<unsigned char> dilated = head_mask;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * stride + x;
            if (target_original[off] == 0 || head_mask[off]) continue;
            if (head_zone.valid && !LikenessPointInPartBox(head_zone, x, y))
                continue;
            float v = ((float)y - (float)tb.y1) / span_y;
            if (v > 0.72f) continue;
            int r, g, b, luma;
            LikenessPaletteRgb(target_pal, target_original[off], &r, &g, &b, &luma);
            if (luma < 78 || LikenessIsBlueRgb(r, g, b, luma) ||
                LikenessIsLowerRgb(r, g, b, luma) ||
                LikenessIsBadGreenRgb(r, g, b, luma))
                continue;
            bool near_head = false;
            for (int dy = -1; dy <= 1 && !near_head; dy++) {
                for (int dx = -1; dx <= 1 && !near_head; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx, ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    if (head_mask[(size_t)ny * stride + nx]) near_head = true;
                }
            }
            if (near_head) {
                dilated[off] = 1;
                skin_mask[off] = 1;
            }
        }
    }
    head_mask.swap(dilated);

    int hc = 0, sc = 0;
    for (size_t i = 0; i < bytes; i++) {
        if (head_mask[i]) hc++;
        if (skin_mask[i]) sc++;
    }
    if (head_pixels) *head_pixels = hc;
    if (skin_pixels) *skin_pixels = sc;
}

static LikenessMaterial LikenessTargetMaterial(const unsigned char *target_original,
                                               int w, int h, int stride,
                                               PAL *target_pal,
                                               int x, int y,
                                               const std::vector<unsigned char> &head_mask,
                                               const std::vector<unsigned char> &skin_mask,
                                               float v)
{
    size_t off = (size_t)y * stride + x;
    if (!target_original || target_original[off] == 0 || !target_pal)
        return LK_MATERIAL_ANY;
    if (off < head_mask.size() && head_mask[off])
        return LK_MATERIAL_HEAD;
    if (off < skin_mask.size() && skin_mask[off])
        return LK_MATERIAL_SKIN;

    unsigned char ci = target_original[off];
    int r, g, b, luma;
    LikenessPaletteRgb(target_pal, ci, &r, &g, &b, &luma);
    int edge = LikenessTransparentNeighborCount(target_original, w, h, stride, x, y);
    if (edge >= 4 && LikenessIsBlackRgb(r, g, b, luma))
        return LK_MATERIAL_BLACK;
    if (LikenessIsBlueRgb(r, g, b, luma))
        return LK_MATERIAL_BLUE;
    if (LikenessIsGoldPixel(target_pal, ci) ||
        LikenessIsLowerRgb(r, g, b, luma))
        return LK_MATERIAL_LOWER;
    if (LikenessIsSkinRgb(r, g, b, luma) && v < 0.82f)
        return LK_MATERIAL_SKIN;
    if (v > 0.56f)
        return LK_MATERIAL_LOWER;
    if (luma < 48)
        return LK_MATERIAL_BLUE;
    return LK_MATERIAL_BLUE;
}

static void LikenessScaleRgbToLuma(float r, float g, float b, float target_luma,
                                   int *out_r, int *out_g, int *out_b)
{
    float src_luma = (float)LikenessRgbLuma8((int)(r + 0.5f),
                                            (int)(g + 0.5f),
                                            (int)(b + 0.5f));
    float scale = target_luma / (src_luma < 1.0f ? 1.0f : src_luma);
    *out_r = LikenessClampByte((int)(r * scale + 0.5f));
    *out_g = LikenessClampByte((int)(g * scale + 0.5f));
    *out_b = LikenessClampByte((int)(b * scale + 0.5f));
}

static int LikenessApplyActorComposite(const unsigned char *source_pix,
                                       int sw, int sh, int ss,
                                       const unsigned char *target_original,
                                       int tw, int th, int ts,
                                       unsigned char *target_pix,
                                       PAL *source_pal, PAL *target_pal,
                                       const LikenessBBox &sb,
                                       const LikenessBBox &tb,
                                       const std::vector<LikenessSample> &samples,
                                       const std::vector<std::vector<int>> &bins,
                                       const bool used_slots[256],
                                       const LikenessPartBox &source_head,
                                       const std::vector<unsigned char> &head_mask,
                                       const std::vector<unsigned char> &skin_mask,
                                       bool mirror,
                                       bool palette_changed,
                                       int *opaque_out,
                                       int *edge_darkened_out)
{
    if (opaque_out) *opaque_out = 0;
    if (edge_darkened_out) *edge_darkened_out = 0;
    if (!source_pix || !target_original || !target_pix ||
        !source_pal || !target_pal)
        return 0;

    float span_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int changed = 0;
    int opaque = 0;
    int edge_darkened = 0;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * ts + x;
            unsigned char old_ci = target_original[off];
            if (old_ci == 0) continue;
            opaque++;

            float u = ((float)x - (float)tb.x1) / span_x;
            float v = ((float)y - (float)tb.y1) / span_y;
            float su = u;
            float sv = v;
            if (sb.anchor_ok && tb.anchor_ok) {
                float src_anchor_u = mirror ? (1.0f - sb.anchor_u) : sb.anchor_u;
                float anchor_u = src_anchor_u + (u - tb.anchor_u);
                float anchor_v = sb.anchor_v + (v - tb.anchor_v);
                su = u * 0.82f + anchor_u * 0.18f;
                sv = v * 0.84f + anchor_v * 0.16f;
            }
            if (su < 0.0f) su = 0.0f; if (su > 1.0f) su = 1.0f;
            if (sv < 0.0f) sv = 0.0f; if (sv > 1.0f) sv = 1.0f;

            int tr, tg, tbv, tluma;
            LikenessPaletteRgb(target_pal, old_ci, &tr, &tg, &tbv, &tluma);
            LikenessMaterial material = LikenessTargetMaterial(target_original,
                                                              tw, th, ts,
                                                              target_pal,
                                                              x, y,
                                                              head_mask,
                                                              skin_mask,
                                                              v);
            LikenessColor src_col = {};
            const LikenessSample *nearest = NULL;
            if (!LikenessSampleMaterialSource(samples, bins, source_pal,
                                              material, &source_head,
                                              su, sv, &src_col, &nearest)) {
                if (!LikenessSampleMaterialSource(samples, bins, source_pal,
                                                  LK_MATERIAL_ANY, NULL,
                                                  su, sv, &src_col, &nearest))
                    continue;
            }
            if (!src_col.found || src_col.idx == 0) continue;

            int local_mean = LikenessLocalSourceMeanLuma(source_pix, sw, sh, ss,
                                                        source_pal, nearest,
                                                        material, &source_head);
            float hi = src_col.luma - (float)local_mean;
            float source_pull = 0.13f;
            float texture_strength = 0.28f;
            float target_rgb_keep = 0.10f;
            switch (material) {
            case LK_MATERIAL_HEAD:
                source_pull = 0.10f;
                texture_strength = 0.15f;
                target_rgb_keep = 0.24f;
                break;
            case LK_MATERIAL_SKIN:
                source_pull = 0.14f;
                texture_strength = 0.18f;
                target_rgb_keep = 0.18f;
                break;
            case LK_MATERIAL_BLUE:
                source_pull = 0.13f;
                texture_strength = 0.30f;
                target_rgb_keep = 0.08f;
                break;
            case LK_MATERIAL_LOWER:
                source_pull = 0.12f;
                texture_strength = 0.34f;
                target_rgb_keep = 0.07f;
                break;
            case LK_MATERIAL_BLACK:
                source_pull = 0.08f;
                texture_strength = 0.18f;
                target_rgb_keep = 0.16f;
                break;
            default:
                break;
            }

            float desired_luma = (float)tluma * (1.0f - source_pull) +
                                 src_col.luma * source_pull +
                                 hi * texture_strength;
            int edge = LikenessTransparentNeighborCount(target_original,
                                                        tw, th, ts, x, y);
            unsigned int h = (unsigned int)(x * 73856093u) ^
                             (unsigned int)(y * 19349663u) ^
                             (unsigned int)(src_col.idx * 83492791u);
            if (edge > 0 &&
                material != LK_MATERIAL_HEAD &&
                material != LK_MATERIAL_SKIN &&
                (edge >= 5 || (edge >= 3 && (h & 15u) < 3u))) {
                desired_luma *= edge >= 5 ? 0.72f : 0.84f;
                edge_darkened++;
            }
            if (desired_luma < 6.0f) desired_luma = 6.0f;
            if (desired_luma > 244.0f) desired_luma = 244.0f;

            int rr, gg, bb;
            LikenessScaleRgbToLuma(src_col.r, src_col.g, src_col.b,
                                   desired_luma, &rr, &gg, &bb);
            rr = (int)((float)rr * (1.0f - target_rgb_keep) +
                       (float)tr * target_rgb_keep + 0.5f);
            gg = (int)((float)gg * (1.0f - target_rgb_keep) +
                       (float)tg * target_rgb_keep + 0.5f);
            bb = (int)((float)bb * (1.0f - target_rgb_keep) +
                       (float)tbv * target_rgb_keep + 0.5f);

            int new_ci = LikenessNearestMaterialSlot(source_pal, used_slots,
                                                     material, rr, gg, bb,
                                                     (int)src_col.r,
                                                     (int)src_col.g,
                                                     (int)src_col.b);
            if (new_ci <= 0) continue;
            if (target_pix[off] != (unsigned char)new_ci || palette_changed) {
                target_pix[off] = (unsigned char)new_ci;
                changed++;
            }
        }
    }
    if (opaque_out) *opaque_out = opaque;
    if (edge_darkened_out) *edge_darkened_out = edge_darkened;
    return changed;
}

static int LikenessApplyActorAccents(const unsigned char *source_pix,
                                     int sw, int sh, int ss,
                                     const unsigned char *target_original,
                                     int tw, int th, int ts,
                                     unsigned char *target_pix,
                                     PAL *source_pal, PAL *target_pal,
                                     const LikenessBBox &sb,
                                     const LikenessBBox &tb,
                                     bool mirror,
                                     const bool used_slots[256],
                                     const LikenessPartBox &source_head,
                                     const std::vector<unsigned char> &head_mask,
                                     const std::vector<unsigned char> &skin_mask,
                                     std::vector<unsigned char> &accent_mask)
{
    if (!source_pix || !target_original || !target_pix ||
        !source_pal || !target_pal)
        return 0;

    size_t bytes = (size_t)ts * th;
    accent_mask.assign(bytes, 0);
    std::vector<int> best_score(bytes, 0x7FFFFFFF);
    float tspan_x = (tb.w > 1) ? (float)(tb.w - 1) : 1.0f;
    float tspan_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int changed = 0;

    for (int sy = sb.y1; sy <= sb.y2; sy++) {
        for (int sx = sb.x1; sx <= sb.x2; sx++) {
            unsigned char sci = source_pix[(size_t)sy * ss + sx];
            if (sci == 0 || LikenessIsBadGreenPixel(source_pal, sci))
                continue;

            float raw_u = ((float)sx - (float)sb.x1) /
                          ((sb.w > 1) ? (float)(sb.w - 1) : 1.0f);
            float raw_v = ((float)sy - (float)sb.y1) /
                          ((sb.h > 1) ? (float)(sb.h - 1) : 1.0f);
            LikenessMaterial sm = LikenessClassifyPaletteSlot(source_pal, sci);
            bool contrast = LikenessHasLocalContrast(source_pix, sw, sh, ss,
                                                     sx, sy, source_pal);
            bool edge = LikenessTransparentNeighborCount(source_pix, sw, sh, ss,
                                                         sx, sy) > 0;
            bool in_source_head = LikenessPointInPartBox(source_head, sx, sy);
            bool gold_accent = sm == LK_MATERIAL_LOWER &&
                               raw_v < 0.82f &&
                               (contrast || edge ||
                                LikenessIsGoldPixel(source_pal, sci));
            bool skin_accent = (sm == LK_MATERIAL_SKIN ||
                                sm == LK_MATERIAL_HEAD) &&
                               raw_v < 0.58f &&
                               contrast && !in_source_head;
            if (!gold_accent && !skin_accent)
                continue;

            float tu, tv;
            if (!LikenessProjectSourceToTarget(raw_u, raw_v, sb, tb, mirror,
                                               &tu, &tv))
                continue;
            int tx0 = LikenessClampInt((int)(tb.x1 + tu * tspan_x + 0.5f),
                                       0, tw - 1);
            int ty0 = LikenessClampInt((int)(tb.y1 + tv * tspan_y + 0.5f),
                                       0, th - 1);
            int search_radius = contrast ? 2 : 1;
            if (gold_accent && raw_v > 0.46f) search_radius = 3;

            int best_off = -1;
            int best = 0x7FFFFFFF;
            LikenessMaterial best_tm = LK_MATERIAL_ANY;
            float best_v = 0.0f;
            for (int dy = -search_radius; dy <= search_radius; dy++) {
                for (int dx = -search_radius; dx <= search_radius; dx++) {
                    int tx = tx0 + dx;
                    int ty = ty0 + dy;
                    if (tx < 0 || ty < 0 || tx >= tw || ty >= th) continue;
                    size_t off = (size_t)ty * ts + tx;
                    if (target_original[off] == 0 || accent_mask[off])
                        continue;
                    float tvn = ((float)ty - (float)tb.y1) / tspan_y;
                    LikenessMaterial tm = LikenessTargetMaterial(target_original,
                                                                 tw, th, ts,
                                                                 target_pal,
                                                                 tx, ty,
                                                                 head_mask,
                                                                 skin_mask,
                                                                 tvn);
                    bool allowed = false;
                    if (gold_accent) {
                        allowed = tm == LK_MATERIAL_BLUE ||
                                  tm == LK_MATERIAL_LOWER ||
                                  tm == LK_MATERIAL_BLACK;
                    } else if (skin_accent) {
                        allowed = tm == LK_MATERIAL_SKIN ||
                                  tm == LK_MATERIAL_HEAD;
                    }
                    if (!allowed) continue;

                    int dist2 = dx * dx + dy * dy;
                    int mismatch = LikenessMaskMismatch(source_pix, sw, sh, ss,
                                                        sx, sy,
                                                        target_original,
                                                        tw, th, ts, tx, ty);
                    int target_edge = LikenessTransparentNeighborCount(
                        target_original, tw, th, ts, tx, ty);
                    int score = dist2 * 10 + mismatch * 8;
                    if (gold_accent && tm == LK_MATERIAL_BLUE) score -= 7;
                    if (gold_accent && tm == LK_MATERIAL_LOWER) score -= 5;
                    if (skin_accent && tm == LK_MATERIAL_SKIN) score -= 6;
                    if (target_edge > 0) score -= 2;
                    if (score < best) {
                        best = score;
                        best_off = (int)off;
                        best_tm = tm;
                        best_v = tvn;
                    }
                }
            }

            int threshold = gold_accent ? 58 : 44;
            if (best_off < 0 || best > threshold ||
                best >= best_score[(size_t)best_off])
                continue;
            best_score[(size_t)best_off] = best;

            unsigned char old_tci = target_original[(size_t)best_off];
            int tr, tg, tbv, tluma;
            LikenessPaletteRgb(target_pal, old_tci, &tr, &tg, &tbv, &tluma);
            int sr, sg, sbc, sluma;
            LikenessPaletteRgb(source_pal, sci, &sr, &sg, &sbc, &sluma);
            float pull = gold_accent ? 0.38f : 0.28f;
            if (best_tm == LK_MATERIAL_LOWER && best_v > 0.58f)
                pull = 0.30f;
            float want_luma = (float)tluma * (1.0f - pull) +
                              (float)sluma * pull;
            int rr, gg, bb;
            LikenessScaleRgbToLuma((float)sr, (float)sg, (float)sbc,
                                   want_luma, &rr, &gg, &bb);
            int new_ci = LikenessNearestMaterialSlot(source_pal, used_slots,
                                                     sm, rr, gg, bb,
                                                     sr, sg, sbc);
            if (new_ci <= 0 || target_pix[(size_t)best_off] == (unsigned char)new_ci)
                continue;
            target_pix[(size_t)best_off] = (unsigned char)new_ci;
            accent_mask[(size_t)best_off] = 1;
            changed++;
        }
    }
    return changed;
}

static int LikenessCleanActorPaletteOutliers(unsigned char *target_pix,
                                             const unsigned char *target_original,
                                             int w, int h, int stride,
                                             PAL *source_pal, PAL *target_pal,
                                             const bool used_slots[256],
                                             const LikenessBBox &tb,
                                             const std::vector<unsigned char> &head_mask,
                                             const std::vector<unsigned char> &skin_mask)
{
    if (!target_pix || !target_original || !source_pal || !target_pal)
        return 0;

    float span_y = (tb.h > 1) ? (float)(tb.h - 1) : 1.0f;
    int changed = 0;
    for (int y = tb.y1; y <= tb.y2; y++) {
        for (int x = tb.x1; x <= tb.x2; x++) {
            size_t off = (size_t)y * stride + x;
            unsigned char ci = target_pix[off];
            if (target_original[off] == 0 || ci == 0 ||
                !LikenessIsBadGreenPixel(source_pal, ci))
                continue;
            float v = ((float)y - (float)tb.y1) / span_y;
            LikenessMaterial material = LikenessTargetMaterial(target_original,
                                                              w, h, stride,
                                                              target_pal,
                                                              x, y,
                                                              head_mask,
                                                              skin_mask,
                                                              v);
            int tr, tg, tbv, tluma;
            LikenessPaletteRgb(target_pal, target_original[off],
                               &tr, &tg, &tbv, &tluma);
            int new_ci = LikenessNearestMaterialSlot(source_pal, used_slots,
                                                     material,
                                                     tr, tg, tbv,
                                                     -1, -1, -1,
                                                     true);
            if (new_ci <= 0 || new_ci == ci)
                continue;
            target_pix[off] = (unsigned char)new_ci;
            changed++;
        }
    }
    return changed;
}

static int ApplyMarkedLikenessToSelected(void)
{
    int target_idx = g_doc->ilselected;
    IMG *target = (target_idx >= 0) ? get_img(target_idx) : NULL;
    if (!target || !target->data_p || target->w == 0 || target->h == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Select a target sprite first.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    int source_idx = -1;
    int marked_sources = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!(img->flags & 1) || idx == target_idx) continue;
        source_idx = idx;
        marked_sources++;
    }
    if (marked_sources != 1) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark exactly one source sprite, then select a different target sprite.");
        g_restore_msg_timer = 5.0f;
        return 0;
    }

    IMG *source = get_img(source_idx);
    PAL *source_pal = source ? get_pal((int)source->palnum) : NULL;
    PAL *target_pal = get_pal((int)target->palnum);
    if (!source || !source->data_p || source->w == 0 || source->h == 0 ||
        !source_pal || !source_pal->data_p || !target_pal || !target_pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Source and target sprites need valid palettes.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    LikenessBBox sb = {}, tb = {};
    if (!LikenessOpaqueBBox(source, &sb) || !LikenessOpaqueBBox(target, &tb)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Source and target need non-transparent pixels.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<LikenessSample> samples;
    bool used_slots[256];
    LikenessBuildSamples(source, source_pal, sb, samples, used_slots, NULL);
    if (samples.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Marked source has no visible pixels to transfer.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    float normal_score = LikenessMaskScore(target, tb, samples, false);
    float mirror_score = LikenessMaskScore(target, tb, samples, true);
    bool mirror = mirror_score > normal_score * 1.08f;

    const int G = 32;
    std::vector<std::vector<int>> bins((size_t)G * G);
    for (int i = 0; i < (int)samples.size(); i++) {
        if (mirror) samples[i].u = 1.0f - samples[i].u;
        int bx = (int)(samples[i].u * G);
        int by = (int)(samples[i].v * G);
        if (bx < 0) bx = 0; if (bx >= G) bx = G - 1;
        if (by < 0) by = 0; if (by >= G) by = G - 1;
        bins[by * G + bx].push_back(i);
    }

    if (!doc_undo_push()) return 0;
    target = get_img(target_idx);
    source = get_img(source_idx);
    source_pal = source ? get_pal((int)source->palnum) : NULL;
    target_pal = target ? get_pal((int)target->palnum) : NULL;
    if (!target || !target->data_p || !source || !source_pal || !target_pal)
        return 0;

    int target_stride = (target->w + 3) & ~3;
    unsigned char *target_pix = (unsigned char *)target->data_p;
    size_t target_bytes = (size_t)target_stride * target->h;
    std::vector<unsigned char> target_original(target_bytes);
    memcpy(target_original.data(), target_pix, target_bytes);
    int source_stride = (source->w + 3) & ~3;
    const unsigned char *source_pix = (const unsigned char *)source->data_p;
    bool palette_changed = target->palnum != source->palnum;
    LikenessPartBox source_head = {};
    LikenessFindHeadPart(source_pix, source->w, source->h, source_stride,
                         source_pal, sb, true, &source_head);
    if (source_head.valid)
        LikenessExpandPartBox(&source_head, source->w, source->h, 1, 1);

    LikenessActorStats stats = {};
    std::vector<unsigned char> head_mask;
    std::vector<unsigned char> skin_mask;
    LikenessBuildActorMasks(target_original.data(),
                            target->w, target->h, target_stride,
                            target_pal, tb,
                            head_mask, skin_mask,
                            &stats.head_pixels, &stats.skin_pixels);

    stats.remapped = LikenessApplyActorComposite(source_pix,
                                                 source->w, source->h, source_stride,
                                                 target_original.data(),
                                                 target->w, target->h, target_stride,
                                                 target_pix,
                                                 source_pal, target_pal,
                                                 sb, tb,
                                                 samples, bins, used_slots,
                                                 source_head,
                                                 head_mask, skin_mask,
                                                 mirror, palette_changed,
                                                 &stats.opaque,
                                                 &stats.edge_darkened);

    std::vector<unsigned char> accent_mask;
    stats.accents = LikenessApplyActorAccents(source_pix,
                                              source->w, source->h, source_stride,
                                              target_original.data(),
                                              target->w, target->h, target_stride,
                                              target_pix,
                                              source_pal, target_pal,
                                              sb, tb, mirror,
                                              used_slots,
                                              source_head,
                                              head_mask, skin_mask,
                                              accent_mask);
    stats.cleaned = LikenessCleanActorPaletteOutliers(target_pix,
                                                      target_original.data(),
                                                      target->w, target->h,
                                                      target_stride,
                                                      source_pal, target_pal,
                                                      used_slots, tb,
                                                      head_mask, skin_mask);
    int total_changed = stats.remapped + stats.accents + stats.cleaned;

    target->palnum = source->palnum;
    g_doc->plselected = source->palnum;
    ApplyPalette(source->palnum);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    InvalidateThumb(target_idx);
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Applied %s likeness to %s: %d/%d remapped, %d accents, %d cleaned, %d head px%s.",
             source->n_s, target->n_s,
             stats.remapped, stats.opaque, stats.accents, stats.cleaned,
             stats.head_pixels,
             mirror ? " (mirrored source fit)" : "");
    g_restore_msg_timer = 6.0f;
    return total_changed;
}

static void unlink_and_free_img(IMG *victim)
{
    if (!victim) return;
    IMG *prev = NULL;
    IMG *cur = (IMG *)g_doc->img_p;
    while (cur && cur != victim) { prev = cur; cur = (IMG *)cur->nxt_p; }
    if (cur == victim) {
        if (prev) prev->nxt_p = cur->nxt_p;
        else g_doc->img_p = cur->nxt_p;
        g_doc->imgcnt--;
    }
    FreeImg(victim);
}

static void SplitSelectionToOverlayFrame(bool clear_source)
{
    IMG *src = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return;
    if (!g_grid_sel.active) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select pixels first, then split an overlay frame.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int stride = (src->w + 3) & ~3;
    unsigned int sz = (unsigned int)stride * src->h;
    PixelHist snap = {};
    bool have_snap = clear_source && pixel_hist_capture(&snap);

    IMG *dst = (IMG *)AllocImg();
    if (!dst) {
        if (have_snap) pixel_hist_free(&snap);
        return;
    }

    dst->w = src->w; dst->h = src->h;
    dst->palnum = src->palnum; dst->flags = src->flags;
    dst->anix = src->anix; dst->aniy = src->aniy;
    dst->anix2 = src->anix2; dst->aniy2 = src->aniy2; dst->aniz2 = src->aniz2;
    dst->opals = src->opals;
    strncpy(dst->src_filename, src->src_filename, sizeof(dst->src_filename) - 1);
    dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';
    snprintf(dst->n_s, sizeof(dst->n_s), "%.12sOVR", src->n_s);

    dst->data_p = PoolAlloc(sz);
    if (!dst->data_p) {
        if (have_snap) pixel_hist_free(&snap);
        unlink_and_free_img(dst);
        return;
    }

    unsigned char *sp = (unsigned char *)src->data_p;
    unsigned char *dp = (unsigned char *)dst->data_p;
    int copied = 0;
    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            if (!selection_contains_pixel(src, x, y)) continue;
            unsigned char v = sp[y * stride + x];
            if (v == 0) continue;
            dp[y * stride + x] = v;
            if (clear_source) sp[y * stride + x] = 0;
            copied++;
        }
    }

    if (copied == 0) {
        if (have_snap) pixel_hist_free(&snap);
        unlink_and_free_img(dst);
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No opaque selected pixels to split.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    if (have_snap) {
        snap.seq = ++g_undo_seq;
        if (g_pixel_hist.size() >= kPixelHistMax) {
            pixel_hist_free(&g_pixel_hist.front());
            g_pixel_hist.erase(g_pixel_hist.begin());
        }
        g_pixel_hist.push_back(snap);
        for (auto &redo : g_pixel_redo) pixel_hist_free(&redo);
        g_pixel_redo.clear();
        ClearDocumentRedoStack();
    }

    g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "%s %d px into overlay frame.",
             clear_source ? "Split" : "Copied", copied);
    g_restore_msg_timer = 4.0f;
}

/* ---- Hard Stroke Remover ----
   Detects a 1-2 px matte/outline ring around transparent sprite edges. Unlike
   Strip Edge, this requires the edge color to contrast against nearby inner
   sprite colors, so normal antialiasing and same-color silhouette detail are
   less likely to be erased. */
static int RemoveHardStrokeFromImage(IMG *img, PAL *pal, int max_width, bool apply)
{
    if (!img || !img->data_p || !pal || !pal->data_p ||
        img->w == 0 || img->h == 0)
        return 0;
    if (max_width < 1) max_width = 1;
    if (max_width > 2) max_width = 2;

    int w = img->w;
    int h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *pixels = (unsigned char *)img->data_p;
    size_t bytes = (size_t)stride * h;
    std::vector<unsigned char> work(bytes);
    memcpy(work.data(), pixels, bytes);
    int changed = 0;
    std::vector<std::pair<int, unsigned char>> writes;

    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = work[(size_t)y * stride + x];
            if (ci == 0) continue;

            int trans = EdgeBufferTransparentNeighbors(work.data(), w, h, stride, x, y);
            if (trans <= 0) continue;

            unsigned short edge_word = pal_word_or_black(pal, ci);
            int edge_luma = StrokeWordLuma8(edge_word);
            int same_edge_neighbors = 0;
            int interior_count = 0;
            int interior_luma_sum = 0;
            int min_dist = 0x7FFFFFFF;
            unsigned char inward_ci = 0;
            bool have_inward = FindInwardEdgeReplacement(work.data(), w, h, stride,
                                                         x, y, pal, ci, max_width,
                                                         &inward_ci);

            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    if (dx == 0 && dy == 0) continue;
                    int nx = x + dx;
                    int ny = y + dy;
                    if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                    unsigned char ni = work[(size_t)ny * stride + nx];
                    if (ni == 0) continue;
                    unsigned short nw = pal_word_or_black(pal, ni);
                    int dist = PaletteColorDistance5(edge_word, nw);
                    if (dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1 &&
                        dist <= 6)
                        same_edge_neighbors++;

                    if (dist <= 6) continue;
                    interior_count++;
                    interior_luma_sum += StrokeWordLuma8(nw);
                    if (dist < min_dist) min_dist = dist;
                }
            }

            if (interior_count <= 0 || !have_inward) continue;
            int avg_luma = interior_luma_sum / interior_count;
            int luma_delta = avg_luma - edge_luma;
            if (luma_delta < 0) luma_delta = -luma_delta;

            bool hard_color_step = min_dist >= 28 || luma_delta >= 34;
            bool dark_outline = edge_luma + 20 < avg_luma && min_dist >= 12;
            bool strong_inward = EdgeColorStrongVariant(pal, ci, inward_ci);
            bool stroke_supported = same_edge_neighbors >= 1 || trans >= 3;
            if (stroke_supported && strong_inward && (hard_color_step || dark_outline)) {
                writes.push_back({(int)((size_t)y * stride + x), inward_ci});
            }
        }
    }

    for (const auto &wr : writes) {
        if (work[(size_t)wr.first] != wr.second) {
            work[(size_t)wr.first] = wr.second;
            changed++;
        }
    }

    if (apply && changed > 0)
        memcpy(pixels, work.data(), bytes);
    return changed;
}

static int RemoveHardStrokeFromTargets(int max_width)
{
    int marked = CountMarkedImages();
    int selected = g_doc->ilselected;
    if (marked == 0 && selected < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark sprites, or select one sprite, before removing hard strokes.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<int> changed_indices;
    int expected_pixels = 0;
    int scanned = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        bool target = marked > 0 ? ((img->flags & 1) != 0) : (idx == selected);
        if (!target || !img->data_p || img->w == 0 || img->h == 0) continue;
        scanned++;
        PAL *pal = get_pal((int)img->palnum);
        int n = RemoveHardStrokeFromImage(img, pal, max_width, false);
        if (n > 0) {
            changed_indices.push_back(idx);
            expected_pixels += n;
        }
    }

    if (expected_pixels <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No hard 1-2px stroke found in %d sprite%s.",
                 scanned, scanned == 1 ? "" : "s");
        g_restore_msg_timer = 5.0f;
        return 0;
    }

    doc_undo_push();
    int changed_images = 0;
    int changed_pixels = 0;
    for (int changed_idx : changed_indices) {
        IMG *img = get_img(changed_idx);
        PAL *pal = img ? get_pal((int)img->palnum) : NULL;
        int n = RemoveHardStrokeFromImage(img, pal, max_width, true);
        if (n > 0) {
            changed_images++;
            changed_pixels += n;
            InvalidateThumb(changed_idx);
        }
    }

    if (changed_pixels > 0) {
        g_img_tex_idx = -2;
        mark_dirty();
    }

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Removed %d hard-stroke pixel%s from %d/%d sprite%s.",
             changed_pixels,
             changed_pixels == 1 ? "" : "s",
             changed_images,
             scanned,
             scanned == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    return changed_pixels;
}

/* ---- Strip Edge (DMA Compression Prep) ---- */
static void StripMarkedImages(int max_transparent_neighbors, int specific_color = -1)
{
    if (max_transparent_neighbors < 1) max_transparent_neighbors = 1;
    if (max_transparent_neighbors > 8) max_transparent_neighbors = 8;

    bool undo_pushed = false;
    int changed_images = 0;
    int changed_pixels = 0;
    int scanned = 0;
    int idx = 0;
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            scanned++;
            int w = img->w;
            int h = img->h;
            int stride = (w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            size_t bytes = (size_t)stride * h;

            std::vector<unsigned char> original(bytes);
            std::vector<unsigned char> flags(bytes, 0);
            if (original.empty() || flags.empty()) {
                img = (IMG *)img->nxt_p;
                idx++;
                continue;
            }
            memcpy(original.data(), pixels, bytes);

            /* Bounds checking counts out-of-bounds as transparent (matches ASM logic) */
            auto is_transparent = [&](int x, int y) -> bool {
                if (x < 0 || x >= w || y < 0 || y >= h) return true;
                return original[(size_t)y * stride + x] == 0;
            };

            /* Pass 1: flag the original outer ring, then repaint only those
               saved edge pixels from a nearby inward color. This keeps the
               operation from chasing its own freshly edited pixels inward. */
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char c = original[(size_t)y * stride + x];
                    if (c == 0) continue;
                    if (specific_color >= 0 && c != specific_color) continue;

                    int trans_count = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            if (is_transparent(x + dx, y + dy)) trans_count++;
                        }
                    }

                    if (trans_count >= 2 && trans_count <= max_transparent_neighbors) {
                        flags[(size_t)y * stride + x] = 1;
                    }
                }
            }

            PAL *pal = get_pal((int)img->palnum);
            std::vector<std::pair<int, unsigned char>> edge_writes;
            if (pal && pal->data_p) {
                for (int y = 0; y < h; y++) {
                    for (int x = 0; x < w; x++) {
                        int off = y * stride + x;
                        if (!flags[(size_t)off]) continue;
                        unsigned char edge_ci = original[(size_t)off];
                        unsigned char repl = 0;
                        if (FindInwardEdgeReplacement(original.data(), w, h, stride,
                                                      x, y, pal, edge_ci, 2, &repl) &&
                            EdgeColorStrongVariant(pal, edge_ci, repl) &&
                            repl != edge_ci) {
                            edge_writes.push_back({off, repl});
                        }
                    }
                }
            }

            if (!edge_writes.empty() && !undo_pushed) {
                if (!doc_undo_push()) return;
                undo_pushed = true;
            }

            int image_changes = 0;
            for (const auto &wr : edge_writes) {
                if (pixels[wr.first] != wr.second) {
                    pixels[wr.first] = wr.second;
                    image_changes++;
                }
            }

            memset(flags.data(), 0, bytes);

            /* Pass 2: Flag lonely pixels (stray dust specs) */
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char c = pixels[(size_t)y * stride + x];
                    if (c == 0) continue;
                    if (specific_color >= 0 && c != specific_color) continue;

                    int trans_count = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            if (x + dx < 0 || x + dx >= w ||
                                y + dy < 0 || y + dy >= h ||
                                pixels[(size_t)(y + dy) * stride + (x + dx)] == 0)
                                trans_count++;
                        }
                    }
                    if (trans_count == 8) flags[(size_t)y * stride + x] = 1;
                }
            }

            /* Pass 2: Delete flagged lonely pixels */
            bool has_lonely = false;
            for (size_t i = 0; i < bytes; i++) {
                if (flags[i]) { has_lonely = true; break; }
            }
            if (has_lonely && !undo_pushed) {
                if (!doc_undo_push()) return;
                undo_pushed = true;
            }
            for (size_t i = 0; i < bytes; i++) {
                if (flags[i] && pixels[i] != 0) {
                    pixels[i] = 0;
                    image_changes++;
                }
            }

            if (image_changes > 0) {
                changed_images++;
                changed_pixels += image_changes;
                InvalidateThumb(idx);
            }
        }
        img = (IMG *)img->nxt_p;
        idx++;
    }
    if (changed_pixels > 0) {
        mark_dirty();
        g_img_tex_idx = -2; /* Force texture rebuild */
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Repainted %d edge pixel%s in %d/%d marked sprite%s.",
                 changed_pixels,
                 changed_pixels == 1 ? "" : "s",
                 changed_images,
                 scanned,
                 scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    } else if (scanned == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark one or more sprites before stripping edges.");
        g_restore_msg_timer = 4.0f;
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No replaceable edge stroke found in %d marked sprite%s.",
                 scanned,
                 scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
}

/* ---- Dither Replace ---- */
static void DitherReplaceMarkedImages(int specific_color)
{
    mark_dirty();
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            int h = img->h;
            int stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            
            for (int y = 0; y < h; y++) {
                int start_x = y & 1; // 0 for even rows, 1 for odd rows
                for (int x = start_x; x < stride; x += 2) {
                    if (pixels[y * stride + x] != 0) {
                        pixels[y * stride + x] = (unsigned char)specific_color;
                    }
                }
            }
        }
        img = (IMG *)img->nxt_p;
    }
    g_img_tex_idx = -2; /* Force texture rebuild */
}

/* ---- Least-Squares Reduce (Shrink Palette/Auto-Crop) ---- */
static void LeastSquaresReduceMarked()
{
    mark_dirty();
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            int w = img->w;
            int h = img->h;
            unsigned short stride = (w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;

            int min_x = w, min_y = h, max_x = -1, max_y = -1;
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    if (pixels[y * stride + x] != 0) {
                        if (x < min_x) min_x = x;
                        if (x > max_x) max_x = x;
                        if (y < min_y) min_y = y;
                        if (y > max_y) max_y = y;
                    }
                }
            }

            if (max_x == -1) {
                /* Image is completely empty. Shrink to 1x1 transparent. */
                img->w = 1; img->h = 1;
                img->anix = 0; img->aniy = 0;
                pixels[0] = 0;
            } else if (min_x > 0 || min_y > 0 || max_x < w - 1 || max_y < h - 1) {
                int new_w = max_x - min_x + 1;
                int new_h = max_y - min_y + 1;
                unsigned short new_stride = (new_w + 3) & ~3;

                /* In-place compaction (safe because new_stride <= stride) */
                for (int y = 0; y < new_h; y++) {
                    for (int x = 0; x < new_w; x++) {
                        pixels[y * new_stride + x] = pixels[(y + min_y) * stride + (x + min_x)];
                    }
                    /* Zero out the padding bytes to be safe */
                    for (int x = new_w; x < new_stride; x++) {
                        pixels[y * new_stride + x] = 0;
                    }
                }

                img->w = (unsigned short)new_w;
                img->h = (unsigned short)new_h;
                img->anix -= (unsigned short)min_x;
                img->aniy -= (unsigned short)min_y;
            }
        }
        img = (IMG *)img->nxt_p;
    }
    g_img_tex_idx = -2; /* Force texture rebuild */
}

/* ---- ImGui Native File Dialog ---- */
enum class FileDialogMode { OpenImg, AppendImg, OpenLod, SaveImg, ExportTga, LoadLbm, SaveLbm, SaveMarkedLbm, LoadTga, SaveTga, ImportPng, ImportPngMatch, ImportSpriteSheetMatch, ImportGif, ExportPng, ExportPalette, ImportPalette, WriteAniLst, WriteTbl, WriteIrw, LoadAsmAnim, SaveAsmAnim };
static bool g_show_file_dialog = false;
static FileDialogMode g_file_dialog_mode = FileDialogMode::OpenImg;
static char g_file_dialog_dir[1024] = "";
static char g_file_dialog_file[256] = "";
static std::vector<std::string> g_file_dialog_multi_files;
static std::string g_file_dialog_anchor_file;
static char g_lod_override_dir[1024] = "";

static unsigned int g_tbl_base_address = 0x02000000;
static bool g_tbl_export_mk3_format = false;
static bool g_tbl_export_palette = false;
static bool g_tbl_export_pad_4bit = false;
static bool g_tbl_export_align_16bit = false;
static bool g_tbl_export_dual_bank = false;
static int  g_tbl_export_bank      = 0;
static int  g_irw_bpp             = 8;
static unsigned int g_irw_base_address = 0x02000000;
static bool g_irw_align_16bit     = true;
static int  g_gif_blend_mode      = GifBlend_Normal;
static int  g_gif_opacity_percent = 100;
static bool g_gif_import_all      = true;
static bool g_palette_export_act  = false;
static int  g_sheet_bg_threshold  = 245;
static int  g_sheet_min_pixels    = 160;
static int  g_sheet_padding       = 2;
static bool g_sheet_crop          = true;
static char g_sheet_prefix[12]    = "FRAME";

static bool FileDialogSupportsMultiSelect(FileDialogMode mode)
{
    return mode == FileDialogMode::ImportPng ||
           mode == FileDialogMode::ImportPngMatch ||
           mode == FileDialogMode::ImportSpriteSheetMatch ||
           mode == FileDialogMode::ImportGif;
}

/* Group file-dialog modes into categories so each remembers its own last
   directory. Users tend to keep sprites, source PNGs, and TGA dumps in
   different folders — sharing one "last dir" was annoying for everyone. */
static const char *dialog_category_for_mode(FileDialogMode m)
{
    switch (m) {
        case FileDialogMode::OpenImg:
        case FileDialogMode::AppendImg:
        case FileDialogMode::SaveImg:
        case FileDialogMode::OpenLod:
        case FileDialogMode::WriteAniLst:
        case FileDialogMode::WriteTbl:
        case FileDialogMode::WriteIrw:        return "img";
        case FileDialogMode::ImportPng:
        case FileDialogMode::ImportPngMatch:
        case FileDialogMode::ImportSpriteSheetMatch:
        case FileDialogMode::ExportPng:       return "png";
        case FileDialogMode::ImportGif:       return "gif";
        case FileDialogMode::ExportPalette:
        case FileDialogMode::ImportPalette:   return "palette";
        case FileDialogMode::LoadTga:
        case FileDialogMode::SaveTga:
        case FileDialogMode::ExportTga:       return "tga";
        case FileDialogMode::LoadLbm:
        case FileDialogMode::SaveLbm:
        case FileDialogMode::SaveMarkedLbm:   return "lbm";
        case FileDialogMode::LoadAsmAnim:
        case FileDialogMode::SaveAsmAnim:     return "asm";
    }
    return "img";
}

static const char *get_dialog_config_path(const char *category)
{
    /* Built lazily into a per-category static buffer so the returned pointer
       stays valid until the next call with a different category. */
    static char path[MAX_PATH] = "";
    static char last_cat[16]   = "";
    if (!category || !category[0]) category = "img";
    if (path[0] && strcmp(last_cat, category) == 0) return path;
    strncpy(last_cat, category, sizeof(last_cat) - 1);
    last_cat[sizeof(last_cat) - 1] = '\0';
#ifdef _WIN32
    char appdata[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
        _snprintf(path, sizeof(path), "%s\\imgtool\\last_dir_%s.txt", appdata, category);
    } else {
        _snprintf(path, sizeof(path), "last_dir_%s.txt", category);
    }
#else
    _snprintf(path, sizeof(path), "%s/.imgtool_last_dir_%s",
        getenv("HOME") ? getenv("HOME") : ".", category);
#endif
    return path;
}

static void save_last_dir(const char *dir, FileDialogMode mode)
{
    if (!dir || !*dir) return;
#ifdef _WIN32
    char parent[MAX_PATH];
    _snprintf(parent, sizeof(parent), "%s\\imgtool",
        getenv("APPDATA") ? getenv("APPDATA") : ".");
    CreateDirectoryA(parent, NULL);
#endif
    FILE *f = fopen(get_dialog_config_path(dialog_category_for_mode(mode)), "w");
    if (f) { fprintf(f, "%s", dir); fclose(f); }
}

static void load_last_dir(char *dir, size_t dirsz, FileDialogMode mode)
{
    if (!dir || !dirsz) return;
    dir[0] = '\0';
    FILE *f = fopen(get_dialog_config_path(dialog_category_for_mode(mode)), "r");
    if (f) {
        if (fgets(dir, (int)dirsz, f)) {
            size_t len = strlen(dir);
            if (len > 0 && dir[len - 1] == '\n') dir[len - 1] = '\0';
        }
        fclose(f);
    }
}

/* Category-string overloads used by code paths that aren't routed through
   the FileDialogMode enum (e.g. the MK2 Browse button which calls the
   native Win32 picker directly). Reuses the same on-disk format and
   per-category file layout so users see a single coherent system. */
static void save_last_dir_cat(const char *dir, const char *category)
{
    if (!dir || !*dir) return;
#ifdef _WIN32
    char parent[MAX_PATH];
    _snprintf(parent, sizeof(parent), "%s\\imgtool",
        getenv("APPDATA") ? getenv("APPDATA") : ".");
    CreateDirectoryA(parent, NULL);
#endif
    FILE *f = fopen(get_dialog_config_path(category), "w");
    if (f) { fprintf(f, "%s", dir); fclose(f); }
}

static void load_last_dir_cat(char *dir, size_t dirsz, const char *category)
{
    if (!dir || !dirsz) return;
    dir[0] = '\0';
    FILE *f = fopen(get_dialog_config_path(category), "r");
    if (f) {
        if (fgets(dir, (int)dirsz, f)) {
            size_t len = strlen(dir);
            if (len > 0 && dir[len - 1] == '\n') dir[len - 1] = '\0';
        }
        fclose(f);
    }
}

struct FileEntry {
    std::string  name;
    bool         is_dir;
    long long    size;     /* bytes; 0 for dirs */
    long long    mtime;    /* unix-epoch-ish seconds; 0 for dirs */
};

static void FileDialogClearMultiSelection()
{
    g_file_dialog_multi_files.clear();
    g_file_dialog_anchor_file.clear();
}

static bool FileDialogHasMultiFile(const std::string &name)
{
    return std::find(g_file_dialog_multi_files.begin(),
                     g_file_dialog_multi_files.end(),
                     name) != g_file_dialog_multi_files.end();
}

static void FileDialogAddMultiFile(const std::string &name)
{
    if (!FileDialogHasMultiFile(name))
        g_file_dialog_multi_files.push_back(name);
}

static void FileDialogSetFocusedFile(const std::string &name)
{
    snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s", name.c_str());
}

static void FileDialogReplaceSelection(const std::string &name)
{
    g_file_dialog_multi_files.clear();
    FileDialogAddMultiFile(name);
    FileDialogSetFocusedFile(name);
    g_file_dialog_anchor_file = name;
}

static void FileDialogToggleSelection(const std::string &name)
{
    auto it = std::find(g_file_dialog_multi_files.begin(),
                        g_file_dialog_multi_files.end(),
                        name);
    if (it != g_file_dialog_multi_files.end())
        g_file_dialog_multi_files.erase(it);
    else
        g_file_dialog_multi_files.push_back(name);

    if (g_file_dialog_multi_files.empty())
        g_file_dialog_file[0] = '\0';
    else
        FileDialogSetFocusedFile(name);
    g_file_dialog_anchor_file = name;
}

static int FileDialogFindEntryIndex(const std::vector<FileEntry> &entries, const std::string &name)
{
    for (int i = 0; i < (int)entries.size(); i++) {
        if (!entries[i].is_dir && entries[i].name == name)
            return i;
    }
    return -1;
}

static void FileDialogSelectRange(const std::vector<FileEntry> &entries, int clicked_idx, bool append)
{
    if (clicked_idx < 0 || clicked_idx >= (int)entries.size() || entries[clicked_idx].is_dir)
        return;

    int anchor_idx = FileDialogFindEntryIndex(entries, g_file_dialog_anchor_file);
    if (anchor_idx < 0) {
        anchor_idx = clicked_idx;
        g_file_dialog_anchor_file = entries[clicked_idx].name;
    }

    if (!append)
        g_file_dialog_multi_files.clear();

    int lo = anchor_idx < clicked_idx ? anchor_idx : clicked_idx;
    int hi = anchor_idx > clicked_idx ? anchor_idx : clicked_idx;
    for (int i = lo; i <= hi; i++) {
        if (!entries[i].is_dir)
            FileDialogAddMultiFile(entries[i].name);
    }
    FileDialogSetFocusedFile(entries[clicked_idx].name);
}

static std::vector<std::string> FileDialogSelectedFiles()
{
    std::vector<std::string> files;
    if (FileDialogSupportsMultiSelect(g_file_dialog_mode) && !g_file_dialog_multi_files.empty())
        files = g_file_dialog_multi_files;
    else if (g_file_dialog_file[0])
        files.push_back(g_file_dialog_file);
    return files;
}

static void FileDialogSyncTypedFilename()
{
    if (!FileDialogSupportsMultiSelect(g_file_dialog_mode))
        return;

    FileDialogClearMultiSelection();
    if (g_file_dialog_file[0]) {
        g_file_dialog_multi_files.push_back(g_file_dialog_file);
        g_file_dialog_anchor_file = g_file_dialog_file;
    }
}

/* File-list sort key. Persists across dialog opens so the user keeps their
   preferred view. */
enum class FileSort { Name, Date, Size };
static FileSort g_file_sort     = FileSort::Name;
static bool     g_file_sort_desc = false; /* false = asc */

/* ---------------- Preview thumbnail for highlighted file ----------------
   Holds the most recent decode so we don't reparse every frame while the
   user hovers the same file. Limited to PNG / TGA today — IMG and LBM
   need their loaders refactored to not touch globals, which is a bigger
   change deferred to a later round. */
struct FilePreview {
    SDL_Texture *tex;
    int          w, h;
    std::string  path;     /* full path that produced the texture */
};
static FilePreview g_file_preview = {NULL, 0, 0, ""};

static void file_preview_clear(void)
{
    if (g_file_preview.tex) {
        SDL_DestroyTexture(g_file_preview.tex);
        g_file_preview.tex = NULL;
    }
    g_file_preview.w = g_file_preview.h = 0;
    g_file_preview.path.clear();
}

/* Build an SDL texture from a row-major RGBA buffer, scaled to fit inside
   max_side while preserving aspect ratio. Nearest-neighbor (matches the
   timeline-thumb path) so pixel art doesn't get blurred. */
static SDL_Texture *make_preview_texture(const unsigned char *rgba, int sw, int sh, int max_side)
{
    if (!rgba || sw <= 0 || sh <= 0 || !g_imgui_renderer) return NULL;
    int tw, th;
    if (sw >= sh) { tw = max_side; th = (int)((long long)max_side * sh / sw); if (th < 1) th = 1; }
    else          { th = max_side; tw = (int)((long long)max_side * sw / sh); if (tw < 1) tw = 1; }

    SDL_Texture *tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ABGR8888,
                                         SDL_TEXTUREACCESS_STREAMING, tw, th);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    void *pixels; int pitch;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) { SDL_DestroyTexture(tex); return NULL; }
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < th; y++) {
        int sy = (int)((long long)y * sh / th);
        if (sy >= sh) sy = sh - 1;
        for (int x = 0; x < tw; x++) {
            int sx_i = (int)((long long)x * sw / tw);
            if (sx_i >= sw) sx_i = sw - 1;
            const unsigned char *p = rgba + (sy * sw + sx_i) * 4;
            dst[y * (pitch / 4) + x] = ((Uint32)p[3] << 24) | ((Uint32)p[2] << 16) | ((Uint32)p[1] << 8) | (Uint32)p[0];
        }
    }
    SDL_UnlockTexture(tex);
    return tex;
}

/* Decode a stb_image-supported file and build a preview texture. */
extern "C" unsigned char *stbi_load(const char *, int *, int *, int *, int);
extern "C" void stbi_image_free(void *);
static SDL_Texture *load_preview_png(const char *path, int max_side)
{
    int w, h, channels;
    unsigned char *data = stbi_load(path, &w, &h, &channels, 4);
    if (!data) return NULL;
    SDL_Texture *tex = make_preview_texture(data, w, h, max_side);
    stbi_image_free(data);
    return tex;
}

/* Self-contained TGA preview. We don't call LoadTga because that one mutates
   global IMG / PAL lists. Supports the formats LoadTga supports (indexed
   8-bit, colormap 15/16/24-bit). */
#pragma pack(push, 1)
struct TgaPreviewHeader {
    unsigned char  id_len, cm_type, i_type;
    unsigned short cm_first, cm_length;
    unsigned char  cm_size;
    unsigned short x_origin, y_origin, width, height;
    unsigned char  bpp, descriptor;
};
#pragma pack(pop)
static SDL_Texture *load_preview_tga(const char *path, int max_side)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    TgaPreviewHeader hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return NULL; }
    if (hdr.id_len > 0) fseek(f, hdr.id_len, SEEK_CUR);
    if (hdr.i_type != 1 || hdr.cm_type != 1 || hdr.bpp != 8 ||
        (hdr.cm_size != 15 && hdr.cm_size != 16 && hdr.cm_size != 24)) {
        fclose(f); return NULL;
    }
    int sw = hdr.width, sh = hdr.height;
    if (sw <= 0 || sh <= 0 || sw > 16384 || sh > 16384) { fclose(f); return NULL; }
    int ncolors = hdr.cm_length ? hdr.cm_length : 256;
    unsigned char palette[256][3] = {};
    for (int i = 0; i < ncolors && i < 256; i++) {
        if (hdr.cm_size == 24) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, f) != 3) { fclose(f); return NULL; }
            palette[i][0] = rgb[2]; palette[i][1] = rgb[1]; palette[i][2] = rgb[0]; /* BGR -> RGB */
        } else {
            unsigned char w2[2];
            if (fread(w2, 1, 2, f) != 2) { fclose(f); return NULL; }
            unsigned short w15 = (unsigned short)(w2[0] | (w2[1] << 8));
            palette[i][0] = (unsigned char)(((w15 >> 10) & 0x1F) << 3);
            palette[i][1] = (unsigned char)(((w15 >>  5) & 0x1F) << 3);
            palette[i][2] = (unsigned char)(( w15        & 0x1F) << 3);
        }
    }
    /* TGA stores bottom-up by default; descriptor bit 5 set means top-down. */
    bool top_down = (hdr.descriptor & 0x20) != 0;
    std::vector<unsigned char> rgba((size_t)sw * sh * 4, 0);
    for (int y = 0; y < sh; y++) {
        int dy = top_down ? y : (sh - 1 - y);
        for (int x = 0; x < sw; x++) {
            int c = fgetc(f);
            if (c == EOF) { fclose(f); return NULL; }
            unsigned char ci = (unsigned char)c;
            unsigned char *p = &rgba[(dy * sw + x) * 4];
            if (ci == 0) { p[3] = 0; } /* index 0 = transparent */
            else { p[0] = palette[ci][0]; p[1] = palette[ci][1]; p[2] = palette[ci][2]; p[3] = 255; }
        }
    }
    fclose(f);
    return make_preview_texture(rgba.data(), sw, sh, max_side);
}

/* Refresh the preview for `path`, no-op if it's already cached. */
static void file_preview_refresh(const std::string &path)
{
    if (path.empty()) { file_preview_clear(); return; }
    if (path == g_file_preview.path && g_file_preview.tex) return; /* cached */

    /* Pick decoder by extension. */
    size_t dot = path.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos) {
        ext = path.substr(dot + 1);
        for (auto &c : ext) c = (char)tolower((unsigned char)c);
    }
    SDL_Texture *tex = NULL;
    if (ext == "png")       tex = load_preview_png(path.c_str(), 192);
    else if (ext == "jpg" || ext == "jpeg") tex = load_preview_png(path.c_str(), 192);
    else if (ext == "gif")  tex = load_preview_png(path.c_str(), 192);
    else if (ext == "tga")  tex = load_preview_tga(path.c_str(), 192);
    /* IMG / LBM previews would require refactoring those loaders to write
       into a sandbox buffer rather than the global IMG / PAL lists. Saved
       for a follow-up. */

    file_preview_clear();
    if (tex) {
        int w, h;
        SDL_QueryTexture(tex, NULL, NULL, &w, &h);
        g_file_preview.tex  = tex;
        g_file_preview.w    = w;
        g_file_preview.h    = h;
        g_file_preview.path = path;
    }
}

static void GetDirectoryFiles(const std::string& dir, std::vector<FileEntry>& entries, const char* ext_filter)
{
    entries.clear();
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    std::string search = dir;
    if (!search.empty() && search.back() != '\\' && search.back() != '/') search += "\\";
    if (ext_filter && ext_filter[0])
        search += std::string("*.") + ext_filter;
    else
        search += "*";
    HANDLE hFind = FindFirstFileA(search.c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0) continue;
            bool is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            long long sz = is_dir ? 0LL
                : ((long long)fd.nFileSizeHigh << 32) | (long long)fd.nFileSizeLow;
            /* FILETIME -> seconds since some epoch is fine for sort-only use. */
            long long mt = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32) | (long long)fd.ftLastWriteTime.dwLowDateTime;
            entries.push_back({fd.cFileName, is_dir, sz, mt});
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    if (ext_filter && ext_filter[0]) {
        std::string dir_search = dir;
        if (!dir_search.empty() && dir_search.back() != '\\' && dir_search.back() != '/') dir_search += "\\";
        dir_search += "*";
        HANDLE hDir = FindFirstFileA(dir_search.c_str(), &fd);
        if (hDir != INVALID_HANDLE_VALUE) {
            do {
                if (strcmp(fd.cFileName, ".") == 0) continue;
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
                bool dup = false;
                for (const auto& e : entries) { if (e.name == fd.cFileName) { dup = true; break; } }
                if (!dup) entries.push_back({fd.cFileName, true, 0LL, 0LL});
            } while (FindNextFileA(hDir, &fd));
            FindClose(hDir);
        }
    }
#else
    DIR* d = opendir(dir.empty() ? "." : dir.c_str());
    if (d) {
        struct dirent* dir_ent;
        while ((dir_ent = readdir(d)) != NULL) {
            if (strcmp(dir_ent->d_name, ".") == 0) continue;
            std::string full_path = dir;
            if (!full_path.empty() && full_path.back() != '/') full_path += "/";
            full_path += dir_ent->d_name;
            struct stat st = {};
            bool have_stat = (stat(full_path.c_str(), &st) == 0);
            bool is_dir = have_stat && S_ISDIR(st.st_mode);
            long long sz = is_dir ? 0LL : (have_stat ? (long long)st.st_size : 0LL);
            long long mt = have_stat ? (long long)st.st_mtime : 0LL;
            if (is_dir) {
                entries.push_back({dir_ent->d_name, true, 0LL, mt});
            } else if (ext_filter && ext_filter[0]) {
                const char* dot = strrchr(dir_ent->d_name, '.');
                if (dot && strcasecmp(dot + 1, ext_filter) == 0)
                    entries.push_back({dir_ent->d_name, false, sz, mt});
            } else {
                entries.push_back({dir_ent->d_name, false, sz, mt});
            }
        }
        closedir(d);
    }
#endif
}

static std::string GetParentDirectory(const std::string& dir)
{
    size_t pos = dir.find_last_of("\\/");
    if (pos != std::string::npos)
    {
        if (pos == 0) return dir.substr(0, 1);
#ifdef _WIN32
        if (pos == 2 && dir[1] == ':') return dir.substr(0, 3);
#endif
        return dir.substr(0, pos);
    }
    return dir;
}

static std::string PathCombine(const std::string& dir, const std::string& file)
{
    if (dir.empty()) return file;
    char last = dir.back();
    if (last == '\\' || last == '/') return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + "/" + file;
#endif
}

/* ===== Recent files (most-recently-opened IMG files) =====
   Persisted as one absolute path per line in <exe_dir>/imgtool_recent.txt.
   Newest entry is at index 0. Capped at RECENT_MAX. */
extern "C" char exe_dir[];   /* defined in shim_file.c */
static const size_t RECENT_MAX = 8;
static std::vector<std::string> g_recent_files;

static std::string RecentFilesPath()
{
    std::string base = exe_dir[0] ? exe_dir : ".";
#ifdef _WIN32
    return base + "\\imgtool_recent.txt";
#else
    return base + "/imgtool_recent.txt";
#endif
}

static void RecentLoad()
{
    g_recent_files.clear();
    FILE *f = fopen(RecentFilesPath().c_str(), "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f) && g_recent_files.size() < RECENT_MAX) {
        size_t n = strlen(line);
        while (n && (line[n-1] == '\n' || line[n-1] == '\r')) line[--n] = '\0';
        if (n) g_recent_files.push_back(line);
    }
    fclose(f);
}

static void RecentSave()
{
    FILE *f = fopen(RecentFilesPath().c_str(), "w");
    if (!f) return;
    for (const std::string &p : g_recent_files) fprintf(f, "%s\n", p.c_str());
    fclose(f);
}

static void RecentAdd(const std::string &full_path)
{
    auto it = std::find(g_recent_files.begin(), g_recent_files.end(), full_path);
    if (it != g_recent_files.end()) g_recent_files.erase(it);
    g_recent_files.insert(g_recent_files.begin(), full_path);
    if (g_recent_files.size() > RECENT_MAX) g_recent_files.resize(RECENT_MAX);
    RecentSave();
}

static std::string DocFullPath(const Document *doc)
{
    if (!doc || doc->fname_s[0] == '\0') return std::string();
    std::string dir(doc->fpath_s);
    std::string file(doc->fname_s);
    if (dir.empty()) return file;
    char last = dir[dir.size() - 1];
    if (last == '\\' || last == '/') return dir + file;
#ifdef _WIN32
    return dir + "\\" + file;
#else
    return dir + "/" + file;
#endif
}

static bool PathEqualsForPlatform(const std::string &a, const std::string &b)
{
#ifdef _WIN32
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        char ca = (char)tolower((unsigned char)a[i]);
        char cb = (char)tolower((unsigned char)b[i]);
        if (ca == '/') ca = '\\';
        if (cb == '/') cb = '\\';
        if (ca != cb) return false;
    }
    return true;
#else
    return a == b;
#endif
}

static int FindOpenDocumentByPath(const std::string &full_path)
{
    for (int i = 0; i < document_tab_count(); i++) {
        Document *doc = document_get(i);
        std::string doc_path = DocFullPath(doc);
        if (!doc_path.empty() && PathEqualsForPlatform(doc_path, full_path))
            return i;
    }
    return -1;
}

static bool DocumentCanReuseForOpen(Document *doc)
{
    return doc && !doc->dirty && doc->imgcnt == 0 && doc->palcnt == 0 &&
           doc->img2cnt == 0 && doc->fname_s[0] == '\0';
}

static void ActivateDocumentTab(int idx)
{
    if (idx < 0 || idx >= document_tab_count()) return;
    if (idx != document_active_index()) {
        document_set_active(idx);
        ResetPerDocumentUiState(false);
    }
    g_doc_tab_select_request = idx;
    Mk2AutoSelectFromImg();
}

static void PrepareDocumentForOpenedFile(void)
{
    if (!DocumentCanReuseForOpen(g_doc))
        document_new_tab();
    g_doc_tab_select_request = document_active_index();
    ResetPerDocumentUiState(false);
    ClearAll();
}

static void SetActiveDocumentPath(const std::string &full_path)
{
    size_t sep = full_path.find_last_of("\\/");
    std::string dir  = (sep == std::string::npos) ? std::string(".") : full_path.substr(0, sep);
    std::string file = (sep == std::string::npos) ? full_path        : full_path.substr(sep + 1);

    size_t n_dir = dir.size();
    if (n_dir > sizeof(g_doc->fpath_s) - 1) n_dir = sizeof(g_doc->fpath_s) - 1;
    memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
    memcpy(g_doc->fpath_s, dir.data(), n_dir);

    size_t n_file = file.size();
    if (n_file > 12) n_file = 12;
    memset(g_doc->fname_s, 0, 13);
    memset(g_doc->fnametmp_s, 0, 13);
    memcpy(g_doc->fname_s, file.data(), n_file);
    memcpy(g_doc->fnametmp_s, file.data(), n_file);
    for (size_t i = 0; i < n_file; i++) {
        g_doc->fname_s[i] = (char)toupper((unsigned char)g_doc->fname_s[i]);
        g_doc->fnametmp_s[i] = (char)toupper((unsigned char)g_doc->fnametmp_s[i]);
    }

    _chdir(dir.c_str());
}

/* Load an IMG by absolute path. Mirrors the Open-button branch of the file
   dialog, but now opens into its own document tab. A clean empty startup tab
   is reused; otherwise a new tab is created and activated. */
static void OpenImgFile(const std::string &full_path)
{
    int existing = FindOpenDocumentByPath(full_path);
    if (existing >= 0) {
        ActivateDocumentTab(existing);
        RecentAdd(full_path);
        return;
    }

    PrepareDocumentForOpenedFile();
    SetActiveDocumentPath(full_path);
    LoadImgFile();
    g_dirty = false; /* fresh load = clean baseline */
    g_img_tex_idx = -2;
    RecentAdd(full_path);
    Mk2AutoSelectFromImg();
}

/* ===== Session restore (open IMG tabs from the last clean shutdown) =====
   Stored beside the MRU list so portable builds keep their state local to
   the executable folder. Only disk-backed IMG documents are persisted. */
static std::string SessionFilesPath()
{
    std::string base = exe_dir[0] ? exe_dir : ".";
#ifdef _WIN32
    return base + "\\imgtool_session.txt";
#else
    return base + "/imgtool_session.txt";
#endif
}

static void SessionSave()
{
    FILE *f = fopen(SessionFilesPath().c_str(), "w");
    if (!f) return;

    std::vector<std::string> paths;
    int active_doc = document_active_index();
    int active_session_idx = -1;

    for (int i = 0; i < document_tab_count(); i++) {
        Document *doc = document_get(i);
        std::string path = DocFullPath(doc);
        if (!doc || path.empty() || doc->imgcnt == 0) continue;
        if (i == active_doc) active_session_idx = (int)paths.size();
        paths.push_back(path);
    }

    if (active_session_idx < 0 && !paths.empty()) active_session_idx = 0;
    fprintf(f, "active=%d\n", active_session_idx);
    for (const std::string &path : paths)
        fprintf(f, "%s\n", path.c_str());
    fclose(f);
}

static bool SessionLoad(std::vector<std::string> *paths, int *active_session_idx)
{
    if (paths) paths->clear();
    if (active_session_idx) *active_session_idx = 0;

    FILE *f = fopen(SessionFilesPath().c_str(), "r");
    if (!f) return false;

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = '\0';
        if (!n) continue;

        if (strncmp(line, "active=", 7) == 0) {
            if (active_session_idx) *active_session_idx = (int)strtol(line + 7, NULL, 10);
            continue;
        }

        if (paths) paths->push_back(line);
    }

    fclose(f);
    return paths && !paths->empty();
}

static bool PathReadable(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static void SessionRestore()
{
    std::vector<std::string> paths;
    int active_session_idx = 0;
    if (!SessionLoad(&paths, &active_session_idx)) return;

    std::vector<std::string> recent_before = g_recent_files;
    std::vector<int> restored_tabs;
    for (const std::string &path : paths) {
        if (!PathReadable(path)) continue;

        OpenImgFile(path);
        int idx = FindOpenDocumentByPath(path);
        Document *doc = document_get(idx);
        if (!doc || doc->imgcnt == 0) continue;

        doc->dirty = false;
        restored_tabs.push_back(idx);
    }

    if (g_recent_files != recent_before) {
        g_recent_files = recent_before;
        RecentSave();
    }

    if (restored_tabs.empty()) return;
    if (active_session_idx < 0 || active_session_idx >= (int)restored_tabs.size())
        active_session_idx = 0;
    ActivateDocumentTab(restored_tabs[(size_t)active_session_idx]);
}

static const char* GetDialogExtension(FileDialogMode mode)
{
    switch (mode) {
        case FileDialogMode::OpenImg:
        case FileDialogMode::AppendImg:
        case FileDialogMode::SaveImg:   return "IMG";
        case FileDialogMode::OpenLod:   return "LOD";
        case FileDialogMode::LoadLbm:
        case FileDialogMode::SaveLbm:
        case FileDialogMode::SaveMarkedLbm: return "LBM";
        case FileDialogMode::LoadTga:
        case FileDialogMode::SaveTga:
        case FileDialogMode::ExportTga: return "TGA";
        case FileDialogMode::ImportPng:
        case FileDialogMode::ImportPngMatch:
        case FileDialogMode::ExportPng: return "PNG";
        case FileDialogMode::ImportSpriteSheetMatch: return "";
        case FileDialogMode::ImportGif: return "GIF";
        case FileDialogMode::ExportPalette: return g_palette_export_act ? "ACT" : "PAL";
        case FileDialogMode::ImportPalette: return "PAL";
        case FileDialogMode::WriteAniLst: return "ASM";
        case FileDialogMode::WriteTbl:  return "TBL";
        case FileDialogMode::WriteIrw:  return "IRW";
        case FileDialogMode::LoadAsmAnim:
        case FileDialogMode::SaveAsmAnim: return "ASM";
    }
    return "";
}

static void OpenFileDialog(FileDialogMode mode) {
    /* For IMG-category modes, g_doc->fpath_s (set when an IMG is currently loaded)
       seeds the dialog so the user starts in the same dir as their open
       file. For other categories, g_doc->fpath_s is irrelevant — those have their
       own per-category remembered directory. Always re-load from disk on
       mode change so switching from Save IMG to Import PNG lands in the
       right folder. */
    const char *cat = dialog_category_for_mode(mode);
    bool is_img_cat = (strcmp(cat, "img") == 0);
    g_file_dialog_dir[0] = '\0';
    if (is_img_cat && g_doc->fpath_s[0] != '\0') {
        size_t n = 0;
        while (n < sizeof(g_doc->fpath_s) - 1 && g_doc->fpath_s[n] != '\0') n++;
        memcpy(g_file_dialog_dir, g_doc->fpath_s, n);
        g_file_dialog_dir[n] = '\0';
    } else {
        load_last_dir(g_file_dialog_dir, sizeof(g_file_dialog_dir), mode);
    }
    if (g_file_dialog_dir[0] == '\0') {
#ifdef _WIN32
        GetCurrentDirectoryA(sizeof(g_file_dialog_dir), g_file_dialog_dir);
#else
        if (getcwd(g_file_dialog_dir, sizeof(g_file_dialog_dir)) == NULL)
            g_file_dialog_dir[0] = '\0';
#endif
    }
    g_file_dialog_mode = mode;
    bool is_export = (mode == FileDialogMode::ExportTga || mode == FileDialogMode::SaveTga ||
                      mode == FileDialogMode::ExportPng || mode == FileDialogMode::ExportPalette ||
                      mode == FileDialogMode::SaveLbm);
    if (mode == FileDialogMode::ExportPalette && g_doc->plselected >= 0) {
        PAL *pal = get_pal(g_doc->plselected);
        if (pal) {
            snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s.%s",
                     pal->n_s, GetDialogExtension(mode));
        }
    } else if (is_export && g_doc->ilselected >= 0) {
        IMG *img = get_img(g_doc->ilselected);
        if (img) {
            size_t n = 0;
            while (n < 16 && img->file_name_raw[n] != '\0') {
                g_file_dialog_file[n] = img->file_name_raw[n];
                n++;
            }
            g_file_dialog_file[n] = '\0';
            const char *ext = GetDialogExtension(mode);
            if (ext && ext[0] != '\0') {
                strcat(g_file_dialog_file, ".");
                strcat(g_file_dialog_file, ext);
            }
        }
    } else if (mode == FileDialogMode::SaveAsmAnim) {
        /* World View ASM is generated from the marked sprites across all open
           tabs, not from the current IMG document. Seeding g_doc->fname_s here
           produced a misleading default (the open IMG's name). Instead seed
           from the first generated animation label (e.g. a_imgtool_slot1_kick),
           falling back to a blank name if nothing has been generated yet. */
        g_file_dialog_file[0] = '\0';
        const std::string &asm_src = g_world_marked_state.generated_asm;
        for (size_t ls = 0; ls < asm_src.size(); ) {
            size_t le = asm_src.find('\n', ls);
            size_t line_end = (le == std::string::npos) ? asm_src.size() : le;
            size_t a = asm_src.find_first_not_of(" \t\r", ls);
            if (a != std::string::npos && a < line_end && asm_src[a] != ';') {
                size_t b = asm_src.find_first_of(" \t\r", a);
                if (b == std::string::npos || b > line_end) b = line_end;
                std::string label = asm_src.substr(a, b - a);
                snprintf(g_file_dialog_file, sizeof(g_file_dialog_file),
                         "%s.ASM", label.c_str());
                break;
            }
            if (le == std::string::npos) break;
            ls = le + 1;
        }
    } else if (g_doc->fname_s[0] != '\0') {
        size_t n = 0;
        while (n < 12 && g_doc->fname_s[n] != '\0') n++;
        memcpy(g_file_dialog_file, g_doc->fname_s, n);
        g_file_dialog_file[n] = '\0';
    } else {
        g_file_dialog_file[0] = '\0';
    }
    FileDialogClearMultiSelection();
    if (FileDialogSupportsMultiSelect(mode) && g_file_dialog_file[0]) {
        g_file_dialog_multi_files.push_back(g_file_dialog_file);
        g_file_dialog_anchor_file = g_file_dialog_file;
    }
    g_show_file_dialog = true;
}

/* Guarded entry points for "load a different file" operations. If there are
   unsaved changes, tabs let us avoid destructive replacement: Open creates a
   fresh document when needed, while Close/Quit still prompt per dirty tab. */
static void RequestOpenDialog(void)
{
    OpenFileDialog(FileDialogMode::OpenImg);
}
static void RequestOpenPath(const std::string &path)
{
    OpenImgFile(path);
}
static void RequestOpenLodDialog(void)
{
    OpenFileDialog(FileDialogMode::OpenLod);
}

/* Drag-and-drop entry point. Extension dispatch:
     .img → RequestOpenPath (unsaved-changes guard + full reset)
     .tga → LoadTga import into the active document
     .lbm → LoadLbm import into the active document
     .png → ImportPng
     .gif → ImportGif
   Unknown extensions toast and return. */
extern "C" void imgui_overlay_open_path(const char *path)
{
    if (!path || !*path) return;
    std::string p = path;
    size_t dot = p.find_last_of('.');
    std::string ext;
    if (dot != std::string::npos) {
        ext = p.substr(dot + 1);
        for (char &c : ext) c = (char)tolower((unsigned char)c);
    }

    /* Import paths (TGA/LBM/PNG) need a document to import *into*. Drop on
       an empty workspace → bootstrap a fresh IMG, matching File → New. */
    auto ensure_new_doc_if_empty = []() {
        if (g_doc->imgcnt == 0) {
            ClearAll();
            g_doc->fileversion = 0x0634;
            g_doc->fname_s[0]  = 0;
            g_undo_count = 0;
            g_undo_idx   = 0;
        }
    };

    if (ext == "img") {
        RequestOpenPath(p);
    } else if (ext == "tga") {
        ensure_new_doc_if_empty();
        LoadTga(p.c_str());
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "lbm") {
        ensure_new_doc_if_empty();
        LoadLbm(p.c_str());
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "png") {
        ensure_new_doc_if_empty();
        ImportPng(p.c_str());
        mark_dirty();
        g_img_tex_idx = -2;
    } else if (ext == "gif") {
        ensure_new_doc_if_empty();
        ImportGif(p.c_str(), g_gif_blend_mode, g_gif_opacity_percent, g_gif_import_all);
        mark_dirty();
        g_img_tex_idx = -2;
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Unsupported file type: .%s", ext.empty() ? "(none)" : ext.c_str());
        g_restore_msg_timer = 4.0f;
    }
}

static void DrawFileDialog() {
    const char* title = "Open File";
    if (g_file_dialog_mode == FileDialogMode::SaveImg) title = "Save IMG File";
    else if (g_file_dialog_mode == FileDialogMode::ExportTga) title = "Export TGA";
    else if (g_file_dialog_mode == FileDialogMode::OpenImg) title = "Open IMG File";
    else if (g_file_dialog_mode == FileDialogMode::AppendImg) title = "Append IMG File";
    else if (g_file_dialog_mode == FileDialogMode::OpenLod) title = "Open LOD File";
    else if (g_file_dialog_mode == FileDialogMode::LoadLbm) title = "Load LBM File";
    else if (g_file_dialog_mode == FileDialogMode::SaveLbm) title = "Save LBM File";
    else if (g_file_dialog_mode == FileDialogMode::SaveMarkedLbm) title = "Save Marked LBM";
    else if (g_file_dialog_mode == FileDialogMode::LoadTga) title = "Load TGA File";
    else if (g_file_dialog_mode == FileDialogMode::SaveTga) title = "Save TGA File";
    else if (g_file_dialog_mode == FileDialogMode::ImportPng) title = "Import PNG File";
    else if (g_file_dialog_mode == FileDialogMode::ImportPngMatch) title = "Import PNG (Match Palette)";
    else if (g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch) title = "Import Sprite Sheet (Match Palette)";
    else if (g_file_dialog_mode == FileDialogMode::ImportGif) title = "Import GIF File";
    else if (g_file_dialog_mode == FileDialogMode::ExportPng) title = "Export PNG File";
    else if (g_file_dialog_mode == FileDialogMode::ExportPalette) title = "Export Palette";
    else if (g_file_dialog_mode == FileDialogMode::ImportPalette) title = "Import Palette";
    else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) title = "Write ANILST";
    else if (g_file_dialog_mode == FileDialogMode::WriteTbl) title = "Write TBL";
    else if (g_file_dialog_mode == FileDialogMode::WriteIrw) title = "Write IRW";
    else if (g_file_dialog_mode == FileDialogMode::LoadAsmAnim) title = "Load Character ASM";
    else if (g_file_dialog_mode == FileDialogMode::SaveAsmAnim) title = "Save World View ASM";

    if (g_show_file_dialog) ImGui::OpenPopup(title);
    
    ImGui::SetNextWindowSize(ImVec2(800, 520), ImGuiCond_Once);
    if (ImGui::BeginPopupModal(title, &g_show_file_dialog, ImGuiWindowFlags_NoSavedSettings)) {
        
        if (ImGui::InputText("Directory", g_file_dialog_dir, sizeof(g_file_dialog_dir))) {
            g_file_dialog_file[0] = '\0';
            FileDialogClearMultiSelection();
            file_preview_clear();
        }

        /* Sort controls. Persistent across dialog opens. */
        ImGui::SameLine();
        const char *sort_labels[] = { "Name", "Date", "Size" };
        int sort_idx = (int)g_file_sort;
        ImGui::SetNextItemWidth(80);
        if (ImGui::Combo("##filesort", &sort_idx, sort_labels, 3))
            g_file_sort = (FileSort)sort_idx;
        ImGui::SameLine();
        if (ImGui::SmallButton(g_file_sort_desc ? "v" : "^")) g_file_sort_desc = !g_file_sort_desc;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle ascending / descending");

        ImGui::Separator();

        /* Two-pane layout: file list on the left, preview thumbnail on the
           right. Preview lives in the same vertical band as the list. */
        const float PREVIEW_PANE_W = 208.0f; /* 192 image + 16 padding */
        float content_h = -ImGui::GetFrameHeightWithSpacing() * 4;

        ImGui::BeginChild("##file_list", ImVec2(-PREVIEW_PANE_W, content_h), true);

        std::string current_dir = g_file_dialog_dir;
        std::string parent_dir  = GetParentDirectory(current_dir);

        if (current_dir != parent_dir) {
            if (ImGui::Selectable("[..] (Up one level)", false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", parent_dir.c_str());
                    g_file_dialog_file[0] = '\0';
                    FileDialogClearMultiSelection();
                    file_preview_clear();
                }
            }
        }

        std::vector<FileEntry> entries;
        GetDirectoryFiles(current_dir, entries, GetDialogExtension(g_file_dialog_mode));

        std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
            /* Directories always come first regardless of sort key, so the
               user can drill down without scrolling past the file list. */
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            bool less;
            switch (g_file_sort) {
                case FileSort::Date: less = a.mtime < b.mtime; break;
                case FileSort::Size: less = a.size  < b.size;  break;
                case FileSort::Name:
                default:             less = a.name  < b.name;  break;
            }
            return g_file_sort_desc ? !less : less;
        });

        /* Double-click on a file = select + commit, matching native OS
           file dialogs. The actual commit re-uses the OK/Open/Save button
           handler below by OR-ing this flag with its click. */
        bool dbl_click_commit = false;
        const bool multi_select = FileDialogSupportsMultiSelect(g_file_dialog_mode);
        std::string hover_preview_path;
        for (int entry_idx = 0; entry_idx < (int)entries.size(); entry_idx++) {
            const auto& entry = entries[entry_idx];
            std::string label = (entry.is_dir ? "[Dir] " : "      ") + entry.name;
            bool selected = multi_select ? FileDialogHasMultiFile(entry.name)
                                         : (entry.name == g_file_dialog_file);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (entry.is_dir) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::string new_dir = PathCombine(current_dir, entry.name);
                        snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", new_dir.c_str());
                        g_file_dialog_file[0] = '\0';
                        FileDialogClearMultiSelection();
                        file_preview_clear();
                    }
                } else {
                    if (multi_select) {
                        const ImGuiIO &io = ImGui::GetIO();
                        if (io.KeyShift)
                            FileDialogSelectRange(entries, entry_idx, io.KeyCtrl);
                        else if (io.KeyCtrl)
                            FileDialogToggleSelection(entry.name);
                        else
                            FileDialogReplaceSelection(entry.name);
                    } else {
                        snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s", entry.name.c_str());
                    }
                    if (ImGui::IsMouseDoubleClicked(0)) dbl_click_commit = true;
                }
            }
            if (!entry.is_dir && ImGui::IsItemHovered())
                hover_preview_path = PathCombine(current_dir, entry.name);
        }
        ImGui::EndChild();

        /* Preview pane on the right. Decodes the currently-selected file
           lazily; cache is keyed by full path so cursoring up/down doesn't
           re-decode the same file. */
        ImGui::SameLine();
        ImGui::BeginChild("##file_preview", ImVec2(PREVIEW_PANE_W - 8, content_h), true);
        {
            std::string preview_path = hover_preview_path;
            if (preview_path.empty() && g_file_dialog_file[0])
                preview_path = PathCombine(g_file_dialog_dir, g_file_dialog_file);
            if (!preview_path.empty()) {
                file_preview_refresh(preview_path);
                if (g_file_preview.tex) {
                    ImGui::TextUnformatted("Preview");
                    ImGui::Image((ImTextureID)(intptr_t)g_file_preview.tex,
                                 ImVec2((float)g_file_preview.w, (float)g_file_preview.h));
                } else {
                    ImGui::TextDisabled("No preview\n(IMG / LBM previews\ncoming soon)");
                }
            } else {
                ImGui::TextDisabled("Select a file");
            }
        }
        ImGui::EndChild();
        
        if (g_file_dialog_mode == FileDialogMode::WriteTbl) {
            ImGui::InputScalar("ROM Base Address (Hex)", ImGuiDataType_U32, &g_tbl_base_address, NULL, NULL, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::Checkbox("MK3 Format (7-value header)", &g_tbl_export_mk3_format);
            ImGui::SetItemTooltip("Includes the 3 extra animation points: ANIX2, ANIY2, and ANIZ2.");
            ImGui::Checkbox("Include Assigned Palette Name", &g_tbl_export_palette);
            ImGui::Checkbox("Pad to 4-bit boundary (/P)", &g_tbl_export_pad_4bit);
            ImGui::Checkbox("Align to 16-bit boundary (/L)", &g_tbl_export_align_16bit);
            ImGui::Checkbox("Dual-Banked Memory (/E)", &g_tbl_export_dual_bank);
            if (g_tbl_export_dual_bank) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                ImGui::InputInt("Bank (0/1)", &g_tbl_export_bank);
                if (g_tbl_export_bank < 0) g_tbl_export_bank = 0;
                if (g_tbl_export_bank > 1) g_tbl_export_bank = 1;
            }
        }
        if (g_file_dialog_mode == FileDialogMode::WriteIrw) {
            ImGui::InputScalar("ROM Base Address (Hex)", ImGuiDataType_U32, &g_irw_base_address, NULL, NULL, "%08X", ImGuiInputTextFlags_CharsHexadecimal);
            ImGui::Text("Bits Per Pixel:");
            if (ImGui::RadioButton("Auto (Image Data)", g_irw_bpp == 0)) g_irw_bpp = 0;
            ImGui::SameLine();
            if (ImGui::RadioButton("Auto (Palette Size) /B", g_irw_bpp == -1)) g_irw_bpp = -1;
            ImGui::SameLine();
            int fixed_bpp = (g_irw_bpp > 0) ? g_irw_bpp : 8;
            if (ImGui::RadioButton("Fixed", g_irw_bpp > 0)) g_irw_bpp = fixed_bpp;
            if (g_irw_bpp > 0) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::SliderInt("##bpp", &fixed_bpp, 1, 8)) g_irw_bpp = fixed_bpp;
            }
            ImGui::Checkbox("Align to 16-bit boundary (/L)", &g_irw_align_16bit);
        }

        if (g_file_dialog_mode == FileDialogMode::ImportGif) {
            if (ImGui::BeginCombo("Blend Mode", GifBlendModeName(g_gif_blend_mode))) {
                for (int i = 0; i < GifBlend_Count; i++) {
                    bool selected = (g_gif_blend_mode == i);
                    if (ImGui::Selectable(GifBlendModeName(i), selected)) g_gif_blend_mode = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::SliderInt("Opacity", &g_gif_opacity_percent, 0, 100, "%d%%");
            ImGui::Checkbox("Import All Frames", &g_gif_import_all);
        }

        if (g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch) {
            ImGui::InputText("Name Prefix", g_sheet_prefix, sizeof(g_sheet_prefix));
            ImGui::SliderInt("Background", &g_sheet_bg_threshold, 180, 255);
            ImGui::InputInt("Min Pixels", &g_sheet_min_pixels);
            if (g_sheet_min_pixels < 1) g_sheet_min_pixels = 1;
            ImGui::InputInt("Padding", &g_sheet_padding);
            if (g_sheet_padding < 0) g_sheet_padding = 0;
            if (g_sheet_padding > 64) g_sheet_padding = 64;
            ImGui::Checkbox("Crop to Sprite Bounds", &g_sheet_crop);
        }

        if (g_file_dialog_mode == FileDialogMode::ExportPalette) {
            ImGui::Checkbox("Adobe ACT (RGB)", &g_palette_export_act);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off writes raw Midway 15-bit palette words.");
        }

        if (g_file_dialog_mode == FileDialogMode::OpenLod) {
            ImGui::InputText("Force Override Directory (/O)", g_lod_override_dir, sizeof(g_lod_override_dir));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If set, forces all IMGs to load from this directory, ignoring paths in the .lod file.");
        }

        if (ImGui::InputText("File Name", g_file_dialog_file, sizeof(g_file_dialog_file)))
            FileDialogSyncTypedFilename();
        ImGui::SameLine();
        
        const char* btn_text = (g_file_dialog_mode == FileDialogMode::ImportPng ||
                                g_file_dialog_mode == FileDialogMode::ImportPngMatch ||
                                g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch ||
                                g_file_dialog_mode == FileDialogMode::ImportGif ||
                                g_file_dialog_mode == FileDialogMode::ExportPng) ? "OK" :
                               (g_file_dialog_mode == FileDialogMode::OpenImg ||
                                g_file_dialog_mode == FileDialogMode::AppendImg ||
                                g_file_dialog_mode == FileDialogMode::OpenLod ||
                                g_file_dialog_mode == FileDialogMode::LoadLbm ||
                                g_file_dialog_mode == FileDialogMode::LoadTga ||
                                g_file_dialog_mode == FileDialogMode::LoadAsmAnim ||
                                g_file_dialog_mode == FileDialogMode::ImportPalette) ? "Open" : "Save";
        if (ImGui::Button(btn_text, ImVec2(100, 0)) || dbl_click_commit) {
            std::vector<std::string> selected_files = FileDialogSelectedFiles();
            std::string full_path = PathCombine(g_file_dialog_dir, g_file_dialog_file);

            if (g_file_dialog_mode == FileDialogMode::ExportTga) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".TGA";
                BuildTgaFromMarked(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::ExportPng) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".PNG";
                ExportPng(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::ExportPalette) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += g_palette_export_act ? ".ACT" : ".PAL";
                ExportPalette(full_path.c_str(), g_palette_export_act);
            } else if (g_file_dialog_mode == FileDialogMode::ImportPalette) {
                doc_undo_push();
                ImportPalette(full_path.c_str());
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportPng) {
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportPng(path.c_str());
                }
                if (selected_files.size() > 1) {
                    unsigned int added = g_doc->imgcnt - before_count;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u image(s) from %d PNG file(s)." : "No PNG images imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportPngMatch) {
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportPngMatch(path.c_str());
                }
                if (selected_files.size() > 1) {
                    unsigned int added = g_doc->imgcnt - before_count;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u image(s) from %d PNG file(s)." : "No PNG images imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportSpriteSheetMatch) {
                unsigned int before_count = g_doc->imgcnt;
                SpriteSheetImportOptions opts = {};
                opts.detect_mode = SpriteSheetDetect_Auto;
                opts.background_threshold = g_sheet_bg_threshold;
                opts.min_pixels = g_sheet_min_pixels;
                opts.padding = g_sheet_padding;
                opts.crop = g_sheet_crop;
                snprintf(opts.name_prefix, sizeof(opts.name_prefix), "%s", g_sheet_prefix);
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportSpriteSheetMatch(path.c_str(), &opts);
                }
                unsigned int added = g_doc->imgcnt - before_count;
                if (selected_files.size() > 1) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u frame(s) from %d sprite sheet(s)." : "No sprite sheet frames imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                if (added > 0) mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::ImportGif) {
                unsigned int before_count = g_doc->imgcnt;
                for (const std::string &file : selected_files) {
                    std::string path = PathCombine(g_file_dialog_dir, file);
                    ImportGif(path.c_str(), g_gif_blend_mode, g_gif_opacity_percent, g_gif_import_all);
                }
                if (selected_files.size() > 1) {
                    unsigned int added = g_doc->imgcnt - before_count;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             added ? "Imported %u image(s) from %d GIF file(s)." : "No GIF images imported.",
                             added, (int)selected_files.size());
                    g_restore_msg_timer = 4.0f;
                }
                mark_dirty();
            } else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".ASM";
                WriteAnilstFromMarked(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::SaveAsmAnim) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".ASM";
                FILE *af = fopen(full_path.c_str(), "wb");
                if (af) {
                    fwrite(g_world_marked_state.generated_asm.data(), 1,
                           g_world_marked_state.generated_asm.size(), af);
                    fclose(af);
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Saved World View ASM (%d bytes).",
                             (int)g_world_marked_state.generated_asm.size());
                } else {
                    snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not write ASM file.");
                }
                g_restore_msg_timer = 4.0f;
            } else if (g_file_dialog_mode == FileDialogMode::WriteTbl) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".TBL";
                WriteTblFromMarked(full_path.c_str(), g_tbl_base_address, g_tbl_export_mk3_format, g_tbl_export_palette, g_tbl_export_pad_4bit, g_tbl_export_align_16bit, g_tbl_export_dual_bank, g_tbl_export_bank);
            } else if (g_file_dialog_mode == FileDialogMode::WriteIrw) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".IRW";
                WriteIrwFromMarked(full_path.c_str(), g_irw_base_address, g_irw_bpp, g_irw_align_16bit);
            } else if (g_file_dialog_mode == FileDialogMode::SaveMarkedLbm) {
                _chdir(g_file_dialog_dir);
                IMG *p = (IMG *)g_doc->img_p;
                int original_selection = g_doc->ilselected;
                int i = 0;
                while (p) {
                    if ((p->flags & 1) && p->w > 0 && p->h > 0) {
                        g_doc->ilselected = i;
                        memset(g_doc->fnametmp_s, 0, 13);
                        snprintf(g_doc->fnametmp_s, 13, "%.8s.LBM", p->n_s);
                        SaveLbm(g_doc->fnametmp_s);
                    }
                    p = (IMG *)p->nxt_p;
                    i++;
                }
                g_doc->ilselected = original_selection;
            } else if (g_file_dialog_mode == FileDialogMode::OpenImg) {
                OpenImgFile(full_path);
                /* If this open was to locate an ASM viewer's IMG, re-resolve. */
                if (g_openimg_for_asm) {
                    g_openimg_for_asm = false;
                    if (!g_asm_anims.empty())
                        AsmAnimSelect(g_asm_anim_sel >= 0 ? g_asm_anim_sel : 0);
                }
                if (g_openimg_for_opp) {
                    g_openimg_for_opp = false;
                    g_asm_opp_doc = g_doc;
                    g_asm_opp_doc_idx = document_active_index();
                    if (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
                        AsmResolveAnimAgainstDoc(g_asm_opp_anims[g_asm_opp_sel], g_asm_opp_doc);
                }
            } else if (g_file_dialog_mode == FileDialogMode::LoadAsmAnim) {
                if (g_asm_dialog_opponent) {
                    LoadAsmOpponent(full_path.c_str());
                } else {
                    LoadAsmAnimations(full_path.c_str());
                }
                g_asm_dialog_opponent = false;
                g_show_asm_anim = true;
            } else if (g_file_dialog_mode == FileDialogMode::OpenLod) {
                LodManifest manifest = ParseLodFile(full_path.c_str(),
                    g_lod_override_dir[0] ? g_lod_override_dir : nullptr);
                verbose_log("OpenLod: %s -> %zu entries, PPP=%d", full_path.c_str(), manifest.entries.size(), manifest.ppp_value);
                if (manifest.parse_error) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg), "LOD: %s", manifest.error_msg.c_str());
                    g_restore_msg_timer = 6.0f;
                } else {
                    if (manifest.has_ppp_value)
                        g_load2_ppp = manifest.ppp_value;

                    PrepareDocumentForOpenedFile();

                    std::string lod_dir(g_file_dialog_dir);

                    int loaded = 0;
                    for (size_t i = 0; i < manifest.entries.size(); i++) {
                        const std::string &rpath = manifest.entries[i].resolved_path;

                        size_t sep = rpath.find_last_of("\\/");
                        std::string dir, file;
                        if (sep != std::string::npos) {
                            dir  = rpath.substr(0, sep);
                            file = rpath.substr(sep + 1);
                        } else {
                            dir  = ".";
                            file = rpath;
                        }

                        size_t n_file = file.length();
                        if (n_file > 12) n_file = 12;

                        auto try_load = [&](const std::string &d) -> bool {
                            size_t nd = d.length();
                            if (nd > sizeof(g_doc->fpath_s) - 1) nd = sizeof(g_doc->fpath_s) - 1;
                            memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
                            memcpy(g_doc->fpath_s, d.c_str(), nd);

                            memset(g_doc->fname_s, 0, 13);
                            memcpy(g_doc->fname_s, file.c_str(), n_file);
                            for (size_t j = 0; j < n_file; j++)
                                g_doc->fname_s[j] = (char)toupper((unsigned char)g_doc->fname_s[j]);

                            unsigned int prev = g_doc->imgcnt;
                            _chdir(d.c_str());
                            LoadImgFile();
                            return g_doc->imgcnt > prev;
                        };

                        if (try_load(dir)) {
                            loaded++;
                        } else if (lod_dir != dir && try_load(lod_dir)) {
                            loaded++;
                        } else {
                            const char *imgdir = getenv("IMGDIR");
                            if (imgdir && imgdir[0] && std::string(imgdir) != dir && std::string(imgdir) != lod_dir) {
                                if (try_load(imgdir)) loaded++;
                            }
                        }
                    }

                    g_doc->ilselected = g_doc->imgcnt > 0 ? 0 : -1;
                    g_dirty = false;
                    RecentAdd(full_path);

                    int total = (int)manifest.entries.size();
                    if (loaded == 0)
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "LOD: 0/%d IMG(s) loaded. Check IMGDIR or file paths.", total);
                    else if (loaded < total)
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "LOD: %d/%d IMG(s) loaded%s", loaded, total,
                            manifest.has_ppp_value ? " (PPP set)" : "");
                    else
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "Loaded %d IMG(s) from LOD%s", loaded,
                            manifest.has_ppp_value ? " (PPP set)" : "");
                    g_restore_msg_timer = 4.0f;
                }
            } else {
                size_t n_dir = strlen(g_file_dialog_dir);
                if (n_dir > sizeof(g_doc->fpath_s) - 1) n_dir = sizeof(g_doc->fpath_s) - 1;
                memset(g_doc->fpath_s, 0, sizeof(g_doc->fpath_s));
                memcpy(g_doc->fpath_s, g_file_dialog_dir, n_dir);
                
                size_t n_file = strlen(g_file_dialog_file);
                if (n_file > 12) n_file = 12;
                memset(g_doc->fname_s, 0, 13);
                memset(g_doc->fnametmp_s, 0, 13);
                memcpy(g_doc->fname_s, g_file_dialog_file, n_file);
                memcpy(g_doc->fnametmp_s, g_file_dialog_file, n_file);
                for (size_t i = 0; i < n_file; i++) {
                    g_doc->fname_s[i] = (char)toupper((unsigned char)g_doc->fname_s[i]);
                    g_doc->fnametmp_s[i] = (char)toupper((unsigned char)g_doc->fnametmp_s[i]);
                }
                _chdir(g_file_dialog_dir);
                
                if (g_file_dialog_mode == FileDialogMode::SaveImg) {
                    SaveImgFile();
                    g_dirty = false; /* Mark as saved in C++ state */
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::AppendImg) {
                    /* Append adds images on top of the current set — even if
                       the file was clean (matched disk) before, after Append
                       it no longer does, so mark dirty. */
                    LoadImgFile();
                    mark_dirty();
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::LoadLbm) {
                    LoadLbm(full_path.c_str());
                    mark_dirty();
                } else if (g_file_dialog_mode == FileDialogMode::LoadTga) {
                    LoadTga(full_path.c_str());
                    mark_dirty();
                } else if (g_file_dialog_mode == FileDialogMode::SaveLbm) {
                    SaveLbm(full_path.c_str());
                } else if (g_file_dialog_mode == FileDialogMode::SaveTga) {
                    SaveTga(full_path.c_str());
                }
            }
            g_img_tex_idx = -2; /* Force canvas texture refresh */
            save_last_dir(g_file_dialog_dir, g_file_dialog_mode);
            file_preview_clear();
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            file_preview_clear();
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        if (FileDialogSupportsMultiSelect(g_file_dialog_mode) && g_file_dialog_multi_files.size() > 1)
            ImGui::TextDisabled("%d files selected", (int)g_file_dialog_multi_files.size());
        ImGui::EndPopup();
    }
}

/* Help modal */
static bool g_show_help = false;
static bool g_show_debug = false;
static bool g_show_about = false;
static const char *g_help_text =
    R"IMA(IMAGE TOOL HELP
================================================================================

QUICKSTART
----------
What is an IMG file?  An IMG file is a container of multiple sprites (images)
plus one or more palettes that colorize them.  A single IMG can hold hundreds
of frames -- e.g. NINJAS10.IMG probably holds every animation frame for a
ninja character.  Think of imgtool as a sprite-sheet editor.

Launch: Double-click imgtool.exe.  You'll see a mostly-black window waiting
for you to load something.

Step 1 -- Open a file:  Ctrl+O opens the file browser.
  Up/Down - scroll through files
  Enter   - open directory or load the selected file
  Backspace - go up one directory
  Esc     - cancel

Step 2 -- Read the screen:
  +----------------------------------+-------------------+
  |         MAIN IMAGE VIEW          |                   |
  |     current sprite drawn here    |   PROPERTIES      |
  |                                  |   (anim points,   |
  |                                  |    hitbox sliders)|
  +-----------------+----------------+-------------------+
  |  PALETTE LIST   |        IMAGE LIST                  |
  |  (pal names)    |   (all sprites in this IMG)        |
  +-----------------+------------------------------------+

  - Main view (center) -- the currently-selected sprite
  - Image list (right) -- every sprite in the file. "*" = marked
  - Palette list (top-right panel) -- palettes in this IMG
  - Bottom palette bar -- 256 color swatches for current palette

Step 3 -- Browse: Up/Down moves one sprite. PgUp/PgDn jumps a page.

Step 4 -- Mark sprites: Space toggles mark. M marks all, m clears all.
  Marking is how you select sprites for batch operations.

Step 5 -- Zoom: toolbar Z+/Z-, Ctrl+= / Ctrl+-, or Ctrl+mouse wheel zooms.
  Ctrl+0 fits the sprite. Mouse wheel scrolls vertically. Middle-mouse drag
  pans. Space + left drag also pans (Photoshop hand-tool style).

Step 6 -- Palettes: click a palette in the Palette list to set it on the
  active sprite. [ sets palette for marked sprites, ] for the current one
  (when no paint tool is active - Pencil reassigns [ and ] to brush size).

Step 7 -- Two IMGs at once: Tab swaps between list 1 and list 2. Open a
  second IMG after pressing Tab, then swap back with Tab.

Step 8 -- Save: Ctrl+S saves. Pre-2.x IMGs are auto-converted on load.


================================================================================
KEYBOARD REFERENCE
------------------

File:
  Ctrl+O               Open IMG
  Ctrl+S               Save IMG
  Alt+L  / Alt+S       Load / Save LBM
  Ctrl+L               Load TGA
  Ctrl+B               Build TGA from marked images
  Esc                  Quit (prompts on unsaved changes)

Edit:
  Ctrl+Z               Undo (paint stroke, anipoint, hitbox, palette ops)
  Ctrl+Y               Redo
  Ctrl+C / Ctrl+X / Ctrl+V   Copy / Cut / Paste
  Ctrl+Shift+X         Cut selection to a new sprite
  Ctrl+Shift+V         Paste clipboard as a new sprite
  Ctrl+A               Select all
  Ctrl+D               Deselect
  Ctrl+Shift+I         Invert selection
  Ctrl+J               Duplicate image (or duplicate floating paste)
  Ctrl+E               Merge Down: commit floating paste in place
  Ctrl+T               Free Transform floating paste (scale / rotate)
  H / V                Flip floating paste horizontally / vertically
  L                    Drop floating paste to a non-destructive sprite layer

Image list:
  Space                Mark / Unmark current image
  Shift+M              Set all marks (typed as "M")
  M                    Clear all marks (typed as "m")
  Del                  Delete image when image list is active
  Shift+Del            Delete image from anywhere
  Ctrl+R               Rename current image
  Ctrl+P               Add / Remove point table on current image
  Alt+PgUp / PgDn      Move current image up / down in the list
  Tab                  Swap image lists (lists 1 and 2)
  ;                    Least-squares size reduce on marked

Tools (toolbar shortcuts):
  P                    Pencil   - paint at current color
  G                    Paint Bucket - fill contiguous area
  V                    Variant Paint - target-palette-only color over opaque pixels
  R                    Marquee  - rectangular selection
  W                    Magic Wand (Ctrl-click adds)
  L                    Lasso
  I                    Eyedropper - pick color from canvas
  (no shortcut)        Smart Eraser, Clone Stamp, Smart Remap (toolbar buttons)

Brush-specific (only fire while Pencil or Variant Paint is active):
  [ / ]                Shrink / grow brush radius (1..16)

Palette (only fire when no paint tool is active):
  [                    Set palette for marked images
  ]                    Set palette for current image
  Shift+8 (`*`)        Merge marked palettes
  Shift+R              Rename selected palette
  Del                  Delete selected palette when palette list is active

Timeline / Anim:
  K                    Toggle timeline play / stop
  Left / Right         Step prev / next animation frame
  Ctrl+Left / Right    Move current timeline frame earlier / later
  Hold                 Set extra base ticks before the current frame advances
  Ctrl-click frames    Pair two frames; Play/Left/Right advances both
  Drag paired sprite   Move sprite by editing its anipoint; lock Back/Front to protect it
  Auto Anipts          With a paired frame locked, align the sequence by sprite sizes
  World Marked         Play marked animations from up to four IMG tabs together
  World Sequence       On-canvas Pause/Refresh plus per-frame delay thumbnails
  Dummy Body           Adds stock *DECAP1-7 body fall as an editable sync lane
  World Left / Right   Pause and scrub all marked-tab sequences together

View / Help:
  Ctrl+= / Ctrl+-      Zoom in / out
  Ctrl+0               Fit sprite to canvas
  H                    Show this help
  F9                   Debug info popup

Mouse on canvas:
  Mouse wheel          Scroll vertically
  Ctrl + mouse wheel   Zoom in / out from canvas center
  Middle drag          Pan
  Space + left drag    Pan (Photoshop hand-tool style)
  Right-click          Eyedrop (any tool mode)
  Blank left-drag      Starts marquee selection from transparent pixels
  Shift + left-click   Flood fill (when no select tool active)


================================================================================
MK2 STRIKE-TABLE EDITOR
-----------------------
Tools - MK2 Hitboxes (MKSTK.ASM)... opens an editor for the strike-table
source file used by the MK2 source tree. Imgtool parses MKSTK.ASM directly
and writes back in place, preserving comments, indentation, and symbolic
literals (e.g. sf_squeeze).

Workflow:
  Browse... - native file picker, remembers last directory
  Load      - parse the .asm into memory
  Save      - write the in-memory edits back to disk
  Reload    - re-read from disk, dropping unsaved edits
  Undo / Redo (and Ctrl+Z / Ctrl+Shift+Z when the panel has focus)

3-pane navigator:
  Left  - character codes (jc, lk, hh, nj, etc. - the two-letter labels
          MKSTK.ASM itself uses). The panel auto-jumps to the matching
          character when you open a sprite IMG with a recognized prefix
          (CAGE -> jc, KANG -> lk, HATHED -> hh, NINJAS -> nj, etc.).
  Mid   - moves for the selected character. Filter box at the top.
  Right - fields for the selected move: x/y/w/h, strike_routine (raw
          token), damage split into hit/block bytes, score (32-bit),
          sound (raw token).

Canvas overlay:
  The selected move's collision box draws on the active sprite as a
  magenta rectangle with drag-to-resize corner handles. The IMG-embedded
  hitbox overlay auto-hides while an MK2 move is selected so the two
  systems don't pile on top of each other.


================================================================================
FILE FORMATS
------------

IMG (Image Library) -- Primary format. Binary, little-endian, packed structs.
  LIB_HDR (28 bytes): g_doc->imgcnt, g_doc->palcnt, version (0x634+)
  IMAGE records (50 bytes each): name[16], flags, anix/y, w, h, palnum
  PALETTE records (26 bytes each): name[10], flags, bitspix, numc
  BLOB: raw pixel data (stride = (w+3)&~3) + 15-bit packed RGB palettes

TGA (TrueVision Targa) -- 8-bit color-mapped, bottom-up by default.
  Loads as new image+palette. Saves current image with its palette.

LBM (IFF/ILBM) -- Chunk-based format. CMAP (palette) + BODY (bitmap).
  Supports RLE-compressed body chunks.

PNG -- Import/Export via stb_image. Auto-quantizes colors to nearest
  15-bit palette on import. Exports RGBA with transparency for color 0.

Pre-2.x IMG files (version < 0x500) are auto-converted on open.
Max sprite size: 640x400. Max 2000 images/palettes per file.


================================================================================
DMA2 HARDWARE REFERENCE
-----------------------
The Williams DMA #2 (January 1992, Rev 1.5) handles pixel transfers between
image memory and the bitmap. This is the hardware that MK/NBA-era games used
to blit sprites to screen.  The following is the original document text:

)IMA"
R"dma2(
		THE BRAND SPANKING NEW DMA (#2)

			KEEP ENTERPRISES, INC.

			JANUARY 1, 1992

			DOCUMENT REV. 1.5


	DMA # 2 - GENERAL INFORMATION

	- THE NEW DMA WILL INCORPORATE BACKWARD COMPATIBILITY TO THE OLD DMA
	IN BOTH PINOUT AND FUNCTIONALITY.

	- THE NEW FEATURES IN ADDITION TO THE OLD ARE AS FOLLOWS:

		1) VARIABLE PIXEL SIZE PROCESSING.  THE NEW DMA CAN PROCESS
		   1 TO 8 BIT PIXELS THAT ARE STORED IN IMAGE MEMORY IN A
		   SERIAL FASHION.

		   EXAMPLE:  5 BIT PIXELS STORED INTO 8 BIT EPROM
		   +---+---+---+---+---+---+---+---+
		   |P1 |P1 |P1 |P1 |P1 |P2 |P2 |P2 |
		   +---+---+---+---+---+---+---+---+
		   |P2 |P2 |P3 |P3 |P3 |P3 |P3 |P4 |
		   +---+---+---+---+---+---+---+---+
		   |P4 |P4 |P4 |P4 |P5 |P5 |P5 |P5 |
		   +---+---+---+---+---+---+---+---+
		   |P5 |P6 |P6 |P6 |P6 |P6 |P7 |P7 |
		   +---+---+---+---+---+---+---+---+

		2) THE NEW DMA CAN BE HALTED IN THE MIDDLE OF A TRANSFER
		   AND THEN BE RESTARTED TO RESUME THE TRANSFER.  THIS IS
		   ACCOMPLISHED BY WRITING A ZERO TO THE DMA GO BIT (BIT 15)
		   IN THE CONTROL REGISTER.  IN THE OLD DMA, THIS WOULD KILL
		   THE TRANSFER SO THAT IT COULD NOT BE RESTARTED.  TO KILL
		   A TRANSFER IN THE NEW DMA, WRITE A ZERO TO THE DMA GO BIT 2
		   TIMES IN A ROW.  TO RESTART A TRANSFER AFTER HALTING,
		   WRITE A ONE TO THE DMA GO BIT IN THE CONTROL REGISTER.

		3) IN ADDITION TO THE CLIPPING ACHIEVED BY MANIPULATING THE
		   OFFSET REGISTER, IN THE NEW DMA, A METHOD OF CLIPPING
		   USING REGISTERED CLIP VALUES IS AVAILABLE. THE HOST CAN
		   SPECIFY CLIP AMOUNTS TO THE DMA AND THE MATH NEEDED TO
		   IMPLEMENT A TRANSFER IS DONE INTERNAL TO THE DMA.

		4) THE NEW DMA CAN DO A TRANSFER FROM THE IMAGE MEMORY TO THE
		   BIT MAP WITH A SCALING EFFECT, I.E. THE IMAGE CAN BE
		   SHRUNK OR ENLARGED.

		5) THE NEW DMA IMPLEMENTS A COMPRESSION MODE IN WHICH LEADING
		   AND TRAILING ZERO DATA PIXELS CAN BE ENCODED IN A RUN LENGTH
		   FASHION TO SAVE ON IMAGE MEMORY.

		6) OFF SCREEN CLIPPING CAN BE AUTOMATIC.  THERE ARE FOUR
		   REGISTERS THAT SPECIFY THE WINDOW TO WHICH THE DMA CAN
		   TRANSFER DATA.

		7) NOTE THAT THE CONTROL REGISTER AND THE OFFSET REGISTER
		   HAVE BEEN SWAPPED SO THAT THE MOVE MULTIPLE INSTRUCTION
		   CAN BE USED TO DOWNLOAD THE REGISTERS AND SET DMA GO
		   EFFICIENTLY.

	DMA # 2 - INTERNAL REGISTERS R5-R0

	    REGISTER # 7 - SOURCE VERTICAL SIZE REGISTER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 |           VERTICAL SIZE              |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 6 - SOURCE HORIZONTAL SIZE REGISTER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 |          HORIZONTAL SIZE             |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 5 - DESTINATION ADDRESS - Y
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 | 0 |      DESTINATION Y COORDINATE     |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 4 - DESTINATION ADDRESS - X
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    | 0 | 0 | 0 | 0 | 0 | 0 | 0 |      DESTINATION X COORDINATE     |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 3 - SOURCE ADDRESS - HIGH ORDER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |               SOURCE ADDRESS UPPER 16 BITS                   |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 2 - SOURCE ADDRESS - LOW ORDER
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |               SOURCE ADDRESS LOWER 16 BITS                   |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 1 - CONTROL REGISTER   ** SEE NOTE 1 BELOW
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |DGO|  PIX SIZE |TM1|TM0|LM1|LM0|CMP|CLP|VFL|HFL|   PIXEL OPS   |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    REGISTER # 0 - OFFSET REGISTER / RCLIP-LCLIP VALUES
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+
	    |            OFFSET VALUE FOR OLD STYLE CLIPPING               |
	    |   LEFT CLIP PIXELS VALUE     |    RIGHT CLIP PIXELS VALUE    |
	    +---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+---+

	    ** NOTE 1:
	    DGO      - BIT 15     - DMA GO / DMA HALT
	    PIX      - BITS 14-12 - PIXEL SIZE (0 = 8 BITS)
	    TM1      - BIT 11     - DMA COMPRESS TRAIL PIX MULT BIT 1
	    TM2      - BIT 10     - DMA COMPRESS TRAIL PIX MULT BIT 0
	    LM1      - BIT 9      - DMA COMPRESS LEAD  PIX MULT BIT 1
	    LM0      - BIT 8      - DMA COMPRESS LEAD  PIX MULT BIT 0
	    CMP      - BIT 7      - DMA COMPRESS MODE
	    CLP      - BIT 6      - DMA CLIP ON = 1 (USING U,D,L,R METHOD)
	    VFL      - BIT 5      - VERTICAL FLIP (FLIP ABOUT X AXIS)
	    HFL      - BIT 4      - HORIZONTAL FLIP (FLIP ABOUT Y AXIS)
	    OPS      - BITS 3-0   - PIXEL CONSTANT/SUBSTITUTION OPS

	    ** NOTE 2: IN COMPRESSION MODE, SCALING IS INHIBITED AND
	               CLIPPING IS INHIBITED.


	DMA # 2 - CLIPPING AN IMAGE

	OVERVIEW: AN IMAGE CAN BE TRANSFERRED TO THE BIT MAP IN ITS
	ENTIRETY OR A PORTION CAN BE "CLIPPED" BY SPECIFYING LEFT AND
	RIGHT CLIP AMOUNTS IN REGISTER 0, WITH BIT 6 (CLP) SET.

	IMPLEMENTATION:
	        - THE OFFSET METHOD (OLD STYLE): REGISTER 0 UPPER BYTE =
	          LEFT CLIP, LOWER BYTE = RIGHT CLIP.
	        - THE REGISTER CLIP METHOD (NEW FEATURE): SET CLP BIT TO 1,
	          USE REGISTERS 12 AND 13 FOR WINDOW BORDERS.


	DMA # 2 - TRANSFERRING A SCALED IMAGE

	OVERVIEW: AN IMAGE CAN BE SCALED BY TRAVERSING EACH LINE WITH A
	PREDETERMINED SAMPLE RATE.  RATIO IS 1:(INT + FRAC/256).
	REGISTER 11 = Y SCALE, REGISTER 10 = X SCALE.
	SCALE FACTORS: UPPER BYTE = INTEGER, LOWER BYTE = FRACTION.

	MAXIMUM SCALE FACTOR FOR SHRINK IN X DIRECTION:
	#BITS/PIXEL    INT  FRACTION
	-----------    ---- --------
	     1          1F    FF
	     2          10    00
	     3          0A    AA
	     4          08    00
	     5          06    66
	     6          05    55
	     7          04    92
	     8          04    00


	DMA # 2 - COMPRESSION OF LEADING AND TRAILING ZEROS

	TO SAVE IMAGE SPACE, LEADING AND TRAILING ZERO PIXELS ARE RUN-
	LENGTH ENCODED. THE FIRST BYTE OF EACH COMPRESSED LINE CONTAINS:
	UPPER NIBBLE = TRAILING ZERO COUNT, LOWER NIBBLE = LEADING ZERO
	COUNT. TMx/LMx BITS IN CONTROL REGISTER MULTIPLY THESE VALUES BY
	1, 2, 4, OR 8.

	IN COMPRESSION MODE, THE DMA DECODES THIS ON THE FLY IF:
	  CMP (BIT 7) = 1, TMx, LMx BITS = 0.


	DMA # 2 - OFF SCREEN CLIPPING (WINDOWING)

	FOUR REGISTERS SPECIFY WINDOW BOUNDARIES (0-511).  SET CONFIG
	REGISTER BIT 5 = 0 FOR LEFT/RIGHT, = 1 FOR UPPER/LOWER.
	REGISTERS 12/13 HOLD LEFT/TOP AND RIGHT/BOTTOM LIMITS.


	DMA # 2 - LIMITATIONS

	SCALING: THERE ARE SIZE LIMITS. SEE TABLE ABOVE FOR SHRINK/GROW
	MAX/MIN IN X DIRECTION. 1-BIT PIXELS CAN SHRINK TO 1F.FF (1:31.996).
	COMPRESSION: CLIPPING AND SCALING ARE DISABLED IN COMPRESS MODE.
)dma2"
R"IMA(
================================================================================
ABOUT
-----
midway-imgtool -- Editor for Midway arcade IMG container files (MK2/MK3,
NBA Jam, NBA Hangtime, etc.).  Originally a 1992 DOS tool by Shawn Liptak
(Williams Electronics), now a pure C/C++ + SDL2 + Dear ImGui port.
SDL-main branch -- 64-bit build.  github.com/junkwax/midway-imgtool
)IMA";

/* ---- Palette persistence ----
   The asm stores palette colors as packed 15-bit RGB words in pal->data_p
   (2 bytes per color). Edits to g_palette[] are written back so subsequent
   Save persists them. */
static void palette_writeback(int color_idx)
{
    mark_dirty();
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (color_idx < 0 || color_idx >= (int)pal->numc) return;

    SDL_Color &c = g_palette[color_idx];
    rgb8_to_pal_word(c.r, c.g, c.b, (unsigned char *)pal->data_p + color_idx * 2);
}

/* Rebuild the working palette from the baseline by applying the three HSL
 * sliders' current absolute values. Saturation and lightness must run off
 * the baseline (not the previous frame) because both are lossy at the
 * extremes — once a color hits 0% saturation there's no hue info left to
 * restore, so a delta-based slider would only ratchet one way. Hue is
 * non-lossy and could be incremental, but doing all three from baseline
 * means Reset is just sliders -> 0 and the math stays trivial. */
static void hsl_adjust_palette_from_baseline(int hue_deg, int sat_pct, int light_pct)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (g_palette_baseline_nc == 0) return;
    mark_dirty();

    int n = (int)pal->numc;
    if (n > 256) n = 256;
    if (n > g_palette_baseline_nc) n = g_palette_baseline_nc;

    unsigned char rgb[256 * 3];
    HslAdjustPaletteWordsFromBaseline(g_palette_baseline, n,
                                      g_palette_selection,
                                      hue_deg, sat_pct, light_pct,
                                      (unsigned char *)pal->data_p,
                                      rgb);

    for (int i = 0; i < n; i++) {
        g_palette[i].r = rgb[i * 3 + 0];
        g_palette[i].g = rgb[i * 3 + 1];
        g_palette[i].b = rgb[i * 3 + 2];
    }
}

/* Back-compat shim: existing call sites use hue_shift_palette(delta_deg).
 * Reroute through the baseline-aware adjuster using the cumulative slider
 * state already maintained in g_hue_slider. delta_deg is ignored. */
static void hue_shift_palette(int /*delta_deg*/)
{
    hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
}

static void save_palette_baseline(void)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) { g_palette_baseline_nc = 0; return; }
    int nc = (int)pal->numc;
    if (nc > 256) nc = 256;
    memcpy(g_palette_baseline, pal->data_p, nc * 2);
    g_palette_baseline_nc = nc;
}

static void reset_palette_adjust_sliders(void)
{
    g_hue_slider = 0;
    g_hue_last   = 0;
    g_sat_slider = 0;
    g_sat_last   = 0;
    g_light_slider = 0;
    g_light_last   = 0;
}

static void commit_palette_adjustments(void)
{
    save_palette_baseline();
    reset_palette_adjust_sliders();
}

static void reset_palette_to_baseline(void)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p || g_palette_baseline_nc == 0) return;
    int nc = (int)pal->numc;
    if (nc > 256) nc = 256;
    if (nc > g_palette_baseline_nc) nc = g_palette_baseline_nc;
    memcpy(pal->data_p, g_palette_baseline, nc * 2);
    ApplyPalette(g_doc->plselected);
    reset_palette_adjust_sliders();
    mark_dirty();
}

static void ClearWorkingPalette(void)
{
    for (int i = 0; i < 256; i++) {
        g_palette[i].r = 0;
        g_palette[i].g = 0;
        g_palette[i].b = 0;
        g_palette[i].a = 255;
    }
}

/* Load the selected palette into g_palette[] so the canvas/swatches reflect it. */
static void ApplyPalette(int pal_idx)
{
    if (pal_idx < 0) {
        ClearWorkingPalette();
        return;
    }
    PAL *pal = get_pal(pal_idx);
    if (!pal || !pal->data_p) {
        ClearWorkingPalette();
        return;
    }
    const unsigned char *src = (const unsigned char *)pal->data_p;
    int n = pal->numc;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        pal_word_to_rgb8(src + i * 2, &g_palette[i].r, &g_palette[i].g, &g_palette[i].b);
        g_palette[i].a = 255;
    }
    for (int i = n; i < 256; i++) {
        g_palette[i].r = 0;
        g_palette[i].g = 0;
        g_palette[i].b = 0;
        g_palette[i].a = 255;
    }
}

/* ---- Image texture renderer ---- */
static void rebuild_img_texture(IMG *img)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        if (g_img_texture) { SDL_DestroyTexture(g_img_texture); g_img_texture = NULL; }
        g_img_tex_w = g_img_tex_h = 0;
        return;
    }
    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;

    if (!g_img_texture || g_img_tex_w != w || g_img_tex_h != h) {
        if (g_img_texture) SDL_DestroyTexture(g_img_texture);
        g_img_texture = SDL_CreateTexture(g_imgui_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureBlendMode(g_img_texture, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_img_texture, SDL_ScaleModeNearest);
        g_img_tex_w = w;
        g_img_tex_h = h;
    }
    void *pixels; int pitch;
    if (SDL_LockTexture(g_img_texture, NULL, &pixels, &pitch) != 0) return;
    const unsigned char *src = (const unsigned char *)img->data_p;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned char ci = src[y * stride + x];
            SDL_Color c = g_palette[ci];
            Uint32 a = (ci == 0) ? 0x00u : 0xFFu;
            dst[y * (pitch / 4) + x] = (a << 24) | ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | c.b;
        }
    }

    /* Composite an attached overlay layer directly into the texture for the
       canvas preview (non-destructive — base data_p is untouched). */
    SpriteLayer *L = img_layer(img);
    if (L && L->visible) {
        const unsigned char *lp = layer_pixels(L);
        for (int ly = 0; ly < L->h; ly++) {
            int dy = L->y + ly;
            if (dy < 0 || dy >= h) continue;
            const unsigned char *lrow = lp + (size_t)ly * L->stride;
            for (int lx = 0; lx < L->w; lx++) {
                int dx = L->x + lx;
                if (dx < 0 || dx >= w) continue;
                unsigned char ci = lrow[lx];
                if (ci == 0) continue;
                SDL_Color c = g_palette[ci];
                dst[dy * (pitch / 4) + dx] =
                    (0xFFu << 24) | ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | c.b;
            }
        }
    }
    SDL_UnlockTexture(g_img_texture);
}

/* ---- Undo helpers ---- */
void undo_push(void)
{
    mark_dirty();
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;

    if (g_undo_idx >= 0) {
        EditSnapshot *last = &g_undo[g_undo_idx];
        if (last->image_idx == g_doc->ilselected &&
            last->anix == img->anix && last->aniy == img->aniy &&
            last->anix2 == img->anix2 && last->aniy2 == img->aniy2 &&
            last->aniz2 == img->aniz2 &&
            last->hitbox_x == g_hitbox_x && last->hitbox_y == g_hitbox_y &&
            last->hitbox_w == g_hitbox_w && last->hitbox_h == g_hitbox_h)
            return;
    }

    if (g_undo_idx < UNDO_STACK_SIZE - 1) {
        g_undo_idx++;
    } else {
        for (int i = 0; i < UNDO_STACK_SIZE - 1; i++)
            g_undo[i] = g_undo[i + 1];
    }

    EditSnapshot *s = &g_undo[g_undo_idx];
    s->seq = ++g_undo_seq;
    s->image_idx = g_doc->ilselected;
    s->anix  = img->anix;  s->aniy  = img->aniy;
    s->anix2 = img->anix2; s->aniy2 = img->aniy2; s->aniz2 = img->aniz2;
    s->w = img->w; s->h = img->h;
    s->palnum = img->palnum; s->flags = img->flags;
    s->hitbox_x = g_hitbox_x; s->hitbox_y = g_hitbox_y;
    s->hitbox_w = g_hitbox_w; s->hitbox_h = g_hitbox_h;
    g_undo_count = g_undo_idx + 1;
    for (auto &r : g_pixel_redo) pixel_hist_free(&r);
    g_pixel_redo.clear();
    ClearDocumentRedoStack();
}

static void undo_apply(int idx)
{
    if (idx < 0 || idx >= g_undo_count) return;
    EditSnapshot *s = &g_undo[idx];
    IMG *img = get_img(s->image_idx);
    if (!img) return;
    img->anix  = s->anix;  img->aniy  = s->aniy;
    img->anix2 = s->anix2; img->aniy2 = s->aniy2; img->aniz2 = s->aniz2;
    img->w = s->w; img->h = s->h;
    img->palnum = s->palnum; img->flags = s->flags;
    g_hitbox_x = s->hitbox_x; g_hitbox_y = s->hitbox_y;
    g_hitbox_w = s->hitbox_w; g_hitbox_h = s->hitbox_h;
}

/* ---- Copy/Paste helpers (pixel data only) ---- */
static void copy_image(bool cut)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    /* Cut clears pixels in the source — capture them for pixel-level undo
       (undo_push only saves metadata, which left cut un-undoable). */
    PixelHist cut_snap = {};
    bool cut_captured = cut && pixel_hist_capture(&cut_snap, false);

    ClearPixelClipboard();

    int x1 = 0, y1 = 0, x2 = img->w - 1, y2 = img->h - 1;

    /* If grid selection is active, copy only the selected region */
    if (g_grid_sel.active) {
        x1 = g_grid_sel.x1; y1 = g_grid_sel.y1;
        x2 = g_grid_sel.x2; y2 = g_grid_sel.y2;
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        if (x1 < 0) x1 = 0; if (x1 >= (int)img->w) x1 = img->w - 1;
        if (y1 < 0) y1 = 0; if (y1 >= (int)img->h) y1 = img->h - 1;
        if (x2 < 0) x2 = 0; if (x2 >= (int)img->w) x2 = img->w - 1;
        if (y2 < 0) y2 = 0; if (y2 >= (int)img->h) y2 = img->h - 1;
    }

    int w = (x2 - x1) + 1;
    int h = (y2 - y1) + 1;
    int origin_x = x1;
    int origin_y = y1;
    unsigned short stride = (img->w + 3) & ~3;
    unsigned short clip_stride = (w + 3) & ~3;
    unsigned int size = clip_stride * h;

    /* Copy selected pixel data */
    g_clipboard.data_p = malloc(size);
    if (!g_clipboard.data_p) { if (cut_captured) pixel_hist_free(&cut_snap); return; }

    for (int y = 0; y < h; y++) {
        unsigned char *src = (unsigned char *)img->data_p + (y1 + y) * stride + x1;
        unsigned char *dst = (unsigned char *)g_clipboard.data_p + y * clip_stride;
        if (g_grid_sel.active && g_grid_sel.is_mask) {
            for (int x = 0; x < w; x++) {
                if (g_grid_sel.pixel_mask[(y1 + y) * g_grid_sel.mask_w + (x1 + x)]) {
                    dst[x] = src[x];
                    if (cut) src[x] = 0;
                } else {
                    dst[x] = 0;
                }
            }
        } else {
            memcpy(dst, src, w);
            if (cut) {
                memset(src, 0, w);
            }
        }
    }

    if (cut) {
        if (cut_captured) push_pixel_history_entry(&cut_snap);
        mark_dirty();
        g_img_tex_idx = -2;
        /* Don't drop the marquee on cut, acts more like Photoshop where selection stays */
    }

    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.stride = clip_stride;
    g_clipboard.valid = true;
    g_clipboard.has_meta = true;
    g_clipboard.has_opaque = false;
    g_clipboard.from_cut = cut;
    g_clipboard.origin_x = origin_x;
    g_clipboard.origin_y = origin_y;
    g_clipboard.palnum = img->palnum;
    g_clipboard.anix = img->anix;
    g_clipboard.aniy = img->aniy;
    g_clipboard.anix2 = img->anix2;
    g_clipboard.aniy2 = img->aniy2;
    g_clipboard.aniz2 = img->aniz2;
    g_clipboard.opals = img->opals;
    g_clipboard.has_palette = false;
    g_clipboard.palette_numc = 0;
    memset(g_clipboard.palette_data, 0, sizeof(g_clipboard.palette_data));
    {
        PAL *clip_pal = get_pal(img->palnum);
        if (clip_pal && clip_pal->data_p && clip_pal->numc > 0) {
            int n = clip_pal->numc;
            if (n > 256) n = 256;
            memcpy(g_clipboard.palette_data, clip_pal->data_p, (size_t)n * 2u);
            g_clipboard.palette_numc = (unsigned short)n;
            g_clipboard.has_palette = true;
        }
    }
    strncpy(g_clipboard.source_name, img->n_s, 15);
    g_clipboard.source_name[15] = '\0';
    strncpy(g_clipboard.src_filename, img->src_filename, sizeof(g_clipboard.src_filename) - 1);
    g_clipboard.src_filename[sizeof(g_clipboard.src_filename) - 1] = '\0';

    /* Tight-crop the clipboard to its non-transparent content bbox. Adobe
       behaviour: a cut/copy carries the visible pixels, not the empty
       transparent padding around them. Without this, a small motif inside a
       large marquee pastes off-center because the rect is bigger than what
       the user actually sees. The crop is applied to *all* copies (not just
       marquee or mask copies) so a full-image copy still drops the empty
       margin most sprite frames have around them. */
    {
        unsigned char *cd = (unsigned char *)g_clipboard.data_p;
        int min_x = w, min_y = h, max_x = -1, max_y = -1;
        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                if (cd[y * clip_stride + x] != 0) {
                    if (x < min_x) min_x = x;
                    if (x > max_x) max_x = x;
                    if (y < min_y) min_y = y;
                    if (y > max_y) max_y = y;
                }
            }
        }
        if (max_x >= 0 && max_y >= 0) {
            g_clipboard.has_opaque = true;
            if (min_x > 0 || min_y > 0 || max_x < w - 1 || max_y < h - 1) {
                int nw = max_x - min_x + 1;
                int nh = max_y - min_y + 1;
                unsigned short nstride = (unsigned short)((nw + 3) & ~3);
                unsigned int nsize = (unsigned int)nstride * nh;
                unsigned char *nbuf = (unsigned char *)malloc(nsize);
                if (nbuf) {
                    memset(nbuf, 0, nsize);
                    for (int y = 0; y < nh; y++) {
                        memcpy(nbuf + y * nstride,
                               cd + (min_y + y) * clip_stride + min_x,
                               nw);
                    }
                    free(g_clipboard.data_p);
                    g_clipboard.data_p = nbuf;
                    g_clipboard.w      = (unsigned short)nw;
                    g_clipboard.h      = (unsigned short)nh;
                    g_clipboard.stride = nstride;
                    g_clipboard.origin_x += min_x;
                    g_clipboard.origin_y += min_y;
                }
            }
        }
        /* If max_x < 0 the selection was entirely transparent; the clipboard
           is left as-is (the user explicitly copied empty pixels — possibly
           intentional for blanking). */
    }
}

static void PasteClipboardAsNewImage(void)
{
    if (!g_clipboard.valid || !g_clipboard.data_p || g_clipboard.w == 0 || g_clipboard.h == 0) return;

    /* Adds a whole new image — needs a document snapshot so undo removes it
       (undo_push only restores the selected image's metadata). */
    doc_undo_push();

    IMG *dst = (IMG *)AllocImg();
    if (!dst) return;

    int w = g_clipboard.w;
    int h = g_clipboard.h;
    int src_stride = g_clipboard.stride;
    int dst_stride = (w + 3) & ~3;
    size_t sz = (size_t)dst_stride * h;

    dst->data_p = PoolAlloc(sz);
    if (!dst->data_p) {
        unlink_and_free_img(dst);
        return;
    }

    unsigned char *src = (unsigned char *)g_clipboard.data_p;
    unsigned char *dp = (unsigned char *)dst->data_p;
    for (int y = 0; y < h; y++) {
        memcpy(dp + y * dst_stride, src + y * src_stride, w);
    }

    dst->w = (unsigned short)w;
    dst->h = (unsigned short)h;
    dst->flags = 0;
    dst->palnum = g_clipboard.has_meta ? g_clipboard.palnum
                 : (g_doc->plselected >= 0 ? (unsigned short)g_doc->plselected : 0);
    if (g_doc->palcnt > 0 && dst->palnum >= g_doc->palcnt)
        dst->palnum = (g_doc->plselected >= 0 && (unsigned)g_doc->plselected < g_doc->palcnt)
                    ? (unsigned short)g_doc->plselected : 0;
    dst->opals = g_clipboard.has_meta ? g_clipboard.opals : 0;
    if (g_clipboard.has_meta) dst->aniz2 = g_clipboard.aniz2;
    else clear_secondary_anipoint(dst);

    if (g_clipboard.has_meta) {
        dst->anix  = (unsigned short)((short)g_clipboard.anix  - (short)g_clipboard.origin_x);
        dst->aniy  = (unsigned short)((short)g_clipboard.aniy  - (short)g_clipboard.origin_y);
        if (clipboard_secondary_anipoint_in_use()) {
            dst->anix2 = (unsigned short)((short)g_clipboard.anix2 - (short)g_clipboard.origin_x);
            dst->aniy2 = (unsigned short)((short)g_clipboard.aniy2 - (short)g_clipboard.origin_y);
        } else {
            clear_secondary_anipoint(dst);
        }
        strncpy(dst->src_filename, g_clipboard.src_filename, sizeof(dst->src_filename) - 1);
        dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';
    }

    MakeDerivedImageName(g_clipboard.source_name[0] ? g_clipboard.source_name : "PASTE",
                         g_clipboard.from_cut ? "CUT" : "CPY",
                         dst->n_s);

    g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_palette_nav = false;
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Pasted clipboard as new sprite: %dx%d.", w, h);
    g_restore_msg_timer = 4.0f;
}

static void CutSelectionToNewImage(void)
{
    if (g_doc->ilselected < 0) return;
    copy_image(true);
    if (g_clipboard.valid) PasteClipboardAsNewImage();
}

static void CopySelectionToNewImage(void)
{
    if (g_doc->ilselected < 0) return;
    copy_image(false);
    if (g_clipboard.valid) PasteClipboardAsNewImage();
}

static const PasteBlendMode k_paste_blend_modes[] = {
    PasteBlendMode::Normal,
    PasteBlendMode::Dissolve,
    PasteBlendMode::Darken,
    PasteBlendMode::Multiply,
    PasteBlendMode::ColorBurn,
    PasteBlendMode::LinearBurn,
    PasteBlendMode::Lighten,
    PasteBlendMode::Screen,
    PasteBlendMode::ColorDodge,
    PasteBlendMode::Overlay,
    PasteBlendMode::SoftLight,
    PasteBlendMode::HardLight,
    PasteBlendMode::Difference,
    PasteBlendMode::Exclusion
};

static const char *PasteBlendModeName(PasteBlendMode mode)
{
    switch (mode) {
    case PasteBlendMode::Normal:     return "Normal";
    case PasteBlendMode::Dissolve:   return "Dissolve";
    case PasteBlendMode::Darken:     return "Darken";
    case PasteBlendMode::Multiply:   return "Multiply";
    case PasteBlendMode::ColorBurn:  return "Color Burn";
    case PasteBlendMode::LinearBurn: return "Linear Burn";
    case PasteBlendMode::Lighten:    return "Lighten";
    case PasteBlendMode::Screen:     return "Screen";
    case PasteBlendMode::ColorDodge: return "Color Dodge";
    case PasteBlendMode::Overlay:    return "Overlay";
    case PasteBlendMode::SoftLight:  return "Soft Light";
    case PasteBlendMode::HardLight:  return "Hard Light";
    case PasteBlendMode::Difference: return "Difference";
    case PasteBlendMode::Exclusion:  return "Exclusion";
    }
    return "Normal";
}

struct PasteRGB {
    int r, g, b;
};

static int paste_clamp_byte(int v)
{
    if (v < 0) return 0;
    if (v > 255) return 255;
    return v;
}

static PasteRGB paste_rgb_from_target_index(const PAL *pal, unsigned char ci)
{
    if (pal && pal->data_p && ci < pal->numc && ci < 256) {
        const unsigned char *pd = (const unsigned char *)pal->data_p;
        unsigned char r = 0, g = 0, b = 0;
        pal_word_to_rgb8(pd + ci * 2, &r, &g, &b);
        return {(int)r, (int)g, (int)b};
    }
    SDL_Color c = g_palette[ci];
    return {(int)c.r, (int)c.g, (int)c.b};
}

static PasteRGB paste_rgb_from_clipboard_index(unsigned char ci, const PAL *target_pal,
                                               const unsigned char pal_map[256],
                                               bool remap_palette)
{
    if (g_clipboard.has_palette && ci < g_clipboard.palette_numc && ci < 256) {
        unsigned char r = 0, g = 0, b = 0;
        pal_word_to_rgb8(g_clipboard.palette_data + ci * 2, &r, &g, &b);
        return {(int)r, (int)g, (int)b};
    }
    unsigned char draw_ci = remap_palette ? pal_map[ci] : ci;
    return paste_rgb_from_target_index(target_pal, draw_ci);
}

static PasteRGB paste_quantize_rgb_to_target(const PAL *target_pal, PasteRGB rgb)
{
    if (!target_pal || !target_pal->data_p || target_pal->numc <= 1) return rgb;
    unsigned short word = rgb_to_word15((unsigned char)paste_clamp_byte(rgb.r),
                                        (unsigned char)paste_clamp_byte(rgb.g),
                                        (unsigned char)paste_clamp_byte(rgb.b));
    int idx = FindNearestPaletteSlot(target_pal, word);
    return paste_rgb_from_target_index(target_pal, (unsigned char)idx);
}

static unsigned int paste_dissolve_hash(int x, int y, unsigned char src_ci)
{
    unsigned int h = (unsigned int)x * 73856093u
                   ^ (unsigned int)y * 19349663u
                   ^ (unsigned int)src_ci * 83492791u;
    h ^= h >> 13;
    h *= 1274126177u;
    h ^= h >> 16;
    return h;
}

static bool paste_dissolve_keeps(int x, int y, unsigned char src_ci, int opacity)
{
    if (opacity >= 100) return true;
    if (opacity <= 0) return false;
    return (int)(paste_dissolve_hash(x, y, src_ci) % 100u) < opacity;
}

static int paste_blend_channel(PasteBlendMode mode, int s, int d)
{
    s = paste_clamp_byte(s);
    d = paste_clamp_byte(d);
    switch (mode) {
    case PasteBlendMode::Darken:
        return (s < d) ? s : d;
    case PasteBlendMode::Multiply:
        return (s * d + 127) / 255;
    case PasteBlendMode::ColorBurn:
        return (s == 0) ? 0 : paste_clamp_byte(255 - ((255 - d) * 255 + s / 2) / s);
    case PasteBlendMode::LinearBurn:
        return paste_clamp_byte(s + d - 255);
    case PasteBlendMode::Lighten:
        return (s > d) ? s : d;
    case PasteBlendMode::Screen:
        return 255 - ((255 - s) * (255 - d) + 127) / 255;
    case PasteBlendMode::ColorDodge:
        return (s >= 255) ? 255 : paste_clamp_byte((d * 255 + (255 - s) / 2) / (255 - s));
    case PasteBlendMode::Overlay:
        return (d < 128)
            ? paste_clamp_byte((2 * s * d + 127) / 255)
            : paste_clamp_byte(255 - (2 * (255 - s) * (255 - d) + 127) / 255);
    case PasteBlendMode::SoftLight: {
        float sf = (float)s / 255.0f;
        float df = (float)d / 255.0f;
        float out = (sf < 0.5f)
            ? (df - (1.0f - 2.0f * sf) * df * (1.0f - df))
            : (df + (2.0f * sf - 1.0f) * (sqrtf(df) - df));
        return paste_clamp_byte((int)(out * 255.0f + 0.5f));
    }
    case PasteBlendMode::HardLight:
        return (s < 128)
            ? paste_clamp_byte((2 * s * d + 127) / 255)
            : paste_clamp_byte(255 - (2 * (255 - s) * (255 - d) + 127) / 255);
    case PasteBlendMode::Difference:
        return (s > d) ? (s - d) : (d - s);
    case PasteBlendMode::Exclusion:
        return paste_clamp_byte(s + d - (2 * s * d + 127) / 255);
    case PasteBlendMode::Normal:
    case PasteBlendMode::Dissolve:
    default:
        return s;
    }
}

static bool paste_composite_rgb(PasteBlendMode mode, PasteRGB src, PasteRGB dst,
                                int opacity, int x, int y, unsigned char src_ci,
                                PasteRGB *out)
{
    if (!out) return false;
    if (opacity <= 0) return false;
    if (opacity > 100) opacity = 100;

    if (mode == PasteBlendMode::Dissolve) {
        if (!paste_dissolve_keeps(x, y, src_ci, opacity)) return false;
        *out = src;
        return true;
    }

    PasteRGB blended = src;
    if (mode != PasteBlendMode::Normal) {
        blended.r = paste_blend_channel(mode, src.r, dst.r);
        blended.g = paste_blend_channel(mode, src.g, dst.g);
        blended.b = paste_blend_channel(mode, src.b, dst.b);
    }

    out->r = paste_clamp_byte((dst.r * (100 - opacity) + blended.r * opacity + 50) / 100);
    out->g = paste_clamp_byte((dst.g * (100 - opacity) + blended.g * opacity + 50) / 100);
    out->b = paste_clamp_byte((dst.b * (100 - opacity) + blended.b * opacity + 50) / 100);
    return true;
}

static unsigned char paste_composite_index(unsigned char src_ci, unsigned char dst_ci,
                                           const PAL *target_pal,
                                           const unsigned char pal_map[256],
                                           bool remap_palette, int x, int y)
{
    if (src_ci == 0) return dst_ci;
    int opacity = g_paste_opacity;
    if (opacity <= 0) return dst_ci;
    if (opacity > 100) opacity = 100;

    unsigned char mapped = remap_palette ? pal_map[src_ci] : src_ci;
    if (g_paste_blend_mode == PasteBlendMode::Normal && opacity >= 100)
        return mapped;

    if (g_paste_blend_mode == PasteBlendMode::Dissolve)
        return paste_dissolve_keeps(x, y, src_ci, opacity) ? mapped : dst_ci;

    if (dst_ci == 0 || !target_pal || !target_pal->data_p || target_pal->numc <= 1)
        return mapped;

    PasteRGB src = paste_rgb_from_clipboard_index(src_ci, target_pal, pal_map, remap_palette);
    PasteRGB dst = paste_rgb_from_target_index(target_pal, dst_ci);
    PasteRGB out;
    if (!paste_composite_rgb(g_paste_blend_mode, src, dst, opacity, x, y, src_ci, &out))
        return dst_ci;

    unsigned short word = rgb_to_word15((unsigned char)out.r,
                                        (unsigned char)out.g,
                                        (unsigned char)out.b);
    return (unsigned char)FindNearestPaletteSlot(target_pal, word);
}

static bool paste_preview_rgba(unsigned char src_ci, unsigned char dst_ci,
                               const PAL *target_pal,
                               const unsigned char pal_map[256],
                               bool remap_palette, int x, int y,
                               int *r, int *g, int *b, int *a)
{
    if (src_ci == 0 || !r || !g || !b || !a) return false;
    int opacity = g_paste_opacity;
    if (opacity <= 0) return false;
    if (opacity > 100) opacity = 100;

    unsigned char mapped = remap_palette ? pal_map[src_ci] : src_ci;
    PasteRGB src = (g_paste_blend_mode == PasteBlendMode::Normal)
        ? paste_rgb_from_target_index(target_pal, mapped)
        : paste_rgb_from_clipboard_index(src_ci, target_pal, pal_map, remap_palette);

    if (g_paste_blend_mode == PasteBlendMode::Normal && opacity >= 100) {
        *r = src.r; *g = src.g; *b = src.b; *a = 255;
        return true;
    }

    if (g_paste_blend_mode == PasteBlendMode::Dissolve) {
        if (!paste_dissolve_keeps(x, y, src_ci, opacity)) return false;
        *r = src.r; *g = src.g; *b = src.b; *a = 255;
        return true;
    }

    if (dst_ci == 0) {
        *r = src.r; *g = src.g; *b = src.b;
        *a = (g_paste_blend_mode == PasteBlendMode::Normal)
            ? paste_clamp_byte((opacity * 255 + 50) / 100)
            : 255;
        return true;
    }

    PasteRGB dst = paste_rgb_from_target_index(target_pal, dst_ci);
    PasteRGB out;
    if (!paste_composite_rgb(g_paste_blend_mode, src, dst, opacity, x, y, src_ci, &out))
        return false;
    out = paste_quantize_rgb_to_target(target_pal, out);
    *r = out.r; *g = out.g; *b = out.b; *a = 255;
    return true;
}

/* Mirror the floating clipboard in place so a paste can be flipped before it
   is committed with Enter. Operates on palette indices, so it is lossless.
   The floating overlay is drawn straight from the clipboard each frame, so the
   preview updates immediately. */
static void flip_clipboard_horizontal(void)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int w = g_clipboard.w, h = g_clipboard.h, stride = g_clipboard.stride;
    unsigned char *d = (unsigned char *)g_clipboard.data_p;
    for (int y = 0; y < h; y++) {
        unsigned char *row = d + (size_t)y * stride;
        for (int x = 0; x < w / 2; x++) {
            unsigned char t = row[x];
            row[x] = row[w - 1 - x];
            row[w - 1 - x] = t;
        }
    }
}

static void flip_clipboard_vertical(void)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int w = g_clipboard.w, h = g_clipboard.h, stride = g_clipboard.stride;
    unsigned char *d = (unsigned char *)g_clipboard.data_p;
    for (int y = 0; y < h / 2; y++) {
        unsigned char *r0 = d + (size_t)y * stride;
        unsigned char *r1 = d + (size_t)(h - 1 - y) * stride;
        for (int x = 0; x < w; x++) {
            unsigned char t = r0[x]; r0[x] = r1[x]; r1[x] = t;
        }
    }
}

/* Permanently merge the layer into the host image's pixels and drop it. */
static void flatten_img_layer(IMG *img)
{
    SpriteLayer *L = img_layer(img);
    if (!L || !img->data_p) { if (L) { free(img->layer_p); img->layer_p = NULL; } return; }
    if (L->visible) {
        int stride = (img->w + 3) & ~3;
        composite_layer_onto(L, (unsigned char *)img->data_p, img->w, img->h, stride);
    }
    free(img->layer_p);
    img->layer_p = NULL;
    g_img_tex_idx = -2;
}

static void delete_img_layer(IMG *img)
{
    if (img && img->layer_p) { free(img->layer_p); img->layer_p = NULL; g_img_tex_idx = -2; }
}

static void flip_layer_horizontal(SpriteLayer *L)
{
    if (!L) return;
    unsigned char *p = layer_pixels(L);
    for (int y = 0; y < L->h; y++) {
        unsigned char *row = p + (size_t)y * L->stride;
        for (int x = 0; x < L->w / 2; x++) {
            unsigned char t = row[x]; row[x] = row[L->w - 1 - x]; row[L->w - 1 - x] = t;
        }
    }
}
static void flip_layer_vertical(SpriteLayer *L)
{
    if (!L) return;
    unsigned char *p = layer_pixels(L);
    for (int y = 0; y < L->h / 2; y++) {
        unsigned char *r0 = p + (size_t)y * L->stride;
        unsigned char *r1 = p + (size_t)(L->h - 1 - y) * L->stride;
        for (int x = 0; x < L->w; x++) { unsigned char t = r0[x]; r0[x] = r1[x]; r1[x] = t; }
    }
}

/* Turn the active floating paste into a layer on the selected sprite. The
   clipboard indices are remapped to the host palette first (same nearest-color
   mapping a normal paste uses) so the layer composites with a plain copy.
   Replaces any existing layer (single-overlay model). */
static void drop_paste_to_layer(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    int w = g_clipboard.w, h = g_clipboard.h;
    if (w <= 0 || h <= 0) return;
    int stride = (w + 3) & ~3;

    SpriteLayer *L = (SpriteLayer *)malloc(layer_total_bytes(w, h));
    if (!L) return;

    doc_undo_push();

    L->w = w; L->h = h; L->stride = stride;
    L->x = g_pasted.paste_x; L->y = g_pasted.paste_y;
    L->visible = 1;

    unsigned char pal_map[256];
    PAL *target_pal = get_pal(img->palnum);
    bool remap = BuildClipboardPaletteMap(target_pal, pal_map);

    unsigned char *dpix = layer_pixels(L);
    int clip_stride = g_clipboard.stride;
    const unsigned char *sp = (const unsigned char *)g_clipboard.data_p;
    for (int y = 0; y < h; y++) {
        unsigned char *drow = dpix + (size_t)y * stride;
        const unsigned char *srow = sp + (size_t)y * clip_stride;
        for (int x = 0; x < w; x++) {
            unsigned char ci = srow[x];
            drow[x] = (ci && remap) ? pal_map[ci] : ci;
        }
        for (int x = w; x < stride; x++) drow[x] = 0;   /* pad */
    }

    if (img->layer_p) free(img->layer_p);
    img->layer_p = L;

    /* The paste has become the layer; clear the floating paste. */
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_img_tex_idx = -2;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Dropped paste to layer (%dx%d). Edit it in the Sprite Layer panel; "
             "it flattens on save.", w, h);
    g_restore_msg_timer = 5.0f;
}

static void apply_pasted_region(void)
{
    mark_dirty();
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    /* Capture pre-paste pixels so committing a paste is undoable. */
    PixelHist paste_snap = {};
    bool paste_captured = pixel_hist_capture(&paste_snap, false);

    unsigned short stride = (img->w + 3) & ~3;
    unsigned short clip_stride = g_clipboard.stride;
    int px = g_pasted.paste_x, py = g_pasted.paste_y;
    int pw = g_clipboard.w, ph = g_clipboard.h;
    unsigned char pal_map[256];
    PAL *target_pal = get_pal(img->palnum);
    bool remap_palette = BuildClipboardPaletteMap(target_pal, pal_map);

    /* Adobe-like clipping: allow pasting partially off-canvas */
    int start_x = (px < 0) ? -px : 0;
    int start_y = (py < 0) ? -py : 0;
    int end_x = pw;
    int end_y = ph;

    if (px + pw > (int)img->w) end_x = img->w - px;
    if (py + ph > (int)img->h) end_y = img->h - py;

    /* Copy clipboard data to target location with clipping and transparency support. */
    for (int y = start_y; y < end_y; y++) {
        unsigned char *src = (unsigned char *)g_clipboard.data_p + y * clip_stride;
        unsigned char *dst = (unsigned char *)img->data_p + (py + y) * stride + px + start_x;
        for (int x = start_x; x < end_x; x++) {
            /* 0 remains transparent. Opaque pixels overwrite by default;
               blend/opacity modes composite in RGB and quantize back to the
               target indexed palette. */
            if (src[x] != 0)
                dst[x - start_x] = paste_composite_index(src[x], dst[x - start_x],
                                                         target_pal, pal_map,
                                                         remap_palette,
                                                         px + x, py + y);
        }
    }
    if (paste_captured) push_pixel_history_entry(&paste_snap);
    g_img_tex_idx = -2;
}

/* Nearest-neighbor resample the clipboard to exactly (nw, nh). Used both
   by paste-to-fit (downscale-only) and free-transform commit (any scale).
   Nearest-neighbor (not bilinear) is required because the clipboard stores
   palette indices, not RGB — averaging indices produces garbage colors. */
static void scale_clipboard_to(int nw, int nh)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int sw = g_clipboard.w, sh = g_clipboard.h;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw == sw && nh == sh) return;

    unsigned short src_stride = g_clipboard.stride;
    unsigned short dst_stride = (unsigned short)((nw + 3) & ~3);
    unsigned char *src = (unsigned char *)g_clipboard.data_p;
    unsigned char *dst = (unsigned char *)malloc((size_t)dst_stride * nh);
    if (!dst) return;
    memset(dst, 0, (size_t)dst_stride * nh);

    /* Inverse mapping: for each destination pixel, sample the source pixel
       nearest to the center of that destination cell. Avoids the gaps you
       get from forward mapping when the ratio isn't integral. Works for
       both upscale and downscale. */
    for (int dy = 0; dy < nh; dy++) {
        int sy_idx = (int)(((long long)dy * sh + sh / 2) / nh);
        if (sy_idx >= sh) sy_idx = sh - 1;
        unsigned char *srow = src + sy_idx * src_stride;
        unsigned char *drow = dst + dy * dst_stride;
        for (int dx = 0; dx < nw; dx++) {
            int sx_idx = (int)(((long long)dx * sw + sw / 2) / nw);
            if (sx_idx >= sw) sx_idx = sw - 1;
            drow[dx] = srow[sx_idx];
        }
    }

    free(g_clipboard.data_p);
    g_clipboard.data_p = dst;
    g_clipboard.w      = (unsigned short)nw;
    g_clipboard.h      = (unsigned short)nh;
    g_clipboard.stride = dst_stride;
}

static void transform_clipboard_to(int scaled_w, int scaled_h, float angle_deg,
                                   int *out_w, int *out_h)
{
    if (out_w) *out_w = g_clipboard.w;
    if (out_h) *out_h = g_clipboard.h;
    if (!g_clipboard.valid || !g_clipboard.data_p) return;

    int sw = g_clipboard.w;
    int sh = g_clipboard.h;
    if (scaled_w < 1) scaled_w = 1;
    if (scaled_h < 1) scaled_h = 1;

    while (angle_deg <= -180.0f) angle_deg += 360.0f;
    while (angle_deg >   180.0f) angle_deg -= 360.0f;
    const float PI_F = 3.14159265358979323846f;
    float rad = angle_deg * PI_F / 180.0f;
    float c = cosf(rad);
    float s = sinf(rad);

    int dw = scaled_w;
    int dh = scaled_h;
    if (fabsf(angle_deg) > 0.001f) {
        dw = (int)ceilf(fabsf((float)scaled_w * c) + fabsf((float)scaled_h * s));
        dh = (int)ceilf(fabsf((float)scaled_w * s) + fabsf((float)scaled_h * c));
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
    }

    unsigned short src_stride = g_clipboard.stride;
    unsigned short dst_stride = (unsigned short)((dw + 3) & ~3);
    unsigned char *src = (unsigned char *)g_clipboard.data_p;
    unsigned char *dst = (unsigned char *)malloc((size_t)dst_stride * dh);
    if (!dst) return;
    memset(dst, 0, (size_t)dst_stride * dh);

    float dst_cx = (float)dw * 0.5f;
    float dst_cy = (float)dh * 0.5f;
    float scaled_cx = (float)scaled_w * 0.5f;
    float scaled_cy = (float)scaled_h * 0.5f;

    for (int y = 0; y < dh; y++) {
        unsigned char *drow = dst + y * dst_stride;
        for (int x = 0; x < dw; x++) {
            float dx = ((float)x + 0.5f) - dst_cx;
            float dy = ((float)y + 0.5f) - dst_cy;
            float ux =  c * dx + s * dy + scaled_cx;
            float uy = -s * dx + c * dy + scaled_cy;
            if (ux < 0.0f || uy < 0.0f || ux >= (float)scaled_w || uy >= (float)scaled_h)
                continue;

            int sx_idx = (int)(ux * (float)sw / (float)scaled_w);
            int sy_idx = (int)(uy * (float)sh / (float)scaled_h);
            if (sx_idx < 0) sx_idx = 0;
            if (sy_idx < 0) sy_idx = 0;
            if (sx_idx >= sw) sx_idx = sw - 1;
            if (sy_idx >= sh) sy_idx = sh - 1;
            drow[x] = src[sy_idx * src_stride + sx_idx];
        }
    }

    free(g_clipboard.data_p);
    g_clipboard.data_p = dst;
    g_clipboard.w      = (unsigned short)dw;
    g_clipboard.h      = (unsigned short)dh;
    g_clipboard.stride = dst_stride;
    if (out_w) *out_w = dw;
    if (out_h) *out_h = dh;
}

/* Downscale-only convenience wrapper used by paste-to-fit: shrink while
   preserving aspect ratio, no-op if the clipboard already fits. */
static void scale_clipboard_to_fit(int max_w, int max_h)
{
    if (!g_clipboard.valid || !g_clipboard.data_p) return;
    int sw = g_clipboard.w, sh = g_clipboard.h;
    if (sw <= max_w && sh <= max_h) return;

    long long rx = ((long long)max_w << 16) / sw;
    long long ry = ((long long)max_h << 16) / sh;
    long long r  = (rx < ry) ? rx : ry;
    int nw = (int)((long long)sw * r >> 16);
    int nh = (int)((long long)sh * r >> 16);
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    if (nw > max_w) nw = max_w;
    if (nh > max_h) nh = max_h;
    scale_clipboard_to(nw, nh);
}

/* Marquee-select the entire current sprite. Adobe's Ctrl+A. Stored as a
   rectangle (not a mask) since "everything" is trivially representable. */
static void select_all(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || img->w == 0 || img->h == 0) return;
    g_active_tool        = ActiveTool::Marquee;
    g_grid_sel.active    = true;
    g_grid_sel.dragging  = false;
    g_grid_sel.is_mask   = false;
    g_grid_sel.pixel_mask.clear();
    g_grid_sel.x1 = 0;             g_grid_sel.y1 = 0;
    g_grid_sel.x2 = img->w - 1;    g_grid_sel.y2 = img->h - 1;
}

/* Clear any active marquee / lasso / wand selection. Adobe's Ctrl+D. Does
   NOT cancel a floating paste — that's Esc's job, and overloading Ctrl+D
   to do both would be surprising. */
static void deselect_all(void)
{
    g_grid_sel.active   = false;
    g_grid_sel.dragging = false;
    g_grid_sel.is_mask  = false;
    g_grid_sel.pixel_mask.clear();
    g_lasso_points.clear();
    g_selection_add_drag = false;
    g_selection_add_mask.clear();
    g_selection_add_mask_w = g_selection_add_mask_h = 0;
}

/* Invert the current selection. Adobe's Shift+Ctrl+I. Promotes a rect
   selection to a pixel mask so the inversion can be expressed precisely. */
static void invert_selection(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || img->w == 0 || img->h == 0) return;
    int sw = img->w, sh = img->h;

    if (!g_grid_sel.active) {
        /* Inverting "nothing" = "everything". */
        select_all();
        return;
    }

    /* Build a fresh mask covering the whole sprite, flipping bits inside the
       current selection. Promotes rect selections to masks transparently. */
    std::vector<bool> new_mask((size_t)sw * sh, false);

    int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
    int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
    if (x1 < 0) x1 = 0; if (x2 >= sw) x2 = sw - 1;
    if (y1 < 0) y1 = 0; if (y2 >= sh) y2 = sh - 1;

    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            bool inside;
            if (g_grid_sel.is_mask) {
                inside = g_grid_sel.pixel_mask[(size_t)y * g_grid_sel.mask_w + x];
            } else {
                inside = (x >= x1 && x <= x2 && y >= y1 && y <= y2);
            }
            new_mask[(size_t)y * sw + x] = !inside;
        }
    }

    g_grid_sel.active     = true;
    g_grid_sel.is_mask    = true;
    g_grid_sel.mask_w     = sw;
    g_grid_sel.mask_h     = sh;
    g_grid_sel.pixel_mask = std::move(new_mask);
    g_grid_sel.x1 = 0; g_grid_sel.y1 = 0;
    g_grid_sel.x2 = sw - 1; g_grid_sel.y2 = sh - 1;
}

static bool selection_bbox_from_mask(const std::vector<bool> &mask, int sw, int sh,
                                     int *x1, int *y1, int *x2, int *y2)
{
    int min_x = sw, min_y = sh, max_x = -1, max_y = -1;
    for (int y = 0; y < sh; y++) {
        for (int x = 0; x < sw; x++) {
            if (!mask[(size_t)y * sw + x]) continue;
            if (x < min_x) min_x = x;
            if (x > max_x) max_x = x;
            if (y < min_y) min_y = y;
            if (y > max_y) max_y = y;
        }
    }
    if (max_x < min_x || max_y < min_y) return false;
    if (x1) *x1 = min_x; if (y1) *y1 = min_y;
    if (x2) *x2 = max_x; if (y2) *y2 = max_y;
    return true;
}

static bool selection_current_to_mask(int sw, int sh, std::vector<bool> *out)
{
    if (!out) return false;
    out->assign((size_t)sw * sh, false);
    if (!g_grid_sel.active) return false;

    if (g_grid_sel.is_mask &&
        g_grid_sel.mask_w == sw && g_grid_sel.mask_h == sh &&
        g_grid_sel.pixel_mask.size() == (size_t)sw * sh) {
        *out = g_grid_sel.pixel_mask;
    } else {
        int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
        int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
        if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
        if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
        if (x1 < 0) x1 = 0; if (x2 >= sw) x2 = sw - 1;
        if (y1 < 0) y1 = 0; if (y2 >= sh) y2 = sh - 1;
        if (x1 > x2 || y1 > y2) return false;
        for (int y = y1; y <= y2; y++)
            for (int x = x1; x <= x2; x++)
                (*out)[(size_t)y * sw + x] = true;
    }

    int bx1, by1, bx2, by2;
    return selection_bbox_from_mask(*out, sw, sh, &bx1, &by1, &bx2, &by2);
}

static void selection_commit_mask(int sw, int sh, const std::vector<bool> &mask)
{
    int x1, y1, x2, y2;
    if (!selection_bbox_from_mask(mask, sw, sh, &x1, &y1, &x2, &y2)) {
        deselect_all();
        return;
    }
    g_grid_sel.active = true;
    g_grid_sel.dragging = false;
    g_grid_sel.is_mask = true;
    g_grid_sel.mask_w = sw;
    g_grid_sel.mask_h = sh;
    g_grid_sel.pixel_mask = mask;
    g_grid_sel.x1 = x1; g_grid_sel.y1 = y1;
    g_grid_sel.x2 = x2; g_grid_sel.y2 = y2;
}

static void selection_apply_mask(int sw, int sh, std::vector<bool> mask, bool add)
{
    if (add) {
        std::vector<bool> base;
        if (selection_current_to_mask(sw, sh, &base)) {
            for (size_t i = 0; i < mask.size() && i < base.size(); i++)
                mask[i] = mask[i] || base[i];
        }
    }
    selection_commit_mask(sw, sh, mask);
}

static void selection_begin_add_drag(int sw, int sh, bool add)
{
    g_selection_add_drag = false;
    g_selection_add_mask.clear();
    g_selection_add_mask_w = g_selection_add_mask_h = 0;
    if (!add) return;
    if (selection_current_to_mask(sw, sh, &g_selection_add_mask)) {
        g_selection_add_drag = true;
        g_selection_add_mask_w = sw;
        g_selection_add_mask_h = sh;
    }
}

static void selection_finish_add_drag(int sw, int sh)
{
    if (!g_selection_add_drag ||
        g_selection_add_mask_w != sw || g_selection_add_mask_h != sh ||
        g_selection_add_mask.size() != (size_t)sw * sh) {
        g_selection_add_drag = false;
        g_selection_add_mask.clear();
        return;
    }

    std::vector<bool> current;
    if (selection_current_to_mask(sw, sh, &current)) {
        for (size_t i = 0; i < current.size(); i++)
            current[i] = current[i] || g_selection_add_mask[i];
        selection_commit_mask(sw, sh, current);
    }
    g_selection_add_drag = false;
    g_selection_add_mask.clear();
}

static void paste_image(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    undo_push();

    /* If the clipboard is larger than the target sprite, nearest-neighbor
       downscale it to fit (preserving aspect ratio). Without this, the user
       has to manually clip away anything that hangs off the edge. */
    int orig_w = g_clipboard.w, orig_h = g_clipboard.h;
    if (g_clipboard.w > img->w || g_clipboard.h > img->h) {
        scale_clipboard_to_fit(img->w, img->h);
        if (g_clipboard.w != orig_w || g_clipboard.h != orig_h) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Paste scaled to fit: %dx%d -> %dx%d",
                     orig_w, orig_h,
                     (int)g_clipboard.w, (int)g_clipboard.h);
            g_restore_msg_timer = 3.0f;
        }
    }

    /* Show paste boundary centered on the target so the floating sprite is
       immediately visible and ready to resize/move. */
    g_pasted.active = true;
    g_pasted.paste_x = ((int)img->w - (int)g_clipboard.w) / 2;
    g_pasted.paste_y = ((int)img->h - (int)g_clipboard.h) / 2;
    if (g_pasted.paste_x < 0) g_pasted.paste_x = 0;
    if (g_pasted.paste_y < 0) g_pasted.paste_y = 0;
    g_pasted.dragging = false;

    /* Clear grid selection since paste is now active */
    g_grid_sel.active = false;

    /* Auto-enter Free Transform so the user can immediately resize without
       having to press Ctrl+T as a separate step. Enter / Ctrl+T / click
       outside the rect all still commit the transform and then commit the
       paste; Esc reverts the transform first, then a second Esc cancels
       the paste entirely. */
    xform_begin();
}

/* Begin Free Transform on the active floating paste. Captures the rect
   geometry at this moment so Esc can revert. The aspect lock persists
   across invocations (g_xform.aspect_locked is not reset here). */
static void xform_begin(void)
{
    if (!g_pasted.active || !g_clipboard.valid) return;
    if (g_xform.active) return; /* already transforming */
    g_xform.active   = true;
    g_xform.rx       = g_pasted.paste_x;
    g_xform.ry       = g_pasted.paste_y;
    g_xform.rw       = g_clipboard.w;
    g_xform.rh       = g_clipboard.h;
    g_xform.start_x  = g_xform.rx;
    g_xform.start_y  = g_xform.ry;
    g_xform.start_w  = g_xform.rw;
    g_xform.start_h  = g_xform.rh;
    g_xform.angle_deg = 0.0f;
    g_xform.start_angle_deg = 0.0f;
    g_xform.handle   = TransformHandle::None;
    g_xform.ref_aspect = (g_xform.rh > 0) ? (float)g_xform.rw / (float)g_xform.rh : 1.0f;
}

/* Cancel transform — revert rect to its pre-transform geometry; the paste
   stays floating at its original size. */
static void xform_cancel(void)
{
    if (!g_xform.active) return;
    g_pasted.paste_x = g_xform.start_x;
    g_pasted.paste_y = g_xform.start_y;
    g_xform.angle_deg = g_xform.start_angle_deg;
    g_xform.active   = false;
    g_xform.handle   = TransformHandle::None;
}

/* Commit transform — if the rect dimensions changed, nearest-neighbor
   resample the clipboard to match, then update the paste position to the
   final top-left. After this the floating paste continues normally and the
   user can still move it before final drop. */
static void xform_commit(void)
{
    if (!g_xform.active) return;
    int nw = g_xform.rw, nh = g_xform.rh;
    if (nw < 1) nw = 1;
    if (nh < 1) nh = 1;
    int out_w = nw;
    int out_h = nh;
    if (nw != (int)g_clipboard.w || nh != (int)g_clipboard.h ||
        fabsf(g_xform.angle_deg) > 0.001f) {
        transform_clipboard_to(nw, nh, g_xform.angle_deg, &out_w, &out_h);
    }
    float cx = (float)g_xform.rx + (float)nw * 0.5f;
    float cy = (float)g_xform.ry + (float)nh * 0.5f;
    g_pasted.paste_x = (int)floorf(cx - (float)out_w * 0.5f + 0.5f);
    g_pasted.paste_y = (int)floorf(cy - (float)out_h * 0.5f + 0.5f);
    g_xform.active   = false;
    g_xform.handle   = TransformHandle::None;
}

/* ---- Full-sprite resize ---- */
enum class SpriteResizeMode { IndexNearest = 0, MaxQuality = 1, QualitySmallBytes = 2 };
static bool g_show_resize_sprite = false;
static int  g_resize_source_idx = -1;
static int  g_resize_source_w = 0;
static int  g_resize_source_h = 0;
static int  g_resize_w = 32;
static int  g_resize_h = 32;
static int  g_resize_scale_x = 100;
static int  g_resize_scale_y = 100;
static bool g_resize_lock_aspect = true;
static int  g_resize_mode = (int)SpriteResizeMode::IndexNearest;
static bool g_resize_trim_bounds = false;
static bool g_show_bulk_resize = false;
static int  g_bulk_resize_scale_x = 100;
static int  g_bulk_resize_scale_y = 100;
static bool g_bulk_resize_lock_aspect = true;
static int  g_bulk_resize_mode = (int)SpriteResizeMode::IndexNearest;
static bool g_bulk_resize_trim_bounds = false;

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int round_to_int(double v)
{
    return (int)(v >= 0.0 ? v + 0.5 : v - 0.5);
}

/* signed_to_img_word now lives in img_util.{h,cpp}. */

/* clear_secondary_anipoint / activate_secondary_anipoint and
   secondary_anipoint_words_in_use now live in anipoint.{h,cpp}. */

static bool clipboard_secondary_anipoint_in_use(void)
{
    return secondary_anipoint_words_in_use(g_clipboard.anix2,
                                           g_clipboard.aniy2,
                                           g_clipboard.aniz2);
}

static void default_anipoints_to_center(IMG *img)
{
    if (!img) return;
    img->anix = signed_to_img_word((int)img->w / 2);
    img->aniy = signed_to_img_word((int)img->h / 2);
    clear_secondary_anipoint(img);
}

static int scaled_coord(unsigned short coord, int old_dim, int new_dim)
{
    if (old_dim <= 0) return (int)(short)coord;
    return round_to_int((double)(short)coord * (double)new_dim / (double)old_dim);
}

static void resize_sync_scale_from_dims(void)
{
    if (g_resize_source_w > 0)
        g_resize_scale_x = clamp_int(round_to_int((double)g_resize_w * 100.0 / (double)g_resize_source_w), 1, 3200);
    if (g_resize_source_h > 0)
        g_resize_scale_y = clamp_int(round_to_int((double)g_resize_h * 100.0 / (double)g_resize_source_h), 1, 3200);
}

static void resize_sync_dims_from_scale(void)
{
    if (g_resize_source_w > 0)
        g_resize_w = clamp_int(round_to_int((double)g_resize_source_w * (double)g_resize_scale_x / 100.0), 1, 4096);
    if (g_resize_source_h > 0)
        g_resize_h = clamp_int(round_to_int((double)g_resize_source_h * (double)g_resize_scale_y / 100.0), 1, 4096);
}

static void OpenResizeSpriteDialog(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;
    g_resize_source_idx = g_doc->ilselected;
    g_resize_source_w = img->w;
    g_resize_source_h = img->h;
    g_resize_w = img->w;
    g_resize_h = img->h;
    g_resize_scale_x = 100;
    g_resize_scale_y = 100;
    g_show_resize_sprite = true;
}

static bool trim_image_to_content(IMG *img, bool shrink_empty,
                                  int *out_trim_x, int *out_trim_y,
                                  bool adjust_hitbox = true)
{
    if (out_trim_x) *out_trim_x = 0;
    if (out_trim_y) *out_trim_y = 0;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;
    unsigned char *src = (unsigned char *)img->data_p;
    int min_x = w, min_y = h, max_x = -1, max_y = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (src[y * stride + x] != 0) {
                if (x < min_x) min_x = x;
                if (x > max_x) max_x = x;
                if (y < min_y) min_y = y;
                if (y > max_y) max_y = y;
            }
        }
    }

    if (max_x < 0) {
        if (!shrink_empty || (w == 1 && h == 1)) return false;
        unsigned char *dst = (unsigned char *)PoolAlloc(4);
        if (!dst) return false;
        free(img->data_p);
        img->data_p = dst;
        img->w = 1;
        img->h = 1;
        img->anix = 0;
        img->aniy = 0;
        clear_secondary_anipoint(img);
        return true;
    }

    if (min_x == 0 && min_y == 0 && max_x == w - 1 && max_y == h - 1) return false;

    int new_w = max_x - min_x + 1;
    int new_h = max_y - min_y + 1;
    int new_stride = (new_w + 3) & ~3;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)new_stride * new_h);
    if (!dst) return false;
    for (int y = 0; y < new_h; y++)
        memcpy(dst + y * new_stride, src + (y + min_y) * stride + min_x, new_w);

    free(img->data_p);
    img->data_p = dst;
    img->w = (unsigned short)new_w;
    img->h = (unsigned short)new_h;
    img->anix = signed_to_img_word((int)(short)img->anix - min_x);
    img->aniy = signed_to_img_word((int)(short)img->aniy - min_y);
    if (secondary_anipoint_in_use(img)) {
        img->anix2 = signed_to_img_word((int)(short)img->anix2 - min_x);
        img->aniy2 = signed_to_img_word((int)(short)img->aniy2 - min_y);
    }
    if (adjust_hitbox) {
        g_hitbox_x -= min_x;
        g_hitbox_y -= min_y;
    }
    if (out_trim_x) *out_trim_x = min_x;
    if (out_trim_y) *out_trim_y = min_y;
    return true;
}

static bool push_pixel_history_entry(PixelHist *snap)
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

/* ---- Lossless full-sprite transform ---- */
enum class SpriteTransformOp {
    FlipHorizontal = 0,
    FlipVertical,
    Rotate90CW,
    Rotate90CCW,
    Rotate180
};

static const char *sprite_transform_name(SpriteTransformOp op)
{
    switch (op) {
    case SpriteTransformOp::FlipHorizontal: return "Flipped horizontal";
    case SpriteTransformOp::FlipVertical:   return "Flipped vertical";
    case SpriteTransformOp::Rotate90CW:     return "Rotated 90 CW";
    case SpriteTransformOp::Rotate90CCW:    return "Rotated 90 CCW";
    case SpriteTransformOp::Rotate180:      return "Rotated 180";
    }
    return "Transformed";
}

static bool sprite_transform_preserves_anipoints(SpriteTransformOp op)
{
    return op == SpriteTransformOp::Rotate90CW ||
           op == SpriteTransformOp::Rotate90CCW;
}

static void transform_hitbox(SpriteTransformOp op, int old_w, int old_h)
{
    if (g_hitbox_w <= 0 || g_hitbox_h <= 0) return;

    int x = g_hitbox_x;
    int y = g_hitbox_y;
    int w = g_hitbox_w;
    int h = g_hitbox_h;

    switch (op) {
    case SpriteTransformOp::FlipHorizontal:
        g_hitbox_x = old_w - (x + w);
        break;
    case SpriteTransformOp::FlipVertical:
        g_hitbox_y = old_h - (y + h);
        break;
    case SpriteTransformOp::Rotate90CW:
        g_hitbox_x = old_h - (y + h);
        g_hitbox_y = x;
        g_hitbox_w = h;
        g_hitbox_h = w;
        break;
    case SpriteTransformOp::Rotate90CCW:
        g_hitbox_x = y;
        g_hitbox_y = old_w - (x + w);
        g_hitbox_w = h;
        g_hitbox_h = w;
        break;
    case SpriteTransformOp::Rotate180:
        g_hitbox_x = old_w - (x + w);
        g_hitbox_y = old_h - (y + h);
        break;
    }
}

static void transform_anipoint(SpriteTransformOp op, int old_w, int old_h,
                               unsigned short *x, unsigned short *y)
{
    int sx = (int)(short)*x;
    int sy = (int)(short)*y;
    int dx = sx;
    int dy = sy;

    switch (op) {
    case SpriteTransformOp::FlipHorizontal:
        dx = old_w - sx;
        dy = sy;
        break;
    case SpriteTransformOp::FlipVertical:
        dx = sx;
        dy = old_h - sy;
        break;
    case SpriteTransformOp::Rotate90CW:
        dx = old_h - sy;
        dy = sx;
        break;
    case SpriteTransformOp::Rotate90CCW:
        dx = sy;
        dy = old_w - sx;
        break;
    case SpriteTransformOp::Rotate180:
        dx = old_w - sx;
        dy = old_h - sy;
        break;
    }

    *x = signed_to_img_word(dx);
    *y = signed_to_img_word(dy);
}

/* secondary_anipoint_in_use now lives in anipoint.{h,cpp}. */

static int rounded_half_delta(int current_dim, int reference_dim)
{
    return round_to_int(((double)current_dim - (double)reference_dim) * 0.5);
}

static bool timeline_image_locked(int img_idx)
{
    for (int i = 0; i < 2; i++) {
        if (g_timeline_composite_locked[i] && g_timeline_composite[i] == img_idx)
            return true;
    }
    return false;
}

static int locked_timeline_anchor_position(void)
{
    if (!TimelineAnyCompositeLocked()) return -1;

    for (int slot = 0; slot < 2; slot++) {
        if (g_timeline_composite_locked[slot] &&
            g_timeline_composite[slot] == g_doc->ilselected) {
            return TimelineFramePosition(g_timeline_composite[slot]);
        }
    }
    for (int slot = 0; slot < 2; slot++) {
        if (g_timeline_composite_locked[slot])
            return TimelineFramePosition(g_timeline_composite[slot]);
    }
    return -1;
}

static int AutoCalculateTimelineAnipointsFromLock(void)
{
    int n = (int)g_timeline_frames.size();
    int anchor_pos = locked_timeline_anchor_position();
    if (n < 2 || anchor_pos < 0 || anchor_pos >= n) return 0;

    struct FrameAnipointState {
        int img_idx;
        int w, h;
        int anix, aniy;
        bool valid;
        bool locked;
    };

    std::vector<FrameAnipointState> states;
    states.reserve(g_timeline_frames.size());
    for (int img_idx : g_timeline_frames) {
        IMG *img = get_img(img_idx);
        FrameAnipointState st = {};
        st.img_idx = img_idx;
        st.valid = img && img->w > 0 && img->h > 0;
        st.locked = timeline_image_locked(img_idx);
        if (st.valid) {
            st.w = img->w;
            st.h = img->h;
            st.anix = (int)(short)img->anix;
            st.aniy = (int)(short)img->aniy;
        }
        states.push_back(st);
    }
    if (!states[anchor_pos].valid) return 0;

    for (int i = anchor_pos + 1; i < n; i++) {
        if (!states[i].valid || !states[i - 1].valid || states[i].locked) continue;
        states[i].anix = states[i - 1].anix + rounded_half_delta(states[i].w, states[i - 1].w);
        states[i].aniy = states[i - 1].aniy + rounded_half_delta(states[i].h, states[i - 1].h);
    }

    for (int i = anchor_pos - 1; i >= 0; i--) {
        if (!states[i].valid || !states[i + 1].valid || states[i].locked) continue;
        states[i].anix = states[i + 1].anix + rounded_half_delta(states[i].w, states[i + 1].w);
        states[i].aniy = states[i + 1].aniy + rounded_half_delta(states[i].h, states[i + 1].h);
    }

    int changed = 0;
    for (const FrameAnipointState &st : states) {
        if (!st.valid || st.locked) continue;
        IMG *img = get_img(st.img_idx);
        if (!img) continue;
        if ((short)img->anix != st.anix || (short)img->aniy != st.aniy)
            changed++;
    }
    if (changed == 0) return 0;
    if (!doc_undo_push()) return 0;

    for (const FrameAnipointState &st : states) {
        if (!st.valid || st.locked) continue;
        IMG *img = get_img(st.img_idx);
        if (!img) continue;
        img->anix = signed_to_img_word(st.anix);
        img->aniy = signed_to_img_word(st.aniy);
    }
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    return changed;
}

static bool TransformSelectedSprite(SpriteTransformOp op)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;

    int old_w = img->w;
    int old_h = img->h;
    int old_stride = (old_w + 3) & ~3;
    int new_w = old_w;
    int new_h = old_h;
    if (op == SpriteTransformOp::Rotate90CW || op == SpriteTransformOp::Rotate90CCW) {
        new_w = old_h;
        new_h = old_w;
    }

    PixelHist snap = {};
    if (!pixel_hist_capture(&snap, true)) return false;

    unsigned int new_stride = ((unsigned int)new_w + 3) & ~3u;
    unsigned char *dst = (unsigned char *)PoolAlloc((size_t)new_stride * new_h);
    if (!dst) {
        pixel_hist_free(&snap);
        return false;
    }
    memset(dst, 0, (size_t)new_stride * new_h);

    const unsigned char *src = (const unsigned char *)img->data_p;
    for (int sy = 0; sy < old_h; sy++) {
        for (int sx = 0; sx < old_w; sx++) {
            int dx = sx, dy = sy;
            switch (op) {
            case SpriteTransformOp::FlipHorizontal:
                dx = old_w - 1 - sx;
                dy = sy;
                break;
            case SpriteTransformOp::FlipVertical:
                dx = sx;
                dy = old_h - 1 - sy;
                break;
            case SpriteTransformOp::Rotate90CW:
                dx = old_h - 1 - sy;
                dy = sx;
                break;
            case SpriteTransformOp::Rotate90CCW:
                dx = sy;
                dy = old_w - 1 - sx;
                break;
            case SpriteTransformOp::Rotate180:
                dx = old_w - 1 - sx;
                dy = old_h - 1 - sy;
                break;
            }
            dst[dy * new_stride + dx] = src[sy * old_stride + sx];
        }
    }

    free(img->data_p);
    img->data_p = dst;
    img->w = (unsigned short)new_w;
    img->h = (unsigned short)new_h;
    if (!sprite_transform_preserves_anipoints(op)) {
        transform_anipoint(op, old_w, old_h, &img->anix, &img->aniy);
        if (secondary_anipoint_in_use(img))
            transform_anipoint(op, old_w, old_h, &img->anix2, &img->aniy2);
    }
    transform_hitbox(op, old_w, old_h);

    push_pixel_history_entry(&snap);
    mark_dirty();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    deselect_all();
    InvalidateThumb(g_doc->ilselected);

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             sprite_transform_preserves_anipoints(op)
                 ? "%s: %s (%dx%d -> %dx%d), anipoints preserved."
                 : "%s: %s (%dx%d -> %dx%d).",
             sprite_transform_name(op), img->n_s, old_w, old_h, new_w, new_h);
    g_restore_msg_timer = 4.0f;
    return true;
}

static void DrawSpriteTransformMenuItems(void)
{
    if (ImGui::MenuItem("Rotate 90 Clockwise"))
        TransformSelectedSprite(SpriteTransformOp::Rotate90CW);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Rotates pixels and hitbox; preserves existing anim points.");
    if (ImGui::MenuItem("Rotate 90 Counterclockwise"))
        TransformSelectedSprite(SpriteTransformOp::Rotate90CCW);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Rotates pixels and hitbox; preserves existing anim points.");
    if (ImGui::MenuItem("Rotate 180"))
        TransformSelectedSprite(SpriteTransformOp::Rotate180);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Rotates pixels, anipoints, and hitbox together.");
    ImGui::Separator();
    if (ImGui::MenuItem("Flip Horizontal / Mirror"))
        TransformSelectedSprite(SpriteTransformOp::FlipHorizontal);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Mirrors pixels, anipoints, and hitbox together.");
    if (ImGui::MenuItem("Flip Vertical"))
        TransformSelectedSprite(SpriteTransformOp::FlipVertical);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Flips pixels, anipoints, and hitbox together.");
}

/* Canvas rotate-button helpers now live in ui_canvas.{h,cpp}. */

static bool ResizeSelectedSprite(int nw, int nh, SpriteResizeMode mode, bool trim_bounds)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;
    nw = clamp_int(nw, 1, 4096);
    nh = clamp_int(nh, 1, 4096);

    int old_w = img->w;
    int old_h = img->h;
    bool optimize_bytes = (mode == SpriteResizeMode::QualitySmallBytes);
    bool use_quality = (mode != SpriteResizeMode::IndexNearest);

    if (nw == old_w && nh == old_h && !(trim_bounds || optimize_bytes)) return false;

    PixelHist snap = {};
    if (!pixel_hist_capture(&snap, true)) return false;

    PAL *pal = get_pal(img->palnum);
    unsigned int new_stride = 0;
    unsigned char *new_pixels = NULL;
    if (use_quality) {
        ResizeRgb fallback_rgb[256];
        for (int i = 0; i < 256; i++) {
            fallback_rgb[i].r = g_palette[i].r;
            fallback_rgb[i].g = g_palette[i].g;
            fallback_rgb[i].b = g_palette[i].b;
        }
        new_pixels = ResizeSpritePixelsQuality(img, pal, fallback_rgb, nw, nh,
                                               optimize_bytes, &new_stride);
    } else {
        new_pixels = ResizeSpritePixelsNearest(img, nw, nh, &new_stride);
    }
    if (!new_pixels) {
        pixel_hist_free(&snap);
        return false;
    }

    free(img->data_p);
    img->data_p = new_pixels;
    img->w = (unsigned short)nw;
    img->h = (unsigned short)nh;
    img->anix = signed_to_img_word(scaled_coord(snap.anix, old_w, nw));
    img->aniy = signed_to_img_word(scaled_coord(snap.aniy, old_h, nh));
    if (secondary_anipoint_words_in_use(snap.anix2, snap.aniy2, snap.aniz2)) {
        img->anix2 = signed_to_img_word(scaled_coord(snap.anix2, old_w, nw));
        img->aniy2 = signed_to_img_word(scaled_coord(snap.aniy2, old_h, nh));
        img->aniz2 = snap.aniz2;
    } else {
        clear_secondary_anipoint(img);
    }
    if (g_hitbox_w > 0 && g_hitbox_h > 0) {
        g_hitbox_x = scaled_coord((unsigned short)(short)g_hitbox_x, old_w, nw);
        g_hitbox_y = scaled_coord((unsigned short)(short)g_hitbox_y, old_h, nh);
        g_hitbox_w = clamp_int(round_to_int((double)g_hitbox_w * (double)nw / (double)old_w), 1, 4096);
        g_hitbox_h = clamp_int(round_to_int((double)g_hitbox_h * (double)nh / (double)old_h), 1, 4096);
    }

    int trim_x = 0, trim_y = 0;
    bool did_trim = false;
    if (trim_bounds || optimize_bytes)
        did_trim = trim_image_to_content(img, optimize_bytes, &trim_x, &trim_y);

    push_pixel_history_entry(&snap);
    mark_dirty();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_pasted.active = false;
    g_pasted.dragging = false;
    g_xform.active = false;
    deselect_all();
    InvalidateThumb(g_doc->ilselected);

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             did_trim ? "Resized %s: %dx%d -> %dx%d (trimmed %d,%d)."
                      : "Resized %s: %dx%d -> %dx%d.",
             img->n_s, old_w, old_h, (int)img->w, (int)img->h, trim_x, trim_y);
    g_restore_msg_timer = 4.0f;
    (void)new_stride;
    return true;
}

static void OpenBulkResizeDialog(void)
{
    g_bulk_resize_scale_x = 100;
    g_bulk_resize_scale_y = 100;
    g_bulk_resize_lock_aspect = true;
    g_bulk_resize_mode = (int)SpriteResizeMode::IndexNearest;
    g_bulk_resize_trim_bounds = false;
    g_show_bulk_resize = true;
}

static void BuildResizeFallbackRgb(ResizeRgb fallback_rgb[256])
{
    for (int i = 0; i < 256; i++) {
        fallback_rgb[i].r = g_palette[i].r;
        fallback_rgb[i].g = g_palette[i].g;
        fallback_rgb[i].b = g_palette[i].b;
    }
}

static int BulkResizeMarkedSprites(int scale_x, int scale_y,
                                   SpriteResizeMode mode, bool trim_bounds)
{
    scale_x = clamp_int(scale_x, 1, 3200);
    scale_y = clamp_int(scale_y, 1, 3200);

    struct BulkResizeTarget {
        IMG *img;
        int idx;
        int nw;
        int nh;
    };
    std::vector<BulkResizeTarget> targets;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!(img->flags & 1) || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        int nw = clamp_int(round_to_int((double)img->w * (double)scale_x / 100.0), 1, 4096);
        int nh = clamp_int(round_to_int((double)img->h * (double)scale_y / 100.0), 1, 4096);
        bool force_trim = (mode == SpriteResizeMode::QualitySmallBytes);
        if (nw == (int)img->w && nh == (int)img->h && !(trim_bounds || force_trim))
            continue;
        targets.push_back({img, idx, nw, nh});
    }
    if (targets.empty()) return 0;
    if (!doc_undo_push()) return 0;

    bool optimize_bytes = (mode == SpriteResizeMode::QualitySmallBytes);
    bool use_quality = (mode != SpriteResizeMode::IndexNearest);
    ResizeRgb fallback_rgb[256];
    BuildResizeFallbackRgb(fallback_rgb);

    int changed = 0;
    for (const BulkResizeTarget &target : targets) {
        IMG *img = target.img;
        int old_w = img->w;
        int old_h = img->h;
        unsigned short old_anix = img->anix;
        unsigned short old_aniy = img->aniy;
        unsigned short old_anix2 = img->anix2;
        unsigned short old_aniy2 = img->aniy2;
        unsigned short old_aniz2 = img->aniz2;

        PAL *pal = get_pal(img->palnum);
        unsigned int new_stride = 0;
        unsigned char *new_pixels = use_quality
            ? ResizeSpritePixelsQuality(img, pal, fallback_rgb,
                                        target.nw, target.nh,
                                        optimize_bytes, &new_stride)
            : ResizeSpritePixelsNearest(img, target.nw, target.nh,
                                        &new_stride);
        if (!new_pixels) continue;

        free(img->data_p);
        img->data_p = new_pixels;
        img->w = (unsigned short)target.nw;
        img->h = (unsigned short)target.nh;
        img->anix = signed_to_img_word(scaled_coord(old_anix, old_w, target.nw));
        img->aniy = signed_to_img_word(scaled_coord(old_aniy, old_h, target.nh));
        if (secondary_anipoint_words_in_use(old_anix2, old_aniy2, old_aniz2)) {
            img->anix2 = signed_to_img_word(scaled_coord(old_anix2, old_w, target.nw));
            img->aniy2 = signed_to_img_word(scaled_coord(old_aniy2, old_h, target.nh));
            img->aniz2 = old_aniz2;
        } else {
            clear_secondary_anipoint(img);
        }

        bool selected = (target.idx == g_doc->ilselected);
        if (selected && g_hitbox_w > 0 && g_hitbox_h > 0) {
            g_hitbox_x = scaled_coord((unsigned short)(short)g_hitbox_x, old_w, target.nw);
            g_hitbox_y = scaled_coord((unsigned short)(short)g_hitbox_y, old_h, target.nh);
            g_hitbox_w = clamp_int(round_to_int((double)g_hitbox_w * (double)target.nw / (double)old_w), 1, 4096);
            g_hitbox_h = clamp_int(round_to_int((double)g_hitbox_h * (double)target.nh / (double)old_h), 1, 4096);
        }

        if (trim_bounds || optimize_bytes) {
            int trim_x = 0, trim_y = 0;
            trim_image_to_content(img, optimize_bytes, &trim_x, &trim_y, selected);
        }

        InvalidateThumb(target.idx);
        changed++;
        (void)new_stride;
    }

    if (changed > 0) {
        mark_dirty();
        g_img_tex_idx = -2;
        g_zoom_reset = true;
        g_pasted.active = false;
        g_pasted.dragging = false;
        g_xform.active = false;
        deselect_all();
    }
    return changed;
}

static void DrawResizeSpriteDialog(void)
{
    if (g_show_resize_sprite) ImGui::OpenPopup("Resize Sprite");
    if (!ImGui::BeginPopupModal("Resize Sprite", &g_show_resize_sprite,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    IMG *img = (g_resize_source_idx >= 0) ? get_img(g_resize_source_idx) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        ImGui::TextDisabled("No sprite selected");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_resize_sprite = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    if (g_resize_source_w <= 0 || g_resize_source_h <= 0 ||
        g_resize_source_idx != g_doc->ilselected) {
        g_resize_source_idx = g_doc->ilselected;
        g_resize_source_w = img->w;
        g_resize_source_h = img->h;
        g_resize_w = img->w;
        g_resize_h = img->h;
        g_resize_scale_x = g_resize_scale_y = 100;
    }

    ImGui::Text("%s  %dx%d", img->n_s, g_resize_source_w, g_resize_source_h);
    ImGui::Separator();

    ImGui::Checkbox("Constrain Aspect Ratio", &g_resize_lock_aspect);
    ImGui::SetNextItemWidth(110);
    int w = g_resize_w;
    if (ImGui::InputInt("Width", &w, 1, 16)) {
        g_resize_w = clamp_int(w, 1, 4096);
        if (g_resize_lock_aspect && g_resize_source_w > 0)
            g_resize_h = clamp_int(round_to_int((double)g_resize_w * (double)g_resize_source_h / (double)g_resize_source_w), 1, 4096);
        resize_sync_scale_from_dims();
    }
    ImGui::SetNextItemWidth(110);
    int h = g_resize_h;
    if (ImGui::InputInt("Height", &h, 1, 16)) {
        g_resize_h = clamp_int(h, 1, 4096);
        if (g_resize_lock_aspect && g_resize_source_h > 0)
            g_resize_w = clamp_int(round_to_int((double)g_resize_h * (double)g_resize_source_w / (double)g_resize_source_h), 1, 4096);
        resize_sync_scale_from_dims();
    }

    ImGui::SetNextItemWidth(110);
    if (g_resize_lock_aspect) {
        int pct = g_resize_scale_x;
        if (ImGui::InputInt("Scale %", &pct, 1, 10)) {
            g_resize_scale_x = g_resize_scale_y = clamp_int(pct, 1, 3200);
            resize_sync_dims_from_scale();
        }
    } else {
        int sx = g_resize_scale_x;
        if (ImGui::InputInt("Scale X %", &sx, 1, 10)) {
            g_resize_scale_x = clamp_int(sx, 1, 3200);
            resize_sync_dims_from_scale();
        }
        ImGui::SetNextItemWidth(110);
        int sy = g_resize_scale_y;
        if (ImGui::InputInt("Scale Y %", &sy, 1, 10)) {
            g_resize_scale_y = clamp_int(sy, 1, 3200);
            resize_sync_dims_from_scale();
        }
    }

    const char *mode_names[] = {
        "Lossless Palette IDs",
        "Max Quality",
        "Quality + Smallest Bytes"
    };
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("Mode", &g_resize_mode, mode_names, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lossless Palette IDs keeps existing indices exact with nearest-neighbor.\n"
                          "Max Quality resamples RGB from the active palette and remaps.\n"
                          "Quality + Smallest Bytes also trims transparent bounds.");
    }

    bool force_trim = (g_resize_mode == (int)SpriteResizeMode::QualitySmallBytes);
    bool trim_box = force_trim ? true : g_resize_trim_bounds;
    if (force_trim) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Trim Transparent Bounds", &trim_box) && !force_trim)
        g_resize_trim_bounds = trim_box;
    if (force_trim) ImGui::EndDisabled();

    ImGui::Spacing();
    int old_bytes = ((g_resize_source_w + 3) & ~3) * g_resize_source_h;
    int new_bytes = ((g_resize_w + 3) & ~3) * g_resize_h;
    ImGui::TextDisabled("IMG data: %d B -> %d B", old_bytes, new_bytes);

    bool same_size = (g_resize_w == g_resize_source_w && g_resize_h == g_resize_source_h);
    bool can_apply = !same_size || g_resize_trim_bounds || force_trim;
    ImGui::BeginDisabled(!can_apply);
    if (ImGui::Button("Resize", ImVec2(100, 0))) {
        SpriteResizeMode mode = (SpriteResizeMode)clamp_int(g_resize_mode, 0, 2);
        if (ResizeSelectedSprite(g_resize_w, g_resize_h, mode, g_resize_trim_bounds)) {
            g_show_resize_sprite = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_resize_sprite = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawBulkResizeDialog(void)
{
    if (g_show_bulk_resize) ImGui::OpenPopup("Bulk Resize Marked");
    if (!ImGui::BeginPopupModal("Bulk Resize Marked", &g_show_bulk_resize,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    int marked = CountMarkedImages();
    if (marked <= 0) {
        ImGui::TextDisabled("No marked sprites");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_bulk_resize = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("%d marked sprite%s", marked, marked == 1 ? "" : "s");
    ImGui::Separator();

    ImGui::Checkbox("Constrain Aspect Ratio", &g_bulk_resize_lock_aspect);
    ImGui::SetNextItemWidth(110);
    if (g_bulk_resize_lock_aspect) {
        int pct = g_bulk_resize_scale_x;
        if (ImGui::InputInt("Scale %", &pct, 1, 10)) {
            g_bulk_resize_scale_x = g_bulk_resize_scale_y = clamp_int(pct, 1, 3200);
        }
    } else {
        int sx = g_bulk_resize_scale_x;
        if (ImGui::InputInt("Scale X %", &sx, 1, 10))
            g_bulk_resize_scale_x = clamp_int(sx, 1, 3200);
        ImGui::SetNextItemWidth(110);
        int sy = g_bulk_resize_scale_y;
        if (ImGui::InputInt("Scale Y %", &sy, 1, 10))
            g_bulk_resize_scale_y = clamp_int(sy, 1, 3200);
    }

    const char *mode_names[] = {
        "Lossless Palette IDs",
        "Max Quality",
        "Quality + Smallest Bytes"
    };
    ImGui::SetNextItemWidth(220);
    ImGui::Combo("Mode", &g_bulk_resize_mode, mode_names, 3);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Lossless Palette IDs keeps existing indices exact with nearest-neighbor.\n"
                          "Max Quality resamples RGB from each sprite palette and remaps.\n"
                          "Quality + Smallest Bytes also trims transparent bounds.");
    }

    bool force_trim = (g_bulk_resize_mode == (int)SpriteResizeMode::QualitySmallBytes);
    bool trim_box = force_trim ? true : g_bulk_resize_trim_bounds;
    if (force_trim) ImGui::BeginDisabled();
    if (ImGui::Checkbox("Trim Transparent Bounds", &trim_box) && !force_trim)
        g_bulk_resize_trim_bounds = trim_box;
    if (force_trim) ImGui::EndDisabled();

    int preview_changed = 0;
    long long old_bytes = 0;
    long long new_bytes = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (!(img->flags & 1) || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        int nw = clamp_int(round_to_int((double)img->w * (double)g_bulk_resize_scale_x / 100.0), 1, 4096);
        int nh = clamp_int(round_to_int((double)img->h * (double)g_bulk_resize_scale_y / 100.0), 1, 4096);
        old_bytes += (long long)(((int)img->w + 3) & ~3) * (long long)img->h;
        new_bytes += (long long)((nw + 3) & ~3) * (long long)nh;
        if (nw != (int)img->w || nh != (int)img->h || g_bulk_resize_trim_bounds || force_trim)
            preview_changed++;
    }
    ImGui::TextDisabled("IMG data before trim: %lld B -> %lld B", old_bytes, new_bytes);

    ImGui::Spacing();
    ImGui::BeginDisabled(preview_changed <= 0);
    if (ImGui::Button("Resize Marked", ImVec2(120, 0))) {
        SpriteResizeMode mode = (SpriteResizeMode)clamp_int(g_bulk_resize_mode, 0, 2);
        int n = BulkResizeMarkedSprites(g_bulk_resize_scale_x,
                                        g_bulk_resize_scale_y,
                                        mode,
                                        g_bulk_resize_trim_bounds);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 n > 0 ? "Bulk resized %d marked sprite%s."
                       : "No marked sprites resized.",
                 n, n == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
        if (n > 0) {
            g_show_bulk_resize = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_bulk_resize = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ---- Public C interface ---- */

void imgui_overlay_init(SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *canvas_tex)
{
#ifdef _WIN32
    SetUnhandledExceptionFilter(CrashHandlerExceptionFilter);
#else
    signal(SIGSEGV, PosixCrashHandler);
    signal(SIGILL, PosixCrashHandler);
    signal(SIGABRT, PosixCrashHandler);
    signal(SIGFPE, PosixCrashHandler);
#endif
    g_imgui_window   = window;
    g_imgui_renderer = renderer;
    g_canvas_texture = canvas_tex;
    g_dirty = false;

    RecentLoad();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding    = ImVec2(4, 4);
    style.FramePadding     = ImVec2(4, 3);
    style.ItemSpacing      = ImVec2(4, 5);
    style.ScrollbarSize    = 12.0f;
    style.WindowBorderSize = 0.0f;
    style.ChildBorderSize  = 1.0f;
    style.WindowRounding   = 0.0f;
    style.FrameRounding    = 2.0f;

    /* Load default font, then merge a Material Symbols icon font on top of it
       so toolbar glyphs render inline. The font ships in assets/ next to the
       exe; if missing we silently fall back to short text labels. */
    io.Fonts->AddFontDefault();
    {
        /* Resolve the font path relative to the running exe so the working
           directory doesn't matter. */
        char fontpath[1024] = {0};
#ifdef _WIN32
        char exepath[MAX_PATH];
        DWORD n = GetModuleFileNameA(NULL, exepath, sizeof(exepath));
        if (n > 0 && n < sizeof(exepath)) {
            char *slash = strrchr(exepath, '\\');
            if (slash) *slash = 0;
            snprintf(fontpath, sizeof(fontpath), "%s\\assets\\MaterialSymbolsSharp-Regular.ttf", exepath);
        }
#elif __APPLE__
        {
            char exepath[PATH_MAX] = {0};
            uint32_t size = sizeof(exepath);
            if (_NSGetExecutablePath(exepath, &size) == 0) {
                char *rp = realpath(exepath, NULL);
                if (rp) {
                    char *p = strrchr(rp, '/'); if (p) *p = '\0'; // imgool
                    p = strrchr(rp, '/'); if (p) *p = '\0'; // MacOS
                    snprintf(fontpath, sizeof(fontpath), "%s/Resources/assets/MaterialSymbolsSharp-Regular.ttf", rp);
                    free(rp);
                }
            }
        }
        if (fontpath[0] == '\0')
            snprintf(fontpath, sizeof(fontpath), "assets/MaterialSymbolsSharp-Regular.ttf");
#else
        snprintf(fontpath, sizeof(fontpath), "assets/MaterialSymbolsSharp-Regular.ttf");
#endif
        /* Material Symbols PUA range — covers all icon glyphs we use. */
        static const ImWchar icon_ranges[] = { 0xE000, 0xF8FF, 0 };
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = 22.0f;
        /* Material Symbols glyphs have empty descender space, so ImGui's
           text-bbox centering biases the visual mass toward the top of the
           button. Nudge down so the icon's optical center sits at button
           center. 5px lands cleanly inside the 28x28 toolbar button. */
        cfg.GlyphOffset = ImVec2(0, 5.0f);
        ImFont *icons = io.Fonts->AddFontFromFileTTF(fontpath, 20.0f, &cfg, icon_ranges);
        if (icons) g_icon_font_loaded = true;
    }

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
    SessionRestore();
}

void imgui_overlay_process_event(SDL_Event *e)
{
    ImGui_ImplSDL2_ProcessEvent(e);
}

void imgui_overlay_newframe(void)
{
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

int imgui_overlay_wants_input(void)
{
    ImGuiIO &io = ImGui::GetIO();
    return (io.WantCaptureMouse || io.WantCaptureKeyboard) ? 1 : 0;
}

int imgui_overlay_wants_keyboard(void)
{
    ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureKeyboard ? 1 : 0;
}

/* MK2 strike-table unsaved-changes confirm. Independent of the IMG
   unsaved-changes flow because it writes a completely different file
   (MKSTK.ASM, not the IMG container). */
static bool g_show_mk2_unsaved_confirm = false;
static bool g_show_mk2_fatality_unsaved_confirm = false;

int imgui_overlay_check_unsaved_and_quit(void)
{
    int dirty_idx = FindDirtyDocumentIndex();
    bool img_dirty = dirty_idx >= 0;
    bool mk2_dirty = g_mk2_doc.dirty && !g_mk2_doc.source_path.empty();
    bool mk2_fatality_dirty = g_mk2_fatality_doc.dirty && !g_mk2_fatality_doc.files.empty();
    if (img_dirty) {
        ActivateDocumentTab(dirty_idx);
        g_pending_action = PendingAction::Quit;
        g_show_unsaved_confirm = true;
    }
    if (mk2_dirty)  g_show_mk2_unsaved_confirm = true;
    if (mk2_fatality_dirty) g_show_mk2_fatality_unsaved_confirm = true;
    if (img_dirty || mk2_dirty || mk2_fatality_dirty) return 0;
    return 1;
}

void imgui_overlay_request_quit(void)
{
    g_pending_quit = true;
}

int imgui_overlay_should_quit(void)
{
    bool mk2_dirty = g_mk2_doc.dirty && !g_mk2_doc.source_path.empty();
    bool mk2_fatality_dirty = g_mk2_fatality_doc.dirty && !g_mk2_fatality_doc.files.empty();
    /* If we're pending quit and no unsaved popup is showing, it's safe to exit */
    return (g_pending_quit && !HasDirtyDocuments() && !mk2_dirty && !mk2_fatality_dirty &&
            !g_show_unsaved_confirm && !g_show_mk2_unsaved_confirm &&
            !g_show_mk2_fatality_unsaved_confirm) ? 1 : 0;
}

void imgui_overlay_mark_saved(void)
{
    g_dirty = false;
}

/* =========================================================
   Main render function — called each frame
   ========================================================= */
/* ---- Modal dialogs ---- */

static void DrawRenameDialog(void)
{
    const char *rename_title =
        g_rename_target == RenameTarget::Image          ? "Rename Image" :
        g_rename_target == RenameTarget::Palette        ? "Rename Palette" :
                                                          "Rename Marked Images";
    if (g_show_rename) ImGui::OpenPopup(rename_title);
    if (!ImGui::BeginPopupModal(rename_title, &g_show_rename, ImGuiWindowFlags_AlwaysAutoResize)) return;

    if (g_rename_target == RenameTarget::MarkedImages) {
        ImGui::TextWrapped("Base text for a numbered sequence. Tail mode keeps each current name and appends this text plus the number.");
    } else if (g_rename_target == RenameTarget::Image) {
        IMG *img = get_img(g_rename_idx);
        if (img) ImGui::Text("Rename: %s", img->n_s);
    } else {
        PAL *pal = get_pal(g_rename_idx);
        if (pal) ImGui::Text("Rename: %s", pal->n_s);
    }
    const int maxlen = g_rename_target == RenameTarget::Palette ? 10 : 16;
    ImGui::InputText("##rn", g_rename_buf,
                     (size_t)maxlen < sizeof(g_rename_buf) ? maxlen : sizeof(g_rename_buf));
    if (g_rename_target == RenameTarget::MarkedImages) {
        ImGui::Checkbox("Tail existing names", &g_rename_tail_existing);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(80.0f);
        if (ImGui::InputInt("Start", &g_rename_start_number, 1, 10)) {
            if (g_rename_start_number < 0) g_rename_start_number = 0;
        }
    }
    if (ImGui::Button("OK", ImVec2(100, 0))) {
        if (g_rename_target == RenameTarget::Image) {
            IMG *img = get_img(g_rename_idx);
            if (img) {
                doc_undo_push();   /* EditSnapshot doesn't store n_s */
                strncpy(img->n_s, g_rename_buf, 15);
                img->n_s[15] = '\0';
            }
        } else if (g_rename_target == RenameTarget::Palette) {
            PAL *pal = get_pal(g_rename_idx);
            if (pal) {
                doc_undo_push();
                strncpy(pal->n_s, g_rename_buf, 9);
                pal->n_s[9] = '\0';
            }
        } else {
            ApplyMarkedImageRename(g_rename_buf);
        }
        g_show_rename = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_rename = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawLoad2VerifyDialog(void)
{
    if (g_show_load2_verify) ImGui::OpenPopup("LOAD2 Packing Verify");
    if (!ImGui::BeginPopupModal("LOAD2 Packing Verify", &g_show_load2_verify,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Checked %d image%s against pristine baseline",
                g_load2_report.imgs_checked,
                g_load2_report.imgs_checked == 1 ? "" : "s");
    if (g_load2_report.imgs_no_baseline > 0) {
        ImGui::TextDisabled("(%d had no baseline — new/duplicated, "
                            "skipped shape check)",
                            g_load2_report.imgs_no_baseline);
    }
    ImGui::Separator();
    ImGui::Text("PPP: %d  (palette-colors limit = %d)",
                g_load2_ppp,
                g_load2_ppp > 0 ? (1 << g_load2_ppp) : 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80.0f);
    if (ImGui::InputInt("##ppp", &g_load2_ppp, 1, 0)) {
        if (g_load2_ppp < 0) g_load2_ppp = 0;
        if (g_load2_ppp > 8) g_load2_ppp = 8;
    }
    ImGui::SameLine();
    ImGui::Checkbox("/3 Limit (Max Scales)", &g_load2_limit_scales_to_3);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Warns if any image uses an eighth scale (M_COPIES=3).");
    ImGui::SameLine();
    if (ImGui::Button("Re-check")) {
        g_load2_report = VerifyLoad2Packing(g_load2_ppp, g_load2_limit_scales_to_3);
        g_load2_selected_idx = -1;
    }
    ImGui::Separator();

    if (g_load2_report.issues.empty()) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                           "OK — no SAG-breaking edits detected.");
    } else {
        ImGui::Text("Breaking: %d   Warnings: %d",
                    g_load2_report.break_count,
                    g_load2_report.warn_count);
        ImGui::Spacing();
        ImGui::BeginChild("l2_issues", ImVec2(640, 280), true);
        for (size_t i = 0; i < g_load2_report.issues.size(); i++) {
            auto &iss = g_load2_report.issues[i];
            bool is_sel = ((int)i == g_load2_selected_idx);
            ImVec4 col = iss.sev == L2Severity::Break
                ? ImVec4(1.0f, 0.45f, 0.45f, 1.0f)
                : ImVec4(1.0f, 0.85f, 0.4f,  1.0f);
            ImGui::PushID((int)i);
            char hdr[40];
            snprintf(hdr, sizeof(hdr), "[%4d] %-15s", iss.img_idx, iss.img_name.c_str());
            if (ImGui::Selectable("##row", is_sel, ImGuiSelectableFlags_AllowItemOverlap,
                                  ImVec2(0, 0))) {
                g_load2_selected_idx = (int)i;
                if (iss.img_idx >= 0) g_doc->ilselected = iss.img_idx;
                if (iss.sev == L2Severity::Break) {
                    update_drift_texture(get_img(iss.img_idx));
                }
            }
            ImGui::SameLine();
            ImGui::TextColored(col, "%s", hdr);
            ImGui::SameLine();
            ImGui::TextWrapped("%s", iss.message.c_str());
            ImGui::PopID();
            ImGui::Separator();
        }
        ImGui::EndChild();

        if (g_load2_selected_idx >= 0
            && g_load2_selected_idx < (int)g_load2_report.issues.size())
        {
            auto &sel = g_load2_report.issues[g_load2_selected_idx];
            IMG *si = get_img(sel.img_idx);
            if (sel.sev == L2Severity::Break && si && si->baseline_p) {
                if (!g_load2_drift_tex
                    || g_load2_drift_tex_w != (int)si->w
                    || g_load2_drift_tex_h != (int)si->h)
                {
                    update_drift_texture(si);
                }
                if (g_load2_drift_tex) {
                    ImGui::Spacing();
                    ImGui::TextDisabled("Red rows = zero-shape drift "
                                        "(silhouette differs from baseline)");
                    float scale = (si->w < 64) ? 4.0f : (si->w < 128) ? 3.0f : 2.0f;
                    ImVec2 sz((float)si->w * scale, (float)si->h * scale);
                    ImGui::Image((ImTextureID)(intptr_t)g_load2_drift_tex, sz);
                }
            } else if (sel.sev != L2Severity::Break) {
                ImGui::Spacing();
                ImGui::TextDisabled("(no drift visualization for warnings)");
            }
        }
    }

    ImGui::Spacing();
    if (ImGui::Button("Close")) {
        g_show_load2_verify = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* ---- ASM animation viewer implementation ---- */

static std::string asm_trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

static bool asm_is_control_token(const std::string &t)
{
    /* MK2 naming: opcodes/anim refs are lowercase (ani_jump, a_xxx); frame
       data symbols are uppercase (RNSTANCE1, RNSTANCE1A). */
    return !t.empty() && (t[0] == '_' || (t[0] >= 'a' && t[0] <= 'z'));
}

/* Parse a TI-asm integer operand: optional '-', decimal, or hex (trailing 'h',
   often with a leading 0 e.g. "0ah", "-020h"). */
static int asm_parse_int(const std::string &tok)
{
    std::string s = asm_trim(tok);
    if (s.empty()) return 0;
    bool neg = false; size_t i = 0;
    if (s[0] == '-') { neg = true; i = 1; } else if (s[0] == '+') i = 1;
    std::string num = s.substr(i);
    long v = 0;
    if (!num.empty() && (num.back() == 'h' || num.back() == 'H')) {
        v = strtol(num.c_str(), NULL, 16);
    } else {
        char *end = NULL;
        v = strtol(num.c_str(), &end, 10);
        if (end && *end) v = strtol(num.c_str(), NULL, 16); /* bare hex fallback */
    }
    return neg ? -(int)v : (int)v;
}

/* Operand count for the known MK2 animation opcodes (token after the opcode). */
static int asm_opcode_operands(const std::string &op)
{
    if (op == "ani_jump")       return 1;  /* target */
    if (op == "ani_adjustx")    return 1;  /* dx */
    if (op == "ani_adjustxy")   return 2;  /* dx, dy */
    if (op == "ani_calla")      return 1;  /* routine */
    if (op == "ani_sound")      return 1;  /* sound id */
    if (op == "ani_ochar_jump") return 2;  /* cond, target */
    if (op == "ani_flip")       return 0;
    if (op == "ani_flip_v")     return 0;
    if (op == "ani_nosleep")    return 0;
    return -1;                              /* unknown opcode */
}

static void asm_split_operands(const std::string &rest, std::vector<std::string> &out)
{
    std::string cur;
    for (char c : rest) {
        if (c == ',') { std::string t = asm_trim(cur); if (!t.empty()) out.push_back(t); cur.clear(); }
        else cur.push_back(c);
    }
    std::string t = asm_trim(cur);
    if (!t.empty()) out.push_back(t);
}

static void ClearAsmAnimTexture(void)
{
    if (g_asm_anim_tex) { SDL_DestroyTexture(g_asm_anim_tex); g_asm_anim_tex = NULL; }
    g_asm_anim_tex_w = g_asm_anim_tex_h = 0;
    g_asm_anim_last_drawn = -1;
}

/* Build a name->IMG-index map (case-insensitive) for the current document. */
static void AsmBuildNameMap(std::unordered_map<std::string,int> &m)
{
    m.clear();
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        std::string n = img_name_string(img);
        for (char &c : n) c = (char)toupper((unsigned char)c);
        if (!n.empty()) m.emplace(n, idx);
    }
}

static int AsmResolveSym(const std::unordered_map<std::string,int> &m, const std::string &sym)
{
    std::string key = sym;
    /* drop any "+offset" suffix and uppercase */
    size_t plus = key.find('+');
    if (plus != std::string::npos) key = key.substr(0, plus);
    for (char &c : key) c = (char)toupper((unsigned char)c);
    auto it = m.find(key);
    return (it != m.end()) ? it->second : -1;
}

/* Normalize a piece symbol to its IMG-frame name key (drop "+offset", uppercase). */
static std::string AsmSymKey(const std::string &sym)
{
    std::string key = sym;
    size_t plus = key.find('+');
    if (plus != std::string::npos) key = key.substr(0, plus);
    for (char &c : key) c = (char)toupper((unsigned char)c);
    return key;
}

/* Build a name -> (document, local index) map spanning EVERY open tab. A
   character's sprites are split across many IMG files (e.g. CAGE1..CAGE10.IMG),
   so a single animation only resolves fully when its pieces are looked up across
   all loaded documents. The active doc is inserted last so it wins name ties. */
static void AsmBuildGlobalNameMap(
        std::unordered_map<std::string, std::pair<Document*,int>> &m)
{
    m.clear();
    int active = document_active_index();
    int ntabs  = document_tab_count();
    /* pass 0: every non-active doc; pass 1: the active doc (overwrites ties). */
    for (int pass = 0; pass < 2; pass++) {
        for (int t = 0; t < ntabs; t++) {
            bool is_active = (t == active);
            if ((pass == 1) != is_active) continue;
            Document *d = document_get(t);
            if (!d) continue;
            int idx = 0;
            for (IMG *img = (IMG *)d->img_p; img; img = (IMG *)img->nxt_p, idx++) {
                std::string n = img_name_string(img);
                for (char &c : n) c = (char)toupper((unsigned char)c);
                if (!n.empty()) m[n] = std::make_pair(d, idx);
            }
        }
    }
}

/* Resolve every piece of an animation against all open documents, filling both
   the doc-local index (piece_img) and the owning document (piece_doc). */
static void AsmResolveAnimGlobal(AsmAnim &a)
{
    std::unordered_map<std::string, std::pair<Document*,int>> m;
    AsmBuildGlobalNameMap(m);
    a.missing = 0;
    for (auto &fr : a.frames) {
        fr.piece_img.assign(fr.piece_syms.size(), -1);
        fr.piece_doc.assign(fr.piece_syms.size(), (Document*)NULL);
        for (size_t p = 0; p < fr.piece_syms.size(); p++) {
            auto it = m.find(AsmSymKey(fr.piece_syms[p]));
            if (it != m.end()) {
                fr.piece_doc[p] = it->second.first;
                fr.piece_img[p] = it->second.second;
            } else {
                a.missing++;
            }
        }
    }
}

/* Pure parse of a character/exported ASM into a list of animations (no globals,
   no IMG load, no resolution against a specific doc beyond a best-effort first
   pass against the active doc). Shared by the player and opponent loaders. */
static bool ParseAsmAnimFile(const char *path, std::vector<AsmAnim> &out)
{
    out.clear();
    if (!path || !path[0]) return false;
    FILE *f = fopen(path, "rb");
    if (!f) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not open ASM: %s", path);
        g_restore_msg_timer = 4.0f;
        return false;
    }

    /* Pass 1: gather label bodies (.long token lists) and per-entry comments. */
    std::unordered_map<std::string, std::vector<std::string>> body;
    std::unordered_map<std::string, std::vector<int>> word_body; /* .word ints, for *_local_anipts */
    std::vector<std::string> label_order;               /* labels in file order */
    std::unordered_map<std::string, std::string> comment_for; /* token -> comment */
    std::vector<std::string> anitab_labels;             /* directory tables, in order */

    char line[1024];
    std::string cur_label;
    while (fgets(line, sizeof(line), f)) {
        std::string raw(line);
        /* split off trailing comment */
        std::string comment;
        size_t sc = raw.find(';');
        if (sc != std::string::npos) { comment = asm_trim(raw.substr(sc + 1)); raw = raw.substr(0, sc); }
        /* full-line comment markers */
        std::string lead = asm_trim(raw);
        if (lead.empty()) continue;
        if (lead[0] == '*') continue;

        bool has_label = (line[0] != ' ' && line[0] != '\t');
        std::string label, directive, rest;
        std::string work = raw;
        if (has_label) {
            size_t ws = work.find_first_of(" \t");
            label = asm_trim(work.substr(0, ws == std::string::npos ? work.size() : ws));
            work = (ws == std::string::npos) ? "" : work.substr(ws);
            if (!label.empty()) {
                cur_label = label;
                if (body.find(cur_label) == body.end()) { body[cur_label]; label_order.push_back(cur_label); }
            }
        }
        work = asm_trim(work);
        if (!work.empty()) {
            size_t ws = work.find_first_of(" \t");
            directive = asm_trim(work.substr(0, ws == std::string::npos ? work.size() : ws));
            rest = (ws == std::string::npos) ? "" : asm_trim(work.substr(ws));
        }

        if (directive == ".long" && !cur_label.empty()) {
            std::vector<std::string> ops;
            asm_split_operands(rest, ops);
            for (auto &t : ops) body[cur_label].push_back(t);
            /* capture comment for a single anim-ref entry (anitab rows) */
            if (ops.size() == 1 && !comment.empty()) comment_for[ops[0]] = comment;
        } else if (directive == ".word" && !cur_label.empty()) {
            std::vector<std::string> ops;
            asm_split_operands(rest, ops);
            for (auto &t : ops) word_body[cur_label].push_back(asm_parse_int(t));
        }
        /* directory tables are named "*anitab*" */
        if (has_label && !label.empty()) {
            std::string low = label; for (char &c : low) c = (char)tolower((unsigned char)c);
            if (low.find("anitab") != std::string::npos) anitab_labels.push_back(label);
        }
    }
    fclose(f);

    /* Pass 2: build the ordered animation list. Prefer directory order. */
    std::unordered_map<std::string,int> name_map;
    AsmBuildNameMap(name_map);

    std::vector<std::string> anim_labels;
    std::unordered_map<std::string,bool> seen;
    auto ends_with = [](const std::string &s, const char *suf) {
        size_t n = strlen(suf);
        return s.size() >= n && s.compare(s.size() - n, n, suf) == 0;
    };
    auto add_anim_label = [&](const std::string &lbl) {
        if (lbl.empty() || seen.count(lbl)) return;
        if (body.find(lbl) == body.end()) return;        /* defined here only */
        if (ends_with(lbl, "_local_anipts")) return;     /* data table, not an anim */
        seen[lbl] = true; anim_labels.push_back(lbl);
    };
    for (auto &tab : anitab_labels)
        for (auto &tok : body[tab]) add_anim_label(tok);
    for (auto &lbl : label_order)
        if (lbl.size() > 2 && lbl[0] == 'a' && lbl[1] == '_') add_anim_label(lbl);

    for (auto &lbl : anim_labels) {
        AsmAnim a;
        a.label = lbl;
        auto cit = comment_for.find(lbl);
        a.name = (cit != comment_for.end() && !cit->second.empty()) ? cit->second : lbl;
        a.missing = 0;

        const std::vector<std::string> &toks = body[lbl];
        int cur_dx = 0, cur_dy = 0; bool cur_mirror = false;
        bool stop = false;
        for (size_t ti = 0; ti < toks.size() && !stop; ti++) {
            const std::string &tok = toks[ti];
            if (tok == "0") break;             /* ani_end terminator */
            if (asm_is_control_token(tok)) {
                int nops = asm_opcode_operands(tok);
                if (tok == "ani_jump") {
                    std::string tgt = (ti + 1 < toks.size()) ? toks[ti + 1] : "";
                    a.control.push_back("loops" + (tgt.empty() ? "" : " to " + tgt));
                    stop = true;               /* loop point — frames captured */
                } else if (tok == "ani_adjustx" && ti + 1 < toks.size()) {
                    cur_dx += asm_parse_int(toks[ti + 1]); ti += 1;
                } else if (tok == "ani_adjustxy" && ti + 2 < toks.size()) {
                    cur_dx += asm_parse_int(toks[ti + 1]);
                    cur_dy += asm_parse_int(toks[ti + 2]); ti += 2;
                } else if (tok == "ani_flip") {
                    cur_mirror = !cur_mirror;
                    if (std::find(a.control.begin(), a.control.end(), "flip") == a.control.end())
                        a.control.push_back("flip");
                } else if (tok == "ani_flip_v") {
                    a.control.push_back("vflip");
                } else if (nops >= 0) {
                    a.control.push_back(tok);  /* known opcode: note + skip operands */
                    ti += (size_t)nops;
                } else {
                    a.control.push_back(tok + "?");  /* unknown: note and stop safely */
                    stop = true;
                }
                continue;
            }
            /* uppercase token = a frame-group label (or lone piece symbol) */
            AsmAnimFrame fr;
            fr.dx = cur_dx; fr.dy = cur_dy; fr.mirror = cur_mirror;
            auto bit = body.find(tok);
            if (bit != body.end()) {
                for (auto &p : bit->second) { if (p == "0") break; fr.piece_syms.push_back(p); }
            } else {
                fr.piece_syms.push_back(tok);  /* treat as a lone piece symbol */
            }
            for (auto &p : fr.piece_syms) {
                int ri = AsmResolveSym(name_map, p);
                fr.piece_img.push_back(ri);
                if (ri < 0) a.missing++;
            }
            a.frames.push_back(fr);
        }
        /* Round-trip local anipoints from a paired "<label>_local_anipts" .word
           table (emitted by imgtool's ASM export), one dx,dy pair per frame. */
        auto wit = word_body.find(lbl + "_local_anipts");
        if (wit != word_body.end()) {
            const std::vector<int> &w = wit->second;
            for (size_t fi = 0; fi < a.frames.size() && fi * 2 + 1 < w.size(); fi++) {
                a.frames[fi].dx = w[fi * 2];
                a.frames[fi].dy = w[fi * 2 + 1];
            }
        }
        if (!a.frames.empty() || !a.control.empty())
            out.push_back(std::move(a));
    }
    return !out.empty();
}

/* Build a name->IMG-index map (case-insensitive) for an arbitrary document. */
/* Resolve an animation's piece symbols across every open document. The doc
   argument is kept for call-site compatibility but no longer constrains lookup:
   a character's frames are split across many IMGs, so resolution must span them. */
static void AsmResolveAnimAgainstDoc(AsmAnim &a, Document *doc)
{
    (void)doc;
    AsmResolveAnimGlobal(a);
}

/* Open every IMG (as a tab) needed to cover this animation's piece symbols that
   no currently-open document already provides. Scans the ASM folder, sibling
   data/ dirs, every open tab's folder and $IMGDIR. Greedy set-cover, so a
   character whose sprites span several files (CAGE1..CAGE10) gets each opened.
   Returns the number of IMGs opened. */
static int AsmAutoOpenImgsForAnim(const AsmAnim &a, const char *asm_path)
{
    std::unordered_map<std::string,bool> need;
    for (auto &fr : a.frames)
        for (auto &s : fr.piece_syms) {
            std::string u = AsmSymKey(s);
            if (!u.empty()) need[u] = true;
        }
    if (need.empty()) return 0;

    /* Drop symbols any already-open document provides. */
    {
        std::unordered_map<std::string, std::pair<Document*,int>> m;
        AsmBuildGlobalNameMap(m);
        for (auto it = need.begin(); it != need.end(); ) {
            if (m.count(it->first)) it = need.erase(it);
            else ++it;
        }
    }
    if (need.empty()) return 0;

    std::string asmdir = asm_path ? asm_path : "";
    size_t sl = asmdir.find_last_of("\\/");
    asmdir = (sl != std::string::npos) ? asmdir.substr(0, sl) : ".";

    /* Collect every plausible folder an IMG could live in, deduped. ASM files
       commonly sit in a src/ tree while the IMGs live in a sibling data/ dir,
       so probe those relatives plus every open tab's folder and IMGDIR. */
    std::vector<std::string> dirs;
    auto add_dir = [&](const std::string &d) {
        if (d.empty()) return;
        std::string low = d; for (char &c : low) c = (char)tolower((unsigned char)c);
        for (auto &ex : dirs) {
            std::string el = ex; for (char &c : el) c = (char)tolower((unsigned char)c);
            if (el == low) return;
        }
        dirs.push_back(d);
    };
    add_dir(asmdir);
    add_dir(asmdir + "\\data");
    add_dir(asmdir + "\\..\\data");
    add_dir(asmdir + "\\..\\DATA");
    add_dir(asmdir + "\\..");
    add_dir(asmdir + "\\..\\..\\data");
    for (int t = 0; t < document_tab_count(); t++) {
        Document *d = document_get(t);
        if (d && d->fpath_s[0]) add_dir(d->fpath_s);
    }
    if (g_doc->fpath_s[0]) add_dir(g_doc->fpath_s);
    const char *imgdir = getenv("IMGDIR");
    if (imgdir && imgdir[0]) add_dir(imgdir);

    /* Probe every candidate IMG once, recording its uppercased frame names. */
    struct ImgCand { std::string path; std::vector<std::string> names; };
    std::vector<ImgCand> cands;
    for (auto &d : dirs) {
        std::vector<FileEntry> entries;
        GetDirectoryFiles(d, entries, "IMG");
        for (auto &e : entries) {
            if (e.is_dir) continue;
            ImgCand c;
            c.path = PathCombine(d, e.name);
            std::vector<std::string> names;
            ProbeImgFrameNames(c.path.c_str(), names);
            for (auto &nm : names) {
                std::string u = nm; for (char &ch : u) ch = (char)toupper((unsigned char)ch);
                c.names.push_back(u);
            }
            cands.push_back(std::move(c));
        }
    }

    /* Greedy set-cover: repeatedly open the IMG covering the most still-missing
       symbols until everything resolves or no remaining file helps. */
    int opened = 0, guard = 0;
    while (!need.empty() && guard++ < 64) {
        int best = -1, best_hits = 0;
        for (size_t i = 0; i < cands.size(); i++) {
            int hits = 0;
            for (auto &u : cands[i].names) if (need.count(u)) hits++;
            if (hits > best_hits) { best_hits = hits; best = (int)i; }
        }
        if (best < 0) break;
        OpenImgFile(cands[best].path);
        opened++;
        for (auto &u : cands[best].names) need.erase(u);
        cands[best].names.clear();   /* don't pick the same file again */
    }
    return opened;
}

static bool LoadAsmAnimations(const char *path)   /* player */
{
    if (!ParseAsmAnimFile(path, g_asm_anims)) {
        if (g_asm_anims.empty())
            snprintf(g_restore_msg, sizeof(g_restore_msg), "No animations found in ASM.");
        g_restore_msg_timer = 4.0f;
        return false;
    }
    g_asm_anim_file = path;
    AsmAnimSelect(g_asm_anims.empty() ? -1 : 0);
    /* Defer IMG loading to the main loop: opening tabs mid-parse is avoided, and
       the handler opens every IMG the selected anim needs, then re-resolves. */
    if (g_asm_anim_sel >= 0) g_request_asm_autoload = true;

    const char *base = (strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Loaded %d animation%s from %s; opening its sprites...",
             (int)g_asm_anims.size(), g_asm_anims.size() == 1 ? "" : "s", base);
    g_restore_msg_timer = 4.0f;
    return !g_asm_anims.empty();
}

static bool LoadAsmOpponent(const char *path)      /* fatality opponent */
{
    if (!ParseAsmAnimFile(path, g_asm_opp_anims)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No animations found in opponent ASM.");
        g_restore_msg_timer = 4.0f;
        return false;
    }
    g_asm_opp_file = path;
    g_asm_opp_doc = g_doc;
    g_asm_opp_doc_idx = document_active_index();
    g_asm_opp_sel = g_asm_opp_anims.empty() ? -1 : 0;
    if (g_asm_opp_sel >= 0)
        AsmResolveAnimGlobal(g_asm_opp_anims[g_asm_opp_sel]);
    g_asm_opp_enabled = true;
    /* Defer opening the opponent's sprite IMGs to the main loop. */
    if (g_asm_opp_sel >= 0) g_request_asm_opp_autoload = true;

    /* Default the opponent to face the player (mirror = opposite of the player
       ASM lane); only set here so the user can still flip it. */
    {
        bool *pf = WorldMarkedMirrorFlag(g_world_marked_state, kWorldAsmSlot);
        bool *of = WorldMarkedMirrorFlag(g_world_marked_state, kWorldAsmOpponentSlot);
        if (of) *of = pf ? !*pf : true;
    }

    const char *base = (strrchr(path, '\\') ? strrchr(path, '\\') + 1 : path);
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Loaded opponent: %d animation%s from %s; opening its sprites...",
             (int)g_asm_opp_anims.size(), g_asm_opp_anims.size() == 1 ? "" : "s", base);
    g_restore_msg_timer = 4.0f;
    return !g_asm_opp_anims.empty();
}

/* Re-resolve the selected anim against the current IMG and size the playback
   canvas to the anipoint-anchored union of all its frame pieces. */
static void AsmAnimSelect(int i)
{
    g_asm_anim_sel = i;
    g_asm_anim_frame = 0;
    g_asm_anim_timer = 0.0f;
    ClearAsmAnimTexture();
    if (i < 0 || i >= (int)g_asm_anims.size()) return;

    g_asm_anim_doc = g_doc;                         /* representative doc for lane fallback */
    g_asm_anim_doc_idx = document_active_index();

    AsmAnim &a = g_asm_anims[i];
    AsmResolveAnimGlobal(a);                         /* resolve across all open IMGs */
    int minx = 0x3FFFFFFF, miny = 0x3FFFFFFF, maxx = -0x3FFFFFFF, maxy = -0x3FFFFFFF;
    bool any = false;
    for (auto &fr : a.frames) {
        for (size_t p = 0; p < fr.piece_syms.size(); p++) {
            int ri = fr.piece_img[p];
            if (ri < 0) continue;
            IMG *img = doc_get_img(fr.piece_doc[p], ri);
            if (!img) continue;
            int x0 = -(int)(short)img->anix + fr.dx, y0 = -(int)(short)img->aniy + fr.dy;
            int x1 = x0 + img->w, y1 = y0 + img->h;
            if (x0 < minx) minx = x0; if (y0 < miny) miny = y0;
            if (x1 > maxx) maxx = x1; if (y1 > maxy) maxy = y1;
            any = true;
        }
    }
    if (!any) { g_asm_anim_canvas_w = g_asm_anim_canvas_h = 0; return; }
    g_asm_anim_minx = minx; g_asm_anim_miny = miny;
    int cw = maxx - minx, ch = maxy - miny;
    if (cw < 1) cw = 1; if (ch < 1) ch = 1;
    if (cw > 1024) cw = 1024; if (ch > 1024) ch = 1024;
    g_asm_anim_canvas_w = cw; g_asm_anim_canvas_h = ch;
}

/* Deferred (main-loop) handler: open every IMG the selected player anim needs as
   tabs, then re-resolve and re-size against them. If nothing resolves even after
   the scan, fall back to prompting the user to locate an IMG. */
static void AsmProcessAutoload(void)
{
    if (g_asm_anim_sel < 0 || g_asm_anim_sel >= (int)g_asm_anims.size()) return;
    AsmAutoOpenImgsForAnim(g_asm_anims[g_asm_anim_sel], g_asm_anim_file.c_str());
    AsmAnimSelect(g_asm_anim_sel);   /* re-resolve against the now-open IMGs */

    int resolved = 0;
    for (auto &fr : g_asm_anims[g_asm_anim_sel].frames)
        for (int ri : fr.piece_img) if (ri >= 0) resolved++;
    if (resolved == 0) g_request_locate_img = true;
}

/* Same as above for the fatality opponent ASM. */
static void AsmProcessOppAutoload(void)
{
    if (g_asm_opp_sel < 0 || g_asm_opp_sel >= (int)g_asm_opp_anims.size()) return;
    AsmAutoOpenImgsForAnim(g_asm_opp_anims[g_asm_opp_sel], g_asm_opp_file.c_str());
    AsmResolveAnimGlobal(g_asm_opp_anims[g_asm_opp_sel]);
    g_asm_opp_doc = g_doc;
    g_asm_opp_doc_idx = document_active_index();

    int resolved = 0;
    for (auto &fr : g_asm_opp_anims[g_asm_opp_sel].frames)
        for (int ri : fr.piece_img) if (ri >= 0) resolved++;
    if (resolved == 0) g_request_locate_opp_img = true;
}

/* (Re)fill the playback texture with the current frame's composited pieces. */
static void AsmAnimRefillTexture(void)
{
    if (g_asm_anim_sel < 0 || g_asm_anim_sel >= (int)g_asm_anims.size()) return;
    if (g_asm_anim_canvas_w <= 0 || g_asm_anim_canvas_h <= 0) return;
    AsmAnim &a = g_asm_anims[g_asm_anim_sel];
    if (a.frames.empty()) return;
    int fi = g_asm_anim_frame % (int)a.frames.size();

    int w = g_asm_anim_canvas_w, h = g_asm_anim_canvas_h;
    if (!g_asm_anim_tex || g_asm_anim_tex_w != w || g_asm_anim_tex_h != h) {
        if (g_asm_anim_tex) SDL_DestroyTexture(g_asm_anim_tex);
        g_asm_anim_tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                                           SDL_TEXTUREACCESS_STREAMING, w, h);
        if (!g_asm_anim_tex) return;
        SDL_SetTextureBlendMode(g_asm_anim_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_asm_anim_tex, SDL_ScaleModeNearest);
        g_asm_anim_tex_w = w; g_asm_anim_tex_h = h;
    }

    void *pixels; int pitch;
    if (SDL_LockTexture(g_asm_anim_tex, NULL, &pixels, &pitch) != 0) return;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            dst[y * (pitch / 4) + x] = 0x00000000u;   /* transparent */

    AsmAnimFrame &fr = a.frames[fi];
    for (size_t p = 0; p < fr.piece_img.size(); p++) {
        int ri = fr.piece_img[p];
        if (ri < 0) continue;
        Document *pdoc = (p < fr.piece_doc.size() && fr.piece_doc[p]) ? fr.piece_doc[p]
                                                                      : g_asm_anim_doc;
        IMG *img = doc_get_img(pdoc, ri);
        if (!img || !img->data_p) continue;
        PAL *pal = doc_get_pal(pdoc, img->palnum);
        const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : NULL;
        int stride = (img->w + 3) & ~3;
        const unsigned char *sp = (const unsigned char *)img->data_p;
        int ox = -(int)(short)img->anix + fr.dx - g_asm_anim_minx;
        int oy = -(int)(short)img->aniy + fr.dy - g_asm_anim_miny;
        for (int y = 0; y < img->h; y++) {
            int dy = oy + y; if (dy < 0 || dy >= h) continue;
            for (int x = 0; x < img->w; x++) {
                /* ani_flip mirrors horizontally about the piece's anipoint */
                int srcx = fr.mirror ? (img->w - 1 - x) : x;
                int dx = ox + x; if (dx < 0 || dx >= w) continue;
                unsigned char ci = sp[y * stride + srcx];
                if (ci == 0) continue;
                Uint32 r = 200, g = 200, b = 200;
                if (pd) {
                    unsigned short w15 = (unsigned short)(pd[ci*2] | (pd[ci*2+1] << 8));
                    r = ((w15 >> 10) & 0x1F) << 3; g = ((w15 >> 5) & 0x1F) << 3; b = (w15 & 0x1F) << 3;
                }
                dst[dy * (pitch / 4) + dx] = (0xFFu << 24) | (r << 16) | (g << 8) | b;
            }
        }
    }
    SDL_UnlockTexture(g_asm_anim_tex);
    g_asm_anim_last_drawn = fi;
}

static void DrawAsmAnimWindow(void)
{
    if (!g_show_asm_anim) return;
    ImGui::SetNextWindowSize(ImVec2(420, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("ASM Animations", &g_show_asm_anim)) { ImGui::End(); return; }

    if (ImGui::Button("Load Character ASM...")) OpenFileDialog(FileDialogMode::LoadAsmAnim);
    if (!g_asm_anim_file.empty()) {
        ImGui::SameLine();
        const char *base = strrchr(g_asm_anim_file.c_str(), '\\');
        ImGui::TextDisabled("%s", base ? base + 1 : g_asm_anim_file.c_str());
    }

    if (g_asm_anims.empty()) {
        ImGui::TextWrapped("Load a per-character ASM (e.g. MKRD.ASM for Raiden) to list its "
                           "animations. Selecting one automatically opens every sprite IMG "
                           "it needs (a character's frames are split across several files) "
                           "and plays it composited across them.");
        ImGui::End();
        return;
    }

    /* Animation chooser */
    const char *cur = (g_asm_anim_sel >= 0 && g_asm_anim_sel < (int)g_asm_anims.size())
                    ? g_asm_anims[g_asm_anim_sel].name.c_str() : "(none)";
    ImGui::SetNextItemWidth(-1);
    if (ImGui::BeginCombo("##asm_anim_sel", cur)) {
        for (int i = 0; i < (int)g_asm_anims.size(); i++) {
            bool sel = (i == g_asm_anim_sel);
            char lbl[96];
            snprintf(lbl, sizeof(lbl), "%s  (%s)", g_asm_anims[i].name.c_str(), g_asm_anims[i].label.c_str());
            if (ImGui::Selectable(lbl, sel)) { AsmAnimSelect(i); g_request_asm_autoload = true; }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (g_asm_anim_sel < 0) { ImGui::End(); return; }
    AsmAnim &a = g_asm_anims[g_asm_anim_sel];

    if (ImGui::Checkbox("Play in World View lane", &g_asm_lane_enabled) && g_asm_lane_enabled) {
        g_world_state.enabled = true;
        g_world_marked_state.marked_play = true;
        WorldMarkedRestart(g_world_marked_state);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Render this animation as a lane in World View, using the script's\n"
                          "ticks, local anipoints, anipoint placement and loop.");
    ImGui::Separator();

    ImGui::Checkbox("Play", &g_asm_anim_play);
    ImGui::SameLine(); ImGui::SetNextItemWidth(120);
    ImGui::SliderFloat("fps", &g_asm_anim_fps, 1.0f, 30.0f, "%.0f");
    int nframes = (int)a.frames.size();
    if (nframes > 0) {
        ImGui::SameLine(); ImGui::SetNextItemWidth(140);
        int disp = g_asm_anim_frame % nframes + 1;
        if (ImGui::SliderInt("##asm_frame", &disp, 1, nframes, "frame %d")) {
            g_asm_anim_frame = disp - 1; g_asm_anim_play = false;
        }
    }

    /* advance playback */
    if (g_asm_anim_play && nframes > 0 && g_asm_anim_fps > 0.0f) {
        g_asm_anim_timer += ImGui::GetIO().DeltaTime;
        float step = 1.0f / g_asm_anim_fps;
        while (g_asm_anim_timer >= step) { g_asm_anim_timer -= step; g_asm_anim_frame = (g_asm_anim_frame + 1) % nframes; }
    }
    if (nframes > 0 && (g_asm_anim_frame % nframes) != g_asm_anim_last_drawn)
        AsmAnimRefillTexture();

    /* preview */
    if (g_asm_anim_tex && g_asm_anim_canvas_w > 0) {
        float avail = ImGui::GetContentRegionAvail().x;
        float scale = (g_asm_anim_canvas_w > 0) ? (avail / (float)g_asm_anim_canvas_w) : 1.0f;
        if (scale > 4.0f) scale = 4.0f; if (scale < 0.25f) scale = 0.25f;
        ImVec2 sz((float)g_asm_anim_canvas_w * scale, (float)g_asm_anim_canvas_h * scale);
        ImGui::Image((ImTextureID)(intptr_t)g_asm_anim_tex, sz);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "No frames of this animation resolve to the loaded IMG.");
    }

    /* missing-data report */
    ImGui::Separator();
    ImGui::Text("Frames: %d   Pieces missing in IMG: %d", nframes, a.missing);
    if (!a.control.empty()) {
        std::string ctl;
        for (auto &c : a.control) { if (!ctl.empty()) ctl += ", "; ctl += c; }
        ImGui::TextColored(ImVec4(0.7f, 0.85f, 1.0f, 1.0f), "Control / opcodes: %s", ctl.c_str());
    }
    if (a.missing > 0 && ImGui::TreeNode("Unresolved symbols")) {
        for (auto &fr : a.frames)
            for (size_t p = 0; p < fr.piece_syms.size(); p++)
                if (p < fr.piece_img.size() && fr.piece_img[p] < 0)
                    ImGui::BulletText("%s", fr.piece_syms[p].c_str());
        ImGui::TreePop();
    }

    /* ---- Fatality opponent (second ASM, drawn in the opponent lane) ---- */
    ImGui::SeparatorText("Fatality opponent");
    if (ImGui::Button("Load Johnny Cage")) {
        /* Default opponent: MKJC.ASM in the same folder as the player ASM. */
        std::string dir = g_asm_anim_file;
        size_t sl = dir.find_last_of("\\/");
        dir = (sl != std::string::npos) ? dir.substr(0, sl) : ".";
        std::string jc = dir + "\\MKJC.ASM";
        FILE *probe = fopen(jc.c_str(), "rb");
        if (probe) { fclose(probe); LoadAsmOpponent(jc.c_str()); }
        else       { g_request_load_opp_asm = true; }   /* not found -> pick manually */
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Load MKJC.ASM (Johnny Cage) from the player ASM's folder as the opponent.");
    ImGui::SameLine();
    if (ImGui::Button("Load Opponent ASM...")) g_request_load_opp_asm = true;

    if (!g_asm_opp_anims.empty()) {
        if (!g_asm_opp_file.empty()) {
            const char *ob = strrchr(g_asm_opp_file.c_str(), '\\');
            ImGui::SameLine(); ImGui::TextDisabled("%s", ob ? ob + 1 : g_asm_opp_file.c_str());
        }
        ImGui::Checkbox("Play opponent in World View lane", &g_asm_opp_enabled);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Draw the opponent animation in its own lane, facing the player.");
        if (g_asm_opp_enabled) { g_world_state.enabled = true; g_world_marked_state.marked_play = true; }

        const char *ocur = (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
                         ? g_asm_opp_anims[g_asm_opp_sel].name.c_str() : "(none)";
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##asm_opp_sel", ocur)) {
            for (int i = 0; i < (int)g_asm_opp_anims.size(); i++) {
                bool seld = (i == g_asm_opp_sel);
                char lbl[96];
                snprintf(lbl, sizeof(lbl), "%s  (%s)", g_asm_opp_anims[i].name.c_str(),
                         g_asm_opp_anims[i].label.c_str());
                if (ImGui::Selectable(lbl, seld)) {
                    g_asm_opp_sel = i;
                    AsmResolveAnimGlobal(g_asm_opp_anims[i]);
                    g_request_asm_opp_autoload = true;
                    WorldMarkedRestart(g_world_marked_state);
                }
                if (seld) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (g_asm_opp_sel >= 0 && g_asm_opp_sel < (int)g_asm_opp_anims.size())
            ImGui::Text("Opponent frames: %d   missing: %d",
                        (int)g_asm_opp_anims[g_asm_opp_sel].frames.size(),
                        g_asm_opp_anims[g_asm_opp_sel].missing);
    }

    ImGui::End();
}

/* Controls for the selected sprite's non-destructive overlay layer. Only shown
   when the current sprite actually has a layer (created via Drop Paste to
   Layer). Move/flip/flatten/delete; the layer bakes onto the sprite on save. */
static void DrawSpriteLayerPanel(void)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    SpriteLayer *L = img_layer(img);
    if (!L) return;

    ImGui::SetNextWindowSize(ImVec2(270, 0), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sprite Layer")) {
        ImGui::Text("On %.15s   layer %dx%d", img->n_s, L->w, L->h);
        bool vis = L->visible != 0;
        if (ImGui::Checkbox("Visible", &vis)) { L->visible = vis ? 1 : 0; mark_dirty(); g_img_tex_idx = -2; }
        ImGui::SameLine();
        ImGui::TextDisabled("(bakes onto sprite on save)");

        ImGui::Separator();
        int pos[2] = { L->x, L->y };
        if (ImGui::DragInt2("Offset", pos, 0.5f)) { L->x = pos[0]; L->y = pos[1]; mark_dirty(); g_img_tex_idx = -2; }
        if (ImGui::Button("Left"))  { L->x--; mark_dirty(); g_img_tex_idx = -2; } ImGui::SameLine();
        if (ImGui::Button("Right")) { L->x++; mark_dirty(); g_img_tex_idx = -2; } ImGui::SameLine();
        if (ImGui::Button("Up"))    { L->y--; mark_dirty(); g_img_tex_idx = -2; } ImGui::SameLine();
        if (ImGui::Button("Down"))  { L->y++; mark_dirty(); g_img_tex_idx = -2; }

        ImGui::Separator();
        if (ImGui::Button("Flip H")) { doc_undo_push(); flip_layer_horizontal(L); g_img_tex_idx = -2; }
        ImGui::SameLine();
        if (ImGui::Button("Flip V")) { doc_undo_push(); flip_layer_vertical(L); g_img_tex_idx = -2; }

        ImGui::Separator();
        if (ImGui::Button("Flatten Now")) { doc_undo_push(); flatten_img_layer(img); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merge the layer into the sprite now (permanent).");
        ImGui::SameLine();
        if (ImGui::Button("Delete Layer")) { doc_undo_push(); delete_img_layer(img); }
    }
    ImGui::End();
}

static void DrawPaletteMergeQualityDialog(void)
{
    if (g_show_palette_merge_quality) ImGui::OpenPopup("Palette Merge Quality Check");
    if (!ImGui::BeginPopupModal("Palette Merge Quality Check", &g_show_palette_merge_quality,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    const PaletteMergeQuality &q = g_palette_merge_quality;

    /* Merge-mode choices. Flipping either recomputes the plan + preview live. */
    bool changed = false;
    changed |= ImGui::Checkbox("Grow target (add used source colors to free slots)",
                               &g_merge_opt_grow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Append source colors the sprites actually use into the\n"
                          "target's empty slots (lossless) instead of approximating\n"
                          "them. Falls back to nearest match when slots run out.");
    changed |= ImGui::Checkbox("Perceptual color match (luma-weighted)",
                               &g_merge_opt_perceptual);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Weight nearest-color search by how the eye perceives\n"
                          "brightness (green > red > blue) for closer-looking matches.");
    if (changed)
        BuildMarkedPaletteMergeQuality(&g_palette_merge_quality);
    ImGui::Separator();

    int drift_pixels = q.color_drift_pixels + q.transparent_drift_pixels;
    double avg_drift = q.affected_pixels > 0
        ? sqrt((double)q.total_dist / (double)q.affected_pixels)
        : 0.0;
    double max_drift = sqrt((double)q.max_dist);

    ImGui::Text("Target: %s", q.target_name);
    ImGui::Text("Marked palettes: %d   Images: %d   Nonzero pixels: %d",
                q.source_palettes, q.remapped_images, q.affected_pixels);
    if (q.opt_grow && (q.colors_added > 0 || q.colors_overflow > 0)) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.6f, 1.0f),
                           "Target grows: %d -> %d colors (+%d added)",
                           q.target_base_numc, q.target_base_numc + q.colors_added,
                           q.colors_added);
        if (q.colors_overflow > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f),
                               "%d color%s could not fit (no free slots) - approximated",
                               q.colors_overflow, q.colors_overflow == 1 ? "" : "s");
    }
    ImGui::Separator();

    if (drift_pixels == 0 && q.ppp_warning_images == 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                           "No visual drift detected.");
    } else {
        if (q.color_drift_pixels > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f),
                               "Color drift: %d pixel%s",
                               q.color_drift_pixels,
                               q.color_drift_pixels == 1 ? "" : "s");
            ImGui::Text("Average 5-bit RGB drift: %.2f   Max: %.2f",
                        avg_drift, max_drift);
            if (q.max_dist > 0) {
                ImGui::Text("Worst: %.9s / %.15s  color %d -> %d",
                            q.max_palette, q.max_image,
                            q.max_src_slot, q.max_dst_slot);
            }
        }
        if (q.transparent_drift_pixels > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "Transparency drift: %d pixel%s would become index 0",
                               q.transparent_drift_pixels,
                               q.transparent_drift_pixels == 1 ? "" : "s");
        }
        if (q.invalid_pixels > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                               "Out-of-range source indices: %d pixel%s",
                               q.invalid_pixels,
                               q.invalid_pixels == 1 ? "" : "s");
        }
        if (q.ppp_warning_images > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.35f, 1.0f));
            ImGui::TextWrapped("LOAD2 PPP risk: %d image%s would move to a palette over the current PPP limit",
                               q.ppp_warning_images,
                               q.ppp_warning_images == 1 ? "" : "s");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    DrawPaletteMergeMappingPreview(q);

    ImGui::Spacing();
    const char *merge_label = drift_pixels > 0 ? "Merge Anyway" : "Merge";
    if (ImGui::Button(merge_label, ImVec2(120, 0))) {
        g_show_palette_merge_quality = false;
        g_palette_merge_preview_only = false;
        ImGui::CloseCurrentPopup();
        MergeMarkedPalettes(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_palette_merge_quality = false;
        g_palette_merge_preview_only = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* MK2 strike-table (hitbox) editor. Reads/writes mk2-main/src/MKSTK.ASM
   directly — the source-of-truth for the strike tables. The MAME stk.bin
   path is intentionally not used here; rebuilding through the ASM is the
   permanent route. */
static void DrawMk2HitboxWindow(void)
{
    if (!g_show_mk2) return;

    ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MK2 Hitboxes (MKSTK.ASM)", &g_show_mk2)) {
        ImGui::End();
        return;
    }

    /* Auto-clear the "Loaded N moves" / "Saved" success message once the
       user has started editing again. Errors keep their sticky flag so
       they stay visible until the next action explicitly resolves them. */
    if (!g_mk2_status_sticky && g_mk2_doc.dirty && !g_mk2_status.empty())
        g_mk2_status.clear();

    /* Window-scoped shortcuts. RouteFocused makes these fire only when
       the MK2 panel (or one of its child widgets) holds focus, so they
       don't hijack the pixel-undo path on the main canvas. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, ImGuiInputFlags_RouteFocused)) {
        int rec = mk2::undo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z, ImGuiInputFlags_RouteFocused)) {
        int rec = mk2::redo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }

    /* --- Path / Browse / Load / Save row --- */
    ImGui::SetNextItemWidth(-280);
    ImGui::InputTextWithHint("##mk2path", "path to MKSTK.ASM (use Browse...)", g_mk2_path, sizeof(g_mk2_path));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
#ifdef _WIN32
        char buf[1024];
        /* Seed the filename buffer with the current path so the dialog
           opens at the last location. lpstrFile doubles as input on
           open-mode. */
        size_t cur = strlen(g_mk2_path);
        if (cur >= sizeof(buf)) cur = sizeof(buf) - 1;
        memcpy(buf, g_mk2_path, cur); buf[cur] = '\0';
        /* Initial directory from the persisted last-dir, used only when
           lpstrFile doesn't already contain a directory component. */
        char init_dir[MAX_PATH] = "";
        load_last_dir_cat(init_dir, sizeof(init_dir), "mk2");
        OPENFILENAMEA ofn = {};
        ofn.lStructSize     = sizeof(ofn);
        ofn.lpstrFilter     = "ASM source\0*.ASM;*.asm\0All files\0*.*\0";
        ofn.lpstrFile       = buf;
        ofn.nMaxFile        = sizeof(buf);
        ofn.lpstrInitialDir = init_dir[0] ? init_dir : NULL;
        ofn.lpstrTitle      = "Select MKSTK.ASM";
        ofn.Flags           = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
        if (GetOpenFileNameA(&ofn)) {
            strncpy(g_mk2_path, buf, sizeof(g_mk2_path) - 1);
            g_mk2_path[sizeof(g_mk2_path) - 1] = '\0';
            /* Persist the directory (everything up to the last separator). */
            const char *last_sep = NULL;
            for (const char *p = buf; *p; p++)
                if (*p == '\\' || *p == '/') last_sep = p;
            if (last_sep && last_sep > buf) {
                char dir[MAX_PATH];
                size_t n = (size_t)(last_sep - buf);
                if (n >= sizeof(dir)) n = sizeof(dir) - 1;
                memcpy(dir, buf, n); dir[n] = '\0';
                save_last_dir_cat(dir, "mk2");
            }
        }
#else
        g_mk2_status = "Browse not implemented on this platform - type the path manually";
        g_mk2_status_sticky = true;
#endif
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick MKSTK.ASM from disk");
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        std::string err;
        if (mk2::load(&g_mk2_doc, g_mk2_path, &err)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "Loaded %d moves, %d char tables",
                     (int)g_mk2_doc.records.size(), (int)g_mk2_doc.char_tables.size());
            g_mk2_status = buf;
            g_mk2_status_sticky = false;
            g_mk2_char_idx = 0;
            g_mk2_move_idx = 0;
            g_mk2_search[0] = '\0';
            /* Fresh load wipes any prior undo/redo history — those entries
               referenced records that may no longer match the new doc. */
            g_mk2_doc.undo_stack.clear();
            g_mk2_doc.redo_stack.clear();
            /* If an IMG is already loaded, pre-select the matching
               character so the panel comes up pointing at the right
               fighter without an extra click. */
            Mk2AutoSelectFromImg();
        } else {
            g_mk2_status = std::string("Load failed: ") + err;
            g_mk2_status_sticky = true;
        }
    }
    ImGui::SameLine();
    bool can_save = g_mk2_doc.dirty && !g_mk2_doc.source_path.empty();
    if (!can_save) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        std::string err;
        if (mk2::save(&g_mk2_doc, &err)) { g_mk2_status = "Saved MKSTK.ASM"; g_mk2_status_sticky = false; }
        else { g_mk2_status = std::string("Save failed: ") + err; g_mk2_status_sticky = true; }
    }
    if (!can_save) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_reload = !g_mk2_doc.source_path.empty();
    if (!can_reload) ImGui::BeginDisabled();
    if (ImGui::Button("Reload")) {
        /* Re-read MKSTK.ASM from disk, discarding any in-memory edits.
           Uses the previously-resolved source_path rather than the input
           box content so a stray edit there can't redirect the reload. */
        std::string err;
        std::string path = g_mk2_doc.source_path;
        if (mk2::load(&g_mk2_doc, path.c_str(), &err)) {
            char buf[160];
            snprintf(buf, sizeof(buf), "Reloaded %d moves, %d char tables",
                     (int)g_mk2_doc.records.size(), (int)g_mk2_doc.char_tables.size());
            g_mk2_status = buf;
            g_mk2_status_sticky = false;
            /* Keep the selection if the labels still resolve, otherwise
               fall back to the first move. */
            int new_char = g_mk2_char_idx;
            if (new_char >= (int)g_mk2_doc.char_tables.size()) new_char = 0;
            g_mk2_char_idx = new_char;
            if (g_mk2_move_idx >= (int)g_mk2_doc.char_tables[new_char].moves.size())
                g_mk2_move_idx = 0;
        } else {
            g_mk2_status = std::string("Reload failed: ") + err;
            g_mk2_status_sticky = true;
        }
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Re-read MKSTK.ASM from disk, discarding unsaved edits");
    if (!can_reload) ImGui::EndDisabled();

    ImGui::SameLine();
    bool can_undo_mk2 = mk2::can_undo(&g_mk2_doc);
    if (!can_undo_mk2) ImGui::BeginDisabled();
    if (ImGui::Button("Undo")) {
        int rec = mk2::undo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }
    if (!can_undo_mk2) ImGui::EndDisabled();

    ImGui::SameLine();
    bool can_redo_mk2 = mk2::can_redo(&g_mk2_doc);
    if (!can_redo_mk2) ImGui::BeginDisabled();
    if (ImGui::Button("Redo")) {
        int rec = mk2::redo_pop(&g_mk2_doc);
        if (rec >= 0) Mk2SelectRecord(rec);
    }
    if (!can_redo_mk2) ImGui::EndDisabled();

    if (!g_mk2_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_mk2_status.c_str());
    }

    /* If no document loaded yet, stop here. */
    if (g_mk2_doc.char_tables.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Click Browse... to pick MKSTK.ASM, then click Load.");
        ImGui::End();
        return;
    }

    ImGui::Separator();

    /* --- Three-pane layout: chars | moves | fields --- */
    const float row_h = ImGui::GetContentRegionAvail().y - 8.0f;
    ImGui::BeginChild("##mk2_chars", ImVec2(120, row_h), true);
    ImGui::TextDisabled("Character");
    for (int i = 0; i < (int)g_mk2_doc.char_tables.size(); i++) {
        const auto &t = g_mk2_doc.char_tables[i];
        char label[40];
        snprintf(label, sizeof(label), "%s (%d)", t.name.c_str(), (int)t.moves.size());
        if (ImGui::Selectable(label, g_mk2_char_idx == i)) {
            g_mk2_char_idx = i;
            g_mk2_move_idx = 0;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##mk2_moves", ImVec2(260, row_h), true);
    ImGui::TextDisabled("Move");
    /* Substring filter — case-insensitive. Empty box matches everything. */
    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##mk2_search", "filter...", g_mk2_search, sizeof(g_mk2_search));
    auto match_filter = [](const std::string &s, const char *needle) {
        if (!needle || !needle[0]) return true;
        std::string a = s; for (auto &c : a) c = (char)std::tolower((unsigned char)c);
        std::string b = needle; for (auto &c : b) c = (char)std::tolower((unsigned char)c);
        return a.find(b) != std::string::npos;
    };
    if (g_mk2_char_idx >= 0 && g_mk2_char_idx < (int)g_mk2_doc.char_tables.size()) {
        const auto &moves = g_mk2_doc.char_tables[g_mk2_char_idx].moves;
        for (int i = 0; i < (int)moves.size(); i++) {
            if (!match_filter(moves[i], g_mk2_search)) continue;
            char label[80];
            snprintf(label, sizeof(label), "%2d  %s", i, moves[i].c_str());
            if (ImGui::Selectable(label, g_mk2_move_idx == i))
                g_mk2_move_idx = i;
        }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##mk2_fields", ImVec2(0, row_h), true);
    /* Resolve selected move to a record index. */
    int rec_idx = -1;
    if (g_mk2_char_idx >= 0 && g_mk2_char_idx < (int)g_mk2_doc.char_tables.size()) {
        const auto &moves = g_mk2_doc.char_tables[g_mk2_char_idx].moves;
        if (g_mk2_move_idx >= 0 && g_mk2_move_idx < (int)moves.size())
            rec_idx = mk2::find_record(&g_mk2_doc, moves[g_mk2_move_idx].c_str());
    }
    if (rec_idx < 0) {
        ImGui::TextDisabled("Select a move on the left.");
    } else {
        const mk2::StrikeRecord &rec = g_mk2_doc.records[rec_idx];
        ImGui::Text("%s", rec.label.c_str());
        ImGui::TextDisabled("MKSTK.ASM line %d", rec.label_line);
        ImGui::Separator();
        ImGui::Spacing();

        /* x_offset / y_offset / x_size / y_size — int editors. */
        for (int fi = mk2::F_X_OFFSET; fi <= mk2::F_Y_SIZE; fi++) {
            int v = rec.fields[fi].has_value ? (int)rec.fields[fi].value : 0;
            ImGui::SetNextItemWidth(140);
            char id[40]; snprintf(id, sizeof(id), "%s##mk2_%d", mk2::kFieldNames[fi], fi);
            if (ImGui::InputInt(id, &v, 1, 8)) {
                if (v < -0x8000) v = -0x8000;
                if (v > 0x7FFF)  v = 0x7FFF;
                mk2::undo_push(&g_mk2_doc, rec_idx, false);
                mk2::set_value(&g_mk2_doc, rec_idx, fi, (int32_t)v);
            }
        }

        ImGui::Spacing();
        /* Strike routine and sound — raw token editors (may be symbolic). */
        for (int fi : { (int)mk2::F_STRIKE, (int)mk2::F_SOUND }) {
            char buf[64];
            const std::string &raw = rec.fields[fi].raw;
            size_t n = raw.size() < sizeof(buf) - 1 ? raw.size() : sizeof(buf) - 1;
            memcpy(buf, raw.data(), n); buf[n] = '\0';
            ImGui::SetNextItemWidth(180);
            char id[40]; snprintf(id, sizeof(id), "%s##mk2_%d", mk2::kFieldNames[fi], fi);
            if (ImGui::InputText(id, buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
                mk2::undo_push(&g_mk2_doc, rec_idx, false);
                mk2::set_raw(&g_mk2_doc, rec_idx, fi, buf);
            }
        }

        ImGui::Spacing();
        /* Damage word: split into hit (hi byte) and block (lo byte). */
        int dmg = rec.fields[mk2::F_DAMAGE].has_value ? (int)rec.fields[mk2::F_DAMAGE].value : 0;
        int hit = mk2::damage_hit(dmg), blk = mk2::damage_block(dmg);
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("damage_hit##mk2", &hit, 1, 4)) {
            if (hit < 0) hit = 0; if (hit > 255) hit = 255;
            mk2::undo_push(&g_mk2_doc, rec_idx, false);
            mk2::set_value(&g_mk2_doc, rec_idx, mk2::F_DAMAGE, mk2::pack_damage(hit, blk));
        }
        ImGui::SetNextItemWidth(80);
        if (ImGui::InputInt("damage_block##mk2", &blk, 1, 4)) {
            if (blk < 0) blk = 0; if (blk > 255) blk = 255;
            mk2::undo_push(&g_mk2_doc, rec_idx, false);
            mk2::set_value(&g_mk2_doc, rec_idx, mk2::F_DAMAGE, mk2::pack_damage(hit, blk));
        }
        ImGui::TextDisabled("damage word = 0x%04X", dmg & 0xFFFF);

        ImGui::Spacing();
        /* Score — 32-bit. */
        int score = rec.fields[mk2::F_SCORE].has_value ? (int)rec.fields[mk2::F_SCORE].value : 0;
        ImGui::SetNextItemWidth(160);
        if (ImGui::InputInt("score##mk2", &score, 100, 1000)) {
            mk2::undo_push(&g_mk2_doc, rec_idx, false);
            mk2::set_value(&g_mk2_doc, rec_idx, mk2::F_SCORE, (int32_t)score);
        }
    }
    ImGui::EndChild();

    ImGui::End();
}

static bool Mk2FatalityFilterMatch(const std::string &text, const char *filter)
{
    if (!filter || !filter[0]) return true;
    std::string a = text;
    std::string b = filter;
    for (char &c : a) c = (char)std::tolower((unsigned char)c);
    for (char &c : b) c = (char)std::tolower((unsigned char)c);
    return a.find(b) != std::string::npos;
}

struct Mk2FatalityFighterDef {
    const char *name;
    const char *source_file;
    const char *command_prefix[4];
    const char *img_files[24];
    const char *fatal_anims[10];
    const char *db1_anim;
    const char *db2_anim;
    const char *db1_victim;
    const char *db2_victim;
};

static const char *g_mk2_fatality_cage_deaths[] = {
    "a_torso_ripped", "a_decapfall", "a_head", "a_headhole", "a_swipe_torso",
    "a_nutcrunched", "a_bike_kicked", "a_drained", "a_banged", "a_impaled",
    "a_back_broke", "a_jc_arms_ripped", NULL
};

static const Mk2FatalityFighterDef g_mk2_fatality_fighters[] = {
    { "Johnny Cage", "MKJC.ASM", { "jc_", NULL },
      { "data/CAGE1.IMG", "data/CAGE2.IMG", "data/CAGE3.IMG", "data/CAGE4.IMG", "data/CAGE5.IMG",
        "data/CAGE6.IMG", "data/CAGE7.IMG", "data/CAGE8.IMG", "data/CAGE9.IMG", "data/CAGE10.IMG", NULL },
      { "a_jcrip", "a_jc_pp", "a_jc_headhole", "a_splits", NULL },
      "a_jcrip", "a_jc_pp", "a_torso_ripped", "a_head" },

    { "Liu Kang", "MKLK.ASM", { "lk_", NULL },
      { "data/KANG1.IMG", "data/KANG2.IMG", "data/KANG3.IMG", "data/KANG4.IMG", "data/KANG5.IMG",
        "data/KANG6.IMG", "data/KANG7.IMG", "data/KANG8.IMG", "data/KANG9.IMG", "data/KANG10.IMG",
        "data/LKBFIST.IMG", NULL },
      { "a_lkdragon", "a_lkwheel", "a_lkbike", NULL },
      "a_lkdragon", "a_lkwheel", "a_torso_ripped", "a_decapfall" },

    { "Raiden", "MKRD.ASM", { "rd_", NULL },
      { "data/RAID1.IMG", "data/RAID2.IMG", "data/RAID3.IMG", "data/RAID4.IMG", "data/RAID5.IMG",
        "data/RAID6.IMG", "data/RAID7.IMG", "data/RAID8.IMG", "data/RAID9.IMG", "data/RAIDWALK.IMG",
        "data/RADBOLT1.IMG", "data/RADBOLT2.IMG", NULL },
      { "a_death_zap1", "a_death_bolt1", "a_death_zap2", "a_death_shock", NULL },
      "a_death_zap1", "a_death_zap2", "a_torso_ripped", "a_decapfall" },

    { "Shang Tsung", "MKST.ASM", { "st_", NULL },
      { "data/TSUNG1.IMG", "data/TSUNG2.IMG", "data/TSUNG3.IMG", "data/TSUNG4.IMG", "data/TSUNG5.IMG",
        "data/TSUNG6.IMG", "data/TSUNG7.IMG", "data/TSUNG8.IMG", "data/TSUNG9.IMG", "data/TSUNG10.IMG",
        "data/TSUNG1G.IMG", "data/OLDSHNG.IMG", NULL },
      { "a_st_kano_morph", "a_st_kano_roll", "a_st_kano_back", "a_st_2_jc", NULL },
      "a_st_kano_roll", "a_st_kano_morph", "a_decapfall", "a_drained" },

    { "Baraka", "MKSA.ASM", { "sa_", NULL },
      { "data/UGMO1.IMG", "data/UGMO2.IMG", "data/UGMO3.IMG", "data/UGMO4.IMG", "data/UGMO5.IMG",
        "data/UGMO6.IMG", "data/UGMO7.IMG", "data/UGMO8.IMG", "data/UGMO9.IMG", "data/UGMO10.IMG",
        "data/UGMO1SHO.IMG", NULL },
      { "a_sashred", "a_sastab", "a_swipe", NULL },
      "a_sashred", "a_sastab", "a_decapfall", "a_impaled" },

    { "Kitana", "MKFN.ASM", { "fn1_", NULL },
      { "data/KAT1.IMG", "data/KAT2.IMG", "data/KAT3.IMG", "data/KAT4.IMG", "data/KAT5.IMG",
        "data/KAT6.IMG", "data/KAT7.IMG", "data/KAT8.IMG", "data/KAT9.IMG", "data/KAT10.IMG",
        "data/KAT11.IMG", NULL },
      { "a_death_kiss1", "a_fan_swipe", NULL },
      "a_death_kiss1", "a_fan_swipe", "a_drained", "a_decapfall" },

    { "Mileena", "MKFN.ASM", { "fn2_", NULL },
      { "data/KAT1.IMG", "data/KAT2.IMG", "data/KAT3.IMG", "data/KAT4.IMG", "data/KAT5.IMG",
        "data/KAT6.IMG", "data/KAT7.IMG", "data/KAT8.IMG", "data/KAT9.IMG", "data/KAT10.IMG",
        "data/KAT11.IMG", NULL },
      { "a_fn2_stab", "a_death_kiss2", NULL },
      "a_fn2_stab", "a_death_kiss2", "a_impaled", "a_drained" },

    { "Sub-Zero", "MKNJ.ASM", { "sz_", NULL },
      { "data/NINJAS1.IMG", "data/NINJAS2.IMG", "data/NINJAS3.IMG", "data/NINJAS4.IMG", "data/NINJAS5.IMG",
        "data/NINJAS6.IMG", "data/NINJAS7.IMG", "data/NINJAS8.IMG", "data/NINJAS9.IMG", "data/NINJAS10.IMG",
        "data/NINJAS11.IMG", "data/NINJAS12.IMG", "data/FREEZE1.IMG", "data/FROZEN.IMG", "data/SNOBALL.IMG", NULL },
      { "a_sz_tornado", "a_pitch", "a_ice_ball", NULL },
      "a_sz_tornado", "a_pitch", "a_torso_ripped", "a_decapfall" },

    { "Scorpion", "MKNJ.ASM", { "sc_", NULL },
      { "data/NINJAS1.IMG", "data/NINJAS2.IMG", "data/NINJAS3.IMG", "data/NINJAS4.IMG", "data/NINJAS5.IMG",
        "data/NINJAS6.IMG", "data/NINJAS7.IMG", "data/NINJAS8.IMG", "data/NINJAS9.IMG", "data/NINJAS10.IMG",
        "data/NINJAS11.IMG", "data/NINJAS12.IMG", "data/NEWROPE.IMG", NULL },
      { "a_scortch", "a_scorpion_skull", "a_sc_swipe", "a_death_spear", NULL },
      "a_scortch", "a_sc_swipe", "a_torso_ripped", "a_swipe_torso" },

    { "Reptile", "MKNJ.ASM", { "rp_", NULL },
      { "data/NINJAS1.IMG", "data/NINJAS2.IMG", "data/NINJAS3.IMG", "data/NINJAS4.IMG", "data/NINJAS5.IMG",
        "data/NINJAS6.IMG", "data/NINJAS7.IMG", "data/NINJAS8.IMG", "data/NINJAS9.IMG", "data/NINJAS10.IMG",
        "data/NINJAS11.IMG", "data/NINJAS12.IMG", "data/ACID1.IMG", NULL },
      { "a_eat_head", "a_spit", "a_slow_proj", NULL },
      "a_eat_head", "a_eat_head", "a_head", "a_head" },

    { "Jax", "MKJX.ASM", { "jx_", NULL },
      { "data/NUJAX1.IMG", "data/NUJAX2.IMG", "data/NUJAX3.IMG", "data/NUJAX4.IMG", "data/NUJAX5.IMG",
        "data/NUJAX6.IMG", "data/NUJAX7.IMG", "data/NUJAX8.IMG", "data/NUJAX9.IMG", "data/NUJAX10.IMG",
        "data/JAXPRO.IMG", "data/MKJXARMS.IMG", NULL },
      { "a_clap", "a_back_breaker", "a_arm_rip", NULL },
      "a_clap", "a_arm_rip", "a_head", "a_jc_arms_ripped" },

    { "Kung Lao", "MKHH.ASM", { "hh_", NULL },
      { "data/HATHED1.IMG", "data/HATHED2.IMG", "data/HATHED3.IMG", "data/HATHED4.IMG", "data/HATHED5.IMG",
        "data/HATHED6.IMG", "data/HATHED7.IMG", "data/HATHED8.IMG", "data/HATHED9.IMG", "data/HATHED10.IMG",
        "data/HATHED11.IMG", "data/HATHED12.IMG", NULL },
      { "a_spin", "a_hh_hat_swipe", NULL },
      "a_spin", "a_hh_hat_swipe", "a_torso_ripped", "a_decapfall" },
};

static const int kMk2FatalityFighterCount =
    (int)(sizeof(g_mk2_fatality_fighters) / sizeof(g_mk2_fatality_fighters[0]));

static void Mk2FatalityClampSelections(void)
{
    if (g_mk2_fatality_command_idx < 0) g_mk2_fatality_command_idx = 0;
    if (g_mk2_fatality_combo_idx < 0) g_mk2_fatality_combo_idx = 0;
    if (g_mk2_fatality_anim_idx < 0) g_mk2_fatality_anim_idx = 0;
    if (g_mk2_fatality_command_idx >= (int)g_mk2_fatality_doc.commands.size())
        g_mk2_fatality_command_idx = (int)g_mk2_fatality_doc.commands.size() - 1;
    if (g_mk2_fatality_combo_idx >= (int)g_mk2_fatality_doc.combos.size())
        g_mk2_fatality_combo_idx = (int)g_mk2_fatality_doc.combos.size() - 1;
    if (g_mk2_fatality_anim_idx >= (int)g_mk2_fatality_doc.animations.size())
        g_mk2_fatality_anim_idx = (int)g_mk2_fatality_doc.animations.size() - 1;
    if (g_mk2_fatality_command_idx < 0) g_mk2_fatality_command_idx = 0;
    if (g_mk2_fatality_combo_idx < 0) g_mk2_fatality_combo_idx = 0;
    if (g_mk2_fatality_anim_idx < 0) g_mk2_fatality_anim_idx = 0;
}

static void Mk2FatalityLoadRoot(const char *root)
{
    std::string err;
    if (mk2fatal::load(&g_mk2_fatality_doc, root, &err)) {
        char buf[192];
        snprintf(buf, sizeof(buf), "Loaded %d command blocks, %d combos, %d animation blocks",
                 (int)g_mk2_fatality_doc.commands.size(),
                 (int)g_mk2_fatality_doc.combos.size(),
                 (int)g_mk2_fatality_doc.animations.size());
        g_mk2_fatality_status = buf;
        g_mk2_fatality_status_sticky = false;
        g_mk2_fatality_command_idx = 0;
        g_mk2_fatality_combo_idx = 0;
        g_mk2_fatality_anim_idx = 0;
        g_mk2_fatality_selected_line = 0;
        g_mk2_fatality_filter[0] = '\0';
    } else {
        g_mk2_fatality_status = std::string("Load failed: ") + err;
        g_mk2_fatality_status_sticky = true;
    }
}

static bool DrawMk2FatalitySourceEditor(const char *id, int file_idx, int start_line, int end_line,
                                        int *selected_line, char *insert_buf,
                                        size_t insert_buf_size, bool allow_insert_delete)
{
    const mk2fatal::SourceFile *sf = mk2fatal::get_file(&g_mk2_fatality_doc, file_idx);
    if (!sf) {
        ImGui::TextDisabled("Source file unavailable.");
        return false;
    }
    if (start_line <= 0) start_line = 1;
    if (end_line > (int)sf->lines.size()) end_line = (int)sf->lines.size();
    if (end_line < start_line) {
        ImGui::TextDisabled("No source lines in this block.");
        return false;
    }
    if (*selected_line < start_line || *selected_line > end_line) *selected_line = start_line;

    ImGui::TextDisabled("%s  lines %d-%d", sf->rel_path.c_str(), start_line, end_line);
    const float footer_h = allow_insert_delete ? 64.0f : 0.0f;
    bool changed = false;
    bool structural = false;

    ImGui::BeginChild(id, ImVec2(0, -footer_h), true);
    if (ImGui::BeginTable("##mk2fatal_src_table", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Line", ImGuiTableColumnFlags_WidthFixed, 54.0f);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        for (int line = start_line; line <= end_line; line++) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::PushID(line);
            char lbuf[24];
            snprintf(lbuf, sizeof(lbuf), "%d", line);
            if (ImGui::Selectable(lbuf, *selected_line == line))
                *selected_line = line;
            ImGui::TableSetColumnIndex(1);
            char buf[1024];
            const std::string &src = sf->lines[line - 1];
            size_t n = src.size() < sizeof(buf) - 1 ? src.size() : sizeof(buf) - 1;
            memcpy(buf, src.data(), n);
            buf[n] = '\0';
            ImGui::SetNextItemWidth(-FLT_MIN);
            if (ImGui::InputText("##src", buf, sizeof(buf))) {
                if (mk2fatal::set_line(&g_mk2_fatality_doc, file_idx, line, buf))
                    changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (allow_insert_delete) {
        ImGui::SetNextItemWidth(-220);
        ImGui::InputTextWithHint("##mk2fatal_insert", "assembly line to insert", insert_buf, insert_buf_size);
        ImGui::SameLine();
        bool can_insert = insert_buf && insert_buf[0] && *selected_line >= start_line && *selected_line <= end_line;
        if (!can_insert) ImGui::BeginDisabled();
        if (ImGui::Button("Insert Before")) {
            if (mk2fatal::insert_line(&g_mk2_fatality_doc, file_idx, *selected_line, insert_buf)) {
                structural = true;
                end_line++;
            }
        }
        if (!can_insert) ImGui::EndDisabled();
        ImGui::SameLine();
        bool can_delete = *selected_line > start_line && *selected_line <= end_line;
        if (!can_delete) ImGui::BeginDisabled();
        if (ImGui::Button("Delete Line")) {
            if (mk2fatal::delete_line(&g_mk2_fatality_doc, file_idx, *selected_line)) {
                structural = true;
                if (*selected_line > start_line) (*selected_line)--;
            }
        }
        if (!can_delete) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("The label line is protected; select a .long/.word line to delete.");
    }

    if (changed || structural) {
        std::string err;
        mk2fatal::reparse(&g_mk2_fatality_doc, &err);
        if (!g_mk2_fatality_status_sticky && !g_mk2_fatality_status.empty())
            g_mk2_fatality_status.clear();
        Mk2FatalityClampSelections();
    }
    return changed || structural;
}

static bool Mk2FatalityFileExists(const std::string &path)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fclose(f);
    return true;
}

static std::string Mk2FatalityResolveProjectAsset(const std::string &rel_path)
{
    std::string root = g_mk2_fatality_doc.root_path.empty()
                     ? std::string(g_mk2_fatality_root)
                     : g_mk2_fatality_doc.root_path;
    std::string p = PathCombine(root, rel_path);
    if (Mk2FatalityFileExists(p)) return p;

    std::string parent = GetParentDirectory(root);
    p = PathCombine(parent, rel_path);
    if (Mk2FatalityFileExists(p)) return p;

    return PathCombine(root, rel_path);
}

static bool Mk2FatalityNameEquals(const std::string &a, const std::string &b)
{
    size_t na = a.size();
    size_t nb = b.size();
    while (na > 0 && a[na - 1] == '\0') na--;
    while (nb > 0 && b[nb - 1] == '\0') nb--;
    if (na != nb) return false;
    for (size_t i = 0; i < na; i++)
        if (std::toupper((unsigned char)a[i]) != std::toupper((unsigned char)b[i]))
            return false;
    return true;
}

static bool Mk2FatalityVectorHas(const std::vector<std::string> &items, const std::string &value)
{
    for (const std::string &item : items)
        if (Mk2FatalityNameEquals(item, value)) return true;
    return false;
}

static void Mk2FatalityPushUnique(std::vector<std::string> *items, const std::string &value)
{
    if (value.empty()) return;
    if (Mk2FatalityVectorHas(*items, value)) return;
    items->push_back(value);
}

static void Mk2FatalityMergePlan(mk2fatal::AssetPlan *dst, const mk2fatal::AssetPlan &src)
{
    if (!dst) return;
    if (dst->root_label.empty()) dst->root_label = src.root_label;
    if (dst->resolved_label.empty()) dst->resolved_label = src.resolved_label;
    if (dst->preferred_file.empty()) dst->preferred_file = src.preferred_file;
    for (const std::string &s : src.animation_labels) Mk2FatalityPushUnique(&dst->animation_labels, s);
    for (const std::string &s : src.sprite_labels) Mk2FatalityPushUnique(&dst->sprite_labels, s);
    for (const std::string &s : src.missing_labels) Mk2FatalityPushUnique(&dst->missing_labels, s);
    for (const std::string &s : src.img_files) Mk2FatalityPushUnique(&dst->img_files, s);
}

static int Mk2FatalityFindImageBySpriteLabel(Document *doc, const std::string &label)
{
    if (!doc) return -1;
    for (int i = 0; i < (int)doc->imgcnt; i++) {
        IMG *img = doc_get_img(doc, i);
        if (!img) continue;
        if (Mk2FatalityNameEquals(img_name_string(img), label)) return i;
    }
    return -1;
}

static int Mk2FatalityFindImageBySpriteLabel(const std::string &label)
{
    return Mk2FatalityFindImageBySpriteLabel(g_doc, label);
}

static mk2fatal::AssetPlan Mk2FatalityBuildPlanForLabels(const std::vector<std::string> &labels,
                                                         const char *preferred_file,
                                                         const char *const *img_files)
{
    mk2fatal::AssetPlan plan;
    plan.root_label = labels.empty() ? "" : labels[0];
    plan.preferred_file = preferred_file ? preferred_file : "";
    for (const std::string &label : labels) {
        mk2fatal::AssetPlan part;
        std::string err;
        if (mk2fatal::build_asset_plan(&g_mk2_fatality_doc, label.c_str(), preferred_file, &part, &err))
            Mk2FatalityMergePlan(&plan, part);
        else
            Mk2FatalityPushUnique(&plan.missing_labels, label);
    }
    if (img_files) {
        for (int i = 0; img_files[i]; i++)
            Mk2FatalityPushUnique(&plan.img_files, img_files[i]);
    }
    return plan;
}

static bool Mk2FatalityCommandMatchesFighter(const mk2fatal::CommandBlock &cmd,
                                             const Mk2FatalityFighterDef &fighter)
{
    for (int i = 0; i < 4 && fighter.command_prefix[i]; i++) {
        const char *p = fighter.command_prefix[i];
        size_t n = strlen(p);
        if (cmd.label.size() >= n) {
            bool match = true;
            for (size_t j = 0; j < n; j++) {
                if (std::tolower((unsigned char)cmd.label[j]) !=
                    std::tolower((unsigned char)p[j])) {
                    match = false;
                    break;
                }
            }
            if (match) return true;
        }
        if (!cmd.combo_label.empty() && Mk2FatalityFilterMatch(cmd.combo_label, p))
            return true;
    }
    return false;
}

static std::vector<int> Mk2FatalityFighterCommandIndices(const Mk2FatalityFighterDef &fighter)
{
    std::vector<int> out;
    for (int i = 0; i < (int)g_mk2_fatality_doc.commands.size(); i++) {
        const mk2fatal::CommandBlock &cmd = g_mk2_fatality_doc.commands[i];
        if (Mk2FatalityCommandMatchesFighter(cmd, fighter))
            out.push_back(i);
    }
    return out;
}

static int Mk2FatalityAnimListIndex(const char *const *items, const char *label)
{
    if (!items || !label || !label[0]) return 0;
    for (int i = 0; items[i]; i++)
        if (Mk2FatalityNameEquals(items[i], label)) return i;
    return 0;
}

static const char *Mk2FatalitySelectedAttackerAnim(const Mk2FatalityFighterDef &fighter)
{
    int count = 0;
    while (count < 10 && fighter.fatal_anims[count]) count++;
    if (count == 0) return NULL;
    if (g_mk2_fatality_attacker_anim_idx < 0) g_mk2_fatality_attacker_anim_idx = 0;
    if (g_mk2_fatality_attacker_anim_idx >= count) g_mk2_fatality_attacker_anim_idx = count - 1;
    return fighter.fatal_anims[g_mk2_fatality_attacker_anim_idx];
}

static const char *Mk2FatalitySelectedVictimAnim(void)
{
    int count = 0;
    while (g_mk2_fatality_cage_deaths[count]) count++;
    if (g_mk2_fatality_victim_anim_idx < 0) g_mk2_fatality_victim_anim_idx = 0;
    if (g_mk2_fatality_victim_anim_idx >= count) g_mk2_fatality_victim_anim_idx = count - 1;
    return g_mk2_fatality_cage_deaths[g_mk2_fatality_victim_anim_idx];
}

static std::string Mk2FatalityInferAnimationFromRoutine(const std::string &routine)
{
    if (routine.size() <= 3) return std::string();
    std::string lower = routine;
    for (char &c : lower) c = (char)std::tolower((unsigned char)c);
    if (lower.find("do_") != 0) return std::string();

    std::string candidate = std::string("a_") + routine.substr(3);
    std::string err;
    mk2fatal::AssetPlan tmp;
    if (mk2fatal::build_asset_plan(&g_mk2_fatality_doc, candidate.c_str(), "MKJC.ASM", &tmp, &err))
        return candidate;
    return std::string();
}

static void Mk2FatalityApplyTimelineFromPlan(const mk2fatal::AssetPlan &plan,
                                             int *matched_sprites,
                                             int *missing_sprites)
{
    if (matched_sprites) *matched_sprites = 0;
    if (missing_sprites) *missing_sprites = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p)
        img->flags &= ~1u;

    TimelineClearFrames();
    g_timeline_play_dir = 1;
    ClearTimelineCompositeSelection();

    for (const std::string &sprite : plan.sprite_labels) {
        int idx = Mk2FatalityFindImageBySpriteLabel(sprite);
        if (idx >= 0) {
            IMG *img = get_img(idx);
            if (img) img->flags |= 1u;
            if (std::find(g_timeline_frames.begin(), g_timeline_frames.end(), idx) == g_timeline_frames.end())
                TimelinePushFrame(idx);
            if (matched_sprites) (*matched_sprites)++;
        } else if (missing_sprites) {
            (*missing_sprites)++;
        }
    }

    g_timeline_built_for_imgcnt = g_doc->imgcnt;
    if (!g_timeline_frames.empty()) {
        g_doc->ilselected = g_timeline_frames[0];
        g_is_playing = true;
        g_play_speed = 8.0f;
        g_zoom_reset = true;
    }
}

static void Mk2FatalityLoadPlanImagesIntoActiveDoc(const mk2fatal::AssetPlan &plan,
                                                   int *loaded_files,
                                                   int *missing_files)
{
    if (loaded_files) *loaded_files = 0;
    if (missing_files) *missing_files = 0;
    for (const std::string &rel : plan.img_files) {
        std::string full = Mk2FatalityResolveProjectAsset(rel);
        if (!Mk2FatalityFileExists(full)) {
            if (missing_files) (*missing_files)++;
            continue;
        }
        unsigned int before = g_doc->imgcnt;
        SetActiveDocumentPath(full);
        LoadImgFile();
        if (g_doc->imgcnt > before) {
            if (loaded_files) (*loaded_files)++;
            RecentAdd(full);
        }
    }
    g_dirty = false;
}

static void Mk2FatalityMarkPlanInDoc(Document *doc, const mk2fatal::AssetPlan &plan,
                                     std::vector<int> *marked_indices,
                                     int *matched_sprites,
                                     int *missing_sprites)
{
    if (marked_indices) marked_indices->clear();
    if (matched_sprites) *matched_sprites = 0;
    if (missing_sprites) *missing_sprites = 0;
    if (!doc) return;
    for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p)
        img->flags &= ~1u;

    for (const std::string &sprite : plan.sprite_labels) {
        int idx = Mk2FatalityFindImageBySpriteLabel(doc, sprite);
        if (idx >= 0) {
            IMG *img = doc_get_img(doc, idx);
            if (img) img->flags |= 1u;
            if (marked_indices &&
                std::find(marked_indices->begin(), marked_indices->end(), idx) == marked_indices->end())
                marked_indices->push_back(idx);
            if (matched_sprites) (*matched_sprites)++;
        } else if (missing_sprites) {
            (*missing_sprites)++;
        }
    }
}

static void Mk2FatalityStageDualPlans(const Mk2FatalityFighterDef &fighter,
                                      const mk2fatal::AssetPlan &attacker_plan,
                                      const mk2fatal::AssetPlan &victim_plan)
{
    PrepareDocumentForOpenedFile();
    int attacker_doc_idx = document_active_index();
    int attacker_loaded = 0, attacker_missing_files = 0;
    Mk2FatalityLoadPlanImagesIntoActiveDoc(attacker_plan, &attacker_loaded, &attacker_missing_files);
    Document *attacker_doc = document_get(attacker_doc_idx);

    std::vector<int> attacker_marked;
    int attacker_matched = 0, attacker_missing_sprites = 0;
    Mk2FatalityMarkPlanInDoc(attacker_doc, attacker_plan, &attacker_marked,
                             &attacker_matched, &attacker_missing_sprites);
    if (attacker_doc) attacker_doc->dirty = 0;

    int victim_doc_idx = document_new_tab();
    ClearAll();
    int victim_loaded = 0, victim_missing_files = 0;
    Mk2FatalityLoadPlanImagesIntoActiveDoc(victim_plan, &victim_loaded, &victim_missing_files);
    Document *victim_doc = document_get(victim_doc_idx);

    std::vector<int> victim_marked;
    int victim_matched = 0, victim_missing_sprites = 0;
    Mk2FatalityMarkPlanInDoc(victim_doc, victim_plan, &victim_marked,
                             &victim_matched, &victim_missing_sprites);
    if (victim_doc) victim_doc->dirty = 0;

    document_set_active(attacker_doc_idx);
    TimelineSetFrames(attacker_marked);
    g_timeline_play_idx = 0;
    g_timeline_play_dir = 1;
    g_timeline_built_for_imgcnt = g_doc->imgcnt;
    if (!g_timeline_frames.empty())
        g_doc->ilselected = g_timeline_frames[0];

    g_world_state.enabled = true;
    g_world_marked_state.marked_play = true;
    g_world_marked_state.fps = g_mk2_fatality_preview_fps;
    g_play_speed = g_mk2_fatality_preview_fps;
    g_is_playing = true;
    g_world_marked_state.mirror_active = false;
    g_world_marked_state.mirror_other = true;
    g_world_marked_state.mirror_extra[0] = false;
    g_world_marked_state.mirror_extra[1] = false;
    g_world_marked_state.mirror_extra[2] = false;
    for (int i = 0; i < kWorldMarkedMaxTabs; i++)
        g_world_marked_state.hold_end[i] = false;
    g_world_marked_state.hold_end[kWorldDummyDecapSlot] = true;
    g_world_marked_state.dummy_decap_body = false;
    g_world_marked_state.dummy_decap_reset = true;
    g_world_marked_state.dummy_decap_manual = false;
    g_world_marked_state.dummy_decap_doc_idx = -1;
    g_world_marked_state.dummy_decap_prefix.clear();
    g_world_marked_state.paused = false;
    WorldMarkedRestart(g_world_marked_state);
    g_zoom_reset = true;

    char buf[320];
    snprintf(buf, sizeof(buf),
             "%s staged: attacker %d IMG/%d sprite%s, Cage victim %d IMG/%d sprite%s%s%s.",
             fighter.name,
             attacker_loaded, attacker_matched, attacker_matched == 1 ? "" : "s",
             victim_loaded, victim_matched, victim_matched == 1 ? "" : "s",
             (attacker_missing_sprites || victim_missing_sprites) ? " (some sprite refs missing)" : "",
             (attacker_missing_files || victim_missing_files) ? " (some IMG files missing)" : "");
    g_mk2_fatality_stage_status = buf;
}

static mk2fatal::AssetPlan Mk2FatalityBuildCageDeathPlan(const std::vector<std::string> &labels)
{
    static const char *kCageImgs[] = {
        "data/CAGE1.IMG", "data/CAGE2.IMG", "data/CAGE3.IMG", "data/CAGE4.IMG", "data/CAGE5.IMG",
        "data/CAGE6.IMG", "data/CAGE7.IMG", "data/CAGE8.IMG", "data/CAGE9.IMG", "data/CAGE10.IMG", NULL
    };
    return Mk2FatalityBuildPlanForLabels(labels, "MKJC.ASM", kCageImgs);
}

static void Mk2FatalityStageFighterWorkspace(void)
{
    if (g_mk2_fatality_fighter_idx < 0 || g_mk2_fatality_fighter_idx >= kMk2FatalityFighterCount)
        return;
    const Mk2FatalityFighterDef &fighter = g_mk2_fatality_fighters[g_mk2_fatality_fighter_idx];

    std::vector<std::string> attacker_labels;
    for (int i = 0; i < 10 && fighter.fatal_anims[i]; i++)
        attacker_labels.push_back(fighter.fatal_anims[i]);
    mk2fatal::AssetPlan attacker_plan =
        Mk2FatalityBuildPlanForLabels(attacker_labels, fighter.source_file, fighter.img_files);

    std::vector<std::string> victim_labels;
    for (int i = 0; g_mk2_fatality_cage_deaths[i]; i++)
        victim_labels.push_back(g_mk2_fatality_cage_deaths[i]);
    mk2fatal::AssetPlan victim_plan = Mk2FatalityBuildCageDeathPlan(victim_labels);

    g_mk2_fatality_plan = victim_plan;
    Mk2FatalityStageDualPlans(fighter, attacker_plan, victim_plan);
}

static void Mk2FatalityApplyFatalityDefaults(const mk2fatal::CommandBlock &cmd,
                                             const Mk2FatalityFighterDef &fighter)
{
    const char *attacker = fighter.fatal_anims[0];
    const char *victim = "a_torso_ripped";
    if (cmd.routine == "do_fatality_1") {
        attacker = fighter.db1_anim ? fighter.db1_anim : attacker;
        victim = fighter.db1_victim ? fighter.db1_victim : victim;
    } else if (cmd.routine == "do_fatality_2") {
        attacker = fighter.db2_anim ? fighter.db2_anim : attacker;
        victim = fighter.db2_victim ? fighter.db2_victim : victim;
    } else if (Mk2FatalityFilterMatch(cmd.routine, "headhole")) {
        attacker = "a_jc_headhole";
        victim = "a_headhole";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "raiden_lift")) {
        attacker = "a_death_zap1";
        victim = "a_torso_ripped";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "decap")) {
        victim = "a_decapfall";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "rip")) {
        victim = "a_torso_ripped";
    } else if (Mk2FatalityFilterMatch(cmd.routine, "head")) {
        victim = "a_head";
    }

    g_mk2_fatality_attacker_anim_idx = Mk2FatalityAnimListIndex(fighter.fatal_anims, attacker);
    g_mk2_fatality_victim_anim_idx = Mk2FatalityAnimListIndex(g_mk2_fatality_cage_deaths, victim);
}

static void Mk2FatalityStageSelectedFatality(void)
{
    if (g_mk2_fatality_fighter_idx < 0 || g_mk2_fatality_fighter_idx >= kMk2FatalityFighterCount)
        return;
    const Mk2FatalityFighterDef &fighter = g_mk2_fatality_fighters[g_mk2_fatality_fighter_idx];

    std::vector<std::string> attacker_labels;
    const char *attacker_anim = Mk2FatalitySelectedAttackerAnim(fighter);
    if (attacker_anim) attacker_labels.push_back(attacker_anim);
    mk2fatal::AssetPlan attacker_plan =
        Mk2FatalityBuildPlanForLabels(attacker_labels, fighter.source_file, fighter.img_files);

    std::vector<std::string> victim_labels;
    const char *victim_anim = Mk2FatalitySelectedVictimAnim();
    if (victim_anim) victim_labels.push_back(victim_anim);
    mk2fatal::AssetPlan victim_plan = Mk2FatalityBuildCageDeathPlan(victim_labels);

    g_mk2_fatality_plan = victim_plan;
    Mk2FatalityStageDualPlans(fighter, attacker_plan, victim_plan);
}

static void Mk2FatalityStageAssetPlan(const mk2fatal::AssetPlan &plan)
{
    if (plan.img_files.empty()) {
        g_mk2_fatality_stage_status = "No IMG libraries were resolved for this plan.";
        return;
    }

    PrepareDocumentForOpenedFile();
    int loaded_files = 0;
    int missing_files = 0;
    for (const std::string &rel : plan.img_files) {
        std::string full = Mk2FatalityResolveProjectAsset(rel);
        if (!Mk2FatalityFileExists(full)) {
            missing_files++;
            continue;
        }
        unsigned int before = g_doc->imgcnt;
        SetActiveDocumentPath(full);
        LoadImgFile();
        if (g_doc->imgcnt > before) {
            loaded_files++;
            RecentAdd(full);
        }
    }

    int matched = 0;
    int missing_sprites = 0;
    Mk2FatalityApplyTimelineFromPlan(plan, &matched, &missing_sprites);
    g_dirty = false;
    g_img_tex_idx = -2;

    char buf[256];
    snprintf(buf, sizeof(buf),
             "Staged %d IMG file%s, %d/%d referenced sprite%s matched%s%s.",
             loaded_files, loaded_files == 1 ? "" : "s",
             matched, (int)plan.sprite_labels.size(),
             plan.sprite_labels.size() == 1 ? "" : "s",
             missing_sprites ? " (some missing)" : "",
             missing_files ? " (some IMG files missing)" : "");
    g_mk2_fatality_stage_status = buf;
}

static void Mk2FatalityBuildAndStage(const char *animation_label)
{
    if (!animation_label || !animation_label[0]) return;
    std::string err;
    mk2fatal::AssetPlan plan;
    if (!mk2fatal::build_asset_plan(&g_mk2_fatality_doc, animation_label, "MKJC.ASM", &plan, &err)) {
        g_mk2_fatality_stage_status = std::string("Stage failed: ") + err;
        return;
    }
    g_mk2_fatality_plan = plan;
    Mk2FatalityStageAssetPlan(g_mk2_fatality_plan);
}

static void DrawMk2FatalityPlanSummary(void)
{
    if (g_mk2_fatality_plan.root_label.empty()) return;
    ImGui::Separator();
    ImGui::TextDisabled("Staged plan: %s via %s",
                        g_mk2_fatality_plan.resolved_label.empty()
                            ? g_mk2_fatality_plan.root_label.c_str()
                            : g_mk2_fatality_plan.resolved_label.c_str(),
                        g_mk2_fatality_plan.preferred_file.empty()
                            ? "source" : g_mk2_fatality_plan.preferred_file.c_str());
    ImGui::TextDisabled("%d animation label%s, %d sprite label%s, %d IMG librar%s",
                        (int)g_mk2_fatality_plan.animation_labels.size(),
                        g_mk2_fatality_plan.animation_labels.size() == 1 ? "" : "s",
                        (int)g_mk2_fatality_plan.sprite_labels.size(),
                        g_mk2_fatality_plan.sprite_labels.size() == 1 ? "" : "s",
                        (int)g_mk2_fatality_plan.img_files.size(),
                        g_mk2_fatality_plan.img_files.size() == 1 ? "y" : "ies");
    if (!g_mk2_fatality_stage_status.empty())
        ImGui::TextDisabled("%s", g_mk2_fatality_stage_status.c_str());
}

static void DrawMk2FatalityWindow(void)
{
    if (!g_show_mk2_fatality) return;

    ImGui::SetNextWindowSize(ImVec2(980, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("MK2 Fatality Lab", &g_show_mk2_fatality)) {
        ImGui::End();
        return;
    }

    if (!g_mk2_fatality_status_sticky && g_mk2_fatality_doc.dirty && !g_mk2_fatality_status.empty())
        g_mk2_fatality_status.clear();

    ImGui::SetNextItemWidth(-360);
    ImGui::InputTextWithHint("##mk2fatal_root", "path to mk2-main or its src folder", g_mk2_fatality_root, sizeof(g_mk2_fatality_root));
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
#ifdef _WIN32
        char path[MAX_PATH] = "";
        BROWSEINFOA bi = {};
        bi.lpszTitle = "Select mk2-main folder";
        bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_USENEWUI;
        LPITEMIDLIST pidl = SHBrowseForFolderA(&bi);
        if (pidl) {
            if (SHGetPathFromIDListA(pidl, path) && path[0]) {
                strncpy(g_mk2_fatality_root, path, sizeof(g_mk2_fatality_root) - 1);
                g_mk2_fatality_root[sizeof(g_mk2_fatality_root) - 1] = '\0';
                save_last_dir_cat(path, "mk2fatal");
            }
            CoTaskMemFree(pidl);
        }
#else
        g_mk2_fatality_status = "Browse not implemented on this platform - type the path manually";
        g_mk2_fatality_status_sticky = true;
#endif
    }
    ImGui::SameLine();
    if (ImGui::Button("Use ../mk2-main")) {
        strncpy(g_mk2_fatality_root, "..\\mk2-main", sizeof(g_mk2_fatality_root) - 1);
        g_mk2_fatality_root[sizeof(g_mk2_fatality_root) - 1] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button("Load")) {
        Mk2FatalityLoadRoot(g_mk2_fatality_root);
    }
    ImGui::SameLine();
    bool can_save = g_mk2_fatality_doc.dirty && !g_mk2_fatality_doc.files.empty();
    if (!can_save) ImGui::BeginDisabled();
    if (ImGui::Button("Save")) {
        std::string err;
        if (mk2fatal::save(&g_mk2_fatality_doc, &err)) {
            g_mk2_fatality_status = "Saved MK2 fatality source edits";
            g_mk2_fatality_status_sticky = false;
        } else {
            g_mk2_fatality_status = std::string("Save failed: ") + err;
            g_mk2_fatality_status_sticky = true;
        }
    }
    if (!can_save) ImGui::EndDisabled();
    ImGui::SameLine();
    bool can_reload = !g_mk2_fatality_doc.root_path.empty();
    if (!can_reload) ImGui::BeginDisabled();
    if (ImGui::Button("Reload")) {
        std::string root = g_mk2_fatality_doc.root_path;
        Mk2FatalityLoadRoot(root.c_str());
    }
    if (!can_reload) ImGui::EndDisabled();

    if (!g_mk2_fatality_status.empty()) {
        ImGui::SameLine();
        ImGui::TextDisabled("%s", g_mk2_fatality_status.c_str());
    }

    if (g_mk2_fatality_doc.files.empty()) {
        ImGui::Spacing();
        ImGui::TextWrapped("Load an MK2 source root to browse fatality command blocks, controller combo tables, and body-ending animation sequences.");
        ImGui::End();
        return;
    }

    Mk2FatalityClampSelections();
    ImGui::Separator();
    ImGui::TextDisabled("%d files loaded. Dirty files save back to the same ASM paths.",
                        (int)g_mk2_fatality_doc.files.size());

    if (g_mk2_fatality_fighter_idx < 0) g_mk2_fatality_fighter_idx = 0;
    if (g_mk2_fatality_fighter_idx >= kMk2FatalityFighterCount)
        g_mk2_fatality_fighter_idx = kMk2FatalityFighterCount - 1;
    const Mk2FatalityFighterDef &fighter = g_mk2_fatality_fighters[g_mk2_fatality_fighter_idx];
    std::vector<int> fighter_cmds = Mk2FatalityFighterCommandIndices(fighter);
    if (g_mk2_fatality_selected_fatality < 0) g_mk2_fatality_selected_fatality = 0;
    if (g_mk2_fatality_selected_fatality >= (int)fighter_cmds.size())
        g_mk2_fatality_selected_fatality = (int)fighter_cmds.size() - 1;
    if (g_mk2_fatality_selected_fatality < 0) g_mk2_fatality_selected_fatality = 0;
    if (!fighter_cmds.empty() &&
        std::find(fighter_cmds.begin(), fighter_cmds.end(), g_mk2_fatality_command_idx) == fighter_cmds.end())
        g_mk2_fatality_command_idx = fighter_cmds[g_mk2_fatality_selected_fatality];

    ImGui::SetNextItemWidth(210);
    if (ImGui::BeginCombo("Fighter##mk2fatal_fighter", fighter.name)) {
        for (int i = 0; i < kMk2FatalityFighterCount; i++) {
            bool selected = (g_mk2_fatality_fighter_idx == i);
            if (ImGui::Selectable(g_mk2_fatality_fighters[i].name, selected)) {
                g_mk2_fatality_fighter_idx = i;
                g_mk2_fatality_selected_fatality = 0;
                g_mk2_fatality_attacker_anim_idx = 0;
                g_mk2_fatality_victim_anim_idx = 0;
                std::vector<int> new_cmds = Mk2FatalityFighterCommandIndices(g_mk2_fatality_fighters[i]);
                if (!new_cmds.empty())
                    Mk2FatalityApplyFatalityDefaults(g_mk2_fatality_doc.commands[new_cmds[0]],
                                                     g_mk2_fatality_fighters[i]);
                Mk2FatalityStageFighterWorkspace();
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::Button("Load Fighter")) {
        Mk2FatalityStageFighterWorkspace();
    }
    ImGui::SameLine();
    if (g_mk2_fatality_preview_fps < 1.0f) g_mk2_fatality_preview_fps = 1.0f;
    if (g_mk2_fatality_preview_fps > 30.0f) g_mk2_fatality_preview_fps = 30.0f;
    ImGui::SetNextItemWidth(86);
    if (ImGui::InputFloat("FPS##mk2fatal_fps", &g_mk2_fatality_preview_fps, 1.0f, 4.0f, "%.1f")) {
        if (g_mk2_fatality_preview_fps < 1.0f) g_mk2_fatality_preview_fps = 1.0f;
        if (g_mk2_fatality_preview_fps > 30.0f) g_mk2_fatality_preview_fps = 30.0f;
        g_world_marked_state.fps = g_mk2_fatality_preview_fps;
        g_play_speed = g_mk2_fatality_preview_fps;
    }

    const char *fatality_preview = fighter_cmds.empty()
        ? "(none found)"
        : g_mk2_fatality_doc.commands[fighter_cmds[g_mk2_fatality_selected_fatality]].label.c_str();
    ImGui::SetNextItemWidth(260);
    if (ImGui::BeginCombo("Fatality##mk2fatal_pick", fatality_preview)) {
        for (int i = 0; i < (int)fighter_cmds.size(); i++) {
            const mk2fatal::CommandBlock &cmd = g_mk2_fatality_doc.commands[fighter_cmds[i]];
            char label[192];
            snprintf(label, sizeof(label), "%s  %s", cmd.label.c_str(),
                     cmd.routine.empty() ? "" : cmd.routine.c_str());
            bool selected = (g_mk2_fatality_selected_fatality == i);
            if (ImGui::Selectable(label, selected)) {
                g_mk2_fatality_selected_fatality = i;
                Mk2FatalityApplyFatalityDefaults(cmd, fighter);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (!fighter_cmds.empty()) {
        const mk2fatal::CommandBlock &cmd = g_mk2_fatality_doc.commands[fighter_cmds[g_mk2_fatality_selected_fatality]];
        ImGui::SameLine();
        ImGui::TextDisabled("%s  %s",
                            cmd.combo_label.empty() ? "combo?" : cmd.combo_label.c_str(),
                            cmd.range_note.empty() ? "" : cmd.range_note.c_str());
    }

    ImGui::SetNextItemWidth(220);
    const char *attacker_preview = Mk2FatalitySelectedAttackerAnim(fighter);
    if (ImGui::BeginCombo("Attacker Animation##mk2fatal_attacker_anim",
                          attacker_preview ? attacker_preview : "(none)")) {
        for (int i = 0; i < 10 && fighter.fatal_anims[i]; i++) {
            bool selected = (g_mk2_fatality_attacker_anim_idx == i);
            if (ImGui::Selectable(fighter.fatal_anims[i], selected))
                g_mk2_fatality_attacker_anim_idx = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    const char *victim_preview = Mk2FatalitySelectedVictimAnim();
    if (ImGui::BeginCombo("Cage Victim Animation##mk2fatal_victim_anim", victim_preview)) {
        for (int i = 0; g_mk2_fatality_cage_deaths[i]; i++) {
            bool selected = (g_mk2_fatality_victim_anim_idx == i);
            if (ImGui::Selectable(g_mk2_fatality_cage_deaths[i], selected))
                g_mk2_fatality_victim_anim_idx = i;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (fighter_cmds.empty()) ImGui::BeginDisabled();
    if (ImGui::Button("Animate Fatality")) {
        Mk2FatalityStageSelectedFatality();
    }
    if (fighter_cmds.empty()) ImGui::EndDisabled();
    if (!g_mk2_fatality_stage_status.empty())
        ImGui::TextDisabled("%s", g_mk2_fatality_stage_status.c_str());
    ImGui::Separator();

    if (ImGui::BeginTabBar("##mk2fatal_tabs")) {
        if (ImGui::BeginTabItem("Fatalities")) {
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            ImGui::BeginChild("##mk2fatal_cmd_list", ImVec2(300, h), true);
            ImGui::TextDisabled("Command Blocks");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##mk2fatal_filter_cmd", "filter...", g_mk2_fatality_filter, sizeof(g_mk2_fatality_filter));
            for (int ci = 0; ci < (int)fighter_cmds.size(); ci++) {
                int i = fighter_cmds[ci];
                const auto &cmd = g_mk2_fatality_doc.commands[i];
                std::string hay = cmd.label + " " + cmd.routine + " " + cmd.combo_label + " " + cmd.trigger;
                if (!Mk2FatalityFilterMatch(hay, g_mk2_fatality_filter)) continue;
                char label[192];
                snprintf(label, sizeof(label), "%s  %s", cmd.label.c_str(),
                         cmd.routine.empty() ? "(routine?)" : cmd.routine.c_str());
                if (ImGui::Selectable(label, g_mk2_fatality_command_idx == i)) {
                    g_mk2_fatality_command_idx = i;
                    g_mk2_fatality_selected_fatality = ci;
                    Mk2FatalityApplyFatalityDefaults(cmd, fighter);
                    g_mk2_fatality_selected_line = 0;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##mk2fatal_cmd_detail", ImVec2(0, h), true);
            if (fighter_cmds.empty()) {
                ImGui::TextDisabled("No fatality command blocks found for this fighter.");
            } else {
                if (g_mk2_fatality_command_idx < 0 ||
                    g_mk2_fatality_command_idx >= (int)g_mk2_fatality_doc.commands.size())
                    g_mk2_fatality_command_idx = fighter_cmds[0];
                mk2fatal::CommandBlock cmd = g_mk2_fatality_doc.commands[g_mk2_fatality_command_idx];
                ImGui::Text("%s", cmd.label.c_str());
                ImGui::TextDisabled("Routine: %s   Transfer: %s   Finish Him: %s",
                                    cmd.routine.empty() ? "(not detected)" : cmd.routine.c_str(),
                                    cmd.transfer.empty() ? "(not detected)" : cmd.transfer.c_str(),
                                    cmd.finish_him_only ? "yes" : "no");
                ImGui::TextDisabled("Combo: %s   Trigger: %s",
                                    cmd.combo_label.empty() ? "(direct / timing)" : cmd.combo_label.c_str(),
                                    cmd.trigger.empty() ? "(not detected)" : cmd.trigger.c_str());
                if (!cmd.range_note.empty()) ImGui::TextDisabled("%s", cmd.range_note.c_str());
                std::string inferred_anim = Mk2FatalityInferAnimationFromRoutine(cmd.routine);
                if (!inferred_anim.empty()) {
                    if (ImGui::Button("Use Routine Animation")) {
                        g_mk2_fatality_attacker_anim_idx =
                            Mk2FatalityAnimListIndex(fighter.fatal_anims, inferred_anim.c_str());
                        Mk2FatalityStageSelectedFatality();
                    }
                    ImGui::SameLine();
                    ImGui::TextDisabled("%s", inferred_anim.c_str());
                } else {
                    ImGui::TextDisabled("No direct victim animation mapping.");
                }
                DrawMk2FatalityPlanSummary();
                ImGui::Separator();
                if (ImGui::CollapsingHeader("Command Source", ImGuiTreeNodeFlags_DefaultOpen)) {
                    DrawMk2FatalitySourceEditor("##mk2fatal_cmd_src", cmd.file_idx, cmd.start_line, cmd.end_line,
                                                &g_mk2_fatality_selected_line, NULL, 0, false);
                }
                int combo_idx = cmd.combo_label.empty() ? -1 : mk2fatal::find_combo(&g_mk2_fatality_doc, cmd.combo_label.c_str());
                if (combo_idx >= 0) {
                    mk2fatal::ComboBlock combo = g_mk2_fatality_doc.combos[combo_idx];
                    if (ImGui::CollapsingHeader("Controller Combo", ImGuiTreeNodeFlags_DefaultOpen)) {
                        ImGui::TextDisabled("%s   time %s   %d words",
                                            combo.label.c_str(),
                                            combo.time_token.empty() ? "?" : combo.time_token.c_str(),
                                            (int)combo.words.size());
                        DrawMk2FatalitySourceEditor("##mk2fatal_cmd_combo_src", combo.file_idx, combo.start_line, combo.end_line,
                                                    &g_mk2_fatality_selected_line,
                                                    g_mk2_fatality_insert_combo, sizeof(g_mk2_fatality_insert_combo), true);
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Animations")) {
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            ImGui::BeginChild("##mk2fatal_anim_list", ImVec2(320, h), true);
            ImGui::TextDisabled("Animation / Body Blocks");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##mk2fatal_filter_anim", "filter...", g_mk2_fatality_filter, sizeof(g_mk2_fatality_filter));
            ImGui::Checkbox("Body endings only", &g_mk2_fatality_body_only);
            for (int i = 0; i < (int)g_mk2_fatality_doc.animations.size(); i++) {
                const auto &anim = g_mk2_fatality_doc.animations[i];
                if (!Mk2FatalityFilterMatch(anim.file_rel, fighter.source_file)) continue;
                if (g_mk2_fatality_body_only && !anim.body_ending) continue;
                std::string hay = anim.label + " " + anim.file_rel;
                if (!Mk2FatalityFilterMatch(hay, g_mk2_fatality_filter)) continue;
                char label[224];
                snprintf(label, sizeof(label), "%s  [%s]", anim.label.c_str(), anim.file_rel.c_str());
                if (ImGui::Selectable(label, g_mk2_fatality_anim_idx == i)) {
                    g_mk2_fatality_anim_idx = i;
                    g_mk2_fatality_selected_line = 0;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##mk2fatal_anim_detail", ImVec2(0, h), true);
            if (g_mk2_fatality_doc.animations.empty()) {
                ImGui::TextDisabled("No animation blocks found.");
            } else {
                mk2fatal::AnimationBlock anim = g_mk2_fatality_doc.animations[g_mk2_fatality_anim_idx];
                ImGui::Text("%s", anim.label.c_str());
                ImGui::TextDisabled("%s   .long tokens %d   .word tokens %d   adjustxy %d",
                                    anim.file_rel.c_str(), anim.long_count, anim.word_count, anim.adjust_count);
                if (anim.body_ending) {
                    ImGui::SameLine();
                    ImGui::TextDisabled("body ending");
                }
                if (ImGui::Button("Animate With Cage")) {
                    std::vector<std::string> attacker_labels;
                    attacker_labels.push_back(anim.label);
                    mk2fatal::AssetPlan attacker_plan =
                        Mk2FatalityBuildPlanForLabels(attacker_labels, fighter.source_file, fighter.img_files);
                    std::vector<std::string> victim_labels;
                    victim_labels.push_back(Mk2FatalitySelectedVictimAnim());
                    mk2fatal::AssetPlan victim_plan = Mk2FatalityBuildCageDeathPlan(victim_labels);
                    g_mk2_fatality_plan = victim_plan;
                    Mk2FatalityStageDualPlans(fighter, attacker_plan, victim_plan);
                }
                ImGui::SameLine();
                ImGui::TextDisabled("Johnny Cage");
                DrawMk2FatalityPlanSummary();
                ImGui::Separator();
                DrawMk2FatalitySourceEditor("##mk2fatal_anim_src", anim.file_idx, anim.start_line, anim.end_line,
                                            &g_mk2_fatality_selected_line,
                                            g_mk2_fatality_insert_anim, sizeof(g_mk2_fatality_insert_anim), true);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Controller")) {
            const float h = ImGui::GetContentRegionAvail().y - 4.0f;
            ImGui::BeginChild("##mk2fatal_combo_list", ImVec2(300, h), true);
            ImGui::TextDisabled("scom_* Tables");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##mk2fatal_filter_combo", "filter...", g_mk2_fatality_filter, sizeof(g_mk2_fatality_filter));
            for (int i = 0; i < (int)g_mk2_fatality_doc.combos.size(); i++) {
                const auto &combo = g_mk2_fatality_doc.combos[i];
                std::string hay = combo.label + " " + combo.time_token;
                if (!Mk2FatalityFilterMatch(hay, g_mk2_fatality_filter)) continue;
                char label[160];
                snprintf(label, sizeof(label), "%s  (%s)", combo.label.c_str(),
                         combo.time_token.empty() ? "time?" : combo.time_token.c_str());
                if (ImGui::Selectable(label, g_mk2_fatality_combo_idx == i)) {
                    g_mk2_fatality_combo_idx = i;
                    g_mk2_fatality_selected_line = 0;
                }
            }
            ImGui::EndChild();

            ImGui::SameLine();
            ImGui::BeginChild("##mk2fatal_combo_detail", ImVec2(0, h), true);
            if (g_mk2_fatality_doc.combos.empty()) {
                ImGui::TextDisabled("No controller combo tables found.");
            } else {
                mk2fatal::ComboBlock combo = g_mk2_fatality_doc.combos[g_mk2_fatality_combo_idx];
                ImGui::Text("%s", combo.label.c_str());
                ImGui::TextDisabled("time %s   %d words",
                                    combo.time_token.empty() ? "?" : combo.time_token.c_str(),
                                    (int)combo.words.size());
                ImGui::Separator();
                DrawMk2FatalitySourceEditor("##mk2fatal_combo_src", combo.file_idx, combo.start_line, combo.end_line,
                                            &g_mk2_fatality_selected_line,
                                            g_mk2_fatality_insert_combo, sizeof(g_mk2_fatality_insert_combo), true);
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

static void DrawPaletteHistogramDialog(void)
{
    if (g_show_histogram) ImGui::OpenPopup("Palette Histogram");
    if (!ImGui::BeginPopupModal("Palette Histogram", &g_show_histogram, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Images using this palette: %d", g_histogram_img_count);
    ImGui::Text("Max occurrences (excluding index 0): %.0f", g_histogram_max);
    ImGui::Spacing();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = 512.0f;
    float height = 150.0f;

    draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(20, 20, 20, 255));

    float bar_w = width / 256.0f;
    for (int i = 0; i < 256; i++) {
        float val = g_histogram_data[i];
        if (val > 0.0f) {
            float bar_h = (val / g_histogram_max) * height;
            if (bar_h > height) bar_h = height;
            if (bar_h < 1.0f) bar_h = 1.0f;

            ImVec2 p_min = ImVec2(p.x + i * bar_w, p.y + height - bar_h);
            ImVec2 p_max = ImVec2(p.x + (i + 1) * bar_w, p.y + height);

            SDL_Color c = g_palette[i];
            draw_list->AddRectFilled(p_min, p_max, IM_COL32(c.r, c.g, c.b, 255));
        }
    }

    ImGui::Dummy(ImVec2(width, height));

    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        g_show_histogram = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawPaletteReduceSwatches(const unsigned char *data, int numc)
{
    if (!data || numc <= 0) {
        ImGui::TextDisabled("No colors");
        return;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float sw = 13.0f;
    const float gap = 1.0f;
    const int cols = 16;
    int rows = (numc + cols - 1) / cols;
    if (rows < 1) rows = 1;

    for (int i = 0; i < numc; i++) {
        int row = i / cols;
        int col = i % cols;
        ImVec2 a(p.x + col * (sw + gap), p.y + row * (sw + gap));
        ImVec2 b(a.x + sw, a.y + sw);
        unsigned char r, g, bch;
        pal_word_to_rgb8(data + i * 2, &r, &g, &bch);
        dl->AddRectFilled(a, b, IM_COL32(r, g, bch, 255));
        dl->AddRect(a, b, i == 0 ? IM_COL32(95, 95, 95, 200)
                                  : IM_COL32(0, 0, 0, 160));
    }
    ImGui::Dummy(ImVec2(cols * (sw + gap), rows * (sw + gap)));
}

static void DrawPaletteReduceImageColumn(const char *title, SDL_Texture *tex,
                                         IMG *img, float max_w, float max_h)
{
    ImGui::TextUnformatted(title);
    if (!tex || !img || img->w == 0 || img->h == 0) {
        ImGui::TextDisabled("No preview");
        return;
    }

    float scale_x = max_w / (float)img->w;
    float scale_y = max_h / (float)img->h;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale > 8.0f) scale = 8.0f;
    if (scale <= 0.0f) scale = 1.0f;
    ImVec2 sz((float)img->w * scale, (float)img->h * scale);
    ImGui::Image((ImTextureID)(intptr_t)tex, sz);
}

static void DrawPaletteReduceDialog(void)
{
    if (g_show_palette_reduce) {
        ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Appearing);
        ImGui::OpenPopup("Downscale Palette");
    }
    if (!ImGui::BeginPopupModal("Downscale Palette", &g_show_palette_reduce,
                                ImGuiWindowFlags_NoSavedSettings)) {
        if (!g_show_palette_reduce) ClearPaletteReducePreviewTextures();
        return;
    }

    int pal_idx = g_doc->plselected;
    PAL *pal = (pal_idx >= 0) ? get_pal(pal_idx) : NULL;
    if (!pal || !pal->data_p) {
        ImGui::TextDisabled("No palette selected");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_palette_reduce = false;
            ClearPaletteReducePreviewTextures();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    PaletteReductionPlan plan = {};
    BuildPaletteReductionPlan(pal_idx, g_palette_reduce_bpp, &plan);

    ImGui::Text("%.9s", pal->n_s);
    ImGui::SameLine();
    ImGui::TextDisabled("%u colors, %u bpp", pal->numc, pal->bitspix);

    bool changed_bpp = false;
    for (int bpp = 8; bpp >= 4; bpp--) {
        char label[32];
        snprintf(label, sizeof(label), "%d bpp (%d)", bpp, 1 << bpp);
        if (bpp != 8) ImGui::SameLine();
        if (ImGui::RadioButton(label, g_palette_reduce_bpp == bpp)) {
            g_palette_reduce_bpp = bpp;
            changed_bpp = true;
        }
    }
    if (changed_bpp) {
        ClearPaletteReducePreviewTextures();
        BuildPaletteReductionPlan(pal_idx, g_palette_reduce_bpp, &plan);
    }

    if (!plan.valid) {
        ImGui::TextDisabled("%s", plan.error[0] ? plan.error : "Unable to build reduction preview.");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_palette_reduce = false;
            ClearPaletteReducePreviewTextures();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    const char *reduce_mode = plan.quantized ? "weighted merge"
                            : (plan.old_numc == plan.new_numc ? "unchanged"
                                                               : "exact pack");
    ImGui::TextDisabled("Target max: %d colors. Active: %d opaque. Result: %d colors. Mode: %s.",
                        plan.target_numc, plan.active_colors,
                        plan.new_numc, reduce_mode);
    ImGui::TextDisabled("Affected: %d image%s, %d px remapped.",
                        plan.images, plan.images == 1 ? "" : "s",
                        plan.pixels_changed);
    ImGui::Separator();

    g_palette_reduce_preview_idx = FindPalettePreviewImage(pal_idx, g_palette_reduce_preview_idx);
    IMG *preview_img = get_img(g_palette_reduce_preview_idx);
    if (preview_img) {
        if (ImGui::SmallButton("<##pal_reduce_prev")) {
            g_palette_reduce_preview_idx =
                StepPalettePreviewImage(pal_idx, g_palette_reduce_preview_idx, -1);
            ClearPaletteReducePreviewTextures();
            preview_img = get_img(g_palette_reduce_preview_idx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(">##pal_reduce_next")) {
            g_palette_reduce_preview_idx =
                StepPalettePreviewImage(pal_idx, g_palette_reduce_preview_idx, 1);
            ClearPaletteReducePreviewTextures();
            preview_img = get_img(g_palette_reduce_preview_idx);
        }
        ImGui::SameLine();
        std::string pname = img_name_string(preview_img);
        ImGui::TextDisabled("[%d] %s", g_palette_reduce_preview_idx, pname.c_str());

        RebuildPaletteReducePreviewTextures(plan);
        float max_w = 330.0f;
        float max_h = 320.0f;
        if (ImGui::BeginTable("##pal_reduce_preview_table", 2,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            DrawPaletteReduceImageColumn("Current", g_palette_reduce_orig_tex, preview_img, max_w, max_h);
            ImGui::TableNextColumn();
            DrawPaletteReduceImageColumn("Downscaled", g_palette_reduce_new_tex, preview_img, max_w, max_h);
            ImGui::EndTable();
        }
    } else {
        if (ImGui::BeginTable("##pal_reduce_swatch_table", 2,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Current");
            DrawPaletteReduceSwatches((const unsigned char *)pal->data_p, plan.old_numc);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Downscaled");
            DrawPaletteReduceSwatches(plan.data, plan.new_numc);
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    bool can_apply = plan.valid &&
        (plan.old_numc != plan.new_numc ||
         plan.pixels_changed > 0 ||
         (int)pal->bitspix != plan.target_bpp);
    ImGui::BeginDisabled(!can_apply);
    if (ImGui::Button("OK", ImVec2(100, 0))) {
        commit_palette_adjustments();
        PaletteReductionPlan apply_plan = {};
        if (BuildPaletteReductionPlan(pal_idx, g_palette_reduce_bpp, &apply_plan) &&
            ApplyPaletteReductionPlan(apply_plan)) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Downscaled palette %.9s to %d bpp: %d -> %d colors, %d px remapped.",
                     pal->n_s, apply_plan.target_bpp, apply_plan.old_numc,
                     apply_plan.new_numc, apply_plan.pixels_changed);
            g_restore_msg_timer = 5.0f;
        }
        g_show_palette_reduce = false;
        ClearPaletteReducePreviewTextures();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_palette_reduce = false;
        ClearPaletteReducePreviewTextures();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    if (!g_show_palette_reduce) ClearPaletteReducePreviewTextures();
}

static void DrawAutoSplitSummaryLine(const char *label,
                                     const AutoSplitTargetSummary &summary)
{
    if (summary.split_count <= 0) {
        ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.30f, 1.0f),
                           "%s: no legal split", label);
        return;
    }

    long long delta = summary.src_zcom_bits - summary.split_zcom_bits;
    double pct = summary.src_zcom_bits > 0
        ? (double)delta * 100.0 / (double)summary.src_zcom_bits
        : 0.0;
    ImVec4 col = delta >= 0
        ? ImVec4(0.42f, 0.90f, 0.55f, 1.0f)
        : ImVec4(1.0f, 0.66f, 0.30f, 1.0f);
    ImGui::TextColored(col, "%s: %lld -> %lld bits (%+.1f%%)",
                       label, summary.src_zcom_bits,
                       summary.split_zcom_bits, pct);
    if (summary.selected_preview.best_split_valid &&
        summary.selected_preview.target_count > 0) {
        ImGui::SameLine();
        ImGui::TextDisabled("selected cut %s=%d",
                            summary.selected_preview.best_split_vertical ? "x" : "y",
                            summary.selected_preview.best_split_pos);
    }
    ImGui::TextDisabled("%d split-ready target%s, %d skipped below/empty threshold",
                        summary.split_count,
                        summary.split_count == 1 ? "" : "s",
                        summary.skipped_count);
}

static void DrawAutoChopDialog(void)
{
    if (g_show_auto_chop) ImGui::OpenPopup("Break into Subframes");
    if (!ImGui::BeginPopupModal("Break into Subframes", &g_show_auto_chop, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("Breaks marked sprites, or the selected sprite if none are marked,\n"
                       "into Midway-style A/B pieces and recalculates ANIX/ANIY.");
    ImGui::Spacing();

    AutoSplitTargetSummary horizontal_summary;
    AutoSplitTargetSummary vertical_summary;
    BuildAutoSplitTargetSummary(false, &horizontal_summary);
    BuildAutoSplitTargetSummary(true, &vertical_summary);

    ImGui::RadioButton("Best Horizontal Cut", &g_chop_mode,
                       AutoChopMode_BestHorizontal);
    ImGui::SameLine();
    ImGui::RadioButton("Best Vertical Cut", &g_chop_mode,
                       AutoChopMode_BestVertical);
    ImGui::SameLine();
    ImGui::RadioButton("Manual Grid", &g_chop_mode, AutoChopMode_ManualGrid);
    ImGui::Checkbox("Trim empty space (Highly recommended)", &g_chop_trim);

    ImGui::Spacing();
    ImGui::TextDisabled("Best cuts require both sides to be greater than %dpx.",
                        k_auto_split_min_side);
    DrawAutoSplitSummaryLine("Horizontal", horizontal_summary);
    DrawAutoSplitSummaryLine("Vertical", vertical_summary);

    if (g_chop_mode == AutoChopMode_ManualGrid) {
        ImGui::Spacing();
        if (ImGui::Button("Auto 3 Subframes", ImVec2(140, 0))) AutoChopSetThreeBandSize();
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("Piece Width", &g_chop_w)) { if (g_chop_w < 1) g_chop_w = 1; }
        ImGui::SetNextItemWidth(100);
        if (ImGui::InputInt("Piece Height", &g_chop_h)) { if (g_chop_h < 1) g_chop_h = 1; }

        AutoChopPreview summary;
        BuildAutoChopTargetSummary(&summary);
        if (summary.target_count > 0) {
            if (summary.pieces.empty()) {
                ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.30f, 1.0f),
                                   "LOAD2 ZCOM: no non-empty pieces");
            } else {
                long long delta = summary.src_zcom_bits - summary.split_zcom_bits;
                double pct = summary.src_zcom_bits > 0
                    ? (double)delta * 100.0 / (double)summary.src_zcom_bits
                    : 0.0;
                ImVec4 col = delta >= 0
                    ? ImVec4(0.42f, 0.90f, 0.55f, 1.0f)
                    : ImVec4(1.0f, 0.66f, 0.30f, 1.0f);
                ImGui::TextColored(col, "Manual grid LOAD2 ZCOM: %lld -> %lld bits (%+.1f%%)",
                                   summary.src_zcom_bits, summary.split_zcom_bits, pct);
            }
            char bpp_buf[32];
            if (summary.bpp > 0) snprintf(bpp_buf, sizeof(bpp_buf), "%d bpp", summary.bpp);
            else snprintf(bpp_buf, sizeof(bpp_buf), "mixed bpp");
            ImGui::TextDisabled("%d target%s, %d piece%s, %d empty cell%s skipped, %s",
                                summary.target_count,
                                summary.target_count == 1 ? "" : "s",
                                (int)summary.pieces.size(),
                                summary.pieces.size() == 1 ? "" : "s",
                                summary.empty_cells,
                                summary.empty_cells == 1 ? "" : "s",
                                bpp_buf);
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool best_horizontal = (g_chop_mode == AutoChopMode_BestHorizontal);
    bool best_vertical = (g_chop_mode == AutoChopMode_BestVertical);
    bool can_best = best_horizontal ? (horizontal_summary.split_count > 0)
                                    : (vertical_summary.split_count > 0);
    if (g_chop_mode == AutoChopMode_ManualGrid) can_best = true;
    ImGui::BeginDisabled(!can_best);
    if (ImGui::Button("Break", ImVec2(100, 0))) {
        int count = 0;
        if (best_horizontal || best_vertical)
            count = ApplyBestAutoSplitToTargets(best_vertical);
        else
            count = ChopMarkedImages(g_chop_w, g_chop_h, g_chop_trim);
        if (count > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "Broke into %d subframe piece(s).", count);
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "No pieces generated (mark or select a sprite).");
        }
        g_restore_msg_timer = 4.0f;
        g_show_auto_chop = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_auto_chop = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawBulkRestoreRegexDialog(void)
{
    if (g_show_restore_regex) ImGui::OpenPopup("Bulk Restore via Regex");
    if (!ImGui::BeginPopupModal("Bulk Restore via Regex", &g_show_restore_regex, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::TextWrapped("Uses a regex to map child names to parent names across the entire file.\n"
                       "Capture group 1 (\\1) is used as the parent name.\n"
                       "Example: ^(.+)[A-Z]$ maps JCJUMPFLIP1A -> JCJUMPFLIP1");
    ImGui::Spacing();

    ImGui::Text("Mode:");
    ImGui::SameLine();
    ImGui::RadioButton("Diff (preserve hand-tuning)", &g_restore_diff_mode, 1);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only the pixels you EDITED on the master are\n"
                          "propagated into children. Every untouched\n"
                          "pixel in each child stays as-is.\n"
                          "(Right choice for adding a logo, edge tweak, etc.)");
    ImGui::SameLine();
    ImGui::RadioButton("Replace (overwrite child bbox)", &g_restore_diff_mode, 0);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Wipes each child to zero, then fills its bbox\n"
                          "with parent pixels. Clobbers hand-tuned\n"
                          "per-piece details.");
    ImGui::SameLine();
    ImGui::RadioButton("Reconstruct from Parent", &g_restore_diff_mode, 2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Treats parent as ground truth. For every child\n"
                          "pixel that diverges from the parent (after\n"
                          "anipoint-relative shift), copies the parent's\n"
                          "value into the child.\n"
                          "Use to restore censored / blacked-out regions\n"
                          "where the master still has the original detail.");
    ImGui::Spacing();

    bool pattern_changed = ImGui::InputText("Regex Pattern", g_restore_regex_buf, sizeof(g_restore_regex_buf));
    if (pattern_changed) {
        g_restore_regex_tested = false;
        g_restore_regex_error = false;
        g_restore_matches.clear();
    }

    if (!g_restore_regex_tested) {
        if (ImGui::Button("Preview Matches", ImVec2(120, 0))) {
            g_restore_matches.clear();
            g_restore_regex_error = false;
            std::regex re;
            try {
                re = std::regex(g_restore_regex_buf);
                for (IMG *child = (IMG *)g_doc->img_p; child; child = (IMG *)child->nxt_p) {
                    if (!child->data_p || child->w == 0 || child->h == 0) continue;
                    std::string name(child->n_s);
                    std::smatch match;
                    if (std::regex_match(name, match, re) && match.size() > 1) {
                        std::string parent_name = match[1].str();
                        IMG *parent = NULL;
                        for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
                            if (parent_name == p->n_s) {
                                parent = p;
                                break;
                            }
                        }
                        if (parent && parent->data_p && parent->w > 0 && parent->h > 0 && parent != child
                            && parent->palnum == child->palnum) {
                            g_restore_matches.push_back({child, parent, true, 0, 0});
                        }
                    }
                }
                std::sort(g_restore_matches.begin(), g_restore_matches.end(), [](const BulkRestoreMatch& a, const BulkRestoreMatch& b) {
                    int cmp = strcmp(a.parent->n_s, b.parent->n_s);
                    if (cmp != 0) return cmp < 0;
                    return strcmp(a.child->n_s, b.child->n_s) < 0;
                });
                ComputeBulkRestoreCoverage(g_restore_matches);
                g_restore_regex_tested = true;
            } catch (const std::regex_error&) {
                g_restore_regex_error = true;
            }
        }
        if (g_restore_regex_error) {
            ImGui::TextColored(ImVec4(1, 0, 0, 1), "Regex Error: invalid pattern");
        }
    } else {
        int partial_count = 0;
        for (auto& m : g_restore_matches) {
            if (m.total_pixels > 0 && m.covered_pixels < m.total_pixels) partial_count++;
        }
        ImGui::Text("Found %d match(es). Select items to restore:", (int)g_restore_matches.size());
        if (partial_count > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "%d match%s with partial coverage (anipoint shift pushes parent rect "
                "out of bounds). Pairs mode will zero-fill the uncovered area.",
                partial_count, partial_count == 1 ? "" : "es");
        }
        ImGui::BeginChild("MatchesList", ImVec2(520, 220), true);
        std::string last_parent = "";
        for (size_t i = 0; i < g_restore_matches.size(); i++) {
            BulkRestoreMatch& m = g_restore_matches[i];
            std::string current_parent = m.parent->n_s;
            if (current_parent != last_parent) {
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "%s", current_parent.c_str());
                last_parent = current_parent;
            }
            ImGui::Indent(16.0f);
            char label[128];
            snprintf(label, sizeof(label), "%s##%zu", m.child->n_s, i);
            bool partial = m.total_pixels > 0 && m.covered_pixels < m.total_pixels;
            if (partial) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.55f, 0.55f, 1.0f));
            ImGui::Checkbox(label, &m.selected);
            if (partial) ImGui::PopStyleColor();
            if (m.total_pixels > 0) {
                ImGui::SameLine();
                int pct = (int)((100.0 * m.covered_pixels) / m.total_pixels + 0.5);
                if (partial) {
                    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.55f, 1.0f),
                                       "(%d%% covered)", pct);
                } else {
                    ImGui::TextDisabled("(100%%)");
                }
            }
            ImGui::Unindent(16.0f);
        }
        ImGui::EndChild();

        if (ImGui::Button("Select All")) {
            for (auto& m : g_restore_matches) m.selected = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Deselect All")) {
            for (auto& m : g_restore_matches) m.selected = false;
        }
        ImGui::SameLine();
        if (ImGui::Button("Deselect Partial")) {
            for (auto& m : g_restore_matches) {
                if (m.total_pixels > 0 && m.covered_pixels < m.total_pixels) m.selected = false;
            }
        }
    }

    ImGui::Spacing();
    ImGui::Separator();

    ImGui::BeginDisabled(!g_restore_regex_tested || g_restore_matches.empty());
    if (ImGui::Button("Start Restore", ImVec2(120, 0))) {
        int n = 0;
        const char *verb_done = "Restored";
        const char *verb_zero = "restored";
        switch (g_restore_diff_mode) {
            case 1:
                n = ExecuteBulkRestoreDiff(g_restore_matches);
                verb_done = "Diff-restored"; verb_zero = "diffed";
                break;
            case 2:
                n = ExecuteBulkRestoreReconstruct(g_restore_matches);
                verb_done = "Reconstructed"; verb_zero = "reconstructed";
                break;
            default:
                n = ExecuteBulkRestorePairs(g_restore_matches);
                break;
        }
        if (n > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "%s %d child image(s) from their parents.",
                     verb_done, n);
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "0 images %s.", verb_zero);
        }
        g_restore_msg_timer = 6.0f;

        g_show_restore_regex = false;
        g_restore_regex_tested = false;
        g_restore_matches.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_restore_regex = false;
        g_restore_regex_tested = false;
        g_restore_matches.clear();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawDeleteImagesConfirm(void)
{
    if (g_show_delete_images_confirm) ImGui::OpenPopup("Delete Sprite Subframes");
    if (!ImGui::BeginPopupModal("Delete Sprite Subframes", &g_show_delete_images_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    std::vector<int> base = g_pending_delete_base_indices;
    std::vector<int> extra = g_pending_delete_subframe_indices;
    NormalizeImageDeleteIndices(&base);
    NormalizeImageDeleteIndices(&extra);

    int base_count = (int)base.size();
    int extra_count = (int)extra.size();
    bool bulk = base_count > 1 || g_pending_delete_parent_name[0] == '\0';

    if (bulk) {
        ImGui::TextWrapped("Delete %d marked sprite%s?", base_count,
                           base_count == 1 ? "" : "s");
        ImGui::TextWrapped("%d subframe%s belong to marked parent sprite%s.",
                           extra_count,
                           extra_count == 1 ? "" : "s",
                           base_count == 1 ? "" : "s");
    } else {
        ImGui::TextWrapped("\"%s\" has %d subframe%s.",
                           g_pending_delete_parent_name,
                           extra_count,
                           extra_count == 1 ? "" : "s");
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    const char *base_label = bulk ? "Delete Marked Only" : "Delete Parent Only";
    if (ImGui::Button(base_label, ImVec2(150, 0))) {
        int deleted = DeleteImagesByIndices(base);
        if (g_last_delete_removed_palettes > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s and %d now-unused palette%s.",
                     deleted, deleted == 1 ? "" : "s",
                     g_last_delete_removed_palettes,
                     g_last_delete_removed_palettes == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s.", deleted, deleted == 1 ? "" : "s");
        }
        g_restore_msg_timer = 4.0f;
        ClearPendingImageDelete();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();

    std::vector<int> all = base;
    all.insert(all.end(), extra.begin(), extra.end());
    const char *all_label = bulk ? "Delete Marked + Subframes" : "Delete Parent + Subframes";
    if (ImGui::Button(all_label, ImVec2(210, 0))) {
        int deleted = DeleteImagesByIndices(all);
        if (g_last_delete_removed_palettes > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s and %d now-unused palette%s.",
                     deleted, deleted == 1 ? "" : "s",
                     g_last_delete_removed_palettes,
                     g_last_delete_removed_palettes == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d sprite%s.", deleted, deleted == 1 ? "" : "s");
        }
        g_restore_msg_timer = 4.0f;
        ClearPendingImageDelete();
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();

    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        ClearPendingImageDelete();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

static void DrawDebugInfoModal(void)
{
    if (g_show_debug) ImGui::OpenPopup("Debug Info");
    if (!ImGui::BeginPopupModal("Debug Info", &g_show_debug, ImGuiWindowFlags_NoMove)) return;
    ImGui::SetNextWindowSize(ImVec2(520, 580), ImGuiCond_Always);

    if (ImGui::CollapsingHeader("LIB_HDR", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("IMGCNT:  %u",     g_doc->imgcnt);
        ImGui::Text("PALCNT:  %u",     g_doc->palcnt);
        ImGui::Text("SEQCNT:  %u",     g_doc->seqcnt);
        ImGui::Text("SCRCNT:  %u",     g_doc->scrcnt);
        ImGui::Text("DAMCNT:  %u",     g_doc->damcnt);
        ImGui::Text("VERSION: 0x%04X", g_doc->fileversion);
        ImGui::Separator();
        ImGui::TextDisabled("SEQSCR/ENTRY blob (load-time, round-trips on save):");
        if (g_doc->scrseqmem_p && g_doc->scrseqbytes > 0) {
            ImGui::Text("SCRSEQBYTES:  %u bytes", g_doc->scrseqbytes);
        } else {
            ImGui::TextDisabled("SCRSEQBYTES:  0  (no seq/scr in file)");
        }
    }

    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (ImGui::CollapsingHeader("IMAGE (runtime)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (img) {
            ImGui::Text("NXT_p:    %p",       img->nxt_p);
            ImGui::Text("N_s:      %.15s",    img->n_s);
            ImGui::Text("FLAGS:    0x%04X",   (int)img->flags);
            ImGui::Text("ANIX:     %d",       (int)(short)img->anix);
            ImGui::Text("ANIY:     %d",       (int)(short)img->aniy);
            ImGui::Text("W:        %d",       (int)img->w);
            ImGui::Text("H:        %d",       (int)img->h);
            ImGui::Text("PALNUM:   %d",       (int)img->palnum);
            ImGui::Text("DATA_p:   %p",       img->data_p);
            if (img->pttbl_p) ImGui::Text("PTTBL_p:  %p", img->pttbl_p);
            else ImGui::TextDisabled("PTTBL_p:  NULL");
            ImGui::Text("ANIX2:    %d",       (int)(short)img->anix2);
            ImGui::Text("ANIY2:    %d",       (int)(short)img->aniy2);
            ImGui::Text("ANIZ2:    %d",       (int)(short)img->aniz2);
            ImGui::Text("OPALS:    0x%04X",   (int)img->opals);
            ImGui::Text("TEMP:     %p",       img->temp);
        } else {
            ImGui::TextDisabled("No image selected");
        }
    }
    if (ImGui::CollapsingHeader("IMAGE_disk (load-time)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (img) {
            ImGui::Text("FILE_OSET:    0x%X (%u)", img->file_oset, img->file_oset);
            ImGui::Text("FILE_LIB:     %u",        (unsigned)img->file_lib);
            ImGui::Text("FILE_FRM:     %u",        (unsigned)img->file_frm);
            if (img->file_pttblnum == 0xFFFF)
                ImGui::TextDisabled("FILE_PTTBLNUM: 0xFFFF (none)");
            else
                ImGui::Text("FILE_PTTBLNUM: %u",  (unsigned)img->file_pttblnum);
            if (!(img->flags & 0x0080)) {
                unsigned int stride = ((unsigned int)img->w + 3u) & ~3u;
                ImGui::Text("PIX_SIZE:     %u bytes (uncompressed, %ux%u)",
                            stride * (unsigned)img->h,
                            (unsigned)img->w, (unsigned)img->h);
            } else {
                ImGui::TextDisabled("PIX_SIZE:     CMP — variable per row");
            }
        } else {
            ImGui::TextDisabled("No image selected");
        }
    }

    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (ImGui::CollapsingHeader("PALETTE (runtime)", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (pal) {
            ImGui::Text("NXT_p:    %p",       pal->nxt_p);
            ImGui::Text("N_s:      %.9s",     pal->n_s);
            ImGui::Text("FLAGS:    0x%02X",   pal->flags);
            ImGui::Text("BITSPIX:  %u",       pal->bitspix);
            ImGui::Text("NUMC:     %u",       pal->numc);
            ImGui::Text("PAD:      0x%04X",   pal->pad);
            ImGui::Text("DATA_p:   %p",       pal->data_p);
            ImGui::Text("TEMP:     %p",       pal->temp);
            ImGui::Separator();
            ImGui::TextDisabled("PALETTE_disk fields (lib/colind/cmap/oset)");
            ImGui::TextDisabled("are not currently retained at load.");
        } else {
            ImGui::TextDisabled("No palette selected");
        }
    }

    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        g_show_debug = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F9 to toggle");
    ImGui::EndPopup();
}

static void DrawNewImgConfirm(void)
{
    if (g_show_new_img_confirm) ImGui::OpenPopup("New IMG");
    if (!ImGui::BeginPopupModal("New IMG", &g_show_new_img_confirm, ImGuiWindowFlags_AlwaysAutoResize)) return;
    ImGui::Text("Create a fresh IMG tab?");
    ImGui::Text("Starts with one blank palette and one 32x32 image.");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("New", ImVec2(80, 0))) {
        PrepareDocumentForOpenedFile();
        g_doc->fileversion = 0x0634;
        g_doc->fname_s[0]  = 0;
        /* Bootstrap: a fresh doc with zero palettes/images is unusable —
           the user can't paint, can't import a TGA target, can't even
           see the editor properly. Seed one default palette and one
           blank image so the UI is immediately functional. AddNewPalette
           handles undo+dirty internally; clear those again so the new
           doc starts pristine. */
        AddNewPalette();
        AddNewBlankImage();
        g_dirty = false;
        ResetPerDocumentUiState(false);
        g_show_new_img_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_new_img_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* "Add new blank image" — small modal that prompts for width and height
   before allocating, replacing the previous fixed 32x32 path. */
static void DrawNewBlankImageDialog(void)
{
    if (g_show_new_blank_dialog) ImGui::OpenPopup("Add Blank Image");
    if (!ImGui::BeginPopupModal("Add Blank Image", &g_show_new_blank_dialog,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Pick the size of the new image.");
    ImGui::Spacing();
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Width",  &g_new_blank_w)) {
        if (g_new_blank_w < 1)    g_new_blank_w = 1;
        if (g_new_blank_w > 1024) g_new_blank_w = 1024;
    }
    ImGui::SetNextItemWidth(120);
    if (ImGui::InputInt("Height", &g_new_blank_h)) {
        if (g_new_blank_h < 1)    g_new_blank_h = 1;
        if (g_new_blank_h > 1024) g_new_blank_h = 1024;
    }
    ImGui::Spacing();
    ImGui::Separator();
    /* Enter commits, Esc cancels — matches the rest of the modals. */
    bool commit = ImGui::Button("Add", ImVec2(80, 0))
               || ImGui::IsKeyPressed(ImGuiKey_Enter);
    if (commit) {
        AddNewBlankImage(g_new_blank_w, g_new_blank_h);
        g_show_new_blank_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_new_blank_dialog = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* Run whatever action queued the unsaved-changes confirm. Called once the
   user has chosen Save or Discard. After running, g_pending_action is reset
   to None so the dialog never re-fires. */
static void RunPendingAction(void)
{
    PendingAction act = g_pending_action;
    std::string   path = g_pending_action_path;
    int           tab_idx = g_pending_tab_index;
    g_pending_action = PendingAction::None;
    g_pending_action_path.clear();
    g_pending_tab_index = -1;
    switch (act) {
        case PendingAction::Quit: {
            int dirty_idx = FindDirtyDocumentIndex();
            if (dirty_idx >= 0) {
                ActivateDocumentTab(dirty_idx);
                g_pending_action = PendingAction::Quit;
                g_show_unsaved_confirm = true;
            }
            break;
        }
        case PendingAction::OpenDialog:     OpenFileDialog(FileDialogMode::OpenImg); break;
        case PendingAction::OpenPath:       OpenImgFile(path); break;
        case PendingAction::OpenLodDialog:  OpenFileDialog(FileDialogMode::OpenLod); break;
        case PendingAction::CloseTab:
            if (tab_idx < 0) tab_idx = document_active_index();
            document_close_tab(tab_idx);
            ResetPerDocumentUiState(false);
            g_doc_tab_select_request = document_active_index();
            break;
        case PendingAction::None: default:  break;
    }
}

static void DrawUnsavedChangesConfirm(void)
{
    /* Legacy: g_pending_quit is set by Esc/window-close; treat it as the
       Quit pending action if nothing else queued. */
    if (g_pending_quit && g_pending_action == PendingAction::None && !g_show_unsaved_confirm) {
        int dirty_idx = FindDirtyDocumentIndex();
        if (dirty_idx >= 0) {
            ActivateDocumentTab(dirty_idx);
            g_pending_action = PendingAction::Quit;
            g_show_unsaved_confirm = true;
        }
    }
    if (g_show_unsaved_confirm) ImGui::OpenPopup("Unsaved Changes");
    if (!ImGui::BeginPopupModal("Unsaved Changes", &g_show_unsaved_confirm, ImGuiWindowFlags_AlwaysAutoResize)) return;

    const char *verb =
        (g_pending_action == PendingAction::OpenDialog ||
         g_pending_action == PendingAction::OpenPath  ||
         g_pending_action == PendingAction::OpenLodDialog) ? "before opening another file"
        : (g_pending_action == PendingAction::CloseTab) ? "before closing this tab"
                                                        : "before quitting";
    ImGui::Text("You have unsaved changes.");
    ImGui::Text("Do you want to save %s?", verb);
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        g_show_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
        if (g_doc->fname_s[0] != '\0') {
            SaveImgFile();
            g_dirty = false;
            RunPendingAction();
        } else {
            /* No filename yet — fall through to Save dialog, and discard the
               pending action since the user needs to drive that flow manually.
               (Avoids racing a fresh Save dialog against an Open dialog.) */
            g_pending_quit = false;
            g_pending_action = PendingAction::None;
            g_pending_action_path.clear();
            g_pending_tab_index = -1;
            OpenFileDialog(FileDialogMode::SaveImg);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(80, 0))) {
        g_show_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
        g_dirty = false; /* user chose to throw the edits away */
        RunPendingAction();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_unsaved_confirm = false;
        if (g_pending_action == PendingAction::Quit) g_pending_quit = false;
        g_pending_action = PendingAction::None;
        g_pending_action_path.clear();
        g_pending_tab_index = -1;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* MK2-side unsaved-changes confirm. Fires when the user tries to quit
   with edits pending in MKSTK.ASM. Independent of the IMG dirty flow
   so the two doc types each get their own prompt. */
static void DrawMk2UnsavedChangesConfirm(void)
{
    if (g_show_mk2_unsaved_confirm) ImGui::OpenPopup("MK2 Hitboxes - Unsaved");
    if (!ImGui::BeginPopupModal("MK2 Hitboxes - Unsaved", &g_show_mk2_unsaved_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("You have unsaved edits in MKSTK.ASM.");
    ImGui::Text("Do you want to save them before quitting?");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        std::string err;
        if (mk2::save(&g_mk2_doc, &err)) {
            g_mk2_status = "Saved MKSTK.ASM";
            g_mk2_status_sticky = false;
            g_show_mk2_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
        } else {
            g_mk2_status = std::string("Save failed: ") + err;
            g_mk2_status_sticky = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(80, 0))) {
        g_mk2_doc.dirty = false; /* user chose to throw the MK2 edits away */
        g_show_mk2_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_mk2_unsaved_confirm = false;
        g_pending_quit = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawMk2FatalityUnsavedChangesConfirm(void)
{
    if (g_show_mk2_fatality_unsaved_confirm) ImGui::OpenPopup("MK2 Fatality Lab - Unsaved");
    if (!ImGui::BeginPopupModal("MK2 Fatality Lab - Unsaved", &g_show_mk2_fatality_unsaved_confirm,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("You have unsaved edits in MK2 fatality source files.");
    ImGui::Text("Do you want to save them before quitting?");
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Save", ImVec2(80, 0))) {
        std::string err;
        if (mk2fatal::save(&g_mk2_fatality_doc, &err)) {
            g_mk2_fatality_status = "Saved MK2 fatality source edits";
            g_mk2_fatality_status_sticky = false;
            g_show_mk2_fatality_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
        } else {
            g_mk2_fatality_status = std::string("Save failed: ") + err;
            g_mk2_fatality_status_sticky = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Discard", ImVec2(80, 0))) {
        g_mk2_fatality_doc.dirty = false;
        for (auto &sf : g_mk2_fatality_doc.files) sf.dirty = false;
        g_show_mk2_fatality_unsaved_confirm = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(80, 0))) {
        g_show_mk2_fatality_unsaved_confirm = false;
        g_pending_quit = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawHelpModal(void)
{
    if (g_show_help) ImGui::OpenPopup("Help");
    if (!ImGui::BeginPopupModal("Help", &g_show_help,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) return;
    ImGui::SetNextWindowSize(ImVec2(700, 500), ImGuiCond_Always);
    if (ImGui::BeginChild("##helpscroll", ImVec2(680, 420), true)) {
        ImGui::TextUnformatted(g_help_text);
        ImGui::EndChild();
    }
    ImGui::Spacing();
    ImGui::Separator();
    if (ImGui::Button("Close", ImVec2(120, 0))) {
        g_show_help = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void DrawAboutModal(void)
{
    if (g_show_about) ImGui::OpenPopup("About IMGTOOL");
    if (ImGui::BeginPopupModal("About IMGTOOL", &g_show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        /* Headline: name + version in a slightly larger font weight. */
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.2f, 1.0f));
        ImGui::Text("IMGTOOL  v%s", IMGTOOL_VERSION);
        ImGui::PopStyleColor();
        ImGui::Separator();

        ImGui::TextWrapped("A modern port of the 1992 Midway Image Tool. "
                           "Sprite + palette editor for the .IMG container files "
                           "shipped with Mortal Kombat, NBA Jam, NBA Hangtime, "
                           "and other Williams/Midway arcade titles of the era.");
        ImGui::Spacing();

        /* Two-column key/value table so the values line up regardless of
           the proportional-font widths of the labels. ImGui::Text uses a
           variable-width font, so space-padding inside the format string
           can't be relied on for alignment. */
        SDL_version sdlv;
        SDL_GetVersion(&sdlv);
        if (ImGui::BeginTable("##about_kv", 2,
                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_NoBordersInBody)) {
            auto row = [](const char *k, const char *fmt, ...) {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextUnformatted(k);
                ImGui::TableSetColumnIndex(1);
                va_list ap; va_start(ap, fmt);
                char buf[256];
                vsnprintf(buf, sizeof(buf), fmt, ap);
                va_end(ap);
                ImGui::TextUnformatted(buf);
            };
            row("Version",    "%s",          IMGTOOL_VERSION);
            row("Built",      "%s %s",       __DATE__, __TIME__);
#ifdef IMGTOOL_GIT_REV
            row("Commit",     "%s",          IMGTOOL_GIT_REV);
#endif
            row("Dear ImGui", "%s",          IMGUI_VERSION);
            row("SDL2",       "%d.%d.%d",    sdlv.major, sdlv.minor, sdlv.patch);
            ImGui::EndTable();
        }

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextDisabled("Original tool: Shawn Liptak, Williams Electronics, 1992");
        ImGui::TextDisabled("SDL/ImGui modernization & feature work: see git history");

        ImGui::Spacing();
        ImGui::TextLinkOpenURL("https://github.com/junkwax/midway-imgtool");
        ImGui::SameLine();
        ImGui::TextDisabled(" (issues + releases)");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            g_show_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

static void DrawVerboseLogWindow(void)
{
    if (!g_verbose) return;
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Verbose Logging", &g_verbose)) {
        if (ImGui::Button("Clear")) { g_log_lines.clear(); }
        ImGui::SameLine();
        if (ImGui::Button("Copy to Clipboard")) {
            std::string all_logs;
            for (const auto& s : g_log_lines) all_logs += s + "\n";
            ImGui::SetClipboardText(all_logs.c_str());
        }
        ImGui::Separator();
        ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);
        for (const auto& s : g_log_lines) {
            ImGui::TextUnformatted(s.c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY()) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
    ImGui::End();
}

static void DrawTransientToast(float dt)
{
    if (g_restore_msg_timer <= 0.0f) return;
    g_restore_msg_timer -= dt;
    ImGuiIO &io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::SetNextWindowPos(ImVec2(sw * 0.5f, sh - 60), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
    if (ImGui::Begin("##toast", NULL,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing)) {
        ImGui::TextUnformatted(g_restore_msg);
    }
    ImGui::End();
}

/* Unified Undo/Redo helpers used by the global shortcut, the Edit menu,
   and the toolbar buttons. Pixel strokes, document/palette snapshots, and
   legacy anipoint/hitbox snapshots share a sequence number so mixed edits
   undo in the order the user made them. */
static bool CanUndo(void) { return !g_pixel_hist.empty() || !g_doc_hist.empty() || g_undo_idx > 0; }
static bool CanRedo(void) { return !g_pixel_redo.empty() || !g_doc_redo.empty() || g_undo_idx < g_undo_count - 1; }

static void DoPixelUndo(void)
{
    if (!g_pixel_hist.empty()) {
        PixelHist e = g_pixel_hist.back();
        g_pixel_hist.pop_back();
        PixelHist r = {};
        bool have_redo = pixel_hist_capture_img(e.img_idx, &r, e.full_state);
        if (have_redo) r.seq = e.seq;
        if (pixel_hist_restore(&e)) {
            if (have_redo) g_pixel_redo.push_back(r);
            g_palette_nav = false;
        } else {
            if (have_redo) pixel_hist_free(&r);
            g_pixel_hist.push_back(e);
            return;
        }
        pixel_hist_free(&e);
    }
}

static void DoDocUndo(void)
{
    if (g_doc_hist.empty()) return;
    DocSnapshot *snap = g_doc_hist.back();
    g_doc_hist.pop_back();

    DocSnapshot *redo = CaptureDocSnapshot(snap->seq);
    if (RestoreDocSnapshot(snap)) {
        if (redo) g_doc_redo.push_back(redo);
    } else {
        if (redo) FreeDocSnapshot(redo);
        g_doc_hist.push_back(snap);
        return;
    }
    FreeDocSnapshot(snap);
}

static void DoLegacyUndo(void)
{
    if (g_undo_idx > 0) {
        g_undo_idx--;
        undo_apply(g_undo_idx);
    }
}

static void DoUndo(void)
{
    unsigned int pixel_seq = !g_pixel_hist.empty() ? g_pixel_hist.back().seq : 0;
    unsigned int doc_seq = !g_doc_hist.empty() ? g_doc_hist.back()->seq : 0;
    unsigned int legacy_seq = (g_undo_idx > 0) ? g_undo[g_undo_idx].seq : 0;

    if (doc_seq && doc_seq >= pixel_seq && doc_seq >= legacy_seq)
        DoDocUndo();
    else if (pixel_seq && pixel_seq >= legacy_seq)
        DoPixelUndo();
    else
        DoLegacyUndo();
}

static void DoPixelRedo(void)
{
    if (!g_pixel_redo.empty()) {
        PixelHist e = g_pixel_redo.back();
        g_pixel_redo.pop_back();
        PixelHist u = {};
        bool have_undo = pixel_hist_capture_img(e.img_idx, &u, e.full_state);
        if (have_undo) u.seq = e.seq;
        if (pixel_hist_restore(&e)) {
            if (have_undo) g_pixel_hist.push_back(u);
            g_palette_nav = false;
        } else {
            if (have_undo) pixel_hist_free(&u);
            g_pixel_redo.push_back(e);
            return;
        }
        pixel_hist_free(&e);
    }
}

static void DoDocRedo(void)
{
    if (g_doc_redo.empty()) return;
    DocSnapshot *snap = g_doc_redo.back();
    g_doc_redo.pop_back();

    DocSnapshot *undo = CaptureDocSnapshot(snap->seq);
    if (RestoreDocSnapshot(snap)) {
        if (undo) g_doc_hist.push_back(undo);
    } else {
        if (undo) FreeDocSnapshot(undo);
        g_doc_redo.push_back(snap);
        return;
    }
    FreeDocSnapshot(snap);
}

static void DoLegacyRedo(void)
{
    if (g_undo_idx < g_undo_count - 1) {
        g_undo_idx++;
        undo_apply(g_undo_idx);
    }
}

static void DoRedo(void)
{
    const unsigned int none = 0xFFFFFFFFu;
    unsigned int pixel_seq = !g_pixel_redo.empty() ? g_pixel_redo.back().seq : none;
    unsigned int doc_seq = !g_doc_redo.empty() ? g_doc_redo.back()->seq : none;
    unsigned int legacy_seq = (g_undo_idx < g_undo_count - 1) ? g_undo[g_undo_idx + 1].seq : none;

    if (doc_seq <= pixel_seq && doc_seq <= legacy_seq)
        DoDocRedo();
    else if (pixel_seq <= legacy_seq)
        DoPixelRedo();
    else
        DoLegacyRedo();
}

static int FindDirtyDocumentIndex(void)
{
    int active = document_active_index();
    Document *cur = document_get(active);
    if (cur && cur->dirty) return active;
    for (int i = 0; i < document_tab_count(); i++) {
        Document *doc = document_get(i);
        if (doc && doc->dirty) return i;
    }
    return -1;
}

static bool HasDirtyDocuments(void)
{
    return FindDirtyDocumentIndex() >= 0;
}

static void RequestCloseDocumentTab(int idx)
{
    Document *doc = document_get(idx);
    if (!doc) return;
    if (doc->dirty) {
        ActivateDocumentTab(idx);
        g_pending_action = PendingAction::CloseTab;
        g_pending_tab_index = idx;
        g_show_unsaved_confirm = true;
        return;
    }
    bool closing_active = (idx == document_active_index());
    document_close_tab(idx);
    ResetPerDocumentUiState(false);
    if (closing_active) g_doc_tab_select_request = document_active_index();
}

static float DrawDocumentTabBar(float y, float sw)
{
    const float tab_h = ImGui::GetFrameHeight() + 5.0f;
    ImGui::SetNextWindowPos(ImVec2(0, y));
    ImGui::SetNextWindowSize(ImVec2(sw, tab_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
    ImGui::Begin("##document_tabs", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);

    /* Use real ImGui tabs, but only switch documents on tab activation.
       BeginTabItem() returns true every frame for the selected tab; treating
       that as a click is what caused the earlier back-and-forth selection
       fight. */
    int activate_idx = -1;
    int close_idx = -1;
    bool new_tab = false;
    int active = document_active_index();

    ImGuiTabBarFlags tab_flags = ImGuiTabBarFlags_FittingPolicyScroll;
    if (ImGui::BeginTabBar("##img_document_tabs", tab_flags)) {
        int n = document_tab_count();
        for (int i = 0; i < n; i++) {
            Document *doc = document_get(i);
            if (!doc) continue;

            const char *base = doc->fname_s[0] ? doc->fname_s : "Untitled";
            char label[96];
            snprintf(label, sizeof(label), "%s%s##doc_tab_%d",
                     doc->dirty ? "* " : "", base, i);

            bool open = true;
            ImGuiTabItemFlags item_flags = (i == active) ? ImGuiTabItemFlags_SetSelected
                                                         : ImGuiTabItemFlags_None;
            bool visible = ImGui::BeginTabItem(label, &open, item_flags);
            bool activated = ImGui::IsItemActivated();
            if (activated && i != active)
                activate_idx = i;
            if (visible)
                ImGui::EndTabItem();
            if (!open)
                close_idx = i;
        }

        if (g_world_state.enabled) {
            auto tab_toggle = [](const char *label, bool *value) {
                bool was_on = *value;
                if (was_on) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                    ImGui::PushStyleColor(ImGuiCol_Tab, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
                    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
                }
                if (ImGui::TabItemButton(label, ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                    *value = !*value;
                if (was_on) ImGui::PopStyleColor(3);
            };
            tab_toggle(g_world_state.onion ? "Onion: On" : "Onion", &g_world_state.onion);
            bool marked_was_on = g_world_marked_state.marked_play;
            tab_toggle(g_world_marked_state.marked_play ? "Marked: On" : "Marked", &g_world_marked_state.marked_play);
            if (marked_was_on != g_world_marked_state.marked_play) {
                WorldMarkedRestart(g_world_marked_state);
            }
            tab_toggle(g_world_marked_state.mirror_active ? "Mirror 1: On" : "Mirror 1", &g_world_marked_state.mirror_active);
            tab_toggle(g_world_marked_state.mirror_other ? "Mirror 2: On" : "Mirror 2", &g_world_marked_state.mirror_other);
        }

        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            new_tab = true;
        ImGui::EndTabBar();
    }
    g_doc_tab_select_request = -1;

    ImGui::End();
    ImGui::PopStyleVar(2);

    if (activate_idx >= 0)
        ActivateDocumentTab(activate_idx);
    if (close_idx >= 0)
        RequestCloseDocumentTab(close_idx);
    if (new_tab) {
        document_new_tab();
        g_doc_tab_select_request = document_active_index();
        ResetPerDocumentUiState(false);
    }
    return tab_h;
}

void imgui_overlay_render(void)
{
    ClearWorldTempTextures();
    ImGuiIO &io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    /* Reset transient per-image tool state when the selected image changes,
       so e.g. a Clone Stamp source from sprite A doesn't get re-applied as
       coords on sprite B (which could OOB-read or just paint garbage). */
    static int g_prev_ilselected = -2;
    static Document *g_prev_render_doc = NULL;
    if (g_doc != g_prev_render_doc || g_doc->ilselected != g_prev_ilselected) {
        g_prev_render_doc = g_doc;
        g_clone_source_set = false;
        g_clone_offset_set = false;
        g_remap_target_color = -1;
        g_snap_bbox.valid = false;
        /* Abort any in-progress freehand selection — its coords are in the
           previous image's pixel space and continuing the drag would mix
           coordinates across sprites. */
        g_lasso_points.clear();
        if (g_grid_sel.dragging) {
            g_grid_sel.dragging = false;
            g_grid_sel.active = false;
        }
        /* Drop multi-swatch selection too. Even if the new image shares a
           palette with the old one, users perceive image-switch as a
           fresh context and a stale yellow border on swatches is
           confusing. They can Ctrl/Shift-click to rebuild it. */
        commit_palette_adjustments();
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        if (TimelineCompositeReady() && TimelineCompositeSlot(g_doc->ilselected) < 0)
            ClearTimelineCompositeSelection();
        g_prev_ilselected = g_doc->ilselected;
    }

    /* ---- Global keyboard shortcuts ---- */
    ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    bool popup_using_keyboard =
        g_show_file_dialog ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    /* Undo / Redo. Three call sites share the same logic via DoUndo/DoRedo:
       the Ctrl+Z/Ctrl+Y shortcuts here, the Edit menu items, and the
       toolbar buttons. The CanUndo/CanRedo predicates drive both the
       toolbar enable/disable and the menu enable/disable so a paint
       stroke immediately makes the buttons clickable. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route)) DoUndo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route)) DoRedo();

    /* Clipboard */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C, route)) CopySelectionToNewImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_X, route)) CutSelectionToNewImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, route)) PasteClipboardAsNewImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, route)) copy_image(false);
    if (!io.KeyShift && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, route)) copy_image(true);
    if (!io.KeyShift && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, route)) paste_image();

    /* Adobe-standard selection shortcuts. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, route)) select_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route)) deselect_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I, route)) invert_selection();
    /* Ctrl+J duplicates: a floating paste commits and stays floating as a
       second copy of itself; otherwise duplicates the current image. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_J, route)) {
        if (g_pasted.active) {
            if (g_xform.active) xform_commit();
            apply_pasted_region(); /* leave g_pasted.active = true */
        } else {
            DuplicateImage();
        }
    }
    /* Ctrl+E commits a floating paste in place (Photoshop "Merge Down"). */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, route)) {
        if (g_pasted.active) {
            if (g_xform.active) xform_commit();
            apply_pasted_region();
            g_pasted.active = false;
            g_pasted.dragging = false;
        }
    }
    /* Shift+Del is an always-image delete escape hatch. Plain Del below follows
       the active side-panel list (images vs palettes). */
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Delete, route)) {
        if (g_doc->ilselected >= 0) RequestDeleteImage(g_doc->ilselected);
    }
    /* Image-list ops the menu advertises but were previously unbound. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R, route))     OpenRenameImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, route))     TogglePointTable();
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_PageUp, route))   MoveImageUp();
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_PageDown, route)) MoveImageDown();

    /* File I/O */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) RequestOpenDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) OpenFileDialog(FileDialogMode::SaveImg);
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_L, route)) OpenFileDialog(FileDialogMode::LoadLbm);
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_S, route)) OpenFileDialog(FileDialogMode::SaveLbm);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L, route)) OpenFileDialog(FileDialogMode::LoadTga);

    /* View / Debug (H is repurposed to flip the floating paste while one is up) */
    if (!g_pasted.active && ImGui::Shortcut(ImGuiKey_H,  route)) g_show_help = true;
    if (ImGui::Shortcut(ImGuiKey_F9, route)) g_show_debug = !g_show_debug;
    if (!popup_using_keyboard && !io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, route))
            QueueZoomStep(1);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, route))
            QueueZoomStep(-1);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, route))
            QueueZoomFit();
    }
    if (ImGui::Shortcut(ImGuiKey_R,  route)) {
        if (g_active_tool == ActiveTool::Marquee) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::Marquee;
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (ImGui::Shortcut(ImGuiKey_W,  route)) {
        if (g_active_tool == ActiveTool::MagicWand) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::MagicWand;
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (!g_pasted.active && ImGui::Shortcut(ImGuiKey_L,  route)) {
        if (g_active_tool == ActiveTool::Lasso) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::Lasso;
        g_lasso_points.clear();
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (ImGui::Shortcut(ImGuiKey_I,  route)) {
        if (g_active_tool == ActiveTool::Eyedropper) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::Eyedropper;
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (ImGui::Shortcut(ImGuiKey_P, route)) {
        /* Pencil — Adobe shortcut. Toggles back to None on a second press
           since the underlying paint behavior is the same as no tool. */
        g_active_tool = (g_active_tool == ActiveTool::Pencil) ? ActiveTool::None : ActiveTool::Pencil;
    }
    if (ImGui::Shortcut(ImGuiKey_G, route)) {
        g_active_tool = (g_active_tool == ActiveTool::PaintBucket) ? ActiveTool::None : ActiveTool::PaintBucket;
    }
    if (!g_pasted.active && ImGui::Shortcut(ImGuiKey_V, route)) {
        g_active_tool = (g_active_tool == ActiveTool::VariantPaint) ? ActiveTool::None : ActiveTool::VariantPaint;
    }
    /* [ and ] do double duty depending on context:
         - Pencil active: [ shrinks brush, ] grows brush (Photoshop convention).
         - Otherwise:     [ Set Palette for Marked, ] Set for Image
                          (matches the menu-item hint advertised next to those entries).
       Other one-key palette shortcuts advertised in the menus:
         *        — Merge Marked Palettes into Selected
         Shift+R  — Rename selected palette
         Del      — Delete selected palette
       All of these were previously advertised in tooltips but never actually
       wired; they're real shortcuts now. */
    if (g_active_tool == ActiveTool::Pencil || g_active_tool == ActiveTool::VariantPaint) {
        int *brush = (g_active_tool == ActiveTool::VariantPaint) ? &g_variant_brush : &g_pencil_brush;
        if (ImGui::Shortcut(ImGuiKey_LeftBracket,  route))
            { if (*brush > 1)  (*brush)--; }
        if (ImGui::Shortcut(ImGuiKey_RightBracket, route))
            { if (*brush < 16) (*brush)++; }
    } else {
        if (ImGui::Shortcut(ImGuiKey_LeftBracket,  route)) SetPaletteOfMarked();
        if (ImGui::Shortcut(ImGuiKey_RightBracket, route)) SetPaletteOfSelected();
    }
    /* '*' merges marked palettes. SDL physical-key bindings only fire on
       US-layout Shift+8, so use ImGui's text-input queue instead — the
       backend posts the actual typed character regardless of keyboard
       layout. Only when no widget owns the input focus (text fields
       would legitimately consume '*'). */
    if (!io.WantTextInput) {
        for (ImWchar c : io.InputQueueCharacters) {
            if (c == '*') { MergeMarkedPalettes(); break; }
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_R, route)) OpenRenamePalette(g_doc->plselected);
    if (!popup_using_keyboard && !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::Shortcut(ImGuiKey_Delete, route)) {
        if (g_palette_nav) DeletePalette();
        else RequestDeleteImage(g_doc->ilselected);
    }

    /* Tool Intercepts. Esc/Enter have a three-level priority: transform
       takes precedence, then floating paste, then marquee. */
    if (ImGui::Shortcut(ImGuiKey_Escape, route)) {
        if (g_xform.active)         { xform_cancel(); }
        else if (g_pasted.active)   { g_pasted.active = false; g_pasted.dragging = false; }
        else if (g_grid_sel.active) { g_grid_sel.active = false; }
    }
    if (ImGui::Shortcut(ImGuiKey_Enter, route)) {
        if (g_xform.active)                                    xform_commit();
        else if (g_pasted.active && !g_pasted.dragging)      { apply_pasted_region(); g_pasted.active = false; }
    }
    /* Ctrl+T enters Free Transform on the floating paste. Pressing it again
       while transforming commits and exits — symmetric with Photoshop. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_T, route)) {
        if (g_xform.active)        xform_commit();
        else if (g_pasted.active)  xform_begin();
    }
    /* While a floating paste is up (and not free-transforming): H / V mirror it
       in place, L drops it onto the sprite as a non-destructive layer. */
    if (g_pasted.active && !g_xform.active && !io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiKey_H, route)) flip_clipboard_horizontal();
        if (ImGui::Shortcut(ImGuiKey_V, route)) flip_clipboard_vertical();
        if (ImGui::Shortcut(ImGuiKey_L, route)) drop_paste_to_layer();
    }

    /* Image Operations */
    if (ImGui::Shortcut(ImGuiKey_Space, route)) {
        IMG *img = get_img(g_doc->ilselected); if (img) { img->flags ^= 1; mark_dirty(); }
    }
    /* Timeline play/pause (K = standard video editor convention). */
    if (ImGui::Shortcut(ImGuiKey_K, route)) imgtool_toggle_timeline_play();
    /* Left/Right scrub the animation timeline, or the marked-tab World View
       sequence when that preview is active. */
    bool widget_using_keyboard = popup_using_keyboard || ImGui::IsAnyItemActive() || ImGui::IsAnyItemFocused() || io.WantTextInput;
    if (!widget_using_keyboard && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
        if (ImGui::Shortcut(ImGuiKey_LeftArrow, route)) {
            if (g_world_state.enabled && g_world_marked_state.marked_play) StepWorldMarkedSequence(g_world_marked_state, -1);
            else StepTimelinePlayhead(-1);
        }
        if (ImGui::Shortcut(ImGuiKey_RightArrow, route)) {
            if (g_world_state.enabled && g_world_marked_state.marked_play) StepWorldMarkedSequence(g_world_marked_state, 1);
            else StepTimelinePlayhead(1);
        }
    }
    /* Ctrl+Left/Right reorders the current play-head frame within the timeline. */
    if (!widget_using_keyboard && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_LeftArrow, route)) {
        if (g_timeline_play_idx > 0 && g_timeline_play_idx < (int)g_timeline_frames.size()) {
            TimelineSwapFrames(g_timeline_play_idx, g_timeline_play_idx - 1);
            g_timeline_play_idx--;
        }
    }
    if (!widget_using_keyboard && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_RightArrow, route)) {
        if (g_timeline_play_idx + 1 < (int)g_timeline_frames.size()) {
            TimelineSwapFrames(g_timeline_play_idx, g_timeline_play_idx + 1);
            g_timeline_play_idx++;
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_M, route)) {
        IMG *p = (IMG*)g_doc->img_p; while (p) { p->flags |= 1; p = (IMG*)p->nxt_p; }
    }
    if (ImGui::Shortcut(ImGuiKey_M, route)) {
        IMG *p = (IMG*)g_doc->img_p; while (p) { p->flags &= ~1; p = (IMG*)p->nxt_p; }
    }
    if (ImGui::Shortcut(ImGuiKey_Semicolon, route)) LeastSquaresReduceMarked();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, route)) OpenFileDialog(FileDialogMode::ExportTga);

    /* Sprite / palette list navigation: cursor up/down flicks
     * between images (default) or palettes (when palette panel
     * was last clicked), matching DOS imgtool muscle memory. */
    if (!popup_using_keyboard && g_palette_nav && g_doc->palcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            SelectPalette((g_doc->plselected + 1) % (int)g_doc->palcnt);
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            SelectPalette((g_doc->plselected <= 0) ? (int)g_doc->palcnt - 1 : g_doc->plselected - 1);
            g_zoom_reset = true;
        }
    } else if (!popup_using_keyboard && g_doc->imgcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            g_doc->ilselected = (g_doc->ilselected + 1) % (int)g_doc->imgcnt;
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            g_doc->ilselected = (g_doc->ilselected <= 0) ? (int)g_doc->imgcnt - 1 : g_doc->ilselected - 1;
            g_zoom_reset = true;
        }
    }
    /* Tab toggles World View mode (anipoint alignment workspace). */
    if (ImGui::Shortcut(ImGuiKey_Tab, route)) {
        g_world_state.enabled = !g_world_state.enabled;
    }

    /* ---- Menu bar ---- */
    if (ImGui::BeginMainMenuBar()) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
        if (ImGui::BeginMenu("File")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("New"))             g_show_new_img_confirm = true;
            if (ImGui::MenuItem("Open...",  "Ctrl+O")) RequestOpenDialog();
            if (ImGui::BeginMenu("Open Recent", !g_recent_files.empty())) {
                std::vector<std::string> snap = g_recent_files;
                for (size_t i = 0; i < snap.size(); i++) {
                    char label[1100];
                    snprintf(label, sizeof(label), "%zu. %s", i + 1, snap[i].c_str());
                    if (ImGui::MenuItem(label)) RequestOpenPath(snap[i]);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent")) {
                    g_recent_files.clear();
                    RecentSave();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save",    "Ctrl+S")) OpenFileDialog(FileDialogMode::SaveImg);
            if (ImGui::MenuItem("Append"))            OpenFileDialog(FileDialogMode::AppendImg);
            if (ImGui::MenuItem("Open LOD..."))       RequestOpenLodDialog();
            ImGui::Separator();
            if (ImGui::BeginMenu("Import")) {
                if (ImGui::MenuItem("PNG File..."))                 OpenFileDialog(FileDialogMode::ImportPng);
                if (ImGui::MenuItem("PNG (Match to Active Palette)...")) OpenFileDialog(FileDialogMode::ImportPngMatch);
                if (ImGui::MenuItem("Sprite Sheet (Match Palette)...")) OpenFileDialog(FileDialogMode::ImportSpriteSheetMatch);
                if (ImGui::MenuItem("GIF File..."))                 OpenFileDialog(FileDialogMode::ImportGif);
                if (ImGui::MenuItem("Palette..."))                  OpenFileDialog(FileDialogMode::ImportPalette);
                ImGui::Separator();
                if (ImGui::MenuItem("Load LBM", "Alt+L"))  OpenFileDialog(FileDialogMode::LoadLbm);
                if (ImGui::MenuItem("Load TGA", "Ctrl+L")) OpenFileDialog(FileDialogMode::LoadTga);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Export")) {
                if (ImGui::MenuItem("PNG File..."))                    OpenFileDialog(FileDialogMode::ExportPng);
                if (ImGui::MenuItem("Palette..."))                     OpenFileDialog(FileDialogMode::ExportPalette);
                ImGui::Separator();
                if (ImGui::MenuItem("Save LBM", "Alt+S"))        OpenFileDialog(FileDialogMode::SaveLbm);
                if (ImGui::MenuItem("Save Marked LBM"))          OpenFileDialog(FileDialogMode::SaveMarkedLbm);
                if (ImGui::MenuItem("Save TGA"))                 OpenFileDialog(FileDialogMode::SaveTga);
                ImGui::Separator();
                if (ImGui::MenuItem("Build TGA from Marked", "Ctrl+B")) OpenFileDialog(FileDialogMode::ExportTga);
                if (ImGui::MenuItem("Write ANILST..."))                OpenFileDialog(FileDialogMode::WriteAniLst);
                if (ImGui::MenuItem("Write TBL..."))                   OpenFileDialog(FileDialogMode::WriteTbl);
    if (ImGui::MenuItem("Write IRW..."))                   OpenFileDialog(FileDialogMode::WriteIrw);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) g_pending_quit = true;
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            bool can_undo = CanUndo();
            bool can_redo = CanRedo();
            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) DoUndo();
            if (!can_undo) ImGui::EndDisabled();
            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) DoRedo();
            if (!can_redo) ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy",  "Ctrl+C", false, g_doc->ilselected >= 0)) copy_image(false);
            if (ImGui::MenuItem("Cut",   "Ctrl+X", false, g_doc->ilselected >= 0)) copy_image(true);
            if (ImGui::MenuItem("Copy to New Sprite", "Ctrl+Shift+C", false, g_doc->ilselected >= 0))
                CopySelectionToNewImage();
            if (ImGui::MenuItem("Cut to New Sprite", "Ctrl+Shift+X", false, g_doc->ilselected >= 0))
                CutSelectionToNewImage();
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, g_clipboard.valid && g_doc->ilselected >= 0))
                paste_image();
            if (ImGui::MenuItem("Paste as New Sprite", "Ctrl+Shift+V", false, g_clipboard.valid))
                PasteClipboardAsNewImage();
            ImGui::Separator();
            /* Selection ops — disabled when nothing's available. */
            if (ImGui::MenuItem("Select All",       "Ctrl+A",       false, g_doc->ilselected >= 0))      select_all();
            if (ImGui::MenuItem("Deselect",         "Ctrl+D",       false, g_grid_sel.active))    deselect_all();
            if (ImGui::MenuItem("Invert Selection", "Ctrl+Shift+I", false, g_doc->ilselected >= 0))      invert_selection();
            ImGui::Separator();
            /* Floating-paste ops — only meaningful while a paste is active. */
            if (ImGui::MenuItem("Free Transform",   "Ctrl+T", false, g_pasted.active && !g_xform.active)) xform_begin();
            if (ImGui::MenuItem("Merge Down",       "Ctrl+E", false, g_pasted.active)) {
                if (g_xform.active) xform_commit();
                apply_pasted_region();
                g_pasted.active = false;
                g_pasted.dragging = false;
            }
            if (ImGui::MenuItem("Drop Paste to Layer", "L", false, g_pasted.active && !g_xform.active))
                drop_paste_to_layer();
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Image",     "Ctrl+R"))     OpenRenameImage();
            if (ImGui::MenuItem("Delete Image",     "Del"))        RequestDeleteImage(g_doc->ilselected);
            if (ImGui::MenuItem("Duplicate",        "Ctrl+J"))     DuplicateImage();
            if (ImGui::MenuItem("Trim Transparent Bounds", NULL, false, g_doc->ilselected >= 0)) {
                doc_undo_push();
                int n = CropSelectedImageToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Trimmed selected sprite to non-transparent bounds."
                               : "Selected sprite already fits, or has no opaque pixels.");
                g_restore_msg_timer = 4.0f;
                if (n > 0) g_zoom_reset = true;
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Image")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Mark / Unmark",      "Space"))  { IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1; }
            if (ImGui::MenuItem("Set All Marks",      "M"))      { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Clear All Marks",    "m"))      { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Invert All Marks"))             { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;} }
            ImGui::Separator();
            if (ImGui::MenuItem("Jump to Prev Marked")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (g_doc->ilselected - i + n_imgs) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { g_doc->ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Jump to Next Marked")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (g_doc->ilselected + i) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { g_doc->ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Move Up",    "Alt+PgUp")) MoveImageUp();
            if (ImGui::MenuItem("Move Down",  "Alt+PgDn")) MoveImageDown();
            ImGui::Separator();
            if (ImGui::MenuItem("Add/Del Point Table",  "Ctrl+P")) TogglePointTable();
            if (ImGui::MenuItem("Set ID from 2nd List"))           SetIDFromSecondList();
            if (ImGui::MenuItem("Switch Image List",    "Tab"))    SwitchImageList();
            if (ImGui::MenuItem("Clear Extra Data"))               ClearExtraData();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Operations")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Break into Subframes...")) OpenAutoChopDialog();
            if (ImGui::MenuItem("Resize Sprite...", NULL, false, g_doc->ilselected >= 0)) OpenResizeSpriteDialog();
            if (ImGui::MenuItem("Bulk Resize Marked...", NULL, false, CountMarkedImages() > 0)) OpenBulkResizeDialog();
            if (ImGui::BeginMenu("Transform Selected", g_doc->ilselected >= 0)) {
                DrawSpriteTransformMenuItems();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Crop Selected to Content", NULL, false, g_doc->ilselected >= 0)) {
                doc_undo_push();
                int n = CropSelectedImageToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Cropped selected sprite to non-transparent bbox."
                               : "Selected sprite already fits, or has no opaque pixels.");
                g_restore_msg_timer = 4.0f;
                if (n > 0) g_zoom_reset = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Trim the selected image to its nearest non-transparent pixels.\n"
                "Anipoints are adjusted so the on-screen position is unchanged.");
            if (ImGui::MenuItem("Crop Marked to Content")) {
                doc_undo_push();
                int n = CropMarkedImagesToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Cropped %d image(s) to non-transparent bbox.", n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Trim each marked image to its non-transparent bounding box.\n"
                "Anipoints are adjusted so the on-screen position is unchanged.");
            if (ImGui::MenuItem("Defringe Marked Edges")) {
                int n = DefringeMarkedImages(1);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Defringe edited %d pixel(s).", n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "One-pass edge defringe: every pixel touching a transparent\n"
                "neighbor is averaged toward its non-transparent neighbors,\n"
                "killing the 1px halo of blue/green-spill on digitized actors.");
            if (ImGui::MenuItem("Remove Hard Stroke (1-2px)")) {
                RemoveHardStrokeFromTargets(2);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Detects a thin high-contrast outline/matte ring around\n"
                "transparent sprite edges and removes it. Uses marked sprites,\n"
                "or the selected sprite if none are marked.");
            if (ImGui::MenuItem("Apply Marked Likeness to Selected", NULL, false,
                                g_doc->ilselected >= 0)) {
                ApplyMarkedLikenessToSelected();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mark one source sprite, then select the target sprite.\n"
                "Uses the source palette/materials while preserving the\n"
                "target pose and actor shading. Best with a marked source\n"
                "frame that has similar costume, lighting, and scale.");
            if (ImGui::MenuItem("Align Marked Anipoints to Selected")) {
                int n = AlignAnipointsToMarked(g_doc->ilselected);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Anchored %d marked image(s) to selected anipoint.", n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Sets the anipoint of every marked image to match the\n"
                "currently-selected image's anipoint. Useful when several\n"
                "frames should share one anchor (head, hand, hilt).");
            if (ImGui::MenuItem("Mirror Marked Anipoints to Reverse")) {
                MirrorMarkedAnipointsToReverseWithToast();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mirrors X anipoints on marked sprites as width - X.\n"
                "Y/Z values stay unchanged.");
            ImGui::Separator();
            if (ImGui::MenuItem("Least-Squares Reduce", ";"))               LeastSquaresReduceMarked();
            ImGui::Separator();
            if (ImGui::MenuItem("Strip Edge"))                               StripMarkedImages(5);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Repaints the original outer edge from nearby inward colors.\n"
                "Keeps the edit on the edge instead of walking into the sprite.");
            if (ImGui::MenuItem("Strip Edge Low"))                           StripMarkedImages(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Conservative edge repaint using the same original-edge logic.");
            if (ImGui::MenuItem("Strip Edge (Selected Color)"))              StripMarkedImages(5, g_sel_color);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Only strips the selected palette index, then softens the edge.");
            if (ImGui::MenuItem("Dither Replace"))                           DitherReplaceMarkedImages(g_sel_color);
            if (ImGui::MenuItem("Match All Sprites to Marked Source Colors")) {
                int source_idx = -1;
                int marked = 0;
                int idx = 0;
                for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p, idx++) {
                    if (p->flags & 1) {
                        source_idx = idx;
                        marked++;
                    }
                }
                if (marked != 1) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Mark exactly one source sprite first.");
                    g_restore_msg_timer = 4.0f;
                } else {
                    int pixels = 0;
                    int n = PreviewMatchAllSpritesToSourceColors(source_idx, &pixels);
                    if (n > 0 && doc_undo_push()) {
                        n = MatchAllSpritesToSourceColors(source_idx, &pixels);
                    }
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             n > 0 ? "Matched %d sprite(s) to source colors (%d px changed)."
                                   : "No sprites changed; colors already match or palettes are missing.",
                             n, pixels);
                    g_restore_msg_timer = 5.0f;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mark exactly one source sprite. Every other sprite is\n"
                "remapped to the nearest non-transparent colors used by\n"
                "that source and assigned to the source palette. Shapes stay unchanged.");
            ImGui::Separator();
            if (ImGui::MenuItem("Apply Variant Paint to Selection"))          ApplyVariantToSelection();
            if (ImGui::MenuItem("Remap Similar Regions to Current Swatch"))   ApplySelectionRemapToMatchingSprites();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Uses the current selection as a sample, then remaps likely\n"
                "matching same-palette regions across the IMG to the current swatch.");
            if (ImGui::MenuItem("Split Selection to Overlay Frame"))          SplitSelectionToOverlayFrame(true);
            if (ImGui::MenuItem("Copy Selection to Overlay Frame"))           SplitSelectionToOverlayFrame(false);
            ImGui::Separator();
            if (ImGui::MenuItem("Restore from Selected (pixel-diff)")) {
                int n = RestoreMarkedFromSource();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Restored %d pixel(s) from selected source."
                               : "No pixels restored. Check selection, marks, palettes, anipoints.",
                         n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "For each marked image, copy non-transparent source pixels\n"
                "into transparent strip pixels. Selected image = source.\n"
                "Uses anipoints to align; same palette required.");
            if (ImGui::MenuItem("Bulk Restore from Source (overwrite)")) {
                int n = RestoreMarkedFromSourceForce();
                IMG *s = get_img(g_doc->ilselected);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Rebuilt %d px from source. Source anipt (%d,%d) %dx%d"
                               : "0 px restored. Source anipt (%d,%d) %dx%d. Check marks/anipoints.",
                         n, s? (int)(short)s->anix:0, s? (int)(short)s->aniy:0, s? (int)s->w:0, s? (int)s->h:0);
                g_restore_msg_timer = 6.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Unconditionally overwrites every pixel in marked images\n"
                "with source pixels. No transparency or palette checks.\n"
                "For rebuilding splits (1A/1B/2A...) from full source.");
            if (ImGui::MenuItem("Bulk Restore via Regex...")) {
                g_show_restore_regex = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Uses a regex to map child names to parent names across the entire file,\n"
                "then restores child pixels from their parent automatically.");
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Marked"))                            OpenRenameMarkedImages();
            if (ImGui::MenuItem("Delete Marked"))                            RequestDeleteMarkedImages();
            if (ImGui::MenuItem("Set Palette for Marked", "["))              SetPaletteOfMarked();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Palette")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Add Palette"))                    AddNewPalette();
            if (ImGui::MenuItem("Duplicate Palette"))              DuplicatePalette();
            ImGui::Separator();
            if (ImGui::MenuItem("Set for Image",       "]"))       SetPaletteOfSelected();
            if (ImGui::MenuItem("Merge Marked into Selected", "*")) MergeMarkedPalettes();
            if (ImGui::MenuItem("Preview Merge Marked into Selected")) OpenPaletteMergePreview();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Shows source-to-target palette mapping and drift before merging.");
            if (ImGui::MenuItem("Delete Palette",      "Del"))     DeletePalette();
            if (ImGui::MenuItem("Rename Palette",      "Shift+R")) OpenRenamePalette(g_doc->plselected);
            ImGui::Separator();
            if (ImGui::MenuItem("Show Histogram"))               { CalculatePaletteHistogram(); g_show_histogram = true; }
            if (ImGui::MenuItem("Clean Up Palette"))             CleanupSelectedPalette();
            if (ImGui::MenuItem("Clean Copy Palette"))           CreateCleanedPaletteCopy();
            if (ImGui::MenuItem("Inherit Colors from Marked"))   InheritSelectedPaletteFromMarked();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mark the source palette, select the target palette.\n"
                "Sprites using the target are remapped to the nearest source colors.");
            if (ImGui::MenuItem("Merge Duplicate Palettes"))      MergeDuplicatePalettes();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Finds byte-identical palettes and remaps sprites using later\n"
                "duplicates to the first matching palette in the list.");
            if (ImGui::MenuItem("Downscale Palette..."))         OpenPaletteReduceDialog(7);
            if (ImGui::MenuItem("Copy #0 to Opaque Slot"))       CopyPaletteZeroToOpaqueSlot();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Index 0 remains transparent; this copies its RGB into\n"
                "a nonzero palette slot so it can be painted visibly.");
            if (ImGui::MenuItem("Import Palette..."))            OpenFileDialog(FileDialogMode::ImportPalette);
            if (ImGui::MenuItem("Export Palette..."))            OpenFileDialog(FileDialogMode::ExportPalette);
            ImGui::Separator();
            if (ImGui::MenuItem("Mark All")) {
                PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Clear Marks")) {
                PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags&=~1;p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Invert Marks")) {
                PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;}
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            ImGui::MenuItem("Verbose Logging", NULL, &g_verbose);
            ImGui::Separator();
            ImGui::BeginDisabled(g_doc->ilselected < 0);
            if (ImGui::MenuItem("Zoom In", "Ctrl+=")) QueueZoomStep(1);
            if (ImGui::MenuItem("Zoom Out", "Ctrl+-")) QueueZoomStep(-1);
            if (ImGui::MenuItem("Fit Sprite", "Ctrl+0")) QueueZoomFit();
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::MenuItem("Anim Points",     NULL, &g_show_points);
            ImGui::MenuItem("Hitboxes",        NULL, &g_show_hitbox);
            ImGui::MenuItem("DMA Compression", NULL, &g_show_dma_comp);
            ImGui::Separator();
            ImGui::MenuItem("World View",      NULL,   &g_world_state.enabled);
            if (g_world_state.enabled) {
                if (ImGui::MenuItem("Marked Tab Playback", NULL, &g_world_marked_state.marked_play)) {
                    WorldMarkedRestart(g_world_marked_state);
                }
                ImGui::MenuItem("Marked Playback Paused", NULL, &g_world_marked_state.paused);
                ImGui::SetNextItemWidth(80);
                ImGui::SliderFloat("Marked FPS", &g_world_marked_state.fps, 1.0f, 60.0f, "%.1f");
                if (ImGui::MenuItem("Dummy Decap Body", NULL,
                                    &g_world_marked_state.dummy_decap_body)) {
                    g_world_marked_state.dummy_decap_reset = true;
                    g_world_marked_state.hold_end[kWorldDummyDecapSlot] = true;
                    WorldMarkedRestart(g_world_marked_state);
                }
                if (ImGui::BeginMenu("Marked Tab Lanes")) {
                    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++) {
                        ImGui::PushID(slot);
                        char label[64];
                        if (slot == kWorldDummyDecapSlot)
                            snprintf(label, sizeof(label), "Dummy Body Hold Final Frame");
                        else
                            snprintf(label, sizeof(label), "Slot %d Hold Final Frame", slot + 1);
                        if (ImGui::MenuItem(label, NULL, &g_world_marked_state.hold_end[slot])) {
                            WorldMarkedRestart(g_world_marked_state);
                        }
                        bool *mirror = WorldMarkedMirrorFlag(g_world_marked_state, slot);
                        if (mirror) {
                            if (slot == kWorldDummyDecapSlot)
                                snprintf(label, sizeof(label), "Dummy Body Mirror");
                            else
                                snprintf(label, sizeof(label), "Slot %d Mirror", slot + 1);
                            ImGui::MenuItem(label, NULL, mirror);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("World W",      &g_world_state.w, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("World H",      &g_world_state.h, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Origin X",     &g_world_state.origin_x, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Origin Y",     &g_world_state.origin_y, 0, 0);
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Verify LOAD2 Packing")) {
                g_load2_report = VerifyLoad2Packing(g_load2_ppp);
                g_load2_selected_idx = -1;
                g_show_load2_verify = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("MK2 Hitboxes (MKSTK.ASM)...")) g_show_mk2 = true;
            if (ImGui::MenuItem("MK2 Fatality Lab...")) g_show_mk2_fatality = true;
            if (ImGui::MenuItem("ASM Animation Viewer...", NULL, &g_show_asm_anim) &&
                g_show_asm_anim && g_asm_anims.empty())
                OpenFileDialog(FileDialogMode::LoadAsmAnim);
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Show Help",  "h"))  g_show_help = true;
            if (ImGui::MenuItem("Debug Info", "F9")) g_show_debug = !g_show_debug;
            ImGui::Separator();
            if (ImGui::MenuItem("About...")) g_show_about = true;
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        /* Right-aligned dirty / filename indicator. Gives users a passive
           reminder that there are unsaved changes — without this, the only
           "this file is modified" signal is the quit-time confirmation. */
        {
            const char *name = (g_doc->fname_s[0] != '\0') ? g_doc->fname_s : "(unsaved)";
            char label[80];
            snprintf(label, sizeof(label), "%s%s",
                     g_dirty ? "* " : "  ",     /* ASCII asterisk — universal 'modified' convention */
                     name);
            float text_w = ImGui::CalcTextSize(label).x + 16.0f;
            float avail_w = ImGui::GetContentRegionAvail().x;
            if (avail_w > text_w) ImGui::SameLine(ImGui::GetCursorPosX() + (avail_w - text_w));
            if (g_dirty) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::TextUnformatted(label);
            if (g_dirty) ImGui::PopStyleColor();
            if (g_dirty && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Unsaved changes — Ctrl+S to save");
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::EndMainMenuBar();
    }

    float menu_h = ImGui::GetFrameHeight();
    float tab_h = DrawDocumentTabBar(menu_h, sw);
    float work_y = menu_h + tab_h;
    float work_h = sh - work_y;

    /* ---- Sync Palette State ---- */
    static Document *last_palette_doc = NULL;
    static int last_ilselected = -2;
    static int last_plselected = -2;
    static void *last_pal_p = (void *)-1;
    static unsigned int last_palcnt = ~0u;
    static unsigned int last_palette_sync_serial = 0;

    bool document_changed = g_doc != last_palette_doc;
    bool palette_head_changed = g_doc->pal_p != last_pal_p;
    bool palette_count_changed = g_doc->palcnt != last_palcnt;
    bool palette_sync_forced = g_palette_sync_serial != last_palette_sync_serial;
    bool palette_list_changed =
        document_changed ||
        palette_head_changed ||
        palette_count_changed ||
        palette_sync_forced;
    bool image_selection_changed = g_doc->ilselected != last_ilselected;

    if (palette_list_changed || image_selection_changed) {
        last_palette_doc = g_doc;
        last_ilselected = g_doc->ilselected;
        last_pal_p = g_doc->pal_p;
        last_palcnt = g_doc->palcnt;
        last_palette_sync_serial = g_palette_sync_serial;
        IMG* img = get_img(g_doc->ilselected);
        bool sync_selection_to_image = document_changed ||
                                       palette_head_changed ||
                                       image_selection_changed;
        if (sync_selection_to_image && img) {
            if ((unsigned)img->palnum < g_doc->palcnt)
                g_doc->plselected = img->palnum;
            else if (g_doc->palcnt == 0)
                g_doc->plselected = -1;
            else if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt)
                g_doc->plselected = (int)g_doc->palcnt - 1;
        } else if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) {
            g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;
        }
    }

    if (palette_list_changed || g_doc->plselected != last_plselected) {
        last_plselected = g_doc->plselected;
        ApplyPalette(g_doc->plselected);
        g_img_tex_idx = -2; /* Force texture rebuild to use new palette */
        reset_palette_adjust_sliders();
        /* Clear multi-select on palette change. Indexes from the previous
           palette don't map cleanly to the new one (different colors at the
           same index), so persisting the selection is misleading. */
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        save_palette_baseline();
    }

    /* Rebuild image texture every frame to pick up palette and data changes.
       When the texture-idx sentinel signals invalidation (set to -2 by any
       pixel-modifying tool), drop the matching timeline thumbnail too. */
    {
        IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
        if (g_img_tex_idx == -2 && g_doc->ilselected >= 0) InvalidateThumb(g_doc->ilselected);
        rebuild_img_texture(img);
        g_img_tex_idx = g_doc->ilselected;
    }

    /* ===== LEFT TOOLBAR ===== */
    ImGui::SetNextWindowPos(ImVec2(0, work_y));
    ImGui::SetNextWindowSize(ImVec2(TOOLBAR_W, work_h - PALETTE_H - TIMELINE_H));
    ImGui::Begin("##toolbar", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    {
        ImVec2 btn(TOOLBAR_W - 12, TOOLBAR_W - 12);
        #define TB_LABEL(icon, txt) (g_icon_font_loaded ? (icon) : (txt))

#define TOOL_ACTIVE_COL(r,g,b) ImVec4((r), (g), (b), 1.0f)
        btn = ImVec2(28.0f, 28.0f);
        float left_x = ImGui::GetCursorPosX();
        float right_x = left_x + btn.x + 4.0f;
        float tool_y = ImGui::GetCursorPosY();
        float action_y = tool_y;
        ImVec4 tool_idle(0.25f, 0.25f, 0.25f, 1.0f);
        ImVec4 action_idle(0.10f, 0.24f, 0.48f, 1.0f);
        ImVec4 action_active(0.12f, 0.42f, 0.78f, 1.0f);

        auto place_tool = [&]() {
            ImGui::SetCursorPos(ImVec2(right_x, tool_y));
            tool_y += btn.y + 4.0f;
        };
        auto place_action = [&]() {
            ImGui::SetCursorPos(ImVec2(left_x, action_y));
            action_y += btn.y + 4.0f;
        };
        auto toggle_tool = [&](ActiveTool tool) {
            if (g_active_tool == tool) g_active_tool = ActiveTool::None;
            else g_active_tool = tool;
            if (tool == ActiveTool::Lasso) g_lasso_points.clear();
            if (g_active_tool == ActiveTool::None &&
                (tool == ActiveTool::Marquee || tool == ActiveTool::MagicWand ||
                 tool == ActiveTool::Lasso || tool == ActiveTool::BackgroundEraser ||
                 tool == ActiveTool::CloneStamp || tool == ActiveTool::SmartRemap ||
                 tool == ActiveTool::Eyedropper))
                g_grid_sel.active = false;
        };
        auto tool_button = [&](ActiveTool tool, const char *icon, const char *txt,
                               ImVec4 active_col, const char *tip) {
            place_tool();
            ImGui::PushStyleColor(ImGuiCol_Button,
                g_active_tool == tool ? active_col : tool_idle);
            if (ImGui::Button(TB_LABEL(icon, txt), btn)) toggle_tool(tool);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        auto action_button = [&](const char *icon, const char *txt,
                                 const char *tip, bool disabled,
                                 bool active, auto on_click) {
            place_action();
            ImGui::PushStyleColor(ImGuiCol_Button, active ? action_active : action_idle);
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button(TB_LABEL(icon, txt), btn)) on_click();
            if (disabled) ImGui::EndDisabled();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };

        tool_button(ActiveTool::Marquee, ICON_MARQUEE, ICON_MARQUEE_TXT,
                    TOOL_ACTIVE_COL(0.2f,0.4f,0.7f), "Marquee Select Tool (R)");
        tool_button(ActiveTool::MagicWand, "\xEF\x8C\x9F", "Wd",
                    TOOL_ACTIVE_COL(0.5f,0.2f,0.7f), "Magic Wand Tool (W)\nCtrl-click adds to the current selection");
        tool_button(ActiveTool::Pencil, "\xEE\x8F\x89", "Pn",
                    TOOL_ACTIVE_COL(0.7f,0.6f,0.2f), "Pencil (P)\n[ / ] to shrink / grow brush");
        tool_button(ActiveTool::PaintBucket, "\xEE\x8E\xAE", "Bk",
                    TOOL_ACTIVE_COL(0.7f,0.45f,0.15f), "Paint Bucket (G)");
        tool_button(ActiveTool::VariantPaint, "\xEE\x90\x8A", "Vt",
                    TOOL_ACTIVE_COL(0.2f,0.6f,0.7f), "Variant Paint (V)");
        tool_button(ActiveTool::BackgroundEraser, "\xEE\x9B\x90", "Er",
                    TOOL_ACTIVE_COL(0.7f,0.2f,0.2f), "Smart Eraser");
        tool_button(ActiveTool::CloneStamp, "\xEE\x8E\xBB", "Cl",
                    TOOL_ACTIVE_COL(0.2f,0.6f,0.3f), "Clone Stamp");
        tool_button(ActiveTool::SmartRemap, "\xEE\x90\x8A", "Rm",
                    TOOL_ACTIVE_COL(0.8f,0.4f,0.1f), "Smart Palette Remapper");
        tool_button(ActiveTool::Lasso, "\xEE\xAC\x83", "Ls",
                    TOOL_ACTIVE_COL(0.3f,0.5f,0.8f), "Lasso Selection Tool (L)");
        tool_button(ActiveTool::Eyedropper, "\xEF\x8D\x91", "Ey",
                    TOOL_ACTIVE_COL(0.6f,0.7f,0.2f), "Eyedropper Tool (I)");

        action_button(ICON_MARK, ICON_MARK_TXT, "Mark/Unmark (Space)", false, false, [&]() {
            IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1;
        });
        action_button(ICON_MARK_ALL, ICON_MARK_ALL_TXT, "Set All Marks (M)", false, false, [&]() {
            IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;}
        });
        action_button(ICON_CLEAR, ICON_CLEAR_TXT, "Clear All Marks (m)", false, false, [&]() {
            IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;}
        });
        action_button(ICON_POINTS, ICON_POINTS_TXT, "Toggle Anim Points", false, g_show_points, [&]() {
            g_show_points = !g_show_points;
        });
        action_button(ICON_HITBOX, ICON_HITBOX_TXT, "Toggle Hitbox", false, g_show_hitbox, [&]() {
            g_show_hitbox = !g_show_hitbox;
        });
        action_button(ICON_RESIZE, ICON_RESIZE_TXT, "Resize Sprite", g_doc->ilselected < 0, false, [&]() {
            OpenResizeSpriteDialog();
        });
        action_button(ICON_ZOOM_IN, ICON_ZOOM_IN_TXT, "Zoom In (Ctrl+=)", g_doc->ilselected < 0, false, [&]() {
            QueueZoomStep(1);
        });
        action_button(ICON_ZOOM_OUT, ICON_ZOOM_OUT_TXT, "Zoom Out (Ctrl+-)", g_doc->ilselected < 0, false, [&]() {
            QueueZoomStep(-1);
        });
        action_button(ICON_UNDO, ICON_UNDO_TXT, "Undo (Ctrl+Z)", !CanUndo(), false, [&]() {
            DoUndo();
        });
        action_button(ICON_REDO, ICON_REDO_TXT, "Redo (Ctrl+Y)", !CanRedo(), false, [&]() {
            DoRedo();
        });

        ImGui::SetCursorPosY((tool_y > action_y ? tool_y : action_y) + 2.0f);
        ImGui::Separator();
        ImGui::SetNextItemWidth(TOOLBAR_W - 16.0f);
        if (g_active_tool == ActiveTool::Pencil) {
            ImGui::SliderInt("##pencil_brush", &g_pencil_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pencil brush radius");
        } else if (g_active_tool == ActiveTool::VariantPaint) {
            ImGui::SliderInt("##variant_brush", &g_variant_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Variant brush radius");
        } else if (g_active_tool == ActiveTool::PaintBucket) {
            ImGui::SliderInt("##bucket_tol", &g_bucket_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint bucket tolerance");
            ImGui::Checkbox("C##bucket", &g_bucket_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous fill");
        } else if (g_active_tool == ActiveTool::MagicWand) {
            ImGui::SliderInt("##wand_tol", &g_wand_tolerance, 0, 64);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Magic Wand strength");
            ImGui::Checkbox("C##wand", &g_wand_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous selection");
        } else if (g_active_tool == ActiveTool::CloneStamp) {
            ImGui::SliderInt("##clone_brush", &g_clone_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clone brush radius");
        } else if (g_active_tool == ActiveTool::BackgroundEraser) {
            ImGui::SliderInt("##eraser_tol", &g_eraser_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart eraser tolerance");
            ImGui::Checkbox("C##eraser", &g_eraser_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous erase");
            ImGui::Checkbox("D##eraser", &g_eraser_defringe);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Defringe edge pixels");
        } else if (g_active_tool == ActiveTool::SmartRemap) {
            ImGui::SliderInt("##remap_tol", &g_remap_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart remap tolerance");
        }

        ImGui::Spacing();
        {
            SDL_Color &c = g_palette[g_sel_color];
            ImU32 col = IM_COL32(c.r, c.g, c.b, 255);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float sw_sz = ImGui::GetContentRegionAvail().x;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(cp, ImVec2(cp.x + sw_sz, cp.y + 24), col);
            dl->AddRect(cp, ImVec2(cp.x + sw_sz, cp.y + 24), IM_COL32(255,255,255,80));
            ImGui::Dummy(ImVec2(sw_sz, 24));
        }
        char col_label[8];
        snprintf(col_label, sizeof(col_label), "#%d", g_sel_color);
        if (ImGui::SmallButton(col_label)) {
            static int last_col = 1;
            if (g_sel_color == 0) g_sel_color = last_col;
            else { last_col = g_sel_color; g_sel_color = 0; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active color index (right-click sprite to pick)");
#undef TOOL_ACTIVE_COL

#if 0
        if (ImGui::Button(TB_LABEL(ICON_MARK, ICON_MARK_TXT), btn))  { IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark/Unmark (Space)");
        if (ImGui::Button(TB_LABEL(ICON_MARK_ALL, ICON_MARK_ALL_TXT), btn))  { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set All Marks (M)");
        if (ImGui::Button(TB_LABEL(ICON_CLEAR, ICON_CLEAR_TXT), btn))  { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All Marks (m)");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, g_show_points ?
            ImVec4(0.2f,0.6f,0.2f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL(ICON_POINTS, ICON_POINTS_TXT), btn)) g_show_points = !g_show_points;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Anim Points");
        ImGui::PushStyleColor(ImGuiCol_Button, g_show_hitbox ?
            ImVec4(0.0f,0.5f,0.6f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL(ICON_HITBOX, ICON_HITBOX_TXT), btn)) g_show_hitbox = !g_show_hitbox;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Hitbox");
        ImGui::BeginDisabled(g_doc->ilselected < 0);
        if (ImGui::Button(TB_LABEL(ICON_RESIZE, ICON_RESIZE_TXT), btn)) OpenResizeSpriteDialog();
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Resize Sprite");
        ImGui::BeginDisabled(g_doc->ilselected < 0);
        if (ImGui::Button(TB_LABEL(ICON_ZOOM_IN, ICON_ZOOM_IN_TXT), btn)) QueueZoomStep(1);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom In (Ctrl+=)");
        ImGui::BeginDisabled(g_doc->ilselected < 0);
        if (ImGui::Button(TB_LABEL(ICON_ZOOM_OUT, ICON_ZOOM_OUT_TXT), btn)) QueueZoomStep(-1);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom Out (Ctrl+-)");
        /* Pencil tool — single-pixel paint at the current palette index.
           Same paint behavior as the "no active tool" state, but exposed
           as an explicit mode so it's discoverable and pairs with the
           Adobe `P` shortcut. Click = 1 pixel; drag = continuous line. */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::Pencil ?
            ImVec4(0.7f,0.6f,0.2f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\x8F\x89", "Pn"), btn)) { /* U+E3C9 edit */
            g_active_tool = (g_active_tool == ActiveTool::Pencil) ? ActiveTool::None : ActiveTool::Pencil;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pencil - paint at current color (P)\n[ / ] to shrink / grow brush");

        /* Paint Bucket tool */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::PaintBucket ?
            ImVec4(0.7f,0.45f,0.15f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\x8E\xAE", "Bk"), btn)) { /* U+E3AE format_color_fill */
            g_active_tool = (g_active_tool == ActiveTool::PaintBucket) ? ActiveTool::None : ActiveTool::PaintBucket;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint Bucket (G)\nClick to fill with current color");

        /* Variant Paint: paints with the selected palette color on the
           current/target palette while cloning the original color into every
           other palette at the new shadow index. */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::VariantPaint ?
            ImVec4(0.2f,0.6f,0.7f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\x90\x8A", "Vt"), btn)) { /* U+E40A palette */
            g_active_tool = (g_active_tool == ActiveTool::VariantPaint) ? ActiveTool::None : ActiveTool::VariantPaint;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Variant Paint (V)\nPaints visible detail only on the current palette\nover existing opaque pixels");

        /* Marquee/select tool — explicit mode toggle. When off, no green-box
           selection ever starts (no more random firing). */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::Marquee ?
            ImVec4(0.2f,0.4f,0.7f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL(ICON_MARQUEE, ICON_MARQUEE_TXT), btn)) {
            if (g_active_tool == ActiveTool::Marquee) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::Marquee;
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Marquee Select Tool (R)");

        /* Magic Wand tool */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::MagicWand ?
            ImVec4(0.5f,0.2f,0.7f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEF\x8C\x9F", "Wd"), btn)) { /* U+F31F wand_shine */
            if (g_active_tool == ActiveTool::MagicWand) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::MagicWand;
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Magic Wand Tool (W)\nCtrl-click adds to the current selection");

        /* Background Eraser tool */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::BackgroundEraser ?
            ImVec4(0.7f,0.2f,0.2f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\x9B\x90", "Er"), btn)) { /* U+E6D0 ink_eraser */
            if (g_active_tool == ActiveTool::BackgroundEraser) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::BackgroundEraser;
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart Eraser - chroma-key with tolerance + defringe (E)");

        /* Clone Stamp tool */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::CloneStamp ?
            ImVec4(0.2f,0.6f,0.3f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\x8E\xBB", "Cl"), btn)) { /* U+E3BB control_point_duplicate */
            if (g_active_tool == ActiveTool::CloneStamp) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::CloneStamp;
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clone Stamp Tool (C)");

        /* Smart Remap tool */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::SmartRemap ?
            ImVec4(0.8f,0.4f,0.1f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\x90\x8A", "Rm"), btn)) { /* U+E40A palette */
            if (g_active_tool == ActiveTool::SmartRemap) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::SmartRemap;
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart Palette Remapper Tool");

        /* Lasso tool */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::Lasso ?
            ImVec4(0.3f,0.5f,0.8f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEE\xAC\x83", "Ls"), btn)) { /* U+EB03 lasso_select */
            if (g_active_tool == ActiveTool::Lasso) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::Lasso;
            g_lasso_points.clear();
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Lasso Selection Tool (L)");

        /* Eyedropper tool — left-click picks the underlying palette color
           (same as right-click in any tool mode, but as the explicit primary
           action). Matches Photoshop's `I` shortcut. */
        ImGui::PushStyleColor(ImGuiCol_Button, g_active_tool == ActiveTool::Eyedropper ?
            ImVec4(0.6f,0.7f,0.2f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL("\xEF\x8D\x91", "Ey"), btn)) { /* U+F351 dropper_eye */
            if (g_active_tool == ActiveTool::Eyedropper) g_active_tool = ActiveTool::None;
            else g_active_tool = ActiveTool::Eyedropper;
            if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Eyedropper Tool (I) - click to pick palette color");

        /* Tool-options strip — appears inline when a configurable tool is active. */
        if (g_active_tool == ActiveTool::Pencil) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderInt("Brush##pencil", &g_pencil_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pencil brush radius (1 = single pixel)\n[ / ] also shrink / grow");
        } else if (g_active_tool == ActiveTool::PaintBucket) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::SliderInt("Tol##bucket", &g_bucket_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Fill tolerance (palette-index distance)");
            ImGui::SameLine();
            ImGui::Checkbox("Contig##bucket", &g_bucket_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If off: replace every matching pixel globally");
        } else if (g_active_tool == ActiveTool::VariantPaint) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderInt("Brush##variant", &g_variant_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Variant brush radius (1 = single pixel)\n[ / ] also shrink / grow");
        } else if (g_active_tool == ActiveTool::MagicWand) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::SliderInt("Strength##wand", &g_wand_tolerance, 0, 64);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Magic Wand strength (palette-index distance)");
            ImGui::SameLine();
            ImGui::Checkbox("Contig##wand", &g_wand_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If off: select all matching pixels globally. Ctrl-click still adds.");
        } else if (g_active_tool == ActiveTool::CloneStamp) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            ImGui::SliderInt("Brush##clone", &g_clone_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clone Stamp brush radius (1 = single pixel)");
            ImGui::SameLine();
            ImGui::TextDisabled(g_clone_source_set ? "src set (Alt+click resets)" : "Alt+click to set source");
        } else if (g_active_tool == ActiveTool::BackgroundEraser) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::SliderInt("Tol##eraser", &g_eraser_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Chroma tolerance (palette-index distance)");
            ImGui::SameLine();
            ImGui::Checkbox("Contig##eraser", &g_eraser_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If off: erase every matching pixel globally");
            ImGui::SameLine();
            ImGui::Checkbox("Defringe##eraser", &g_eraser_defringe);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("After erase, average-replace 1px halo around transparent edges (kills blue-spill)");
        } else if (g_active_tool == ActiveTool::SmartRemap) {
            ImGui::SameLine();
            ImGui::TextDisabled("|");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::SliderInt("Tol##remap", &g_remap_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Remap tolerance (palette-index distance)");
            if (g_remap_target_color >= 0) {
                ImGui::SameLine();
                ImGui::TextDisabled("->");
                ImGui::SameLine();
                ImVec4 c(1,1,1,1);
                PAL *p = (g_doc->ilselected >= 0) ? get_pal(get_img(g_doc->ilselected) ? get_img(g_doc->ilselected)->palnum : 0) : NULL;
                if (p && p->data_p) {
                    unsigned char *pd = (unsigned char *)p->data_p;
                    int ci = g_remap_target_color;
                    unsigned short w15 = (unsigned short)(pd[ci*2] | (pd[ci*2+1] << 8));
                    c = ImVec4(((w15 >> 10) & 0x1F) / 31.f,
                               ((w15 >>  5) & 0x1F) / 31.f,
                               ( w15        & 0x1F) / 31.f, 1.0f);
                }
                ImGui::ColorButton("##remap_target", c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoBorder, ImVec2(16,16));
                ImGui::SameLine();
                if (ImGui::SmallButton("Clear##remap")) g_remap_target_color = -1;
            }
        }

        ImGui::Spacing();
        bool can_undo = CanUndo();
        bool can_redo = CanRedo();
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button(TB_LABEL(ICON_UNDO, ICON_UNDO_TXT), btn)) DoUndo();
        if (!can_undo) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo (Ctrl+Z)");
        if (!can_redo) ImGui::BeginDisabled();
        if (ImGui::Button(TB_LABEL(ICON_REDO, ICON_REDO_TXT), btn)) DoRedo();
        if (!can_redo) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Redo (Ctrl+Y)");

        #undef TB_LABEL

        /* Selected color swatch */
        ImGui::Spacing();
        {
            SDL_Color &c = g_palette[g_sel_color];
            ImU32 col = IM_COL32(c.r, c.g, c.b, 255);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float sw_sz = ImGui::GetContentRegionAvail().x;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(cp, ImVec2(cp.x + sw_sz, cp.y + 24), col);
            dl->AddRect(cp, ImVec2(cp.x + sw_sz, cp.y + 24), IM_COL32(255,255,255,80));
            ImGui::Dummy(ImVec2(sw_sz, 24));
        }
        char col_label[8];
        snprintf(col_label, sizeof(col_label), "#%d", g_sel_color);
        if (ImGui::SmallButton(col_label)) {
            static int last_col = 1;
            if (g_sel_color == 0) g_sel_color = last_col;
            else { last_col = g_sel_color; g_sel_color = 0; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active color index (right-click sprite to pick)");
#endif
        #undef TB_LABEL
    }
    ImGui::End();

    /* ===== RIGHT PANEL STRIP ===== */
    float panel_x = sw - PANEL_W;
    float panel_y = work_y;
    float panel_h = work_h - PALETTE_H - TIMELINE_H;

    ImGui::SetNextWindowPos(ImVec2(panel_x, panel_y));
    ImGui::SetNextWindowSize(ImVec2(PANEL_W, panel_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x06, 0x06, 0x06, 0xFF));
    ImGui::Begin("##panels", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);
    {
        /* --- Image List --- */
        int n_imgs = count_imgs();
        if (ImGui::CollapsingHeader("Images", ImGuiTreeNodeFlags_DefaultOpen)) {
            float list_h = panel_h * 0.30f;
            if (ImGui::BeginListBox("##imglist", ImVec2(-1, list_h))) {
                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    g_palette_nav = false;
                /* Auto-scroll: when g_doc->ilselected changes (typically via Up/Down
                   keyboard nav, but also Prev/Next-Marked jumps or programmatic
                   selection), make sure the selected row is visible. Without
                   this the scroll bar stays put and the user loses track of
                   where they are in a long sprite list. */
                static int last_scrolled_to = -2;
                bool need_scroll = (g_doc->ilselected != last_scrolled_to);

                struct ImagePanelRow {
                    int idx;
                    IMG *img;
                    std::string name;
                    std::string src;
                    int area;
                    int parent_row;
                    std::string virtual_parent;
                    std::vector<int> children;
                };

                std::vector<ImagePanelRow> rows;
                std::vector<std::string> source_groups;
                rows.reserve(n_imgs);
                for (int i = 0; i < n_imgs; i++) {
                    IMG *img = get_img(i);
                    if (!img) break;
                    const char *src_name = img->src_filename[0] ? img->src_filename : "Workspace";
                    bool have_group = false;
                    for (const std::string &g : source_groups) {
                        if (g == src_name) { have_group = true; break; }
                    }
                    if (!have_group) source_groups.push_back(src_name);
                    rows.push_back({i, img, img->n_s, src_name,
                                    (int)img->w * (int)img->h, -1,
                                    std::string(), {}});
                }
                for (int r = 0; r < (int)rows.size(); r++) {
                    std::string parent_name = InferSubframeParentName(rows[r].name.c_str());
                    if (parent_name.empty()) continue;
                    bool found_parent = false;
                    for (int p = 0; p < (int)rows.size(); p++) {
                        if (p == r) continue;
                        if (rows[p].src == rows[r].src && rows[p].name == parent_name) {
                            rows[r].parent_row = p;
                            rows[p].children.push_back(r);
                            found_parent = true;
                            break;
                        }
                    }
                    if (!found_parent)
                        rows[r].virtual_parent = parent_name;
                }
                for (int r = 0; r < (int)rows.size(); r++) {
                    if (rows[r].virtual_parent.empty()) continue;
                    int siblings = 0;
                    for (const ImagePanelRow &other : rows) {
                        if (other.src == rows[r].src &&
                            other.virtual_parent == rows[r].virtual_parent)
                            siblings++;
                    }
                    if (siblings < 2)
                        rows[r].virtual_parent.clear();
                }

                auto row_less = [&](int a, int b) {
                    const ImagePanelRow &ra = rows[a];
                    const ImagePanelRow &rb = rows[b];
                    if (g_image_list_sort == ImageListSort::Name && ra.name != rb.name)
                        return g_image_list_sort_desc ? (ra.name > rb.name) : (ra.name < rb.name);
                    if (g_image_list_sort == ImageListSort::Size && ra.area != rb.area)
                        return g_image_list_sort_desc ? (ra.area > rb.area) : (ra.area < rb.area);
                    return g_image_list_sort_desc ? (ra.idx > rb.idx) : (ra.idx < rb.idx);
                };

                auto draw_image_context = [&](int img_idx) {
                    if (ImGui::BeginPopupContextItem("##imgctx")) {
                        g_doc->ilselected = img_idx;
                        IMG *ctx_img = get_img(img_idx);
                        if (ImGui::MenuItem("Mark / Unmark") && ctx_img) { ctx_img->flags ^= 1; }
                        if (ImGui::MenuItem("Rename"))        OpenRenameImage();
                        if (ImGui::MenuItem("Duplicate"))     DuplicateImage();
                        if (ImGui::MenuItem("Resize..."))     OpenResizeSpriteDialog();
                        if (ImGui::BeginMenu("Transform")) {
                            DrawSpriteTransformMenuItems();
                            ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem("Trim Bounds")) {
                            doc_undo_push();
                            int n = CropSelectedImageToContent();
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     n > 0 ? "Trimmed selected sprite to non-transparent bounds."
                                           : "Selected sprite already fits, or has no opaque pixels.");
                            g_restore_msg_timer = 4.0f;
                            if (n > 0) g_zoom_reset = true;
                        }
                        if (ImGui::MenuItem("Delete"))        RequestDeleteImage(g_doc->ilselected);
                        if (ImGui::MenuItem("Delete Marked", NULL, false, CountMarkedImages() > 0))
                            RequestDeleteMarkedImages();
                        ImGui::Separator();
                        if (ImGui::MenuItem("Build TGA"))     OpenFileDialog(FileDialogMode::ExportTga);
                        if (ImGui::MenuItem("Set Palette"))   SetPaletteOfSelected();
                        ImGui::EndPopup();
                    }
                };

                auto draw_expand_triangle = [&](bool open) {
                    ImVec2 item_min = ImGui::GetItemRectMin();
                    ImVec2 item_max = ImGui::GetItemRectMax();
                    ImVec2 arrow_min(item_max.x - 18.0f, item_min.y);
                    ImVec2 arrow_max(item_max.x - 2.0f, item_max.y);
                    bool hovered = ImGui::IsMouseHoveringRect(arrow_min, arrow_max);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImU32 col = hovered ? IM_COL32(255, 235, 140, 255) : IM_COL32(235, 235, 235, 210);
                    float cx = arrow_min.x + 8.0f;
                    float cy = (item_min.y + item_max.y) * 0.5f;
                    if (open) {
                        dl->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f),
                                              ImVec2(cx + 4.0f, cy - 2.0f),
                                              ImVec2(cx,        cy + 4.0f), col);
                    } else {
                        dl->AddTriangleFilled(ImVec2(cx - 2.0f, cy - 4.0f),
                                              ImVec2(cx - 2.0f, cy + 4.0f),
                                              ImVec2(cx + 4.0f, cy),        col);
                    }
                    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                };

                auto draw_leaf = [&](int row_id, bool subframe) {
                    ImagePanelRow &row = rows[row_id];
                    IMG *img = row.img;
                    bool marked   = (img->flags & 1) != 0;
                    bool selected = (row.idx == g_doc->ilselected);
                    ImGui::PushID(row.idx);
                    if (subframe) ImGui::Indent(18.0f);

                    char label[96];
                    const char *vis_icon = marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    const char *row_icon = subframe
                        ? (g_icon_font_loaded ? ICON_SUBFRAME : ICON_SUBFRAME_TXT)
                        : (g_icon_font_loaded ? ICON_IMAGE : ICON_IMAGE_TXT);
                    snprintf(label, sizeof(label), "%s %s  %s", vis_icon, row_icon, img->n_s);

                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                    }
                    if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        g_doc->ilselected = row.idx;
                        g_palette_nav = false;
                        if (ImGui::IsMouseDoubleClicked(0)) img->flags ^= 1;
                    }
                    if (selected && need_scroll && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                    if (selected && need_scroll) last_scrolled_to = g_doc->ilselected;
                    if (selected) ImGui::PopStyleColor(2);
                    draw_image_context(row.idx);
                    if (subframe) ImGui::Unindent(18.0f);
                    ImGui::PopID();
                };

                auto draw_parent_group = [&](int row_id) {
                    ImagePanelRow &row = rows[row_id];
                    IMG *img = row.img;
                    bool marked   = (img->flags & 1) != 0;
                    bool selected = (row.idx == g_doc->ilselected);
                    ImGui::PushID(row.idx);

                    ImGuiStorage *storage = ImGui::GetStateStorage();
                    ImGuiID open_id = ImGui::GetID("subframes_open");
                    bool open = storage->GetBool(open_id, true);

                    char label[96];
                    const char *vis_icon = marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    const char *img_icon = g_icon_font_loaded ? ICON_IMAGE : ICON_IMAGE_TXT;
                    snprintf(label, sizeof(label), "%s %s  %s", vis_icon, img_icon, img->n_s);

                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                    }
                    bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick);
                    bool toggle_clicked = draw_expand_triangle(open);
                    if (toggle_clicked) {
                        open = !open;
                        storage->SetBool(open_id, open);
                    }
                    if (clicked) {
                        g_doc->ilselected = row.idx;
                        g_palette_nav = false;
                        if (ImGui::IsMouseDoubleClicked(0) && !toggle_clicked) img->flags ^= 1;
                    }
                    if (selected && need_scroll && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                    if (selected && need_scroll) last_scrolled_to = g_doc->ilselected;
                    if (selected) ImGui::PopStyleColor(2);
                    draw_image_context(row.idx);

                    if (open) {
                        std::vector<int> child_rows = row.children;
                        std::stable_sort(child_rows.begin(), child_rows.end(), row_less);
                        for (int child_id : child_rows) draw_leaf(child_id, true);
                    }
                    ImGui::PopID();
                };

                auto collect_virtual_children = [&](const std::string &src,
                                                    const std::string &parent,
                                                    std::vector<int> &out) {
                    out.clear();
                    for (int r = 0; r < (int)rows.size(); r++) {
                        if (rows[r].src == src && rows[r].virtual_parent == parent)
                            out.push_back(r);
                    }
                };

                auto virtual_group_stats = [&](const std::string &src,
                                               const std::string &parent,
                                               int *order, int *area) {
                    int best_order = 0x7FFFFFFF;
                    int best_area = 0;
                    for (const ImagePanelRow &row : rows) {
                        if (row.src != src || row.virtual_parent != parent) continue;
                        if (row.idx < best_order) best_order = row.idx;
                        if (row.area > best_area) best_area = row.area;
                    }
                    if (order) *order = (best_order == 0x7FFFFFFF) ? 0 : best_order;
                    if (area) *area = best_area;
                };

                auto draw_virtual_group = [&](const std::string &src,
                                              const std::string &parent) {
                    std::vector<int> child_rows;
                    collect_virtual_children(src, parent, child_rows);
                    if (child_rows.empty()) return;
                    std::stable_sort(child_rows.begin(), child_rows.end(), row_less);

                    bool any_marked = false;
                    bool any_selected = false;
                    for (int child_id : child_rows) {
                        IMG *img = rows[child_id].img;
                        if (img && (img->flags & 1)) any_marked = true;
                        if (rows[child_id].idx == g_doc->ilselected) any_selected = true;
                    }

                    const char *vis_icon = any_marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    char label[96];
                    snprintf(label, sizeof(label), "%s     %s", vis_icon, parent.c_str());

                    ImGui::PushID(src.c_str());
                    ImGui::PushID(parent.c_str());
                    ImGuiStorage *storage = ImGui::GetStateStorage();
                    ImGuiID open_id = ImGui::GetID("virtual_subframes_open");
                    bool open = storage->GetBool(open_id, true);
                    if (any_selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                    }
                    bool clicked = ImGui::Selectable(label, any_selected);
                    bool toggle_clicked = draw_expand_triangle(open);
                    if (toggle_clicked) {
                        open = !open;
                        storage->SetBool(open_id, open);
                    }
                    if (clicked) {
                        g_doc->ilselected = rows[child_rows[0]].idx;
                        g_palette_nav = false;
                    }
                    if (any_selected) ImGui::PopStyleColor(2);
                    if (open) {
                        for (int child_id : child_rows) draw_leaf(child_id, true);
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                };

                for (const std::string &src_group : source_groups) {
                    ImGui::PushID(src_group.c_str());
                    char group_label[96];
                    snprintf(group_label, sizeof(group_label), "%s  %s",
                             g_icon_font_loaded ? ICON_FOLDER : ICON_FOLDER_TXT,
                             src_group.c_str());
                    bool group_open = ImGui::TreeNodeEx(group_label,
                        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
                    ImGui::PopID();
                    if (!group_open) continue;

                    struct ImagePanelItem {
                        bool is_virtual;
                        int row_id;
                        std::string parent;
                    };

                    std::vector<ImagePanelItem> items;
                    for (int r = 0; r < (int)rows.size(); r++) {
                        if (rows[r].src == src_group &&
                            rows[r].parent_row < 0 &&
                            rows[r].virtual_parent.empty())
                            items.push_back({false, r, std::string()});
                    }
                    std::vector<std::string> virtual_parents;
                    for (const ImagePanelRow &row : rows) {
                        if (row.src != src_group || row.virtual_parent.empty()) continue;
                        bool seen = false;
                        for (const std::string &parent : virtual_parents) {
                            if (parent == row.virtual_parent) { seen = true; break; }
                        }
                        if (!seen) virtual_parents.push_back(row.virtual_parent);
                    }
                    for (const std::string &parent : virtual_parents)
                        items.push_back({true, -1, parent});

                    auto item_less = [&](const ImagePanelItem &a, const ImagePanelItem &b) {
                        std::string an = a.is_virtual ? a.parent : rows[a.row_id].name;
                        std::string bn = b.is_virtual ? b.parent : rows[b.row_id].name;
                        int aa = 0, ba = 0;
                        int ai = 0, bi = 0;
                        if (a.is_virtual) virtual_group_stats(src_group, a.parent, &ai, &aa);
                        else { ai = rows[a.row_id].idx; aa = rows[a.row_id].area; }
                        if (b.is_virtual) virtual_group_stats(src_group, b.parent, &bi, &ba);
                        else { bi = rows[b.row_id].idx; ba = rows[b.row_id].area; }

                        if (g_image_list_sort == ImageListSort::Name && an != bn)
                            return g_image_list_sort_desc ? (an > bn) : (an < bn);
                        if (g_image_list_sort == ImageListSort::Size && aa != ba)
                            return g_image_list_sort_desc ? (aa > ba) : (aa < ba);
                        return g_image_list_sort_desc ? (ai > bi) : (ai < bi);
                    };
                    std::stable_sort(items.begin(), items.end(), item_less);

                    for (const ImagePanelItem &item : items) {
                        if (item.is_virtual) {
                            draw_virtual_group(src_group, item.parent);
                            continue;
                        }

                        int row_id = item.row_id;
                        ImagePanelRow &row = rows[row_id];
                        if (row.children.empty()) {
                            draw_leaf(row_id, false);
                            continue;
                        }

                        draw_parent_group(row_id);
                    }
                    ImGui::TreePop();
                }
                ImGui::EndListBox();
            }
            const char *sort_labels[] = { "Order", "Name", "Size" };
            int img_sort_idx = (int)g_image_list_sort;
            ImGui::TextUnformatted("Sort");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(92.0f);
            if (ImGui::Combo("##imgsort", &img_sort_idx, sort_labels, 3))
                g_image_list_sort = (ImageListSort)img_sort_idx;
            ImGui::SameLine();
            if (ImGui::SmallButton(g_image_list_sort_desc ? "v##imgsort" : "^##imgsort"))
                g_image_list_sort_desc = !g_image_list_sort_desc;
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle image list sort direction");

            /* Mark and edit buttons below list. Keep them in short rows so
               the fixed-width side panel never clips the rightmost actions. */
            int n_marked_imgs = CountMarkedImages();
            if (ImGui::SmallButton("Mk All"))   { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clr All"))  { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Invert"))   { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mk Sel##img")) { IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark / unmark selected sprite");

            if (ImGui::SmallButton("Add##img")) { g_show_new_blank_dialog = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new blank image (W/H prompt)");
            ImGui::SameLine();
            if (g_doc->ilselected < 0) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Dup##img")) DuplicateImage();
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate selected sprite (Ctrl+J)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Resize##img")) OpenResizeSpriteDialog();
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Resize selected sprite");
            ImGui::SameLine();
            if (ImGui::SmallButton("Trim##img")) {
                doc_undo_push();
                int n = CropSelectedImageToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Trimmed selected sprite to non-transparent bounds."
                               : "Selected sprite already fits, or has no opaque pixels.");
                g_restore_msg_timer = 4.0f;
                if (n > 0) g_zoom_reset = true;
            }
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Remove transparent padding from selected sprite");
            ImGui::SameLine();
            if (ImGui::SmallButton("Del##img")) RequestDeleteImage(g_doc->ilselected);
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Delete selected sprite (Del)");
            if (g_doc->ilselected < 0) ImGui::EndDisabled();

            if (g_doc->ilselected < 0) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Copy+##img")) CopySelectionToNewImage();
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Copy selection, or the whole sprite, into a new sprite (Ctrl+Shift+C)");
            ImGui::SameLine();
            if (ImGui::SmallButton("Cut+##img")) CutSelectionToNewImage();
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Cut selection, or the whole sprite, into a new sprite (Ctrl+Shift+X)");
            ImGui::SameLine();
            if (g_doc->ilselected < 0) ImGui::EndDisabled();
            if (!g_clipboard.valid) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Paste+##img")) PasteClipboardAsNewImage();
            if (g_clipboard.valid && ImGui::IsItemHovered())
                ImGui::SetTooltip("Paste clipboard as a new sprite (Ctrl+Shift+V)");
            if (!g_clipboard.valid) ImGui::EndDisabled();
            ImGui::SameLine();
            if (n_marked_imgs == 0) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Bulk Size##img")) OpenBulkResizeDialog();
            if (n_marked_imgs > 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Resize every marked sprite by percentage");
            ImGui::SameLine();
            if (ImGui::SmallButton("Bulk Rename##img")) OpenRenameMarkedImages();
            if (n_marked_imgs > 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Rename marked sprites as Base1, Base2, Base3...");
            ImGui::SameLine();
            if (ImGui::SmallButton("Del Marked##img")) RequestDeleteMarkedImages();
            if (n_marked_imgs > 0 && ImGui::IsItemHovered())
                ImGui::SetTooltip("Delete marked sprites");
            if (n_marked_imgs == 0) ImGui::EndDisabled();

            bool can_break_subframes = (n_marked_imgs > 0 || g_doc->ilselected >= 0);
            if (!can_break_subframes) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Break Sub##img")) OpenAutoChopDialog();
            if (can_break_subframes && ImGui::IsItemHovered())
                ImGui::SetTooltip("Break marked sprites, or selected sprite if none are marked, into A/B/C subframes");
            if (!can_break_subframes) ImGui::EndDisabled();
        }

        /* --- Palette List --- */
        int n_pals = count_pals();
        if (ImGui::CollapsingHeader("Palettes", ImGuiTreeNodeFlags_DefaultOpen)) {
            float list_h = panel_h * 0.22f;
            if (ImGui::BeginListBox("##pallist", ImVec2(-1, list_h))) {
                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    g_palette_nav = true;
                for (int i = 0; i < n_pals; i++) {
                    PAL *pal = get_pal(i);
                    if (!pal) break;
                    bool sel    = (i == g_doc->plselected);
                    bool marked = (pal->flags & 1) != 0;
                    ImGui::PushID(1000 + i);
                    char label[16];
                    if (marked) snprintf(label, sizeof(label), "* %s", pal->n_s);
                    else        snprintf(label, sizeof(label), "  %s", pal->n_s);
                    
                    if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                    if (ImGui::Selectable(label, sel)) {
                        SelectPalette(i);
                    }
                    if (sel) ImGui::PopStyleColor();

                    /* Right-click context menu */
                    if (ImGui::BeginPopupContextItem("##palctx")) {
                        if (ImGui::MenuItem("Mark / Unmark"))             pal->flags ^= 1;
                        ImGui::Separator();
                        if (ImGui::MenuItem("Add New"))                   AddNewPalette();
                        if (ImGui::MenuItem("Duplicate"))                 DuplicatePalette();
                        ImGui::Separator();
                        if (ImGui::MenuItem("Set for Image",         "]"))       SetPaletteOfSelected();
                        if (ImGui::MenuItem("Set for Marked Images", "["))       SetPaletteOfMarked();
                        if (ImGui::MenuItem("Merge Marked into Selected", "*"))  MergeMarkedPalettes();
                        if (ImGui::MenuItem("Preview Merge"))                    OpenPaletteMergePreview();
                        ImGui::Separator();
                        if (ImGui::MenuItem("Clean Up Palette")) CleanupSelectedPalette();
                        if (ImGui::MenuItem("Clean Copy Palette")) CreateCleanedPaletteCopy();
                        if (ImGui::MenuItem("Inherit Colors from Marked")) InheritSelectedPaletteFromMarked();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "Mark the source palette, select the target palette.\n"
                            "Sprites using the target are remapped to nearest source colors.");
                        if (ImGui::MenuItem("Merge Duplicate Palettes")) MergeDuplicatePalettes();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "Remaps sprites using later duplicate palettes to the first matching palette.");
                        if (ImGui::MenuItem("Downscale Palette...")) OpenPaletteReduceDialog(7);
                        if (ImGui::MenuItem("Copy #0 to Opaque Slot")) CopyPaletteZeroToOpaqueSlot();
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                            "Copies palette index 0's RGB into a nonzero slot.\n"
                            "Pixels with index 0 still remain transparent.");
                        if (ImGui::MenuItem("Import Palette...")) OpenFileDialog(FileDialogMode::ImportPalette);
                        if (ImGui::MenuItem("Export Palette...")) OpenFileDialog(FileDialogMode::ExportPalette);
                        if (ImGui::MenuItem("Show Histogram")) { CalculatePaletteHistogram(); g_show_histogram = true; }
                        if (ImGui::MenuItem("Rename", "Shift+R")) OpenRenamePalette(i);
                        if (ImGui::MenuItem("Delete", "Del")) DeletePalette();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            /* Palette mark buttons (wrapped to 2 rows) */
            ImGui::PushID("palette_controls");
            if (ImGui::SmallButton("Mk All"))    { PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clr All"))   { PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags&=~1;p=(PAL*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Invert"))    { PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mk"))        { PAL *p=get_pal(g_doc->plselected); if(p) p->flags^=1; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark / unmark selected palette");
            ImGui::SameLine();
            if (ImGui::SmallButton("Add")) AddNewPalette();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new blank 256-color palette");

            if (ImGui::SmallButton("Merge"))     MergeMarkedPalettes();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merge marked palettes into selected palette");
            ImGui::SameLine();
            if (ImGui::SmallButton("Prev"))      OpenPaletteMergePreview();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Preview marked-palette merge before applying it");
            ImGui::SameLine();
            if (ImGui::SmallButton("Dup"))       DuplicatePalette();
            ImGui::SameLine();
            if (ImGui::SmallButton("Del"))       DeletePalette();
            ImGui::SameLine();
            if (ImGui::SmallButton("Copy"))      CopyPaletteToClipboard();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy selected palette to cross-file clipboard");
            ImGui::SameLine();
            if (!g_pal_clipboard.valid) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Paste"))     PastePaletteFromClipboard();
            if (g_pal_clipboard.valid && ImGui::IsItemHovered())
                ImGui::SetTooltip("Paste clipboard palette as new (%s, %d colors)",
                                  g_pal_clipboard.n_s, (int)g_pal_clipboard.numc);
            if (!g_pal_clipboard.valid) ImGui::EndDisabled();

            if (ImGui::SmallButton("Clean")) CleanupSelectedPalette();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pack used colors into visible ramps and remap images using this palette");
            ImGui::SameLine();
            if (ImGui::SmallButton("Clean+")) CreateCleanedPaletteCopy();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Create a new cleaned palette copy without remapping sprites");
            ImGui::SameLine();
            if (ImGui::SmallButton("Dups")) MergeDuplicatePalettes();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Merge duplicate palettes into the first matching palette");
            ImGui::SameLine();
            if (ImGui::SmallButton("Inh")) InheritSelectedPaletteFromMarked();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Inherit colors from marked source palette into selected target");
            ImGui::SameLine();
            if (ImGui::SmallButton("Bpp-")) OpenPaletteReduceDialog(7);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Downscale selected palette to 8, 7, 6, 5, or 4 bpp with preview");
            ImGui::SameLine();
            if (ImGui::SmallButton("#0>")) CopyPaletteZeroToOpaqueSlot();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copy transparent color #0 to the first safe opaque slot");
            ImGui::SameLine();
            if (ImGui::SmallButton("Export")) OpenFileDialog(FileDialogMode::ExportPalette);
            ImGui::PopID();
        }

        /* --- Color: kept directly under Palettes for palette-first editing. --- */
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
        if (ImGui::CollapsingHeader("Color##quick", ImGuiTreeNodeFlags_DefaultOpen)) {
            auto begin_palette_drag_undo = []() {
                if (!g_palette_drag_undo_active) {
                    doc_undo_push();
                    g_palette_drag_undo_active = true;
                }
            };
            SDL_Color &col = g_palette[g_sel_color];
            int r = col.r, g = col.g, b = col.b;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("R##quick_cr", &r, 0, 255)) {
                begin_palette_drag_undo();
                col.r = (unsigned char)r;
                palette_writeback(g_sel_color);
                commit_palette_adjustments();
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("G##quick_cg", &g, 0, 255)) {
                begin_palette_drag_undo();
                col.g = (unsigned char)g;
                palette_writeback(g_sel_color);
                commit_palette_adjustments();
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("B##quick_cb", &b, 0, 255)) {
                begin_palette_drag_undo();
                col.b = (unsigned char)b;
                palette_writeback(g_sel_color);
                commit_palette_adjustments();
            }
            if (g_palette_drag_undo_active && !ImGui::IsAnyItemActive())
                g_palette_drag_undo_active = false;
        }

        /* --- Color tools: remap and palette-wide adjustments. --- */
        if (ImGui::CollapsingHeader("Color Tools")) {
            auto begin_palette_drag_undo = []() {
                if (!g_palette_drag_undo_active) {
                    doc_undo_push();
                    g_palette_drag_undo_active = true;
                }
            };
            PAL *active_pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
            bool can_copy_zero = active_pal && active_pal->data_p;
            if (!can_copy_zero) ImGui::BeginDisabled();
            if (g_sel_color == 0) {
                if (ImGui::SmallButton("Copy #0 to Free Slot")) {
                    CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Copies the RGB stored at transparent index 0 into\n"
                    "the first safe nonzero palette slot and selects it.");
            } else {
                if (ImGui::SmallButton("Copy #0 Here")) {
                    CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None, g_sel_color);
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Copies transparent index 0's RGB into the selected\n"
                    "nonzero swatch. Existing pixels using this swatch change color.");
            }
            bool has_selection = g_grid_sel.active;
            if (!has_selection) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Remap Selection")) {
                CopyPaletteZeroAndRemap(PaletteZeroRemapMode::Selection,
                                        g_sel_color > 0 ? g_sel_color : -1);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Copies #0 to an opaque slot, then changes selected\n"
                "pixels with index 0 to that slot.");
            if (!has_selection) ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::SmallButton("Remap Sprite")) {
                CopyPaletteZeroAndRemap(PaletteZeroRemapMode::CurrentImage,
                                        g_sel_color > 0 ? g_sel_color : -1);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Copies #0 to an opaque slot, then changes every index-0\n"
                "pixel in the current sprite to that slot. Transparent padding\n"
                "will become opaque too.");
            if (!can_copy_zero) ImGui::EndDisabled();
            ImGui::Separator();
            bool any_sel = false;
            for (int psi = 0; psi < 256; psi++) if (g_palette_selection[psi]) { any_sel = true; break; }
            if (any_sel) {
                int n_sel = 0;
                for (int psi = 0; psi < 256; psi++) if (g_palette_selection[psi]) n_sel++;
                ImGui::TextDisabled("HSL adjustments target %d selected swatch%s (Ctrl/Shift-click to add).",
                                    n_sel, n_sel == 1 ? "" : "es");
            } else {
                ImGui::TextDisabled("HSL adjustments target the whole palette. Ctrl/Shift-click swatches to scope to a subset.");
            }
            ImGui::Text("Hue");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##hue", &g_hue_slider, -180, 180)) {
                begin_palette_drag_undo();
                g_hue_last = g_hue_slider;
                hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
            }
            ImGui::Text("Saturation");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##sat", &g_sat_slider, -100, 100, "%d%%")) {
                begin_palette_drag_undo();
                g_sat_last = g_sat_slider;
                hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("-100 = grayscale, +100 = fully saturated. Makes a yellow more yellow at positive values.");
            ImGui::Text("Lightness");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##light", &g_light_slider, -100, 100, "%d%%")) {
                begin_palette_drag_undo();
                g_light_last = g_light_slider;
                hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("-100 = black, +100 = white.");
            if (ImGui::SmallButton("Reset HSL")) {
                doc_undo_push();
                reset_palette_adjust_sliders();
                reset_palette_to_baseline();
            }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Reset Hue/Saturation/Lightness sliders to 0 and restore the palette baseline.");
            ImGui::SameLine();
            if (ImGui::SmallButton("New from HSL")) {
                PAL *src = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
                if (src && src->data_p) {
                    doc_undo_push();
                    PAL *pal = (PAL *)AllocPal();
                    if (pal) {
                        pal->flags   = src->flags;
                        pal->bitspix = src->bitspix;
                        pal->numc    = src->numc;
                        pal->pad     = 0;
                        memcpy(pal->n_s, src->n_s, 10);
                        unsigned int col_sz = (unsigned int)pal->numc * 2;
                        unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
                        if (buf) {
                            pal->data_p = buf;
                            memcpy(buf, src->data_p, col_sz);
                            if (g_doc->palcnt > 0) g_doc->plselected = (int)g_doc->palcnt - 1;
                            ApplyPalette(g_doc->plselected);
                            commit_palette_adjustments();
                            mark_dirty();
                        }
                    }
                }
            }
            if (g_palette_drag_undo_active && !ImGui::IsAnyItemActive())
                g_palette_drag_undo_active = false;
            ImGui::Separator();
            if (ImGui::SmallButton("Variant Selection")) ApplyVariantToSelection();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use the current swatch as the target-palette color,\n"
                                  "but keep selected pixels visually unchanged on other palettes.");
            ImGui::SameLine();
            if (ImGui::SmallButton("Remap Similar")) ApplySelectionRemapToMatchingSprites();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use the current selection as a sample, then remap likely\n"
                                  "matching regions in every same-palette sprite to the\n"
                                  "current swatch index.");
            if (ImGui::SmallButton("Split Overlay")) SplitSelectionToOverlayFrame(true);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Move selected opaque pixels into a new transparent overlay frame.");
        }

        /* --- Anipts: close to palette/color controls for sprite alignment. --- */
        if (ImGui::CollapsingHeader("Anipts##quick")) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                int ax = (short)img->anix, ay = (short)img->aniy;
                int ax2 = (short)img->anix2, ay2 = (short)img->aniy2, az2 = (short)img->aniz2;
                if (AnimPointSliderInt("X1##quick_ptx",  &ax,  -1024, 1024))
                    set_primary_anipoint_with_sequence(img, ax, (int)(short)img->aniy);
                if (AnimPointSliderInt("Y1##quick_pty",  &ay,  -1024, 1024))
                    set_primary_anipoint_with_sequence(img, (int)(short)img->anix, ay);
                if (AnimPointSliderInt("X2##quick_ptx2", &ax2, -1024, 1024)) {
                    int cur_y2 = secondary_anipoint_in_use(img) ? (int)(short)img->aniy2 : 0;
                    set_secondary_anipoint_with_sequence(img, ax2, cur_y2);
                }
                if (AnimPointSliderInt("Y2##quick_pty2", &ay2, -1024, 1024)) {
                    int cur_x2 = secondary_anipoint_in_use(img) ? (int)(short)img->anix2 : 0;
                    set_secondary_anipoint_with_sequence(img, cur_x2, ay2);
                }
                if (AnimPointSliderInt("AZ2##quick_ptz2", &az2, -1024, 1024)) {
                    if (begin_sequence_anipoint_edit()) {
                        if (az2 == -1) clear_secondary_anipoint(img);
                        else {
                            activate_secondary_anipoint(img);
                            img->aniz2 = (unsigned short)(short)az2;
                        }
                    }
                }
                if (ImGui::SmallButton("Default Center##quick_anipts")) {
                    set_primary_anipoint_with_sequence(img,
                                                       (int)img->w / 2,
                                                       (int)img->h / 2);
                    if (begin_sequence_anipoint_edit())
                        clear_secondary_anipoint(img);
                    g_img_tex_idx = -2;
                }
                ImGui::SameLine();
                bool had_second_point = secondary_anipoint_in_use(img);
                if (!had_second_point) ImGui::BeginDisabled();
                if (ImGui::SmallButton("Clear 2nd##quick_anipts")) {
                    if (begin_sequence_anipoint_edit())
                        clear_secondary_anipoint(img);
                }
                if (!had_second_point) ImGui::EndDisabled();
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                LabeledValue("Name:", "%.15s", img->n_s);
                LabeledValue("Size:", "%d x %d", (int)img->w, (int)img->h);

                if (img->data_p && img->w > 0 && img->h > 0) {
                    int uncomp_size = img->w * img->h;
                    int comp_size = 0;
                    unsigned short stride = (img->w + 3) & ~3;
                    unsigned char *pixels = (unsigned char *)img->data_p;
                    for (int y = 0; y < img->h; y++) {
                        int leading = 0;
                        while (leading < img->w && pixels[y * stride + leading] == 0) leading++;
                        if (leading == img->w) {
                            comp_size += 1; /* completely empty line: 1 byte header, 0 pixels */
                        } else {
                            int trailing = 0;
                            while (trailing < img->w && pixels[y * stride + (img->w - 1 - trailing)] == 0) trailing++;
                            comp_size += 1 + (img->w - leading - trailing);
                        }
                    }
                    LabeledValue("DMA ROM:", "%d B raw", uncomp_size);
                    LabeledValue("",         "%d B compressed", comp_size);
                }

                PAL *pal = get_pal(img->palnum);
                if (pal) LabeledValue("Pal:", "%d  %.9s", (int)img->palnum, pal->n_s);
                else     LabeledValue("Pal:", "%d", (int)img->palnum);

                LabeledValue("AX/AY:",   "%d, %d", (int)(short)img->anix,  (int)(short)img->aniy);
                LabeledValue("AX2/AY2:", "%d, %d", (int)(short)img->anix2, (int)(short)img->aniy2);
                LabeledValue("AZ2:",     "%d",     (int)(short)img->aniz2);

                char flagbuf[48] = {};
                if (img->flags & 1)  strncat(flagbuf, "Marked ", 47);
                if (img->flags & 2)  strncat(flagbuf, "Loaded ", 47);
                if (img->flags & 4)  strncat(flagbuf, "Changed ", 47);
                if (img->flags & 8)  strncat(flagbuf, "Delete ", 47);
                if (!flagbuf[0])     strncpy(flagbuf, "-", 47);
                LabeledValue("Flags:", "0x%04X  %s", (int)img->flags, flagbuf);

                LabeledValue("DATA:", "%p", img->data_p);

                ImGui::Spacing();
                if (g_clipboard.valid) ImGui::TextDisabled("Clip:   %dx%d pixels", g_clipboard.w, g_clipboard.h);
                ImGui::TextDisabled("Undo:   %d/%d", g_undo_idx + 1, g_undo_count);
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Anim Point Editor --- */
        if (ImGui::CollapsingHeader("Anipts Tools")) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                int ax = (short)img->anix, ay = (short)img->aniy;
                int ax2 = (short)img->anix2, ay2 = (short)img->aniy2, az2 = (short)img->aniz2;
                if (AnimPointSliderInt("X1##ptx",  &ax,  -1024, 1024))
                    set_primary_anipoint_with_sequence(img, ax, (int)(short)img->aniy);
                if (AnimPointSliderInt("Y1##pty",  &ay,  -1024, 1024))
                    set_primary_anipoint_with_sequence(img, (int)(short)img->anix, ay);
                if (AnimPointSliderInt("X2##ptx2", &ax2, -1024, 1024)) {
                    int cur_y2 = secondary_anipoint_in_use(img) ? (int)(short)img->aniy2 : 0;
                    set_secondary_anipoint_with_sequence(img, ax2, cur_y2);
                }
                if (AnimPointSliderInt("Y2##pty2", &ay2, -1024, 1024)) {
                    int cur_x2 = secondary_anipoint_in_use(img) ? (int)(short)img->anix2 : 0;
                    set_secondary_anipoint_with_sequence(img, cur_x2, ay2);
                }
                if (AnimPointSliderInt("AZ2##ptz2", &az2, -1024, 1024)) {
                    if (begin_sequence_anipoint_edit()) {
                        if (az2 == -1) {
                            clear_secondary_anipoint(img);
                        } else {
                            activate_secondary_anipoint(img);
                            img->aniz2 = (unsigned short)(short)az2;
                        }
                    }
                }

                ImGui::Spacing();
                if (ImGui::Button("Default Center", ImVec2(-1, 0))) {
                    set_primary_anipoint_with_sequence(img,
                                                       (int)img->w / 2,
                                                       (int)img->h / 2);
                    if (begin_sequence_anipoint_edit())
                        clear_secondary_anipoint(img);
                    g_img_tex_idx = -2;
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Centered anim point for %s and cleared secondary.", img->n_s);
                    g_restore_msg_timer = 3.0f;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Sets X1/Y1 to the sprite center and clears X2/Y2/AZ2 to -1.");

                bool had_second_point = secondary_anipoint_in_use(img);
                if (!had_second_point) ImGui::BeginDisabled();
                if (ImGui::Button("Clear 2nd Point", ImVec2(-1, 0))) {
                    if (begin_sequence_anipoint_edit())
                        clear_secondary_anipoint(img);
                }
                if (had_second_point && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clears X2/Y2/AZ2. AZ2 becomes -1.");
                if (!had_second_point) ImGui::EndDisabled();

                ImGui::Spacing();
                if (ImGui::Button("Push to Open Tabs", ImVec2(-1, 0))) {
                    int matched = 0;
                    int docs_changed = 0;
                    std::string pattern;
                    int changed = PushAnipointsToMatchingOpenTabs(img, &matched, &docs_changed, &pattern);
                    if (changed > 0) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Pushed anim points to %d sprite%s in %d tab%s.",
                                 changed, changed == 1 ? "" : "s",
                                 docs_changed, docs_changed == 1 ? "" : "s");
                    } else if (matched > 1) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Anim points already match across %d regex hit%s.",
                                 matched, matched == 1 ? "" : "s");
                    } else {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "No other open-tab sprites match %s.",
                                 pattern.empty() ? "that name" : pattern.c_str());
                    }
                    g_restore_msg_timer = 4.0f;
                }
                if (ImGui::IsItemHovered()) {
                    std::string name = img_name_string(img);
                    std::string key = sprite_family_key(name);
                    std::string pattern = sprite_family_regex_pattern(name);
                    ImGui::SetTooltip("Copies these anim points to open-tab sprites matching %s (%s).",
                                      pattern.c_str(), key.c_str());
                }

                if (ImGui::Button("Mirror Marked to Reverse", ImVec2(-1, 0))) {
                    MirrorMarkedAnipointsToReverseWithToast();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("For every marked sprite, mirrors X1 and active X2 as width - X. Y/Z stay unchanged.");
                }
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Hitbox Editor --- */
        if (ImGui::CollapsingHeader("Hitbox")) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("X##hbx",  &g_hitbox_x, -1024, 1024)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("Y##hby",  &g_hitbox_y, -1024, 1024)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("W##hbw",  &g_hitbox_w, 1, 2048)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("H##hbh",  &g_hitbox_h, 1, 2048)) undo_push();

            ImGui::Spacing();
            if (ImGui::Button("Copy ASM to Clipboard", ImVec2(-1, 0))) {
                char buf[128];
                snprintf(buf, sizeof(buf), "\t.word   %d,%d,%d,%d\t; Hitbox X, Y, W, H\n", g_hitbox_x, g_hitbox_y, g_hitbox_w, g_hitbox_h);
                ImGui::SetClipboardText(buf);
            }
        }

        /* --- Library Info --- */
        if (ImGui::CollapsingHeader("Library")) {
            ImGui::Text("Images:   %u", g_doc->imgcnt);
            ImGui::Text("Palettes: %u", g_doc->palcnt);
            ImGui::Text("Seqs:     %u", g_doc->seqcnt);
            ImGui::Text("Scripts:  %u", g_doc->scrcnt);
            ImGui::Text("DamTbls:  %u", g_doc->damcnt);
            ImGui::Text("Version:  0x%04X", g_doc->fileversion);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    /* ===== CANVAS ===== */
    float canvas_x = TOOLBAR_W;
    float canvas_y = work_y;
    float canvas_w = sw - TOOLBAR_W - PANEL_W;
    float canvas_h = work_h - PALETTE_H - TIMELINE_H;

    ImGui::SetNextWindowPos(ImVec2(canvas_x, canvas_y));
    ImGui::SetNextWindowSize(ImVec2(canvas_w, canvas_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x06, 0x06, 0x06, 0xFF));
    ImGui::Begin("##canvas", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    {
        ImVec2 avail   = ImGui::GetContentRegionAvail();
        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_origin = img_pos;
        ImVec2 img_sz(0, 0);
        float sx = 1.0f, sy = 1.0f;
        bool timeline_composite_preview_active = false;
        bool rotate_buttons_visible = false;
        bool rotate_button_hovered = false;
        int rotate_button_hover_idx = -1;
        ImVec2 rotate_button_min[2] = {};
        ImVec2 rotate_button_max[2] = {};

        /* ---- World View mode (DOS-style anipoint alignment workspace) ----
         * Renders the sprite inside a fixed black canvas, sprite anchored at
         * (world origin - sprite.anipoint). Left-drag adjusts anix/aniy.
         * Up/Down (handled in the global shortcut block) flicks frames.
         * When this branch runs, the rest of the canvas pipeline (pixel
         * paint, marquee, anim-point handles, hitboxes, DMA overlay,
         * grid-selection) is skipped. */
        if (g_world_state.enabled) {
            bool drew_dual_marked = DrawWorldMarkedTabs(avail, img_pos, io);
            if (!drew_dual_marked && g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
                IMG *cimg = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
                /* Single-sprite World View now lives in ui_canvas.{h,cpp};
                   the larger marked-tab World View path remains above. */
                DrawWorldViewSingleSprite(avail, img_pos, io,
                                          cimg, g_img_texture,
                                          g_doc->ilselected, (int)g_doc->imgcnt,
                                          g_world_state.w, g_world_state.h,
                                          g_world_state.origin_x, g_world_state.origin_y,
                                          g_world_state.onion, g_world_marked_state.mirror_active);
            }
        }
        else if ((timeline_composite_preview_active = DrawTimelineCompositePreview(avail, img_pos))) {
            /* Composite preview is read-only: the canvas is showing two
               timeline frames in shared anipoint space, not one editable IMG. */
        }
        else if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            if (g_zoom_reset) ResetZoomToFit();

            auto apply_canvas_zoom_step = [&](int dir, ImVec2 anchor) {
                ImVec2 old_pos, old_size;
                float old_scale = 1.0f;
                ZoomImageRectForAvailable(avail, canvas_origin,
                                          &old_pos, &old_size, &old_scale);
                float new_scale = ZoomNextLevel(old_scale, dir);
                if (fabsf(new_scale - old_scale) < 0.001f) return;
                ApplyZoomScale(old_scale, new_scale, anchor, old_pos, old_size, avail);
            };

            if (g_zoom_pending_fit) {
                ResetZoomToFit();
                g_zoom_pending_fit = false;
            }

            ImVec2 canvas_center(canvas_origin.x + avail.x * 0.5f,
                                 canvas_origin.y + avail.y * 0.5f);

            /* ---- Mouse wheel: scroll normally, Ctrl+wheel zooms from center ---- */
            if (ImGui::IsWindowHovered()) {
                if (io.KeyCtrl) {
                    g_zoom_wheel_accum += io.MouseWheel;
                    while (g_zoom_wheel_accum >= 1.0f) {
                        apply_canvas_zoom_step(1, canvas_center);
                        g_zoom_wheel_accum -= 1.0f;
                    }
                    while (g_zoom_wheel_accum <= -1.0f) {
                        apply_canvas_zoom_step(-1, canvas_center);
                        g_zoom_wheel_accum += 1.0f;
                    }
                } else {
                    if (io.MouseWheel != 0.0f || io.MouseWheelH != 0.0f) {
                        const float wheel_pan_step = 80.0f;
                        float dx = io.MouseWheelH * wheel_pan_step;
                        float dy = io.MouseWheel * wheel_pan_step;
                        if (io.KeyShift && io.MouseWheel != 0.0f && io.MouseWheelH == 0.0f) {
                            dx = io.MouseWheel * wheel_pan_step;
                            dy = 0.0f;
                        }
                        ZoomPanBy(avail, dx, dy);
                    }
                    g_zoom_wheel_accum = 0.0f;
                }
            }

            while (g_zoom_pending_steps > 0) {
                apply_canvas_zoom_step(1, canvas_center);
                g_zoom_pending_steps--;
            }
            while (g_zoom_pending_steps < 0) {
                apply_canvas_zoom_step(-1, canvas_center);
                g_zoom_pending_steps++;
            }
            ZoomClampPanForAvailable(avail);

            float scale = 1.0f;
            ZoomImageRectForAvailable(avail, canvas_origin, &img_pos, &img_sz, &scale);
            ImGui::SetCursorScreenPos(img_pos);

            float tw = img_sz.x;
            float th = img_sz.y;
            img_sz  = ImVec2(tw, th);
            sx = tw / (float)g_img_tex_w;
            sy = th / (float)g_img_tex_h;
            rotate_buttons_visible = true;
            CanvasRotateButtonRects(img_pos, img_sz, canvas_origin, avail,
                                    rotate_button_min, rotate_button_max);

            ImDrawList *dl = ImGui::GetWindowDrawList();
            DrawCanvasCheckerboard(dl, img_pos, img_sz, scale);
            /* Timeline onion-skin: draw prev/next frames of the current
               timeline order behind the live sprite, anipoint-aligned and
               faint, so the user can scrub or play and see motion arcs. */
            if (g_timeline_onion && !g_timeline_frames.empty()
                && g_timeline_play_idx >= 0
                && g_timeline_play_idx < (int)g_timeline_frames.size()
                && g_doc->ilselected >= 0)
            {
                IMG *cur_img = get_img(g_doc->ilselected);
                if (cur_img) {
                    int cur_ax = (int)(short)cur_img->anix;
                    int cur_ay = (int)(short)cur_img->aniy;
                    int neighbors[2] = {
                        g_timeline_play_idx == 0
                            ? (int)g_timeline_frames.size() - 1
                            : g_timeline_play_idx - 1,
                        (g_timeline_play_idx + 1) % (int)g_timeline_frames.size()
                    };
                    ImU32 tints[2] = {
                        IM_COL32(120, 180, 255, 70), /* prev: cool */
                        IM_COL32(255, 160, 120, 70)  /* next: warm */
                    };
                    for (int side = 0; side < 2; side++) {
                        if (neighbors[side] == g_timeline_play_idx) continue;
                        int img_idx = g_timeline_frames[neighbors[side]];
                        if (img_idx == g_doc->ilselected) continue;
                        IMG *nimg = get_img(img_idx);
                        if (!nimg) continue;
                        TimelineThumb *t = EnsureThumb(img_idx);
                        if (!t || !t->tex) continue;
                        /* Place the neighbor so that its anipoint coincides
                           with the current sprite's anipoint on screen. */
                        int n_ax = (int)(short)nimg->anix;
                        int n_ay = (int)(short)nimg->aniy;
                        float scale_x = sx * ((float)nimg->w / (float)t->w);
                        float scale_y = sy * ((float)nimg->h / (float)t->h);
                        float nw_screen = t->w * scale_x;
                        float nh_screen = t->h * scale_y;
                        ImVec2 npos(img_pos.x + (cur_ax - n_ax) * sx,
                                    img_pos.y + (cur_ay - n_ay) * sy);
                        dl->AddImage((ImTextureID)(intptr_t)t->tex,
                                     npos,
                                     ImVec2(npos.x + nw_screen, npos.y + nh_screen),
                                     ImVec2(0,0), ImVec2(1,1),
                                     tints[side]);
                    }
                }
            }

            AutoChopPreview auto_chop_preview;
            bool show_auto_chop_preview =
                g_show_auto_chop && SelectedImageWillAutoChop();
            if (show_auto_chop_preview) {
                IMG *chop_img = get_img(g_doc->ilselected);
                if (g_chop_mode == AutoChopMode_BestHorizontal ||
                    g_chop_mode == AutoChopMode_BestVertical) {
                    bool vertical = (g_chop_mode == AutoChopMode_BestVertical);
                    show_auto_chop_preview =
                        BuildBestAutoSplitPreviewForImage(chop_img, vertical,
                                                          &auto_chop_preview) &&
                        !auto_chop_preview.pieces.empty();
                } else {
                    show_auto_chop_preview =
                        BuildAutoChopPreviewForImage(chop_img, &auto_chop_preview) &&
                        !auto_chop_preview.pieces.empty();
                }
                if (show_auto_chop_preview) {
                    DrawAutoChopPreviewRects(dl, auto_chop_preview,
                                             img_pos, sx, sy, false);
                }
            }

            ImGui::Image((ImTextureID)(intptr_t)g_img_texture, img_sz);

            /* Color isolation: dim everything that isn't in the "kept" set.
               The set is either (a) the single Alt-clicked isolate index, or
               (b) the multi-selected swatch set (yellow rings in the palette
               grid). Alt-isolate wins when both are active.
               Scanline-coalesced so a 256x256 sprite emits at most ~256 rects
               per row of contiguous non-target pixels, not 65k per-pixel. */
            bool kept[256];
            bool any_kept = false;
            if (g_isolate_color >= 0) {
                memset(kept, 0, sizeof(kept));
                kept[g_isolate_color] = true;
                any_kept = true;
            } else {
                for (int ki = 0; ki < 256; ki++) {
                    kept[ki] = g_palette_selection[ki];
                    if (kept[ki]) any_kept = true;
                }
            }
            if (any_kept && g_doc->ilselected >= 0) {
                IMG *iimg = get_img(g_doc->ilselected);
                DrawCanvasColorIsolationOverlay(dl, iimg, kept, img_pos, sx, sy);
            }

            if (g_show_dma_comp) {
                IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
                DrawCanvasDmaCompressionOverlay(dl, img, img_pos, sx, sy);
            }

            DrawCanvasPixelGrid(dl, img_pos, img_sz, g_img_tex_w, g_img_tex_h, scale);

            if (show_auto_chop_preview) {
                DrawAutoChopPreviewRects(dl, auto_chop_preview,
                                         img_pos, sx, sy, true);
            }

            DrawCanvasZoomIndicator(g_zoom_fit, g_zoom);
        } else {
            g_zoom_pending_steps = 0;
            g_zoom_pending_fit = false;
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.45f);
            float tw = ImGui::CalcTextSize("No image selected").x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - tw) * 0.5f);
            ImGui::TextDisabled("No image selected");
        }

        ImVec2 mouse = io.MousePos;
        bool   mbdn  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

        /* When a modal or popup window is on top, ImGui sets WantCaptureMouse —
         * suppress all canvas interaction (paint, eyedropper, highlight, drag,
         * marquee, anim-point handles, etc.) so clicks meant for the modal
         * don't bleed through to the sprite underneath. */
        bool canvas_input_blocked = io.WantCaptureMouse && !ImGui::IsWindowHovered();
        if (canvas_input_blocked) mbdn = false;

        /* Set when an overlay widget (anim point, hitbox corner) eats this frame's
           click, so the grid-selection block below doesn't also start a selection. */
        bool widget_consumed_click = false;
        bool blank_marquee_click = false;

        if (rotate_buttons_visible && !canvas_input_blocked && !timeline_composite_preview_active) {
            for (int i = 0; i < 1; i++) {
                if (CanvasPointInRect(mouse, rotate_button_min[i], rotate_button_max[i])) {
                    rotate_button_hovered = true;
                    rotate_button_hover_idx = i;
                    ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
                    ImGui::SetTooltip("Rotate 90 Clockwise (preserve anim points)");
                    widget_consumed_click = true;
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        TransformSelectedSprite(SpriteTransformOp::Rotate90CW);
                    }
                    break;
                }
            }
        }

        /* Pixel highlight at high zoom */
        if (!canvas_input_blocked && !timeline_composite_preview_active) {
            DrawCanvasPixelHoverHighlight(ImGui::GetWindowDrawList(),
                                          mouse, img_pos, img_sz, sx, sy,
                                          rotate_button_hovered);
        }

        /* ---- Pencil + eyedropper + fill + pan tools ---- */
        if (!canvas_input_blocked && !timeline_composite_preview_active) {
            IMG *cimg = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            bool over = mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                        mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y &&
                        !rotate_button_hovered;

            /* Pan: middle-mouse drag or spacebar+drag or right-drag at zoom */
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
                ZoomPanBy(avail, d.x, d.y);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                widget_consumed_click = true;
            }
            if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                ZoomPanBy(avail, d.x, d.y);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                widget_consumed_click = true;
            }
            if (g_active_tool == ActiveTool::None && g_zoom > 1.0f &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) && over) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
                ZoomPanBy(avail, d.x, d.y);
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
                widget_consumed_click = true;
            }

            if (cimg && cimg->data_p && cimg->w > 0 && cimg->h > 0 && over) {
                int px = (int)((mouse.x - img_pos.x) / sx);
                int py = (int)((mouse.y - img_pos.y) / sy);
                if (px >= 0 && px < (int)cimg->w && py >= 0 && py < (int)cimg->h) {
                    unsigned short stride = (cimg->w + 3) & ~3;
                    unsigned char *pix = (unsigned char *)cimg->data_p + py * stride + px;

                    /* Right-click: eyedropper (works in any tool mode) */
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        g_sel_color = *pix;
                        widget_consumed_click = true;
                    }
                    /* Eyedropper tool active: left-click also picks color.
                       Consumes the click so the pencil branch below is skipped. */
                    if (g_active_tool == ActiveTool::Eyedropper &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_sel_color = *pix;
                        widget_consumed_click = true;
                    }
                    /* Left-click: pencil, paint bucket, background eraser, clone stamp, or smart remap.
                       Suppress when the cursor is over (or dragging) an anipoint or hitbox
                       handle. The anipoint render block runs *after* this branch, so the
                       in-progress drag flag isn't enough on the first click frame — we
                       need to also detect "about to start dragging" via a fresh hover test. */
                    bool over_anipoint = false;
                    if (g_show_points && cimg) {
                        ImVec2 a1(img_pos.x + (short)cimg->anix * sx, img_pos.y + (short)cimg->aniy * sy);
                        ImVec2 da1 = mouse - a1;
                        if (da1.x*da1.x + da1.y*da1.y < 10*10) over_anipoint = true;
                        if (!over_anipoint && secondary_anipoint_in_use(cimg)) {
                            ImVec2 a2(img_pos.x + (short)cimg->anix2 * sx, img_pos.y + (short)cimg->aniy2 * sy);
                            ImVec2 da2 = mouse - a2;
                            if (da2.x*da2.x + da2.y*da2.y < 10*10) over_anipoint = true;
                        }
                    }
                    if (!g_pasted.active && !over_anipoint && !g_anipoint_drag1 && !g_anipoint_drag2 &&
                        g_hitbox_drag_corner < 0 && g_active_tool == ActiveTool::None &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left) && *pix == 0 &&
                        !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
                        blank_marquee_click = true;
                        g_active_tool = ActiveTool::Marquee;
                    }
                    if (!blank_marquee_click && !g_pasted.active && !over_anipoint && !g_anipoint_drag1 && !g_anipoint_drag2 && g_hitbox_drag_corner < 0
                        && (g_active_tool == ActiveTool::None || g_active_tool == ActiveTool::Pencil || g_active_tool == ActiveTool::PaintBucket || g_active_tool == ActiveTool::VariantPaint || g_active_tool == ActiveTool::BackgroundEraser || g_active_tool == ActiveTool::CloneStamp || g_active_tool == ActiveTool::SmartRemap)) {
                        /* Stroke begin: capture a pre-stroke snapshot of the
                           image's pixel buffer on the first frame of left-mouse
                           down for any paint tool. Skipped for Clone Stamp's
                           Alt-click "set source" which doesn't modify pixels. */
                        bool stroke_begin = ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                            && !(g_active_tool == ActiveTool::CloneStamp && io.KeyAlt);
                        if (stroke_begin) {
                            if (g_active_tool == ActiveTool::VariantPaint && g_sel_color > 0)
                                doc_undo_push();
                            else if (g_active_tool != ActiveTool::VariantPaint)
                                pixel_hist_push_stroke();
                        }
                        if (g_active_tool == ActiveTool::CloneStamp) {
                            if (io.KeyAlt && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                g_clone_src_x = px;
                                g_clone_src_y = py;
                                g_clone_source_set = true;
                                g_clone_offset_set = false;
                                widget_consumed_click = true;
                            } else if (!io.KeyAlt && ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_clone_source_set) {
                                if (!g_clone_offset_set) {
                                    g_clone_dx = g_clone_src_x - px;
                                    g_clone_dy = g_clone_src_y - py;
                                    g_clone_offset_set = true;
                                }
                                if (g_pixel_undo_img != g_doc->ilselected) {
                                    free(g_pixel_undo); g_pixel_undo = NULL;
                                    unsigned short s = (cimg->w + 3) & ~3;
                                    unsigned int sz = (unsigned int)s * cimg->h;
                                    g_pixel_undo = (unsigned char *)malloc(sz);
                                    if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                                    g_pixel_undo_img = g_doc->ilselected;
                                }
                                /* Round-brush stamp. g_clone_brush is the radius;
                                   1 = single pixel (kept for sharp work), >1 = soft disc. */
                                int r = g_clone_brush > 0 ? g_clone_brush : 1;
                                int r2 = (r - 1) * (r - 1);
                                unsigned short stride = (cimg->w + 3) & ~3;
                                unsigned char *cdata = (unsigned char *)cimg->data_p;
                                for (int by = -(r - 1); by <= (r - 1); by++) {
                                    for (int bx = -(r - 1); bx <= (r - 1); bx++) {
                                        if (r > 1 && bx * bx + by * by > r2) continue;
                                        int dx_px = px + bx;
                                        int dy_px = py + by;
                                        if (dx_px < 0 || dy_px < 0 ||
                                            dx_px >= (int)cimg->w || dy_px >= (int)cimg->h) continue;
                                        int src_px = dx_px + g_clone_dx;
                                        int src_py = dy_px + g_clone_dy;
                                        if (src_px < 0 || src_py < 0 ||
                                            src_px >= (int)cimg->w || src_py >= (int)cimg->h) continue;
                                        unsigned char src_col = cdata[src_py * stride + src_px];
                                        cdata[dy_px * stride + dx_px] = src_col;
                                    }
                                }
                                mark_dirty();
                                g_img_tex_idx = -2;
                                widget_consumed_click = true;
                            }
                        } else if (g_active_tool == ActiveTool::PaintBucket) {
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                int changed = PaintBucketFill(cimg, px, py,
                                                              (unsigned char)g_sel_color,
                                                              g_bucket_tolerance,
                                                              g_bucket_contiguous);
                                if (changed > 0) {
                                    mark_dirty();
                                    g_img_tex_idx = -2;
                                }
                                if (changed > 0) {
                                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                                             "Paint bucket filled %d pixel%s.",
                                             changed, changed == 1 ? "" : "s");
                                } else {
                                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                                             "Paint bucket: no pixels changed.");
                                }
                                g_restore_msg_timer = 3.0f;
                                widget_consumed_click = true;
                            }
                        } else if (g_active_tool == ActiveTool::VariantPaint) {
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                                VariantPaintResult vr = ApplyVariantBrush(cimg, px, py, g_variant_brush);
                                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                    if (g_sel_color == 0) {
                                        snprintf(g_restore_msg, sizeof(g_restore_msg), "Variant paint needs an opaque target color.");
                                        g_restore_msg_timer = 4.0f;
                                    } else if (vr.skipped_no_slot > 0 && vr.pixels == 0) {
                                        snprintf(g_restore_msg, sizeof(g_restore_msg), "No free palette index for variant shadow.");
                                        g_restore_msg_timer = 4.0f;
                                    }
                                }
                                widget_consumed_click = true;
                            }
                        } else if (g_active_tool == ActiveTool::SmartRemap) {
                            /* On the first click of a stroke, capture the target
                               color. Hold to paint replacement over every pixel
                               within tolerance of that target. The target sticks
                               until mouse-up so a single drag has consistent
                               behavior even as the brush crosses varied pixels. */
                            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                                g_remap_target_color = *pix;
                            }
                            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) && g_remap_target_color != -1) {
                                int diff = (int)*pix - g_remap_target_color;
                                if (diff < 0) diff = -diff;
                                if (diff <= g_remap_tolerance) {
                                    if (g_pixel_undo_img != g_doc->ilselected) {
                                        free(g_pixel_undo); g_pixel_undo = NULL;
                                        unsigned short s = (cimg->w + 3) & ~3;
                                        unsigned int sz = (unsigned int)s * cimg->h;
                                        g_pixel_undo = (unsigned char *)malloc(sz);
                                        if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                                        g_pixel_undo_img = g_doc->ilselected;
                                    }
                                    *pix = (unsigned char)g_sel_color;
                                    mark_dirty();
                                    g_img_tex_idx = -2;
                                }
                                widget_consumed_click = true;
                            }
                            /* Release: forget the target so the next click can pick
                               a different reference color. */
                            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                                g_remap_target_color = -1;
                            }
                        } else if (g_active_tool == ActiveTool::BackgroundEraser
                                       ? ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                                       : ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                            if (g_pixel_undo_img != g_doc->ilselected) {
                                free(g_pixel_undo); g_pixel_undo = NULL;
                                unsigned short s = (cimg->w + 3) & ~3;
                                unsigned int sz = (unsigned int)s * cimg->h;
                                g_pixel_undo = (unsigned char *)malloc(sz);
                                if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                                g_pixel_undo_img = g_doc->ilselected;
                            }
                            if (g_active_tool == ActiveTool::BackgroundEraser) {
                                mark_dirty();
                                SmartErase(cimg, px, py,
                                           g_eraser_tolerance,
                                           g_eraser_contiguous,
                                           g_eraser_defringe);
                            } else if (io.KeyShift) {
                                mark_dirty();
                                FloodFill(cimg, px, py, (unsigned char)g_sel_color);
                            } else {
                                mark_dirty();
                                /* Pencil with radius >1 stamps a disc. r=1 keeps
                                   the single-pixel behavior the underlying paint
                                   path has always had. */
                                int r = (g_active_tool == ActiveTool::Pencil && g_pencil_brush > 1)
                                            ? g_pencil_brush : 1;
                                if (r == 1) {
                                    *pix = (unsigned char)g_sel_color;
                                } else {
                                    int r2 = (r - 1) * (r - 1);
                                    unsigned short stride = (cimg->w + 3) & ~3;
                                    unsigned char *cdata = (unsigned char *)cimg->data_p;
                                    for (int by = -(r - 1); by <= (r - 1); by++)
                                    for (int bx = -(r - 1); bx <= (r - 1); bx++) {
                                        if (bx * bx + by * by > r2) continue;
                                        int dx_px = px + bx, dy_px = py + by;
                                        if (dx_px < 0 || dy_px < 0
                                            || dx_px >= (int)cimg->w
                                            || dy_px >= (int)cimg->h) continue;
                                        cdata[dy_px * stride + dx_px] = (unsigned char)g_sel_color;
                                    }
                                }
                            }
                            g_img_tex_idx = -2;
                            widget_consumed_click = true;
                        }
                    }
                }
            }
        }

        /* --- Anim point overlay + dragging --- */
        /* Anipoints + IMG hitbox don't render in World View. The World
           View canvas anchors the sprite at world-origin-minus-anipoint
           so the on-sprite anipoint marker would land outside or at the
           wrong spot, and the IMG hitbox box would visually float
           detached from the playfield rectangle. Both stay reachable
           via their normal modes when World View is off. */
        if (g_show_points && !canvas_input_blocked && !g_world_state.enabled && !timeline_composite_preview_active) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img && img->w > 0) {
                ImDrawList *dl = ImGui::GetWindowDrawList();

                /* Previous-frame anipoint ghost — gray crosshair when onion-skin
                   is on, mirroring the DOS tool's registration reference. */
                if (g_timeline_onion && g_doc->ilselected > 0) {
                    int prev_idx = g_doc->ilselected - 1;
                    IMG *prev = get_img(prev_idx);
                    if (prev) {
                        ImVec2 sp(img_pos.x + (short)prev->anix * sx, img_pos.y + (short)prev->aniy * sy);
                        DrawCanvasAnipointCrosshair(dl, sp, IM_COL32(160, 160, 160, 180), 12.f, 1.f);
                        if (secondary_anipoint_in_use(prev)) {
                            ImVec2 sp2(img_pos.x + (short)prev->anix2 * sx, img_pos.y + (short)prev->aniy2 * sy);
                            DrawCanvasAnipointCrosshair(dl, sp2, IM_COL32(160, 160, 160, 140), 9.f, 1.f);
                        }
                    }
                }

                ImVec2 s1(img_pos.x + (short)img->anix * sx, img_pos.y + (short)img->aniy * sy);
                ImVec2 d1 = mouse - s1;
                bool h1 = (d1.x*d1.x + d1.y*d1.y) < 10*10;
                /* Primary anipoint: white crosshair, brightens on hover. */
                ImU32 col1 = h1 ? IM_COL32(255, 220, 60, 255) : IM_COL32(255, 255, 255, 255);
                DrawCanvasAnipointCrosshair(dl, s1, col1, 14.f, h1 ? 2.f : 1.5f);

                if (h1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { g_anipoint_drag1 = true; widget_consumed_click = true; }
                if (g_anipoint_drag1 && mbdn) {
                    int nx = (int)((mouse.x - img_pos.x) / sx);
                    int ny = (int)((mouse.y - img_pos.y) / sy);
                    set_primary_anipoint_with_sequence(img, nx, ny);
                    widget_consumed_click = true;
                } else if (!mbdn && g_anipoint_drag1) { g_anipoint_drag1 = false; }

                /* Secondary anipoint sentinel is signed -1; cast first so
                   (-1, -1) doesn't read as 0xFFFF and emit a phantom line. */
                if (secondary_anipoint_in_use(img)) {
                    ImVec2 s2(img_pos.x + (short)img->anix2 * sx, img_pos.y + (short)img->aniy2 * sy);
                    ImVec2 d2 = mouse - s2;
                    bool h2 = (d2.x*d2.x + d2.y*d2.y) < 10*10;
                    /* Secondary anipoint: cyan crosshair (distinct from primary). */
                    ImU32 col2 = h2 ? IM_COL32(120, 255, 255, 255) : IM_COL32(60, 200, 220, 255);
                    DrawCanvasAnipointCrosshair(dl, s2, col2, 10.f, h2 ? 2.f : 1.5f);
                    /* Thin connector line between the two anipoints. */
                    dl->AddLine(s1, s2, IM_COL32(255, 255, 0, 140), 1.f);

                    if (h2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { g_anipoint_drag2 = true; widget_consumed_click = true; }
                    if (g_anipoint_drag2 && mbdn) {
                        int nx = (int)((mouse.x - img_pos.x) / sx);
                        int ny = (int)((mouse.y - img_pos.y) / sy);
                        set_secondary_anipoint_with_sequence(img, nx, ny);
                        widget_consumed_click = true;
                    } else if (!mbdn && g_anipoint_drag2) { g_anipoint_drag2 = false; }
                }
            }
        }

        /* --- Hitbox overlay + corner dragging ---
           Suppressed when the MK2 strike-table overlay is showing a move,
           so the two hitbox systems don't pile on top of each other. */
        bool mk2_overlay_active = g_show_mk2 && Mk2CurrentRecord() >= 0;
        if (g_show_hitbox && !canvas_input_blocked && !mk2_overlay_active && !g_world_state.enabled && !timeline_composite_preview_active) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            bool hovering[4] = {false, false, false, false};
            DrawCanvasHitboxOverlay(dl, img_pos, sx, sy,
                                    g_hitbox_x, g_hitbox_y,
                                    g_hitbox_w, g_hitbox_h,
                                    mouse, hovering);
            for (int c = 0; c < 4; c++) {
                if (hovering[c] && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_hitbox_drag_corner = c;
                    undo_push();
                    widget_consumed_click = true;
                }
            }
            if (g_hitbox_drag_corner >= 0 && mbdn) {
                int mx = (int)((mouse.x - img_pos.x) / sx);
                int my = (int)((mouse.y - img_pos.y) / sy);
                int c = g_hitbox_drag_corner;
                if (c == 0) { g_hitbox_w += g_hitbox_x - mx; g_hitbox_h += g_hitbox_y - my; g_hitbox_x = mx; g_hitbox_y = my; }
                if (c == 1) { g_hitbox_w = mx - g_hitbox_x; g_hitbox_h += g_hitbox_y - my; g_hitbox_y = my; }
                if (c == 2) { g_hitbox_w = mx - g_hitbox_x; g_hitbox_h = my - g_hitbox_y; }
                if (c == 3) { g_hitbox_w += g_hitbox_x - mx; g_hitbox_x = mx; g_hitbox_h = my - g_hitbox_y; }
                if (g_hitbox_w < 1) g_hitbox_w = 1;
                if (g_hitbox_h < 1) g_hitbox_h = 1;
            } else if (!mbdn && g_hitbox_drag_corner >= 0) {
                undo_push();
                g_hitbox_drag_corner = -1;
            }
        }

        /* --- MK2 strike-table overlay (separate from IMG hitbox) ---
           Draws the currently-selected MKSTK.ASM move's collision box on
           the sprite, with corner handles for drag-to-resize. Magenta to
           distinguish from the cyan IMG-hitbox overlay.
           Drawing always runs whenever a move is selected — the editor
           panel can hold focus (which sets canvas_input_blocked) without
           hiding the box. Only the corner-drag interaction is gated. */
        int mk2_rec = (g_show_mk2 && !g_world_state.enabled && !timeline_composite_preview_active) ? Mk2CurrentRecord() : -1;
        if (mk2_rec >= 0) {
            const mk2::StrikeRecord &rec = g_mk2_doc.records[mk2_rec];
            int hx = rec.fields[mk2::F_X_OFFSET].has_value ? (int)rec.fields[mk2::F_X_OFFSET].value : 0;
            int hy = rec.fields[mk2::F_Y_OFFSET].has_value ? (int)rec.fields[mk2::F_Y_OFFSET].value : 0;
            int hw = rec.fields[mk2::F_X_SIZE  ].has_value ? (int)rec.fields[mk2::F_X_SIZE  ].value : 0;
            int hh = rec.fields[mk2::F_Y_SIZE  ].has_value ? (int)rec.fields[mk2::F_Y_SIZE  ].value : 0;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            char tag[80];
            snprintf(tag, sizeof(tag), "%s  (%d,%d %dx%d)", rec.label.c_str(), hx, hy, hw, hh);
            bool  hovering[4] = { false, false, false, false };
            DrawCanvasStrikeBoxOverlay(dl, img_pos, sx, sy,
                                       hx, hy, hw, hh,
                                       tag, mouse, !canvas_input_blocked,
                                       hovering);
            if (!canvas_input_blocked) {
                for (int c = 0; c < 4; c++) {
                    if (hovering[c] && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        /* Snapshot the pre-drag state once. Subsequent
                           per-pixel updates during the drag coalesce. */
                        mk2::undo_push(&g_mk2_doc, mk2_rec, false);
                        g_mk2_drag_corner = c;
                        widget_consumed_click = true;
                    }
                }
            }
            if (g_mk2_drag_corner >= 0 && mbdn) {
                int mx = (int)((mouse.x - img_pos.x) / sx);
                int my = (int)((mouse.y - img_pos.y) / sy);
                int c = g_mk2_drag_corner;
                int nx = hx, ny = hy, nw = hw, nh = hh;
                if (c == 0) { nw += hx - mx; nh += hy - my; nx = mx; ny = my; }
                if (c == 1) { nw  = mx - hx; nh += hy - my;            ny = my; }
                if (c == 2) { nw  = mx - hx; nh  = my - hy; }
                if (c == 3) { nw += hx - mx; nx = mx;       nh  = my - hy; }
                if (nw < 1) nw = 1;
                if (nh < 1) nh = 1;
                /* Push values through the document so the .ASM line buffer
                   stays in sync and Save picks them up. */
                if (nx != hx) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_X_OFFSET, nx);
                if (ny != hy) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_Y_OFFSET, ny);
                if (nw != hw) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_X_SIZE,   nw);
                if (nh != hh) mk2::set_value(&g_mk2_doc, mk2_rec, mk2::F_Y_SIZE,   nh);
                widget_consumed_click = true;
            } else if (!mbdn && g_mk2_drag_corner >= 0) {
                g_mk2_drag_corner = -1;
            }
        }

        /* --- Grid selection tool (for copy/paste) --- */
        if (!timeline_composite_preview_active && g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            ImDrawList *dl = ImGui::GetWindowDrawList();

            /* Mouse-over-sprite test — clicks outside this rect must NOT start a selection. */
            bool mouse_over_sprite =
                mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y &&
                !rotate_button_hovered;

            /* Pencil cursor indicator — color tracks the currently-selected
               palette entry so the user previews what they're about to paint.
               Index 0 (transparent) falls back to white. Two render modes:
                 brush > 1: ring around the round-disc stamp footprint.
                 brush = 1: small offset crosshair so the single target pixel
                            stays visible underneath. The crosshair lives
                            outside the pixel rect itself so it never hides
                            the pixel it points at. */
            if (g_active_tool == ActiveTool::Pencil && mouse_over_sprite) {
                int mx = (int)((mouse.x - img_pos.x) / sx);
                int my = (int)((mouse.y - img_pos.y) / sy);
                ImVec2 cc(img_pos.x + (mx + 0.5f) * sx,
                          img_pos.y + (my + 0.5f) * sy);
                ImU32 col;
                if (g_sel_color > 0) {
                    SDL_Color &c = g_palette[g_sel_color];
                    col = IM_COL32(c.r, c.g, c.b, 230);
                } else {
                    col = IM_COL32(255, 255, 255, 200);
                }
                if (g_pencil_brush > 1) {
                    float rr = (sx + sy) * 0.5f * (g_pencil_brush - 1);
                    /* Black halo so the ring stays visible against same-
                       colored pixels. */
                    dl->AddCircle(cc, rr, IM_COL32(0, 0, 0, 200), 0, 3.0f);
                    dl->AddCircle(cc, rr, col, 0, 1.5f);
                } else {
                    /* Half-pixel inset puts the gap right at the target
                       pixel's edge so the arms hug it, not float away.
                       The gap is capped: at high zoom the pixel is huge,
                       but a 30-px gap would visually disconnect the arms
                       from the pixel they point at. Cap at 4px. The arm
                       length stays modest (8px) so the marker doesn't
                       grow into the rest of the sprite. */
                    float pix = (sx + sy) * 0.5f;
                    float gap = pix * 0.5f;
                    if (gap > 4.0f) gap = 4.0f;
                    if (gap < 1.0f) gap = 1.0f;
                    float len = 8.0f;
                    /* 1-px darker halo behind each arm so the cursor stays
                       legible when its color matches the underlying pixel. */
                    ImU32 halo = IM_COL32(0, 0, 0, 200);
                    auto arm = [&](ImVec2 a, ImVec2 b) {
                        dl->AddLine(a, b, halo, 3.0f);
                        dl->AddLine(a, b, col,  1.5f);
                    };
                    arm(ImVec2(cc.x - gap - len, cc.y), ImVec2(cc.x - gap, cc.y));
                    arm(ImVec2(cc.x + gap,        cc.y), ImVec2(cc.x + gap + len, cc.y));
                    arm(ImVec2(cc.x, cc.y - gap - len), ImVec2(cc.x, cc.y - gap));
                    arm(ImVec2(cc.x, cc.y + gap),        ImVec2(cc.x, cc.y + gap + len));
                }
            }

            /* Clone Stamp visual aids: source crosshair and destination brush ring. */
            if (g_active_tool == ActiveTool::CloneStamp && g_clone_source_set) {
                ImVec2 sc(img_pos.x + (g_clone_src_x + 0.5f) * sx,
                          img_pos.y + (g_clone_src_y + 0.5f) * sy);
                ImU32 src_col = IM_COL32(0, 255, 255, 230);
                dl->AddLine(ImVec2(sc.x - 8, sc.y), ImVec2(sc.x + 8, sc.y), src_col, 1.5f);
                dl->AddLine(ImVec2(sc.x, sc.y - 8), ImVec2(sc.x, sc.y + 8), src_col, 1.5f);
                dl->AddCircle(sc, 4.0f, src_col, 0, 1.0f);
                if (mouse_over_sprite && g_clone_brush > 1) {
                    int r = g_clone_brush - 1;
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    ImVec2 cc(img_pos.x + (mx + 0.5f) * sx,
                              img_pos.y + (my + 0.5f) * sy);
                    float rr = (sx + sy) * 0.5f * r;
                    dl->AddCircle(cc, rr, IM_COL32(255, 255, 255, 200), 0, 1.0f);
                }
            }

            /* Start a new selection only on a fresh click that lands on the sprite
               and isn't already being consumed by an anim-point or hitbox-corner drag.
               Once a drag is in progress we keep updating x2/y2 wherever the mouse
               goes (clamped) until the button is released. */
            /* Block selection when:
               - the mouse is over a hovered ImGui widget (menu item, button)
                 OR an active item is being interacted with;
               - any popup/menu is open (its dropdown can overlap the canvas
                 and clicking through it must not start a marquee).
               Geometric mouse_over_sprite still has to be true. */
            bool any_popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
            bool ui_blocking = ImGui::IsAnyItemHovered() || ImGui::IsAnyItemActive() || any_popup;
            if (!g_pasted.active && (g_active_tool == ActiveTool::Marquee || g_active_tool == ActiveTool::MagicWand || g_active_tool == ActiveTool::Lasso)) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                        && mouse_over_sprite
                        && !widget_consumed_click
                        && !ui_blocking) {
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    if (mx < 0) mx = 0; if (mx >= (int)g_img_tex_w) mx = g_img_tex_w - 1;
                    if (my < 0) my = 0; if (my >= (int)g_img_tex_h) my = g_img_tex_h - 1;
                    
                    if (g_active_tool == ActiveTool::MagicWand && g_doc->ilselected >= 0) {
                        IMG* simg = get_img(g_doc->ilselected);
                        if (simg && simg->data_p) {
                            int sw = simg->w;
                            int sh = simg->h;
                            int stride = (sw + 3) & ~3;
                            unsigned char* pdata = (unsigned char*)simg->data_p;
                            int target_color = pdata[my * stride + mx];
                            int tol = g_wand_tolerance;
                            selection_begin_add_drag(sw, sh, ImGui::GetIO().KeyCtrl);

                            g_grid_sel.active = true;
                            g_grid_sel.is_mask = true;
                            g_grid_sel.mask_w = sw;
                            g_grid_sel.mask_h = sh;
                            g_grid_sel.pixel_mask.assign((size_t)sw * sh, false);

                            int min_x = mx, max_x = mx;
                            int min_y = my, max_y = my;

                            if (g_wand_contiguous) {
                                std::vector<std::pair<int, int>> stack;
                                stack.push_back({mx, my});
                                g_grid_sel.pixel_mask[my * sw + mx] = true;

                                while(!stack.empty()) {
                                    std::pair<int, int> pt = stack.back();
                                    stack.pop_back();
                                    int cx = pt.first;
                                    int cy = pt.second;

                                    if (cx < min_x) min_x = cx;
                                    if (cx > max_x) max_x = cx;
                                    if (cy < min_y) min_y = cy;
                                    if (cy > max_y) max_y = cy;

                                    const int dx[] = {0, 1, 0, -1};
                                    const int dy[] = {-1, 0, 1, 0};
                                    for(int i = 0; i < 4; i++) {
                                        int nx = cx + dx[i];
                                        int ny = cy + dy[i];
                                        if (nx >= 0 && nx < sw && ny >= 0 && ny < sh) {
                                            int diff = (int)pdata[ny * stride + nx] - target_color;
                                            if (diff < 0) diff = -diff;
                                            if (!g_grid_sel.pixel_mask[ny * sw + nx] && diff <= tol) {
                                                g_grid_sel.pixel_mask[ny * sw + nx] = true;
                                                stack.push_back({nx, ny});
                                            }
                                        }
                                    }
                                }
                            } else {
                                /* Global non-contiguous: every pixel in image within tolerance */
                                bool first = true;
                                for (int y = 0; y < sh; y++) {
                                    for (int x = 0; x < sw; x++) {
                                        int diff = (int)pdata[y * stride + x] - target_color;
                                        if (diff < 0) diff = -diff;
                                        if (diff <= tol) {
                                            g_grid_sel.pixel_mask[y * sw + x] = true;
                                            if (first) {
                                                min_x = max_x = x; min_y = max_y = y;
                                                first = false;
                                            } else {
                                                if (x < min_x) min_x = x;
                                                if (x > max_x) max_x = x;
                                                if (y < min_y) min_y = y;
                                                if (y > max_y) max_y = y;
                                            }
                                        }
                                    }
                                }
                            }
                            g_grid_sel.x1 = min_x;
                            g_grid_sel.y1 = min_y;
                            g_grid_sel.x2 = max_x;
                            g_grid_sel.y2 = max_y;
                            g_grid_sel.dragging = false;
                            selection_finish_add_drag(sw, sh);
                        }
                    } else if (g_active_tool == ActiveTool::Lasso) {
                        selection_begin_add_drag(g_img_tex_w, g_img_tex_h, ImGui::GetIO().KeyCtrl);
                        g_lasso_points.clear();
                        g_lasso_points.push_back({mx, my});
                        g_grid_sel.active = true;
                        g_grid_sel.dragging = true;
                        g_grid_sel.is_mask = false; /* becomes a mask on release */
                        g_grid_sel.x1 = g_grid_sel.x2 = mx;
                        g_grid_sel.y1 = g_grid_sel.y2 = my;
                    } else {
                        selection_begin_add_drag(g_img_tex_w, g_img_tex_h, ImGui::GetIO().KeyCtrl);
                        g_grid_sel.active = true;
                        g_grid_sel.dragging = true;
                        g_grid_sel.is_mask = false;
                        g_grid_sel.x1 = g_grid_sel.x2 = mx;
                        g_grid_sel.y1 = g_grid_sel.y2 = my;
                    }
                } else if (g_grid_sel.dragging && mbdn) {
                    /* Only extend the rect while we're in the user-initiated
                       drag — not on every frame the button happens to be
                       down (e.g. a click on a menu would otherwise reposition
                       the marquee to wherever the menu click landed). */
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    if (mx < 0) mx = 0; if (mx >= (int)g_img_tex_w) mx = g_img_tex_w - 1;
                    if (my < 0) my = 0; if (my >= (int)g_img_tex_h) my = g_img_tex_h - 1;
                    if (g_active_tool == ActiveTool::Lasso) {
                        /* Append point if it moved at least 1 pixel from the last vertex */
                        if (g_lasso_points.empty() ||
                            g_lasso_points.back().first != mx ||
                            g_lasso_points.back().second != my) {
                            g_lasso_points.push_back({mx, my});
                        }
                    } else {
                        g_grid_sel.x2 = mx;
                        g_grid_sel.y2 = my;
                    }
                } else if (g_grid_sel.dragging && !mbdn) {
                    g_grid_sel.dragging = false;
                    if (g_active_tool == ActiveTool::Lasso && g_doc->ilselected >= 0 && g_lasso_points.size() >= 3) {
                        /* Rasterize the polygon into a pixel mask using a scanline
                           even-odd test. */
                        IMG *simg = get_img(g_doc->ilselected);
                        if (simg && simg->data_p) {
                            int sw = simg->w, sh = simg->h;
                            int min_x = sw, max_x = -1, min_y = sh, max_y = -1;
                            for (auto &p : g_lasso_points) {
                                if (p.first  < min_x) min_x = p.first;
                                if (p.first  > max_x) max_x = p.first;
                                if (p.second < min_y) min_y = p.second;
                                if (p.second > max_y) max_y = p.second;
                            }
                            if (min_x < 0) min_x = 0;
                            if (min_y < 0) min_y = 0;
                            if (max_x >= sw) max_x = sw - 1;
                            if (max_y >= sh) max_y = sh - 1;

                            g_grid_sel.is_mask = true;
                            g_grid_sel.mask_w = sw;
                            g_grid_sel.mask_h = sh;
                            g_grid_sel.pixel_mask.assign((size_t)sw * sh, false);

                            int n = (int)g_lasso_points.size();
                            for (int y = min_y; y <= max_y; y++) {
                                /* Crossings at half-pixel y */
                                float yf = y + 0.5f;
                                std::vector<float> xs;
                                for (int i = 0; i < n; i++) {
                                    float ax = (float)g_lasso_points[i].first;
                                    float ay = (float)g_lasso_points[i].second;
                                    float bx = (float)g_lasso_points[(i + 1) % n].first;
                                    float by = (float)g_lasso_points[(i + 1) % n].second;
                                    if ((ay <= yf) != (by <= yf)) {
                                        float t = (yf - ay) / (by - ay);
                                        xs.push_back(ax + t * (bx - ax));
                                    }
                                }
                                std::sort(xs.begin(), xs.end());
                                for (size_t i = 0; i + 1 < xs.size(); i += 2) {
                                    int x0 = (int)ceilf(xs[i]);
                                    int x1 = (int)floorf(xs[i + 1]);
                                    if (x0 < min_x) x0 = min_x;
                                    if (x1 > max_x) x1 = max_x;
                                    for (int x = x0; x <= x1; x++) {
                                        g_grid_sel.pixel_mask[y * sw + x] = true;
                                    }
                                }
                            }
                            g_grid_sel.x1 = min_x;
                            g_grid_sel.y1 = min_y;
                            g_grid_sel.x2 = max_x < min_x ? min_x : max_x;
                            g_grid_sel.y2 = max_y < min_y ? min_y : max_y;
                        }
                        g_lasso_points.clear();
                    } else if (g_active_tool == ActiveTool::Lasso) {
                        /* Aborted / too few points */
                        g_lasso_points.clear();
                        g_grid_sel.active = false;
                    }
                    if (ImGui::GetIO().KeyShift && !g_grid_sel.is_mask && g_doc->ilselected >= 0) {
                        IMG *simg = get_img(g_doc->ilselected);
                        if (simg && simg->data_p) {
                            int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
                            int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
                            if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
                            if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
                            int min_x = x2, max_x = x1, min_y = y2, max_y = y1;
                            bool found = false;
                            unsigned short stride = (simg->w + 3) & ~3;
                            unsigned char *pdata = (unsigned char *)simg->data_p;
                            for (int y = y1; y <= y2; y++) {
                                for (int x = x1; x <= x2; x++) {
                                    if (pdata[y * stride + x] != 0) {
                                        if (x < min_x) min_x = x;
                                        if (x > max_x) max_x = x;
                                        if (y < min_y) min_y = y;
                                        if (y > max_y) max_y = y;
                                        found = true;
                                    }
                                }
                            }
                            if (found) {
                                g_grid_sel.x1 = min_x;
                                g_grid_sel.y1 = min_y;
                                g_grid_sel.x2 = max_x;
                                g_grid_sel.y2 = max_y;
                            }
                        }
                    }
                    if (g_selection_add_drag && g_doc->ilselected >= 0) {
                        IMG *simg = get_img(g_doc->ilselected);
                        if (simg) selection_finish_add_drag(simg->w, simg->h);
                    }
                }
            }

            /* Draw selection rectangle only when the marquee tool is on. Toggling
               the tool off via the toolbar/R also clears g_grid_sel, but this
               extra gate makes sure no stray green box renders if some other
               code path leaves g_grid_sel.active=true with the tool off. */
            /* Live lasso path while drawing */
            if (g_active_tool == ActiveTool::Lasso && g_grid_sel.dragging && g_lasso_points.size() >= 2) {
                std::vector<ImVec2> screen_pts;
                screen_pts.reserve(g_lasso_points.size() + 1);
                for (auto &p : g_lasso_points) {
                    screen_pts.push_back(ImVec2(img_pos.x + (p.first + 0.5f) * sx,
                                                img_pos.y + (p.second + 0.5f) * sy));
                }
                /* Show closing edge as a dashed-ish thin line */
                dl->AddPolyline(screen_pts.data(), (int)screen_pts.size(),
                                IM_COL32(255, 0, 255, 220), 0, 1.5f);
                if (screen_pts.size() >= 2) {
                    dl->AddLine(screen_pts.back(), screen_pts.front(),
                                IM_COL32(255, 0, 255, 110), 1.0f);
                }
            }

            if (g_grid_sel.active && (g_active_tool == ActiveTool::Marquee || g_active_tool == ActiveTool::MagicWand || g_active_tool == ActiveTool::Lasso)) {
                int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
                int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
                if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
                if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
                
                if (g_grid_sel.is_mask) {
                    for (int y = y1; y <= y2; y++) {
                        for (int x = x1; x <= x2; x++) {
                            if (g_grid_sel.pixel_mask[y * g_grid_sel.mask_w + x]) {
                                ImVec2 r1(img_pos.x + x * sx, img_pos.y + y * sy);
                                ImVec2 r2(img_pos.x + (x + 1) * sx, img_pos.y + (y + 1) * sy);
                                dl->AddRectFilled(r1, r2, IM_COL32(255, 0, 255, 80), 0.0f);
                            }
                        }
                    }
                    ImVec2 br1(img_pos.x + x1 * sx, img_pos.y + y1 * sy);
                    ImVec2 br2(img_pos.x + (x2 + 1) * sx, img_pos.y + (y2 + 1) * sy);
                    dl->AddRect(br1, br2, IM_COL32(255, 0, 255, 255), 0.0f, 0, 1.0f);
                } else {
                    ImVec2 r1(img_pos.x + x1 * sx, img_pos.y + y1 * sy);
                    ImVec2 r2(img_pos.x + (x2 + 1) * sx, img_pos.y + (y2 + 1) * sy);
                    dl->AddRect(r1, r2, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
                    dl->AddRectFilled(r1, r2, IM_COL32(0, 255, 0, 30), 0.0f);
                }
            }

            /* Defensive: transform mode can't exist without a floating paste.
               Several state-clearing paths (ClearAll, file-open, etc.) drop
               g_pasted.active without knowing about the transform, so latch
               g_xform off here rather than scatter g_xform.active = false
               across every site. */
            if (g_xform.active && !g_pasted.active) {
                g_xform.active = false;
                g_xform.handle = TransformHandle::None;
            }

            /* Paste overlay with pixel preview */
            if (g_pasted.active && g_clipboard.valid && g_clipboard.w > 0 && g_clipboard.h > 0) {
                /* In transform mode the visible rect is the live transform
                   rect, not the static clipboard size. The pixel preview is
                   skipped during transform (interpolated preview would
                   disagree with the post-commit nearest-neighbor result and
                   confuse the user). */
                int px, py, pw, ph;
                if (g_xform.active) {
                    px = g_xform.rx; py = g_xform.ry;
                    pw = g_xform.rw; ph = g_xform.rh;
                } else {
                    px = g_pasted.paste_x; py = g_pasted.paste_y;
                    pw = g_clipboard.w;    ph = g_clipboard.h;
                }
                unsigned short cs = g_clipboard.stride;

                ImVec2 p1(img_pos.x + px * sx, img_pos.y + py * sy);
                ImVec2 p2(img_pos.x + (px + pw) * sx, img_pos.y + (py + ph) * sy);
                float angle_deg = g_xform.active ? g_xform.angle_deg : 0.0f;
                const float PI_F = 3.14159265358979323846f;
                float angle_rad = angle_deg * PI_F / 180.0f;
                float ca = cosf(angle_rad);
                float sa = sinf(angle_rad);
                float cx_img = (float)px + (float)pw * 0.5f;
                float cy_img = (float)py + (float)ph * 0.5f;
                auto xform_point_screen = [&](float ix, float iy) -> ImVec2 {
                    float dxp = ix - cx_img;
                    float dyp = iy - cy_img;
                    float rxp = cx_img + dxp * ca - dyp * sa;
                    float ryp = cy_img + dxp * sa + dyp * ca;
                    return ImVec2(img_pos.x + rxp * sx, img_pos.y + ryp * sy);
                };
                auto xform_point_image = [&](float ix, float iy) -> ImVec2 {
                    float dxp = ix - cx_img;
                    float dyp = iy - cy_img;
                    return ImVec2(cx_img + dxp * ca - dyp * sa,
                                  cy_img + dxp * sa + dyp * ca);
                };
                ImVec2 paste_controls_min(canvas_origin.x + 10.0f, canvas_origin.y + 10.0f);
                ImVec2 paste_controls_max(paste_controls_min.x + 276.0f,
                                           paste_controls_min.y + 62.0f);
                bool paste_controls_block =
                    mouse.x >= paste_controls_min.x && mouse.x < paste_controls_max.x &&
                    mouse.y >= paste_controls_min.y && mouse.y < paste_controls_max.y;
                ImVec2 rc[4] = {
                    xform_point_screen((float)px,      (float)py),
                    xform_point_screen((float)px + pw, (float)py),
                    xform_point_screen((float)px + pw, (float)py + ph),
                    xform_point_screen((float)px,      (float)py + ph),
                };
                ImVec2 rb_min = rc[0], rb_max = rc[0];
                for (int i = 1; i < 4; i++) {
                    if (rc[i].x < rb_min.x) rb_min.x = rc[i].x;
                    if (rc[i].y < rb_min.y) rb_min.y = rc[i].y;
                    if (rc[i].x > rb_max.x) rb_max.x = rc[i].x;
                    if (rc[i].y > rb_max.y) rb_max.y = rc[i].y;
                }
                bool hovering = mouse.x >= rb_min.x && mouse.x < rb_max.x && mouse.y >= rb_min.y && mouse.y < rb_max.y;
                bool over_sprite = mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                                   mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y;

                /* Render clipboard pixel preview, including live scale/rotation
                   while Free Transform is active. At Normal/100 this is fully
                   opaque so the pasted sprite is visible; opacity/blend choices
                   preview the same RGB composite used by final commit. */
                IMG *simg = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
                PAL *spal = simg ? get_pal(simg->palnum) : NULL;
                const unsigned char *dst_pixels = simg ? (const unsigned char *)simg->data_p : NULL;
                int dst_stride = simg ? ((int)simg->w + 3) & ~3 : 0;
                const unsigned char *src = (const unsigned char *)g_clipboard.data_p;
                unsigned char paste_pal_map[256];
                bool paste_remap = BuildClipboardPaletteMap(spal, paste_pal_map);
                int cw = g_clipboard.w;
                int ch = g_clipboard.h;
                for (int y = 0; y < ch; y++) {
                    for (int x = 0; x < cw; x++) {
                        unsigned char ci = src[y * cs + x];
                        if (ci == 0) continue;
                        float ix0 = (float)px + ((float)x * (float)pw / (float)cw);
                        float iy0 = (float)py + ((float)y * (float)ph / (float)ch);
                        float ix1 = (float)px + ((float)(x + 1) * (float)pw / (float)cw);
                        float iy1 = (float)py + ((float)(y + 1) * (float)ph / (float)ch);
                        ImVec2 mid = xform_point_image((ix0 + ix1) * 0.5f,
                                                       (iy0 + iy1) * 0.5f);
                        int dxp = (int)floorf(mid.x);
                        int dyp = (int)floorf(mid.y);
                        unsigned char dst_ci = 0;
                        if (dst_pixels && simg && dxp >= 0 && dyp >= 0 &&
                            dxp < (int)simg->w && dyp < (int)simg->h)
                            dst_ci = dst_pixels[dyp * dst_stride + dxp];
                        int rr = 255, gg = 255, bb = 255, aa = 255;
                        if (!paste_preview_rgba(ci, dst_ci, spal, paste_pal_map,
                                                paste_remap, dxp, dyp,
                                                &rr, &gg, &bb, &aa))
                            continue;
                        ImU32 col = IM_COL32((unsigned char)rr,
                                             (unsigned char)gg,
                                             (unsigned char)bb,
                                             (unsigned char)aa);
                        ImVec2 q0 = xform_point_screen(ix0, iy0);
                        ImVec2 q1 = xform_point_screen(ix1, iy0);
                        ImVec2 q2 = xform_point_screen(ix1, iy1);
                        ImVec2 q3 = xform_point_screen(ix0, iy1);
                        dl->AddQuadFilled(q0, q1, q2, q3, col);
                    }
                }

                /* Border — cyan when transforming, gold when hovering, yellow otherwise */
                ImU32 border_col = g_xform.active ? IM_COL32(0, 220, 255, 255)
                                                  : (hovering ? IM_COL32(255, 200, 0, 255)
                                                              : IM_COL32(255, 255, 0, 255));
                dl->AddPolyline(rc, 4, border_col, ImDrawFlags_Closed, 2.0f);

                /* Snap guides — drawn while a snap is active this frame so
                   the user sees exactly which edge their paste locked onto. */
                if (g_pasted.dragging && g_snap_hit_x) {
                    float gx = img_pos.x + g_snap_guide_x * sx;
                    dl->AddLine(ImVec2(gx, img_pos.y),
                                ImVec2(gx, img_pos.y + g_img_tex_h * sy),
                                IM_COL32(255, 0, 255, 220), 1.5f);
                }
                if (g_pasted.dragging && g_snap_hit_y) {
                    float gy = img_pos.y + g_snap_guide_y * sy;
                    dl->AddLine(ImVec2(img_pos.x, gy),
                                ImVec2(img_pos.x + g_img_tex_w * sx, gy),
                                IM_COL32(255, 0, 255, 220), 1.5f);
                }

                /* Instruction text */
                if (g_xform.active) {
                    const char *hint = (g_xform.handle != TransformHandle::None)
                        ? (g_xform.handle == TransformHandle::Rotate ? "Rotating..."
                           : (g_xform.handle == TransformHandle::Move ? "Moving..." : "Scaling..."))
                        : "Drag inside to move | handles scale | top dot rotates | Enter commits";
                    dl->AddText(ImVec2(img_pos.x + 6, img_pos.y + 6), IM_COL32(0, 220, 255, 255), hint);
                } else if (g_pasted.dragging)
                    dl->AddText(ImVec2(img_pos.x + 6, img_pos.y + 6), IM_COL32(255, 200, 0, 255), "Moving...");
                else
                    dl->AddText(ImVec2(img_pos.x + 6, img_pos.y + 6), IM_COL32(255, 255, 0, 255), "Drag to move | H/V flip | L to layer | Ctrl+T transform | Click outside to place | Esc cancel");

                dl->AddRectFilled(paste_controls_min, paste_controls_max,
                                  IM_COL32(18, 20, 24, 230), 4.0f);
                dl->AddRect(paste_controls_min, paste_controls_max,
                            IM_COL32(90, 130, 180, 210), 4.0f, 0, 1.0f);
                ImGui::PushID("paste_controls");
                ImGui::SetCursorScreenPos(ImVec2(paste_controls_min.x + 8.0f,
                                                  paste_controls_min.y + 7.0f));
                ImGui::TextUnformatted("Blend");
                ImGui::SetCursorScreenPos(ImVec2(paste_controls_min.x + 76.0f,
                                                  paste_controls_min.y + 5.0f));
                ImGui::SetNextItemWidth(paste_controls_max.x - paste_controls_min.x - 86.0f);
                if (ImGui::BeginCombo("##blend", PasteBlendModeName(g_paste_blend_mode))) {
                    for (PasteBlendMode mode : k_paste_blend_modes) {
                        bool selected = (g_paste_blend_mode == mode);
                        if (ImGui::Selectable(PasteBlendModeName(mode), selected))
                            g_paste_blend_mode = mode;
                        if (selected) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::SetCursorScreenPos(ImVec2(paste_controls_min.x + 8.0f,
                                                  paste_controls_min.y + 34.0f));
                ImGui::TextUnformatted("Opacity");
                ImGui::SetCursorScreenPos(ImVec2(paste_controls_min.x + 76.0f,
                                                  paste_controls_min.y + 32.0f));
                ImGui::SetNextItemWidth(paste_controls_max.x - paste_controls_min.x - 86.0f);
                ImGui::SliderInt("##opacity", &g_paste_opacity, 0, 100, "%d%%");
                ImGui::PopID();

                /* ----- Free Transform handles + interaction ----- */
                if (g_xform.active && !canvas_input_blocked) {
                    /* 8 handles positioned at the corners and edge midpoints
                       of the live rect. Each is a 10x10 screen-pixel square
                       drawn outside the rect so the rect stroke stays clean. */
                    const float HSZ = 5.0f; /* half-size in screen pixels */
                    struct HandleSpec { TransformHandle h; float cx, cy; };
                    HandleSpec specs[8] = {
                        { TransformHandle::TL, rc[0].x, rc[0].y },
                        { TransformHandle::T,  (rc[0].x + rc[1].x) * 0.5f, (rc[0].y + rc[1].y) * 0.5f },
                        { TransformHandle::TR, rc[1].x, rc[1].y },
                        { TransformHandle::L,  (rc[0].x + rc[3].x) * 0.5f, (rc[0].y + rc[3].y) * 0.5f },
                        { TransformHandle::R,  (rc[1].x + rc[2].x) * 0.5f, (rc[1].y + rc[2].y) * 0.5f },
                        { TransformHandle::BL, rc[3].x, rc[3].y },
                        { TransformHandle::B,  (rc[3].x + rc[2].x) * 0.5f, (rc[3].y + rc[2].y) * 0.5f },
                        { TransformHandle::BR, rc[2].x, rc[2].y },
                    };

                    TransformHandle hover_h = TransformHandle::None;
                    for (int i = 0; i < 8; i++) {
                        const HandleSpec &s = specs[i];
                        bool hov = mouse.x >= s.cx - HSZ && mouse.x <= s.cx + HSZ &&
                                   mouse.y >= s.cy - HSZ && mouse.y <= s.cy + HSZ;
                        if (hov && g_xform.handle == TransformHandle::None) hover_h = s.h;
                        ImU32 fill = (hov || g_xform.handle == s.h) ? IM_COL32(255, 255, 255, 255)
                                                                    : IM_COL32(0, 220, 255, 255);
                        dl->AddRectFilled(ImVec2(s.cx - HSZ, s.cy - HSZ),
                                          ImVec2(s.cx + HSZ, s.cy + HSZ),
                                          fill);
                        dl->AddRect(ImVec2(s.cx - HSZ, s.cy - HSZ),
                                    ImVec2(s.cx + HSZ, s.cy + HSZ),
                                    IM_COL32(0, 0, 0, 255), 0.0f, 0, 1.0f);
                    }

                    float center_sx = (rc[0].x + rc[2].x) * 0.5f;
                    float center_sy = (rc[0].y + rc[2].y) * 0.5f;
                    float top_mid_x = (rc[0].x + rc[1].x) * 0.5f;
                    float top_mid_y = (rc[0].y + rc[1].y) * 0.5f;
                    float vx = top_mid_x - center_sx;
                    float vy = top_mid_y - center_sy;
                    float vlen = sqrtf(vx * vx + vy * vy);
                    if (vlen < 0.001f) { vx = 0.0f; vy = -1.0f; vlen = 1.0f; }
                    vx /= vlen; vy /= vlen;
                    ImVec2 rot_handle(top_mid_x + vx * 26.0f, top_mid_y + vy * 26.0f);
                    dl->AddLine(ImVec2(top_mid_x, top_mid_y), rot_handle, IM_COL32(0, 220, 255, 190), 1.5f);
                    float rdist = (mouse.x - rot_handle.x) * (mouse.x - rot_handle.x) +
                                  (mouse.y - rot_handle.y) * (mouse.y - rot_handle.y);
                    bool rot_hov = rdist <= 9.0f * 9.0f;
                    if (rot_hov && g_xform.handle == TransformHandle::None) hover_h = TransformHandle::Rotate;
                    dl->AddCircleFilled(rot_handle, 7.0f,
                                        (rot_hov || g_xform.handle == TransformHandle::Rotate)
                                            ? IM_COL32(255, 255, 255, 255)
                                            : IM_COL32(0, 220, 255, 255));
                    dl->AddCircle(rot_handle, 7.0f, IM_COL32(0, 0, 0, 255), 0, 1.0f);
                    if (rot_hov)
                        ImGui::SetTooltip("Rotate paste");

                    /* Chain icon at the top-right corner of the rect, offset
                       upward so it doesn't collide with the TR handle. Click
                       toggles g_xform.aspect_locked (persistent). Drawn as
                       two interlocked squares — minimal but recognizable. */
                    float chain_cx = rc[1].x + 14;
                    float chain_cy = rc[1].y - 14;
                    float chain_hs = 8;
                    ImVec2 ch1(chain_cx - chain_hs, chain_cy - chain_hs);
                    ImVec2 ch2(chain_cx + chain_hs, chain_cy + chain_hs);
                    bool chain_hov = mouse.x >= ch1.x && mouse.x <= ch2.x &&
                                     mouse.y >= ch1.y && mouse.y <= ch2.y;
                    ImU32 chain_bg = chain_hov ? IM_COL32(255, 255, 255, 200)
                                               : IM_COL32(40, 40, 40, 200);
                    ImU32 chain_fg = g_xform.aspect_locked ? IM_COL32(0, 220, 255, 255)
                                                           : IM_COL32(180, 180, 180, 255);
                    dl->AddRectFilled(ch1, ch2, chain_bg, 2.0f);
                    dl->AddRect(ch1, ch2, IM_COL32(0, 0, 0, 255), 2.0f, 0, 1.0f);
                    /* Glyph: two linked rings when locked, two broken arcs when not.
                       Drawn with primitives — no font dependency. */
                    if (g_xform.aspect_locked) {
                        dl->AddCircle(ImVec2(chain_cx - 3, chain_cy), 3.5f, chain_fg, 0, 1.5f);
                        dl->AddCircle(ImVec2(chain_cx + 3, chain_cy), 3.5f, chain_fg, 0, 1.5f);
                    } else {
                        dl->AddCircle(ImVec2(chain_cx - 4, chain_cy - 2), 3.0f, chain_fg, 0, 1.5f);
                        dl->AddCircle(ImVec2(chain_cx + 4, chain_cy + 2), 3.0f, chain_fg, 0, 1.5f);
                    }
                    if (chain_hov) {
                        ImGui::SetTooltip(g_xform.aspect_locked
                            ? "Aspect ratio locked. Click to unlock (free scale)."
                            : "Aspect ratio free. Click to lock (proportional scale).");
                        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                            g_xform.aspect_locked = !g_xform.aspect_locked;
                        }
                    }

                    /* Handle drag: pick on click, scale on drag, release commits. */
                    if (!paste_controls_block &&
                        g_xform.handle == TransformHandle::None && hover_h != TransformHandle::None &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_xform.handle  = hover_h;
                        g_xform.drag_mx = mouse.x;
                        g_xform.drag_my = mouse.y;
                        g_xform.drag_rx = g_xform.rx;
                        g_xform.drag_ry = g_xform.ry;
                        g_xform.drag_rw = g_xform.rw;
                        g_xform.drag_rh = g_xform.rh;
                        g_xform.drag_angle_deg = g_xform.angle_deg;
                        g_xform.ref_aspect = (g_xform.rh > 0)
                            ? (float)g_xform.rw / (float)g_xform.rh
                            : 1.0f;
                    }
                    if (!paste_controls_block &&
                        g_xform.handle == TransformHandle::None && hover_h == TransformHandle::None &&
                        hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_xform.handle  = TransformHandle::Move;
                        g_xform.drag_mx = mouse.x;
                        g_xform.drag_my = mouse.y;
                        g_xform.drag_rx = g_xform.rx;
                        g_xform.drag_ry = g_xform.ry;
                        g_xform.drag_rw = g_xform.rw;
                        g_xform.drag_rh = g_xform.rh;
                        g_xform.drag_angle_deg = g_xform.angle_deg;
                    }
                    if (g_xform.handle != TransformHandle::None && mbdn) {
                        if (g_xform.handle == TransformHandle::Move) {
                            int dx = (int)((mouse.x - g_xform.drag_mx) / sx);
                            int dy = (int)((mouse.y - g_xform.drag_my) / sy);
                            g_xform.rx = g_xform.drag_rx + dx;
                            g_xform.ry = g_xform.drag_ry + dy;
                        } else if (g_xform.handle == TransformHandle::Rotate) {
                            float a0 = atan2f(g_xform.drag_my - center_sy,
                                              g_xform.drag_mx - center_sx);
                            float a1 = atan2f(mouse.y - center_sy,
                                              mouse.x - center_sx);
                            float new_angle = g_xform.drag_angle_deg +
                                (a1 - a0) * 180.0f / PI_F;
                            while (new_angle <= -180.0f) new_angle += 360.0f;
                            while (new_angle >   180.0f) new_angle -= 360.0f;
                            if (ImGui::GetIO().KeyShift) {
                                new_angle = roundf(new_angle / 15.0f) * 15.0f;
                            }
                            g_xform.angle_deg = new_angle;
                        } else {
                        /* Convert mouse delta from screen pixels back into
                           image pixels via the sx/sy zoom factors. */
                        int dx = (int)((mouse.x - g_xform.drag_mx) / sx);
                        int dy = (int)((mouse.y - g_xform.drag_my) / sy);
                        int rx = g_xform.drag_rx, ry = g_xform.drag_ry;
                        int rw = g_xform.drag_rw, rh = g_xform.drag_rh;

                        /* Apply the delta to the right edge(s) for the chosen
                           handle. East/south edges move with positive delta,
                           west/north edges move and shrink the rect. */
                        TransformHandle h = g_xform.handle;
                        bool affects_left   = (h == TransformHandle::TL || h == TransformHandle::L || h == TransformHandle::BL);
                        bool affects_right  = (h == TransformHandle::TR || h == TransformHandle::R || h == TransformHandle::BR);
                        bool affects_top    = (h == TransformHandle::TL || h == TransformHandle::T || h == TransformHandle::TR);
                        bool affects_bottom = (h == TransformHandle::BL || h == TransformHandle::B || h == TransformHandle::BR);

                        if (affects_left)   { rx += dx; rw -= dx; }
                        if (affects_right)  {           rw += dx; }
                        if (affects_top)    { ry += dy; rh -= dy; }
                        if (affects_bottom) {           rh += dy; }

                        /* Aspect handling. Adobe convention:
                            - Corner handles: respect lock (Shift inverts).
                            - Edge handles: ALWAYS free in Photoshop's classic
                              behavior, but with the chain locked the user
                              expects edges to also scale proportionally —
                              honor the lock there too. Shift still inverts. */
                        bool is_corner = (h == TransformHandle::TL || h == TransformHandle::TR ||
                                          h == TransformHandle::BL || h == TransformHandle::BR);
                        bool shift_inverts = ImGui::GetIO().KeyShift;
                        bool lock_now = g_xform.aspect_locked ^ shift_inverts;

                        if (lock_now && g_xform.ref_aspect > 0.0f) {
                            if (is_corner) {
                                /* Use the dominant axis to drive the other. */
                                float scale_w = (float)rw / (float)g_xform.drag_rw;
                                float scale_h = (float)rh / (float)g_xform.drag_rh;
                                float scale   = (fabsf(scale_w - 1.0f) > fabsf(scale_h - 1.0f)) ? scale_w : scale_h;
                                int new_w = (int)(g_xform.drag_rw * scale + 0.5f);
                                int new_h = (int)(new_w / g_xform.ref_aspect + 0.5f);
                                if (new_w < 1) new_w = 1;
                                if (new_h < 1) new_h = 1;
                                if (affects_left)  rx = (g_xform.drag_rx + g_xform.drag_rw) - new_w;
                                if (affects_top)   ry = (g_xform.drag_ry + g_xform.drag_rh) - new_h;
                                rw = new_w; rh = new_h;
                            } else {
                                /* Edge handle with lock: drive the OTHER axis
                                   from this one, anchored at the center of the
                                   non-moving axis. */
                                if (h == TransformHandle::T || h == TransformHandle::B) {
                                    int new_w = (int)(rh * g_xform.ref_aspect + 0.5f);
                                    if (new_w < 1) new_w = 1;
                                    int cx_old = g_xform.drag_rx + g_xform.drag_rw / 2;
                                    rx = cx_old - new_w / 2;
                                    rw = new_w;
                                } else {
                                    int new_h = (int)(rw / g_xform.ref_aspect + 0.5f);
                                    if (new_h < 1) new_h = 1;
                                    int cy_old = g_xform.drag_ry + g_xform.drag_rh / 2;
                                    ry = cy_old - new_h / 2;
                                    rh = new_h;
                                }
                            }
                        }

                        if (rw < 1) rw = 1;
                        if (rh < 1) rh = 1;
                        g_xform.rx = rx; g_xform.ry = ry;
                        g_xform.rw = rw; g_xform.rh = rh;
                        }
                    }
                    if (g_xform.handle != TransformHandle::None && !mbdn) {
                        g_xform.handle = TransformHandle::None;
                    }

                    /* Click outside the transform rect (but on the sprite,
                       not on a handle and not on the chain icon) commits the
                       transform AND applies the paste — matches Photoshop's
                       "click anywhere outside the bbox to commit" behavior.
                       Without this, every paste would require an extra
                       Enter / Ctrl+T keystroke before the user could click
                       to drop it, because paste auto-enters transform now. */
                    if (!paste_controls_block &&
                        g_xform.handle == TransformHandle::None && !hovering &&
                        over_sprite && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        /* Skip if the click landed on the chain icon — that
                           click is consumed by the chain toggle above. */
                        bool on_chain = mouse.x >= ch1.x && mouse.x <= ch2.x &&
                                        mouse.y >= ch1.y && mouse.y <= ch2.y;
                        if (!on_chain) {
                            xform_commit();
                            apply_pasted_region();
                            g_pasted.active = false;
                            g_pasted.dragging = false;
                        }
                    }
                }

                if (!canvas_input_blocked && !g_xform.active) {
                    /* Start drag: click inside paste rect */
                    if (!paste_controls_block && hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        g_pasted.dragging = true;
                        g_pasted.drag_start_mx = mouse.x;
                        g_pasted.drag_start_my = mouse.y;
                        g_pasted.drag_start_px = g_pasted.paste_x;
                        g_pasted.drag_start_py = g_pasted.paste_y;
                    }

                    /* Drag to move */
                    if (g_pasted.dragging && mbdn) {
                        int dx = (int)((mouse.x - g_pasted.drag_start_mx) / sx);
                        int dy = (int)((mouse.y - g_pasted.drag_start_my) / sy);
                        int nx = g_pasted.drag_start_px + dx;
                        int ny = g_pasted.drag_start_py + dy;

                        g_snap_hit_x = g_snap_hit_y = false;
                        if (ImGui::GetIO().KeyShift && g_doc->ilselected >= 0) {
                            /* Cache the content bbox of the underlying sprite
                               on the first frame Shift is held during this
                               drag; recompute only on image change. */
                            if (!g_snap_bbox.valid || g_snap_bbox.img_idx != g_doc->ilselected) {
                                IMG *cimg = get_img(g_doc->ilselected);
                                if (cimg && cimg->data_p) {
                                    int min_x = cimg->w, min_y = cimg->h, max_x = 0, max_y = 0;
                                    unsigned short cw = (cimg->w + 3) & ~3;
                                    bool found = false;
                                    unsigned char *dp = (unsigned char *)cimg->data_p;
                                    for (int y = 0; y < cimg->h; y++) {
                                        for (int x = 0; x < cimg->w; x++) {
                                            if (dp[y * cw + x] != 0) {
                                                if (x < min_x) min_x = x;
                                                if (x > max_x) max_x = x;
                                                if (y < min_y) min_y = y;
                                                if (y > max_y) max_y = y;
                                                found = true;
                                            }
                                        }
                                    }
                                    if (found) {
                                        g_snap_bbox = {true, min_x, min_y, max_x, max_y, g_doc->ilselected};
                                    }
                                }
                            }
                            if (g_snap_bbox.valid) {
                                /* Threshold is screen-relative (~6 screen px)
                                   then converted into image pixels. */
                                int tx = (int)(6.0f / sx); if (tx < 1) tx = 1;
                                int ty = (int)(6.0f / sy); if (ty < 1) ty = 1;
                                int sx_min = g_snap_bbox.min_x;
                                int sy_min = g_snap_bbox.min_y;
                                int sx_max = g_snap_bbox.max_x + 1;
                                int sy_max = g_snap_bbox.max_y + 1;

                                if (abs(nx - sx_min) < tx)              { nx = sx_min;            g_snap_hit_x = true; g_snap_guide_x = sx_min; }
                                else if (abs((nx + pw) - sx_max) < tx)  { nx = sx_max - pw;       g_snap_hit_x = true; g_snap_guide_x = sx_max; }
                                else if (abs(nx - sx_max) < tx)         { nx = sx_max;            g_snap_hit_x = true; g_snap_guide_x = sx_max; }
                                else if (abs((nx + pw) - sx_min) < tx)  { nx = sx_min - pw;       g_snap_hit_x = true; g_snap_guide_x = sx_min; }

                                if (abs(ny - sy_min) < ty)              { ny = sy_min;            g_snap_hit_y = true; g_snap_guide_y = sy_min; }
                                else if (abs((ny + ph) - sy_max) < ty)  { ny = sy_max - ph;       g_snap_hit_y = true; g_snap_guide_y = sy_max; }
                                else if (abs(ny - sy_max) < ty)         { ny = sy_max;            g_snap_hit_y = true; g_snap_guide_y = sy_max; }
                                else if (abs((ny + ph) - sy_min) < ty)  { ny = sy_min - ph;       g_snap_hit_y = true; g_snap_guide_y = sy_min; }

                                /* Center snap: align the paste rect's center
                                   with the sprite's center. Adobe-style. Only
                                   engages if no edge snap fired this frame so
                                   edge alignment takes priority. */
                                IMG *cimg2 = get_img(g_doc->ilselected);
                                if (cimg2 && !g_snap_hit_x) {
                                    int sprite_cx = cimg2->w / 2;
                                    int paste_cx  = nx + pw / 2;
                                    if (abs(paste_cx - sprite_cx) < tx) {
                                        nx = sprite_cx - pw / 2;
                                        g_snap_hit_x = true;
                                        g_snap_guide_x = sprite_cx;
                                    }
                                }
                                if (cimg2 && !g_snap_hit_y) {
                                    int sprite_cy = cimg2->h / 2;
                                    int paste_cy  = ny + ph / 2;
                                    if (abs(paste_cy - sprite_cy) < ty) {
                                        ny = sprite_cy - ph / 2;
                                        g_snap_hit_y = true;
                                        g_snap_guide_y = sprite_cy;
                                    }
                                }
                            }
                        } else {
                            g_snap_bbox.valid = false;
                        }

                        /* Passive centering guide: even without Shift, show a
                           magenta center line when the paste rect's center
                           lands exactly on the sprite's center axis. Lets the
                           user see "I'm centered" without engaging snap. */
                        {
                            IMG *cimg3 = get_img(g_doc->ilselected);
                            if (cimg3 && !g_snap_hit_x) {
                                int sprite_cx = cimg3->w / 2;
                                if (nx + pw / 2 == sprite_cx) {
                                    g_snap_hit_x   = true;
                                    g_snap_guide_x = sprite_cx;
                                }
                            }
                            if (cimg3 && !g_snap_hit_y) {
                                int sprite_cy = cimg3->h / 2;
                                if (ny + ph / 2 == sprite_cy) {
                                    g_snap_hit_y   = true;
                                    g_snap_guide_y = sprite_cy;
                                }
                            }
                        }

                        if (nx < 0) nx = 0;
                        if (ny < 0) ny = 0;
                        if (nx + pw > (int)g_img_tex_w) nx = g_img_tex_w - pw;
                        if (ny + ph > (int)g_img_tex_h) ny = g_img_tex_h - ph;
                        g_pasted.paste_x = nx;
                        g_pasted.paste_y = ny;
                    }

                    /* Stop drag on release — keep floating */
                    if (g_pasted.dragging && !mbdn)
                        g_pasted.dragging = false;

                    /* Click outside paste rect (but on sprite) to confirm */
                    if (!paste_controls_block && !hovering && over_sprite && !g_pasted.dragging &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        apply_pasted_region();
                        g_pasted.active = false;
                    }
                }
            }
            if (rotate_buttons_visible)
                DrawCanvasRotateButtons(dl, rotate_button_min, rotate_button_max, rotate_button_hover_idx);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    /* ===== BOTTOM TIMELINE BAR =====
       State (g_is_playing/g_play_speed/g_play_timer/g_timeline_frames/
       g_timeline_play_idx/g_timeline_built_for_imgcnt/g_timeline_onion) is
       at file scope so keyboard shortcuts and other overlays can address it. */

    /* Drop stale frame indices if the underlying image set shrank or was reloaded */
    if (g_doc->imgcnt != g_timeline_built_for_imgcnt) {
        EnsureTimelineHolds();
        std::vector<int> valid_frames;
        std::vector<int> valid_holds;
        valid_frames.reserve(g_timeline_frames.size());
        valid_holds.reserve(g_timeline_holds.size());
        for (size_t i = 0; i < g_timeline_frames.size(); i++) {
            int idx = g_timeline_frames[i];
            if (idx < 0 || (unsigned int)idx >= g_doc->imgcnt) continue;
            valid_frames.push_back(idx);
            valid_holds.push_back(g_timeline_holds[i]);
        }
        g_timeline_frames.swap(valid_frames);
        g_timeline_holds.swap(valid_holds);
        /* Free thumbnail textures past the new end. */
        for (size_t i = g_doc->imgcnt; i < g_thumb_cache.size(); i++) {
            if (g_thumb_cache[i].tex) SDL_DestroyTexture(g_thumb_cache[i].tex);
        }
        if (g_thumb_cache.size() > g_doc->imgcnt) g_thumb_cache.resize(g_doc->imgcnt);
        g_timeline_built_for_imgcnt = g_doc->imgcnt;
        PruneTimelineCompositeSelection();
    }

    /* Build default timeline if empty (fresh file, or after Reset Sequence) */
    if (g_timeline_frames.empty() && g_doc->imgcnt > 0) {
        for (unsigned int i = 0; i < g_doc->imgcnt; i++) {
            IMG *p = get_img(i);
            if (p && (p->flags & 1)) TimelinePushFrame((int)i);
        }
        if (g_timeline_frames.empty()) {
            for (unsigned int i = 0; i < g_doc->imgcnt; i++) TimelinePushFrame((int)i);
        }
    }

    if (g_timeline_play_idx >= (int)g_timeline_frames.size())
        g_timeline_play_idx = 0;
    
    /* Playback logic */
    if (g_is_playing && !g_timeline_frames.empty()) {
        EnsureTimelineHolds();
        g_play_timer += ImGui::GetIO().DeltaTime;
        float frame_seconds = (float)TimelineHoldAt(g_timeline_play_idx) / g_play_speed;
        if (frame_seconds < 0.001f) frame_seconds = 0.001f;
        if (g_play_timer >= frame_seconds) {
            g_play_timer = 0.0f;
            int n = (int)g_timeline_frames.size();
            int step_delta = 1;
            if (g_timeline_pingpong && n > 1) {
                /* Ping-pong: walk in g_timeline_play_dir and bounce at the
                   endpoints, landing on them once per cycle. e.g. for 7
                   frames the sequence is 0,1,2,3,4,5,6,5,4,3,2,1,0,1,...
                   The bounce happens on the frame we'd otherwise overshoot:
                   when the next step would leave the [0, n-1] range, flip
                   direction and step inward by 2 instead of out by 1. */
                int next = g_timeline_play_idx + g_timeline_play_dir;
                if (next >= n || next < 0) {
                    g_timeline_play_dir = -g_timeline_play_dir;
                    next = g_timeline_play_idx + g_timeline_play_dir;
                    if (next < 0) next = 0;
                    if (next >= n) next = n - 1;
                }
                step_delta = next - g_timeline_play_idx;
            }
            StepTimelinePlayhead(step_delta);
        }
    } else if (!g_is_playing && !g_timeline_frames.empty()) {
        /* Sync play_idx with manual selection if possible */
        if (g_doc->ilselected != g_timeline_frames[g_timeline_play_idx]) {
            for (size_t i = 0; i < g_timeline_frames.size(); i++) {
                if (g_timeline_frames[i] == g_doc->ilselected) {
                    g_timeline_play_idx = (int)i;
                    break;
                }
            }
        }
    }

    float timeline_y = sh - PALETTE_H - TIMELINE_H;
    ImGui::SetNextWindowPos(ImVec2(0, timeline_y));
    ImGui::SetNextWindowSize(ImVec2(sw, TIMELINE_H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##timeline", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    {
        ImGui::Text("Animation Timeline");
        ImGui::SameLine(180);
        
        if (g_is_playing) {
            if (ImGui::Button("\xEE\x81\x8D Stop", ImVec2(80, 0))) { /* U+E04D stop */
                g_is_playing = false;
            }
        } else {
            if (ImGui::Button("\xEE\x80\xB7 Play", ImVec2(80, 0))) { /* U+E037 play_arrow */
                if (!g_is_playing && !g_timeline_frames.empty()) {
                    if (TimelineCompositeReady()) {
                        int p0 = TimelineFramePosition(g_timeline_composite[0]);
                        if (p0 >= 0) g_timeline_play_idx = p0;
                    }
                    g_play_timer = 0.0f;
                    g_is_playing = true;
                    g_doc->ilselected = g_timeline_frames[g_timeline_play_idx];
                    g_zoom_reset = true;
                }
            }
        }
        ImGui::SameLine();
        ImGui::PushItemWidth(120);
        ImGui::SliderFloat("FPS", &g_play_speed, 1.0f, 60.0f, "%.1f");
        ImGui::PopItemWidth();

        ImGui::SameLine();
        int cur_hold = TimelineHoldAt(g_timeline_play_idx);
        ImGui::PushItemWidth(72);
        if (ImGui::InputInt("Hold", &cur_hold, 1, 4))
            TimelineSetHoldAt(g_timeline_play_idx, cur_hold);
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Base ticks to wait before this frame advances. At 12 FPS, Hold 3 lasts 0.25 seconds.");
        
        ImGui::SameLine();
        if (ImGui::Button("Reset Sequence")) {
            TimelineClearFrames();
            ClearTimelineCompositeSelection();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Onion", &g_timeline_onion);
        /* SetTooltip is printf-style; escape the literal % so it isn't read
           as a format specifier (CodeQL cpp/wrong-number-format-arguments). */
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ghost prev/next timeline frame at 25%% alpha while scrubbing or playing");
        ImGui::SameLine();
        if (ImGui::Checkbox("Ping-Pong", &g_timeline_pingpong)) {
            /* Reset direction so the first cycle after enabling always
               starts forward, regardless of which way we were going. */
            g_timeline_play_dir = 1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play forward then reverse and loop (e.g. 1-7 then 7-1, repeating)");
        if (TimelineCompositeReady()) {
            ImGui::SameLine();
            DrawTimelineCompositeLockToggle(0, "Back");
            ImGui::SameLine();
            DrawTimelineCompositeLockToggle(1, "Front");
            ImGui::SameLine();
            bool can_auto_anipts = TimelineAnyCompositeLocked() &&
                                   g_timeline_frames.size() > 1;
            if (!can_auto_anipts) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Auto Anipts")) {
                int changed = AutoCalculateTimelineAnipointsFromLock();
                if (changed > 0) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Auto-calculated anim points for %d timeline frame%s.",
                             changed, changed == 1 ? "" : "s");
                } else {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Timeline anim points already match sprite sizes.");
                }
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses the locked frame as the anchor, then offsets each neighboring frame by half the sprite size difference.");
            }
            if (!can_auto_anipts) ImGui::EndDisabled();
        }

        /* Draw a horizontal scrolling list of frames with drag & drop. Buttons
           render a per-frame thumbnail so the user can scan visually instead
           of by numeric index. */
        ImGui::Dummy(ImVec2(0, 4));
        float scr_w = ImGui::GetContentRegionAvail().x;
        float scr_h = ImGui::GetContentRegionAvail().y;

        ImGui::BeginChild("TimelineScrubber", ImVec2(scr_w, scr_h), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
        if (!g_timeline_frames.empty()) {
            for (size_t i = 0; i < g_timeline_frames.size(); i++) {
                if (i > 0) ImGui::SameLine(0, 4.0f);
                ImGui::PushID((int)i);

                int img_idx = g_timeline_frames[i];
                char label[32];
                snprintf(label, sizeof(label), "%d", img_idx);

                bool is_current = (int)i == g_timeline_play_idx;
                int composite_slot = TimelineCompositeSlot(img_idx);
                if (is_current) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
                }

                TimelineThumb *t = EnsureThumb(img_idx);
                bool clicked = false;
                ImVec2 item_min(0, 0), item_max(0, 0);
                if (t && t->tex) {
                    /* Frame: thumb + index label below in same button. We use
                       an ImageButton with the rendered thumbnail and overlay
                       text via the drawlist after. */
                    ImVec2 btn_sz(52, 52);
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    if (ImGui::ImageButton(label, (ImTextureID)(intptr_t)t->tex,
                                           btn_sz, ImVec2(0,0), ImVec2(1,1),
                                           ImVec4(0,0,0,0),
                                           is_current ? ImVec4(0.4f,0.7f,1.f,1.f) : ImVec4(1,1,1,1))) {
                        clicked = true;
                    }
                    item_min = ImGui::GetItemRectMin();
                    item_max = ImGui::GetItemRectMax();
                    ImDrawList *fdl = ImGui::GetWindowDrawList();
                    fdl->AddText(ImVec2(cursor.x + 4, cursor.y + 2),
                                 IM_COL32(255,255,255,200), label);
                } else {
                    if (ImGui::Button(label, ImVec2(48, 48))) clicked = true;
                    item_min = ImGui::GetItemRectMin();
                    item_max = ImGui::GetItemRectMax();
                }
                int frame_hold = TimelineHoldAt((int)i);
                if (frame_hold > 1) {
                    char hold_label[16];
                    snprintf(hold_label, sizeof(hold_label), "x%d", frame_hold);
                    ImDrawList *fdl = ImGui::GetWindowDrawList();
                    ImVec2 hold_sz = ImGui::CalcTextSize(hold_label);
                    ImVec2 hold_min(item_max.x - hold_sz.x - 8.0f, item_max.y - hold_sz.y - 6.0f);
                    ImVec2 hold_max(item_max.x - 2.0f, item_max.y - 2.0f);
                    fdl->AddRectFilled(hold_min, hold_max, IM_COL32(0, 0, 0, 185), 2.0f);
                    fdl->AddText(ImVec2(hold_min.x + 3.0f, hold_min.y + 1.0f),
                                 IM_COL32(255, 235, 130, 255), hold_label);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Ctrl-click two frames to pair them.\nPlay or Left/Right advances both positions together.\nHold: x%d", frame_hold);
                }
                if (composite_slot >= 0) {
                    ImDrawList *fdl = ImGui::GetWindowDrawList();
                    ImU32 col = (composite_slot == 0)
                        ? IM_COL32(120, 190, 255, 255)
                        : IM_COL32(255, 190, 90, 255);
                    fdl->AddRect(item_min, item_max, col, 0.0f, 0, 3.0f);
                }
                if (clicked) {
                    if (io.KeyCtrl) {
                        ToggleTimelineCompositeFrame(img_idx);
                    } else {
                        ClearTimelineCompositeSelection();
                    }
                    g_timeline_play_idx = (int)i;
                    g_doc->ilselected = img_idx;
                    g_zoom_reset = true;
                }

                if (is_current) {
                    ImGui::PopStyleColor();
                }
                
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    int payload_idx = (int)i;
                    ImGui::SetDragDropPayload("TIMELINE_FRAME", &payload_idx, sizeof(int));
                    ImGui::Text("Move frame %d", img_idx);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TIMELINE_FRAME")) {
                        int src_idx = *(const int*)payload->Data;
                        int dst_idx = (int)i;
                        TimelineMoveFrame(src_idx, dst_idx);
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
    ImGui::PopStyleVar();

    /* ===== BOTTOM PALETTE BAR ===== */
    float pal_y = sh - PALETTE_H;
    ImGui::SetNextWindowPos(ImVec2(0, pal_y));
    ImGui::SetNextWindowSize(ImVec2(sw, PALETTE_H));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 4));
    ImGui::Begin("##palette", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    {
        ImDrawList *dl   = ImGui::GetWindowDrawList();
        ImVec2      pos0 = ImGui::GetCursorScreenPos();
        float       gap  = 1.0f;
        float       header_h = 34.0f;
        int         pal_cols = 16;
        float       swatch = 7.0f;
        const int   col_options[] = {64, 48, 32, 16};
        float       grid_h_avail = PALETTE_H - header_h - 10.0f;
        float       grid_w_avail = sw - 8.0f;
        for (int opt : col_options) {
            int rows = (256 + opt - 1) / opt;
            float h_fit = floorf((grid_h_avail - gap * (float)(rows - 1)) / (float)rows);
            float w_fit = floorf((grid_w_avail - gap * (float)(opt - 1)) / (float)opt);
            float size = h_fit < w_fit ? h_fit : w_fit;
            if (size > 16.0f) size = 16.0f;
            if (size >= 7.0f) {
                pal_cols = opt;
                swatch = size;
                break;
            }
        }
        float       row_h = swatch + gap;
        float       col_w = swatch + gap;
        ImVec2      grid_pos(pos0.x, pos0.y + header_h);
        BuildSelectedPaletteUsage();
        PAL        *usage_pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
        int         usage_numc = g_palette_usage_pal_numc;
        int         candidate_colors = usage_numc > 0 ? usage_numc - 1 : 0;
        SDL_Color  &selc = g_palette[g_sel_color];
        unsigned long long sel_use =
            (g_sel_color >= 0 && g_sel_color < 256) ? g_palette_usage_counts[g_sel_color] : 0;

        dl->AddRectFilled(pos0, ImVec2(pos0.x + sw, pos0.y + header_h - 3.0f),
                          IM_COL32(8, 8, 8, 245));
        ImGui::SetCursorScreenPos(ImVec2(pos0.x + 6.0f, pos0.y + 1.0f));
        ImGui::Text("Pal %d %.12s   #%d  R:%d G:%d B:%d",
                    g_doc->plselected,
                    usage_pal ? usage_pal->n_s : "",
                    g_sel_color, selc.r, selc.g, selc.b);
        if (sw >= 980.0f) {
            ImGui::SameLine();
            ImGui::TextDisabled("selected %llu px", (unsigned long long)sel_use);
            ImGui::SameLine();
            ImGui::TextDisabled("%d/%d used   %d free   %d low <= %d",
                                g_palette_usage_used_colors,
                                candidate_colors,
                                g_palette_usage_unused_colors,
                                g_palette_usage_low_colors,
                                g_palette_usage_low_threshold);
        }
        ImGui::SetCursorScreenPos(ImVec2(pos0.x + 6.0f, pos0.y + 18.0f));
        if (sw < 980.0f) {
            ImGui::TextDisabled("selected %llu px   %d/%d used   %d free   %d low <= %d",
                                (unsigned long long)sel_use,
                                g_palette_usage_used_colors,
                                candidate_colors,
                                g_palette_usage_unused_colors,
                                g_palette_usage_low_colors,
                                g_palette_usage_low_threshold);
        } else {
            ImVec2 legend_pos = ImGui::GetCursorScreenPos();
            dl->AddTriangleFilled(ImVec2(legend_pos.x, legend_pos.y + 2.0f),
                                  ImVec2(legend_pos.x + 8.0f, legend_pos.y + 2.0f),
                                  ImVec2(legend_pos.x + 8.0f, legend_pos.y + 10.0f),
                                  IM_COL32(0, 220, 255, 235));
            ImGui::SetCursorScreenPos(ImVec2(legend_pos.x + 14.0f, legend_pos.y));
            ImGui::TextDisabled("unused");
            ImGui::SameLine();
            ImVec2 dot_pos = ImGui::GetCursorScreenPos();
            dl->AddCircleFilled(ImVec2(dot_pos.x + 5.0f, dot_pos.y + 7.0f),
                                3.0f, IM_COL32(255, 185, 40, 255), 8);
            ImGui::SetCursorScreenPos(ImVec2(dot_pos.x + 14.0f, dot_pos.y));
            ImGui::TextDisabled("low use");
        }

        for (int i = 0; i < 256; i++) {
            int row = i / pal_cols, col = i % pal_cols;
            ImVec2 p0(grid_pos.x + col * col_w, grid_pos.y + row * row_h);
            ImVec2 p1(p0.x + swatch, p0.y + swatch);
            SDL_Color c = g_palette[i];
            bool in_palette = (i < usage_numc);
            unsigned long long use_count = g_palette_usage_counts[i];
            dl->AddRectFilled(p0, p1, IM_COL32(c.r, c.g, c.b, 255));
            /* Borders: the swatch can have multiple states at once
               (e.g. it's the current color AND in the multi-selection).
               Draw them as concentric rings so each is visible. */
            if (i == g_sel_color)
                dl->AddRect(p0, p1, IM_COL32(255,255,255,255), 0, 0, 1.5f);
            else if (i == 0)
                dl->AddRect(p0, p1, IM_COL32(80,80,80,120), 0, 0, 0.5f);
            if (g_palette_selection[i]) {
                /* Inset yellow ring so it coexists with the white current-color ring. */
                dl->AddRect(ImVec2(p0.x + 2, p0.y + 2),
                            ImVec2(p1.x - 2, p1.y - 2),
                            IM_COL32(255, 255, 0, 255), 0, 0, 1.5f);
            }

            /* Isolation badge */
            if (g_isolate_color == i) {
                dl->AddRect(p0, p1, IM_COL32(255, 0, 255, 255), 0, 0, 2.0f);
            }

            if (in_palette && i > 0 && use_count == 0) {
                float tri = swatch < 10.0f ? 5.0f : 7.0f;
                ImVec2 t0(p1.x - tri, p0.y);
                ImVec2 t1(p1.x, p0.y);
                ImVec2 t2(p1.x, p0.y + tri);
                dl->AddTriangleFilled(t0, t1, t2, IM_COL32(0, 220, 255, 235));
                float inset = swatch < 10.0f ? 1.0f : 2.0f;
                dl->AddLine(ImVec2(p0.x + inset, p1.y - inset),
                            ImVec2(p1.x - inset, p0.y + inset),
                            IM_COL32(0, 0, 0, 220), 1.25f);
                dl->AddLine(ImVec2(p0.x + inset, p1.y - inset),
                            ImVec2(p1.x - inset, p0.y + inset),
                            IM_COL32(255, 255, 255, 235), 0.75f);
            } else if (in_palette && i > 0 &&
                       use_count <= (unsigned long long)g_palette_usage_low_threshold) {
                float r = swatch < 10.0f ? 1.6f : 2.2f;
                ImVec2 dot(p1.x - r - 1.0f, p0.y + r + 1.0f);
                dl->AddCircleFilled(dot, r + 0.7f, IM_COL32(0, 0, 0, 210), 8);
                dl->AddCircleFilled(dot, r, IM_COL32(255, 185, 40, 255), 8);
            }

            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton(("##sw" + std::to_string(i)).c_str(), ImVec2(swatch, swatch));
            if (ImGui::IsItemClicked()) {
                ImGuiIO &cio = ImGui::GetIO();
                if (cio.KeyAlt) {
                    /* Alt-click: toggle color isolation on this index */
                    g_isolate_color = (g_isolate_color == i) ? -1 : i;
                } else if (cio.KeyCtrl) {
                    commit_palette_adjustments();
                    /* Ctrl-click: toggle this swatch's membership in the
                       multi-selection. Doesn't move g_sel_color — Photoshop
                       convention — so the active color stays put and the
                       toggle's yellow ring change is visible against an
                       unchanged white ring (or its absence). */
                    g_palette_selection[i] = !g_palette_selection[i];
                } else if (cio.KeyShift) {
                    commit_palette_adjustments();
                    int start = g_sel_color < i ? g_sel_color : i;
                    int end = g_sel_color < i ? i : g_sel_color;
                    for (int j = start; j <= end; j++) g_palette_selection[j] = true;
                    g_sel_color = i;
                } else {
                    commit_palette_adjustments();
                    memset(g_palette_selection, 0, sizeof(g_palette_selection));
                    g_sel_color = i;
                }
            }
            if (ImGui::BeginPopupContextItem(("##swctx" + std::to_string(i)).c_str())) {
                if (i == 0) {
                    if (ImGui::MenuItem("Copy #0 to Free Opaque Slot")) {
                        CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None);
                    }
                } else {
                    if (ImGui::MenuItem("Copy #0 Color Here")) {
                        CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None, i);
                    }
                }
                ImGui::Separator();
                if (!g_grid_sel.active) ImGui::BeginDisabled();
                if (ImGui::MenuItem(i == 0 ? "Copy #0 + Remap Selection"
                                           : "Copy #0 Here + Remap Selection")) {
                    CopyPaletteZeroAndRemap(PaletteZeroRemapMode::Selection, i == 0 ? -1 : i);
                }
                if (!g_grid_sel.active) ImGui::EndDisabled();
                if (ImGui::MenuItem(i == 0 ? "Copy #0 + Remap Current Sprite"
                                           : "Copy #0 Here + Remap Current Sprite")) {
                    CopyPaletteZeroAndRemap(PaletteZeroRemapMode::CurrentImage, i == 0 ? -1 : i);
                }
                ImGui::EndPopup();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Palette index %d", i);
                if (!in_palette) {
                    ImGui::TextDisabled("Outside selected palette (%d colors)", usage_numc);
                } else if (i == 0) {
                    ImGui::TextDisabled("Index 0 pixels are transparent");
                    ImGui::Text("Usage: %llu pixel%s",
                                (unsigned long long)use_count,
                                use_count == 1 ? "" : "s");
                } else {
                    if (use_count == 0) {
                        ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.0f, 1.0f),
                                           "Unused by sprites using this palette");
                    } else {
                        ImGui::Text("Usage: %llu pixel%s across %d sprite%s",
                                    (unsigned long long)use_count,
                                    use_count == 1 ? "" : "s",
                                    g_palette_usage_img_count,
                                    g_palette_usage_img_count == 1 ? "" : "s");
                        if (use_count <= (unsigned long long)g_palette_usage_low_threshold) {
                            ImGui::TextColored(ImVec4(1.0f, 0.74f, 0.24f, 1.0f),
                                               "Low-use candidate");
                        }
                    }
                    int nearest_dist = 0;
                    int nearest = FindNearestUsedPaletteSlotForUsage(i, &nearest_dist);
                    if (nearest >= 0) {
                        ImGui::TextDisabled("Nearest used color: #%d (distance %.1f)",
                                            nearest, sqrt((double)nearest_dist));
                    }
                }
                ImGui::TextDisabled("Right-click: #0 relocation tools");
                ImGui::TextDisabled("Alt+click: isolate this color in the canvas");
                ImGui::TextDisabled("Ctrl/Shift+click: multi-select — selected swatches stay lit, rest dim on canvas");
                ImGui::EndTooltip();
            }
        }
            }
            ImGui::PopStyleVar();
    ImGui::End();

    DrawRenameDialog();

    DrawLoad2VerifyDialog();

    DrawPaletteMergeQualityDialog();

    DrawSpriteLayerPanel();

    if (g_request_save_world_asm) { g_request_save_world_asm = false; OpenFileDialog(FileDialogMode::SaveAsmAnim); }
    if (g_request_load_asm)       { g_request_load_asm = false; g_asm_dialog_opponent = false; OpenFileDialog(FileDialogMode::LoadAsmAnim); }
    if (g_request_load_opp_asm)   { g_request_load_opp_asm = false; g_asm_dialog_opponent = true; OpenFileDialog(FileDialogMode::LoadAsmAnim); }
    if (g_request_asm_autoload)     { g_request_asm_autoload = false; AsmProcessAutoload(); }
    if (g_request_asm_opp_autoload) { g_request_asm_opp_autoload = false; AsmProcessOppAutoload(); }
    if (g_request_locate_img)     { g_request_locate_img = false; g_openimg_for_asm = true; OpenFileDialog(FileDialogMode::OpenImg); }
    else if (g_request_locate_opp_img) { g_request_locate_opp_img = false; g_openimg_for_opp = true; OpenFileDialog(FileDialogMode::OpenImg); }

    DrawAsmAnimWindow();

    DrawPaletteHistogramDialog();

    DrawPaletteReduceDialog();

    DrawMk2HitboxWindow();

    DrawMk2FatalityWindow();

    DrawAutoChopDialog();

    DrawResizeSpriteDialog();

    DrawBulkResizeDialog();

    DrawBulkRestoreRegexDialog();

    DrawDeleteImagesConfirm();

    DrawDebugInfoModal();

    /* ===== FILE DIALOG ===== */
    DrawFileDialog();

    DrawNewImgConfirm();

    DrawNewBlankImageDialog();

    DrawUnsavedChangesConfirm();

    DrawMk2UnsavedChangesConfirm();

    DrawMk2FatalityUnsavedChangesConfirm();

    DrawHelpModal();

    DrawAboutModal();
    DrawTransientToast(io.DeltaTime);
    DrawVerboseLogWindow();
    finish_sequence_anipoint_edit_if_idle();

    /* Flush to renderer */
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_imgui_renderer);
}

void imgui_overlay_shutdown(void)
{
    SessionSave();
    if (g_img_texture) { SDL_DestroyTexture(g_img_texture); g_img_texture = NULL; }
    ClearCanvasUiTextures();
    ClearPaletteReducePreviewTextures();
    ClearWorldTempTextures();
    ClearTimelineThumbCache();
    ClearPixelHistoryStacks();
    ClearDocumentHistoryStacks();
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void imgui_overlay_present(void)
{
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_imgui_renderer);
}
