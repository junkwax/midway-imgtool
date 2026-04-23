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

/* Point editor state */
static bool show_point_editor = false;
static int point_editor_dragging = -1;  /* -1=none, 0=anix/y, 1=anix2/y2 */
static bool show_anim_points = true;
static int point_nudge_amount = 1;  /* Pixels to move with arrow keys */

/* Hitbox editor state */
static bool show_hitbox_editor = false;
static bool show_hitboxes_overlay = false;
static int hitbox_x = 0, hitbox_y = 0;        /* Hitbox top-left */
static int hitbox_w = 32, hitbox_h = 32;      /* Hitbox size */
static int hitbox_dragging_corner = -1;       /* -1=none, 0=TL, 1=TR, 2=BR, 3=BL */

/* Undo/redo system */
#define UNDO_STACK_SIZE 64
struct EditSnapshot {
    int image_idx;
    unsigned short anix, aniy;      /* Primary animation point */
    unsigned short anix2, aniy2;    /* Secondary animation point */
    unsigned short w, h;            /* Image dimensions */
    unsigned short palnum;          /* Palette index */
    unsigned short flags;           /* Flags (marked, etc.) */
    int hitbox_x, hitbox_y;         /* Hitbox position */
    int hitbox_w, hitbox_h;         /* Hitbox dimensions */
};
static EditSnapshot undo_stack[UNDO_STACK_SIZE];
static int undo_stack_idx = -1;     /* Current position in undo stack (-1 = empty) */
static int undo_stack_count = 0;    /* Number of valid undo entries */

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

/* Take snapshot of current image state for undo */
static void undo_snapshot(void) {
    IMG *current_img = (ilselected >= 0) ? get_image_by_index(ilselected) : NULL;
    if (!current_img) return;

    /* Only save if this is a new change (not just moving through history) */
    if (undo_stack_idx >= 0 && undo_stack_idx < UNDO_STACK_SIZE - 1) {
        /* Check if the next snapshot is identical (same point, same state) */
        EditSnapshot *last = &undo_stack[undo_stack_idx];
        if (last->image_idx == ilselected &&
            last->anix == current_img->anix && last->aniy == current_img->aniy &&
            last->anix2 == current_img->anix2 && last->aniy2 == current_img->aniy2) {
            return;  /* No change, don't snapshot */
        }
    }

    /* Add new snapshot and advance stack pointer */
    if (undo_stack_idx < UNDO_STACK_SIZE - 1) {
        undo_stack_idx++;
    } else {
        /* Shift stack left to make room at end */
        for (int i = 0; i < UNDO_STACK_SIZE - 1; i++) {
            undo_stack[i] = undo_stack[i + 1];
        }
    }

    EditSnapshot *snap = &undo_stack[undo_stack_idx];
    snap->image_idx = ilselected;
    snap->anix = current_img->anix;
    snap->aniy = current_img->aniy;
    snap->anix2 = current_img->anix2;
    snap->aniy2 = current_img->aniy2;
    snap->w = current_img->w;
    snap->h = current_img->h;
    snap->palnum = current_img->palnum;
    snap->flags = current_img->flags;
    snap->hitbox_x = hitbox_x;
    snap->hitbox_y = hitbox_y;
    snap->hitbox_w = hitbox_w;
    snap->hitbox_h = hitbox_h;

    undo_stack_count = undo_stack_idx + 1;
}

