/*************************************************************
 * platform/imgui_overlay.h
 * ImGui-based UI overlay for modernizing the DOS UI
 * Provides C-callable functions to init/render the overlay
 *************************************************************/
#pragma once
#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize ImGui overlay with SDL window and renderer.
   Called once during startup after SDL objects are created. */
void imgui_overlay_init(SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *canvas_texture);

/* Process an SDL_Event through ImGui (IO layer).
   Called for every event from SDL_PollEvent. */
void imgui_overlay_process_event(SDL_Event *event);

/* Start a new ImGui frame. Called before rendering.
   Must be called after ImGui_ImplSDL2_NewFrame(). */
void imgui_overlay_newframe(void);

/* Render all ImGui UI and composite onto the SDL renderer.
   Must be called before SDL_RenderPresent(). */
void imgui_overlay_render(void);

/* Shut down ImGui and free resources. Called on app exit. */
void imgui_overlay_shutdown(void);

/* Inject a synthetic keycode into the input queue (for menu commands).
   Allows ImGui menu items to trigger asm key handlers. */
void imgui_overlay_inject_key(unsigned short keycode);

/* Check if ImGui wants to capture mouse or keyboard input.
   Returns 1 if ImGui is hovering/interacting with UI, 0 otherwise.
   Used to suppress asm menus when ImGui panels are active. */
int imgui_overlay_wants_input(void);

#ifdef __cplusplus
}
#endif
