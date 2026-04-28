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

/* Check if ImGui wants to capture mouse or keyboard input.
   Returns 1 if ImGui is hovering/interacting with UI, 0 otherwise.
   Used to suppress asm menus when ImGui panels are active. */
int imgui_overlay_wants_input(void);

/* Check if ImGui specifically wants keyboard input (e.g. text boxes).
   Returns 1 if keyboard is captured, 0 otherwise. */
int imgui_overlay_wants_keyboard(void);

/* Check if there are unsaved changes and show confirmation if needed.
   Returns 1 if confirmed quit (or no changes), 0 if user cancelled. */
int imgui_overlay_check_unsaved_and_quit(void);

/* Called after a successful save to mark version as saved. */
void imgui_overlay_mark_saved(void);

/* Signal that the app should quit (from menu or window close).
   The render loop will show unsaved changes prompt if needed. */
void imgui_overlay_request_quit(void);

/* Returns 1 when it is safe to exit the main loop (no unsaved
   changes, or user confirmed via the unsaved changes popup). */
int imgui_overlay_should_quit(void);

/* Composite and present the ImGui draw data to the SDL renderer.
   Must be called after imgui_overlay_render() and before SDL_RenderPresent(). */
void imgui_overlay_present(void);

#ifdef __cplusplus
}
#endif