/* Restore image state from snapshot */
static void undo_restore(int snap_idx) {
    if (snap_idx < 0 || snap_idx >= undo_stack_count) return;

    EditSnapshot *snap = &undo_stack[snap_idx];
    IMG *img = get_image_by_index(snap->image_idx);
    if (!img) return;

    img->anix = snap->anix;
    img->aniy = snap->aniy;
    img->anix2 = snap->anix2;
    img->aniy2 = snap->aniy2;
    img->w = snap->w;
    img->h = snap->h;
    img->palnum = snap->palnum;
    img->flags = snap->flags;
    hitbox_x = snap->hitbox_x;
    hitbox_y = snap->hitbox_y;
    hitbox_w = snap->hitbox_w;
    hitbox_h = snap->hitbox_h;
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

    /* Note: Arrow key nudging deferred to Phase 6c+ pending keyboard enum resolution.
       Currently can use sliders in Point Editor / Hitbox Editor panels or drag on canvas. */

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
            bool can_undo = (undo_stack_idx > 0);
            bool can_redo = (undo_stack_idx < undo_stack_count - 1);

            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) {
                if (can_undo) {
                    undo_stack_idx--;
                    undo_restore(undo_stack_idx);
                }
            }
            if (!can_undo) ImGui::EndDisabled();

            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) {
                if (can_redo) {
                    undo_stack_idx++;
                    undo_restore(undo_stack_idx);
                }
            }
            if (!can_redo) ImGui::EndDisabled();

            ImGui::Separator();
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
            ImGui::Separator();
            ImGui::MenuItem("Animation Points", NULL, &show_anim_points);
            ImGui::MenuItem("Point Editor", NULL, &show_point_editor);
            ImGui::MenuItem("Hitboxes", NULL, &show_hitboxes_overlay);
            ImGui::MenuItem("Hitbox Editor", NULL, &show_hitbox_editor);
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

        ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
        ImVec2 canvas_size(w, h);
        ImGui::Image((ImTextureID)(intptr_t)g_canvas_texture, canvas_size);

        /* Draw animation points overlay if enabled */
        if (show_anim_points) {
            IMG *current_img = (ilselected >= 0) ? get_image_by_index(ilselected) : NULL;
            if (current_img && current_img->w > 0 && current_img->h > 0) {
                ImDrawList *draw_list = ImGui::GetWindowDrawList();
                float scale_x = canvas_size.x / 640.0f;
                float scale_y = canvas_size.y / 400.0f;
                ImGuiIO &io = ImGui::GetIO();
                ImVec2 mouse_pos = io.MousePos;
                bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

                /* Draw primary animation point (anix, aniy) */
                ImVec2 pt1_vga((float)current_img->anix, (float)current_img->aniy);
                ImVec2 pt1_screen(canvas_pos.x + pt1_vga.x * scale_x,
                                  canvas_pos.y + pt1_vga.y * scale_y);
                ImVec2 diff1 = mouse_pos - pt1_screen;
                float dist_to_pt1 = diff1.x*diff1.x + diff1.y*diff1.y;
                bool hovering_pt1 = (dist_to_pt1 < 10*10);  /* 10px hit radius */
                ImU32 pt1_color = hovering_pt1 ? IM_COL32(255, 100, 0, 255) : IM_COL32(255, 0, 0, 255);
                draw_list->AddCircleFilled(pt1_screen, 6.0f, pt1_color);
                draw_list->AddCircle(pt1_screen, 6.0f, IM_COL32(255, 255, 255, 255), 0, 1.5f);

                /* Handle point dragging */
                static bool dragging_pt1 = false;
                static bool dragging_pt1_started = false;
                if (hovering_pt1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    dragging_pt1 = true;
                    dragging_pt1_started = true;
                    undo_snapshot();  /* Save state before starting drag */
                }
                if (dragging_pt1 && mouse_down) {
                    /* Update point position while dragging */
                    ImVec2 relative = mouse_pos - canvas_pos;
                    int new_x = (int)(relative.x / scale_x);
                    int new_y = (int)(relative.y / scale_y);
                    /* Clamp to canvas bounds */
                    if (new_x < 0) new_x = 0; else if (new_x > 639) new_x = 639;
                    if (new_y < 0) new_y = 0; else if (new_y > 399) new_y = 399;
                    current_img->anix = (unsigned short)new_x;
                    current_img->aniy = (unsigned short)new_y;
                } else if (!mouse_down && dragging_pt1) {
                    dragging_pt1 = false;
                    if (dragging_pt1_started) {
                        undo_snapshot();  /* Save state after drag complete */
                        dragging_pt1_started = false;
                    }
                }

                /* Draw secondary animation point (anix2, aniy2) if exists */
                if (current_img->anix2 > 0 || current_img->aniy2 > 0) {
                    ImVec2 pt2_vga((float)current_img->anix2, (float)current_img->aniy2);
                    ImVec2 pt2_screen(canvas_pos.x + pt2_vga.x * scale_x,
                                      canvas_pos.y + pt2_vga.y * scale_y);
                    ImVec2 diff2 = mouse_pos - pt2_screen;
                    float dist_to_pt2 = diff2.x*diff2.x + diff2.y*diff2.y;
                    bool hovering_pt2 = (dist_to_pt2 < 10*10);
                    ImU32 pt2_color = hovering_pt2 ? IM_COL32(0, 255, 100, 255) : IM_COL32(0, 255, 0, 255);
                    draw_list->AddCircleFilled(pt2_screen, 6.0f, pt2_color);
                    draw_list->AddCircle(pt2_screen, 6.0f, IM_COL32(255, 255, 255, 255), 0, 1.5f);

                    /* Handle second point dragging */
                    static bool dragging_pt2 = false;
                    static bool dragging_pt2_started = false;
                    if (hovering_pt2 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                        dragging_pt2 = true;
                        dragging_pt2_started = true;
                        undo_snapshot();  /* Save state before starting drag */
                    }
                    if (dragging_pt2 && mouse_down) {
                        ImVec2 relative = mouse_pos - canvas_pos;
                        int new_x = (int)(relative.x / scale_x);
                        int new_y = (int)(relative.y / scale_y);
                        if (new_x < 0) new_x = 0; else if (new_x > 639) new_x = 639;
                        if (new_y < 0) new_y = 0; else if (new_y > 399) new_y = 399;
                        current_img->anix2 = (unsigned short)new_x;
                        current_img->aniy2 = (unsigned short)new_y;
                    } else if (!mouse_down && dragging_pt2) {
                        dragging_pt2 = false;
                        if (dragging_pt2_started) {
                            undo_snapshot();  /* Save state after drag complete */
                            dragging_pt2_started = false;
                        }
                    }

                    /* Draw line between points */
                    draw_list->AddLine(pt1_screen, pt2_screen, IM_COL32(255, 255, 0, 192), 1.0f);
                }
            }
        }

        /* Draw hitbox overlay if enabled */
        if (show_hitboxes_overlay) {
            IMG *current_img = (ilselected >= 0) ? get_image_by_index(ilselected) : NULL;
            if (current_img && current_img->w > 0 && current_img->h > 0) {
                ImDrawList *draw_list = ImGui::GetWindowDrawList();
                float scale_x = canvas_size.x / 640.0f;
                float scale_y = canvas_size.y / 400.0f;
                ImGuiIO &io = ImGui::GetIO();
                ImVec2 mouse_pos = io.MousePos;
                bool mouse_down = ImGui::IsMouseDown(ImGuiMouseButton_Left);

                /* Draw hitbox rectangle */
                ImVec2 box_tl_screen(canvas_pos.x + hitbox_x * scale_x,
                                     canvas_pos.y + hitbox_y * scale_y);
                ImVec2 box_br_screen(canvas_pos.x + (hitbox_x + hitbox_w) * scale_x,
                                     canvas_pos.y + (hitbox_y + hitbox_h) * scale_y);
                draw_list->AddRect(box_tl_screen, box_br_screen, IM_COL32(0, 255, 255, 255), 0, 0, 2.0f);

                /* Draw corners as draggable handles */
                float handle_size = 5.0f;
                ImVec2 box_tr_screen(box_br_screen.x, box_tl_screen.y);
                ImVec2 box_bl_screen(box_tl_screen.x, box_br_screen.y);

                /* Check corner hover distance */
                ImVec2 diff_tl = mouse_pos - box_tl_screen;
                ImVec2 diff_tr = mouse_pos - box_tr_screen;
                ImVec2 diff_br = mouse_pos - box_br_screen;
                ImVec2 diff_bl = mouse_pos - box_bl_screen;
                float dist_tl = diff_tl.x*diff_tl.x + diff_tl.y*diff_tl.y;
                float dist_tr = diff_tr.x*diff_tr.x + diff_tr.y*diff_tr.y;
                float dist_br = diff_br.x*diff_br.x + diff_br.y*diff_br.y;
                float dist_bl = diff_bl.x*diff_bl.x + diff_bl.y*diff_bl.y;
                float hit_radius = 12*12;  /* 12px hit radius */

                bool hovering_tl = (dist_tl < hit_radius);
                bool hovering_tr = (dist_tr < hit_radius);
                bool hovering_br = (dist_br < hit_radius);
                bool hovering_bl = (dist_bl < hit_radius);

                /* Handle corner dragging */
                if ((hovering_tl || hovering_tr || hovering_br || hovering_bl) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    if (hovering_tl) { hitbox_dragging_corner = 0; undo_snapshot(); }
                    else if (hovering_tr) { hitbox_dragging_corner = 1; undo_snapshot(); }
                    else if (hovering_br) { hitbox_dragging_corner = 2; undo_snapshot(); }
                    else if (hovering_bl) { hitbox_dragging_corner = 3; undo_snapshot(); }
                }

                if (hitbox_dragging_corner >= 0 && mouse_down) {
                    ImVec2 relative = mouse_pos - canvas_pos;
                    int mouse_vga_x = (int)(relative.x / scale_x);
                    int mouse_vga_y = (int)(relative.y / scale_y);
                    if (mouse_vga_x < 0) mouse_vga_x = 0; else if (mouse_vga_x > 639) mouse_vga_x = 639;
                    if (mouse_vga_y < 0) mouse_vga_y = 0; else if (mouse_vga_y > 399) mouse_vga_y = 399;

                    /* Resize based on which corner is being dragged */
                    if (hitbox_dragging_corner == 0) {  /* TL corner */
                        hitbox_x = mouse_vga_x;
                        hitbox_y = mouse_vga_y;
                        hitbox_w = hitbox_x + hitbox_w - mouse_vga_x;
                        hitbox_h = hitbox_y + hitbox_h - mouse_vga_y;
                    } else if (hitbox_dragging_corner == 1) {  /* TR corner */
                        hitbox_y = mouse_vga_y;
                        hitbox_w = mouse_vga_x - hitbox_x;
                        hitbox_h = hitbox_y + hitbox_h - mouse_vga_y;
                    } else if (hitbox_dragging_corner == 2) {  /* BR corner */
                        hitbox_w = mouse_vga_x - hitbox_x;
                        hitbox_h = mouse_vga_y - hitbox_y;
                    } else if (hitbox_dragging_corner == 3) {  /* BL corner */
                        hitbox_x = mouse_vga_x;
                        hitbox_w = hitbox_x + hitbox_w - mouse_vga_x;
                        hitbox_h = mouse_vga_y - hitbox_y;
                    }
                    /* Clamp to minimum size */
                    if (hitbox_w < 1) hitbox_w = 1;
                    if (hitbox_h < 1) hitbox_h = 1;
                    /* Clamp to canvas */
                    if (hitbox_x < 0) hitbox_x = 0;
                    if (hitbox_y < 0) hitbox_y = 0;
                    if (hitbox_x + hitbox_w > 640) hitbox_x = 640 - hitbox_w;
                    if (hitbox_y + hitbox_h > 400) hitbox_y = 400 - hitbox_h;
                } else if (!mouse_down) {
                    if (hitbox_dragging_corner >= 0) {
                        undo_snapshot();  /* Save final state after drag */
                    }
                    hitbox_dragging_corner = -1;
                }

                /* Draw corners with color feedback */
                ImU32 tl_color = hovering_tl ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 255, 255, 255);
                ImU32 tr_color = hovering_tr ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 255, 255, 255);
                ImU32 br_color = hovering_br ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 255, 255, 255);
                ImU32 bl_color = hovering_bl ? IM_COL32(255, 255, 0, 255) : IM_COL32(0, 255, 255, 255);
                draw_list->AddCircleFilled(box_tl_screen, handle_size, tl_color);
                draw_list->AddCircleFilled(box_tr_screen, handle_size, tr_color);
                draw_list->AddCircleFilled(box_br_screen, handle_size, br_color);
                draw_list->AddCircleFilled(box_bl_screen, handle_size, bl_color);
            }
        }

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
                    /* Copy new name into pal structure (asm will read on next redraw) */
                    strncpy(pal->n_s, palette_rename_buffer, 9);
                    pal->n_s[9] = '\0';
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
                ImGui::TextDisabled("(Requires asm-side support to unlink from pal_p list)");
                ImGui::Separator();
                ImGui::BeginDisabled(true);
                ImGui::Button("Delete", ImVec2(120, 0));
                ImGui::EndDisabled();
                ImGui::TextDisabled("Delete not yet implemented");
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
                ImGui::TextDisabled("(Requires asm-side support to update image palette references)");
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
                ImGui::Button("Merge", ImVec2(120, 0));
                if (!can_merge) ImGui::EndDisabled();
                ImGui::TextDisabled("Merge not yet implemented");
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                    show_palette_merge = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::EndPopup();
        }
    }

    /* Point Editor Panel */
    if (show_point_editor) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.65f, menu_height), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(right_w * 0.5f, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Point Editor", &show_point_editor)) {
            IMG *current_img = (ilselected >= 0) ? get_image_by_index(ilselected) : NULL;
            if (current_img) {
                ImGui::Text("Primary Point (Anipt 1):");
                ImGui::Text("  X: %d   Y: %d", current_img->anix, current_img->aniy);
                ImGui::SliderInt("##anix", (int *)&current_img->anix, 0, 639);
                ImGui::SliderInt("##aniy", (int *)&current_img->aniy, 0, 399);
                ImGui::Separator();

                ImGui::Text("Secondary Point (Anipt 2):");
                ImGui::Text("  X: %d   Y: %d", current_img->anix2, current_img->aniy2);
                ImGui::SliderInt("##anix2", (int *)&current_img->anix2, 0, 639);
                ImGui::SliderInt("##aniy2", (int *)&current_img->aniy2, 0, 399);
                ImGui::Separator();

                ImGui::SliderInt("Arrow Nudge Pixels", &point_nudge_amount, 1, 10);
                ImGui::TextDisabled("(Drag points on canvas or use sliders)");
                ImGui::TextDisabled("(Arrow keys: hold Shift+Ctrl, press arrow)");
            } else {
                ImGui::Text("Select an image to edit points.");
            }
            ImGui::End();
        }
    }

    /* Hitbox Editor Panel */
    if (show_hitbox_editor) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.65f, menu_height + 250), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(right_w * 0.5f, 200), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Hitbox Editor", &show_hitbox_editor)) {
            ImGui::Text("Collision Box:");
            ImGui::SliderInt("X##hbx", &hitbox_x, 0, 639);
            ImGui::SliderInt("Y##hby", &hitbox_y, 0, 399);
            ImGui::SliderInt("Width##hbw", &hitbox_w, 1, 640);
            ImGui::SliderInt("Height##hbh", &hitbox_h, 1, 400);
            ImGui::Separator();
            ImGui::Text("Box: (%d,%d) %dx%d", hitbox_x, hitbox_y, hitbox_w, hitbox_h);
            ImGui::TextDisabled("(Cyan box on canvas shows hitbox)");
            ImGui::TextDisabled("(Check 'Hitboxes' in View menu to display)");
        }
        ImGui::End();
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
