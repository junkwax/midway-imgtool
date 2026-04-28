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
#include "compat.h"
#include "imgui_overlay.h"
#include "itimg_exports.h"

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

static void PosixCrashHandler(int sig) {
    fprintf(stderr, "CRASH: Fatal signal %d received!\n", sig);
    exit(1);
}
#endif

/* Structure definitions matching wmpstruc.inc */
#pragma pack(push, 2)
struct IMG {
    void          *nxt_p;
    char           n_s[16];
    unsigned short flags;
    unsigned short anix;
    unsigned short aniy;
    unsigned short w;
    unsigned short h;
    unsigned short palnum;
    void          *data_p;
    void          *pttbl_p;     /* point table pointer or NULL */
    unsigned short anix2;
    unsigned short aniy2;
    unsigned short aniz2;
    unsigned short opals;
    void          *temp;        /* temp for offset when saving */
    /* Debug-only: disk file fields (populated during load, used during save) */
    unsigned int   file_oset;   /* offset in file of this image */
    unsigned short file_lib;    /* library handle index */
    unsigned short file_frm;    /* frame number for anim */
    unsigned short file_pttblnum; /* point table index or 0xFFFF */
};

struct PAL {
    void          *nxt_p;
    char           n_s[10];
    unsigned char  flags;
    unsigned char  bitspix;   /* bits per pixel — 8 for 256-color */
    unsigned short numc;      /* number of colors */
    unsigned short pad;
    void          *data_p;    /* pointer to packed 15-bit RGB words (2 bytes each, little-endian: XRRRRRGG GGGBBBBB) */
    void          *temp;      /* temp for offset when saving */
};

/* Pack/unpack helpers for the 15-bit RGB palette word stored in PAL.DATA_p.
   Format matches what shim_setvgapal15_impl reads and what img_save writes to disk:
   little-endian word = XRRRRRGG GGGBBBBB, 5 bits per channel. */
static inline void pal_word_to_rgb8(const unsigned char *src, unsigned char *r, unsigned char *g, unsigned char *b)
{
    unsigned short w = (unsigned short)(src[0] | (src[1] << 8));
    unsigned char r5 = (unsigned char)((w >> 10) & 0x1F);
    unsigned char g5 = (unsigned char)((w >>  5) & 0x1F);
    unsigned char b5 = (unsigned char)( w        & 0x1F);
    *r = (unsigned char)((r5 << 3) | (r5 >> 2));
    *g = (unsigned char)((g5 << 3) | (g5 >> 2));
    *b = (unsigned char)((b5 << 3) | (b5 >> 2));
}
static inline void rgb8_to_pal_word(unsigned char r, unsigned char g, unsigned char b, unsigned char *dst)
{
    unsigned short r5 = (unsigned short)(r >> 3);
    unsigned short g5 = (unsigned short)(g >> 3);
    unsigned short b5 = (unsigned short)(b >> 3);
    unsigned short w  = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
    dst[0] = (unsigned char)(w & 0xFF);
    dst[1] = (unsigned char)((w >> 8) & 0xFF);
}
#pragma pack(pop)

/* Disk file-format structures matching wmpstruc.inc */
#pragma pack(push, 2)
struct LIB_HDR {
    unsigned short imgcnt;    /* +0  number of images */
    unsigned short palcnt;    /* +2  number of palettes */
    unsigned int   oset;      /* +4  offset to IMAGE/PALETTE records */
    unsigned short version;   /* +8  WIMP version (0x500+) */
    unsigned short seqcnt;    /* +10 sequence count */
    unsigned short scrcnt;    /* +12 script count */
    unsigned short damcnt;    /* +14 damage table count */
    unsigned short temp;      /* +16 0xABCD if valid */
    unsigned char  bufscr[4]; /* +18 buffer script indices */
    unsigned short spare1;    /* +22 */
    unsigned short spare2;    /* +24 */
    unsigned short spare3;    /* +26 */
};

struct IMAGE_disk {
    char           n_s[16];   /* +0  image name */
    unsigned short flags;     /* +16 */
    unsigned short anix;      /* +18 */
    unsigned short aniy;      /* +20 */
    unsigned short w;         /* +22 */
    unsigned short h;         /* +24 */
    unsigned short palnum;    /* +26 */
    unsigned int   oset;      /* +28 offset to pixel data */
    unsigned int   data;      /* +32 (unused in file, placeholder) */
    unsigned short lib;       /* +36 library index */
    unsigned short anix2;     /* +38 */
    unsigned short aniy2;     /* +40 */
    unsigned short aniz2;     /* +42 */
    unsigned short frm;       /* +44 frame number */
    unsigned short opals;     /* +46 */
    unsigned short pttblnum;  /* +48 point table index or 0xFFFF */
};

struct PALETTE_disk {
    char           n_s[10];   /* +0  palette name */
    unsigned char  flags;     /* +10 */
    unsigned char  bitspix;   /* +11 */
    unsigned short numc;      /* +12 number of colors */
    unsigned int   oset;      /* +14 offset to color data */
    unsigned short data;      /* +18 (unused in file) */
    unsigned short lib;       /* +20 */
    unsigned char  colind;    /* +22 */
    unsigned char  cmap;      /* +23 */
    unsigned short spare;     /* +24 */
};
#pragma pack(pop)

#define NUMDEFPAL 3

/* Allocator wrappers — switch from ASM pool to C heap.
   Once img_load/img_save are fully ported, the ASM memory pool
   is no longer needed by the C++ code path. */
static IMG *AllocImg(void)
{
    IMG *img = (IMG *)calloc(1, sizeof(IMG));
    if (!img) return NULL;
    /* Link into list */
    IMG **pp = (IMG **)&img_p;
    while (*pp) pp = (IMG **)&(*pp)->nxt_p;
    *pp = img;
    imgcnt++;
    return img;
}

static PAL *AllocPal(void)
{
    PAL *pal = (PAL *)calloc(1, sizeof(PAL));
    if (!pal) return NULL;
    PAL **pp = (PAL **)&pal_p;
    while (*pp) pp = (PAL **)&(*pp)->nxt_p;
    *pp = pal;
    palcnt++;
    return pal;
}

static void FreeImg(IMG *img)
{
    if (!img) return;
    free(img->data_p);
    free(img->pttbl_p);
    free(img);
}

static void FreePal(PAL *pal)
{
    if (!pal) return;
    free(pal->data_p);
    free(pal);
}

static void *PoolAlloc(unsigned int n) { return calloc(1, n); }
#define PoolFree  free
#define PoolDup(p, sz) memcpy(malloc(sz), (p), (sz))

static IMG *get_img(int idx);  /* forward — defined later */

/* Allocate a 40-byte PTTBL and attach it to the given image index. */
static void AddPointTable(int img_idx)
{
    IMG *img = (img_idx >= 0) ? get_img(img_idx) : NULL;
    if (!img || img->pttbl_p) return;
    img->pttbl_p = calloc(1, 40);
}

