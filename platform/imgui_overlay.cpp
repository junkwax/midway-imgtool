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
#include <cstdlib>
#include <vector>
#include <algorithm>
#include <regex>
#include "compat.h"
#ifdef _WIN32
#include <shlobj.h>
#pragma comment(lib, "shell32.lib")
#endif
#include "img_format.h"
#include "img_io.h"
#include "imgui_overlay.h"
#include "load2_verify.h"
#include "lod_parser.h"

/* PPP setting from img_io.cpp — used to drive the verifier modal. */

/* Globals defined in platform/globals.c */
extern "C" {
extern void          *img_p;
extern unsigned int   imgcnt;
extern int            ilselected;
extern void          *pal_p;
extern unsigned int   palcnt;
extern int            plselected;
extern void          *img2_p;
extern unsigned int   img2cnt;
extern int            il2selected;
extern unsigned int   il1stprt;
extern unsigned int   il21stprt;
extern unsigned int   seqcnt;
extern unsigned int   scrcnt;
extern unsigned int   damcnt;
extern unsigned int   fileversion;
extern void          *scrseqmem_p;
extern unsigned int   scrseqbytes;
extern int            ilpalloaded;
extern char           fpath_s[64];
extern char           fname_s[13];
extern char           fnametmp_s[13];
extern char           exe_dir[];
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

/* ---- SDL state ---- */
static SDL_Window   *g_imgui_window   = NULL;
static SDL_Renderer *g_imgui_renderer = NULL;
static SDL_Texture  *g_canvas_texture = NULL;  /* VGA plane tex — kept for init compat, not displayed */

/* Did we successfully load a Unicode-symbol font? Determines whether toolbar
   shows glyph icons or falls back to short text labels. */
static bool g_icon_font_loaded = false;

/* Toolbar icon glyphs (Material Symbols Sharp codepoints, UTF-8 encoded).
   Codepoints are stable across the Material Symbols family — see
   https://fonts.google.com/icons. When g_icon_font_loaded is false we fall
   back to the *_TXT strings instead. */
#define ICON_OPEN     "\xEE\x8B\x88"     /* U+E2C8 folder_open */
#define ICON_SAVE     "\xEE\x85\xA1"     /* U+E161 save */
#define ICON_MARK     "\xEE\xA2\x92"     /* U+E892 label */
#define ICON_MARK_ALL "\xEE\x85\xA2"     /* U+E162 select_all */
#define ICON_CLEAR    "\xEE\x97\x8D"     /* U+E5CD close */
#define ICON_POINTS   "\xEE\x95\x9F"     /* U+E55F place */
#define ICON_HITBOX   "\xEE\x8F\x82"     /* U+E3C2 crop_free */
#define ICON_MARQUEE  "\xEE\xBD\x92"     /* U+EF52 highlight_alt — dashed-rect marquee */
#define ICON_UNDO     "\xEE\x85\xA6"     /* U+E166 undo */
#define ICON_REDO     "\xEE\x85\x9A"     /* U+E15A redo */

#define ICON_OPEN_TXT     "Op"
#define ICON_SAVE_TXT     "Sv"
#define ICON_MARK_TXT     "Mk"
#define ICON_MARK_ALL_TXT "MA"
#define ICON_CLEAR_TXT    "CM"
#define ICON_POINTS_TXT   "Pt"
#define ICON_HITBOX_TXT   "Hb"
#define ICON_MARQUEE_TXT  "[]"
#define ICON_UNDO_TXT     "Uz"
#define ICON_REDO_TXT     "Ry"

/* Per-image render texture — rebuilt when selected image or palette changes */
static SDL_Texture  *g_img_texture    = NULL;
static int           g_img_tex_w      = 0;
static int           g_img_tex_h      = 0;
extern int           g_img_tex_idx;

/* ---- Zoom / Pan ---- */
static float g_zoom       = 1.0f;
static float g_pan_x      = 0.0f;
static float g_pan_y      = 0.0f;
static bool  g_zoom_reset = true;
static unsigned char *g_pixel_undo = NULL;
static int            g_pixel_undo_img = -1;  /* -2 = never built */

/* ---- Layout constants ---- */
static const float TOOLBAR_W   = 44.0f;
static const float PANEL_W     = 240.0f;
static const float PALETTE_H   = 78.0f;

/* ---- Undo system ---- */
#define UNDO_STACK_SIZE 32
struct EditSnapshot {
    int            image_idx;
    unsigned short anix, aniy;
    unsigned short anix2, aniy2;
    unsigned short w, h;
    unsigned short palnum;
    unsigned short flags;
    int            hitbox_x, hitbox_y, hitbox_w, hitbox_h;
};
static EditSnapshot g_undo[UNDO_STACK_SIZE];
static int          g_undo_idx   = -1;
static int          g_undo_count =  0;

/* ---- Clipboard (pixel data only) ---- */
struct CopiedImage {
    bool           valid;
    unsigned short w, h;
    void          *data_p;  /* pixel data */
    unsigned short stride;  /* bytes per row */
};
static CopiedImage g_clipboard = {false};

/* ---- Editor state ---- */
static int  g_sel_color   = 0;
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

/* Unsaved changes confirmation */
static bool g_show_unsaved_confirm = false;
static bool g_pending_quit = false;
static bool g_dirty = false;

/* New IMG / Add Palette confirmations */
static bool g_show_new_img_confirm = false;

/* Keyboard navigation focus: up/down arrows navigate palettes when true */
static bool g_palette_nav = false;

/* Hue shift slider state (reset on palette change) */
static int g_hue_slider = 0;
static int g_hue_last = 0;
static unsigned char g_palette_baseline[512];
static int g_palette_baseline_nc = 0;

/* Grid selection tool (for copy/paste) */
struct GridSelection {
    bool active;        /* a selection rectangle exists and should be drawn */
    bool dragging;      /* user is currently click-dragging the rect's far corner */
    int x1, y1;         /* start coords (pixels) */
    int x2, y2;         /* end coords (pixels) */
};
static GridSelection g_grid_sel = {false, false, 0, 0, 0, 0};
/* Marquee tool gate: when false, click-drag on the sprite never starts a
   selection. Photoshop-style explicit mode toggle — fixes the green box
   misfiring on stray clicks. Toggled via the toolbar button or 'R' key. */
static bool g_marquee_active = false;

/* Pasted image placement (with move feedback) */
struct PastedImage {
    bool active;        /* paste is active and can be moved */
    int paste_x, paste_y;  /* top-left corner where paste will go */
    bool dragging;      /* user is dragging the paste boundary */
};
static PastedImage g_pasted = {false};

static void ApplyPalette(int pal_idx);
void undo_push(void);

/* ---- Histogram state ---- */
static bool  g_show_histogram = false;
static bool  g_show_load2_verify = false;
static L2Report g_load2_report;

/* ---- World View mode (DOS-tool-style anipoint alignment workspace) ----
 * When on, renders the sprite inside a fixed black canvas at
 * (world_origin - anix, world_origin - aniy). Left-drag the sprite
 * to update its anipoint (drag direction is "sprite follows cursor"
 * which means anix/aniy DECREASE as you drag right/down). Use up/down
 * arrows to flick through frames — origin stays put so you can
 * eyeball whether anipoints line up across frames. */
static bool  g_world_view = false;
static int   g_world_w = 400;       /* arcade playfield width */
static int   g_world_h = 254;       /* arcade playfield height */
static int   g_world_origin_x = 200;/* anchor target inside world */
static int   g_world_origin_y = 200;
static bool  g_world_onion = false; /* faintly draw prev frame underneath */
static SDL_Texture *g_world_onion_tex = NULL;
static int   g_world_onion_tex_w = 0, g_world_onion_tex_h = 0;
static int   g_world_onion_idx = -1; /* which sprite the onion tex holds */
static int   g_load2_selected_idx = -1;          /* index into g_load2_report.issues */
static SDL_Texture *g_load2_drift_tex = NULL;
static int   g_load2_drift_tex_w = 0, g_load2_drift_tex_h = 0;

/* Build a per-row drift overlay texture for the given image. Renders the
 * sprite's current pixels at full color, then tints rows where the
 * baseline-vs-current zero-shape differs in semi-transparent red so the
 * user can see exactly which scanlines will shift LOAD2's destbits. */
static void update_drift_texture(IMG *img)
{
    if (!img || !img->data_p || !img->baseline_p || img->w == 0 || img->h == 0) {
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
static char g_restore_regex_buf[256] = "^(.+)[A-Z]$";
static std::vector<BulkRestoreMatch> g_restore_matches;
static bool g_restore_regex_tested = false;
static bool g_restore_regex_error = false;
/* Mode: 0 = Replace (overwrite child bbox with parent pixels — clobbers
 * hand-tuned per-piece details). 1 = Diff (only propagate the user's
 * edits to the master, leaving every untouched pixel alone). Diff is the
 * right choice when adding a small detail to a master sprite. */
static int g_restore_diff_mode = 1;

static void DeleteImage(int idx)
{
    if (idx < 0 || (unsigned)idx >= imgcnt) return;

    undo_push();

    IMG *prev = NULL;
    IMG *curr = (IMG *)img_p;
    for (int i = 0; curr && i < idx; i++) {
        prev = curr;
        curr = (IMG *)curr->nxt_p;
    }
    if (!curr) return;

    if (prev) prev->nxt_p = curr->nxt_p;
    else img_p = curr->nxt_p;
    imgcnt--;

    if (idx < ilselected) ilselected--;
    else if (idx == ilselected && (unsigned)ilselected >= imgcnt)
        ilselected = (int)imgcnt - 1;

    if (curr->data_p) free(curr->data_p);
    if (curr->pttbl_p) free(curr->pttbl_p);
    free(curr);

    g_img_tex_idx = -2;
}

static void DeleteMarkedImages(void)
{
    undo_push();

    IMG *prev = NULL;
    IMG *curr = (IMG *)img_p;
    int idx = 0;
    int deleted_before_sel = 0;
    bool sel_was_deleted = false;

    while (curr) {
        if (curr->flags & 1) {
            IMG *to_delete = curr;
            if (prev) prev->nxt_p = curr->nxt_p;
            else img_p = curr->nxt_p;
            curr = (IMG *)curr->nxt_p;
            imgcnt--;

            if (idx < ilselected) deleted_before_sel++;
            else if (idx == ilselected) sel_was_deleted = true;

            if (to_delete->data_p) free(to_delete->data_p);
            if (to_delete->pttbl_p) free(to_delete->pttbl_p);
            free(to_delete);
            /* idx tracks the original list position; advance it for the deleted entry */
            idx++;
        } else {
            prev = curr;
            curr = (IMG *)curr->nxt_p;
            idx++;
        }
    }

    ilselected -= deleted_before_sel;
    if (sel_was_deleted || (unsigned)ilselected >= imgcnt)
        ilselected = imgcnt ? (int)imgcnt - 1 : -1;

    g_img_tex_idx = -2;
}

/* Swap two adjacent IMG nodes in the linked list. `before_a` is the node
   whose nxt_p points at `a` (or NULL if `a` is the head); `b` must equal
   a->nxt_p. After the call, the order is ...before_a -> b -> a -> b->nxt_p. */
static void swap_adjacent_img(IMG *before_a, IMG *a, IMG *b)
{
    a->nxt_p = b->nxt_p;
    b->nxt_p = a;
    if (before_a) before_a->nxt_p = b;
    else img_p = b;
}

static void MoveImageUp(void)
{
    if (ilselected <= 0) return;
    undo_push();

    IMG *before_prev = NULL;
    IMG *prev = (IMG *)img_p;
    for (int i = 0; prev && i < ilselected - 1; i++) {
        before_prev = prev;
        prev = (IMG *)prev->nxt_p;
    }
    if (!prev || !prev->nxt_p) return;
    swap_adjacent_img(before_prev, prev, (IMG *)prev->nxt_p);
    ilselected--;
    g_img_tex_idx = -2;
}

static void MoveImageDown(void)
{
    if (ilselected < 0) return;
    undo_push();

    IMG *before_curr = NULL;
    IMG *curr = (IMG *)img_p;
    for (int i = 0; curr && i < ilselected; i++) {
        before_curr = curr;
        curr = (IMG *)curr->nxt_p;
    }
    if (!curr || !curr->nxt_p) return;
    swap_adjacent_img(before_curr, curr, (IMG *)curr->nxt_p);
    ilselected++;
    g_img_tex_idx = -2;
}

static void DeletePalette(void)
{
    if (plselected < 0 || (unsigned)plselected >= palcnt) return;
    undo_push();

    PAL *prev = NULL;
    PAL *curr = (PAL *)pal_p;
    for (int i = 0; curr && i < plselected; i++) {
        prev = curr;
        curr = (PAL *)curr->nxt_p;
    }
    if (!curr) return;

    if (prev) prev->nxt_p = curr->nxt_p;
    else pal_p = curr->nxt_p;
    palcnt--;

    if ((unsigned)plselected >= palcnt)
        plselected = palcnt ? (int)palcnt - 1 : -1;

    if (curr->data_p) free(curr->data_p);
    free(curr);

    g_img_tex_idx = -2;
}

/* Assign the currently-selected palette to the currently-selected image.
   No-ops on an invalid selection on either side. */
static void SetPaletteOfSelected(void)
{
    if (plselected < 0 || (unsigned)plselected >= palcnt) return;
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img) return;
    undo_push();
    img->palnum = (unsigned short)plselected;
    g_img_tex_idx = -2;
}

/* Assign the currently-selected palette to every marked image. */
static void SetPaletteOfMarked(void)
{
    if (plselected < 0 || (unsigned)plselected >= palcnt) return;
    bool any = false;
    for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p)
        if (p->flags & 1) { any = true; break; }
    if (!any) return;
    undo_push();
    for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p)
        if (p->flags & 1) p->palnum = (unsigned short)plselected;
    g_img_tex_idx = -2;
}

/* Toggle the selected image's point table: allocate via the asm pool when
   absent (so mem_free can release it later), free when present. No "are you
   sure" — matches DeleteImage's no-confirm behavior. */
static void TogglePointTable(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img) return;
    undo_push();
    if (img->pttbl_p) {
        free(img->pttbl_p);
        img->pttbl_p = NULL;
    } else {
        AddPointTable(ilselected);  /* cdecl-safe wrapper around img_pttbladd */
    }
}

/* Clear all "extra" anipt/pttbl data on every image. Mirrors ilst_clrxdata:
   zeros anix2/aniy2/aniz2 and the contents of any attached PTTBL (without
   freeing the PTTBL itself, so toggle state is preserved). */
