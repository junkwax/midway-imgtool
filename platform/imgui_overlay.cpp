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


/* now lives in ui_undo.cpp: struct DocSnapshot */

/* now lives in ui_undo.cpp: g_doc_hist */
/* now lives in ui_undo.cpp: g_doc_redo */
/* now lives in ui_undo.cpp: kDocHistMax */

/* now lives in ui_undo.cpp: ImgPixelBytes */

/* now lives in ui_undo.cpp: CloneBytes */

/* now lives in ui_undo.cpp: FreeImgChainForSnapshot */

/* now lives in ui_undo.cpp: FreePalChainForSnapshot */

/* now lives in ui_undo.cpp: CloneImgChainForSnapshot */

/* now lives in ui_undo.cpp: ClonePalChainForSnapshot */

/* now lives in ui_undo.cpp: FreeDocSnapshot */

/* now lives in ui_undo.cpp: CaptureDocSnapshot */

/* now lives in ui_undo.cpp: ClearDocumentRedoStack */

/* now lives in ui_undo.cpp: ClearDocumentHistoryStacks */

/* now lives in ui_undo.cpp: RestoreDocSnapshot */

/* now lives in ui_undo.cpp: doc_undo_push */


/* now lives in ui_undo.cpp: ResetPerDocumentUiState */

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
/* g_load2_selected_idx is defined in ui_state.cpp. */



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

/* now lives in ui_main.cpp: PushAnipointsToMatchingOpenTabs */

/* now lives in ui_canvas.cpp: int CountMarkedImages */

/* anipoint_sequence_parent_name, same_anipoint_sequence,
   begin_sequence_anipoint_edit, finish_sequence_anipoint_edit_if_idle,
   apply_anipoint_delta_to_sequence, and set_primary/secondary_anipoint_with_sequence
   now live in anipoint_edit.{h,cpp}. */

/* now lives in ui_canvas.cpp: void MirrorMarkedAnipointsToReverseWithToast */

/* doc_get_pal now lives in world_render.{h,cpp}. */

/* WorldCollectMarkedFrames now lives in ui_canvas.{h,cpp}. */

/* BuildWorldSpriteTexture now lives in world_render.{h,cpp}. */

/* DrawTimelineCompositePreview now lives in ui_timeline.{h,cpp} — its last
   blockers (world_render, anipoint, anipoint_edit) are all extracted. */

/* now lives in ui_canvas.cpp: bool DrawWorldMarkedTabs */

/* now lives in ui_main.cpp: void update_drift_texture */

static float g_histogram_data[256] = {0};
static float g_histogram_max = 0.0f;
static int   g_histogram_img_count = 0;



/* now lives in ui_autochop.cpp: AutoChopPrimaryTarget */

/* now lives in ui_autochop.cpp: AutoChopSetThreeBandSize */

/* now lives in ui_autochop.cpp: OpenAutoChopDialog */

/* now lives in ui_autochop.cpp: AutoChopPreviewClear */

/* now lives in ui_autochop.cpp: AutoChopBppForImage */

/* now lives in ui_main.cpp: EstimateZcomBitsForRect */

/* now lives in ui_autochop.cpp: BuildAutoChopPreviewForImage */

/* now lives in ui_autochop.cpp: BuildAutoSplitPieceForRect */

/* now lives in ui_autochop.cpp: BuildAutoSplitPreviewForImageAt */

/* now lives in ui_autochop.cpp: BuildBestAutoSplitPreviewForImage */

/* now lives in ui_autochop.cpp: SelectedImageWillAutoChop */

/* now lives in ui_autochop.cpp: BuildAutoChopTargetSummary */

struct AutoChopTargetRef {
    IMG *img;
    int idx;
};

/* now lives in ui_autochop.cpp: CollectAutoChopTargets */



/* now lives in ui_autochop.cpp: AutoSplitTargetSummaryClear */

/* now lives in ui_autochop.cpp: BuildAutoSplitTargetSummary */

/* now lives in ui_autochop.cpp: UnlinkAllocatedImage */

/* now lives in ui_autochop.cpp: CreateAutoSplitPiece */

/* now lives in ui_autochop.cpp: ApplyBestAutoSplitToTargets */

/* now lives in ui_autochop.cpp: AutoChopPieceLabel */

/* now lives in ui_autochop.cpp: DrawAutoChopPreviewRects */