/* ---- ImgLoad: port of img_load (IMG file reader) ---- */
static void LoadImgFile(void)
{
    /* Open the file using the shim's fname_s in the current directory */
    FILE *f = fopen(fname_s, "rb");
    if (!f) return;

    LIB_HDR hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) { fclose(f); return; }

    /* Validate */
    if (hdr.temp != 0xABCD || hdr.version < 0x500) { fclose(f); return; }
    fileversion = hdr.version;

    if (hdr.imgcnt == 0 || hdr.imgcnt > 2000) { fclose(f); return; }

    ilpalloaded = -1;
    damcnt = 0;

    /* Calculate palette offset (OSET + imgcnt*sizeof(IMAGE)) */
    unsigned int pal_oset = (unsigned int)hdr.oset + (unsigned int)hdr.imgcnt * sizeof(IMAGE_disk);
    if (hdr.palcnt > NUMDEFPAL)
        pal_oset += (unsigned int)(hdr.palcnt - NUMDEFPAL) * sizeof(PALETTE_disk);

    /* Seek past sequences/scripts to point table area.
       Read sequence/script headers but discard the data (our UI
       does not use the legacy animation system). */
    {
        int seqcnt = (signed short)hdr.seqcnt;
        int scrcnt = (signed short)hdr.scrcnt;
        int is_far = (hdr.version >= 0x634);
        int entry_sz = is_far ? 16 : 14;
        int seqscr_sz = is_far ? 32 : 24;  /* sizeof SEQSCR */

        for (int s = 0; s < seqcnt; s++) {
            fseek(f, seqscr_sz - 2, SEEK_CUR);  /* past header except 'num' */
            unsigned short num;
            fread(&num, 1, 2, f);
            fseek(f, (long)num * entry_sz, SEEK_CUR);
        }
        for (int s = 0; s < scrcnt; s++) {
            fseek(f, seqscr_sz - 2, SEEK_CUR);
            unsigned short num;
            fread(&num, 1, 2, f);
            fseek(f, (long)num * entry_sz, SEEK_CUR);
        }
    }

    /* Get point table offset */
    long ptoset = ftell(f);

    /* Read images */
    int pal_base = (int)palcnt;
    unsigned int img_oset = hdr.oset;

    for (int i = 0; i < hdr.imgcnt; i++) {
        fseek(f, (long)img_oset, SEEK_SET);
        img_oset += sizeof(IMAGE_disk);

        IMAGE_disk idisk;
        if (fread(&idisk, 1, sizeof(idisk), f) != sizeof(idisk)) break;

        IMG *img = AllocImg();
        if (!img) break;

        /* Version-specific field mapping */
        if (hdr.version < 0x634) {
            unsigned short tmp = idisk.aniz2;
            idisk.aniz2 = idisk.frm;
            idisk.frm   = tmp;
            idisk.opals = (unsigned short)-1;
        }

        img->flags  = idisk.flags;
        img->anix   = idisk.anix;
        img->aniy   = idisk.aniy;
        img->w      = (idisk.w < 3) ? 3 : idisk.w;  /* min width 3 */
        img->h      = idisk.h;
        img->palnum = (unsigned short)((int)idisk.palnum - NUMDEFPAL + pal_base);
        img->anix2  = idisk.anix2;
        img->aniy2  = idisk.aniy2;
        img->aniz2  = idisk.aniz2;
        img->opals  = idisk.opals;
        img->pttbl_p = NULL;

        /* Debug fields */
        img->file_oset     = idisk.oset;
        img->file_lib      = idisk.lib;
        img->file_frm      = idisk.frm;
        img->file_pttblnum = idisk.pttblnum;

        strncpy(img->n_s, idisk.n_s, 15);
        img->n_s[15] = '\0';

        /* Read pixel data */
        unsigned int stride = ((unsigned int)img->w + 3) & ~3;
        unsigned int pix_sz = stride * img->h;
        img->data_p = PoolAlloc(pix_sz);
        if (!img->data_p) break;

        fseek(f, (long)idisk.oset, SEEK_SET);
        fread(img->data_p, 1, pix_sz, f);

        /* Read point table (version >= 0x60A) */
        if (hdr.version >= 0x60A && (signed short)idisk.pttblnum >= 0) {
            fseek(f, (long)(ptoset + (unsigned int)(signed short)idisk.pttblnum * 40), SEEK_SET);
            img->pttbl_p = PoolAlloc(40);
            if (img->pttbl_p) fread(img->pttbl_p, 1, 40, f);
        }
    }

    /* Read palettes */
    unsigned int pal_foffset = (unsigned int)hdr.oset + (unsigned int)hdr.imgcnt * sizeof(IMAGE_disk);
    int num_pals = (int)hdr.palcnt - NUMDEFPAL;
    if (num_pals < 0) num_pals = 0;

    for (int i = 0; i < num_pals; i++) {
        fseek(f, (long)pal_foffset, SEEK_SET);
        pal_foffset += sizeof(PALETTE_disk);

        PALETTE_disk pdisk;
        if (fread(&pdisk, 1, sizeof(pdisk), f) != sizeof(pdisk)) break;

        PAL *pal = AllocPal();
        if (!pal) break;

        pal->flags   = pdisk.flags;
        pal->bitspix = pdisk.bitspix;
        pal->numc    = pdisk.numc;
        pal->pad     = 0;

        strncpy(pal->n_s, pdisk.n_s, 9);
        pal->n_s[9] = '\0';

        /* Read color data (NUMC * 2 bytes of 15-bit packed RGB) */
        unsigned int col_sz = (unsigned int)pal->numc * 2;
        pal->data_p = PoolAlloc(col_sz);
        if (!pal->data_p) break;

        fseek(f, (long)pdisk.oset, SEEK_SET);
        fread(pal->data_p, 1, col_sz, f);
    }

    /* Skip old-format conversion message (not needed in modern UI) */
    fclose(f);

    /* Select first image */
    if (imgcnt > 0) ilselected = 0;
}

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
static int           g_img_tex_idx    = -2;  /* -2 = never built */

/* ---- Layout constants ---- */
static const float TOOLBAR_W   = 40.0f;
static const float PANEL_W     = 240.0f;
static const float PALETTE_H   = 110.0f;

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
static unsigned int g_last_saved_version = 0;

/* New IMG / Add Palette confirmations */
static bool g_show_new_img_confirm = false;

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

/* ---- Forward declarations for linked-list helpers ---- */
static IMG *get_img(int idx);
static PAL *get_pal(int idx);
static int count_imgs(void);
static int count_pals(void);
static void ApplyPalette(int pal_idx);
static void undo_push(void);

/* ---- Histogram state ---- */
static bool  g_show_histogram = false;
static float g_histogram_data[256] = {0};
static float g_histogram_max = 0.0f;
static int   g_histogram_img_count = 0;

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

/* Add a new blank 256-color palette.  Moved from imgtool_thunks.asm;
   uses the ASM memory pool via thunks (pal_alloc, mem_alloc). */
static void AddNewPalette(void)
{
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

/* ---- Write ANILST (Export Marked Images to Assembly) ---- */
/* Restore-from-source. The selected image is the "source" (un-chopped); every
   marked image is treated as a strip cut from it. For each strip pixel that
   is currently transparent (index 0) but whose corresponding source pixel
   isn't, copy the source pixel in. Restores artist-erased content (e.g. a
   logo scrubbed for copyright reasons after the chop) without touching pixels
   the strip already has its own data for.

   Mapping uses anipoints: source local (sx, sy) corresponds to strip local
   (sx - source.anix + strip.anix, sy - source.aniy + strip.aniy). Same
   palette is assumed (no recoloring). Returns the count of pixels written. */
static int RestoreMarkedFromSource(void)
{
    IMG *src = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) return 0;

    /* Sanity check: are there any marked targets that aren't the source itself? */
    bool any_target = false;
    for (IMG *t = (IMG *)img_p; t; t = (IMG *)t->nxt_p) {
        if ((t->flags & 1) && t != src) { any_target = true; break; }
    }
    if (!any_target) return 0;

    undo_push();

    const unsigned char *src_pix = (const unsigned char *)src->data_p;
    int src_stride = (src->w + 3) & ~3;
    int total_written = 0;

    for (IMG *t = (IMG *)img_p; t; t = (IMG *)t->nxt_p) {
        if (!(t->flags & 1) || t == src || !t->data_p || t->w == 0 || t->h == 0) continue;

        /* Skip if palette differs — restoring across palettes would mis-color. */
        if (t->palnum != src->palnum) continue;

        unsigned char *dst_pix = (unsigned char *)t->data_p;
        int dst_stride = (t->w + 3) & ~3;

        /* dx, dy are the src-local coords of the strip's (0,0). */
        int dx = (int)(short)t->anix - (int)(short)src->anix;
        int dy = (int)(short)t->aniy - (int)(short)src->aniy;

        for (int y = 0; y < t->h; y++) {
            int sy = y + dy;
            if (sy < 0 || sy >= src->h) continue;
            for (int x = 0; x < t->w; x++) {
                int sx = x + dx;
                if (sx < 0 || sx >= src->w) continue;
                unsigned char dst_p = dst_pix[y * dst_stride + x];
                if (dst_p != 0) continue;   /* strip already has data here */
                unsigned char src_p = src_pix[sy * src_stride + sx];
                if (src_p == 0) continue;    /* source is also transparent */
                dst_pix[y * dst_stride + x] = src_p;
                total_written++;
            }
        }
    }

    if (total_written > 0) g_img_tex_idx = -2;
    return total_written;
}

/* Status-message state for the next render after restore. */
static char  g_restore_msg[128] = {0};
static float g_restore_msg_timer = 0.0f;

