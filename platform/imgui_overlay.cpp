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
#include "imgui_overlay.h"

/* Link asm-side symbols (COFF has no leading underscore from asm) */
#ifdef _MSC_VER
#pragma comment(linker, "/alternatename:_img_p=img_p")
#pragma comment(linker, "/alternatename:_imgcnt=imgcnt")
#pragma comment(linker, "/alternatename:_ilselected=ilselected")
#pragma comment(linker, "/alternatename:_pal_p=pal_p")
#pragma comment(linker, "/alternatename:_palcnt=palcnt")
#pragma comment(linker, "/alternatename:_plselected=plselected")
#pragma comment(linker, "/alternatename:_seqcnt=seqcnt")
#pragma comment(linker, "/alternatename:_scrcnt=scrcnt")
#pragma comment(linker, "/alternatename:_damcnt=damcnt")
#pragma comment(linker, "/alternatename:_fileversion=fileversion")
#pragma comment(linker, "/alternatename:_ilst_savelbmmrkd=ilst_savelbmmrkd")
#pragma comment(linker, "/alternatename:_ilst_renamemrkd=ilst_renamemrkd")
#pragma comment(linker, "/alternatename:_ilst_deletemrkd=ilst_deletemrkd")
#pragma comment(linker, "/alternatename:_ilst_stripmrkd=ilst_stripmrkd")
#pragma comment(linker, "/alternatename:_ilst_striplowmrkd=ilst_striplowmrkd")
#pragma comment(linker, "/alternatename:_ilst_striprngmrkd=ilst_striprngmrkd")
#pragma comment(linker, "/alternatename:_ilst_ditherrepmrkd=ilst_ditherrepmrkd")
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
    unsigned int   oset;        /* offset in file of image */
    void          *data_p;      /* pointer to image data */
    unsigned short lib;         /* library handle index */
    unsigned short anix2;
    unsigned short aniy2;
    unsigned short aniz2;
    unsigned short frm;         /* frame number for anim */
    unsigned short pttblnum;    /* point table index or 0xFFFF */
    unsigned short opals;       /* alternate palette or 0xFFFF */
};

struct PAL {
    void          *nxt_p;
    char           n_s[10];
    unsigned char  flags;
    unsigned char  bitspix;   /* bits per pixel — 8 for 256-color */
    unsigned short numc;      /* number of colors */
    unsigned int   oset;      /* offset in file of palette */
    void          *data_p;    /* pointer to packed RGB triplets (3 bytes each, 0-63 range) */
    unsigned short lib;       /* library handle index */
    unsigned char  colind;    /* CRAM start color */
    unsigned char  cmap;      /* color map selection (0-F) */
    unsigned short spare;
};
#pragma pack(pop)

/* SDL state */
static SDL_Window   *g_imgui_window   = NULL;
static SDL_Renderer *g_imgui_renderer = NULL;
static SDL_Texture  *g_canvas_texture = NULL;  /* VGA plane tex — kept for init compat, not displayed */

/* Per-image render texture — rebuilt when selected image or palette changes */
static SDL_Texture  *g_img_texture    = NULL;
static int           g_img_tex_w      = 0;
static int           g_img_tex_h      = 0;
static int           g_img_tex_idx    = -2;  /* -2 = never built */

/* Asm-side symbol externs */
extern "C" {
    extern unsigned int   shim_ebx, shim_ecx, shim_edx;
    extern unsigned short shim_keycode;
    extern int            shim_zf;
    extern SDL_Color      g_palette[256];
    extern void          *img_p;
    extern unsigned int   imgcnt;
    extern int            ilselected;
    extern void          *pal_p;
    extern unsigned int   palcnt;
    extern int            plselected;
    extern unsigned int   seqcnt;
    extern unsigned int   scrcnt;
    extern unsigned int   damcnt;
    extern unsigned int   fileversion;
    void shim_key_inject(unsigned short keycode);
    void ilst_savelbmmrkd(void);
    void ilst_renamemrkd(void);
    void ilst_deletemrkd(void);
    void ilst_stripmrkd(void);
    void ilst_striplowmrkd(void);
    void ilst_striprngmrkd(void);
    void ilst_ditherrepmrkd(void);
}

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

/* ---- Clipboard (cut/copy/paste) ---- */
struct CopiedImage {
    bool           valid;
    char           n_s[16];
    unsigned short flags;
    unsigned short anix, aniy;
    unsigned short anix2, aniy2, aniz2;
    unsigned short w, h;
    unsigned short palnum;
    unsigned short opals;
};
static CopiedImage g_clipboard = {false};