static void ClearExtraData(void)
{
    if (!img_p) return;
    undo_push();
    for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p) {
        p->anix2 = p->aniy2 = p->aniz2 = 0;
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

/* Free all images, palettes, and sequence/script data.  Resets all counters
   and selections.  Ported from img_clearall — called before img_load. */
static void ClearAll(void)
{
    /* Delete all images (delete index 0 repeatedly, mirroring img_del(0) loop) */
    while (img_p) {
        IMG *cur = (IMG *)img_p;
        img_p = cur->nxt_p;
        if (cur->data_p)  free(cur->data_p);
        if (cur->pttbl_p) free(cur->pttbl_p);
        free(cur);
    }
    imgcnt = 0;
    ilselected = -1;

    /* Delete all palettes */
    while (pal_p) {
        PAL *cur = (PAL *)pal_p;
        pal_p = cur->nxt_p;
        if (cur->data_p) free(cur->data_p);
        free(cur);
    }
    palcnt = 0;
    plselected = -1;

    /* Free sequence/script memory */
    if (scrseqmem_p) {
        free(scrseqmem_p);
        scrseqmem_p = NULL;
        scrseqbytes  = 0;
    }

    seqcnt = 0;
    scrcnt = 0;
    damcnt = 0;
    ilpalloaded = -1;

    /* Reset second image list */
    if (img2_p) {
        while (img2_p) {
            IMG *cur = (IMG *)img2_p;
            img2_p = cur->nxt_p;
            if (cur->data_p)  free(cur->data_p);
            if (cur->pttbl_p) free(cur->pttbl_p);
            free(cur);
        }
    }
    img2cnt     = 0;
    il2selected = -1;
    il1stprt    = 0;
    il21stprt   = 0;

    g_img_tex_idx = -2;
    g_palette_nav = false;
    g_hue_slider  = 0;
    g_hue_last    = 0;
    g_palette_baseline_nc = 0;
}

/* Swap to the alternate (second) image list.  Purely swaps globals —
   no ASM dependencies. */
static void SwitchImageList(void)
{
    void *tmp_p = img_p;  img_p = img2_p;  img2_p = tmp_p;
    unsigned int tmp_cnt = imgcnt;  imgcnt = img2cnt;  img2cnt = tmp_cnt;
    int tmp_sel = ilselected;  ilselected = il2selected;  il2selected = tmp_sel;
    unsigned int tmp_prt = il1stprt;  il1stprt = il21stprt;  il21stprt = tmp_prt;
    g_img_tex_idx = -2;
}

/* Set the selected image's PTTBL.ID to (il2selected + 1).  If no PTTBL
   exists for the image, allocate one via the ASM memory pool. */
static void SetIDFromSecondList(void)
{
    g_dirty = true;
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img) return;

    if (!img->pttbl_p) {
        AddPointTable(ilselected);
        img = (ilselected >= 0) ? get_img(ilselected) : NULL;
        if (!img || !img->pttbl_p) return;
    }

    /* PTTBL.ID is at struct offset 14 (dw aligned, pack-2) */
    unsigned char *pttbl = (unsigned char *)img->pttbl_p;
    unsigned short new_id = (unsigned short)(il2selected + 1);
    pttbl[14] = (unsigned char)(new_id & 0xFF);
    pttbl[15] = (unsigned char)(new_id >> 8);
}

/* Duplicate the selected image: allocate a new IMG via the ASM pool,
   copy all fields (including pixel data and PTTBL), then open the
   ImGui rename dialog so the user can name the copy. */
static void DuplicateImage(void)
{
    IMG *src = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!src) return;

    undo_push();

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

    strncpy(dst->n_s, src->n_s, 15);
    dst->n_s[15] = '\0';

    /* Select the new image and open rename */
    ilselected = (int)imgcnt - 1;
    g_img_tex_idx = -2;
    OpenRenameImage();
    return;

err:
    /* Rollback: delete the newly-allocated image */
    if (dst->data_p) free(dst->data_p);
    if (dst->pttbl_p) free(dst->pttbl_p);
    /* img_alloc appended the node — unlink it */
    {
        IMG *prev = NULL;
        IMG *cur = (IMG *)img_p;
        while (cur && cur != dst) { prev = cur; cur = (IMG *)cur->nxt_p; }
        if (cur == dst) {
            if (prev) prev->nxt_p = cur->nxt_p;
            else img_p = cur->nxt_p;
            imgcnt--;
        }
    }
    free(dst);
}

/* Add a new blank IMG (32x32, 8bpp, transparent) to the current library.
 * Uses the currently selected palette if any, otherwise palette 0.
 * Useful when starting a new library from scratch — File → New gives you
 * an empty library, "Add" on palettes makes a blank palette, then this
 * gives you the first sprite to start drawing on. */
static void AddNewBlankImage(void)
{
    g_dirty = true;
    IMG *img = AllocImg();
    if (!img) return;

    img->w        = 32;
    img->h        = 32;
    img->flags    = 0;
    img->anix     = 0;
    img->aniy     = 0;
    img->anix2    = 0;
    img->aniy2    = 0;
    img->aniz2    = 0;
    img->opals    = (unsigned short)-1;
    img->pttbl_p  = NULL;
    img->palnum   = (plselected >= 0) ? (unsigned short)plselected : 0;

    unsigned int stride = ((unsigned int)img->w + 3) & ~3;
    unsigned int sz = stride * img->h;
    img->data_p = PoolAlloc(sz);
    if (img->data_p) memset(img->data_p, 0, sz);
    img->baseline_p = PoolAlloc(sz);
    if (img->baseline_p) memset(img->baseline_p, 0, sz);

    static int next_id = 1;
    snprintf(img->n_s, sizeof(img->n_s), "NEW%d", next_id++);

    if (imgcnt > 0) ilselected = (int)imgcnt - 1;
    g_img_tex_idx = -2;
}

/* Add a new blank 256-color palette.  Moved from imgtool_thunks.asm;
   uses the ASM memory pool via thunks (pal_alloc, mem_alloc). */
static void AddNewPalette(void)
{
    g_dirty = true;
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = 256;
    pal->pad     = 0;
    pal->n_s[0]  = '\0';

    unsigned char *buf = (unsigned char *)PoolAlloc(512);
    if (!buf) return;
    pal->data_p = buf;
    memset(buf, 0, 512);

    if (palcnt > 0) plselected = (int)palcnt - 1;
}

static void DuplicatePalette(void)
{
    PAL *src = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!src || !src->data_p) return;

    g_dirty = true;
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = src->flags;
    pal->bitspix = src->bitspix;
    pal->numc    = src->numc;
    pal->pad     = 0;
    memcpy(pal->n_s, src->n_s, 10);

    unsigned int col_sz = (unsigned int)pal->numc * 2;
    unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
    if (!buf) return;
    pal->data_p = buf;
    memcpy(buf, src->data_p, col_sz);

    if (palcnt > 0) plselected = (int)palcnt - 1;
}

/* Merge marked palettes into the selected palette.
   For each marked palette, each color is remapped to the closest match
   in the selected palette (Euclidean distance in 5-bit RGB space).
   All images using the marked palette are remapped and reassigned.
   Finally, the marked palettes are deleted.  Ported from plst_merge. */
static void MergeMarkedPalettes(void)
{
    PAL *sel = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!sel || !sel->data_p || sel->numc == 0) return;

    /* Count marked palettes (excluding the selected one) */
    bool any_marked = false;
    for (PAL *p = (PAL *)pal_p; p; p = (PAL *)p->nxt_p)
        if ((p->flags & 1) && p != sel) { any_marked = true; break; }
    if (!any_marked) return;

    undo_push();

    /* Clear mark on the selected palette so it survives deletion pass */
    sel->flags &= ~1;

    unsigned short dst_numc   = sel->numc;
    unsigned char *dst_colors = (unsigned char *)sel->data_p;

    /* Phase 1: build remap and remap images for each marked palette */
    PAL *pal = (PAL *)pal_p;
    while (pal) {
        if (!(pal->flags & 1) || pal == sel || !pal->data_p || pal->numc == 0) {
            pal = (PAL *)pal->nxt_p;
            continue;
        }

        unsigned short  src_numc   = pal->numc;
        unsigned char  *src_colors = (unsigned char *)pal->data_p;

        /* Build remap: for each source color find closest destination */
        unsigned char remap[256] = {0};
        for (int si = 0; si < src_numc; si++) {
            unsigned short sw = (unsigned short)(src_colors[si * 2] |
                                                 (src_colors[si * 2 + 1] << 8));
            int sr5 = (sw >> 10) & 0x1F;
            int sg5 = (sw >>  5) & 0x1F;
            int sb5 =  sw        & 0x1F;

            int best_dist = 0x7FFFFFFF;
            int best_idx  = 0;
            for (int di = 0; di < dst_numc; di++) {
                unsigned short dw = (unsigned short)(dst_colors[di * 2] |
                                                     (dst_colors[di * 2 + 1] << 8));
                int dr5 = (dw >> 10) & 0x1F;
                int dg5 = (dw >>  5) & 0x1F;
                int db5 =  dw        & 0x1F;
                int dr = dr5 - sr5, dg = dg5 - sg5, db = db5 - sb5;
                int dist = dr * dr + dg * dg + db * db;
                if (dist < best_dist) { best_dist = dist; best_idx = di; }
            }
            remap[si] = (unsigned char)best_idx;
        }

        /* Find this palette's index in the linked list */
        int pal_idx = 0;
        for (PAL *q = (PAL *)pal_p; q && q != pal; q = (PAL *)q->nxt_p) pal_idx++;

        /* Remap all images that use this palette */
        for (IMG *img = (IMG *)img_p; img; img = (IMG *)img->nxt_p) {
            if (img->palnum != pal_idx) continue;
            img->palnum = (unsigned short)plselected;

            if (!img->data_p || img->w == 0 || img->h == 0) continue;
            unsigned short stride = (img->w + 3) & ~3;
            int total = (int)stride * img->h;
            unsigned char *pixels = (unsigned char *)img->data_p;
            for (int i = 0; i < total; i++)
                if (pixels[i] != 0) pixels[i] = remap[pixels[i]];
        }

        pal = (PAL *)pal->nxt_p;
    }

    /* Phase 2: delete marked palettes and fix up image palette indices */
    PAL *prev = NULL;
    PAL *cur  = (PAL *)pal_p;
    int del_idx = 0;

    while (cur) {
        if (cur->flags & 1) {
            PAL *to_del = cur;
            /* Unlink */
            if (prev) prev->nxt_p = cur->nxt_p;
            else pal_p = cur->nxt_p;
            cur = (PAL *)cur->nxt_p;
            palcnt--;

            /* Any image pointing to palette index > del_idx shifts down */
            for (IMG *img = (IMG *)img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > del_idx) img->palnum--;
            }
            /* plselected also shifts if the deleted palette was before it */
            if ((int)plselected > del_idx) plselected--;

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
    if ((unsigned)plselected >= palcnt)
        plselected = palcnt ? (int)palcnt - 1 : -1;

    g_img_tex_idx = -2;
}

static void OpenRenameImage(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img) return;
    g_rename_target = RenameTarget::Image;
    g_rename_idx = ilselected;
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
    for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p) {
        if (p->flags & 1) { first = p; break; }
    }
    if (!first) return;
    g_rename_target = RenameTarget::MarkedImages;
    g_rename_idx = -1;
    strncpy(g_rename_buf, first->n_s, 12);
    g_rename_buf[12] = '\0';
    g_show_rename = true;
}

static void ApplyMarkedImageRename(const char *base)
{
    if (!base || !*base) return;
    undo_push();
    bool prepend = (base[0] == '+');
    const char *core = prepend ? base + 1 : base;
    int n = 0;
    for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p) {
        if (!(p->flags & 1)) continue;
        if (prepend) {
            char old[16];
            strncpy(old, p->n_s, 15); old[15] = '\0';
            snprintf(p->n_s, sizeof(p->n_s), "%s%s", core, old);
        } else {
            n++;
            snprintf(p->n_s, sizeof(p->n_s), "%s%d", core, n);
        }
        p->n_s[15] = '\0';
    }
}

static void CalculatePaletteHistogram()
{
    memset(g_histogram_data, 0, sizeof(g_histogram_data));
    g_histogram_max = 0.0f;
    g_histogram_img_count = 0;

    if (plselected < 0) return;

    IMG *img = (IMG *)img_p;
    while (img) {
        if (img->palnum == plselected && img->data_p && img->w > 0 && img->h > 0) {
            g_histogram_img_count++;
            unsigned short stride = (img->w + 3) & ~3; // DMA hardware alignment
            unsigned char *pixels = (unsigned char *)img->data_p;
            int total_pixels = stride * img->h;
            for (int i = 0; i < total_pixels; i++) {
                g_histogram_data[pixels[i]] += 1.0f;
            }
        }
        img = (IMG *)img->nxt_p;
    }

    // Find max (skipping index 0, matching original ASM behavior so transparent bg doesn't dwarf the chart)
    for (int i = 1; i < 256; i++) {
        if (g_histogram_data[i] > g_histogram_max) {
            g_histogram_max = g_histogram_data[i];
        }
    }
    if (g_histogram_max == 0.0f) g_histogram_max = 1.0f;
}

static void DeleteUnusedPaletteColors()
{
    if (plselected < 0) return;
    PAL *pal = get_pal(plselected);
    if (!pal || !pal->data_p) return;

    bool used[256] = {false};
    used[0] = true; /* Transparent index 0 is always preserved */

    /* 1. Find used colors across all images sharing this palette */
    IMG *img = (IMG *)img_p;
    while (img) {
        if (img->palnum == plselected && img->data_p && img->w > 0 && img->h > 0) {
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            int total_pixels = stride * img->h;
            for (int i = 0; i < total_pixels; i++) {
                used[pixels[i]] = true;
            }
        }
        img = (IMG *)img->nxt_p;
    }

    /* 2. Build remap table */
    unsigned char remap[256] = {0};
    unsigned short next_avail = 1;
    for (int i = 1; i < 256; i++) {
        if (used[i]) {
            remap[i] = (unsigned char)next_avail++;
        }
    }

    /* If no optimization is possible, early out */
    if (next_avail == 256 || next_avail == pal->numc) return;

    /* 3. Pack palette colors in-place (2 bytes per color, 15-bit packed RGB) */
    unsigned char *colors = (unsigned char *)pal->data_p;
    int max_colors = pal->numc; /* honor allocated buffer size */
    for (int i = 1; i < max_colors; i++) {
        if (used[i]) {
            int new_idx = remap[i];
            if (new_idx != i) {
                colors[new_idx * 2 + 0] = colors[i * 2 + 0];
                colors[new_idx * 2 + 1] = colors[i * 2 + 1];
            }
        }
    }

    /* Clear remaining colors to black */
    for (int i = next_avail; i < max_colors; i++) {
        colors[i * 2 + 0] = 0;
        colors[i * 2 + 1] = 0;
    }

    pal->numc = next_avail;

    /* 4. Remap pixel indices in all affected images */
    img = (IMG *)img_p;
    while (img) {
        if (img->palnum == plselected && img->data_p && img->w > 0 && img->h > 0) {
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            int total_pixels = stride * img->h;
            for (int i = 0; i < total_pixels; i++) {
                pixels[i] = remap[pixels[i]];
            }
        }
        img = (IMG *)img->nxt_p;
    }

    /* 5. Update global SDL palette so the UI reflects changes instantly */
    ApplyPalette(plselected);

    /* 6. Force canvas texture rebuild */
    g_img_tex_idx = -2;
}

