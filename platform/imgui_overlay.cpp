/*************************************************************
 * platform/imgui_overlay.cpp
 * ImGui-based UI overlay for modernizing the DOS UI
 * Phase 2: Complete panels for image list, palettes, properties, swatches
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

/* Link asm-side symbols defined in itimg.asm (COFF has no leading underscore from asm) */
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
    void *nxt_p;              /* +0: * next IMG or 0 */
    char n_s[16];             /* +4: Name (15+1) */
    unsigned short flags;     /* +20: Flags (B0=Marked) */
    unsigned short anix;      /* +22: Ani pt X */
    unsigned short aniy;      /* +24: Ani pt Y */
    unsigned short w;         /* +26: Width */
    unsigned short h;         /* +28: Height */
    unsigned short palnum;    /* +30: Palette index */
    void *data_p;             /* +32: * to image data */
    void *pttbl_p;            /* +36: * point table or 0 */
    unsigned short anix2;     /* +40: 2nd anipt X */
    unsigned short aniy2;     /* +42: 2nd anipt Y */
    unsigned short aniz2;     /* +44: 2nd anipt Z */
    unsigned short opals;     /* +46: * to alternate pal */
    void *temp;               /* +48: Temp offset */
};

struct PAL {
    void *nxt_p;              /* +0: * next PAL or 0 */
    char n_s[10];             /* +4: Name (9+1) */
    unsigned char flags;      /* +14: Flags (B0=Marked) */
    unsigned char bitspix;    /* +15: Bits per pixel */
    unsigned short numc;      /* +16: # of colors */
    unsigned short pad;       /* +18: padding */
    void *data_p;             /* +20: * to palette data */
    void *temp;               /* +24: Temp offset */
};
#pragma pack(pop)

/* Global state */
static SDL_Window *g_imgui_window = NULL;
static SDL_Renderer *g_imgui_renderer = NULL;
static SDL_Texture *g_canvas_texture = NULL;

/* Key injection ring buffer */
static struct {
    unsigned short buffer[64];
    int head;
    int tail;
} g_key_inject = { {0}, 0, 0 };

/* Panel state */
static bool show_image_list = true;
static bool show_palette_list = true;
static bool show_properties = true;
static bool show_palette_swatches = true;

/* Cache state */
static int g_selected_image_idx = -1;
static int g_selected_palette_idx = -1;
static int g_selected_color_idx = 0;

/* Dialog state for palette operations */
static bool show_palette_rename = false;
static bool show_palette_delete = false;
static bool show_palette_merge = false;
static int palette_op_index = -1;
static char palette_rename_buffer[10] = {0};
static int palette_merge_target = -1;

extern "C" {
    /* Relay globals from shim_input.c and shim_vid.c */
    extern unsigned int shim_ebx, shim_ecx, shim_edx;
    extern unsigned short shim_keycode;
    extern int shim_zf;

    /* VGA palette from shim_vid.c */
    extern SDL_Color g_palette[256];

    /* Asm image/palette lists (from itimg.asm) */
    extern void *img_p;              /* * to first IMG struct or NULL */
    extern unsigned int imgcnt;      /* Number of images */
    extern int ilselected;           /* Currently selected image (-1 = none) */
    extern void *pal_p;              /* * to first PAL struct or NULL */
    extern unsigned int palcnt;      /* Number of palettes */
    extern int plselected;           /* Currently selected palette (-1 = none) */
}

/* Helper: count images in the linked list */
static int count_images(void) {
    int count = 0;
    IMG *p = (IMG *)img_p;
    while (p) {
        count++;
        p = (IMG *)p->nxt_p;
    }
    return count;
}

/* Helper: get image by index */
static IMG *get_image_by_index(int idx) {
    IMG *p = (IMG *)img_p;
    int i = 0;
    while (p && i < idx) {
        p = (IMG *)p->nxt_p;
        i++;
    }
    return p;
}

/* Helper: count palettes in the linked list */
static int count_palettes(void) {
    int count = 0;
    PAL *p = (PAL *)pal_p;
    while (p) {
        count++;
        p = (PAL *)p->nxt_p;
    }
    return count;
}