static void WriteAnilstFromMarked(const char* filepath)
{
    FILE* f = fopen(filepath, "w");
    if (!f) return;

    fprintf(f, "\t.asg\t1,N\n");

    IMG* p = (IMG*)img_p;
    int aninum = 0;
    while (p) {
        if (p->flags & 1) {
            fprintf(f, "\t.word\tN,%d\t;%.15s\n", aninum, p->n_s);
            aninum++;
        }
        p = (IMG*)p->nxt_p;
    }
    fclose(f);
}

/* ---- Build TGA (Export Marked Images) ---- */
#pragma pack(push, 1)
struct TGA_HEADER {
    uint8_t  id_len;
    uint8_t  cm_type;
    uint8_t  i_type;
    uint16_t cm_first;
    uint16_t cm_length;
    uint8_t  cm_size;
    uint16_t x_origin;
    uint16_t y_origin;
    uint16_t width;
    uint16_t height;
    uint8_t  bpp;
    uint8_t  desc;
};
struct AFACE {
    uint32_t CTRL;
    uint32_t PAL;
    uint32_t O1;
    uint32_t O2;
    uint32_t O3;
    uint32_t O4;
    uint16_t AYX;
    uint16_t BYX;
    uint16_t CYX;
    uint16_t DYX;
    uint32_t LINE;
};
#pragma pack(pop)

static void BuildTgaFromMarked(const char* filepath)
{
    std::vector<IMG*> marked_imgs;
    IMG* p = (IMG*)img_p;
    int pal_num = -1;
    while (p) {
        if ((p->flags & 1) && p->w <= 256 && p->h > 0 && p->data_p) {
            marked_imgs.push_back(p);
            if (pal_num == -1) pal_num = p->palnum;
        }
        p = (IMG*)p->nxt_p;
    }
    if (marked_imgs.empty()) return;

    /* Sort by height descending to optimize packing */
    std::sort(marked_imgs.begin(), marked_imgs.end(), [](IMG* a, IMG* b) { return a->h > b->h; });

    const int MAX_LINES = 6000;
    std::vector<int> free_width(MAX_LINES, 256);
    std::vector<uint8_t> pixels(MAX_LINES * 256, 0);
    struct PackedImg { IMG* img; int x, y; };
    std::vector<PackedImg> packed;
    int max_y = 0;

    for (IMG* img : marked_imgs) {
        int w = img->w, h = img->h, best_y = -1, best_free_w = 0;
        for (int y = 0; y <= MAX_LINES - h; y++) {
            int min_free = 256;
            for (int dy = 0; dy < h; dy++) if (free_width[y + dy] < min_free) min_free = free_width[y + dy];
            if (min_free >= w) {
                bool ok = true;
                for (int dy = 0; dy < h; dy++) {
                    /* Replicate ASM heuristic to prevent leaving ugly ragged edges */
                    if (min_free < 254 && free_width[y + dy] >= min_free + 10) { ok = false; break; }
                }
                if (ok) { best_y = y; best_free_w = min_free; break; }
            }
        }
        if (best_y != -1) {
            int x = 256 - best_free_w;
            for (int dy = 0; dy < h; dy++) free_width[best_y + dy] -= w;
            packed.push_back({img, x, best_y});
            if (best_y + h > max_y) max_y = best_y + h;

            int stride = (w + 3) & ~3;
            unsigned char* src = (unsigned char*)img->data_p;
            for (int py = 0; py < h; py++) memcpy(&pixels[(best_y + py) * 256 + x], src + py * stride, w);
        }
    }
    if (max_y == 0) return;
    PAL* pal = get_pal(pal_num);
    if (!pal) return;

    /* Write TGA File (Bottom up format) */
    FILE* f = fopen(filepath, "wb");
    if (f) {
        TGA_HEADER tga = {0};
        tga.cm_type = 1; tga.i_type = 1; tga.cm_length = pal->numc;
        tga.cm_size = 15; tga.width = 256; tga.height = max_y; tga.bpp = 8;
        fwrite(&tga, 1, sizeof(tga), f);
        fwrite(pal->data_p, 2, pal->numc, f);
        for (int y = max_y - 1; y >= 0; y--) fwrite(&pixels[y * 256], 1, 256, f);
        fclose(f);
    }

    /* Write associated ANF manifest */
    std::string anf_path = filepath;
    size_t dot = anf_path.find_last_of('.');
    if (dot != std::string::npos) anf_path = anf_path.substr(0, dot);
    anf_path += ".ANF";
    f = fopen(anf_path.c_str(), "wb");
    if (f) {
        fwrite("ANF ", 1, 4, f);
        uint32_t fcnt = packed.size();
        fwrite(&fcnt, 1, 4, f);
        for (auto& pk : packed) {
            uint32_t zero = 0; fwrite(&zero, 1, 4, f);
            AFACE face = {0};
            face.CTRL = (pk.img->aniy << 16) | pk.img->anix;
            face.O2 = 1 * 3; face.O3 = 2 * 3; face.O4 = 3 * 3; face.LINE = pk.y;
            face.AYX = (uint16_t)(-pk.x);
            face.BYX = (uint16_t)(uint8_t)(pk.img->w - 1 - pk.x);
            face.CYX = (uint16_t)(((pk.img->h - 1) << 8) | (uint8_t)(pk.img->w - 1 - pk.x));
            face.DYX = (uint16_t)(((pk.img->h - 1) << 8) | (uint8_t)(-pk.x));
            fwrite(&face, 1, sizeof(face), f);
        }
        fclose(f);
    }
}

/* Save the selected image to a TGA (Truevision TARGA) file.
   Ported from savetga.  Uses fnametmp_s as the filename. */
