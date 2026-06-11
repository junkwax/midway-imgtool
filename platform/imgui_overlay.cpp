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





void ClearDocumentRedoStack(void);
/* Free the buffer inside a PixelHist (caller still owns the vector slot). */
/* Capture the current pixel buffer of an image into `out`.
   Returns true if `out` now owns a valid snapshot. */
/* Stroke-begin: push a fresh pre-stroke snapshot. Drops the oldest entry
   if the stack is full; clears the redo stack since a new edit branch
   invalidates any pending redo. */
/* TOOLBAR_W and PANEL_W are now defined in ui_state.cpp */


/* ---- Undo system ---- */
/* EditSnapshot, UNDO_STACK_SIZE, and g_undo[]/g_undo_idx/g_undo_count moved to
   ui_internal.h / ui_state.cpp. */

/* ---- Clipboard (pixel data only) ---- */

/* now lives in ui_canvas.cpp: static void ClearPixelClipboard */

/* now lives in ui_canvas.cpp: bool BuildClipboardPaletteMap */

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

void ClearDocumentRedoStack(void)
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
/* Smart eraser: removes the clicked chroma color (and anything within
   `tolerance` palette-index distance of it) by setting it to index 0
   (transparency). In `contiguous` mode it floods from the click site; in
   global mode every matching pixel in the image is wiped.
   When `defringe` is set, after the chroma pass each transparent pixel
   that touches a still-opaque pixel scans its 8-neighborhood and replaces
   the opaque neighbor with the average of its own non-chroma neighbors —
   this kills the 1-pixel blue-spill halo that survives digitized actor
   bluescreen removal. */
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

void MakeDerivedImageName(const char *base, const char *suffix, char out[16])
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

/* now lives in ui_canvas.cpp: bool selection_contains_pixel */

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
/* ---- Hard Stroke Remover ----
   Detects a 1-2 px matte/outline ring around transparent sprite edges. Unlike
   Strip Edge, this requires the edge color to contrast against nearby inner
   sprite colors, so normal antialiasing and same-color silhouette detail are
   less likely to be erased. */
/* ---- Strip Edge (DMA Compression Prep) ---- */
/* ---- Dither Replace ---- */
/* ---- Least-Squares Reduce (Shrink Palette/Auto-Crop) ---- */
/* File Dialog and Modals now live in ui_modals.{h,cpp} */

/* Help modal */
/* g_show_help, g_show_debug, and g_show_about are defined in ui_state.cpp */



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
/* now lives in ui_canvas.cpp: void copy_image */

/* now lives in ui_canvas.cpp: void PasteClipboardAsNewImage */

/* now lives in ui_canvas.cpp: void CutSelectionToNewImage */

/* now lives in ui_canvas.cpp: void CopySelectionToNewImage */

/* now lives in ui_canvas.cpp: const PasteBlendMode k_paste_blend_modes[] = { */
/* now lives in ui_canvas.cpp: static int paste_clamp_byte */

/* now lives in ui_canvas.cpp: static PasteRGB paste_rgb_from_target_index */

/* now lives in ui_canvas.cpp: static PasteRGB paste_rgb_from_clipboard_index */

/* now lives in ui_canvas.cpp: static PasteRGB paste_quantize_rgb_to_target */

/* now lives in ui_canvas.cpp: static unsigned int paste_dissolve_hash */

/* now lives in ui_canvas.cpp: static bool paste_dissolve_keeps */

/* now lives in ui_canvas.cpp: static int paste_blend_channel */

/* now lives in ui_canvas.cpp: static bool paste_composite_rgb */

/* now lives in ui_canvas.cpp: static unsigned char paste_composite_index */
/* now lives in ui_canvas.cpp: bool paste_preview_rgba */

/* Mirror the floating clipboard in place so a paste can be flipped before it
   is committed with Enter. Operates on palette indices, so it is lossless.
   The floating overlay is drawn straight from the clipboard each frame, so the
   preview updates immediately. */
/* now lives in ui_canvas.cpp: void flip_clipboard_horizontal */

/* now lives in ui_canvas.cpp: void flip_clipboard_vertical */

/* Permanently merge the layer into the host image's pixels and drop it. */
/* now lives in ui_canvas.cpp: void flatten_img_layer */

/* now lives in ui_canvas.cpp: void delete_img_layer */

/* now lives in ui_canvas.cpp: void flip_layer_horizontal */
/* now lives in ui_canvas.cpp: void flip_layer_vertical */

/* Turn the active floating paste into a layer on the selected sprite. The
   clipboard indices are remapped to the host palette first (same nearest-color
   mapping a normal paste uses) so the layer composites with a plain copy.
   Replaces any existing layer (single-overlay model). */
/* now lives in ui_canvas.cpp: void drop_paste_to_layer */

/* now lives in ui_canvas.cpp: void apply_pasted_region */

/* Nearest-neighbor resample the clipboard to exactly (nw, nh). Used both
   by paste-to-fit (downscale-only) and free-transform commit (any scale).
   Nearest-neighbor (not bilinear) is required because the clipboard stores
   palette indices, not RGB — averaging indices produces garbage colors. */
/* now lives in ui_canvas.cpp: static void scale_clipboard_to */

/* now lives in ui_canvas.cpp: static void transform_clipboard_to */

/* Downscale-only convenience wrapper used by paste-to-fit: shrink while
   preserving aspect ratio, no-op if the clipboard already fits. */
/* now lives in ui_canvas.cpp: static void scale_clipboard_to_fit */

/* Marquee-select the entire current sprite. Adobe's Ctrl+A. Stored as a
   rectangle (not a mask) since "everything" is trivially representable. */
/* now lives in ui_canvas.cpp: void select_all */

/* Clear any active marquee / lasso / wand selection. Adobe's Ctrl+D. Does
   NOT cancel a floating paste — that's Esc's job, and overloading Ctrl+D
   to do both would be surprising. */
/* now lives in ui_canvas.cpp: void deselect_all */

/* Invert the current selection. Adobe's Shift+Ctrl+I. Promotes a rect
   selection to a pixel mask so the inversion can be expressed precisely. */
/* now lives in ui_canvas.cpp: void invert_selection */

/* now lives in ui_canvas.cpp: static bool selection_bbox_from_mask */

/* now lives in ui_canvas.cpp: static bool selection_current_to_mask */

/* now lives in ui_canvas.cpp: static void selection_commit_mask */

/* now lives in ui_canvas.cpp: static void selection_apply_mask */

/* now lives in ui_canvas.cpp: void selection_begin_add_drag */

/* now lives in ui_canvas.cpp: void selection_finish_add_drag */

/* now lives in ui_canvas.cpp: void paste_image */

/* Begin Free Transform on the active floating paste. Captures the rect
   geometry at this moment so Esc can revert. The aspect lock persists
   across invocations (g_xform.aspect_locked is not reset here). */
/* now lives in ui_canvas.cpp: void xform_begin */

/* now lives in ui_canvas.cpp: /* Cancel transform — revert rect to its pre-transform geometry; the paste */

/* Commit transform — if the rect dimensions changed, nearest-neighbor
   resample the clipboard to match, then update the paste position to the
   final top-left. After this the floating paste continues normally and the
   user can still move it before final drop. */
/* now lives in ui_canvas.cpp: void xform_commit */

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

/* now lives in ui_canvas.cpp: bool clipboard_secondary_anipoint_in_use */

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
