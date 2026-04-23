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
#include "imgui_overlay.h"

/* Link asm-side symbols (COFF has no leading underscore from asm) */
#ifdef _MSC_VER
#pragma comment(linker, "/alternatename:_img_p=img_p")
#pragma comment(linker, "/alternatename:_imgcnt=imgcnt")
#pragma comment(linker, "/alternatename:_ilselected=ilselected")
#pragma comment(linker, "/alternatename:_pal_p=pal_p")
#pragma comment(linker, "/alternatename:_palcnt=palcnt")
#pragma comment(linker, "/alternatename:_plselected=plselected")
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
    void          *pttbl_p;
    unsigned short anix2;
    unsigned short aniy2;
    unsigned short aniz2;
    unsigned short opals;
    void          *temp;
};

struct PAL {
    void          *nxt_p;
    char           n_s[10];
    unsigned char  flags;
    unsigned char  bitspix;
    unsigned short numc;
    unsigned short pad;
    void          *data_p;
    void          *temp;
};
#pragma pack(pop)

/* SDL state */
static SDL_Window   *g_imgui_window   = NULL;
static SDL_Renderer *g_imgui_renderer = NULL;
static SDL_Texture  *g_canvas_texture = NULL;  /* VGA plane tex — kept for init compat, not displayed */

/* Per-image render texture — rebuilt when selected image changes */
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
    void shim_key_inject(unsigned short keycode);
}

/* ---- Layout constants ---- */
static const float TOOLBAR_W   = 40.0f;   /* left toolbar width */
static const float PANEL_W     = 220.0f;  /* right panel strip width */
static const float PALETTE_H   = 110.0f;  /* bottom palette bar height */

/* ---- Undo system ---- */
#define UNDO_STACK_SIZE 64
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

/* ---- Editor state ---- */
static int  g_sel_color   = 0;
static bool g_show_points = true;
static bool g_show_hitbox = false;
static int  g_hitbox_x = 0, g_hitbox_y = 0, g_hitbox_w = 32, g_hitbox_h = 32;
static int  g_hitbox_drag_corner = -1;  /* -1=none, 0=TL,1=TR,2=BR,3=BL */

/* Palette rename dialog */
static bool g_show_rename = false;
static int  g_rename_pal_idx = -1;
static char g_rename_buf[10] = {0};

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

/* ---- Image texture renderer ---- */
static void rebuild_img_texture(IMG *img)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        if (g_img_texture) { SDL_DestroyTexture(g_img_texture); g_img_texture = NULL; }
        g_img_tex_w = g_img_tex_h = 0;
        return;
    }
    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;  /* row stride padded to 4-byte boundary */

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
            last->anix2 == img->anix2 && last->aniy2 == img->aniy2)
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
    /* Tighten up the style to feel more like a pro tool */
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