/* ---- Editor state ---- */
static int  g_sel_color   = 0;
static bool g_show_points = true;
static bool g_show_hitbox = false;
static int  g_hitbox_x = 0, g_hitbox_y = 0, g_hitbox_w = 32, g_hitbox_h = 32;
static int  g_hitbox_drag_corner = -1;

/* Palette rename dialog */
static bool g_show_rename = false;
static int  g_rename_pal_idx = -1;
static char g_rename_buf[10] = {0};

/* Help modal */
static bool g_show_help = false;
static bool g_show_debug = false;
static const char *g_help_text =
    "IMAGE TOOL HELP\n\n"
    "Escape - Aborts a function           Enter - Accepts a function\n"
    "h - Shows this help                  f - Redraws screen\n\n"
    "l / s - IMG load/save                m - clear all marks\n"
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
   The asm stores palette colors as packed 6-bit-per-channel RGB triplets in pal->data_p.
   When the user edits g_palette[] via the sliders, write the change back so the
   in-memory PAL struct reflects it and a subsequent Save will persist it. */
static void palette_writeback(int color_idx)
{
    PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (color_idx < 0 || color_idx >= (int)pal->numc) return;

    SDL_Color &c = g_palette[color_idx];
    unsigned char *dst = (unsigned char *)pal->data_p + color_idx * 3;
    dst[0] = (unsigned char)(c.r >> 2);  /* 8-bit → 6-bit */
    dst[1] = (unsigned char)(c.g >> 2);
    dst[2] = (unsigned char)(c.b >> 2);
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

/* ---- Copy/Paste helpers ---- */
static void copy_image(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img) return;
    memcpy(g_clipboard.n_s, img->n_s, 16);
    g_clipboard.flags  = img->flags & ~1;
    g_clipboard.anix   = img->anix;  g_clipboard.aniy  = img->aniy;
    g_clipboard.anix2  = img->anix2; g_clipboard.aniy2 = img->aniy2;
    g_clipboard.aniz2  = img->aniz2;
    g_clipboard.w      = img->w;     g_clipboard.h     = img->h;
    g_clipboard.palnum = img->palnum;
    g_clipboard.opals  = img->opals;
    g_clipboard.valid  = true;
}

static void paste_image(void)
{
    IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
    if (!img || !g_clipboard.valid) return;
    undo_push();
    memcpy(img->n_s, g_clipboard.n_s, 16);
    img->flags  = (img->flags & 1) | (g_clipboard.flags & ~1);
    img->anix   = g_clipboard.anix;  img->aniy  = g_clipboard.aniy;
    img->anix2  = g_clipboard.anix2; img->aniy2 = g_clipboard.aniy2;
    img->aniz2  = g_clipboard.aniz2;
    img->w      = g_clipboard.w;     img->h     = g_clipboard.h;
    img->palnum = g_clipboard.palnum;
    img->opals  = g_clipboard.opals;
    g_img_tex_idx = -2;
}

/* ---- Public C interface ---- */

