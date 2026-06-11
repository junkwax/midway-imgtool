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
#include "ui_main.h"
#include "ui_canvas.h"
#include "ui_timeline.h"
#include "ui_palette.h"
#include "ui_tools.h"
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

/* TOOLBAR_W and PANEL_W are now defined in ui_state.cpp */


/* ---- Undo system ---- */
/* EditSnapshot, UNDO_STACK_SIZE, and g_undo[]/g_undo_idx/g_undo_count moved to
   ui_internal.h / ui_state.cpp. */

/* ---- Clipboard (pixel data only) ---- */

static void ClearPixelClipboard(void)
{
    if (g_clipboard.data_p) free(g_clipboard.data_p);
    memset(&g_clipboard, 0, sizeof(g_clipboard));
}

bool BuildClipboardPaletteMap(const PAL *target_pal, unsigned char map[256])
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

/* ---- Editor state ---- */
/* g_show_dma_comp defined in ui_state.cpp */

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

/* ImageListSort is defined in ui_internal.h and defined in ui_state.cpp */

/* Unsaved changes confirmation — now defined in ui_state.cpp */
int           g_doc_tab_select_request = 0;
/* g_pending_quit defined in ui_state.cpp */

/* Anipoint drag state, hoisted to file scope so the pencil branch can
   gate on it (the anipoint render block runs *after* the pencil block,
   so widget_consumed_click is too late to suppress the first paint
   frame of a drag). */
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
void LabeledValue(const char *label, const char *fmt, ...)
{
    ImGui::TextUnformatted(label);
    ImGui::SameLine(90.0f);
    va_list args;
    va_start(args, fmt);
    ImGui::TextV(fmt, args);
    va_end(args);
}

bool AnimPointSliderInt(const char *label, int *value, int min_value, int max_value)
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
/* g_show_new_img_confirm defined in ui_state.cpp */
/* New-image dialog state. Width/height persist between opens so the user
   doesn't have to retype after a stream of additions at the same size. */
/* g_show_new_blank_dialog defined in ui_state.cpp */
static int  g_new_blank_w = 32;
static int  g_new_blank_h = 32;



/* Deferred sprite delete confirmation variables now defined in ui_state.cpp */

void InvalidatePaletteSync(void)
{
    g_palette_sync_serial++;
    if (g_palette_sync_serial == 0) g_palette_sync_serial = 1;
}


static int  g_selection_add_mask_w = 0;
static int  g_selection_add_mask_h = 0;
static std::vector<bool> g_selection_add_mask;

/* Active tool state (moved to ui_internal.h / ui_state.cpp) */



/* Timeline thumbnail cache (TimelineThumb, g_thumb_cache, EnsureThumb,
   InvalidateThumb, ClearTimelineThumbCache) now lives in ui_timeline.{h,cpp}. */

/* Timeline state (lifted to file scope so keyboard shortcuts and onion-skin
   can address it). g_timeline_built_for_imgcnt drives stale-index pruning. */
/* g_is_playing lives in ui_timeline.{h,cpp}. */
/* g_play_speed, g_play_timer, g_timeline_built_for_imgcnt, g_timeline_pingpong, g_timeline_play_dir defined in ui_state.cpp */
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

/* Tool properties (moved to ui_internal.h / ui_state.cpp) */

/* Snap-to-content cached bbox (recomputed when Shift goes down on a paste drag) */
/* snap variables declared in ui_internal.h and defined in ui_state.cpp */



void undo_push(void);

int FindDirtyDocumentIndex(void);
static bool HasDirtyDocuments(void);
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


static void ClearPixelHistoryStacks(void)
{
    for (auto &e : g_pixel_hist) pixel_hist_free(&e);
    for (auto &e : g_pixel_redo) pixel_hist_free(&e);
    g_pixel_hist.clear();
    g_pixel_redo.clear();
}

void ResetPerDocumentUiState(bool clear_pixel_clipboard)
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
    ResetPaletteUiState();
    g_snap_bbox.valid = false;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    g_palette_baseline_nc = 0;
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    InvalidatePaletteSync();
}

/* g_show_histogram, g_show_load2_verify, and g_load2_report defined in ui_state.cpp */

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

        /* last load/save message */
 /* errors stay until next action; success messages clear on edit */
  /* selected char-table index */
  /* selected move index within that table */
 /* filter for the move list */

/* ---- MK2 fatality lab state ---- */
/* g_show_mk2_fatality defined in ui_state.cpp */









