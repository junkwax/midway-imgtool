/*************************************************************
 * platform/imgui_overlay.cpp
 * ImGui-based UI overlay for modernizing the DOS UI
 * Composites on top of SDL renderer after VGA texture blit
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>
#include <SDL.h>
#include <cstdint>
#include "imgui_overlay.h"

/* Global state */
static SDL_Window *g_imgui_window = NULL;
static SDL_Renderer *g_imgui_renderer = NULL;
static SDL_Texture *g_canvas_texture = NULL;

/* Simple key injection ring buffer (64 entries, matching the asm key queue) */
static struct {
    unsigned short buffer[64];
    int head;
    int tail;
} g_key_inject = { {0}, 0, 0 };

/* Panel visibility state */
static bool show_image_list = true;
static bool show_palette_list = true;
static bool show_properties = true;
static bool show_palette_swatches = true;

void imgui_overlay_init(SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *canvas_texture)
{
    g_imgui_window = window;
    g_imgui_renderer = renderer;
    g_canvas_texture = canvas_texture;

    /* Setup ImGui context */
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    /* Docking requires the docking branch of ImGui, which we don't have.
       For Phase 1, just use basic window layout without docking. */
    /* io.ConfigFlags |= ImGuiConfigFlags_DockingEnable; */
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;

    /* Setup ImGui style (optional — use default for now) */
    ImGui::StyleColorsDark();

    /* Initialize platform and renderer backends */
    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

    /* Key injection buffer */
    g_key_inject.head = 0;
    g_key_inject.tail = 0;
}

void imgui_overlay_process_event(SDL_Event *event)
{
    ImGui_ImplSDL2_ProcessEvent(event);
}

void imgui_overlay_newframe(void)
{
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
}

void imgui_overlay_render(void)
{
    ImGuiIO &io = ImGui::GetIO();

    /* Main menu bar */
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open...", "Ctrl+O")) {
                imgui_overlay_inject_key(0x0C);  /* Ctrl+O */
            }
            if (ImGui::MenuItem("Save", "Ctrl+S")) {
                imgui_overlay_inject_key(0x13);  /* Ctrl+S */
            }
            if (ImGui::MenuItem("Save Raw")) {
                /* TODO: key for raw save */
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Ctrl+Q")) {
                imgui_overlay_inject_key(0x11);  /* Ctrl+Q */
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Edit")) {
            if (ImGui::MenuItem("Rename", "Ctrl+R")) {
                imgui_overlay_inject_key(0x12);  /* Ctrl+R */
            }
            if (ImGui::MenuItem("Delete", "Ctrl+D")) {
                imgui_overlay_inject_key(0x04);  /* Ctrl+D */
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Image")) {
            if (ImGui::MenuItem("Duplicate")) {
                /* TODO: key for duplicate */
            }
            if (ImGui::MenuItem("Build TGA", "Ctrl+B")) {
                imgui_overlay_inject_key(0x02);  /* Ctrl+B */
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            if (ImGui::MenuItem("Image List", NULL, &show_image_list)) {
            }
            if (ImGui::MenuItem("Palette List", NULL, &show_palette_list)) {
            }
            if (ImGui::MenuItem("Properties", NULL, &show_properties)) {
            }
            if (ImGui::MenuItem("Palette Swatches", NULL, &show_palette_swatches)) {
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("Help")) {
            if (ImGui::MenuItem("Help", "h")) {
                imgui_overlay_inject_key('h');
            }
            ImGui::EndMenu();
        }

        ImGui::EndMainMenuBar();
    }

    /* Phase 1: Simple layout without docking
       Canvas fills most of the window, with small side panels */

    ImGui::SetNextWindowPos(ImVec2(0, ImGui::GetFrameHeight()), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.75f, io.DisplaySize.y - ImGui::GetFrameHeight()), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Canvas", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        /* Maintain aspect ratio for the 640×400 canvas */
        float aspect = 640.0f / 400.0f;
        float w = avail.x;
        float h = w / aspect;
        if (h > avail.y) {
            h = avail.y;
            w = h * aspect;
        }
        /* Display the canvas texture as an ImGui image */
        ImGui::Image((ImTextureID)(intptr_t)g_canvas_texture, ImVec2(w, h));
        ImGui::End();
    }

    /* Right sidebar panels — Phase 2 TODO */

    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.75f, ImGui::GetFrameHeight()), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.25f, io.DisplaySize.y - ImGui::GetFrameHeight()), ImGuiCond_FirstUseEver);
    if (show_image_list && ImGui::Begin("Images", &show_image_list)) {
        ImGui::Text("Image list (Phase 2)");
        ImGui::End();
    }

    /* Render ImGui draw data */
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), g_imgui_renderer);
}

void imgui_overlay_shutdown(void)
{
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
}

void imgui_overlay_inject_key(unsigned short keycode)
{
    /* Push keycode into the ring buffer */
    int next_head = (g_key_inject.head + 1) % 64;
    if (next_head != g_key_inject.tail) {
        g_key_inject.buffer[g_key_inject.head] = keycode;
        g_key_inject.head = next_head;
    }
}

/* Called from shim_input.c to pop injected keys before the normal queue */
extern "C" int imgui_overlay_get_injected_key(unsigned short *keycode)
{
    if (g_key_inject.tail != g_key_inject.head) {
        *keycode = g_key_inject.buffer[g_key_inject.tail];
        g_key_inject.tail = (g_key_inject.tail + 1) % 64;
        return 1;  /* key available */
    }
    return 0;  /* no key */
}