/* ---- Strip Edge (DMA Compression Prep) ---- */
static void StripMarkedImages(int max_transparent_neighbors, int specific_color = -1)
{
    g_dirty = true;
    IMG *img = (IMG *)img_p;
    while (img) {
        if ((img->flags & 1) && img->data_p && img->w > 0 && img->h > 0) {
            int w = img->w;
            int h = img->h;
            int stride = (w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            
            unsigned char *flags = (unsigned char *)calloc(1, stride * h);
            if (!flags) {
                img = (IMG *)img->nxt_p;
                continue;
            }

            /* Bounds checking counts out-of-bounds as transparent (matches ASM logic) */
            auto is_transparent = [&](int x, int y) -> bool {
                if (x < 0 || x >= w || y < 0 || y >= h) return true;
                return pixels[y * stride + x] == 0;
            };

            /* Pass 1: Flag edge pixels */
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char c = pixels[y * stride + x];
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
                        flags[y * stride + x] = 1;
                    }
                }
            }

            /* Pass 1: Delete flagged pixels */
            for (int i = 0; i < stride * h; i++) if (flags[i]) pixels[i] = 0;
            memset(flags, 0, stride * h);

            /* Pass 2: Flag lonely pixels (stray dust specs) */
            for (int y = 0; y < h; y++) {
                for (int x = 0; x < w; x++) {
                    unsigned char c = pixels[y * stride + x];
                    if (c == 0) continue;
                    if (specific_color >= 0 && c != specific_color) continue;

                    int trans_count = 0;
                    for (int dy = -1; dy <= 1; dy++) {
                        for (int dx = -1; dx <= 1; dx++) {
                            if (dx == 0 && dy == 0) continue;
                            if (is_transparent(x + dx, y + dy)) trans_count++;
                        }
                    }
                    if (trans_count == 8) flags[y * stride + x] = 1;
                }
            }

            /* Pass 2: Delete flagged lonely pixels */
            for (int i = 0; i < stride * h; i++) if (flags[i]) pixels[i] = 0;
            free(flags);
        }
        img = (IMG *)img->nxt_p;
    }
    g_img_tex_idx = -2; /* Force texture rebuild */
}

/* ---- Dither Replace ---- */
static void DitherReplaceMarkedImages(int specific_color)
{
    g_dirty = true;
    IMG *img = (IMG *)img_p;
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
    g_dirty = true;
    IMG *img = (IMG *)img_p;
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
enum class FileDialogMode { OpenImg, AppendImg, OpenLod, SaveImg, ExportTga, LoadLbm, SaveLbm, SaveMarkedLbm, LoadTga, SaveTga, ImportPng, ImportPngMatch, ExportPng, WriteAniLst, WriteTbl, WriteIrw };
static bool g_show_file_dialog = false;
static FileDialogMode g_file_dialog_mode = FileDialogMode::OpenImg;
static char g_file_dialog_dir[1024] = "";
static char g_file_dialog_file[256] = "";
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

static const char *get_dialog_config_path(void)
{
    static char path[MAX_PATH] = "";
    if (!path[0]) {
#ifdef _WIN32
        char appdata[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata))) {
            _snprintf(path, sizeof(path), "%s\\imgtool\\last_dir.txt", appdata);
        } else {
            _snprintf(path, sizeof(path), "last_dir.txt");
        }
#else
        _snprintf(path, sizeof(path), "%s/.imgtool_last_dir",
            getenv("HOME") ? getenv("HOME") : ".");
#endif
    }
    return path;
}

static void save_last_dir(const char *dir)
{
    if (!dir || !*dir) return;
#ifdef _WIN32
    char parent[MAX_PATH];
    _snprintf(parent, sizeof(parent), "%s\\imgtool",
        getenv("APPDATA") ? getenv("APPDATA") : ".");
    CreateDirectoryA(parent, NULL);
#endif
    FILE *f = fopen(get_dialog_config_path(), "w");
    if (f) { fprintf(f, "%s", dir); fclose(f); }
}

static void load_last_dir(char *dir, size_t dirsz)
{
    if (!dir || !dirsz) return;
    dir[0] = '\0';
    FILE *f = fopen(get_dialog_config_path(), "r");
    if (f) {
        if (fgets(dir, (int)dirsz, f)) {
            size_t len = strlen(dir);
            if (len > 0 && dir[len - 1] == '\n') dir[len - 1] = '\0';
        }
        fclose(f);
    }
}

struct FileEntry { std::string name; bool is_dir; };

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
            entries.push_back({fd.cFileName, is_dir});
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
                if (!dup) entries.push_back({fd.cFileName, true});
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
            bool is_dir = false;
            if (dir_ent->d_type == DT_DIR) {
                is_dir = true;
            } else if (dir_ent->d_type == DT_UNKNOWN) {
                struct stat st;
                std::string full_path = dir;
                if (!full_path.empty() && full_path.back() != '/') full_path += "/";
                full_path += dir_ent->d_name;
                if (stat(full_path.c_str(), &st) == 0 && S_ISDIR(st.st_mode))
                    is_dir = true;
            }
            if (is_dir) {
                entries.push_back({dir_ent->d_name, true});
            } else if (ext_filter && ext_filter[0]) {
                const char* dot = strrchr(dir_ent->d_name, '.');
                if (dot) {
#ifdef _WIN32
                    if (_stricmp(dot + 1, ext_filter) == 0)
                        entries.push_back({dir_ent->d_name, false});
#else
                    if (strcasecmp(dot + 1, ext_filter) == 0)
                        entries.push_back({dir_ent->d_name, false});
#endif
                }
            } else {
                entries.push_back({dir_ent->d_name, false});
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

/* Load an IMG by absolute path. Mirrors the Open-button branch of the file
   dialog: split into dir+file, populate the asm-side fpath_s/fname_s globals,
   chdir, then img_clearall + img_load. Used by both the dialog and the
   Recent Files menu. */
static void OpenImgFile(const std::string &full_path)
{
    size_t sep = full_path.find_last_of("\\/");
    std::string dir  = (sep == std::string::npos) ? std::string(".") : full_path.substr(0, sep);
    std::string file = (sep == std::string::npos) ? full_path        : full_path.substr(sep + 1);

    size_t n_dir = dir.size();
    if (n_dir > 63) n_dir = 63;
    memset(fpath_s, 0, 64);
    memcpy(fpath_s, dir.data(), n_dir);

    size_t n_file = file.size();
    if (n_file > 12) n_file = 12;
    memset(fname_s, 0, 13);
    memset(fnametmp_s, 0, 13);
    memcpy(fname_s, file.data(), n_file);
    memcpy(fnametmp_s, file.data(), n_file);
    for (size_t i = 0; i < n_file; i++) {
        fname_s[i] = (char)toupper((unsigned char)fname_s[i]);
        fnametmp_s[i] = (char)toupper((unsigned char)fnametmp_s[i]);
    }
    _chdir(dir.c_str());

    g_undo_count = 0;
    g_undo_idx   = 0;
    if (g_clipboard.valid && g_clipboard.data_p) {
        free(g_clipboard.data_p);
        g_clipboard.data_p = NULL;
        g_clipboard.valid = false;
    }
    g_grid_sel.active = false;
    g_grid_sel.dragging = false;
    g_marquee_active = false;
    g_pasted.active = false;
    g_pasted.dragging = false;

    ClearAll();
    LoadImgFile();
    g_dirty = false; /* fresh load = clean baseline */
    g_img_tex_idx = -2;
    RecentAdd(full_path);
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
        case FileDialogMode::WriteAniLst: return "ASM";
        case FileDialogMode::WriteTbl:  return "TBL";
    }
    return "";
}

static void OpenFileDialog(FileDialogMode mode) {
    if (fpath_s[0] != '\0') {
        size_t n = 0;
        while (n < 63 && fpath_s[n] != '\0') n++;
        memcpy(g_file_dialog_dir, fpath_s, n);
        g_file_dialog_dir[n] = '\0';
    } else if (g_file_dialog_dir[0] == '\0') {
        load_last_dir(g_file_dialog_dir, sizeof(g_file_dialog_dir));
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
    bool is_export = (mode == FileDialogMode::ExportTga || mode == FileDialogMode::SaveTga || mode == FileDialogMode::ExportPng || mode == FileDialogMode::SaveLbm);
    if (is_export && ilselected >= 0) {
        IMG *img = get_img(ilselected);
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
    } else if (fname_s[0] != '\0') {
        size_t n = 0;
        while (n < 12 && fname_s[n] != '\0') n++;
        memcpy(g_file_dialog_file, fname_s, n);
        g_file_dialog_file[n] = '\0';
    } else {
        g_file_dialog_file[0] = '\0';
    }
    g_show_file_dialog = true;
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
    else if (g_file_dialog_mode == FileDialogMode::ExportPng) title = "Export PNG File";
    else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) title = "Write ANILST";
    else if (g_file_dialog_mode == FileDialogMode::WriteTbl) title = "Write TBL";
    else if (g_file_dialog_mode == FileDialogMode::WriteIrw) title = "Write IRW";

    if (g_show_file_dialog) ImGui::OpenPopup(title);
    
    ImGui::SetNextWindowSize(ImVec2(800, 520), ImGuiCond_Once);
    if (ImGui::BeginPopupModal(title, &g_show_file_dialog, ImGuiWindowFlags_NoSavedSettings)) {
        
        ImGui::InputText("Directory", g_file_dialog_dir, sizeof(g_file_dialog_dir));
        ImGui::Separator();
        
        ImGui::BeginChild("##file_list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 4), true);
        
        std::string current_dir = g_file_dialog_dir;
        std::string parent_dir = GetParentDirectory(current_dir);
        
        if (current_dir != parent_dir) {
            if (ImGui::Selectable("[..] (Up one level)", false, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (ImGui::IsMouseDoubleClicked(0)) {
                    snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", parent_dir.c_str());
                }
            }
        }
        
        std::vector<FileEntry> entries;
        GetDirectoryFiles(current_dir, entries, GetDialogExtension(g_file_dialog_mode));
        
        std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
        
        for (const auto& entry : entries) {
            std::string label = (entry.is_dir ? "[Dir] " : "      ") + entry.name;
            bool selected = (entry.name == g_file_dialog_file);
            if (ImGui::Selectable(label.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                if (entry.is_dir) {
                    if (ImGui::IsMouseDoubleClicked(0)) {
                        std::string new_dir = PathCombine(current_dir, entry.name);
                        snprintf(g_file_dialog_dir, sizeof(g_file_dialog_dir), "%s", new_dir.c_str());
                        g_file_dialog_file[0] = '\0';
                    }
                } else {
                    snprintf(g_file_dialog_file, sizeof(g_file_dialog_file), "%s", entry.name.c_str());
                }
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

        if (g_file_dialog_mode == FileDialogMode::OpenLod) {
            ImGui::InputText("Force Override Directory (/O)", g_lod_override_dir, sizeof(g_lod_override_dir));
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("If set, forces all IMGs to load from this directory, ignoring paths in the .lod file.");
        }

        ImGui::InputText("File Name", g_file_dialog_file, sizeof(g_file_dialog_file));
        ImGui::SameLine();
        
        const char* btn_text = (g_file_dialog_mode == FileDialogMode::ImportPng ||
                                g_file_dialog_mode == FileDialogMode::ExportPng) ? "OK" :
                               (g_file_dialog_mode == FileDialogMode::OpenImg ||
                                g_file_dialog_mode == FileDialogMode::AppendImg ||
                                g_file_dialog_mode == FileDialogMode::OpenLod ||
                                g_file_dialog_mode == FileDialogMode::LoadLbm ||
                                g_file_dialog_mode == FileDialogMode::LoadTga) ? "Open" : "Save";
        if (ImGui::Button(btn_text, ImVec2(100, 0))) {
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
            } else if (g_file_dialog_mode == FileDialogMode::ImportPng) {
                ImportPng(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".ASM";
                WriteAnilstFromMarked(full_path.c_str());
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
                IMG *p = (IMG *)img_p;
                int original_selection = ilselected;
                int i = 0;
                while (p) {
                    if ((p->flags & 1) && p->w > 0 && p->h > 0) {
                        ilselected = i;
                        memset(fnametmp_s, 0, 13);
                        snprintf(fnametmp_s, 13, "%.8s.LBM", p->n_s);
                        SaveLbm();
                    }
                    p = (IMG *)p->nxt_p;
                    i++;
                }
                ilselected = original_selection;
            } else if (g_file_dialog_mode == FileDialogMode::OpenLod) {
                LodManifest manifest = ParseLodFile(full_path.c_str());
                verbose_log("OpenLod: %s -> %zu entries, PPP=%d", full_path.c_str(), manifest.entries.size(), manifest.ppp_value);
                if (manifest.parse_error) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg), "LOD: %s", manifest.error_msg.c_str());
                    g_restore_msg_timer = 6.0f;
                } else {
                    if (manifest.ppp_value > 0)
                        g_load2_ppp = manifest.ppp_value;

                    g_undo_count = 0;
                    g_undo_idx   = 0;
                    if (g_clipboard.valid && g_clipboard.data_p) {
                        free(g_clipboard.data_p);
                        g_clipboard.data_p = NULL;
                        g_clipboard.valid = false;
                    }
                    g_grid_sel.active = false;
                    g_grid_sel.dragging = false;
                    g_marquee_active = false;
                    g_pasted.active = false;
                    g_pasted.dragging = false;
                    ClearAll();

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
                            if (nd > 63) nd = 63;
                            memset(fpath_s, 0, 64);
                            memcpy(fpath_s, d.c_str(), nd);

                            memset(fname_s, 0, 13);
                            memcpy(fname_s, file.c_str(), n_file);
                            for (size_t j = 0; j < n_file; j++)
                                fname_s[j] = (char)toupper((unsigned char)fname_s[j]);

                            unsigned int prev = imgcnt;
                            _chdir(d.c_str());
                            LoadImgFile();
                            return imgcnt > prev;
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

                    ilselected = imgcnt > 0 ? 0 : -1;
                    g_dirty = false;
                    RecentAdd(full_path);

                    int total = (int)manifest.entries.size();
                    if (loaded == 0)
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "LOD: 0/%d IMG(s) loaded. Check IMGDIR or file paths.", total);
                    else if (loaded < total)
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "LOD: %d/%d IMG(s) loaded%s", loaded, total,
                            manifest.ppp_value > 0 ? " (PPP set)" : "");
                    else
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                            "Loaded %d IMG(s) from LOD%s", loaded,
                            manifest.ppp_value > 0 ? " (PPP set)" : "");
                    g_restore_msg_timer = 4.0f;
                }
            } else {
                size_t n_dir = strlen(g_file_dialog_dir);
                if (n_dir > 63) n_dir = 63;
                memset(fpath_s, 0, 64);
                memcpy(fpath_s, g_file_dialog_dir, n_dir);
                
                size_t n_file = strlen(g_file_dialog_file);
                if (n_file > 12) n_file = 12;
                memset(fname_s, 0, 13);
                memset(fnametmp_s, 0, 13);
                memcpy(fname_s, g_file_dialog_file, n_file);
                memcpy(fnametmp_s, g_file_dialog_file, n_file);
                for (size_t i = 0; i < n_file; i++) {
                    fname_s[i] = (char)toupper((unsigned char)fname_s[i]);
                    fnametmp_s[i] = (char)toupper((unsigned char)fnametmp_s[i]);
                }
                _chdir(g_file_dialog_dir);
                
                if (g_file_dialog_mode == FileDialogMode::SaveImg) {
                    SaveImgFile();
                    g_dirty = false; /* Mark as saved in C++ state */
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::OpenImg) {
                    g_undo_count = 0;
                    g_undo_idx   = 0;
                    if (g_clipboard.valid && g_clipboard.data_p) {
                        free(g_clipboard.data_p);
                        g_clipboard.data_p = NULL;
                        g_clipboard.valid = false;
                    }
                    g_grid_sel.active = false;
                    g_grid_sel.dragging = false;
                    g_marquee_active = false;
                    g_pasted.active = false;
                    g_pasted.dragging = false;
                    ClearAll();
                    LoadImgFile();
    g_dirty = false; /* fresh load = clean baseline */
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::AppendImg) {
                    /* Append modifies the in-memory set on top of whatever
                       was there — leave the dirty bookkeeping alone so the
                       user is prompted before throwing it away. */
                    LoadImgFile();
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::LoadLbm) {
                    LoadLbm();
                } else if (g_file_dialog_mode == FileDialogMode::LoadTga) {
                    LoadTga();
                } else if (g_file_dialog_mode == FileDialogMode::SaveLbm) {
                    SaveLbm();
                } else if (g_file_dialog_mode == FileDialogMode::SaveTga) {
                    SaveTga();
                }
            }
            g_img_tex_idx = -2; /* Force canvas texture refresh */
            save_last_dir(g_file_dialog_dir);
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }
        
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) {
            g_show_file_dialog = false;
            ImGui::CloseCurrentPopup();
        }
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