static char                 g_mk2_fatality_insert_anim[256] = "\t.long\t0";
static char                 g_mk2_fatality_insert_combo[256] = "\t.word\tsw_right";









/* Resolve the current MK2 record index, or -1 if no valid selection. */
int Mk2CurrentRecord(void) {
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
void Mk2AutoSelectFromImg(void)
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
void Mk2SelectRecord(int rec_idx) {
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
/* g_world_state and g_world_marked_state defined in ui_state.cpp */
/* g_world_temp_textures + ClearWorldTempTextures + BuildWorldSpriteTexture +
   doc_get_pal now live in world_render.{h,cpp}. */
static int   g_load2_selected_idx = -1;          /* index into g_load2_report.issues */



/* ---- ASM animation viewer ----
   Parses a MK2 per-character ASM (e.g. MKRD.ASM) and lets the user inspect /
   play the animations it defines against the currently loaded IMG. An anim
   like a_rdstance is a list of frame-GROUP labels; each group lists sprite
   piece symbols (RNSTANCE1A,...,0) that resolve to IMG frames by name. */
std::vector<AsmAnim> g_asm_anims;
int          g_asm_anim_sel = -1;

bool         g_show_asm_anim = false;







 /* anipoint-anchored bbox origin */

      /* doc the anim's frames resolved against */

 /* show the selected anim as a World View lane */
/* g_request_save_world_asm, g_request_load_asm, g_request_load_opp_asm, g_request_locate_img, g_request_locate_opp_img, g_request_asm_autoload, g_request_asm_opp_autoload defined in ui_state.cpp */
std::vector<AsmAnim> g_asm_opp_anims;
int          g_asm_opp_sel = -1;

Document    *g_asm_opp_doc = NULL;
int          g_asm_opp_doc_idx = -1;

bool         g_asm_dialog_opponent = false;
bool         g_openimg_for_asm = false;
bool         g_openimg_for_opp = false;

void AsmResolveAnimAgainstDoc(AsmAnim &a, Document *doc);

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

std::string sprite_family_key(const std::string &name)
{
    return (name.size() > 2) ? name.substr(2) : name;
}

std::string sprite_family_regex_pattern(const std::string &name)
{
    if (name.size() > 2)
        return std::string("^..") + regex_escape(name.substr(2)) + "$";
    return std::string("^") + regex_escape(name) + "$";
}

int PushAnipointsToMatchingOpenTabs(const IMG *src, int *matched_count, int *doc_count, std::string *pattern_out)
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

int CountMarkedImages(void)
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

void MirrorMarkedAnipointsToReverseWithToast(void)
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

bool DrawWorldMarkedTabs(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
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
void update_drift_texture(IMG *img)
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

void AutoChopSetThreeBandSize(void)
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

void OpenAutoChopDialog(void)

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

bool BuildAutoChopPreviewForImage(const IMG *img, AutoChopPreview *out)
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

bool BuildBestAutoSplitPreviewForImage(const IMG *img, bool vertical,
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

bool SelectedImageWillAutoChop(void)
{
    if (g_doc->ilselected < 0) return false;
    int marked = CountMarkedImages();
    if (marked == 0) return true;
    IMG *img = get_img(g_doc->ilselected);
    return img && (img->flags & 1);
}

void BuildAutoChopTargetSummary(AutoChopPreview *out)
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

void BuildAutoSplitTargetSummary(bool vertical,
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

int ApplyBestAutoSplitToTargets(bool vertical)
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

void DrawAutoChopPreviewRects(ImDrawList *dl,
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


void NormalizeImageDeleteIndices(std::vector<int> *indices)
{
    if (!indices) return;
    indices->erase(std::remove_if(indices->begin(), indices->end(),
        [](int idx) { return idx < 0 || (unsigned int)idx >= g_doc->imgcnt; }),
        indices->end());
    std::sort(indices->begin(), indices->end());
    indices->erase(std::unique(indices->begin(), indices->end()), indices->end());
}

/* Swap two adjacent IMG nodes in the linked list. `before_a` is the node
   whose nxt_p points at `a` (or NULL if `a` is the head); `b` must equal
   a->nxt_p. After the call, the order is ...before_a -> b -> a -> b->nxt_p. */
/* Toggle the selected image's point table: allocate via the asm pool when
   absent (so mem_free can release it later), free when present. No "are you
   sure" — matches DeleteImage's no-confirm behavior. */
/* Clear all "extra" anipt/pttbl data on every image. Mirrors ilst_clrxdata:
   clears the secondary anipoint sentinel and the contents of any attached
   PTTBL (without freeing the PTTBL itself, so toggle state is preserved). */
/* ---- C++ ports of ASM operations ---- */

void OpenRenameImage(void);  /* forward decl — used by DuplicateImage */

/* Flood fill helper — 4-connected stack-based fill */
void FloodFill(IMG *img, int sx, int sy, unsigned char new_color)
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

int PaintBucketFill(IMG *img, int sx, int sy, unsigned char new_color, int tolerance, bool contiguous)
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
void SmartErase(IMG *img, int sx, int sy, int tolerance, bool contiguous, bool defringe)
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
/* Swap to the alternate (second) image list.  Purely swaps globals —
   no ASM dependencies. */
/* Set the selected image's PTTBL.ID to (g_doc->il2selected + 1).  If no PTTBL
   exists for the image, allocate one via the ASM memory pool. */
void SetIDFromSecondList(void)

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
void DuplicateImage(void)
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
void AddNewBlankImage(int w, int h)
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


/* VariantPaintResult definition moved to ui_internal.h */


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

bool selection_contains_pixel(IMG *img, int x, int y)
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

VariantPaintResult ApplyVariantBrush(IMG *img, int cx, int cy, int brush)
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

void ApplyVariantToSelection(void)
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

int ApplyMarkedLikenessToSelected(void)

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

void SplitSelectionToOverlayFrame(bool clear_source)
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

int RemoveHardStrokeFromTargets(int max_width)

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
void StripMarkedImages(int max_transparent_neighbors, int specific_color)
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
void DitherReplaceMarkedImages(int specific_color)

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
void LeastSquaresReduceMarked()

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

/* File Dialog and Modals now live in ui_modals.{h,cpp} */

/* Help modal */
/* g_show_help, g_show_debug, and g_show_about are defined in ui_state.cpp */
const char *g_help_text =
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


/* ---- Image texture renderer ---- */
void rebuild_img_texture(IMG *img)
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
void copy_image(bool cut)

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

void PasteClipboardAsNewImage(void)

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

void CutSelectionToNewImage(void)

{
    if (g_doc->ilselected < 0) return;
    copy_image(true);
    if (g_clipboard.valid) PasteClipboardAsNewImage();
}

void CopySelectionToNewImage(void)

{
    if (g_doc->ilselected < 0) return;
    copy_image(false);
    if (g_clipboard.valid) PasteClipboardAsNewImage();
}

const PasteBlendMode k_paste_blend_modes[] = {
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

const char *PasteBlendModeName(PasteBlendMode mode)
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

bool paste_preview_rgba(unsigned char src_ci, unsigned char dst_ci,
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
void flip_clipboard_horizontal(void)

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

void flip_clipboard_vertical(void)

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
void flatten_img_layer(IMG *img)
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

void delete_img_layer(IMG *img)
{
    if (img && img->layer_p) { free(img->layer_p); img->layer_p = NULL; g_img_tex_idx = -2; }
}

void flip_layer_horizontal(SpriteLayer *L)
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
void flip_layer_vertical(SpriteLayer *L)
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
void drop_paste_to_layer(void)

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

void apply_pasted_region(void)
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
void select_all(void)

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
void deselect_all(void)

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
void invert_selection(void)

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

void selection_begin_add_drag(int sw, int sh, bool add)
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

void selection_finish_add_drag(int sw, int sh)
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

void paste_image(void)

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
void xform_begin(void)
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
void xform_cancel(void)
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
void xform_commit(void)
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

void OpenResizeSpriteDialog(void)
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

int AutoCalculateTimelineAnipointsFromLock(void)
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

bool TransformSelectedSprite(SpriteTransformOp op)
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

void DrawSpriteTransformMenuItems(void)
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

void OpenBulkResizeDialog(void)

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

void DrawResizeSpriteDialog(void)
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

void DrawBulkResizeDialog(void)
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

/* Unified Undo/Redo helpers used by the global shortcut, the Edit menu,
   and the toolbar buttons. Pixel strokes, document/palette snapshots, and
   legacy anipoint/hitbox snapshots share a sequence number so mixed edits
   undo in the order the user made them. */
bool CanUndo(void) { return !g_pixel_hist.empty() || !g_doc_hist.empty() || g_undo_idx > 0; }
bool CanRedo(void) { return !g_pixel_redo.empty() || !g_doc_redo.empty() || g_undo_idx < g_undo_count - 1; }

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

void DoUndo(void)
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

void DoRedo(void)
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

int FindDirtyDocumentIndex(void)
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

float DrawDocumentTabBar(float y, float sw)
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
    DrawMainLayout();
    
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
