/*****************************************************************************
 * platform/main.cpp
 *
 * Program entry point: SDL2 init, window/renderer creation, and the event
 * loop. Everything else (UI, file I/O, palette/sprite editing) is driven by
 * the ImGui overlay module.
 *
 * Lineage: This is the modernized C++ descendant of the original 1992 DOS
 * IT.EXE entry point by Shawn Liptak / Williams Electronics. The DOS asm
 * entry, Watcom DOS/4GW wrapper, and gadget-based UI are gone — only the
 * shape of "init SDL, run loop, shut down" survives.
 *****************************************************************************/

#include <cstring>
#include <cstdlib>
#include "compat.h"
#include <SDL.h>
#include "shim_file.h"
#include "imgui_overlay.h"

#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  ifdef __APPLE__
#    include <mach-o/dyld.h>
#  endif
#endif

/* Populate shim_file's exe_dir so legacy "c:\bin\" path remapping resolves
   against the directory the executable lives in. */
static void capture_exe_dir(int argc, char **argv)
{
    char full[MAX_PATH] = {0};
#ifdef _WIN32
    (void)argc; (void)argv;
    if (GetModuleFileNameA(NULL, full, sizeof(full))) {
        char *slash = strrchr(full, '\\');
        if (slash) { *slash = '\0'; strncpy(exe_dir, full, MAX_PATH - 1); }
    }
#else
    ssize_t len = -1;
#  ifdef __APPLE__
    /* macOS: /proc/self/exe doesn't exist; use the dyld API instead. */
    uint32_t sz = sizeof(full);
    if (_NSGetExecutablePath(full, &sz) == 0) {
        len = (ssize_t)strlen(full);
    }
#  else
    len = readlink("/proc/self/exe", full, sizeof(full) - 1);
#  endif
    if (len < 0 && argc > 0) {  /* fallback to argv[0] when the OS lookup fails */
        strncpy(full, argv[0], sizeof(full) - 1);
        len = (ssize_t)strlen(full);
    }
    if (len > 0) {
        full[len] = '\0';
        char *slash = strrchr(full, '/');
        if (slash) { *slash = '\0'; strncpy(exe_dir, full, MAX_PATH - 1); }
    }
#endif
}

int main(int argc, char *argv[])
{
    capture_exe_dir(argc, argv);

    /* ---- Init SDL2 ---- */
    SDL_SetMainReady();
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        MessageBoxA(NULL, SDL_GetError(), "SDL_Init Error", MB_OK | MB_ICONERROR);
        return 1;
    }

    SDL_Window *window = SDL_CreateWindow("Imgtool",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        1024, 768, SDL_WINDOW_RESIZABLE);
    if (!window) { SDL_Quit(); return 1; }

    SDL_Renderer *renderer = SDL_CreateRenderer(window, -1,
        SDL_RENDERER_PRESENTVSYNC | SDL_RENDERER_ACCELERATED);
    if (!renderer) { SDL_DestroyWindow(window); SDL_Quit(); return 1; }

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    /* 640x480 canvas matches the original VGA resolution the editor was
       built around; the ImGui overlay zooms this into the visible canvas. */
    SDL_Texture *canvas_tex = SDL_CreateTexture(renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 640, 480);

    imgui_overlay_init(window, renderer, canvas_tex);

    /* ---- Main loop ----
     *
     * Idle-friendly: SDL_WaitEventTimeout blocks when nothing's happening
     * instead of spinning a core at 100%. The 16 ms timeout is one frame at
     * 60 Hz, so reactive elements (tooltip fades, mouse-button-held
     * dragging, popup animations) catch up within a single frame.
     *
     * We unconditionally re-render after the wait — ImGui needs at least
     * one frame to settle after every state change. */
    bool running = true;
    while (running) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 16)) {
            do {
                imgui_overlay_process_event(&e);
                if (e.type == SDL_QUIT)
                    imgui_overlay_request_quit();
            } while (SDL_PollEvent(&e));
        }

        SDL_SetRenderDrawColor(renderer, 0x06, 0x06, 0x06, 0xFF);
        SDL_RenderClear(renderer);

        imgui_overlay_newframe();
        imgui_overlay_render();
        imgui_overlay_present();

        SDL_RenderPresent(renderer);

        if (imgui_overlay_should_quit())
            running = false;
    }

    imgui_overlay_shutdown();
    SDL_DestroyTexture(canvas_tex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