void imgui_overlay_init(SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *canvas_tex)
{
    g_imgui_window   = window;
    g_imgui_renderer = renderer;
    g_canvas_texture = canvas_tex;

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

void imgui_overlay_inject_key(unsigned short code)
{
    shim_key_inject(code);
}

/* =========================================================
   Main render function — called each frame
   ========================================================= */
void imgui_overlay_render(void)
{
    ImGuiIO &io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    /* ---- Global keyboard shortcuts (only when ImGui is not eating keys for text input) ---- */
    if (!io.WantTextInput) {
        bool ctrl = io.KeyCtrl;
        /* Ctrl+Z — undo within ImGui editor state */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (g_undo_idx > 0) { g_undo_idx--; undo_apply(g_undo_idx); }
        }
        /* Ctrl+Y — redo */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
            if (g_undo_idx < g_undo_count - 1) { g_undo_idx++; undo_apply(g_undo_idx); }
        }
        /* Ctrl+C — copy */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_C, false)) copy_image();
        /* Ctrl+X — cut */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            copy_image();
            imgui_overlay_inject_key(0x04);
        }
        /* Ctrl+V — paste */
        if (ctrl && ImGui::IsKeyPressed(ImGuiKey_V, false)) paste_image();
        /* h — help */
        if (ImGui::IsKeyPressed(ImGuiKey_H, false)) g_show_help = true;
        /* F9 — debug info */
        if (ImGui::IsKeyPressed(ImGuiKey_F9, false)) g_show_debug = !g_show_debug;
    }

    /* ---- Menu bar ---- */
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...",  "l"))   imgui_overlay_inject_key('l');
            if (ImGui::MenuItem("Save",     "s"))   imgui_overlay_inject_key('s');
            if (ImGui::MenuItem("Append",   "a"))   imgui_overlay_inject_key('a');
            ImGui::Separator();
            if (ImGui::MenuItem("Load LBM", "Alt+L"))   imgui_overlay_inject_key(0x2600);
            if (ImGui::MenuItem("Save LBM", "Alt+S"))        imgui_overlay_inject_key(0x1f00);
            if (ImGui::MenuItem("Save Marked LBM"))          ilst_savelbmmrkd();
            if (ImGui::MenuItem("Load TGA", "Ctrl+L"))       imgui_overlay_inject_key(0x000C);
            if (ImGui::MenuItem("Save TGA", "Ctrl+S"))  imgui_overlay_inject_key(0x0013);
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) imgui_overlay_inject_key(27);
            ImGui::EndMenu();
        }
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
                imgui_overlay_inject_key(0x04);
            }
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, g_clipboard.valid && ilselected >= 0))
                paste_image();
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Image",  "Ctrl+R"))  imgui_overlay_inject_key(0x12);
            if (ImGui::MenuItem("Delete Image",  "Ctrl+D"))  imgui_overlay_inject_key(0x04);
            if (ImGui::MenuItem("Duplicate"))                imgui_overlay_inject_key('a');
            if (ImGui::MenuItem("Build TGA",     "Ctrl+B"))  imgui_overlay_inject_key(0x02);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Image")) {
            if (ImGui::MenuItem("Mark / Unmark",            "Space"))        imgui_overlay_inject_key(' ');
            if (ImGui::MenuItem("Set All Marks",            "M"))            imgui_overlay_inject_key('M');
            if (ImGui::MenuItem("Clear All Marks",          "m"))            imgui_overlay_inject_key('m');
            if (ImGui::MenuItem("Invert All Marks")) {
                IMG *p=(IMG*)img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;}
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Jump to Prev Marked",      "Left"))         imgui_overlay_inject_key(0x4b00);
            if (ImGui::MenuItem("Jump to Next Marked",      "Right"))        imgui_overlay_inject_key(0x4d00);
            if (ImGui::MenuItem("Move Image Up in List",    "Alt+PgUp"))     imgui_overlay_inject_key(0x9900);
            if (ImGui::MenuItem("Move Image Down in List",  "Alt+PgDn"))     imgui_overlay_inject_key(0xa100);
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Image",             "Ctrl+R"))       imgui_overlay_inject_key(0x12);
            if (ImGui::MenuItem("Add/Del Point Table",      "Ctrl+P"))       imgui_overlay_inject_key(0x10);
            if (ImGui::MenuItem("Set ID from 2nd List",     "i"))            imgui_overlay_inject_key('i');
            if (ImGui::MenuItem("Least-Squares Reduce",     ";"))            imgui_overlay_inject_key(';');
            if (ImGui::MenuItem("Clear Extra Data",         "Alt+C"))        imgui_overlay_inject_key(0x2E00);
            ImGui::Separator();
            if (ImGui::MenuItem("Switch Image List",        "Tab"))          imgui_overlay_inject_key(0x09);
            if (ImGui::MenuItem("Toggle Anim Point Mode",   "a"))            imgui_overlay_inject_key('a');
            if (ImGui::MenuItem("Show True Palette Colors", "t"))            imgui_overlay_inject_key('t');
            if (ImGui::MenuItem("Redraw",                   "f"))            imgui_overlay_inject_key('f');
            ImGui::Separator();
            if (ImGui::BeginMenu("Marked Images")) {
                if (ImGui::MenuItem("Rename Marked"))               ilst_renamemrkd();
                if (ImGui::MenuItem("Delete Marked"))               ilst_deletemrkd();
                ImGui::Separator();
                if (ImGui::MenuItem("Set Palette",     "["))        imgui_overlay_inject_key('[');
                ImGui::Separator();
                if (ImGui::MenuItem("Strip Edge"))                  ilst_stripmrkd();
                if (ImGui::MenuItem("Strip Edge Low"))              ilst_striplowmrkd();
                if (ImGui::MenuItem("Strip Edge Range"))            ilst_striprngmrkd();
                ImGui::Separator();
                if (ImGui::MenuItem("Least Squares",   ";"))        imgui_overlay_inject_key(';');
                if (ImGui::MenuItem("Dither Replace"))              ilst_ditherrepmrkd();
                if (ImGui::MenuItem("Build TGA",       "Ctrl+B"))   imgui_overlay_inject_key(0x02);
                ImGui::EndMenu();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Palette")) {
            if (ImGui::MenuItem("Set Palette for Image",      "]"))       imgui_overlay_inject_key(']');
            if (ImGui::MenuItem("Set Palette for Marked",     "["))       imgui_overlay_inject_key('[');
            ImGui::Separator();
            if (ImGui::MenuItem("Merge Marked into Selected", "*"))       imgui_overlay_inject_key('*');
            if (ImGui::MenuItem("Delete Palette",             "Del"))     imgui_overlay_inject_key(0x5300);
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Palette",             "Shift+R")) imgui_overlay_inject_key(0x0052);
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
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom In",  "d"))  imgui_overlay_inject_key('d');
            if (ImGui::MenuItem("Zoom Out", "D"))  imgui_overlay_inject_key('D');
            if (ImGui::MenuItem("Redraw",   "f"))  imgui_overlay_inject_key('f');
            ImGui::Separator();
            ImGui::MenuItem("Anim Points", NULL, &g_show_points);
            ImGui::MenuItem("Hitboxes",    NULL, &g_show_hitbox);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Show Help", "h")) g_show_help = true;
            if (ImGui::MenuItem("Debug Info", "F9")) g_show_debug = !g_show_debug;
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    float menu_h = ImGui::GetFrameHeight();
    float work_y = menu_h;
    float work_h = sh - work_y;

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
        if (ImGui::Button("Op", btn))  imgui_overlay_inject_key('l');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open IMG (l)");
        if (ImGui::Button("Sv", btn))  imgui_overlay_inject_key('s');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save IMG (s)");
        ImGui::Spacing();
        if (ImGui::Button("Z+", btn))  imgui_overlay_inject_key('d');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom In (d)");
        if (ImGui::Button("Z-", btn))  imgui_overlay_inject_key('D');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom Out (D)");
        ImGui::Spacing();
        if (ImGui::Button("Mk", btn))  imgui_overlay_inject_key(' ');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark/Unmark (Space)");
        if (ImGui::Button("MA", btn))  imgui_overlay_inject_key('M');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Set All Marks (M)");
        if (ImGui::Button("CM", btn))  imgui_overlay_inject_key('m');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear All Marks (m)");
        ImGui::Spacing();
        ImGui::PushStyleColor(ImGuiCol_Button, g_show_points ?
            ImVec4(0.2f,0.6f,0.2f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button("Pt", btn)) g_show_points = !g_show_points;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Anim Points");
        ImGui::PushStyleColor(ImGuiCol_Button, g_show_hitbox ?
            ImVec4(0.0f,0.5f,0.6f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button("Hb", btn)) g_show_hitbox = !g_show_hitbox;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Hitbox");
        ImGui::Spacing();
        bool can_undo = g_undo_idx > 0;
        bool can_redo = g_undo_idx < g_undo_count - 1;
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button("Uz", btn)) { g_undo_idx--; undo_apply(g_undo_idx); }
        if (!can_undo) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Undo (Ctrl+Z)");
        if (!can_redo) ImGui::BeginDisabled();
        if (ImGui::Button("Ry", btn)) { g_undo_idx++; undo_apply(g_undo_idx); }
        if (!can_redo) ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Redo (Ctrl+Y)");
    }
    ImGui::End();

    /* ===== RIGHT PANEL STRIP ===== */
    float panel_x = sw - PANEL_W;
    float panel_y = work_y;
    float panel_h = work_h - PALETTE_H;

    ImGui::SetNextWindowPos(ImVec2(panel_x, panel_y));
    ImGui::SetNextWindowSize(ImVec2(PANEL_W, panel_h));
    ImGui::Begin("##panels", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);
    {
        /* --- Library Info --- */
        if (ImGui::CollapsingHeader("Library")) {
            ImGui::Text("Images:  %u", imgcnt);
            ImGui::Text("Palettes:%u", palcnt);
            ImGui::Text("Seqs:    %u", seqcnt);
            ImGui::Text("Scripts: %u", scrcnt);
            ImGui::Text("DamTbls: %u", damcnt);
            ImGui::Text("Version: 0x%04X", fileversion);
        }

        /* --- Image List --- */
        int n_imgs = count_imgs();
        if (ImGui::CollapsingHeader("Images", ImGuiTreeNodeFlags_DefaultOpen)) {
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
                    if (ImGui::Selectable(label, selected)) {
                        /* Navigate to the clicked image via arrow key injection */
                        int delta = i - ilselected;
                        unsigned short nav_key = delta > 0 ? 0x5000 : 0x4800;
                        int steps = delta > 0 ? delta : -delta;
                        for (int s = 0; s < steps; s++) imgui_overlay_inject_key(nav_key);
                        if (delta == 0) imgui_overlay_inject_key(0x5000);  /* refresh */
                    }
                    /* Right-click context menu on image items */
                    if (ImGui::BeginPopupContextItem("##imgctx")) {
                        if (ImGui::MenuItem("Mark / Unmark")) imgui_overlay_inject_key(' ');
                        if (ImGui::MenuItem("Rename"))        imgui_overlay_inject_key(0x12);
                        if (ImGui::MenuItem("Delete"))        imgui_overlay_inject_key(0x04);
                        ImGui::Separator();
                        if (ImGui::MenuItem("Build TGA"))     imgui_overlay_inject_key(0x02);
                        if (ImGui::MenuItem("Set Palette"))   imgui_overlay_inject_key(']');
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
            /* Mark buttons inline below list */
            if (ImGui::SmallButton("Mk All"))   imgui_overlay_inject_key('M');
            ImGui::SameLine();
            if (ImGui::SmallButton("Clr All"))  imgui_overlay_inject_key('m');
            ImGui::SameLine();
            if (ImGui::SmallButton("Invert"))   { IMG *p=(IMG*)img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;} }
            ImGui::SameLine();
            if (ImGui::SmallButton("Mk"))       imgui_overlay_inject_key(' ');
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
                    if (ImGui::Selectable(label, sel)) {
                        int delta = i - plselected;
                        unsigned short key = delta > 0 ? (unsigned short)'/' : (unsigned short)'\'';
                        int steps = delta > 0 ? delta : -delta;
                        for (int s = 0; s < steps; s++) imgui_overlay_inject_key(key);
                    }
                    /* Right-click context menu */
                    if (ImGui::BeginPopupContextItem("##palctx")) {
                        if (ImGui::MenuItem("Mark / Unmark"))             pal->flags ^= 1;
                        ImGui::Separator();
                        if (ImGui::MenuItem("Set for Image"))             imgui_overlay_inject_key(']');
                        if (ImGui::MenuItem("Set for Marked Images"))     imgui_overlay_inject_key('[');
                        if (ImGui::MenuItem("Merge Marked into Selected")) imgui_overlay_inject_key('*');
                        ImGui::Separator();
                        if (ImGui::MenuItem("Rename")) {
                            g_rename_pal_idx = i;
                            strncpy(g_rename_buf, pal->n_s, 9);
                            g_rename_buf[9] = '\0';
                            g_show_rename = true;
                        }
                        if (ImGui::MenuItem("Delete")) imgui_overlay_inject_key(0x5300);
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
            if (ImGui::SmallButton("Merge"))     imgui_overlay_inject_key('*');
            ImGui::SameLine();
            if (ImGui::SmallButton("Del"))       imgui_overlay_inject_key(0x5300);
        }

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img) {
                ImGui::Text("Name:   %.15s", img->n_s);
                ImGui::Text("Size:   %d x %d", img->w, img->h);

                PAL *pal = get_pal(img->palnum);
                if (pal) ImGui::Text("Pal:    %d  %.9s", img->palnum, pal->n_s);
                else     ImGui::Text("Pal:    %d", img->palnum);

                ImGui::Text("AX/AY:  %d, %d", img->anix, img->aniy);
                ImGui::Text("AX2/AY2: %d, %d", img->anix2, img->aniy2);
                ImGui::Text("AZ2:     %d", img->aniz2);

                char flagbuf[48] = {};
                if (img->flags & 1)  strncat(flagbuf, "Marked ", 47);
                if (img->flags & 2)  strncat(flagbuf, "Loaded ", 47);
                if (img->flags & 4)  strncat(flagbuf, "Changed ", 47);
                if (img->flags & 8)  strncat(flagbuf, "Delete ", 47);
                if (!flagbuf[0])     strncpy(flagbuf, "-", 47);
                ImGui::Text("Flags:  0x%04X  %s", img->flags, flagbuf);

                if (img->opals == 0xFFFF) ImGui::TextDisabled("OPALS:  none");
                else                      ImGui::Text("OPALS:  0x%04X", img->opals);

                if (img->pttblnum != 0xFFFF) ImGui::Text("PTTBL:  present (idx %u)", img->pttblnum);
                else                         ImGui::TextDisabled("PTTBL:  none");

                ImGui::Text("DATA:   0x%08X", (unsigned)(uintptr_t)img->data_p);

                ImGui::Spacing();
                if (g_clipboard.valid) ImGui::TextDisabled("Clip:   %.15s", g_clipboard.n_s);
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

    /* ===== CANVAS ===== */
    float canvas_x = TOOLBAR_W;
    float canvas_y = work_y;
    float canvas_w = sw - TOOLBAR_W - PANEL_W;
    float canvas_h = work_h - PALETTE_H;

    ImGui::SetNextWindowPos(ImVec2(canvas_x, canvas_y));
    ImGui::SetNextWindowSize(ImVec2(canvas_w, canvas_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 8));
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
        } else {
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.45f);
            float tw = ImGui::CalcTextSize("No image selected").x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - tw) * 0.5f);
            ImGui::TextDisabled("No image selected");
        }

        ImVec2 mouse = io.MousePos;
        bool   mbdn  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

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
                if (h1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { drag1 = true; undo_push(); }
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
                    if (h2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) { drag2 = true; undo_push(); }
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
    }
    ImGui::End();

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
    }
    ImGui::End();

    /* ===== PALETTE RENAME DIALOG ===== */
    if (g_show_rename) ImGui::OpenPopup("Rename Palette");
    if (ImGui::BeginPopupModal("Rename Palette", &g_show_rename, ImGuiWindowFlags_AlwaysAutoResize)) {
        PAL *pal = get_pal(g_rename_pal_idx);
        if (pal) {
            ImGui::Text("Rename: %s", pal->n_s);
            ImGui::InputText("##rn", g_rename_buf, sizeof(g_rename_buf));
            if (ImGui::Button("OK", ImVec2(100, 0))) {
                strncpy(pal->n_s, g_rename_buf, 9);
                pal->n_s[9] = '\0';
                g_show_rename = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel", ImVec2(100, 0))) {
                g_show_rename = false;
                ImGui::CloseCurrentPopup();
            }
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

        /* --- Selected IMAGE record --- */
        IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
        if (ImGui::CollapsingHeader("IMAGE record", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (img) {
                ImGui::Text("N_s:      %.15s",   img->n_s);
                ImGui::Text("FLAGS:    0x%04X",   img->flags);
                ImGui::Text("ANIX:     %u",       img->anix);
                ImGui::Text("ANIY:     %u",       img->aniy);
                ImGui::Text("W:        %u",       img->w);
                ImGui::Text("H:        %u",       img->h);
                ImGui::Text("PALNUM:   %u",       img->palnum);
                ImGui::Text("OSET:     0x%08X",   img->oset);
                ImGui::Text("DATA:     0x%08X",   (unsigned)(uintptr_t)img->data_p);
                ImGui::Text("LIB:      %u",       img->lib);
                ImGui::Text("ANIX2:    %u",       img->anix2);
                ImGui::Text("ANIY2:    %u",       img->aniy2);
                ImGui::Text("ANIZ2:    %u",       img->aniz2);
                ImGui::Text("FRM:      %u",       img->frm);
                if (img->pttblnum == 0xFFFF) ImGui::TextDisabled("PTTBLNUM: 0xFFFF (none)");
                else ImGui::Text("PTTBLNUM: %u",   img->pttblnum);
                if (img->opals == 0xFFFF) ImGui::TextDisabled("OPALS:    0xFFFF (none)");
                else ImGui::Text("OPALS:    0x%04X", img->opals);
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Selected PALETTE record --- */
        PAL *pal = (plselected >= 0) ? get_pal(plselected) : NULL;
        if (ImGui::CollapsingHeader("PALETTE record", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (pal) {
                ImGui::Text("N_s:     %.9s",     pal->n_s);
                ImGui::Text("FLAGS:   0x%02X",   pal->flags);
                ImGui::Text("BITSPIX: %u",       pal->bitspix);
                ImGui::Text("NUMC:    %u",       pal->numc);
                ImGui::Text("OSET:    0x%08X",   pal->oset);
                ImGui::Text("DATA:    0x%08X",   (unsigned)(uintptr_t)pal->data_p);
                ImGui::Text("LIB:     %u",       pal->lib);
                ImGui::Text("COLIND:  %u",       pal->colind);
                ImGui::Text("CMAP:    0x%X",     pal->cmap);
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