Step 5 -- Zoom: d doubles size, D halves size. F11/F12 fine zoom.
  Mouse wheel also zooms. Middle-mouse drag pans.

Step 6 -- Palettes: ' / scroll through palettes.
  ] assigns current palette to current sprite
  [ assigns palette to all marked sprites
  t toggles "true palette" display

Step 7 -- Two IMGs at once (Tab): Swap between list 1 and list 2.
  Load a second IMG after hitting Tab, swap with Tab.
  i grabs ID from list 2's selected sprite for list 1's current sprite.

Step 8 -- Save: Ctrl+S saves. Pre-2.x IMGs are auto-converted on load.


================================================================================
KEYBOARD REFERENCE
------------------

File Operations:
  Ctrl+O / Ctrl+S      Open / Save IMG
  Alt+L  / Alt+S       Load / Save LBM
  Ctrl+L / Ctrl+S      Load / Save TGA
  Ctrl+B               Build TGA from marked images

Image Operations:
  Space                Mark / Unmark current image
  M / m                Mark all / Clear all marks
  Ctrl+D               Delete image
  Ctrl+R               Rename image
  ;                    Least-squares size reduce
  Arrow Up/Dn          Move in image list
  PgUp/Dn              Page up/down
  Alt+PgUp/PgDn        Move image in list
  Tab                  Swap image lists
  i                    Set ID from 2nd list

Palette Operations:
  ' /                  Move up/down in palette list
  " / ?                Page up/down palette
  ]                    Set palette for current image
  [                    Set palette for marked images
  *                    Merge marked palettes
  Shift+R              Rename palette

View Controls:
  h                    Show this help
  f                    Redraw screen
  d / D                Double / Halve view size
  F11 / F12            Fine zoom out / in
  t                    Toggle true palette colors
  T                    Toggle animation points
  F9                   Debug info popup

Animation Points:
  Alt+U/D/L/R          Move primary anim point
  Ctrl+U/D/L/R         Move secondary anim point
  Ctrl+Del             Clear 2nd anim XYZ
  Ctrl+Y               Clear 2nd anim Y
  Ctrl+Z               Clear 2nd anim Z

Canvas Tools (toolbar):
  Pencil (P)           Draw with current color
  Eyedropper (E)       Pick color from canvas
  Fill (F)             Flood fill with current color
  Marquee               Select pixels for copy/cut/paste
  Undo (Ctrl+Z)        Undo pixel/animation changes
  Redo (Ctrl+Y)        Redo

  Ctrl+C / Ctrl+X / Ctrl+V    Copy / Cut / Paste
  Shift                 Quarter scroll sensitivity


================================================================================
FILE FORMATS
------------

IMG (Image Library) -- Primary format. Binary, little-endian, packed structs.
  LIB_HDR (28 bytes): imgcnt, palcnt, version (0x634+)
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
    g_dirty = true;
    PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (color_idx < 0 || color_idx >= (int)pal->numc) return;

    SDL_Color &c = g_palette[color_idx];
    rgb8_to_pal_word(c.r, c.g, c.b, (unsigned char *)pal->data_p + color_idx * 2);
}

static void hue_shift_palette(int delta_deg)
{
    PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!pal || !pal->data_p || delta_deg == 0) return;
    g_dirty = true;

    float dh = (float)delta_deg / 360.0f;
    int n = (int)pal->numc;
    if (n > 256) n = 256;

    for (int i = 0; i < n; i++) {
        float r = (float)g_palette[i].r / 255.0f;
        float g = (float)g_palette[i].g / 255.0f;
        float b = (float)g_palette[i].b / 255.0f;

        float mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
        float mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
        float l = (mx + mn) * 0.5f;

        float h = 0.0f, s = 0.0f;
        if (mx != mn) {
            float d = mx - mn;
            s = l > 0.5f ? d / (2.0f - mx - mn) : d / (mx + mn);
            if (r == mx)      h = (g - b) / d + (g < b ? 6.0f : 0.0f);
            else if (g == mx) h = (b - r) / d + 2.0f;
            else              h = (r - g) / d + 4.0f;
            h /= 6.0f;
        }

        h += dh;
        if (h < 0.0f) h += 1.0f;
        if (h >= 1.0f) h -= 1.0f;

        auto hue2rgb = [](float p, float q, float t) -> float {
            if (t < 0.0f) t += 1.0f;
            if (t > 1.0f) t -= 1.0f;
            if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
            if (t < 0.5f) return q;
            if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
            return p;
        };

        if (s < 0.0001f) {
            r = g = b = l;
        } else {
            float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s;
            float p = 2.0f * l - q;
            r = hue2rgb(p, q, h + 1.0f / 3.0f);
            g = hue2rgb(p, q, h);
            b = hue2rgb(p, q, h - 1.0f / 3.0f);
        }

        int ri = (int)(r * 255.0f + 0.5f); if (ri < 0) ri = 0; if (ri > 255) ri = 255;
        int gi = (int)(g * 255.0f + 0.5f); if (gi < 0) gi = 0; if (gi > 255) gi = 255;
        int bi = (int)(b * 255.0f + 0.5f); if (bi < 0) bi = 0; if (bi > 255) bi = 255;

        g_palette[i].r = (unsigned char)ri;
        g_palette[i].g = (unsigned char)gi;
        g_palette[i].b = (unsigned char)bi;
        rgb8_to_pal_word((unsigned char)ri, (unsigned char)gi, (unsigned char)bi,
                         (unsigned char *)pal->data_p + i * 2);
    }
}

static void save_palette_baseline(void)
{
    PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!pal || !pal->data_p) { g_palette_baseline_nc = 0; return; }
    int nc = (int)pal->numc;
    if (nc > 256) nc = 256;
    memcpy(g_palette_baseline, pal->data_p, nc * 2);
    g_palette_baseline_nc = nc;
}

static void reset_palette_to_baseline(void)
{
    PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!pal || !pal->data_p || g_palette_baseline_nc == 0) return;
    int nc = (int)pal->numc;
    if (nc > 256) nc = 256;
    if (nc > g_palette_baseline_nc) nc = g_palette_baseline_nc;
    memcpy(pal->data_p, g_palette_baseline, nc * 2);
    ApplyPalette(plselected);
    g_hue_slider = 0;
    g_hue_last   = 0;
    g_dirty = true;
}

/* Load the selected palette into g_palette[] so the canvas/swatches reflect it. */
static void ApplyPalette(int pal_idx)
{
    if (pal_idx < 0) return;
    PAL *pal = get_pal(pal_idx);
    if (!pal || !pal->data_p) return;
    const unsigned char *src = (const unsigned char *)pal->data_p;
    int n = pal->numc;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        pal_word_to_rgb8(src + i * 2, &g_palette[i].r, &g_palette[i].g, &g_palette[i].b);
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
    SDL_UnlockTexture(g_img_texture);
}

/* ---- Undo helpers ---- */
void undo_push(void)
{
    g_dirty = true;
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img) return;

    if (g_undo_idx >= 0) {
        EditSnapshot *last = &g_undo[g_undo_idx];
        if (last->image_idx == ilselected &&
            last->anix == img->anix && last->aniy == img->aniy &&
            last->anix2 == img->anix2 && last->aniy2 == img->aniy2 &&
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
    s->image_idx = ilselected;
    s->anix  = img->anix;  s->aniy  = img->aniy;
    s->anix2 = img->anix2; s->aniy2 = img->aniy2;
    s->w = img->w; s->h = img->h;
    s->palnum = img->palnum; s->flags = img->flags;
    s->hitbox_x = g_hitbox_x; s->hitbox_y = g_hitbox_y;
    s->hitbox_w = g_hitbox_w; s->hitbox_h = g_hitbox_h;
    g_undo_count = g_undo_idx + 1;
}

static void undo_apply(int idx)
{
    if (idx < 0 || idx >= g_undo_count) return;
    EditSnapshot *s = &g_undo[idx];
    IMG *img = get_img(s->image_idx);
    if (!img) return;
    img->anix  = s->anix;  img->aniy  = s->aniy;
    img->anix2 = s->anix2; img->aniy2 = s->aniy2;
    img->w = s->w; img->h = s->h;
    img->palnum = s->palnum; img->flags = s->flags;
    g_hitbox_x = s->hitbox_x; g_hitbox_y = s->hitbox_y;
    g_hitbox_w = s->hitbox_w; g_hitbox_h = s->hitbox_h;
}

/* ---- Copy/Paste helpers (pixel data only) ---- */
static void copy_image(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    /* Free previous clipboard if any */
    if (g_clipboard.valid && g_clipboard.data_p) {
        free(g_clipboard.data_p);
        g_clipboard.data_p = NULL;
    }

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
    unsigned short stride = (img->w + 3) & ~3;
    unsigned short clip_stride = (w + 3) & ~3;
    unsigned int size = clip_stride * h;

    /* Copy selected pixel data */
    g_clipboard.data_p = malloc(size);
    if (!g_clipboard.data_p) return;

    for (int y = 0; y < h; y++) {
        unsigned char *src = (unsigned char *)img->data_p + (y1 + y) * stride + x1;
        unsigned char *dst = (unsigned char *)g_clipboard.data_p + y * clip_stride;
        memcpy(dst, src, w);
    }

    g_clipboard.w = w;
    g_clipboard.h = h;
    g_clipboard.stride = clip_stride;
    g_clipboard.valid = true;
}

static void apply_pasted_region(void)
{
    g_dirty = true;
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    unsigned short stride = (img->w + 3) & ~3;
    unsigned short clip_stride = g_clipboard.stride;
    int px = g_pasted.paste_x, py = g_pasted.paste_y;
    int pw = g_clipboard.w, ph = g_clipboard.h;

    /* Clamp to image bounds */
    if (px < 0 || py < 0 || px + pw > (int)img->w || py + ph > (int)img->h) return;

    /* Copy clipboard data to target location */
    for (int y = 0; y < ph; y++) {
        unsigned char *src = (unsigned char *)g_clipboard.data_p + y * clip_stride;
        unsigned char *dst = (unsigned char *)img->data_p + (py + y) * stride + px;
        memcpy(dst, src, pw);
    }
    g_img_tex_idx = -2;
}

static void paste_image(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !g_clipboard.valid || !g_clipboard.data_p) return;

    undo_push();

    /* Show paste boundary and let user position it */
    g_pasted.active = true;
    g_pasted.paste_x = 0;
    g_pasted.paste_y = 0;
    g_pasted.dragging = false;

    /* Clear grid selection since paste is now active */
    g_grid_sel.active = false;
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
        cfg.GlyphOffset = ImVec2(0, 3.0f);
        ImFont *icons = io.Fonts->AddFontFromFileTTF(fontpath, 20.0f, &cfg, icon_ranges);
        if (icons) g_icon_font_loaded = true;
    }

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);
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

int imgui_overlay_check_unsaved_and_quit(void)
{
    if (g_dirty && imgcnt > 0) {
        g_show_unsaved_confirm = true;
        return 0;
    }
    return 1;
}

void imgui_overlay_request_quit(void)
{
    g_pending_quit = true;
}

int imgui_overlay_should_quit(void)
{
    /* If we're pending quit and no unsaved popup is showing, it's safe to exit */
    return (g_pending_quit && !g_show_unsaved_confirm) ? 1 : 0;
}

void imgui_overlay_mark_saved(void)
{
    g_dirty = false;
}

/* =========================================================
   Main render function — called each frame
   ========================================================= */