static void SaveTga(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    PAL *pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;

    FILE *f = fopen(fnametmp_s, "wb");
    if (!f) return;

    TGA_HEADER hdr = {};
    hdr.cm_type   = 1;
    hdr.i_type    = 1;
    hdr.cm_length = pal->numc;
    hdr.cm_size   = 15;
    hdr.width     = img->w;
    hdr.height    = img->h;
    hdr.bpp       = 8;

    fwrite(&hdr, 1, sizeof(hdr), f);
    fwrite(pal->data_p, 2, pal->numc, f);

    /* Pixel rows: bottom-up (TGA convention) */
    unsigned short stride = (img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p
                                + (unsigned int)stride * (img->h - 1);
    for (int y = 0; y < img->h; y++) {
        fwrite(src, 1, img->w, f);
        src -= stride;
    }
    fclose(f);
}

/* Save the selected image to an IFF/ILBM (LBM) file.
   Ported from savelbm.  Uses fnametmp_s as the filename. */
static void SaveLbm(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return;

    PAL *pal = get_pal(img->palnum);
    if (!pal || !pal->data_p) return;

    FILE *f = fopen(fnametmp_s, "wb");
    if (!f) return;

    auto wbe32 = [&](unsigned int v) {
        unsigned char b[4] = { (unsigned char)(v>>24), (unsigned char)(v>>16),
                               (unsigned char)(v>>8),  (unsigned char)v };
        fwrite(b, 1, 4, f);
    };
    auto wbe16 = [&](unsigned short v) {
        unsigned char b[2] = { (unsigned char)(v>>8), (unsigned char)v };
        fwrite(b, 1, 2, f);
    };

    unsigned short row_bytes = (unsigned short)((img->w + 1) & ~1);
    unsigned int body_len = (unsigned int)row_bytes * img->h;

    fwrite("FORM", 1, 4, f);
    wbe32(0);
    fwrite("PBM ", 1, 4, f);

    fwrite("BMHD", 1, 4, f);
    wbe32(20);
    wbe16(img->w); wbe16(img->h);
    wbe16(0); wbe16(0);
    fputc(8, f); fputc(0, f); fputc(0, f); fputc(0, f);
    wbe16(0);
    fputc(5, f); fputc(6, f);
    wbe16(img->w); wbe16(img->h);

    fwrite("CMAP", 1, 4, f);
    wbe32(256 * 3);
    const unsigned char *pal_data = (const unsigned char *)pal->data_p;
    for (int i = 0; i < 256; i++) {
        unsigned short w = (unsigned short)(pal_data[i*2] | (pal_data[i*2+1] << 8));
        fputc((w >> 7) & 0xF8, f);
        fputc((w >> 2) & 0xF8, f);
        fputc((w << 3) & 0xF8, f);
    }

    fwrite("BODY", 1, 4, f);
    wbe32(body_len);
    const unsigned char *src = (const unsigned char *)img->data_p;
    unsigned short stride = (img->w + 3) & ~3;
    for (int y = 0; y < img->h; y++) {
        fwrite(src, 1, row_bytes, f);
        src += stride;
    }

    long end_pos = ftell(f);
    fseek(f, 4, SEEK_SET);
    wbe32((unsigned int)(end_pos - 8));
    fclose(f);
}

/* Load a TGA (Truevision TARGA) file as a new image+palette.
   Ported from loadtga.  Uses fnametmp_s as the filename. */
static void LoadTga(void)
{
    FILE *f = fopen(fnametmp_s, "rb");
    if (!f) return;

    TGA_HEADER hdr;
    if (fread(&hdr, 1, sizeof(hdr), f) != sizeof(hdr)) goto err;

    /* Skip ID field */
    if (hdr.id_len > 0) fseek(f, hdr.id_len, SEEK_CUR);

    /* Validate: must be uncompressed colour-mapped 8-bit */
    if (hdr.i_type != 1 || hdr.cm_type != 1 || hdr.bpp != 8) goto err;
    if (hdr.cm_size != 15 && hdr.cm_size != 16 && hdr.cm_size != 24) goto err;

    /* Allocate image */
    IMG *img = (IMG *)AllocImg();
    if (!img) goto err;

    img->w = hdr.width;
    img->h = hdr.height;
    if (img->w == 0 || img->h == 0) goto err;

    unsigned short stride = (img->w + 3) & ~3;
    unsigned int   pix_sz = (unsigned int)stride * img->h;
    img->data_p = (unsigned char *)PoolAlloc(pix_sz);
    if (!img->data_p) goto err;

    img->palnum = (unsigned short)palcnt;
    img->flags  = 0;
    img->anix   = 0; img->aniy  = 0;
    img->anix2  = 0; img->aniy2 = 0; img->aniz2 = 0;
    img->pttbl_p = NULL;
    img->opals  = (unsigned short)-1;

    /* Copy filename as image name, strip extension */
    {
        std::string name = fnametmp_s;
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        strncpy(img->n_s, name.c_str(), 15);
        img->n_s[15] = '\0';
    }

    /* Read palette — convert to 15-bit packed RGB */
    unsigned short num_colors = hdr.cm_length;
    if (num_colors == 0) num_colors = 256;

    unsigned char *pal_buf = (unsigned char *)PoolAlloc((unsigned int)num_colors * 2);
    if (!pal_buf) goto err;

    for (int i = 0; i < num_colors; i++) {
        int r, g, b;
        if (hdr.cm_size == 24) {
            unsigned char rgb[3];
            if (fread(rgb, 1, 3, f) != 3) { free(pal_buf); goto err; }
            b = rgb[0]; g = rgb[1]; r = rgb[2];
        } else {
            unsigned char w2[2];
            if (fread(w2, 1, 2, f) != 2) { free(pal_buf); goto err; }
            unsigned short w = (unsigned short)(w2[0] | (w2[1] << 8));
            if (hdr.cm_size == 15) {
                r = (w >> 7) & 0xF8;   /* ARRRRRGG GGGBBBBB → 15-bit */
                g = (w >> 2) & 0xF8;
                b = (w << 3) & 0xF8;
            } else { /* 16-bit */
                r = (w >> 8) & 0xF8;
                g = (w >> 3) & 0xF8;
                b = (w << 3) & 0xF8;
            }
        }
        unsigned short r5 = (unsigned short)(r >> 3);
        unsigned short g5 = (unsigned short)(g >> 3);
        unsigned short b5 = (unsigned short)(b >> 3);
        unsigned short p15 = (unsigned short)((r5 << 10) | (g5 << 5) | b5);
        pal_buf[i * 2]     = (unsigned char)(p15 & 0xFF);
        pal_buf[i * 2 + 1] = (unsigned char)(p15 >> 8);
    }

    /* Allocate palette */
    PAL *pal = (PAL *)AllocPal();
    if (!pal) { free(pal_buf); goto err; }

    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = num_colors;
    pal->data_p  = pal_buf;
    pal->pad     = 0;
    {
        std::string name = fnametmp_s;
        size_t dot = name.find_last_of('.');
        if (dot != std::string::npos) name = name.substr(0, dot);
        name += "P";
        strncpy(pal->n_s, name.c_str(), 9);
        pal->n_s[9] = '\0';
    }

    /* Read pixel data */
    unsigned char *dst = (unsigned char *)img->data_p;
    if (hdr.desc & 0x10) {
        /* Top-down */
        for (int y = 0; y < img->h; y++) {
            if (fread(dst, 1, img->w, f) != (size_t)img->w) goto err;
            dst += stride;
        }
    } else {
        /* Bottom-up (TGA default) */
        dst += (unsigned int)stride * (img->h - 1);
        for (int y = 0; y < img->h; y++) {
            if (fread(dst, 1, img->w, f) != (size_t)img->w) goto err;
            dst -= stride;
        }
    }

    fclose(f);

    /* Select the newly loaded image */
    if (imgcnt > 0) ilselected = (int)imgcnt - 1;
    return;

err:
    if (f) fclose(f);
}

/* Load an IFF/ILBM (LBM) file as a new image+palette.
   Ported from loadlbm.  Uses fnametmp_s as the filename. */
static void LoadLbm(void)
{
    FILE *f = fopen(fnametmp_s, "rb");
    if (!f) return;

    auto rbe32 = [&](unsigned int *out) -> bool {
        unsigned char b[4];
        if (fread(b, 1, 4, f) != 4) return false;
        *out = ((unsigned int)b[0]<<24)|((unsigned int)b[1]<<16)|((unsigned int)b[2]<<8)|(unsigned int)b[3];
        return true;
    };
    auto rbe16 = [&](unsigned short *out) -> bool {
        unsigned char b[2];
        if (fread(b, 1, 2, f) != 2) return false;
        *out = (unsigned short)((b[0]<<8)|b[1]);
        return true;
    };
    auto try_close = [&] { if (f) { fclose(f); f = NULL; } };

    enum { TAG_FORM=0x4D524F46, TAG_ILBM=0x4D424C49, TAG_PBM =0x204D4250,
           TAG_BMHD=0x44484D42, TAG_CMAP=0x50414D43, TAG_BODY=0x59444F42,
           TAG_ANIM=0x4D494E41 };

    unsigned int tag, form_len;
    if (!rbe32(&tag) || tag != TAG_FORM) { try_close(); return; }
    if (!rbe32(&form_len)) { try_close(); return; }
    if (!rbe32(&tag) || (tag != TAG_PBM && tag != TAG_ILBM && tag != TAG_ANIM)) { try_close(); return; }

    int have_bmhd = 0;
    unsigned short bm_w = 0, bm_h = 0;
    unsigned char  bm_comp = 0;
    PAL *loaded_pal = NULL;
    IMG *loaded_img = NULL;

    for (;;) {
        long pos = ftell(f);
        if (pos & 1) fseek(f, 1, SEEK_CUR);

        unsigned int chunk_tag = 0, chunk_len = 0;
        if (fread(&chunk_tag, 1, 4, f) != 4) break;
        if (!rbe32(&chunk_len)) break;

        if (chunk_tag == TAG_FORM) { fseek(f, 4, SEEK_CUR); continue; }

        if (chunk_tag == TAG_BMHD) {
            unsigned short w, h, xo, yo, tcol, pagew, pageh;
            unsigned char  nplanes, masking, comp, pad1, xasp, yasp;
            if (!rbe16(&w)||!rbe16(&h)||!rbe16(&xo)||!rbe16(&yo)) { try_close(); return; }
            nplanes=(unsigned char)fgetc(f); masking=(unsigned char)fgetc(f);
            comp   =(unsigned char)fgetc(f); pad1   =(unsigned char)fgetc(f);
            if (!rbe16(&tcol)) { try_close(); return; }
            xasp=(unsigned char)fgetc(f); yasp=(unsigned char)fgetc(f);
            if (!rbe16(&pagew)||!rbe16(&pageh)) { try_close(); return; }
            bm_w = w; bm_h = h; bm_comp = comp;
            have_bmhd |= 1;
            continue;
        }

        if (chunk_tag == TAG_CMAP) {
            unsigned int num_colors = chunk_len / 3;
            if (num_colors > 256) { try_close(); return; }

            loaded_pal = (PAL *)AllocPal();
            if (!loaded_pal) { try_close(); return; }
            loaded_pal->flags = 0; loaded_pal->bitspix = 8;
            loaded_pal->numc = (unsigned short)num_colors; loaded_pal->pad = 0;

            unsigned char *pal_buf = (unsigned char *)PoolAlloc(num_colors * 2);
            if (!pal_buf) { try_close(); return; }
            loaded_pal->data_p = pal_buf;

            for (unsigned int i = 0; i < num_colors; i++) {
                int r = fgetc(f), g = fgetc(f), b = fgetc(f);
                if (r<0||g<0||b<0) { try_close(); return; }
                unsigned short r5=(unsigned short)(r>>3), g5=(unsigned short)(g>>3), b5=(unsigned short)(b>>3);
                unsigned short w15 = (unsigned short)((r5<<10)|(g5<<5)|b5);
                pal_buf[i*2]=(unsigned char)(w15&0xFF); pal_buf[i*2+1]=(unsigned char)(w15>>8);
            }
            { std::string n=fnametmp_s; size_t d=n.find_last_of('.'); if(d!=std::string::npos)n=n.substr(0,d); n+="P";
              strncpy(loaded_pal->n_s,n.c_str(),9); loaded_pal->n_s[9]='\0'; }
            have_bmhd |= 2;
            continue;
        }

        if (chunk_tag == TAG_BODY) {
            if (have_bmhd != 3) { try_close(); return; }

            loaded_img = (IMG *)AllocImg();
            if (!loaded_img) { try_close(); return; }
            loaded_img->w=bm_w; loaded_img->h=bm_h;
            if (!bm_w||!bm_h) { try_close(); return; }

            unsigned short stride=(bm_w+3)&~3;
            loaded_img->data_p=PoolAlloc((unsigned)stride*bm_h);
            if (!loaded_img->data_p) { try_close(); return; }

            loaded_img->palnum=(unsigned short)(palcnt-1); loaded_img->flags=0;
            loaded_img->anix=0; loaded_img->aniy=0; loaded_img->anix2=0; loaded_img->aniy2=0; loaded_img->aniz2=0;
            loaded_img->pttbl_p=NULL; loaded_img->opals=(unsigned short)-1;
            { std::string n=fnametmp_s; size_t d=n.find_last_of('.'); if(d!=std::string::npos)n=n.substr(0,d);
              strncpy(loaded_img->n_s,n.c_str(),15); loaded_img->n_s[15]='\0'; }

            unsigned short even_w=(unsigned short)((bm_w+1)&~1);
            if (bm_comp != 0) {
                /* RLE decompression: literal runs and repeat runs per row */
                unsigned char *dst=(unsigned char*)loaded_img->data_p;
                for (int y=0; y<bm_h; y++) {
                    int remaining=even_w;
                    while (remaining>0) {
                        int b1=fgetc(f); if(b1<0){try_close();return;}
                        signed char sc=(signed char)(unsigned char)b1;
                        if (sc>=0) {
                            int n=sc+1; if(fread(dst,1,(size_t)n,f)!=(size_t)n){try_close();return;}
                            dst+=n; remaining-=n;
                        } else {
                            int rb=fgetc(f); if(rb<0){try_close();return;}
                            int n=(-sc)+1; memset(dst,rb,(size_t)n); dst+=n; remaining-=n;
                        }
                    }
                    for (int x=bm_w; x<stride; x++) dst[x]=0;
                    dst+=stride;
                }
            } else {
                unsigned char *dst=(unsigned char*)loaded_img->data_p;
                for (int y=0; y<bm_h; y++) {
                    if (fread(dst,1,even_w,f)!=even_w) { try_close(); return; }
                    for (int x=bm_w; x<stride; x++) dst[x]=0;
                    dst+=stride;
                }
            }
            break;
        }

        /* Skip unknown chunks */
        fseek(f, (long)chunk_len, SEEK_CUR);
    }

    try_close();
    if (loaded_img && imgcnt>0) ilselected=(int)imgcnt-1;
}

/* ---- ImGui Native File Dialog ---- */
enum class FileDialogMode { OpenImg, AppendImg, SaveImg, ExportTga, LoadLbm, SaveLbm, SaveMarkedLbm, LoadTga, SaveTga, WriteAniLst };
static bool g_show_file_dialog = false;
static FileDialogMode g_file_dialog_mode = FileDialogMode::OpenImg;
static char g_file_dialog_dir[1024] = "";
static char g_file_dialog_file[256] = "";

struct FileEntry { std::string name; bool is_dir; };

static void GetDirectoryFiles(const std::string& dir, std::vector<FileEntry>& entries)
{
    entries.clear();
#ifdef _WIN32
    WIN32_FIND_DATAA fd;
    std::string search = dir;
    if (!search.empty() && search.back() != '\\' && search.back() != '/') search += "\\";
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
            entries.push_back({dir_ent->d_name, is_dir});
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

    ClearAll();
    LoadImgFile();
    g_last_saved_version = fileversion; /* fresh load = clean baseline */
    g_img_tex_idx = -2;
    RecentAdd(full_path);
}

static void OpenFileDialog(FileDialogMode mode) {
    if (g_file_dialog_dir[0] == '\0') {
#ifdef _WIN32
        GetCurrentDirectoryA(sizeof(g_file_dialog_dir), g_file_dialog_dir);
#else
        if (getcwd(g_file_dialog_dir, sizeof(g_file_dialog_dir)) == NULL)
            g_file_dialog_dir[0] = '\0';
#endif
    }
    g_file_dialog_file[0] = '\0';
    g_file_dialog_mode = mode;
    g_show_file_dialog = true;
}

static void DrawFileDialog() {
    const char* title = "Open File";
    if (g_file_dialog_mode == FileDialogMode::SaveImg) title = "Save IMG File";
    else if (g_file_dialog_mode == FileDialogMode::ExportTga) title = "Export TGA";
    else if (g_file_dialog_mode == FileDialogMode::OpenImg) title = "Open IMG File";
    else if (g_file_dialog_mode == FileDialogMode::AppendImg) title = "Append IMG File";
    else if (g_file_dialog_mode == FileDialogMode::LoadLbm) title = "Load LBM File";
    else if (g_file_dialog_mode == FileDialogMode::SaveLbm) title = "Save LBM File";
    else if (g_file_dialog_mode == FileDialogMode::SaveMarkedLbm) title = "Save Marked LBM";
    else if (g_file_dialog_mode == FileDialogMode::LoadTga) title = "Load TGA File";
    else if (g_file_dialog_mode == FileDialogMode::SaveTga) title = "Save TGA File";
    else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) title = "Write ANILST";

    if (g_show_file_dialog) ImGui::OpenPopup(title);
    
    ImGui::SetNextWindowSize(ImVec2(600, 400), ImGuiCond_FirstUseEver);
    if (ImGui::BeginPopupModal(title, &g_show_file_dialog, ImGuiWindowFlags_NoSavedSettings)) {
        
        ImGui::InputText("Directory", g_file_dialog_dir, sizeof(g_file_dialog_dir));
        ImGui::Separator();
        
        ImGui::BeginChild("##file_list", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 2), true);
        
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
        GetDirectoryFiles(current_dir, entries);
        
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
        
        ImGui::InputText("File Name", g_file_dialog_file, sizeof(g_file_dialog_file));
        ImGui::SameLine();
        
        const char* btn_text = (g_file_dialog_mode == FileDialogMode::OpenImg ||
                                g_file_dialog_mode == FileDialogMode::AppendImg ||
                                g_file_dialog_mode == FileDialogMode::LoadLbm ||
                                g_file_dialog_mode == FileDialogMode::LoadTga) ? "Open" : "Save";
        if (ImGui::Button(btn_text, ImVec2(100, 0))) {
            std::string full_path = PathCombine(g_file_dialog_dir, g_file_dialog_file);

            if (g_file_dialog_mode == FileDialogMode::ExportTga) {
                size_t dot = full_path.find_last_of('.');
                if (dot != std::string::npos) full_path = full_path.substr(0, dot);
                full_path += ".TGA";
                BuildTgaFromMarked(full_path.c_str());
            } else if (g_file_dialog_mode == FileDialogMode::WriteAniLst) {
                size_t dot = full_path.find_last_of('.');
                if (dot == std::string::npos) full_path += ".ASM";
                WriteAnilstFromMarked(full_path.c_str());
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
                    img_save();
                    g_last_saved_version = fileversion; /* Mark as saved in C++ state */
                    RecentAdd(full_path);
                } else if (g_file_dialog_mode == FileDialogMode::OpenImg) {
                    ClearAll();
                    LoadImgFile();
                    g_last_saved_version = fileversion; /* fresh load = clean baseline */
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
    "IMAGE TOOL HELP\n\n"
    "Escape - Aborts a function           Enter - Accepts a function\n"
    "h - Shows this help                  f - Redraws screen\n\n"
    "Ctrl+O/S - IMG load/save             m - clear all marks\n"
    "Alt+l/s - LBM load/save              M - set all marks\n"
    "Ctrl+l/s - TGA load/save             Space - mark/unmark image\n\n"
    "Ctrl+B - Build TGA from marked       Ctrl+D - Delete image\n"
    "; - Least-squares size reduce        Ctrl+R - Rename image\n\n"
    "D - Halve view size                  d - Double view size\n"
    "F11 - Decrease view size             F12 - Increase view size\n\n"
    "Tab - Swap image lists               i - Set ID from 2nd list\n"
    "t - Show true palette colors         T - Toggle anim points\n\n"
    "' / - Move up/down in palette list   \" ? - Page up/down palette\n"
    "] - Set palette for image            [ - Set palette for marked\n"
    "* - Merge marked palettes            Shift+R - Rename palette\n\n"
    "Up/Dn - Move in image list           PgUp/Dn - Page up/down\n"
    "Alt U/D/L/R - Move anim point        Ctrl U/D/L/R - Move 2nd anim point\n"
    "Alt PgUp/Dn - Move image in list\n\n"
    "Ctrl+Del - Clear 2nd anim XYZ        Ctrl+Y - Clear 2nd anim Y\n"
    "Ctrl+Z - Clear 2nd anim Z            Ctrl+P - Point table change\n"
    "Alt+C - Clear extra data of all      F7/8 - Add/sub line from top\n"
    "Shift - Quarter scroll sensitivity";

/* ---- Linked list helpers ---- */
static IMG *get_img(int idx)
{
    IMG *p = (IMG *)img_p;
    for (int i = 0; p && i < idx; i++) p = (IMG *)p->nxt_p;
    return p;
}

static PAL *get_pal(int idx)
{
    PAL *p = (PAL *)pal_p;
    for (int i = 0; p && i < idx; i++) p = (PAL *)p->nxt_p;
    return p;
}

static int count_imgs(void)
{
    int n = 0;
    for (IMG *p = (IMG *)img_p; p; p = (IMG *)p->nxt_p) n++;
    return n;
}

static int count_pals(void)
{
    int n = 0;
    for (PAL *p = (PAL *)pal_p; p; p = (PAL *)p->nxt_p) n++;
    return n;
}

/* ---- Palette persistence ----
   The asm stores palette colors as packed 15-bit RGB words in pal->data_p
   (2 bytes per color). Edits to g_palette[] are written back so subsequent
   Save persists them. */
static void palette_writeback(int color_idx)
{
    PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (color_idx < 0 || color_idx >= (int)pal->numc) return;

    SDL_Color &c = g_palette[color_idx];
    rgb8_to_pal_word(c.r, c.g, c.b, (unsigned char *)pal->data_p + color_idx * 2);
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
static void undo_push(void)
{
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
    g_last_saved_version = fileversion;

    RecentLoad();

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    ImGui::StyleColorsDark();
    ImGuiStyle &style = ImGui::GetStyle();
    style.WindowPadding    = ImVec2(4, 4);
    style.FramePadding     = ImVec2(4, 3);
    style.ItemSpacing      = ImVec2(4, 3);
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
#else
        snprintf(fontpath, sizeof(fontpath), "assets/MaterialSymbolsSharp-Regular.ttf");
#endif
        /* Material Symbols PUA range — covers all icon glyphs we use. */
        static const ImWchar icon_ranges[] = { 0xE000, 0xF8FF, 0 };
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.PixelSnapH = true;
        cfg.GlyphMinAdvanceX = 16.0f;
        cfg.GlyphOffset = ImVec2(0, 3.0f);  /* Material glyphs sit high — nudge down */
        ImFont *icons = io.Fonts->AddFontFromFileTTF(fontpath, 16.0f, &cfg, icon_ranges);
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
    /* User opted out of unsaved-changes prompts — just exit. */
    (void)g_last_saved_version;
    return 1;
}

void imgui_overlay_mark_saved(void)
{
    g_last_saved_version = fileversion;
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
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route)) { if (g_undo_idx > 0) { g_undo_idx--; undo_apply(g_undo_idx); } }
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
    if (ImGui::Shortcut(ImGuiKey_Semicolon, route)) LeastSquaresReduceMarked();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, route)) OpenFileDialog(FileDialogMode::ExportTga);

    /* ---- Menu bar ---- */
    if (ImGui::BeginMainMenuBar()) {
        ImGui::Spacing();  /* space before File */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("New"))             g_show_new_img_confirm = true;
            if (ImGui::MenuItem("Open...",  "Ctrl+O")) OpenFileDialog(FileDialogMode::OpenImg);
            if (ImGui::BeginMenu("Open Recent", !g_recent_files.empty())) {
                /* Snapshot since OpenImgFile mutates g_recent_files. */
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
            if (ImGui::MenuItem("Save",     "Ctrl+S")) OpenFileDialog(FileDialogMode::SaveImg);
            if (ImGui::MenuItem("Append",   "a"))   OpenFileDialog(FileDialogMode::AppendImg);
            ImGui::Separator();
            if (ImGui::MenuItem("Load LBM", "Alt+L"))        OpenFileDialog(FileDialogMode::LoadLbm);
            if (ImGui::MenuItem("Save LBM", "Alt+S"))        OpenFileDialog(FileDialogMode::SaveLbm);
            if (ImGui::MenuItem("Save Marked LBM"))          OpenFileDialog(FileDialogMode::SaveMarkedLbm);
            if (ImGui::MenuItem("Load TGA", "Ctrl+L"))       OpenFileDialog(FileDialogMode::LoadTga);
            if (ImGui::MenuItem("Save TGA", "Ctrl+S"))       OpenFileDialog(FileDialogMode::SaveTga);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) ExitProcess(0);
            ImGui::EndMenu();
        }
        ImGui::Spacing();  /* space between File and Edit */
        if (ImGui::BeginMenu("Edit")) {
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
            if (ImGui::MenuItem("Rename Image",  "Ctrl+R"))  OpenRenameImage();
            if (ImGui::MenuItem("Delete Image",  "Ctrl+D"))  DeleteImage(ilselected);
            if (ImGui::MenuItem("Duplicate"))                DuplicateImage();
            if (ImGui::MenuItem("Build TGA",     "Ctrl+B"))  OpenFileDialog(FileDialogMode::ExportTga);
            ImGui::EndMenu();
        }
        ImGui::Spacing();  /* space between Edit and Image */
        if (ImGui::BeginMenu("Image")) {
            if (ImGui::MenuItem("Mark / Unmark",            "Space"))        { IMG *img = get_img(ilselected); if (img) img->flags ^= 1; }
            if (ImGui::MenuItem("Set All Marks",            "M"))            { IMG *p=(IMG*)img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Clear All Marks",          "m"))            { IMG *p=(IMG*)img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Invert All Marks")) {
                IMG *p=(IMG*)img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;}
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Jump to Prev Marked",      "Left")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (ilselected - i + n_imgs) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Jump to Next Marked",      "Right")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (ilselected + i) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Move Image Up in List",    "Alt+PgUp"))     MoveImageUp();
            if (ImGui::MenuItem("Move Image Down in List",  "Alt+PgDn"))     MoveImageDown();
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Image",             "Ctrl+R"))       OpenRenameImage();
            if (ImGui::MenuItem("Add/Del Point Table",      "Ctrl+P"))       TogglePointTable();
            if (ImGui::MenuItem("Set ID from 2nd List",     "i"))            SetIDFromSecondList();
            if (ImGui::MenuItem("Least-Squares Reduce",     ";"))            LeastSquaresReduceMarked();
            if (ImGui::MenuItem("Clear Extra Data",         "Alt+C"))        ClearExtraData();
            ImGui::Separator();
            if (ImGui::MenuItem("Switch Image List",        "Tab"))          SwitchImageList();
            ImGui::Separator();
            if (ImGui::BeginMenu("Marked Images")) {
                if (ImGui::MenuItem("Rename Marked"))               OpenRenameMarkedImages();
                if (ImGui::MenuItem("Delete Marked"))               DeleteMarkedImages();
                ImGui::Separator();
                if (ImGui::MenuItem("Set Palette",     "["))        SetPaletteOfMarked();
                ImGui::Separator();
                if (ImGui::MenuItem("Strip Edge"))                  StripMarkedImages(5);
                if (ImGui::MenuItem("Strip Edge Low"))              StripMarkedImages(3);
                if (ImGui::MenuItem("Strip Edge (Selected Color)")) StripMarkedImages(5, g_sel_color);
                ImGui::Separator();
                if (ImGui::MenuItem("Least Squares",   ";"))        LeastSquaresReduceMarked();
                if (ImGui::MenuItem("Dither Replace"))              DitherReplaceMarkedImages(g_sel_color);
                ImGui::Separator();
                if (ImGui::MenuItem("Restore from Selected (pixel-diff)")) {
                    int n = RestoreMarkedFromSource();
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             n > 0 ? "Restored %d pixel(s) from selected source."
                                   : "No pixels restored. Check selection, marks, palettes, anipoints.",
                             n);
                    g_restore_msg_timer = 4.0f;  /* seconds */
                }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "For each marked image, copy non-transparent source pixels\n"
                    "into transparent strip pixels. Selected image = source.\n"
                    "Uses anipoints to align; same palette required.");
                ImGui::Separator();
                if (ImGui::MenuItem("Build TGA",       "Ctrl+B"))   OpenFileDialog(FileDialogMode::ExportTga);
                if (ImGui::MenuItem("Write ANILST"))                OpenFileDialog(FileDialogMode::WriteAniLst);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        ImGui::Spacing();  /* space between Image and Palette */
        if (ImGui::BeginMenu("Palette")) {
            if (ImGui::MenuItem("Set Palette for Image",      "]"))       SetPaletteOfSelected();
            if (ImGui::MenuItem("Set Palette for Marked",     "["))       SetPaletteOfMarked();
            ImGui::Separator();
            if (ImGui::MenuItem("Merge Marked into Selected", "*"))       MergeMarkedPalettes();
            if (ImGui::MenuItem("Delete Palette",             "Del"))     DeletePalette();
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Palette",             "Shift+R")) OpenRenamePalette(plselected);
            ImGui::Separator();
            if (ImGui::MenuItem("Show Histogram")) { CalculatePaletteHistogram(); g_show_histogram = true; }
            if (ImGui::MenuItem("Delete Unused Colors"))                  DeleteUnusedPaletteColors();
            ImGui::Separator();
            if (ImGui::MenuItem("Mark All Palettes")) {
                PAL *p=(PAL*)pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Clear All Palette Marks")) {
                PAL *p=(PAL*)pal_p; while(p){p->flags&=~1;p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Invert Palette Marks")) {
                PAL *p=(PAL*)pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;}
            }
            ImGui::EndMenu();
        }
        ImGui::Spacing();  /* space between Palette and View */
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Anim Points", NULL, &g_show_points);
            ImGui::MenuItem("Hitboxes",    NULL, &g_show_hitbox);
            ImGui::MenuItem("DMA Compression", NULL, &g_show_dma_comp);
            ImGui::EndMenu();
        }
        ImGui::Spacing();  /* space between View and Programming */
        if (ImGui::BeginMenu("Programming")) {
            if (ImGui::MenuItem("Write ANILST...")) OpenFileDialog(FileDialogMode::WriteAniLst);
            ImGui::EndMenu();
        }
        ImGui::Spacing();  /* space between Programming and Help */
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Show Help", "h")) g_show_help = true;
            if (ImGui::MenuItem("Debug Info", "F9")) g_show_debug = !g_show_debug;
            ImGui::Separator();
            if (ImGui::MenuItem("About...")) g_show_about = true;
            ImGui::EndMenu();
        }
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
    }

    if (plselected != last_plselected || pal_p != last_pal_p) {
        last_plselected = plselected;
        last_pal_p = pal_p;
        ApplyPalette(plselected);
        g_img_tex_idx = -2; /* Force texture rebuild to use new palette */
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
        ImVec2 btn(TOOLBAR_W - 8, TOOLBAR_W - 8);
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
            if (ImGui::SmallButton("Del"))       DeletePalette();
        }

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img) {
                ImGui::Text("Name:        %.15s", img->n_s);
                ImGui::Text("Size:        %d x %d", img->w, img->h);
                
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
                    ImGui::Text("DMA ROM:     %d B raw / %d B comp", uncomp_size, comp_size);
                }

                PAL *pal = get_pal(img->palnum);
                if (pal) ImGui::Text("Pal:         %d  %.9s", img->palnum, pal->n_s);
                else     ImGui::Text("Pal:         %d", img->palnum);

                ImGui::Text("AX/AY:       %d, %d", img->anix, img->aniy);
                ImGui::Text("AX2/AY2:     %d, %d", img->anix2, img->aniy2);
                ImGui::Text("AZ2:         %d", img->aniz2);

                char flagbuf[48] = {};
                if (img->flags & 1)  strncat(flagbuf, "Marked ", 47);
                if (img->flags & 2)  strncat(flagbuf, "Loaded ", 47);
                if (img->flags & 4)  strncat(flagbuf, "Changed ", 47);
                if (img->flags & 8)  strncat(flagbuf, "Delete ", 47);
                if (!flagbuf[0])     strncpy(flagbuf, "-", 47);
                ImGui::Text("Flags:       0x%04X  %s", img->flags, flagbuf);

                ImGui::Text("DATA:        0x%08X", (unsigned)(uintptr_t)img->data_p);

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
                int ax = img->anix, ay = img->aniy;
                int ax2 = img->anix2, ay2 = img->aniy2;
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("X1##ptx",  &ax,  0, 639)) { undo_push(); img->anix  = (unsigned short)ax;  }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("Y1##pty",  &ay,  0, 399)) { undo_push(); img->aniy  = (unsigned short)ay;  }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("X2##ptx2", &ax2, 0, 639)) { undo_push(); img->anix2 = (unsigned short)ax2; }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::SliderInt("Y2##pty2", &ay2, 0, 399)) { undo_push(); img->aniy2 = (unsigned short)ay2; }
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Hitbox Editor --- */
        if (ImGui::CollapsingHeader("Hitbox")) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("X##hbx",  &g_hitbox_x, 0, 639)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("Y##hby",  &g_hitbox_y, 0, 399)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("W##hbw",  &g_hitbox_w, 1, 640)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("H##hbh",  &g_hitbox_h, 1, 400)) undo_push();
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

        if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            float tw = avail.x, th = (float)g_img_tex_h * (tw / (float)g_img_tex_w);
            if (th > avail.y) { th = avail.y; tw = (float)g_img_tex_w * (th / (float)g_img_tex_h); }
            float scale = (float)(int)(tw / (float)g_img_tex_w);
            if (scale < 1.0f) scale = 1.0f;
            tw = (float)g_img_tex_w * scale;
            th = (float)g_img_tex_h * scale;
            float off_x = (avail.x - tw) * 0.5f;
            float off_y = (avail.y - th) * 0.5f;
            if (off_x > 0) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off_x);
            if (off_y > 0) ImGui::SetCursorPosY(ImGui::GetCursorPosY() + off_y);

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
        } else {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.45f);
            float tw = ImGui::CalcTextSize("No image selected").x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - tw) * 0.5f);
            ImGui::TextDisabled("No image selected");
        }

        ImVec2 mouse = io.MousePos;
        bool   mbdn  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

        /* Set when an overlay widget (anim point, hitbox corner) eats this frame's
           click, so the grid-selection block below doesn't also start a selection. */
        bool widget_consumed_click = false;

        /* --- Anim point overlay + dragging --- */
        if (g_show_points) {
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
                    if (nx < 0) nx = 0; if (nx > 639) nx = 639;
                    if (ny < 0) ny = 0; if (ny > 399) ny = 399;
                    img->anix = (unsigned short)nx;
                    img->aniy = (unsigned short)ny;
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
                        if (nx < 0) nx = 0; if (nx > 639) nx = 639;
                        if (ny < 0) ny = 0; if (ny > 399) ny = 399;
                        img->anix2 = (unsigned short)nx;
                        img->aniy2 = (unsigned short)ny;
                    } else if (!mbdn && drag2) { drag2 = false; undo_push(); }
                }
            }
        }

        /* --- Hitbox overlay + corner dragging --- */
        if (g_show_hitbox) {
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
                if (mx < 0) mx = 0; if (mx > 639) mx = 639;
                if (my < 0) my = 0; if (my > 399) my = 399;
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

            /* Draw paste boundary overlay (when paste is active) */
            if (g_pasted.active && g_clipboard.valid && g_clipboard.w > 0 && g_clipboard.h > 0) {
                int px = g_pasted.paste_x;
                int py = g_pasted.paste_y;
                int pw = g_clipboard.w;
                int ph = g_clipboard.h;

                /* Check if mouse is over the paste boundary for dragging */
                ImVec2 p1(img_pos.x + px * sx, img_pos.y + py * sy);
                ImVec2 p2(img_pos.x + (px + pw) * sx, img_pos.y + (py + ph) * sy);
                bool hovering = mouse.x >= p1.x && mouse.x < p2.x && mouse.y >= p1.y && mouse.y < p2.y;

                /* Draw border */
                ImU32 border_col = hovering ? IM_COL32(255, 200, 0, 255) : IM_COL32(255, 255, 0, 255);
                dl->AddRect(p1, p2, border_col, 0.0f, 0, 3.0f);

                /* Draw paste instruction text */
                if (g_pasted.dragging) {
                    dl->AddText(ImVec2(img_pos.x + 10, img_pos.y + 10), IM_COL32(255, 200, 0, 255), "Release to place paste");
                } else {
                    dl->AddText(ImVec2(img_pos.x + 10, img_pos.y + 10), IM_COL32(255, 255, 0, 255), "Drag to position | Enter to apply | Esc to cancel");
                }

                /* Handle dragging the paste */
                if (hovering && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_pasted.dragging = true;
                }
                if (g_pasted.dragging && mbdn) {
                    int new_x = (int)((mouse.x - img_pos.x) / sx);
                    int new_y = (int)((mouse.y - img_pos.y) / sy);
                    if (new_x < 0) new_x = 0;
                    if (new_y < 0) new_y = 0;
                    if (new_x + pw > (int)g_img_tex_w) new_x = g_img_tex_w - pw;
                    if (new_y + ph > (int)g_img_tex_h) new_y = g_img_tex_h - ph;
                    g_pasted.paste_x = new_x;
                    g_pasted.paste_y = new_y;
                } else if (!mbdn && g_pasted.dragging) {
                    g_pasted.dragging = false;
                    g_pasted.active = false;  /* Paste complete */
                    apply_pasted_region();
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

            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton(("##sw" + std::to_string(i)).c_str(), ImVec2(sw16, sh16));
            if (ImGui::IsItemClicked()) g_sel_color = i;
        }

        /* Advance cursor past the swatch grid */
        ImGui::SetCursorScreenPos(ImVec2(pos0.x + 16 * col_w + 6, pos0.y));

        ImGui::BeginGroup();
        {
            SDL_Color &col = g_palette[g_sel_color];
            int r = col.r, g_c = col.g, b = col.b;
            ImGui::Text("Index: %d", g_sel_color);
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("R##cr", &r,   0, 255)) {
                col.r = (unsigned char)r;
                palette_writeback(g_sel_color);
            }
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("G##cg", &g_c, 0, 255)) {
                col.g = (unsigned char)g_c;
                palette_writeback(g_sel_color);
            }
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("B##cb", &b,   0, 255)) {
                col.b = (unsigned char)b;
                palette_writeback(g_sel_color);
            }
        }
        ImGui::EndGroup();

        /* --- Library Info (far right) --- */
        float right_x = sw - 180.f;
        ImGui::SetCursorScreenPos(ImVec2(right_x, pos0.y));
        ImGui::BeginGroup();
        {
            ImGui::Text("Images:   %u", imgcnt);
            ImGui::Text("Palettes: %u", palcnt);
            ImGui::Text("Seqs:     %u", seqcnt);
            ImGui::Text("Scripts:  %u", scrcnt);
            ImGui::Text("DamTbls:  %u", damcnt);
            ImGui::Text("Version:  0x%04X", fileversion);
        }
        ImGui::EndGroup();
    }
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

    /* ===== HELP MODAL ===== */
    /* ---- Debug Info popup ---- */
    if (g_show_debug) ImGui::OpenPopup("Debug Info");
    if (ImGui::BeginPopupModal("Debug Info", &g_show_debug, ImGuiWindowFlags_NoMove)) {
        ImGui::SetNextWindowSize(ImVec2(520, 580), ImGuiCond_Always);

        /* --- Library Header --- */
        if (ImGui::CollapsingHeader("LIB_HDR", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::Text("IMGCNT:  %u",   imgcnt);
            ImGui::Text("PALCNT:  %u",   palcnt);
            ImGui::Text("SEQCNT:  %u",   seqcnt);
            ImGui::Text("SCRCNT:  %u",   scrcnt);
            ImGui::Text("DAMCNT:  %u",   damcnt);
            ImGui::Text("VERSION: 0x%04X", fileversion);
        }

        /* --- Selected IMAGE record (runtime) --- */
        IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
        if (ImGui::CollapsingHeader("IMAGE (runtime)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (img) {
                ImGui::Text("NXT_p:    0x%08X",   (unsigned)(uintptr_t)img->nxt_p);
                ImGui::Text("N_s:      %.15s",    img->n_s);
                ImGui::Text("FLAGS:    0x%04X",   img->flags);
                ImGui::Text("ANIX:     %u",       img->anix);
                ImGui::Text("ANIY:     %u",       img->aniy);
                ImGui::Text("W:        %u",       img->w);
                ImGui::Text("H:        %u",       img->h);
                ImGui::Text("PALNUM:   %u",       img->palnum);
                ImGui::Text("DATA_p:   0x%08X",   (unsigned)(uintptr_t)img->data_p);
                if (img->pttbl_p) ImGui::Text("PTTBL_p:  0x%08X", (unsigned)(uintptr_t)img->pttbl_p);
                else ImGui::TextDisabled("PTTBL_p:  NULL");
                ImGui::Text("ANIX2:    %u",       img->anix2);
                ImGui::Text("ANIY2:    %u",       img->aniy2);
                ImGui::Text("ANIZ2:    %u",       img->aniz2);
                ImGui::Text("OPALS:    0x%04X",   img->opals);
                ImGui::Text("TEMP:     0x%08X",   (unsigned)(uintptr_t)img->temp);
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Selected PALETTE record (runtime) --- */
        PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
        if (ImGui::CollapsingHeader("PALETTE (runtime)", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (pal) {
                ImGui::Text("NXT_p:    0x%08X",   (unsigned)(uintptr_t)pal->nxt_p);
                ImGui::Text("N_s:      %.9s",     pal->n_s);
                ImGui::Text("FLAGS:    0x%02X",   pal->flags);
                ImGui::Text("BITSPIX:  %u",       pal->bitspix);
                ImGui::Text("NUMC:     %u",       pal->numc);
                ImGui::Text("PAD:      0x%04X",   pal->pad);
                ImGui::Text("DATA_p:   0x%08X",   (unsigned)(uintptr_t)pal->data_p);
                ImGui::Text("TEMP:     0x%08X",   (unsigned)(uintptr_t)pal->temp);
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
                img_save();
                g_last_saved_version = fileversion;
                ExitProcess(0);
            } else {
                OpenFileDialog(FileDialogMode::SaveImg); /* Ask for location before closing if no filename exists */
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Discard", ImVec2(80, 0))) {
            g_show_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
            ExitProcess(0);
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(80, 0))) {
            g_show_unsaved_confirm = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (g_show_help) ImGui::OpenPopup("Help");
    if (ImGui::BeginPopupModal("Help", &g_show_help,
            ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::SetNextWindowSize(ImVec2(580, 400), ImGuiCond_Always);
        ImGui::TextUnformatted(g_help_text);
        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Close", ImVec2(120, 0))) {
            g_show_help = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    /* ===== ABOUT MODAL ===== */
    if (g_show_about) ImGui::OpenPopup("About midway-imgtool");
    if (ImGui::BeginPopupModal("About midway-imgtool", &g_show_about, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("midway-imgtool");
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
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}