/* ---- Inject key into asm queue ---- */
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

    /* ---- Menu bar ---- */
    if (ImGui::BeginMainMenuBar()) {
        /* File — key bindings from itimg.asm key_t table:
           'l'=load IMG  's'=save IMG  'a'=append  27=Esc/quit */
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...",  "l"))   imgui_overlay_inject_key('l');
            if (ImGui::MenuItem("Save",     "s"))   imgui_overlay_inject_key('s');
            if (ImGui::MenuItem("Append"))          imgui_overlay_inject_key('a');
            ImGui::Separator();
            if (ImGui::MenuItem("Quit",     "Esc")) imgui_overlay_inject_key(27);
            ImGui::EndMenu();
        }
        /* Edit */
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
            if (ImGui::MenuItem("Rename",    "Ctrl+R"))  imgui_overlay_inject_key(0x12);
            if (ImGui::MenuItem("Delete",    "Ctrl+D"))  imgui_overlay_inject_key(0x04);
            if (ImGui::MenuItem("Build TGA", "Ctrl+B"))  imgui_overlay_inject_key(0x02);
            ImGui::EndMenu();
        }
        /* Image */
        if (ImGui::BeginMenu("Image")) {
            if (ImGui::MenuItem("Add/Del Point Table", "Ctrl+P")) imgui_overlay_inject_key(0x10);
            ImGui::Separator();
            if (ImGui::MenuItem("Redraw", "f"))  imgui_overlay_inject_key('f');
            ImGui::EndMenu();
        }
        /* In/Out — Alt+L=load LBM, Alt+S=save LBM, Ctrl+L=load TGA, Ctrl+S=save TGA */
        if (ImGui::BeginMenu("In/Out")) {
            if (ImGui::MenuItem("Load LBM", "Alt+L"))  imgui_overlay_inject_key(0x2600);
            if (ImGui::MenuItem("Save LBM", "Alt+S"))  imgui_overlay_inject_key(0x1f00);
            if (ImGui::MenuItem("Load TGA", "Ctrl+L")) imgui_overlay_inject_key(0x000C);
            if (ImGui::MenuItem("Save TGA", "Ctrl+S")) imgui_overlay_inject_key(0x0013);
            ImGui::EndMenu();
        }
        /* Palette — '*'=merge, plst nav via ' / " ? */
        if (ImGui::BeginMenu("Palette")) {
            if (ImGui::MenuItem("Merge Selected", "*"))  imgui_overlay_inject_key('*');
            ImGui::EndMenu();
        }
        /* Marks — 'm'=clear all, 'M'=set all from key_t */
        if (ImGui::BeginMenu("Marks")) {
            if (ImGui::MenuItem("Clear All Image Marks", "m"))  imgui_overlay_inject_key('m');
            if (ImGui::MenuItem("Set All Image Marks",   "M"))  imgui_overlay_inject_key('M');
            ImGui::EndMenu();
        }
        /* View — 'd'=zoom in, 'D'=zoom out, 'T'=toggle, '2'=2nd list, 'p'=iwin */
        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Zoom In",  "d"))  imgui_overlay_inject_key('d');
            if (ImGui::MenuItem("Zoom Out", "D"))  imgui_overlay_inject_key('D');
            if (ImGui::MenuItem("Redraw",   "f"))  imgui_overlay_inject_key('f');
            ImGui::Separator();
            ImGui::MenuItem("Anim Points", NULL, &g_show_points);
            ImGui::MenuItem("Hitboxes",    NULL, &g_show_hitbox);
            ImGui::EndMenu();
        }
        /* Help */
        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Help", "h"))  imgui_overlay_inject_key('h');
            ImGui::EndMenu();
        }
        ImGui::EndMainMenuBar();
    }

    float menu_h = ImGui::GetFrameHeight();
    float work_y = menu_h;
    float work_h = sh - work_y;

    /* Rebuild image texture every frame — captures palette changes and live edits */
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
        /* Open */
        if (ImGui::Button("Op", btn))  imgui_overlay_inject_key('l');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Open IMG file (l)");
        /* Save */
        if (ImGui::Button("Sv", btn))  imgui_overlay_inject_key('s');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Save IMG file (s)");
        ImGui::Spacing();
        /* Zoom in */
        if (ImGui::Button("Z+", btn))  imgui_overlay_inject_key('d');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom In");
        /* Zoom out */
        if (ImGui::Button("Z-", btn))  imgui_overlay_inject_key('D');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom Out");
        /* Zoom 1:1 */
        if (ImGui::Button("1:1", btn)) imgui_overlay_inject_key('1');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Zoom 1:1");
        ImGui::Spacing();
        /* Mark / Unmark */
        if (ImGui::Button("Mk", btn))  imgui_overlay_inject_key(' ');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark/Unmark");
        /* Mark All */
        if (ImGui::Button("MA", btn))  imgui_overlay_inject_key('m');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Mark All");
        /* Clear Marks */
        if (ImGui::Button("CM", btn))  imgui_overlay_inject_key('M');
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clear Marks");
        ImGui::Spacing();
        /* Toggle anim points */
        ImGui::PushStyleColor(ImGuiCol_Button, g_show_points ?
            ImVec4(0.2f,0.6f,0.2f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button("Pt", btn)) g_show_points = !g_show_points;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Anim Points");
        /* Toggle hitbox */
        ImGui::PushStyleColor(ImGuiCol_Button, g_show_hitbox ?
            ImVec4(0.0f,0.5f,0.6f,1.f) : ImVec4(0.25f,0.25f,0.25f,1.f));
        if (ImGui::Button("Hb", btn)) g_show_hitbox = !g_show_hitbox;
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Toggle Hitbox");
        ImGui::Spacing();
        /* Undo / Redo */
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
                        int delta = i - ilselected;
                        unsigned short key = delta > 0 ? 0x5000 : 0x4800;
                        int steps = delta > 0 ? delta : -delta;
                        for (int s = 0; s < steps; s++) imgui_overlay_inject_key(key);
                        if (delta == 0) imgui_overlay_inject_key(0x5000);
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
        }

        /* --- Palette List --- */
        int n_pals = count_pals();
        if (ImGui::CollapsingHeader("Palettes", ImGuiTreeNodeFlags_DefaultOpen)) {
            float list_h = panel_h * 0.15f;
            if (ImGui::BeginListBox("##pallist", ImVec2(-1, list_h))) {
                for (int i = 0; i < n_pals; i++) {
                    PAL *pal = get_pal(i);
                    if (!pal) break;
                    bool selected = (i == plselected);
                    ImGui::PushID(1000 + i);
                    if (ImGui::Selectable(pal->n_s, selected)) {}
                    /* Right-click rename */
                    if (ImGui::BeginPopupContextItem("##palctx")) {
                        if (ImGui::MenuItem("Rename")) {
                            g_rename_pal_idx = i;
                            strncpy(g_rename_buf, pal->n_s, 9);
                            g_rename_buf[9] = '\0';
                            g_show_rename = true;
                        }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }
                ImGui::EndListBox();
            }
        }

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img) {
                ImGui::Text("Name:   %s", img->n_s);
                ImGui::Text("Size:   %d x %d", img->w, img->h);
                ImGui::Text("Pal:    %d", img->palnum);
                ImGui::Text("Anipt:  %d, %d", img->anix, img->aniy);
                if (img->anix2 || img->aniy2)
                    ImGui::Text("Anipt2: %d, %d", img->anix2, img->aniy2);
                ImGui::Text("Marked: %s", (img->flags & 1) ? "Yes" : "No");
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Point Editor --- */
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
        ImVec2 avail = ImGui::GetContentRegionAvail();
        ImVec2 img_pos = ImGui::GetCursorScreenPos();
        ImVec2 img_sz(0, 0);
        float sx = 1.0f, sy = 1.0f;

        if (g_img_texture && g_img_tex_w > 0 && g_img_tex_h > 0) {
            /* Scale image to fit canvas, keeping pixel aspect ratio */
            float tw = avail.x, th = (float)g_img_tex_h * (tw / (float)g_img_tex_w);
            if (th > avail.y) { th = avail.y; tw = (float)g_img_tex_w * (th / (float)g_img_tex_h); }
            /* Integer-scale snap for crisp pixels */
            float scale = (float)(int)(tw / (float)g_img_tex_w);
            if (scale < 1.0f) scale = 1.0f;
            tw = (float)g_img_tex_w * scale;
            th = (float)g_img_tex_h * scale;
            /* Centre in available area */
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
                for (float cx = img_pos.x; cx < img_pos.x + tw; cx += cs) {
                    int row = (int)((cy - img_pos.y) / cs);
                    int col = (int)((cx - img_pos.x) / cs);
                    ImU32 col32 = ((row + col) & 1) ? IM_COL32(160,160,160,255) : IM_COL32(100,100,100,255);
                    float x2 = cx + cs; if (x2 > img_pos.x + tw) x2 = img_pos.x + tw;
                    float y2 = cy + cs; if (y2 > img_pos.y + th) y2 = img_pos.y + th;
                    dl->AddRectFilled(ImVec2(cx, cy), ImVec2(x2, y2), col32);
                }
            }
            ImGui::Image((ImTextureID)(intptr_t)g_img_texture, img_sz);
        } else {
            /* No image selected */
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + avail.y * 0.45f);
            float tw = ImGui::CalcTextSize("No image selected").x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (avail.x - tw) * 0.5f);
            ImGui::TextDisabled("No image selected");
        }
        ImGuiIO &cio = ImGui::GetIO();
        ImVec2 mouse = cio.MousePos;
        bool   mbdn  = ImGui::IsMouseDown(ImGuiMouseButton_Left);

        /* --- Anim point overlay + dragging --- */
        if (g_show_points) {
            IMG *img = (ilselected >= 0) ? get_img(ilselected) : NULL;
            if (img && img->w > 0) {
                ImDrawList *dl = ImGui::GetWindowDrawList();

                /* Primary point */
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

                /* Secondary point */
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
                if (c == 1) { g_hitbox_w = mx - g_hitbox_x;  g_hitbox_h += g_hitbox_y - my; g_hitbox_y = my; }
                if (c == 2) { g_hitbox_w = mx - g_hitbox_x;  g_hitbox_h = my - g_hitbox_y; }
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
        /* 256-color swatch grid */
        ImDrawList *dl    = ImGui::GetWindowDrawList();
        ImVec2      pos0  = ImGui::GetCursorScreenPos();
        float       sw16  = 14.0f;
        float       sh16  = 14.0f;
        float       gap   = 1.0f;
        float       row_h = sh16 + gap;
        float       col_w = sw16 + gap;

        for (int i = 0; i < 256; i++) {
            int row = i / 16, col = i % 16;
            ImVec2 p0(pos0.x + col * col_w, pos0.y + row * row_h);
            ImVec2 p1(p0.x + sw16, p0.y + sh16);
            SDL_Color c = g_palette[i];
            dl->AddRectFilled(p0, p1,
                IM_COL32(c.r, c.g, c.b, 255));
            if (i == g_sel_color)
                dl->AddRect(p0, p1, IM_COL32(255,255,255,255), 0, 0, 1.5f);

            /* Hit test */
            ImGui::SetCursorScreenPos(p0);
            ImGui::InvisibleButton(("##sw" + std::to_string(i)).c_str(), ImVec2(sw16, sh16));
            if (ImGui::IsItemClicked()) g_sel_color = i;
        }

        /* Advance cursor past the swatch grid */
        ImGui::SetCursorScreenPos(ImVec2(pos0.x + 16 * col_w + 6, pos0.y));

        /* Color sliders next to the swatches */
        ImGui::BeginGroup();
        {
            SDL_Color &col = g_palette[g_sel_color];
            int r = col.r, g_c = col.g, b = col.b;
            ImGui::Text("Index: %d", g_sel_color);
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("R##cr", &r,   0, 255)) col.r = (unsigned char)r;
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("G##cg", &g_c, 0, 255)) col.g = (unsigned char)g_c;
            ImGui::SetNextItemWidth(160);
            if (ImGui::SliderInt("B##cb", &b,   0, 255)) col.b = (unsigned char)b;
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

    /* Flush to renderer */
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_imgui_renderer);
}

void imgui_overlay_shutdown(void)
{
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}