void imgui_overlay_render(void)
{
    ImGuiIO &io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    /* ---- Global keyboard shortcuts ---- */
    ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;

    /* Undo / Redo */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route)) {
        if (g_pixel_undo && g_pixel_undo_img == ilselected) {
            IMG *img = get_img(ilselected);
            if (img && img->data_p) {
                unsigned short s = (img->w + 3) & ~3;
                unsigned int sz = (unsigned int)s * img->h;
                unsigned char *tmp = (unsigned char *)malloc(sz);
                if (tmp) { memcpy(tmp, img->data_p, sz); memcpy(img->data_p, g_pixel_undo, sz);
                           memcpy(g_pixel_undo, tmp, sz); free(tmp); }
    g_img_tex_idx = -2;
    g_palette_nav = false;
}
        } else if (g_undo_idx > 0) { g_undo_idx--; undo_apply(g_undo_idx); }
    }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route)) { if (g_undo_idx < g_undo_count - 1) { g_undo_idx++; undo_apply(g_undo_idx); } }

    /* Clipboard */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, route)) copy_image();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, route)) { copy_image(); DeleteImage(ilselected); }
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, route)) paste_image();

    /* File I/O */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) OpenFileDialog(FileDialogMode::OpenImg);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) OpenFileDialog(FileDialogMode::SaveImg);
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_L, route)) OpenFileDialog(FileDialogMode::LoadLbm);
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_S, route)) OpenFileDialog(FileDialogMode::SaveLbm);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L, route)) OpenFileDialog(FileDialogMode::LoadTga);

    /* View / Debug */
    if (ImGui::Shortcut(ImGuiKey_H,  route)) g_show_help = true;
    if (ImGui::Shortcut(ImGuiKey_F9, route)) g_show_debug = !g_show_debug;
    if (ImGui::Shortcut(ImGuiKey_R,  route)) {
        g_marquee_active = !g_marquee_active;
        if (!g_marquee_active) g_grid_sel.active = false;
    }

    /* Tool Intercepts */
    if (ImGui::Shortcut(ImGuiKey_Escape, route)) {
        if (g_pasted.active) { g_pasted.active = false; g_pasted.dragging = false; } 
        else if (g_grid_sel.active) { g_grid_sel.active = false; }
    }
    if (ImGui::Shortcut(ImGuiKey_Enter, route)) {
        if (g_pasted.active && !g_pasted.dragging) { apply_pasted_region(); g_pasted.active = false; }
    }

    /* Image Operations */
    if (ImGui::Shortcut(ImGuiKey_Space, route)) {
        IMG *img = get_img(ilselected); if (img) { img->flags ^= 1; g_dirty = true; }
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_M, route)) {
        IMG *p = (IMG*)img_p; while (p) { p->flags |= 1; p = (IMG*)p->nxt_p; }
    }
    if (ImGui::Shortcut(ImGuiKey_M, route)) {
        IMG *p = (IMG*)img_p; while (p) { p->flags &= ~1; p = (IMG*)p->nxt_p; }
    }
    if (ImGui::Shortcut(ImGuiKey_Semicolon, route)) LeastSquaresReduceMarked();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, route)) OpenFileDialog(FileDialogMode::ExportTga);

    /* Sprite / palette list navigation: cursor up/down flicks
     * between images (default) or palettes (when palette panel
     * was last clicked), matching DOS imgtool muscle memory. */
    if (g_palette_nav && palcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            plselected = (plselected + 1) % (int)palcnt;
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            plselected = (plselected <= 0) ? (int)palcnt - 1 : plselected - 1;
            g_zoom_reset = true;
        }
    } else if (imgcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            ilselected = (ilselected + 1) % (int)imgcnt;
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            ilselected = (ilselected <= 0) ? (int)imgcnt - 1 : ilselected - 1;
            g_zoom_reset = true;
        }
    }
    /* Tab toggles World View mode (anipoint alignment workspace). */
    if (ImGui::Shortcut(ImGuiKey_Tab, route)) {
        g_world_view = !g_world_view;
    }

    /* ---- Menu bar ---- */
    if (ImGui::BeginMainMenuBar()) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
        if (ImGui::BeginMenu("File")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("New"))             g_show_new_img_confirm = true;
            if (ImGui::MenuItem("Open...",  "Ctrl+O")) OpenFileDialog(FileDialogMode::OpenImg);
            if (ImGui::BeginMenu("Open Recent", !g_recent_files.empty())) {
                std::vector<std::string> snap = g_recent_files;
                for (size_t i = 0; i < snap.size(); i++) {
                    char label[1100];
                    snprintf(label, sizeof(label), "%zu. %s", i + 1, snap[i].c_str());
                    if (ImGui::MenuItem(label)) OpenImgFile(snap[i]);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent")) {
                    g_recent_files.clear();
                    RecentSave();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save",    "Ctrl+S")) OpenFileDialog(FileDialogMode::SaveImg);
            if (ImGui::MenuItem("Append",  "a"))      OpenFileDialog(FileDialogMode::AppendImg);
            if (ImGui::MenuItem("Open LOD..."))       OpenFileDialog(FileDialogMode::OpenLod);
            ImGui::Separator();
            if (ImGui::BeginMenu("Import")) {
                if (ImGui::MenuItem("PNG File..."))                 OpenFileDialog(FileDialogMode::ImportPng);
                if (ImGui::MenuItem("PNG (Match to Active Palette)...")) OpenFileDialog(FileDialogMode::ImportPngMatch);
                ImGui::Separator();
                if (ImGui::MenuItem("Load LBM", "Alt+L"))  OpenFileDialog(FileDialogMode::LoadLbm);
                if (ImGui::MenuItem("Load TGA", "Ctrl+L")) OpenFileDialog(FileDialogMode::LoadTga);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Export")) {
                if (ImGui::MenuItem("PNG File..."))                    OpenFileDialog(FileDialogMode::ExportPng);
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
            bool can_undo = g_undo_idx > 0;
            bool can_redo = g_undo_idx < g_undo_count - 1;
            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) { g_undo_idx--; undo_apply(g_undo_idx); }
            if (!can_undo) ImGui::EndDisabled();
            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) { g_undo_idx++; undo_apply(g_undo_idx); }
            if (!can_redo) ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy",  "Ctrl+C", false, ilselected >= 0)) copy_image();
            if (ImGui::MenuItem("Cut",   "Ctrl+X", false, ilselected >= 0)) {
                copy_image();
                DeleteImage(ilselected);
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, g_clipboard.valid && ilselected >= 0))
                paste_image();
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Image", "Ctrl+R")) OpenRenameImage();
            if (ImGui::MenuItem("Delete Image", "Ctrl+D")) DeleteImage(ilselected);
            if (ImGui::MenuItem("Duplicate"))              DuplicateImage();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Image")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Mark / Unmark",      "Space"))  { IMG *img = get_img(ilselected); if (img) img->flags ^= 1; }
            if (ImGui::MenuItem("Set All Marks",      "M"))      { IMG *p=(IMG*)img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Clear All Marks",    "m"))      { IMG *p=(IMG*)img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Invert All Marks"))             { IMG *p=(IMG*)img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;} }
            ImGui::Separator();
            if (ImGui::MenuItem("Jump to Prev Marked", "Left")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (ilselected - i + n_imgs) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Jump to Next Marked", "Right")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (ilselected + i) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Move Up",    "Alt+PgUp")) MoveImageUp();
            if (ImGui::MenuItem("Move Down",  "Alt+PgDn")) MoveImageDown();
            ImGui::Separator();
            if (ImGui::MenuItem("Add/Del Point Table",  "Ctrl+P")) TogglePointTable();
            if (ImGui::MenuItem("Set ID from 2nd List", "i"))      SetIDFromSecondList();
            if (ImGui::MenuItem("Switch Image List",    "Tab"))    SwitchImageList();
            if (ImGui::MenuItem("Clear Extra Data",     "Alt+C"))  ClearExtraData();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Operations")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Least-Squares Reduce", ";"))               LeastSquaresReduceMarked();
            ImGui::Separator();
            if (ImGui::MenuItem("Strip Edge"))                               StripMarkedImages(5);
            if (ImGui::MenuItem("Strip Edge Low"))                           StripMarkedImages(3);
            if (ImGui::MenuItem("Strip Edge (Selected Color)"))              StripMarkedImages(5, g_sel_color);
            if (ImGui::MenuItem("Dither Replace"))                           DitherReplaceMarkedImages(g_sel_color);
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
                IMG *s = get_img(ilselected);
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
            if (ImGui::MenuItem("Delete Marked"))                            DeleteMarkedImages();
            if (ImGui::MenuItem("Set Palette for Marked", "["))              SetPaletteOfMarked();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Palette")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Set for Image",       "]"))       SetPaletteOfSelected();
            if (ImGui::MenuItem("Merge Marked into Selected", "*")) MergeMarkedPalettes();
            if (ImGui::MenuItem("Delete Palette",      "Del"))     DeletePalette();
            if (ImGui::MenuItem("Rename Palette",      "Shift+R")) OpenRenamePalette(plselected);
            ImGui::Separator();
            if (ImGui::MenuItem("Show Histogram"))               { CalculatePaletteHistogram(); g_show_histogram = true; }
            if (ImGui::MenuItem("Delete Unused Colors"))         DeleteUnusedPaletteColors();
            ImGui::Separator();
            if (ImGui::MenuItem("Mark All")) {
                PAL *p=(PAL*)pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Clear Marks")) {
                PAL *p=(PAL*)pal_p; while(p){p->flags&=~1;p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Invert Marks")) {
                PAL *p=(PAL*)pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;}
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            ImGui::MenuItem("Anim Points",     NULL, &g_show_points);
            ImGui::MenuItem("Hitboxes",        NULL, &g_show_hitbox);
            ImGui::MenuItem("DMA Compression", NULL, &g_show_dma_comp);
            ImGui::Separator();
            ImGui::MenuItem("World View",      "Tab",  &g_world_view);
            if (g_world_view) {
                ImGui::MenuItem("  Onion-skin prev frame", NULL, &g_world_onion);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("World W",      &g_world_w, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("World H",      &g_world_h, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Origin X",     &g_world_origin_x, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Origin Y",     &g_world_origin_y, 0, 0);
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
        ImGui::PopStyleVar(2);
        ImGui::EndMainMenuBar();
    }

    float menu_h = ImGui::GetFrameHeight();
    float work_y = menu_h;
    float work_h = sh - work_y;

    /* ---- Sync Palette State ---- */
    static int last_ilselected = -1;
    static int last_plselected = -1;
    static void* last_pal_p = NULL;

    if (ilselected != last_ilselected || pal_p != last_pal_p) {
        last_ilselected = ilselected;
        last_pal_p = pal_p;
        IMG* img = get_img(ilselected);
        if (img) plselected = img->palnum;
        last_plselected = -1; /* force palette reapply when pal_p changes */
    }

    if (plselected != last_plselected) {
        last_plselected = plselected;
        ApplyPalette(plselected);
        g_img_tex_idx = -2; /* Force texture rebuild to use new palette */
        g_hue_slider = 0;
        g_hue_last   = 0;
        save_palette_baseline();
    }

    /* Rebuild image texture every frame to pick up palette and data changes */
    {
        IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
        rebuild_img_texture(img);
        g_img_tex_idx = ilselected;
    }

    /* ===== LEFT TOOLBAR ===== */
    ImGui::SetNextWindowPos(ImVec2(0, work_y));
    ImGui::SetNextWindowSize(ImVec2(TOOLBAR_W, work_h - PALETTE_H));
    ImGui::Begin("##toolbar", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    {
        ImVec2 btn(TOOLBAR_W - 12, TOOLBAR_W - 12);
        #define TB_LABEL(icon, txt) (g_icon_font_loaded ? (icon) : (txt))

        if (ImGui::Button(TB_LABEL(ICON_OPEN, ICON_OPEN_TXT), btn))  OpenFileDialog(FileDialogMode::OpenImg);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open IMG (Ctrl+O)");
        if (ImGui::Button(TB_LABEL(ICON_SAVE, ICON_SAVE_TXT), btn))  OpenFileDialog(FileDialogMode::SaveImg);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save IMG (Ctrl+S)");
        ImGui::Spacing();
        if (ImGui::Button(TB_LABEL(ICON_MARK, ICON_MARK_TXT), btn))  { IMG *img = get_img(ilselected); if (img) img->flags ^= 1; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark/Unmark (Space)");
        if (ImGui::Button(TB_LABEL(ICON_MARK_ALL, ICON_MARK_ALL_TXT), btn))  { IMG *p=(IMG*)img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set All Marks (M)");
        if (ImGui::Button(TB_LABEL(ICON_CLEAR, ICON_CLEAR_TXT), btn))  { IMG *p=(IMG*)img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
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
        /* Marquee/select tool — explicit mode toggle. When off, no green-box
           selection ever starts (no more random firing). */
        ImGui::PushStyleColor(ImGuiCol_Button, g_marquee_active ?
            ImVec4(0.2f,0.4f,0.7f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button(TB_LABEL(ICON_MARQUEE, ICON_MARQUEE_TXT), btn)) {
            g_marquee_active = !g_marquee_active;
            if (!g_marquee_active) g_grid_sel.active = false;
        }
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Marquee Select Tool (R)");
        ImGui::Spacing();
        bool can_undo = g_undo_idx > 0;
        bool can_redo = g_undo_idx < g_undo_count - 1;
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button(TB_LABEL(ICON_UNDO, ICON_UNDO_TXT), btn)) { g_undo_idx--; undo_apply(g_undo_idx); }
        if (!can_undo) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo (Ctrl+Z)");
        if (!can_redo) ImGui::BeginDisabled();
        if (ImGui::Button(TB_LABEL(ICON_REDO, ICON_REDO_TXT), btn)) { g_undo_idx++; undo_apply(g_undo_idx); }
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
    }
    ImGui::End();

    /* ===== RIGHT PANEL STRIP ===== */
    float panel_x = sw - PANEL_W;
    float panel_y = work_y;
    float panel_h = work_h - PALETTE_H;

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
        char img_header[280];
        if (fname_s[0]) {
            const char *basename = fname_s;
            for (const char *p = fname_s; *p; p++) {
                if (*p == '\\' || *p == '/') basename = p + 1;
            }
            snprintf(img_header, sizeof(img_header), "%s", basename);
        } else {
            snprintf(img_header, sizeof(img_header), "Images");
        }
        if (ImGui::CollapsingHeader(img_header, ImGuiTreeNodeFlags_DefaultOpen)) {
            float list_h = panel_h * 0.30f;
            if (ImGui::BeginListBox("##imglist", ImVec2(-1, list_h))) {
                for (int i = 0; i < n_imgs; i++) {
                    IMG *img = get_img(i);
                    if (!img) break;
                    bool marked   = (img->flags & 1) != 0;
                    bool selected = (i == ilselected);
                    ImGui::PushID(i);
                    char label[24];
                    if (marked) snprintf(label, sizeof(label), "* %s", img->n_s);
                    else        snprintf(label, sizeof(label), "  %s", img->n_s);
                    
                    if (selected) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                    if (ImGui::Selectable(label, selected)) {
                        ilselected = i;
                        g_palette_nav = false;
                    }
                    if (selected) ImGui::PopStyleColor();

                    /* Right-click context menu on image items */
                    if (ImGui::BeginPopupContextItem("##imgctx")) {
                        if (ImGui::MenuItem("Mark / Unmark")) { img->flags ^= 1; }
                        if (ImGui::MenuItem("Rename"))        OpenRenameImage();
                        if (ImGui::MenuItem("Delete"))        DeleteImage(ilselected);
                        ImGui::Separator();
                        if (ImGui::MenuItem("Build TGA"))     OpenFileDialog(FileDialogMode::ExportTga);
                        if (ImGui::MenuItem("Set Palette"))   SetPaletteOfSelected();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            /* Mark buttons inline below list */
            if (ImGui::SmallButton("Mk All"))   { IMG *p=(IMG*)img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clr All"))  { IMG *p=(IMG*)img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Invert"))   { IMG *p=(IMG*)img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mk"))       { IMG *img = get_img(ilselected); if (img) img->flags ^= 1; }
            ImGui::SameLine();
            if (ImGui::SmallButton("Add"))      { AddNewBlankImage(); }
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Add a blank 32x32 sprite using the\n"
                                  "currently selected palette. Rename via\n"
                                  "right-click on the new entry.");
        }

        /* --- Palette List --- */
        int n_pals = count_pals();
        if (ImGui::CollapsingHeader("Palettes", ImGuiTreeNodeFlags_DefaultOpen)) {
            float list_h = panel_h * 0.22f;
            if (ImGui::BeginListBox("##pallist", ImVec2(-1, list_h))) {
                for (int i = 0; i < n_pals; i++) {
                    PAL *pal = get_pal(i);
                    if (!pal) break;
                    bool sel    = (i == plselected);
                    bool marked = (pal->flags & 1) != 0;
                    ImGui::PushID(1000 + i);
                    char label[16];
                    if (marked) snprintf(label, sizeof(label), "* %s", pal->n_s);
                    else        snprintf(label, sizeof(label), "  %s", pal->n_s);
                    
                    if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                    if (ImGui::Selectable(label, sel)) {
                        plselected = i;
                        g_palette_nav = true;
                    }
                    if (sel) ImGui::PopStyleColor();

                    /* Right-click context menu */
                    if (ImGui::BeginPopupContextItem("##palctx")) {
                        if (ImGui::MenuItem("Mark / Unmark"))             pal->flags ^= 1;
                        ImGui::Separator();
                        if (ImGui::MenuItem("Set for Image"))             SetPaletteOfSelected();
                        if (ImGui::MenuItem("Set for Marked Images"))     SetPaletteOfMarked();
                        if (ImGui::MenuItem("Merge Marked into Selected")) MergeMarkedPalettes();
                        ImGui::Separator();
                        if (ImGui::MenuItem("Delete Unused Colors")) DeleteUnusedPaletteColors();
                        if (ImGui::MenuItem("Show Histogram")) { CalculatePaletteHistogram(); g_show_histogram = true; }
                        if (ImGui::MenuItem("Rename")) OpenRenamePalette(i);
                        if (ImGui::MenuItem("Delete")) DeletePalette();
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            /* Palette mark buttons (wrapped to 2 rows) */
            if (ImGui::SmallButton("Mk All"))    { PAL *p=(PAL*)pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Clr All"))   { PAL *p=(PAL*)pal_p; while(p){p->flags&=~1;p=(PAL*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Invert"))    { PAL *p=(PAL*)pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;} }
            if (ImGui::SmallButton("Add")) {
                AddNewPalette();
                if (palcnt > 0) plselected = (int)palcnt - 1;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Merge"))     MergeMarkedPalettes();
            ImGui::SameLine();
            if (ImGui::SmallButton("Dup"))       DuplicatePalette();
            ImGui::SameLine();
            if (ImGui::SmallButton("Del"))       DeletePalette();
        }

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img) {
                ImGui::Text("Name:        %.15s", img->n_s);
                ImGui::Text("Size:        %d x %d", (int)img->w, (int)img->h);
                
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
                    ImGui::Text("DMA ROM:  %d B raw", uncomp_size);
                    ImGui::Text("           %d B compressed", comp_size);
                }

                PAL *pal = get_pal(img->palnum);
                if (pal) ImGui::Text("Pal:         %d  %.9s", (int)img->palnum, pal->n_s);
                else     ImGui::Text("Pal:         %d", (int)img->palnum);

                ImGui::Text("AX/AY:       %d, %d", (int)(short)img->anix, (int)(short)img->aniy);
                ImGui::Text("AX2/AY2:     %d, %d", (int)(short)img->anix2, (int)(short)img->aniy2);
                ImGui::Text("AZ2:         %d", (int)(short)img->aniz2);

                char flagbuf[48] = {};
                if (img->flags & 1)  strncat(flagbuf, "Marked ", 47);
                if (img->flags & 2)  strncat(flagbuf, "Loaded ", 47);
                if (img->flags & 4)  strncat(flagbuf, "Changed ", 47);
                if (img->flags & 8)  strncat(flagbuf, "Delete ", 47);
                if (!flagbuf[0])     strncpy(flagbuf, "-", 47);
                ImGui::Text("Flags:       0x%04X  %s", (int)img->flags, flagbuf);

                ImGui::Text("DATA:        %p", img->data_p);

                ImGui::Spacing();
                if (g_clipboard.valid) ImGui::TextDisabled("Clip:   %dx%d pixels", g_clipboard.w, g_clipboard.h);
                ImGui::TextDisabled("Undo:   %d/%d", g_undo_idx + 1, g_undo_count);
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Anim Point Editor --- */
        if (ImGui::CollapsingHeader("Anim Points")) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img) {
                int ax = (short)img->anix, ay = (short)img->aniy;
                int ax2 = (short)img->anix2, ay2 = (short)img->aniy2;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("X1##ptx",  &ax,  -1024, 1024)) { undo_push(); img->anix  = (unsigned short)(short)ax;  }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("Y1##pty",  &ay,  -1024, 1024)) { undo_push(); img->aniy  = (unsigned short)(short)ay;  }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("X2##ptx2", &ax2, -1024, 1024)) { undo_push(); img->anix2 = (unsigned short)(short)ax2; }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("Y2##pty2", &ay2, -1024, 1024)) { undo_push(); img->aniy2 = (unsigned short)(short)ay2; }
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

        /* --- Color --- */
        if (ImGui::CollapsingHeader("Color")) {
            SDL_Color &col = g_palette[g_sel_color];
            int r = col.r, g = col.g, b = col.b;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("R##cr", &r, 0, 255)) {
                col.r = (unsigned char)r;
                palette_writeback(g_sel_color);
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("G##cg", &g, 0, 255)) {
                col.g = (unsigned char)g;
                palette_writeback(g_sel_color);
            }
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("B##cb", &b, 0, 255)) {
                col.b = (unsigned char)b;
                palette_writeback(g_sel_color);
            }
            ImGui::Separator();
            ImGui::Text("Hue");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("##hue", &g_hue_slider, -180, 180)) {
                int delta = g_hue_slider - g_hue_last;
                g_hue_last = g_hue_slider;
                hue_shift_palette(delta);
            }
            if (ImGui::SmallButton("Reset")) {
                reset_palette_to_baseline();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("New from Hue")) {
                PAL *src = (plselected >= 0) ? get_pal(plselected) : NULL;
                if (src && src->data_p) {
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
                            if (palcnt > 0) plselected = (int)palcnt - 1;
                            ApplyPalette(plselected);
                            g_hue_slider = 0;
                            g_hue_last   = 0;
                            g_dirty = true;
                        }
                    }
                }
            }
        }

        /* --- Library Info --- */
        if (ImGui::CollapsingHeader("Library")) {
            ImGui::Text("Images:   %u", imgcnt);
            ImGui::Text("Palettes: %u", palcnt);
            ImGui::Text("Seqs:     %u", seqcnt);
            ImGui::Text("Scripts:  %u", scrcnt);
            ImGui::Text("DamTbls:  %u", damcnt);
            ImGui::Text("Version:  0x%04X", fileversion);
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    /* ===== CANVAS ===== */
    float canvas_x = TOOLBAR_W;
    float canvas_y = work_y;
    float canvas_w = sw - TOOLBAR_W - PANEL_W;
    float canvas_h = work_h - PALETTE_H;

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
        ImVec2 img_sz(0, 0);
        float sx = 1.0f, sy = 1.0f;

        /* ---- World View mode (DOS-style anipoint alignment workspace) ----
         * Renders the sprite inside a fixed black canvas, sprite anchored at
         * (g_world_origin - sprite.anipoint). Left-drag adjusts anix/aniy.
         * Up/Down (handled in the global shortcut block) flicks frames.
         * When this branch runs, the rest of the canvas pipeline (pixel
         * paint, marquee, anim-point handles, hitboxes, DMA overlay,
         * grid-selection) is skipped. */
        if (g_world_view && g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            IMG *cimg = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (cimg) {
                /* Auto-fit the world canvas inside the available area. */
                float fit_x = avail.x / (float)g_world_w;
                float fit_y = avail.y / (float)g_world_h;
                float wscale = (fit_x < fit_y) ? fit_x : fit_y;
                if (wscale < 1.0f) wscale = 1.0f;
                wscale = (float)(int)wscale;
                if (wscale < 1.0f) wscale = 1.0f;

                float ww = (float)g_world_w * wscale;
                float wh = (float)g_world_h * wscale;
                ImVec2 wpos(img_pos.x + (avail.x - ww) * 0.5f,
                            img_pos.y + (avail.y - wh) * 0.5f);

                ImDrawList *dl = ImGui::GetWindowDrawList();
                /* Solid black world background. */
                dl->AddRectFilled(wpos, ImVec2(wpos.x + ww, wpos.y + wh),
                                  IM_COL32(0, 0, 0, 255));
                /* Origin crosshair. */
                float ox = wpos.x + g_world_origin_x * wscale;
                float oy = wpos.y + g_world_origin_y * wscale;
                dl->AddLine(ImVec2(ox - 8, oy), ImVec2(ox + 8, oy),
                            IM_COL32(120, 120, 120, 255));
                dl->AddLine(ImVec2(ox, oy - 8), ImVec2(ox, oy + 8),
                            IM_COL32(120, 120, 120, 255));

                /* Onion-skin: faintly draw the previous sprite. */
                if (g_world_onion && imgcnt > 1) {
                    int prev_idx = (ilselected <= 0) ? (int)imgcnt - 1 : ilselected - 1;
                    IMG *pimg = get_img(prev_idx);
                    if (pimg && pimg->data_p && pimg->w > 0 && pimg->h > 0) {
                        if (!g_world_onion_tex
                            || g_world_onion_tex_w != pimg->w
                            || g_world_onion_tex_h != pimg->h
                            || g_world_onion_idx != prev_idx)
                        {
                            if (g_world_onion_tex) SDL_DestroyTexture(g_world_onion_tex);
                            g_world_onion_tex = SDL_CreateTexture(g_imgui_renderer,
                                SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                                pimg->w, pimg->h);
                            SDL_SetTextureBlendMode(g_world_onion_tex, SDL_BLENDMODE_BLEND);
                            SDL_SetTextureScaleMode(g_world_onion_tex, SDL_ScaleModeNearest);
                            g_world_onion_tex_w = pimg->w;
                            g_world_onion_tex_h = pimg->h;
                            g_world_onion_idx = prev_idx;
                            void *pix; int pitch;
                            if (SDL_LockTexture(g_world_onion_tex, NULL, &pix, &pitch) == 0) {
                                int s = (pimg->w + 3) & ~3;
                                const unsigned char *src = (const unsigned char *)pimg->data_p;
                                Uint32 *dst = (Uint32 *)pix;
                                for (int y = 0; y < pimg->h; y++)
                                for (int x = 0; x < pimg->w; x++) {
                                    unsigned char ci = src[y * s + x];
                                    SDL_Color c = g_palette[ci];
                                    Uint32 a = (ci == 0) ? 0u : 90u;  /* faint */
                                    dst[y * (pitch / 4) + x] =
                                        (a << 24) | ((Uint32)c.r << 16) |
                                        ((Uint32)c.g << 8) | c.b;
                                }
                                SDL_UnlockTexture(g_world_onion_tex);
                            }
                        }
                        float pw = pimg->w * wscale;
                        float ph = pimg->h * wscale;
                        ImVec2 ppos(ox - (int)(short)pimg->anix * wscale,
                                    oy - (int)(short)pimg->aniy * wscale);
                        dl->AddImage((ImTextureID)(intptr_t)g_world_onion_tex,
                                     ppos, ImVec2(ppos.x + pw, ppos.y + ph));
                    }
                }

                /* Sprite at world origin minus its anipoint. */
                int ax = (int)(short)cimg->anix;
                int ay = (int)(short)cimg->aniy;
                float spw = cimg->w * wscale;
                float sph = cimg->h * wscale;
                ImVec2 spos(ox - ax * wscale, oy - ay * wscale);

                dl->AddImage((ImTextureID)(intptr_t)g_img_texture,
                             spos, ImVec2(spos.x + spw, spos.y + sph));

                /* Anchor marker on the sprite's anipoint (== world origin). */
                dl->AddCircle(ImVec2(ox, oy), 4.0f,
                              IM_COL32(255, 200, 0, 255), 0, 1.5f);

                /* Drag-to-move: left-drag inside world canvas adjusts anipoint.
                 * "Sprite follows cursor" means anix/aniy DECREASE as you drag
                 * right/down (the anchor moves left/up in sprite-space). */
                if (!io.WantCaptureMouse) {
                    bool over_world =
                        io.MousePos.x >= wpos.x && io.MousePos.x < wpos.x + ww &&
                        io.MousePos.y >= wpos.y && io.MousePos.y < wpos.y + wh;
                    if (over_world &&
                        ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f))
                    {
                        ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                        ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                        int dx = (int)(d.x / wscale);
                        int dy = (int)(d.y / wscale);
                        if (dx != 0 || dy != 0) {
                            cimg->anix = (unsigned short)((int)(short)cimg->anix - dx);
                            cimg->aniy = (unsigned short)((int)(short)cimg->aniy - dy);
                            g_dirty = true;
                        }
                    }
                    /* Wheel zooms world canvas (changes wscale via origin sizing). */
                }

                /* Coord readout overlay (top-left of world canvas). */
                char buf[64];
                snprintf(buf, sizeof(buf),
                         "[%d] %s   anix=%d aniy=%d   world=%dx%d",
                         ilselected, cimg->n_s, ax, ay, g_world_w, g_world_h);
                dl->AddRectFilled(ImVec2(wpos.x, wpos.y),
                                  ImVec2(wpos.x + 320, wpos.y + 18),
                                  IM_COL32(0, 0, 0, 180));
                dl->AddText(ImVec2(wpos.x + 4, wpos.y + 2),
                            IM_COL32(220, 220, 220, 255), buf);

                /* Reserve the canvas region so other widgets don't overlap. */
                ImGui::Dummy(ImVec2(avail.x, avail.y));
            }
        }
        else if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            /* ---- Zoom: mouse wheel ---- */
            if (ImGui::IsWindowHovered()) {
                float wh = io.MouseWheel;
                if (wh != 0.0f) {
                    float old_z = g_zoom;
                    g_zoom += wh * 0.25f;
                    if (g_zoom < 1.0f) g_zoom = 1.0f;
                    if (g_zoom > 32.0f) g_zoom = 32.0f;
                    if (g_zoom <= 4.0f) g_zoom = roundf(g_zoom);
                    ImVec2 m = io.MousePos;
                    float cx = img_pos.x + img_sz.x * 0.5f;
                    float cy = img_pos.y + img_sz.y * 0.5f;
                    g_pan_x = (g_pan_x + cx - m.x) * (g_zoom / old_z) - (cx - m.x);
                    g_pan_y = (g_pan_y + cy - m.y) * (g_zoom / old_z) - (cy - m.y);
                    g_zoom_reset = false;
                }
            }

            if (g_zoom_reset) { g_zoom = 1.0f; g_pan_x = 0; g_pan_y = 0; g_zoom_reset = false; }
            else if (ilselected != g_img_tex_idx) g_zoom_reset = true;

            float scale = g_zoom;
            if (g_zoom == 1.0f) {
                float fitscale = (float)(int)(avail.x / (float)g_img_tex_w);
                if (fitscale < 1.0f) fitscale = 1.0f;
                float fith = (float)g_img_tex_h * fitscale;
                if (fith > avail.y) fitscale = (float)(int)(avail.y / (float)g_img_tex_h);
                if (fitscale < 1.0f) fitscale = 1.0f;
                scale = fitscale;
            }
            float tw = (float)g_img_tex_w * scale;
            float th = (float)g_img_tex_h * scale;
            float off_x = (avail.x - tw) * 0.5f + g_pan_x;
            float off_y = (avail.y - th) * 0.5f + g_pan_y;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off_y);

            img_pos = ImGui::GetCursorScreenPos();
            img_sz  = ImVec2(tw, th);
            sx = tw / (float)g_img_tex_w;
            sy = th / (float)g_img_tex_h;

            /* Checkerboard background for transparency */
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float cs = 8.0f * scale; if (cs < 8.f) cs = 8.f;
            for (float cy = img_pos.y; cy < img_pos.y + th; cy += cs) {
                for (float cx2 = img_pos.x; cx2 < img_pos.x + tw; cx2 += cs) {
                    int row = (int)((cy - img_pos.y) / cs);
                    int col = (int)((cx2 - img_pos.x) / cs);
                    ImU32 col32 = ((row + col) & 1) ? IM_COL32(160,160,160,255) : IM_COL32(100,100,100,255);
                    float x2 = cx2 + cs; if (x2 > img_pos.x + tw) x2 = img_pos.x + tw;
                    float y2 = cy  + cs; if (y2 > img_pos.y + th) y2 = img_pos.y + th;
                    dl->AddRectFilled(ImVec2(cx2, cy), ImVec2(x2, y2), col32);
                }
            }
            ImGui::Image((ImTextureID)(intptr_t)g_img_texture, img_sz);

            /* --- DMA Compression overlay --- */
            if (g_show_dma_comp) {
                IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
                if (img && img->data_p) {
                    unsigned short stride = (img->w + 3) & ~3;
                    unsigned char *pixels = (unsigned char *)img->data_p;
                    for (int y = 0; y < img->h; y++) {
                        int leading = 0;
                        while (leading < img->w && pixels[y * stride + leading] == 0) leading++;

                        if (leading == img->w) {
                            /* Entire line is compressed */
                            ImVec2 p_min(img_pos.x, img_pos.y + y * sy);
                            ImVec2 p_max(img_pos.x + img->w * sx, img_pos.y + (y + 1) * sy);
                            dl->AddRectFilled(p_min, p_max, IM_COL32(255, 0, 255, 100));
                        } else {
                            /* Leading zeros */
                            if (leading > 0) {
                                ImVec2 p_min(img_pos.x, img_pos.y + y * sy);
                                ImVec2 p_max(img_pos.x + leading * sx, img_pos.y + (y + 1) * sy);
                                dl->AddRectFilled(p_min, p_max, IM_COL32(255, 0, 255, 100));
                            }
                            /* Trailing zeros */
                            int trailing = 0;
                            while (trailing < img->w && pixels[y * stride + (img->w - 1 - trailing)] == 0) trailing++;
                            if (trailing > 0) {
                                ImVec2 p_min(img_pos.x + (img->w - trailing) * sx, img_pos.y + y * sy);
                                ImVec2 p_max(img_pos.x + img->w * sx, img_pos.y + (y + 1) * sy);
                                dl->AddRectFilled(p_min, p_max, IM_COL32(0, 255, 255, 100));
                            }
                        }
                    }
                }
            }

            /* Pixel grid overlay at high zoom */
            if (scale >= 4.0f) {
                ImU32 gc = IM_COL32(60, 60, 60, 100);
                for (int x = 0; x <= g_img_tex_w; x++)
                    dl->AddLine(ImVec2(img_pos.x + x * sx, img_pos.y),
                                ImVec2(img_pos.x + x * sx, img_pos.y + th), gc, 0.5f);
                for (int y = 0; y <= g_img_tex_h; y++)
                    dl->AddLine(ImVec2(img_pos.x, img_pos.y + y * sy),
                                ImVec2(img_pos.x + tw, img_pos.y + y * sy), gc, 0.5f);
            }

            /* Zoom indicator */
            if (g_zoom > 1.001f) {
                char zbuf[32];
                snprintf(zbuf, sizeof(zbuf), "%.0f%%", g_zoom * 100.0f);
                ImGui::SetCursorPos(ImVec2(8, 4));
                ImGui::TextDisabled("%s", zbuf);
            }
        } else {
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

        /* Pixel highlight at high zoom */
        if (!canvas_input_blocked) {
            bool over = mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                        mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y;
            if (over && img_sz.x > 0 && g_zoom >= 4.0f) {
                int hx = (int)((mouse.x - img_pos.x) / sx);
                int hy = (int)((mouse.y - img_pos.y) / sy);
                ImDrawList *dl = ImGui::GetWindowDrawList();
                dl->AddRect(ImVec2(img_pos.x + hx * sx, img_pos.y + hy * sy),
                            ImVec2(img_pos.x + (hx + 1) * sx, img_pos.y + (hy + 1) * sy),
                            IM_COL32(255, 255, 0, 180), 0.0f, 0, 1.5f);
            }
        }

        /* ---- Pencil + eyedropper + fill + pan tools ---- */
        if (!canvas_input_blocked) {
            IMG *cimg = (ilselected >= 0) ? get_img(ilselected) : NULL;
            bool over = mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                        mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y;

            /* Pan: middle-mouse drag or spacebar+drag or right-drag at zoom */
            if (ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 0.0f)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Middle, 0.0f);
                g_pan_x += d.x; g_pan_y += d.y;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Middle);
                widget_consumed_click = true;
            }
            if (ImGui::IsKeyDown(ImGuiKey_Space) && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Left, 0.0f);
                g_pan_x += d.x; g_pan_y += d.y;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
                widget_consumed_click = true;
            }
            if (!g_marquee_active && g_zoom > 1.0f &&
                ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f) && over) {
                ImVec2 d = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.0f);
                g_pan_x += d.x; g_pan_y += d.y;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
                widget_consumed_click = true;
            }

            if (cimg && cimg->data_p && cimg->w > 0 && cimg->h > 0 && over) {
                int px = (int)((mouse.x - img_pos.x) / sx);
                int py = (int)((mouse.y - img_pos.y) / sy);
                if (px >= 0 && px < (int)cimg->w && py >= 0 && py < (int)cimg->h) {
                    unsigned short stride = (cimg->w + 3) & ~3;
                    unsigned char *pix = (unsigned char *)cimg->data_p + py * stride + px;

                    /* Right-click: eyedropper */
                    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                        g_sel_color = *pix;
                        widget_consumed_click = true;
                    }
                    /* Left-click: pencil or fill */
                    if (!g_marquee_active && !g_pasted.active &&
                        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                        if (g_pixel_undo_img != ilselected) {
                            free(g_pixel_undo); g_pixel_undo = NULL;
                            unsigned short s = (cimg->w + 3) & ~3;
                            unsigned int sz = (unsigned int)s * cimg->h;
                            g_pixel_undo = (unsigned char *)malloc(sz);
                            if (g_pixel_undo) memcpy(g_pixel_undo, cimg->data_p, sz);
                            g_pixel_undo_img = ilselected;
                        }
                        if (io.KeyShift) {
                            g_dirty = true;
                            FloodFill(cimg, px, py, (unsigned char)g_sel_color);
                        } else {
                            g_dirty = true;
                            *pix = (unsigned char)g_sel_color;
                        }
                        g_img_tex_idx = -2;
                        widget_consumed_click = true;
                    }
                }
            }
        }

        /* --- Anim point overlay + dragging --- */
        if (g_show_points && !canvas_input_blocked) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img && img->w > 0) {
                ImDrawList *dl = ImGui::GetWindowDrawList();

                ImVec2 s1(img_pos.x + img->anix * sx, img_pos.y + img->aniy * sy);
                ImVec2 d1 = mouse - s1;
                bool h1 = (d1.x*d1.x + d1.y*d1.y) < 10*10;
                dl->AddCircleFilled(s1, 6.f, h1 ? IM_COL32(255,120,0,255) : IM_COL32(255,0,0,255));
                dl->AddCircle(s1, 6.f, IM_COL32(255,255,255,255), 0, 1.5f);

                static bool drag1 = false;
                if (h1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { drag1 = true; undo_push(); widget_consumed_click = true; }
                if (drag1 && mbdn) {
                    int nx = (int)((mouse.x - img_pos.x) / sx);
                    int ny = (int)((mouse.y - img_pos.y) / sy);
                    img->anix = (unsigned short)(short)nx;
                    img->aniy = (unsigned short)(short)ny;
                } else if (!mbdn && drag1) { drag1 = false; undo_push(); }

                if (img->anix2 || img->aniy2) {
                    ImVec2 s2(img_pos.x + img->anix2 * sx, img_pos.y + img->aniy2 * sy);
                    ImVec2 d2 = mouse - s2;
                    bool h2 = (d2.x*d2.x + d2.y*d2.y) < 10*10;
                    dl->AddCircleFilled(s2, 6.f, h2 ? IM_COL32(0,255,120,255) : IM_COL32(0,255,0,255));
                    dl->AddCircle(s2, 6.f, IM_COL32(255,255,255,255), 0, 1.5f);
                    dl->AddLine(s1, s2, IM_COL32(255,255,0,192), 1.f);

                    static bool drag2 = false;
                    if (h2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { drag2 = true; undo_push(); widget_consumed_click = true; }
                    if (drag2 && mbdn) {
                        int nx = (int)((mouse.x - img_pos.x) / sx);
                        int ny = (int)((mouse.y - img_pos.y) / sy);
                        img->anix2 = (unsigned short)(short)nx;
                        img->aniy2 = (unsigned short)(short)ny;
                    } else if (!mbdn && drag2) { drag2 = false; undo_push(); }
                }
            }
        }

        /* --- Hitbox overlay + corner dragging --- */
        if (g_show_hitbox && !canvas_input_blocked) {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            ImVec2 tl(img_pos.x + g_hitbox_x * sx, img_pos.y + g_hitbox_y * sy);
            ImVec2 br(img_pos.x + (g_hitbox_x + g_hitbox_w) * sx,
                      img_pos.y + (g_hitbox_y + g_hitbox_h) * sy);
            ImVec2 tr(br.x, tl.y), bl(tl.x, br.y);
            dl->AddRect(tl, br, IM_COL32(0,255,255,255), 0, 0, 2.f);

            ImVec2 corners[4] = { tl, tr, br, bl };
            float hr = 12.f * 12.f;
            bool hovering[4];
            for (int c = 0; c < 4; c++) {
                ImVec2 d = mouse - corners[c];
                hovering[c] = (d.x*d.x + d.y*d.y < hr);
            }
            for (int c = 0; c < 4; c++) {
                ImU32 col = hovering[c] ? IM_COL32(255,255,0,255) : IM_COL32(0,255,255,255);
                dl->AddCircleFilled(corners[c], 5.f, col);
            }
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

        /* --- Grid selection tool (for copy/paste) --- */
        if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            ImDrawList *dl = ImGui::GetWindowDrawList();

            /* Mouse-over-sprite test — clicks outside this rect must NOT start a selection. */
            bool mouse_over_sprite =
                mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y;

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
            if (!g_pasted.active && g_marquee_active) {
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)
                        && mouse_over_sprite
                        && !widget_consumed_click
                        && !ui_blocking) {
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    if (mx < 0) mx = 0; if (mx >= (int)g_img_tex_w) mx = g_img_tex_w - 1;
                    if (my < 0) my = 0; if (my >= (int)g_img_tex_h) my = g_img_tex_h - 1;
                    g_grid_sel.active = true;
                    g_grid_sel.dragging = true;
                    g_grid_sel.x1 = g_grid_sel.x2 = mx;
                    g_grid_sel.y1 = g_grid_sel.y2 = my;
                } else if (g_grid_sel.dragging && mbdn) {
                    /* Only extend the rect while we're in the user-initiated
                       drag — not on every frame the button happens to be
                       down (e.g. a click on a menu would otherwise reposition
                       the marquee to wherever the menu click landed). */
                    int mx = (int)((mouse.x - img_pos.x) / sx);
                    int my = (int)((mouse.y - img_pos.y) / sy);
                    if (mx < 0) mx = 0; if (mx >= (int)g_img_tex_w) mx = g_img_tex_w - 1;
                    if (my < 0) my = 0; if (my >= (int)g_img_tex_h) my = g_img_tex_h - 1;
                    g_grid_sel.x2 = mx;
                    g_grid_sel.y2 = my;
                } else if (g_grid_sel.dragging && !mbdn) {
                    g_grid_sel.dragging = false;
                }
            }

            /* Draw selection rectangle only when the marquee tool is on. Toggling
               the tool off via the toolbar/R also clears g_grid_sel, but this
               extra gate makes sure no stray green box renders if some other
               code path leaves g_grid_sel.active=true with the tool off. */
            if (g_grid_sel.active && g_marquee_active) {
                int x1 = g_grid_sel.x1, y1 = g_grid_sel.y1;
                int x2 = g_grid_sel.x2, y2 = g_grid_sel.y2;
                if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
                if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }
                ImVec2 r1(img_pos.x + x1 * sx, img_pos.y + y1 * sy);
                ImVec2 r2(img_pos.x + (x2 + 1) * sx, img_pos.y + (y2 + 1) * sy);
                dl->AddRect(r1, r2, IM_COL32(0, 255, 0, 255), 0.0f, 0, 2.0f);
                dl->AddRectFilled(r1, r2, IM_COL32(0, 255, 0, 30), 0.0f);
            }

            /* Paste overlay with pixel preview */
            if (g_pasted.active && g_clipboard.valid && g_clipboard.w > 0 && g_clipboard.h > 0) {
                int px = g_pasted.paste_x;
                int py = g_pasted.paste_y;
                int pw = g_clipboard.w;
                int ph = g_clipboard.h;
                unsigned short cs = g_clipboard.stride;

                ImVec2 p1(img_pos.x + px * sx, img_pos.y + py * sy);
                ImVec2 p2(img_pos.x + (px + pw) * sx, img_pos.y + (py + ph) * sy);
                bool hovering = mouse.x >= p1.x && mouse.x < p2.x && mouse.y >= p1.y && mouse.y < p2.y;
                bool over_sprite = mouse.x >= img_pos.x && mouse.x < img_pos.x + img_sz.x &&
                                   mouse.y >= img_pos.y && mouse.y < img_pos.y + img_sz.y;

                /* Render clipboard pixel preview with alpha */
                IMG *simg = (ilselected >= 0) ? get_img(ilselected) : NULL;
                PAL *spal = simg ? get_pal(simg->palnum) : NULL;
                const unsigned char *pald = spal ? (const unsigned char *)spal->data_p : NULL;
                const unsigned char *src = (const unsigned char *)g_clipboard.data_p;
                for (int y = 0; y < ph; y++) {
                    for (int x = 0; x < pw; x++) {
                        unsigned char ci = src[y * cs + x];
                        if (ci == 0) continue;
                        ImU32 col;
                        if (pald) {
                            unsigned short w15 = (unsigned short)(pald[ci*2] | (pald[ci*2+1] << 8));
                            col = IM_COL32(
                                (unsigned char)(((w15 >> 10) & 0x1F) << 3),
                                (unsigned char)(((w15 >>  5) & 0x1F) << 3),
                                (unsigned char)(( w15        & 0x1F) << 3),
                                160);
                        } else {
                            col = IM_COL32(255, 255, 255, 160);
                        }
                        ImVec2 pp1(img_pos.x + (px + x) * sx, img_pos.y + (py + y) * sy);
                        ImVec2 pp2(img_pos.x + (px + x + 1) * sx, img_pos.y + (py + y + 1) * sy);
                        dl->AddRectFilled(pp1, pp2, col);
                    }
                }

                /* Border — gold when hovering, yellow otherwise */
                ImU32 border_col = hovering ? IM_COL32(255, 200, 0, 255) : IM_COL32(255, 255, 0, 255);
                dl->AddRect(p1, p2, border_col, 0.0f, 0, 2.0f);

                /* Instruction text */
                if (g_pasted.dragging)
                    dl->AddText(ImVec2(img_pos.x + 6, img_pos.y + 6), IM_COL32(255, 200, 0, 255), "Moving...");
                else
                    dl->AddText(ImVec2(img_pos.x + 6, img_pos.y + 6), IM_COL32(255, 255, 0, 255), "Drag to move | Click outside to place | Esc cancel");

                if (!canvas_input_blocked) {
                    /* Start drag: click inside paste rect */
                    if (hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                        g_pasted.dragging = true;

                    /* Drag to move */
                    if (g_pasted.dragging && mbdn) {
                        int nx = (int)((mouse.x - img_pos.x) / sx);
                        int ny = (int)((mouse.y - img_pos.y) / sy);
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
                    if (!hovering && over_sprite && !g_pasted.dragging &&
                        ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    {
                        apply_pasted_region();
                        g_pasted.active = false;
                    }
                }
            }
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

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
        float       sw16 = 14.0f;
        float       sh16 = 14.0f;
        float       gap  = 1.0f;
        float       row_h = sh16 + gap;
        float       col_w = sw16 + gap;

        for (int i = 0; i < 256; i++) {
            int row = i / 16, col = i % 16;
            ImVec2 p0(pos0.x + col * col_w, pos0.y + row * row_h);
            ImVec2 p1(p0.x + sw16, p0.y + sh16);
            SDL_Color c = g_palette[i];
            dl->AddRectFilled(p0, p1, IM_COL32(c.r, c.g, c.b, 255));
            if (i == g_sel_color)
                dl->AddRect(p0, p1, IM_COL32(255,255,255,255), 0, 0, 1.5f);
            else if (i == 0)
                dl->AddRect(p0, p1, IM_COL32(80,80,80,120), 0, 0, 0.5f);

            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton(("##sw" + std::to_string(i)).c_str(), ImVec2(sw16, sh16));
            if (ImGui::IsItemClicked()) g_sel_color = i;
        }

        /* Selected color index + RGB */
        ImGui::SetCursorScreenPos(ImVec2(pos0.x + 16 * col_w + 8, pos0.y + 4));
        SDL_Color &c = g_palette[g_sel_color];
        ImGui::Text("#%d  R:%d G:%d B:%d", g_sel_color, c.r, c.g, c.b);
            }
            ImGui::Separator();
            ImGui::MenuItem("Verbose Logging",  NULL, &g_verbose);
            ImGui::PopStyleVar();
    ImGui::End();

    /* ===== RENAME DIALOG ===== */
    const char *rename_title =
        g_rename_target == RenameTarget::Image          ? "Rename Image" :
        g_rename_target == RenameTarget::Palette        ? "Rename Palette" :
                                                          "Rename Marked Images";
    if (g_show_rename) ImGui::OpenPopup(rename_title);
    if (ImGui::BeginPopupModal(rename_title, &g_show_rename, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (g_rename_target == RenameTarget::MarkedImages) {
            ImGui::TextWrapped("Base name. Prefix with '+' to prepend "
                               "to existing names; otherwise the base is "
                               "used with a numeric suffix (1, 2, 3, ...).");
        } else if (g_rename_target == RenameTarget::Image) {
            IMG *img = get_img(g_rename_idx);
            if (img) ImGui::Text("Rename: %s", img->n_s);
        } else {
            PAL *pal = get_pal(g_rename_idx);
            if (pal) ImGui::Text("Rename: %s", pal->n_s);
        }
        /* Cap input width to whatever the destination field can hold. */
        const int maxlen =
            g_rename_target == RenameTarget::Palette ? 10 : 16;
        ImGui::InputText("##rn", g_rename_buf,
                         (size_t)maxlen < sizeof(g_rename_buf) ? maxlen : sizeof(g_rename_buf));
        if (ImGui::Button("OK", ImVec2(100, 0))) {
            if (g_rename_target == RenameTarget::Image) {
                IMG *img = get_img(g_rename_idx);
                if (img) {
                    undo_push();
                    strncpy(img->n_s, g_rename_buf, 15);
                    img->n_s[15] = '\0';
                }
            } else if (g_rename_target == RenameTarget::Palette) {
                PAL *pal = get_pal(g_rename_idx);
                if (pal) {
                    undo_push();
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

    /* ===== LOAD2 PACKING VERIFY DIALOG ===== */
    if (g_show_load2_verify) ImGui::OpenPopup("LOAD2 Packing Verify");
    if (ImGui::BeginPopupModal("LOAD2 Packing Verify", &g_show_load2_verify,
                               ImGuiWindowFlags_AlwaysAutoResize))
    {
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
                    if (iss.img_idx >= 0) ilselected = iss.img_idx;
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

            /* ---- Drift visualization for the selected issue ---- */
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

    /* ===== PALETTE HISTOGRAM DIALOG ===== */
    if (g_show_histogram) ImGui::OpenPopup("Palette Histogram");
    if (ImGui::BeginPopupModal("Palette Histogram", &g_show_histogram, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Images using this palette: %d", g_histogram_img_count);
        ImGui::Text("Max occurrences (excluding index 0): %.0f", g_histogram_max);
        ImGui::Spacing();

        ImDrawList* draw_list = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float width = 512.0f; // 2 pixels per color bar
        float height = 150.0f;

        // Draw dark background for the chart
        draw_list->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(20, 20, 20, 255));

        // Draw custom colored bars
        float bar_w = width / 256.0f;
        for (int i = 0; i < 256; i++) {
            float val = g_histogram_data[i];
            if (val > 0.0f) {
                float bar_h = (val / g_histogram_max) * height;
                if (bar_h > height) bar_h = height; // Clamp index 0 if it exceeds the max of the other colors
                if (bar_h < 1.0f) bar_h = 1.0f;     // Guarantee visible colors show at least 1px height

                ImVec2 p_min = ImVec2(p.x + i * bar_w, p.y + height - bar_h);
                ImVec2 p_max = ImVec2(p.x + (i + 1) * bar_w, p.y + height);

                SDL_Color c = g_palette[i];
                draw_list->AddRectFilled(p_min, p_max, IM_COL32(c.r, c.g, c.b, 255));
            }
        }

        ImGui::Dummy(ImVec2(width, height)); // Advance ImGui layout cursor past our custom draw area

        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            g_show_histogram = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ===== BULK RESTORE REGEX DIALOG ===== */
    if (g_show_restore_regex) ImGui::OpenPopup("Bulk Restore via Regex");
    if (ImGui::BeginPopupModal("Bulk Restore via Regex", &g_show_restore_regex, ImGuiWindowFlags_AlwaysAutoResize)) {
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
                    for (IMG *child = (IMG *)img_p; child; child = (IMG *)child->nxt_p) {
                        if (!child->data_p || child->w == 0 || child->h == 0) continue;
                        std::string name(child->n_s);
                        std::smatch match;
                        if (std::regex_match(name, match, re) && match.size() > 1) {
                            std::string parent_name = match[1].str();
                            IMG *parent = NULL;
                            for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p) {
                                if (parent_name == p->n_s) {
                                    parent = p;
                                    break;
                                }
                            }
                            if (parent && parent->data_p && parent->w > 0 && parent->h > 0 && parent != child
                                && parent->palnum == child->palnum) {
                                /* Same-palette only — pixel indices across different palettes
                                 * map to different colors, producing garbled output. */
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

    /* ===== HELP MODAL ===== */
    /* ---- Debug Info popup ---- */
    if (g_show_debug) ImGui::OpenPopup("Debug Info");
    if (ImGui::BeginPopupModal("Debug Info", &g_show_debug, ImGuiWindowFlags_NoMove)) {
        ImGui::SetNextWindowSize(ImVec2(520, 580), ImGuiCond_Always);

        /* --- Library Header (LIB_HDR + seq/scr blob round-trip status) --- */
        if (ImGui::CollapsingHeader("LIB_HDR", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("IMGCNT:  %u",     imgcnt);
            ImGui::Text("PALCNT:  %u",     palcnt);
            ImGui::Text("SEQCNT:  %u",     seqcnt);
            ImGui::Text("SCRCNT:  %u",     scrcnt);
            ImGui::Text("DAMCNT:  %u",     damcnt);
            ImGui::Text("VERSION: 0x%04X", fileversion);
            ImGui::Separator();
            ImGui::TextDisabled("SEQSCR/ENTRY blob (load-time, round-trips on save):");
            if (scrseqmem_p && scrseqbytes > 0) {
                ImGui::Text("SCRSEQBYTES:  %u bytes", scrseqbytes);
            } else {
                ImGui::TextDisabled("SCRSEQBYTES:  0  (no seq/scr in file)");
            }
        }

        /* --- Selected IMAGE record (runtime + disk-side) --- */
        IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
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
                /* Pixel-data byte size, derived from runtime w/h (CMP variable) */
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

        /* --- Selected PALETTE record (runtime) --- */
        PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
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

    /* ===== FILE DIALOG ===== */
    DrawFileDialog();

    /* ===== NEW IMG CONFIRMATION ===== */
    if (g_show_new_img_confirm) ImGui::OpenPopup("New IMG");
    if (ImGui::BeginPopupModal("New IMG", &g_show_new_img_confirm, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Discard all loaded images and palettes?");
        ImGui::Text("This cannot be undone.");
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("New", ImVec2(80, 0))) {
            ClearAll();
            fileversion = 0x0634;   /* Wimp V6.34 — current format */
            fname_s[0]  = 0;
            g_undo_count = 0;
            g_undo_idx   = 0;
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

    /* ===== UNSAVED CHANGES CONFIRMATION ===== */
    if (g_pending_quit && !g_show_unsaved_confirm) {
        if (g_dirty && imgcnt > 0) {
            g_show_unsaved_confirm = true;
        }
    }
    if (g_show_unsaved_confirm) ImGui::OpenPopup("Unsaved Changes");
    if (ImGui::BeginPopupModal("Unsaved Changes", &g_show_unsaved_confirm, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("You have unsaved changes.");
        ImGui::Text("Do you want to save before quitting?");
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Save", ImVec2(80, 0))) {
            g_show_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
            if (fname_s[0] != '\0') {
                SaveImgFile();
                g_dirty = false;
            } else {
                g_pending_quit = false;
                OpenFileDialog(FileDialogMode::SaveImg);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(80, 0))) {
            g_show_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            g_show_unsaved_confirm = false;
            g_pending_quit = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_show_help) ImGui::OpenPopup("Help");
    if (ImGui::BeginPopupModal("Help", &g_show_help,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
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

    /* ===== ABOUT MODAL ===== */
    if (g_show_about) ImGui::OpenPopup("About Imgtool");
    if (ImGui::BeginPopupModal("About Imgtool", &g_show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Imgtool");
        ImGui::Separator();
        ImGui::Text("A modern port of the 1992 Midway Image Tool.");
        ImGui::Spacing();
        ImGui::Text("Build: %s %s", __DATE__, __TIME__);
#ifdef IMGTOOL_GIT_REV
        ImGui::Text("Commit: %s", IMGTOOL_GIT_REV);
#endif
        ImGui::Text("ImGui: %s", IMGUI_VERSION);
        ImGui::Spacing();
        ImGui::TextLinkOpenURL("https://github.com/junkwax/midway-imgtool");
        ImGui::Spacing();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            g_show_about = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* Transient toast (e.g. RestoreMarkedFromSource result). */
    if (g_restore_msg_timer > 0.0f) {
        g_restore_msg_timer -= io.DeltaTime;
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

    /* Flush to renderer */
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_imgui_renderer);
}

void imgui_overlay_shutdown(void)
{
    if (g_img_texture) { SDL_DestroyTexture(g_img_texture); g_img_texture = NULL; }
    if (g_world_onion_tex) { SDL_DestroyTexture(g_world_onion_tex); g_world_onion_tex = NULL; }
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void imgui_overlay_present(void)
{
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_imgui_renderer);
}