/* Helper: get palette by index */
static PAL *get_palette_by_index(int idx) {
    PAL *p = (PAL *)pal_p;
    int i = 0;
    while (p && i < idx) {
        p = (PAL *)p->nxt_p;
        i++;
    }
    return p;
}

void imgui_overlay_init(SDL_Window *window, SDL_Renderer *renderer, SDL_Texture *canvas_texture)
{
    g_imgui_window = window;
    g_imgui_renderer = renderer;
    g_canvas_texture = canvas_texture;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();

    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();

    ImGui_ImplSDL2_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer2_Init(renderer);

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
                /* No standard key */
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
                /* TODO */
            }
            if (ImGui::MenuItem("Build TGA", "Ctrl+B")) {
                imgui_overlay_inject_key(0x02);  /* Ctrl+B */
            }
            ImGui::EndMenu();
        }

        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Image List", NULL, &show_image_list);
            ImGui::MenuItem("Palette List", NULL, &show_palette_list);
            ImGui::MenuItem("Properties", NULL, &show_properties);
            ImGui::MenuItem("Palette Swatches", NULL, &show_palette_swatches);
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

    float menu_height = ImGui::GetFrameHeight();

    /* Canvas panel (left/center, resizable) */
    ImGui::SetNextWindowPos(ImVec2(0, menu_height), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x * 0.65f, io.DisplaySize.y - menu_height), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Canvas", NULL, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = 640.0f / 400.0f;
        float w = avail.x;
        float h = w / aspect;
        if (h > avail.y) {
            h = avail.y;
            w = h * aspect;
        }
        ImGui::Image((ImTextureID)(intptr_t)g_canvas_texture, ImVec2(w, h));
        ImGui::End();
    }

    /* Right sidebar: image list, palette list, properties */
    float right_x = io.DisplaySize.x * 0.65f;
    float right_w = io.DisplaySize.x * 0.35f;

    /* Image List Panel */
    if (show_image_list) {
        ImGui::SetNextWindowPos(ImVec2(right_x, menu_height), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(right_w, io.DisplaySize.y * 0.35f - menu_height / 2), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Images", &show_image_list)) {
            int img_count = count_images();
            if (img_count > 0) {
                ImGui::Text("Images: %d", img_count);
                ImGui::Separator();

                if (ImGui::BeginListBox("##image_list", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()))) {
                    for (int i = 0; i < img_count; i++) {
                        IMG *img = get_image_by_index(i);
                        if (!img) break;

                        bool is_marked = (img->flags & 1) != 0;
                        bool is_selected = (i == ilselected);

                        ImGui::PushID(i);

                        if (ImGui::Selectable(img->n_s, is_selected)) {
                            /* Click to select — inject up/down keys to move selection */
                            if (i > ilselected && ilselected >= 0) {
                                for (int j = ilselected; j < i; j++) {
                                    imgui_overlay_inject_key(0x5000);  /* Down arrow */
                                }
                            } else if (i < ilselected) {
                                for (int j = i; j < ilselected; j++) {
                                    imgui_overlay_inject_key(0x4800);  /* Up arrow */
                                }
                            } else {
                                imgui_overlay_inject_key(0x5000);  /* First down arrow */
                            }
                        }

                        /* Right-click context menu */
                        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
                            /* Set selection first */
                            if (i != ilselected) {
                                imgui_overlay_inject_key(0x5000);
                            }
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndListBox();
                }
            } else {
                ImGui::Text("No images loaded.");
            }
            ImGui::End();
        }
    }

    /* Palette List Panel */
    if (show_palette_list) {
        ImGui::SetNextWindowPos(ImVec2(right_x, menu_height + io.DisplaySize.y * 0.35f - menu_height / 2), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(right_w, io.DisplaySize.y * 0.25f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Palettes", &show_palette_list)) {
            int pal_count = count_palettes();
            if (pal_count > 0) {
                ImGui::Text("Palettes: %d", pal_count);
                ImGui::Separator();

                if (ImGui::BeginListBox("##palette_list", ImVec2(-1, -ImGui::GetFrameHeightWithSpacing()))) {
                    for (int i = 0; i < pal_count; i++) {
                        PAL *pal = get_palette_by_index(i);
                        if (!pal) break;

                        bool is_selected = (i == plselected);

                        ImGui::PushID(2000 + i);

                        if (ImGui::Selectable(pal->n_s, is_selected)) {
                            g_selected_palette_idx = i;
                            /* TODO: inject key to trigger asm palette selection */
                        }

                        /* Right-click context menu for palette operations */
                        if (ImGui::BeginPopupContextItem("palette_context")) {
                            palette_op_index = i;
                            if (ImGui::MenuItem("Rename")) {
                                PAL *pal = get_palette_by_index(palette_op_index);
                                if (pal) {
                                    strncpy(palette_rename_buffer, pal->n_s, 9);
                                    palette_rename_buffer[9] = '\0';
                                }
                                show_palette_rename = true;
                            }
                            if (ImGui::MenuItem("Delete")) {
                                show_palette_delete = true;
                            }
                            if (ImGui::MenuItem("Merge with...")) {
                                show_palette_merge = true;
                                palette_merge_target = -1;
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::PopID();
                    }
                    ImGui::EndListBox();
                }
            } else {
                ImGui::Text("No palettes loaded.");
            }
            ImGui::End();
        }
    }

    /* Properties Panel */
    if (show_properties) {
        ImGui::SetNextWindowPos(ImVec2(right_x, menu_height + io.DisplaySize.y * 0.60f - menu_height), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(right_w, io.DisplaySize.y * 0.25f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Properties", &show_properties)) {
            IMG *current_img = (ilselected >= 0) ? get_image_by_index(ilselected) : NULL;
            if (current_img) {
                ImGui::Text("Name: %s", current_img->n_s);
                ImGui::Text("Size: %d x %d", current_img->w, current_img->h);
                ImGui::Text("Palette: %d", current_img->palnum);
                ImGui::Text("Anipt: (%d, %d)", current_img->anix, current_img->aniy);
                ImGui::Text("Marked: %s", (current_img->flags & 1) ? "Yes" : "No");
            } else {
                ImGui::Text("Select an image to see properties.");
            }
            ImGui::End();
        }
    }

    /* Palette Swatches Panel (bottom) */
    if (show_palette_swatches) {
        ImGui::SetNextWindowPos(ImVec2(0, io.DisplaySize.y * 0.8f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(io.DisplaySize.x, io.DisplaySize.y * 0.2f), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Palette", &show_palette_swatches)) {
            ImGui::Text("Color index: %d", g_selected_color_idx);
            ImGui::Separator();

            /* 16x16 swatch grid */
            ImDrawList *draw_list = ImGui::GetWindowDrawList();
            ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
            ImVec2 swatch_size(16.0f, 16.0f);
            ImVec2 spacing(2.0f, 2.0f);

            for (int row = 0; row < 16; row++) {
                for (int col = 0; col < 16; col++) {
                    int color_idx = row * 16 + col;
                    SDL_Color c = g_palette[color_idx];
                    ImVec4 color_normalized(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, 1.0f);

                    ImVec2 pos(canvas_pos.x + col * (swatch_size.x + spacing.x),
                               canvas_pos.y + row * (swatch_size.y + spacing.y));
                    ImVec2 pos_max(pos.x + swatch_size.x, pos.y + swatch_size.y);

                    /* Draw swatch */
                    ImU32 col_u32 = ImGui::GetColorU32(color_normalized);
                    draw_list->AddRectFilled(pos, pos_max, col_u32);

                    /* Border for selected color */
                    if (color_idx == g_selected_color_idx) {
                        draw_list->AddRect(pos, pos_max, IM_COL32(255, 255, 255, 255), 0, 0, 2.0f);
                    }

                    /* Detect click */
                    ImGui::SetCursorScreenPos(pos);
                    ImGui::InvisibleButton(("##swatch_" + std::to_string(color_idx)).c_str(), swatch_size);
                    if (ImGui::IsItemClicked()) {
                        g_selected_color_idx = color_idx;
                    }
                }
            }

            ImGui::NewLine();
            ImGui::Separator();

            /* Color editor sliders */
            SDL_Color current_color = g_palette[g_selected_color_idx];
            int r = current_color.r;
            int g = current_color.g;
            int b = current_color.b;

            bool changed = false;
            changed |= ImGui::SliderInt("R##color_r", &r, 0, 255);
            changed |= ImGui::SliderInt("G##color_g", &g, 0, 255);
            changed |= ImGui::SliderInt("B##color_b", &b, 0, 255);

            if (changed) {
                g_palette[g_selected_color_idx] = {(unsigned char)r, (unsigned char)g, (unsigned char)b, 0xFF};
                /* TODO: Call shim to update asm palette */
            }

            ImGui::End();
        }
    }

    /* Palette Rename Dialog */
    if (show_palette_rename) {
        if (ImGui::BeginPopupModal("Rename Palette", &show_palette_rename, ImGuiWindowFlags_AlwaysAutoResize)) {
            PAL *pal = (palette_op_index >= 0) ? get_palette_by_index(palette_op_index) : NULL;
            if (pal) {
                ImGui::Text("Renaming: %s", pal->n_s);
                ImGui::InputText("##rename_input", palette_rename_buffer, sizeof(palette_rename_buffer));
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    /* TODO: inject key or call shim to update pal->n_s in asm */
                    strncpy(palette_rename_buffer, pal->n_s, 9);
                    show_palette_rename = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    show_palette_rename = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    /* Palette Delete Dialog */
    if (show_palette_delete) {
        if (ImGui::BeginPopupModal("Delete Palette", &show_palette_delete, ImGuiWindowFlags_AlwaysAutoResize)) {
            PAL *pal = (palette_op_index >= 0) ? get_palette_by_index(palette_op_index) : NULL;
            if (pal) {
                ImGui::Text("Delete palette '%s'? This cannot be undone.", pal->n_s);
                ImGui::Separator();
                if (ImGui::Button("Delete", ImVec2(120, 0))) {
                    /* TODO: inject key or call shim to delete palette from asm */
                    show_palette_delete = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    show_palette_delete = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    /* Palette Merge Dialog */
    if (show_palette_merge) {
        if (ImGui::BeginPopupModal("Merge Palettes", &show_palette_merge, ImGuiWindowFlags_AlwaysAutoResize)) {
            PAL *pal = (palette_op_index >= 0) ? get_palette_by_index(palette_op_index) : NULL;
            if (pal) {
                ImGui::Text("Merge '%s' with:", pal->n_s);
                ImGui::Separator();
                if (ImGui::BeginListBox("##merge_target", ImVec2(-1, 200))) {
                    int pal_count = count_palettes();
                    for (int i = 0; i < pal_count; i++) {
                        if (i == palette_op_index) continue;  /* Skip self */
                        PAL *target = get_palette_by_index(i);
                        if (!target) break;
                        bool is_selected = (i == palette_merge_target);
                        if (ImGui::Selectable(target->n_s, is_selected)) {
                            palette_merge_target = i;
                        }
                    }
                    ImGui::EndListBox();
                }
                ImGui::Separator();
                bool can_merge = (palette_merge_target >= 0 && palette_merge_target != palette_op_index);
                if (!can_merge) ImGui::BeginDisabled();
                if (ImGui::Button("Merge", ImVec2(120, 0))) {
                    /* TODO: inject key or call shim to merge palettes */
                    show_palette_merge = false;
                    ImGui::CloseCurrentPopup();
                }
                if (!can_merge) ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    show_palette_merge = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    /* Render ImGui */
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
    int next_head = (g_key_inject.head + 1) % 64;
    if (next_head != g_key_inject.tail) {
        g_key_inject.buffer[g_key_inject.head] = keycode;
        g_key_inject.head = next_head;
    }
}

extern "C" int imgui_overlay_get_injected_key(unsigned short *keycode)
{
    if (g_key_inject.tail != g_key_inject.head) {
        *keycode = g_key_inject.buffer[g_key_inject.tail];
        g_key_inject.tail = (g_key_inject.tail + 1) % 64;
        return 1;
    }
    return 0;
}

extern "C" int imgui_overlay_wants_input(void)
{
    ImGuiIO &io = ImGui::GetIO();
    return io.WantCaptureMouse || io.WantCaptureKeyboard;
}