static char g_restore_regex_buf[256] = "^(.+)[A-Z]$";
static std::vector<BulkRestoreMatch> g_restore_matches;
static bool g_restore_regex_tested = false;
static bool g_restore_regex_error = false;
/* Mode: 0 = Replace (overwrite child bbox with parent pixels — clobbers
 * hand-tuned per-piece details). 1 = Diff (only propagate the user's
 * edits to the master, leaving every untouched pixel alone). Diff is the
 * right choice when adding a small detail to a master sprite. */
static int g_restore_diff_mode = 1;


/* now lives in ui_main.cpp: void NormalizeImageDeleteIndices */

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
/* now lives in ui_main.cpp: void SetIDFromSecondList */

/* now lives in ui_main.cpp: static bool ImageNameExists */
/* now lives in ui_main.cpp: void MakeDerivedImageName */

/* now lives in ui_main.cpp: void DuplicateImage */

/* now lives in ui_main.cpp: void AddNewBlankImage */


/* VariantPaintResult definition moved to ui_internal.h */


/* now lives in ui_canvas.cpp: variant paint helpers and ApplyVariantBrush */
/* now lives in ui_canvas.cpp: void ApplyVariantToSelection */

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



/* now lives in ui_main.cpp: void rebuild_img_texture */

/* ---- Undo helpers ---- */
/* now lives in ui_undo.cpp: undo_push */

/* now lives in ui_undo.cpp: undo_apply */

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

/* now lives in ui_canvas.cpp: void xform_cancel */

/* Commit transform — if the rect dimensions changed, nearest-neighbor
   resample the clipboard to match, then update the paste position to the
   final top-left. After this the floating paste continues normally and the
   user can still move it before final drop. */
/* now lives in ui_canvas.cpp: void xform_commit */

/* ---- Full-sprite resize ---- */
/* now lives in subsystems: SpriteResizeMode enum */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */
/* now lives in subsystems: static variables */

/* now lives in ui_canvas.cpp: bool clipboard_secondary_anipoint_in_use */

/* now lives in subsystems: default_anipoints_to_center */

/* now lives in subsystems: scaled_coord */

/* now lives in subsystems: resize_sync_scale_from_dims */

/* now lives in subsystems: resize_sync_dims_from_scale */

/* now lives in subsystems: OpenResizeSpriteDialog */

/* now lives in subsystems: trim_image_to_content */

/* ---- Lossless full-sprite transform ---- */

/* now lives in subsystems: sprite_transform_name */

/* now lives in subsystems: sprite_transform_preserves_anipoints */

/* now lives in subsystems: transform_hitbox */

/* now lives in subsystems: transform_anipoint */

/* secondary_anipoint_in_use now lives in anipoint.{h,cpp}. */

/* now lives in subsystems: rounded_half_delta */

/* now lives in subsystems: timeline_image_locked */

/* now lives in subsystems: locked_timeline_anchor_position */

/* now lives in subsystems: AutoCalculateTimelineAnipointsFromLock */

/* now lives in subsystems: TransformSelectedSprite */

/* now lives in subsystems: DrawSpriteTransformMenuItems */

/* Canvas rotate-button helpers now live in ui_canvas.{h,cpp}. */

/* now lives in subsystems: ResizeSelectedSprite */

/* now lives in subsystems: OpenBulkResizeDialog */

/* now lives in subsystems: BuildResizeFallbackRgb */

/* now lives in subsystems: BulkResizeMarkedSprites */

/* now lives in subsystems: DrawResizeSpriteDialog */

/* now lives in subsystems: DrawBulkResizeDialog */

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
/* now lives in ui_undo.cpp: CanUndo */
/* now lives in ui_undo.cpp: CanRedo */

/* now lives in ui_undo.cpp: DoPixelUndo */

/* now lives in ui_undo.cpp: DoDocUndo */

/* now lives in ui_undo.cpp: DoLegacyUndo */

/* now lives in ui_undo.cpp: DoUndo */

/* now lives in ui_undo.cpp: DoPixelRedo */

/* now lives in ui_undo.cpp: DoDocRedo */

/* now lives in ui_undo.cpp: DoLegacyRedo */

/* now lives in ui_undo.cpp: DoRedo */

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

/* now lives in ui_main.cpp: float DrawDocumentTabBar */

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
