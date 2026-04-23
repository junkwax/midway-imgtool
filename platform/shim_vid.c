/*************************************************************
 * platform/shim_vid.c
 * VGA Mode X shadow buffer + SDL2 rendering (renderer path)
 *************************************************************/
/* All functions here are called from 32-bit x86 asm that does not maintain
 * the 16-byte stack alignment required by the System V i386 ABI / SSE.
 * force_align_arg_pointer adds andl $-16, %esp at every function entry. */

#include <string.h>
#include <SDL.h>
#include "shim_vid.h"
#include "shim_file.h"   /* relay globals (shim_eax, shim_ecx, shim_esi) */
#include "imgui_overlay.h"

/* ---- shadow buffer ---- */
/* Each VGA plane is 64 KB on real hardware; the assembly code uses 16-bit DI
   arithmetic that can transiently address offsets 64000..65535 (e.g. when the
   image starts above y=0).  Allocating exactly 65536 bytes per plane matches
   the real hardware and prevents those transient accesses from overflowing. */
BYTE  g_vga_plane[4][65536];
BYTE *g_plane_ptrs[4] = {
    g_vga_plane[0],
    g_vga_plane[1],
    g_vga_plane[2],
    g_vga_plane[3]
};

/* Current write-plane pointer (DWORD holds pointer value) */
DWORD vga_base_p = 0;   /* initialised in shim_vid_init */

/* ---- SDL2 state ---- */
SDL_Color    g_palette[256];
SDL_Window  *g_window   = NULL;
SDL_Surface *g_surface  = NULL;   /* kept for API compat; not used for display */

static SDL_Renderer *s_renderer = NULL;
static SDL_Texture  *s_texture  = NULL;
static Uint32        s_argb_buf[640*400];  /* ARGB8888 conversion buffer */

/* ---- init / shutdown ---- */

__attribute__((force_align_arg_pointer))
void shim_vid_init(void)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_Init failed", MB_OK|MB_ICONERROR);
        ExitProcess(1);
    }

    g_window = SDL_CreateWindow(
        "Image Tool",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1280, 820,      /* 2x logical (640x410) at startup */
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!g_window) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_CreateWindow failed", MB_OK|MB_ICONERROR);
        ExitProcess(1);
    }
    /* Prevent shrinking below 1x logical — smaller than that becomes unreadable. */
    SDL_SetWindowMinimumSize(g_window, 640, 410);

    s_renderer = SDL_CreateRenderer(g_window, -1,
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!s_renderer) {
        /* fallback to software renderer */
        s_renderer = SDL_CreateRenderer(g_window, -1, SDL_RENDERER_SOFTWARE);
        if (!s_renderer) {
            MessageBoxA(NULL, SDL_GetError(), "SDL_CreateRenderer failed", MB_OK|MB_ICONERROR);
            ExitProcess(1);
        }
    }

    /* Logical-size rendering: 640x410 (10px menu strip + 640x400 VGA).
     * SDL scales this up to the window, and integer-scale mode clamps
     * to whole multiples so pixels stay crisp. */
    SDL_RenderSetLogicalSize(s_renderer, 640, 410);
    SDL_RenderSetIntegerScale(s_renderer, SDL_TRUE);

    s_texture = SDL_CreateTexture(s_renderer,
        SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING,
        640, 400);
    if (!s_texture) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_CreateTexture failed", MB_OK|MB_ICONERROR);
        ExitProcess(1);
    }

    /* default initialise palette to greyscale */
    {
        int i;
        for (i = 0; i < 256; i++) {
            g_palette[i].r = (BYTE)i;
            g_palette[i].g = (BYTE)i;
            g_palette[i].b = (BYTE)i;
            g_palette[i].a = 0xFF;
        }
    }

    /* seed vga_base_p to plane 0 */
    vga_base_p = (DWORD)(UINT_PTR)g_vga_plane[0];

    /* clear shadow buffer */
    shim_scr_clr();

    /* Initialize ImGui overlay */
    imgui_overlay_init(g_window, s_renderer, s_texture);
}

__attribute__((force_align_arg_pointer))
void shim_vid_shutdown(void)
{
    imgui_overlay_shutdown();
    if (s_texture)  { SDL_DestroyTexture(s_texture);   s_texture  = NULL; }
    if (s_renderer) { SDL_DestroyRenderer(s_renderer); s_renderer = NULL; }
    if (g_window)   { SDL_DestroyWindow(g_window);     g_window   = NULL; }
    SDL_Quit();
}

/* ---- suppress gadget region (DOS UI area) ---- */
/* The asm draws gadgets (canvas frame, status bar, toolbars, windows) throughout
   the VGA plane. Since ImGui now owns all UI, suppress DOS gadgets entirely. */
static void suppress_gadget_region(void)
{
    int x, y;
    /* Clear the entire screen to suppress all DOS gadgets, menus, windows, and UI elements.
       ImGui will draw all necessary UI on top. This prevents stale gadget frames
       and DOS window decorations from appearing behind ImGui panels. */
    for (y = 0; y < 400; y++) {
        for (x = 0; x < 640; x++) {
            /* Write palette index 0 (black/background) to clear all gadgets */
            g_vga_plane[x & 3][y*160 + (x >> 2)] = 0;
        }
    }
}

/* ---- present: deplanarize + palette expand + upload texture ---- */

__attribute__((force_align_arg_pointer))
void shim_vid_present(void)
{
    if (!s_renderer || !s_texture) return;

    /* Suppress old DOS gadgets now that ImGui owns the UI */
    suppress_gadget_region();

    /* Deplanarize and convert indexed → ARGB8888 */
    {
        int x, y;
        for (y = 0; y < 400; y++) {
            for (x = 0; x < 640; x++) {
                BYTE idx = g_vga_plane[x & 3][y*160 + (x >> 2)];
                SDL_Color c = g_palette[idx];
                s_argb_buf[y*640 + x] =
                    (0xFF000000u) |
                    ((Uint32)c.r << 16) |
                    ((Uint32)c.g <<  8) |
                    ((Uint32)c.b);
            }
        }
    }

    SDL_UpdateTexture(s_texture, NULL, s_argb_buf, 640 * sizeof(Uint32));
    /* Clear to black so letterbox bars outside the integer-scaled
     * logical area don't show stale pixels. */
    SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
    SDL_RenderClear(s_renderer);
    /* Logical coordinate system is 640x410. SDL handles scaling.
     * Canvas fills entire viewport; ImGui owns the UI layout. */
    {
        SDL_Rect dst = { 0, 0, 640, 410 };
        SDL_RenderCopy(s_renderer, s_texture, NULL, &dst);
    }

    /* Render ImGui overlay on top of canvas */
    imgui_overlay_newframe();
    imgui_overlay_render();

    SDL_RenderPresent(s_renderer);
}

/* ---- palette ---- */

/* Called from asm relay:
   shim_eax (low byte) = 1st DAC color #
   shim_ecx (low word) = # colors
   shim_esi             = pointer to 18-bit (6-bit per channel) RGB triplets */
__attribute__((force_align_arg_pointer))
void shim_setvgapal18_impl(void)
{
    BYTE    first = (BYTE)(shim_eax & 0xFF);
    int     count = (int)(shim_ecx & 0xFFFF);
    const BYTE *src = (const BYTE *)(UINT_PTR)shim_esi;
    int i;

    for (i = 0; i < count && (first + i) < 256; i++) {
        BYTE r6 = *src++;
        BYTE g6 = *src++;
        BYTE b6 = *src++;
        int idx = first + i;
        /* 6-bit → 8-bit: shift left 2, duplicate top 2 bits in low 2 */
        g_palette[idx].r = (BYTE)((r6 << 2) | (r6 >> 4));
        g_palette[idx].g = (BYTE)((g6 << 2) | (g6 >> 4));
        g_palette[idx].b = (BYTE)((b6 << 2) | (b6 >> 4));
        g_palette[idx].a = 0xFF;
    }
}

/* Called from asm relay:
   shim_eax (low byte) = 1st DAC color #
   shim_ecx (low word) = # colors
   shim_esi             = pointer to packed 15-bit RGB words (GGGBBBBB XRRRRRGG) */
__attribute__((force_align_arg_pointer))
void shim_setvgapal15_impl(void)
{
    BYTE    first = (BYTE)(shim_eax & 0xFF);
    int     count = (int)(shim_ecx & 0xFFFF);
    const BYTE *src = (const BYTE *)(UINT_PTR)shim_esi;
    int i;

    for (i = 0; i < count && (first + i) < 256; i++) {
        /* DOS 15-bit format: word = XRRRRRGG GGGBBBBB (little-endian) */
        BYTE lo = *src++;
        BYTE hi = *src++;
        WORD w  = (WORD)(lo | (hi << 8));
        int idx = first + i;
        BYTE r5 = (BYTE)((w >> 10) & 0x1F);
        BYTE g5 = (BYTE)((w >>  5) & 0x1F);
        BYTE b5 = (BYTE)( w        & 0x1F);
        /* 5-bit → 8-bit */
        g_palette[idx].r = (BYTE)((r5 << 3) | (r5 >> 2));
        g_palette[idx].g = (BYTE)((g5 << 3) | (g5 >> 2));
        g_palette[idx].b = (BYTE)((b5 << 3) | (b5 >> 2));
        g_palette[idx].a = 0xFF;
    }
}

/* ---- clear screen ---- */

__attribute__((force_align_arg_pointer))
void shim_scr_clr(void)
{
    memset(g_vga_plane, 0, sizeof(g_vga_plane));
}

/* Replaces ds:[46Ch] BIOS 18.2Hz tick counter used in test_main benchmark */
__attribute__((force_align_arg_pointer))
DWORD shim_gettick(void)
{
    return GetTickCount();
}

/* Clear 320x200 image window: 80 bytes/row (320/4), 200 rows, all 4 planes */
__attribute__((force_align_arg_pointer))
void shim_iwin_clr(void)
{
    int p, y;
    for (p = 0; p < 4; p++)
        for (y = 0; y < 200; y++)
            memset(&g_vga_plane[p][y * 160], 0, 80);
}

/* ---- ImGui overlay access ---- */

SDL_Renderer *shim_get_renderer(void)
{
    return s_renderer;
}

SDL_Texture *shim_get_canvas_tex(void)
{
    return s_texture;
}
