/*************************************************************
 * platform/ui_main.cpp
 * Main layout, dockspace, menu bar, and panels.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <SDL.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <regex>

#include "img_format.h"
#include "ui_internal.h"
#include "ui_main.h"
#include "ui_canvas.h"
#include "ui_timeline.h"
#include "ui_palette.h"
#include "ui_tools.h"
#include "world_render.h"
#include "anipoint.h"
#include "anipoint_edit.h"
#include "img_util.h"
#include "sprite_resize_ops.h"
#include "img_io.h"
#include "load2_verify.h"
#include "lod_parser.h"
#include "mk2_hitbox.h"
#include "mk2_fatality.h"
#include "compat.h"

extern "C" { extern struct SDL_Color g_palette[256]; }
extern int g_img_tex_idx;

static bool g_open_set_group_anipoints = false;
static int g_group_anipoint_x = 0;
static int g_group_anipoint_y = 0;
static bool g_group_set_x = true;
static bool g_group_set_y = true;
/* 0 preserves distance from the leading edge, 1 from center, 2 from trailing edge. */
static int g_group_x_basis = 1;
static int g_group_y_basis = 2;

static IMG *FirstMarkedImage(void)
{
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p)
        if (img->flags & 1) return img;
    return NULL;
}

static void OpenSetGroupAnipointsDialog(void)
{
    /* Image-list order defines the group reference, not whichever marked frame
       happens to be selected when the menu opens. */
    IMG *img = FirstMarkedImage();
    if (img) {
        g_group_anipoint_x = (int)(short)img->anix;
        g_group_anipoint_y = (int)(short)img->aniy;
    }
    g_open_set_group_anipoints = true;
}

static void DrawSetGroupAnipointsDialog(void)
{
    if (g_open_set_group_anipoints) {
        ImGui::OpenPopup("Set Group Animation Points");
        g_open_set_group_anipoints = false;
    }

    if (!ImGui::BeginPopupModal("Set Group Animation Points", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    int marked = CountMarkedImages();
    IMG *reference = FirstMarkedImage();
    ImGui::Text("Set the primary animation point for %d marked frame%s.",
                marked, marked == 1 ? "" : "s");
    if (reference)
        ImGui::TextDisabled("Reference: %.15s  %dx%d  AX/AY=%d,%d",
                            reference->n_s, (int)reference->w, (int)reference->h,
                            (int)(short)reference->anix, (int)(short)reference->aniy);
    ImGui::Spacing();
    ImGui::Checkbox("Set X", &g_group_set_x);
    ImGui::SameLine(120.0f);
    ImGui::BeginDisabled(!g_group_set_x);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("X value", &g_group_anipoint_x);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(145.0f);
    const char *x_basis[] = { "From Left", "From Center", "From Right" };
    ImGui::Combo("##group_x_basis", &g_group_x_basis, x_basis, 3);
    ImGui::EndDisabled();

    ImGui::Checkbox("Set Y", &g_group_set_y);
    ImGui::SameLine(120.0f);
    ImGui::BeginDisabled(!g_group_set_y);
    ImGui::SetNextItemWidth(110.0f);
    ImGui::InputInt("Y value", &g_group_anipoint_y);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(145.0f);
    const char *y_basis[] = { "From Top", "From Center", "From Bottom" };
    ImGui::Combo("##group_y_basis", &g_group_y_basis, y_basis, 3);
    ImGui::EndDisabled();

    ImGui::TextDisabled("Values apply to the first marked frame. Other frames compensate for size.");
    ImGui::TextDisabled("Leading edge: same value. Center: + half size delta. Trailing edge: + full delta.");

    bool valid = marked > 0 && reference && (g_group_set_x || g_group_set_y) &&
                 (!g_group_set_x || (g_group_anipoint_x >= -32768 && g_group_anipoint_x <= 32767)) &&
                 (!g_group_set_y || (g_group_anipoint_y >= -32768 && g_group_anipoint_y <= 32767));
    if (!valid) ImGui::BeginDisabled();
    if (ImGui::Button("Set Group", ImVec2(120, 0))) {
        int changed = 0;
        int reference_w = (int)reference->w;
        int reference_h = (int)reference->h;
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if (!(img->flags & 1)) continue;
            int dx_size = (int)img->w - reference_w;
            int dy_size = (int)img->h - reference_h;
            int next_x = g_group_anipoint_x +
                         (g_group_x_basis == 1 ? dx_size / 2 :
                          g_group_x_basis == 2 ? dx_size : 0);
            int next_y = g_group_anipoint_y +
                         (g_group_y_basis == 1 ? dy_size / 2 :
                          g_group_y_basis == 2 ? dy_size : 0);
            if (next_x < -32768) next_x = -32768;
            if (next_x > 32767) next_x = 32767;
            if (next_y < -32768) next_y = -32768;
            if (next_y > 32767) next_y = 32767;
            unsigned short ax = g_group_set_x ? signed_to_img_word(next_x) : img->anix;
            unsigned short ay = g_group_set_y ? signed_to_img_word(next_y) : img->aniy;
            if (img->anix == ax && img->aniy == ay) continue;
            if (changed == 0) doc_undo_push();
            img->anix = ax;
            img->aniy = ay;
            changed++;
        }
        if (changed > 0) {
            mark_dirty();
            g_img_tex_idx = -2;
            ClearTimelineThumbCache();
        }
        if (changed > 0)
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Set %d marked animation point%s (%s%s%s), size-compensated from first frame.",
                     changed, changed == 1 ? "" : "s",
                     g_group_set_x ? "X" : "", g_group_set_x && g_group_set_y ? "/" : "",
                     g_group_set_y ? "Y" : "");
        else
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Marked animation points already match the requested %s%s%s alignment.",
                     g_group_set_x ? "X" : "", g_group_set_x && g_group_set_y ? "/" : "",
                     g_group_set_y ? "Y" : "");
        g_restore_msg_timer = 4.0f;
        ImGui::CloseCurrentPopup();
    }
    if (!valid) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}


void DrawMainLayout(void)
{
    ImGuiIO &io = ImGui::GetIO();
    float sw = io.DisplaySize.x;
    float sh = io.DisplaySize.y;

    /* Reset transient per-image tool state when the selected image changes,
       so e.g. a Clone Stamp source from sprite A doesn't get re-applied as
       coords on sprite B (which could OOB-read or just paint garbage). */
    static int g_prev_ilselected = -2;
    static Document *g_prev_render_doc = NULL;
    if (g_doc != g_prev_render_doc || g_doc->ilselected != g_prev_ilselected) {
        g_prev_render_doc = g_doc;
        g_clone_source_set = false;
        g_clone_offset_set = false;
        g_remap_target_color = -1;
        g_snap_bbox.valid = false;
        /* Abort any in-progress freehand selection — its coords are in the
           previous image's pixel space and continuing the drag would mix
           coordinates across sprites. */
        g_lasso_points.clear();
        if (g_grid_sel.dragging) {
            g_grid_sel.dragging = false;
            g_grid_sel.active = false;
        }
        /* Drop multi-swatch selection too. Even if the new image shares a
           palette with the old one, users perceive image-switch as a
           fresh context and a stale yellow border on swatches is
           confusing. They can Ctrl/Shift-click to rebuild it. */
        commit_palette_adjustments();
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        if (TimelineCompositeReady() && TimelineCompositeSlot(g_doc->ilselected) < 0)
            ClearTimelineCompositeSelection();
        g_prev_ilselected = g_doc->ilselected;
    }

    /* ---- Global keyboard shortcuts ---- */
    ImGuiInputFlags route = ImGuiInputFlags_RouteGlobal;
    bool popup_using_keyboard =
        g_show_file_dialog ||
        ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);

    /* Undo / Redo. Three call sites share the same logic via DoUndo/DoRedo:
       the Ctrl+Z/Ctrl+Y shortcuts here, the Edit menu items, and the
       toolbar buttons. The CanUndo/CanRedo predicates drive both the
       toolbar enable/disable and the menu enable/disable so a paint
       stroke immediately makes the buttons clickable. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z, route)) DoUndo();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Y, route)) DoRedo();

    /* Clipboard */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_C, route)) CopySelectionToNewImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_X, route)) CutSelectionToNewImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_V, route)) PasteClipboardAsNewImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_C, route)) copy_image(false);
    if (!io.KeyShift && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_X, route)) copy_image(true);
    if (!io.KeyShift && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_V, route)) paste_image();

    /* Adobe-standard selection shortcuts. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, route)) select_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route)) deselect_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I, route)) invert_selection();
    /* Ctrl+J duplicates: a floating paste commits and stays floating as a
       second copy of itself; otherwise duplicates the current image. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_J, route)) {
        if (g_pasted.active) {
            if (g_xform.active) xform_commit();
            apply_pasted_region(); /* leave g_pasted.active = true */
        } else {
            DuplicateImage();
        }
    }
    /* Ctrl+E commits a floating paste in place (Photoshop "Merge Down"). */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_E, route)) {
        if (g_pasted.active) {
            if (g_xform.active) xform_commit();
            apply_pasted_region();
            g_pasted.active = false;
            g_pasted.dragging = false;
        }
    }
    /* Shift+Del is an always-image delete escape hatch. Plain Del below follows
       the active side-panel list (images vs palettes). */
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_Delete, route)) {
        if (g_doc->ilselected >= 0) RequestDeleteImage(g_doc->ilselected);
    }
    /* Image-list ops the menu advertises but were previously unbound. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_R, route))     OpenRenameImage();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P, route))     TogglePointTable();
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_PageUp, route))   MoveImageUp();
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_PageDown, route)) MoveImageDown();

    /* File I/O */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_O, route)) RequestOpenDialog();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S, route)) OpenFileDialog(FileDialogMode::SaveImg);
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_L, route)) OpenFileDialog(FileDialogMode::LoadLbm);
    if (ImGui::Shortcut(ImGuiMod_Alt  | ImGuiKey_S, route)) OpenFileDialog(FileDialogMode::SaveLbm);
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_L, route)) OpenFileDialog(FileDialogMode::LoadTga);

    /* View / Debug (H is repurposed to flip the floating paste while one is up) */
    if (!g_pasted.active && ImGui::Shortcut(ImGuiKey_H,  route)) g_show_help = true;
    if (ImGui::Shortcut(ImGuiKey_F9, route)) g_show_debug = !g_show_debug;
    if (!popup_using_keyboard && !io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Equal, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Equal, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadAdd, route))
            QueueZoomStep(1);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Minus, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_KeypadSubtract, route))
            QueueZoomStep(-1);
        if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_0, route) ||
            ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Keypad0, route))
            QueueZoomFit();
    }
    if (ImGui::Shortcut(ImGuiKey_R,  route)) {
        if (g_active_tool == ActiveTool::Marquee) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::Marquee;
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (ImGui::Shortcut(ImGuiKey_W,  route)) {
        if (g_active_tool == ActiveTool::MagicWand) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::MagicWand;
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (!g_pasted.active && ImGui::Shortcut(ImGuiKey_L,  route)) {
        if (g_active_tool == ActiveTool::Lasso) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::Lasso;
        g_lasso_points.clear();
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (ImGui::Shortcut(ImGuiKey_I,  route)) {
        if (g_active_tool == ActiveTool::Eyedropper) g_active_tool = ActiveTool::None;
        else g_active_tool = ActiveTool::Eyedropper;
        if (g_active_tool == ActiveTool::None) g_grid_sel.active = false;
    }
    if (ImGui::Shortcut(ImGuiKey_P, route)) {
        /* Pencil — Adobe shortcut. Toggles back to None on a second press
           since the underlying paint behavior is the same as no tool. */
        g_active_tool = (g_active_tool == ActiveTool::Pencil) ? ActiveTool::None : ActiveTool::Pencil;
    }
    if (ImGui::Shortcut(ImGuiKey_G, route)) {
        g_active_tool = (g_active_tool == ActiveTool::PaintBucket) ? ActiveTool::None : ActiveTool::PaintBucket;
    }
    if (!g_pasted.active && ImGui::Shortcut(ImGuiKey_V, route)) {
        g_active_tool = (g_active_tool == ActiveTool::VariantPaint) ? ActiveTool::None : ActiveTool::VariantPaint;
    }
    /* [ and ] do double duty depending on context:
         - Pencil active: [ shrinks brush, ] grows brush (Photoshop convention).
         - Otherwise:     [ Set Palette for Marked, ] Set for Image
                          (matches the menu-item hint advertised next to those entries).
       Other one-key palette shortcuts advertised in the menus:
         *        — Merge Marked Palettes into Selected
         Shift+R  — Rename selected palette
         Del      — Delete selected palette
       All of these were previously advertised in tooltips but never actually
       wired; they're real shortcuts now. */
    if (g_active_tool == ActiveTool::Pencil || g_active_tool == ActiveTool::VariantPaint) {
        int *brush = (g_active_tool == ActiveTool::VariantPaint) ? &g_variant_brush : &g_pencil_brush;
        if (ImGui::Shortcut(ImGuiKey_LeftBracket,  route))
            { if (*brush > 1)  (*brush)--; }
        if (ImGui::Shortcut(ImGuiKey_RightBracket, route))
            { if (*brush < 16) (*brush)++; }
    } else {
        if (ImGui::Shortcut(ImGuiKey_LeftBracket,  route)) SetPaletteOfMarked();
        if (ImGui::Shortcut(ImGuiKey_RightBracket, route)) SetPaletteOfSelected();
    }
    /* '*' merges marked palettes. SDL physical-key bindings only fire on
       US-layout Shift+8, so use ImGui's text-input queue instead — the
       backend posts the actual typed character regardless of keyboard
       layout. Only when no widget owns the input focus (text fields
       would legitimately consume '*'). */
    if (!io.WantTextInput) {
        for (ImWchar c : io.InputQueueCharacters) {
            if (c == '*') { MergeMarkedPalettes(); break; }
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_R, route)) OpenRenamePalette(g_doc->plselected);
    if (!popup_using_keyboard && !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        ImGui::Shortcut(ImGuiKey_Delete, route)) {
        if (g_palette_nav) DeletePalette();
        else RequestDeleteImage(g_doc->ilselected);
    }

    /* Tool Intercepts. Esc/Enter have a three-level priority: transform
       takes precedence, then floating paste, then marquee. */
    if (ImGui::Shortcut(ImGuiKey_Escape, route)) {
        if (g_xform.active)         { xform_cancel(); }
        else if (g_pasted.active)   { g_pasted.active = false; g_pasted.dragging = false; }
        else if (g_grid_sel.active) { g_grid_sel.active = false; }
    }
    if (ImGui::Shortcut(ImGuiKey_Enter, route)) {
        if (g_xform.active)                                    xform_commit();
        else if (g_pasted.active && !g_pasted.dragging)      { apply_pasted_region(); g_pasted.active = false; }
    }
    /* Ctrl+T enters Free Transform on the floating paste. Pressing it again
       while transforming commits and exits — symmetric with Photoshop. */
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_T, route)) {
        if (g_xform.active)        xform_commit();
        else if (g_pasted.active)  xform_begin();
    }
    /* While a floating paste is up (and not free-transforming): H / V mirror it
       in place, L drops it onto the sprite as a non-destructive layer. */
    if (g_pasted.active && !g_xform.active && !io.WantTextInput) {
        if (ImGui::Shortcut(ImGuiKey_H, route)) flip_clipboard_horizontal();
        if (ImGui::Shortcut(ImGuiKey_V, route)) flip_clipboard_vertical();
        if (ImGui::Shortcut(ImGuiKey_L, route)) drop_paste_to_layer();
    }
    /* A floating paste owns the cursor keys until committed or cancelled.
       This prevents accidental frame changes and permits precise placement. */
    if (g_pasted.active && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt) {
        int step = io.KeyShift ? 10 : 1;
        int dx = 0, dy = 0;
        if (ImGui::Shortcut((io.KeyShift ? ImGuiMod_Shift : 0) | ImGuiKey_LeftArrow, route))  dx -= step;
        if (ImGui::Shortcut((io.KeyShift ? ImGuiMod_Shift : 0) | ImGuiKey_RightArrow, route)) dx += step;
        if (ImGui::Shortcut((io.KeyShift ? ImGuiMod_Shift : 0) | ImGuiKey_UpArrow, route))    dy -= step;
        if (ImGui::Shortcut((io.KeyShift ? ImGuiMod_Shift : 0) | ImGuiKey_DownArrow, route))  dy += step;
        if (g_xform.active) { g_xform.rx += dx; g_xform.ry += dy; }
        else { g_pasted.paste_x += dx; g_pasted.paste_y += dy; }
    }

    /* Image Operations */
    if (ImGui::Shortcut(ImGuiKey_Space, route)) {
        IMG *img = get_img(g_doc->ilselected); if (img) { img->flags ^= 1; mark_dirty(); }
    }
    /* Timeline play/pause (K = standard video editor convention). */
    if (ImGui::Shortcut(ImGuiKey_K, route)) imgtool_toggle_timeline_play();
    /* Left/Right scrub the animation timeline, or the marked-tab World View
       sequence when that preview is active. */
    /* A merely focused numeric widget must not steal Left/Right in World View.
       Active drags and text entry still block navigation normally. */
    bool widget_using_keyboard = popup_using_keyboard || ImGui::IsAnyItemActive() ||
                                 (!g_world_state.enabled && ImGui::IsAnyItemFocused()) ||
                                 io.WantTextInput;
    if (!g_pasted.active && !widget_using_keyboard && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
        if (ImGui::Shortcut(ImGuiKey_LeftArrow, route)) {
            if (g_world_state.enabled && g_world_marked_state.marked_play) StepWorldMarkedSequence(g_world_marked_state, -1);
            else StepTimelinePlayhead(-1);
        }
        if (ImGui::Shortcut(ImGuiKey_RightArrow, route)) {
            if (g_world_state.enabled && g_world_marked_state.marked_play) StepWorldMarkedSequence(g_world_marked_state, 1);
            else StepTimelinePlayhead(1);
        }
    }
    /* Ctrl+Left/Right reorders the current play-head frame within the timeline. */
    if (!g_pasted.active && !widget_using_keyboard && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_LeftArrow, route)) {
        if (g_timeline_play_idx > 0 && g_timeline_play_idx < (int)g_timeline_frames.size()) {
            TimelineSwapFrames(g_timeline_play_idx, g_timeline_play_idx - 1);
            g_timeline_play_idx--;
        }
    }
    if (!g_pasted.active && !widget_using_keyboard && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_RightArrow, route)) {
        if (g_timeline_play_idx + 1 < (int)g_timeline_frames.size()) {
            TimelineSwapFrames(g_timeline_play_idx, g_timeline_play_idx + 1);
            g_timeline_play_idx++;
        }
    }
    if (ImGui::Shortcut(ImGuiMod_Shift | ImGuiKey_M, route)) {
        IMG *p = (IMG*)g_doc->img_p; while (p) { p->flags |= 1; p = (IMG*)p->nxt_p; }
    }
    if (ImGui::Shortcut(ImGuiKey_M, route)) {
        IMG *p = (IMG*)g_doc->img_p; while (p) { p->flags &= ~1; p = (IMG*)p->nxt_p; }
    }
    if (ImGui::Shortcut(ImGuiKey_Semicolon, route)) LeastSquaresReduceMarked();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_B, route)) OpenFileDialog(FileDialogMode::ExportTga);

    /* Sprite / palette list navigation: cursor up/down flicks
     * between images (default) or palettes (when palette panel
     * was last clicked), matching DOS imgtool muscle memory. */
    if (!g_pasted.active && !popup_using_keyboard && !widget_using_keyboard &&
        g_world_state.enabled &&
        WorldEmbeddedSeqScrActive(g_world_marked_state)) {
        /* When a World View script/sequence table is loaded, Up/Down step
           through its entries (loading each target sprite) instead of walking
           the main image list, so you can scrub the script without clicking. */
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route))
            StepWorldEmbeddedSeqScrEntry(g_world_marked_state, 1);
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route))
            StepWorldEmbeddedSeqScrEntry(g_world_marked_state, -1);
    } else if (!g_pasted.active && !popup_using_keyboard && g_palette_nav && g_doc->palcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            SelectPalette((g_doc->plselected + 1) % (int)g_doc->palcnt);
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            SelectPalette((g_doc->plselected <= 0) ? (int)g_doc->palcnt - 1 : g_doc->plselected - 1);
            g_zoom_reset = true;
        }
    } else if (!g_pasted.active && !popup_using_keyboard && g_doc->imgcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            g_doc->ilselected = (g_doc->ilselected + 1) % (int)g_doc->imgcnt;
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            g_doc->ilselected = (g_doc->ilselected <= 0) ? (int)g_doc->imgcnt - 1 : g_doc->ilselected - 1;
            g_zoom_reset = true;
        }
    }
    /* Tab toggles World View mode (anipoint alignment workspace). */
    if (ImGui::Shortcut(ImGuiKey_Tab, route)) {
        AnipointLink().enabled = false;
        g_world_state.enabled = !g_world_state.enabled;
    }

    /* ---- Menu bar ---- */
    if (ImGui::BeginMainMenuBar()) {
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 4));
        if (ImGui::BeginMenu("File")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("New"))             g_show_new_img_confirm = true;
            if (ImGui::MenuItem("Open...",  "Ctrl+O")) RequestOpenDialog();
            if (ImGui::BeginMenu("Open Recent", !g_recent_files.empty())) {
                std::vector<std::string> snap = g_recent_files;
                for (size_t i = 0; i < snap.size(); i++) {
                    char label[1100];
                    snprintf(label, sizeof(label), "%zu. %s", i + 1, snap[i].c_str());
                    if (ImGui::MenuItem(label)) RequestOpenPath(snap[i]);
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Clear Recent")) {
                    g_recent_files.clear();
                    RecentSave();
                }
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Save",    "Ctrl+S")) OpenFileDialog(FileDialogMode::SaveImg);
            if (ImGui::MenuItem("Append"))            OpenFileDialog(FileDialogMode::AppendImg);
            if (ImGui::MenuItem("Open LOD..."))       RequestOpenLodDialog();
            ImGui::Separator();
            if (ImGui::BeginMenu("Import")) {
                if (ImGui::MenuItem("PNG File..."))                 OpenFileDialog(FileDialogMode::ImportPng);
                if (ImGui::MenuItem("PNG (Match to Active Palette)...")) OpenFileDialog(FileDialogMode::ImportPngMatch);
                if (ImGui::MenuItem("Sprite Sheet (Match Palette)...")) OpenFileDialog(FileDialogMode::ImportSpriteSheetMatch);
                if (ImGui::MenuItem("GIF File..."))                 OpenFileDialog(FileDialogMode::ImportGif);
                if (ImGui::MenuItem("Palette..."))                  OpenFileDialog(FileDialogMode::ImportPalette);
                ImGui::Separator();
                if (ImGui::MenuItem("Load LBM", "Alt+L"))  OpenFileDialog(FileDialogMode::LoadLbm);
                if (ImGui::MenuItem("Load TGA", "Ctrl+L")) OpenFileDialog(FileDialogMode::LoadTga);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("Export")) {
                if (ImGui::MenuItem("PNG File..."))                    OpenFileDialog(FileDialogMode::ExportPng);
                if (ImGui::MenuItem("Animated GIF (Timeline)...", NULL, false,
                                    !g_timeline_frames.empty()))        OpenFileDialog(FileDialogMode::ExportGif);
                if (ImGui::MenuItem("Palette..."))                     OpenFileDialog(FileDialogMode::ExportPalette);
                ImGui::Separator();
                if (ImGui::MenuItem("Save LBM", "Alt+S"))        OpenFileDialog(FileDialogMode::SaveLbm);
                if (ImGui::MenuItem("Save Marked LBM"))          OpenFileDialog(FileDialogMode::SaveMarkedLbm);
                if (ImGui::MenuItem("Save TGA"))                 OpenFileDialog(FileDialogMode::SaveTga);
                ImGui::Separator();
                if (ImGui::MenuItem("Build TGA from Marked", "Ctrl+B")) OpenFileDialog(FileDialogMode::ExportTga);
                if (ImGui::MenuItem("Write ANILST..."))                OpenFileDialog(FileDialogMode::WriteAniLst);
                if (ImGui::MenuItem("Write TBL..."))                   OpenFileDialog(FileDialogMode::WriteTbl);
    if (ImGui::MenuItem("Write IRW..."))                   OpenFileDialog(FileDialogMode::WriteIrw);
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Quit", "Esc")) g_pending_quit = true;
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            bool can_undo = CanUndo();
            bool can_redo = CanRedo();
            if (!can_undo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Undo", "Ctrl+Z")) DoUndo();
            if (!can_undo) ImGui::EndDisabled();
            if (!can_redo) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Redo", "Ctrl+Y")) DoRedo();
            if (!can_redo) ImGui::EndDisabled();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy",  "Ctrl+C", false, g_doc->ilselected >= 0)) copy_image(false);
            if (ImGui::MenuItem("Cut",   "Ctrl+X", false, g_doc->ilselected >= 0)) copy_image(true);
            if (ImGui::MenuItem("Copy to New Sprite", "Ctrl+Shift+C", false, g_doc->ilselected >= 0))
                CopySelectionToNewImage();
            if (ImGui::MenuItem("Cut to New Sprite", "Ctrl+Shift+X", false, g_doc->ilselected >= 0))
                CutSelectionToNewImage();
            if (ImGui::MenuItem("Paste", "Ctrl+V", false, g_clipboard.valid && g_doc->ilselected >= 0))
                paste_image();
            if (ImGui::MenuItem("Paste as New Sprite", "Ctrl+Shift+V", false, g_clipboard.valid))
                PasteClipboardAsNewImage();
            ImGui::Separator();
            if (ImGui::MenuItem("Capture Sprite Cookie Cutter", NULL, false, g_doc->ilselected >= 0))
                CaptureCookieCutter();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Captures the selected frame's exact non-transparent silhouette\n"
                "and its tight pixel bounds. The cutter survives opening another IMG.");
            if (ImGui::MenuItem("Place Cookie Cutter", NULL, false,
                                g_clipboard.valid && g_clipboard.has_opaque && g_doc->ilselected >= 0))
                PlaceCookieCutter();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Overlays the captured silhouette on this frame. Drag it into place,\n"
                "then press Enter or click outside it to cut those pixels to transparent.");
            ImGui::Separator();
            /* Selection ops — disabled when nothing's available. */
            if (ImGui::MenuItem("Select All",       "Ctrl+A",       false, g_doc->ilselected >= 0))      select_all();
            if (ImGui::MenuItem("Deselect",         "Ctrl+D",       false, g_grid_sel.active))    deselect_all();
            if (ImGui::MenuItem("Invert Selection", "Ctrl+Shift+I", false, g_doc->ilselected >= 0))      invert_selection();
            ImGui::Separator();
            /* Floating-paste ops — only meaningful while a paste is active. */
            if (ImGui::MenuItem("Free Transform",   "Ctrl+T", false, g_pasted.active && !g_xform.active)) xform_begin();
            if (ImGui::MenuItem("Merge Down",       "Ctrl+E", false, g_pasted.active)) {
                if (g_xform.active) xform_commit();
                apply_pasted_region();
                g_pasted.active = false;
                g_pasted.dragging = false;
            }
            if (ImGui::MenuItem("Drop Paste to Layer", "L", false, g_pasted.active && !g_xform.active))
                drop_paste_to_layer();
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Image",     "Ctrl+R"))     OpenRenameImage();
            if (ImGui::MenuItem("Delete Image",     "Del"))        RequestDeleteImage(g_doc->ilselected);
            if (ImGui::MenuItem("Duplicate",        "Ctrl+J"))     DuplicateImage();
            if (ImGui::MenuItem("Trim Transparent Bounds", NULL, false, g_doc->ilselected >= 0)) {
                doc_undo_push();
                int n = CropSelectedImageToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Trimmed selected sprite to non-transparent bounds."
                               : "Selected sprite already fits, or has no opaque pixels.");
                g_restore_msg_timer = 4.0f;
                if (n > 0) g_zoom_reset = true;
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Image")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Mark / Unmark",      "Space"))  { IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1; }
            if (ImGui::MenuItem("Set All Marks",      "M"))      { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Clear All Marks",    "m"))      { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
            if (ImGui::MenuItem("Invert All Marks"))             { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags^=1;p=(IMG*)p->nxt_p;} }
            ImGui::Separator();
            if (ImGui::MenuItem("Jump to Prev Marked")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (g_doc->ilselected - i + n_imgs) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { g_doc->ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Jump to Next Marked")) {
                int n_imgs = count_imgs();
                for (int i = 1; i <= n_imgs; i++) {
                    int idx = (g_doc->ilselected + i) % n_imgs;
                    IMG *img = get_img(idx);
                    if (img && (img->flags & 1)) { g_doc->ilselected = idx; break; }
                }
            }
            if (ImGui::MenuItem("Move Up",    "Alt+PgUp")) MoveImageUp();
            if (ImGui::MenuItem("Move Down",  "Alt+PgDn")) MoveImageDown();
            ImGui::Separator();
            if (ImGui::MenuItem("Add/Del Point Table",  "Ctrl+P")) TogglePointTable();
            if (ImGui::MenuItem("Set ID from 2nd List"))           SetIDFromSecondList();
            if (ImGui::MenuItem("Switch Image List",    "Tab"))    SwitchImageList();
            if (ImGui::MenuItem("Clear Extra Data"))               ClearExtraData();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Operations")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Break into Subframes...")) OpenAutoChopDialog();
            if (ImGui::MenuItem("Resize Sprite...", NULL, false, g_doc->ilselected >= 0)) OpenResizeSpriteDialog();
            if (ImGui::MenuItem("Bulk Resize Marked...", NULL, false, CountMarkedImages() > 0)) OpenBulkResizeDialog();
            if (ImGui::MenuItem("Opacity Gradient...", NULL, false, g_doc->ilselected >= 0)) OpenOpacityGradientDialog();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Fade sprites to transparent index #0 using a directional\n"
                "dithered opacity gradient. Can target selected or marked sprites.");
            if (ImGui::MenuItem("Indexed Color Gradient...", NULL, false, g_doc->ilselected >= 0)) OpenIndexedGradientDialog();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Fit the palette colors selected before opening into a custom\n"
                "2-11 color ramp. Sprite pixel indices remain unchanged.");
            if (ImGui::MenuItem("3-Tone Inner Stroke...", NULL, false, g_doc->ilselected >= 0)) OpenInnerStrokeDialog();
            if (ImGui::BeginMenu("Transform Selected", g_doc->ilselected >= 0)) {
                DrawSpriteTransformMenuItems();
                ImGui::EndMenu();
            }
            if (ImGui::MenuItem("Crop Selected to Content", NULL, false, g_doc->ilselected >= 0)) {
                doc_undo_push();
                int n = CropSelectedImageToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Cropped selected sprite to non-transparent bbox."
                               : "Selected sprite already fits, or has no opaque pixels.");
                g_restore_msg_timer = 4.0f;
                if (n > 0) g_zoom_reset = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Trim the selected image to its nearest non-transparent pixels.\n"
                "Anipoints are adjusted so the on-screen position is unchanged.");
            if (ImGui::MenuItem("Crop Marked to Content")) {
                doc_undo_push();
                int n = CropMarkedImagesToContent();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Cropped %d image(s) to non-transparent bbox.", n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Trim each marked image to its non-transparent bounding box.\n"
                "Anipoints are adjusted so the on-screen position is unchanged.");
            if (ImGui::MenuItem("Defringe Marked Edges")) {
                int n = DefringeMarkedImages(1);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Defringe edited %d pixel(s).", n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "One-pass edge defringe: every pixel touching a transparent\n"
                "neighbor is averaged toward its non-transparent neighbors,\n"
                "killing the 1px halo of blue/green-spill on digitized actors.");
            if (ImGui::MenuItem("Clean Sprite Artifacts...", NULL, false,
                                CountMarkedImages() > 0 || g_doc->ilselected >= 0)) {
                OpenSpriteCleanupDialog();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Finds isolated wrong-color pixels and repaints them from\n"
                "nearby supported colors, or transparent #0 for dust specks.");
            if (ImGui::MenuItem("Remove Hard Stroke (1-2px)")) {
                RemoveHardStrokeFromTargets(2);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Detects a thin high-contrast outline/matte ring around\n"
                "transparent sprite edges and removes it. Uses marked sprites,\n"
                "or the selected sprite if none are marked.");
            if (ImGui::MenuItem("Apply Marked Likeness to Selected", NULL, false,
                                g_doc->ilselected >= 0)) {
                ApplyMarkedLikenessToSelected();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mark one source sprite, then select the target sprite.\n"
                "Uses the source palette/materials while preserving the\n"
                "target pose and actor shading. Best with a marked source\n"
                "frame that has similar costume, lighting, and scale.");
            if (ImGui::MenuItem("Align Marked Anipoints to Selected")) {
                int n = AlignAnipointsToMarked(g_doc->ilselected);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Anchored %d marked image(s) to selected anipoint.", n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Sets the anipoint of every marked image to match the\n"
                "currently-selected image's anipoint. Useful when several\n"
                "frames should share one anchor (head, hand, hilt).");
            if (ImGui::MenuItem("Set Marked Anipoints to X/Y...", NULL, false,
                                CountMarkedImages() > 0)) {
                OpenSetGroupAnipointsDialog();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Sets every marked frame's primary animation point to the\n"
                "exact signed X and Y coordinates entered in the dialog.");
            if (ImGui::MenuItem("Mirror Marked Anipoints to Reverse")) {
                MirrorMarkedAnipointsToReverseWithToast();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mirrors X anipoints on marked sprites as width - X.\n"
                "Y/Z values stay unchanged.");
            ImGui::Separator();
            if (ImGui::MenuItem("Least-Squares Reduce", ";"))               LeastSquaresReduceMarked();
            ImGui::Separator();
            if (ImGui::MenuItem("Strip Edge"))                               StripMarkedImages(5);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Repaints the original outer edge from nearby inward colors.\n"
                "Keeps the edit on the edge instead of walking into the sprite.");
            if (ImGui::MenuItem("Strip Edge Low"))                           StripMarkedImages(3);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Conservative edge repaint using the same original-edge logic.");
            if (ImGui::MenuItem("Strip Edge (Selected Color)"))              StripMarkedImages(5, g_sel_color);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Only strips the selected palette index, then softens the edge.");
            if (ImGui::MenuItem("Dither Replace"))                           DitherReplaceMarkedImages(g_sel_color);
            if (ImGui::MenuItem("Match All Sprites to Marked Source Colors")) {
                int source_idx = -1;
                int marked = 0;
                int idx = 0;
                for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p, idx++) {
                    if (p->flags & 1) {
                        source_idx = idx;
                        marked++;
                    }
                }
                if (marked != 1) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Mark exactly one source sprite first.");
                    g_restore_msg_timer = 4.0f;
                } else {
                    int pixels = 0;
                    int n = PreviewMatchAllSpritesToSourceColors(source_idx, &pixels);
                    if (n > 0 && doc_undo_push()) {
                        n = MatchAllSpritesToSourceColors(source_idx, &pixels);
                    }
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             n > 0 ? "Matched %d sprite(s) to source colors (%d px changed)."
                                   : "No sprites changed; colors already match or palettes are missing.",
                             n, pixels);
                    g_restore_msg_timer = 5.0f;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mark exactly one source sprite. Every other sprite is\n"
                "remapped to the nearest non-transparent colors used by\n"
                "that source and assigned to the source palette. Shapes stay unchanged.");
            ImGui::Separator();
            if (ImGui::MenuItem("Apply Variant Paint to Selection"))          ApplyVariantToSelection();
            if (ImGui::MenuItem("Remap Similar Regions to Current Swatch"))   ApplySelectionRemapToMatchingSprites();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Uses the current selection as a sample, then remaps likely\n"
                "matching same-palette regions across the IMG to the current swatch.");
            if (ImGui::MenuItem("Split Selection to Overlay Frame"))          SplitSelectionToOverlayFrame(true);
            if (ImGui::MenuItem("Copy Selection to Overlay Frame"))           SplitSelectionToOverlayFrame(false);
            ImGui::Separator();
            if (ImGui::MenuItem("Restore from Selected (pixel-diff)")) {
                int n = RestoreMarkedFromSource();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Restored %d pixel(s) from selected source."
                               : "No pixels restored. Check selection, marks, palettes, anipoints.",
                         n);
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "For each marked image, copy non-transparent source pixels\n"
                "into transparent strip pixels. Selected image = source.\n"
                "Uses anipoints to align; same palette required.");
            if (ImGui::MenuItem("Bulk Restore from Source (overwrite)")) {
                int n = RestoreMarkedFromSourceForce();
                IMG *s = get_img(g_doc->ilselected);
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         n > 0 ? "Rebuilt %d px from source. Source anipt (%d,%d) %dx%d"
                               : "0 px restored. Source anipt (%d,%d) %dx%d. Check marks/anipoints.",
                         n, s? (int)(short)s->anix:0, s? (int)(short)s->aniy:0, s? (int)s->w:0, s? (int)s->h:0);
                g_restore_msg_timer = 6.0f;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Unconditionally overwrites every pixel in marked images\n"
                "with source pixels. No transparency or palette checks.\n"
                "For rebuilding splits (1A/1B/2A...) from full source.");
            if (ImGui::MenuItem("Bulk Restore via Regex...")) {
                g_show_restore_regex = true;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Uses a regex to map child names to parent names across the entire file,\n"
                "then restores child pixels from their parent automatically.");
            ImGui::Separator();
            if (ImGui::MenuItem("Rename Marked"))                            OpenRenameMarkedImages();
            if (ImGui::MenuItem("Delete Marked"))                            RequestDeleteMarkedImages();
            if (ImGui::MenuItem("Set Palette for Marked", "["))              SetPaletteOfMarked();
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Palette")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Add Palette"))                    AddNewPalette();
            if (ImGui::MenuItem("Duplicate Palette"))              DuplicatePalette();
            ImGui::Separator();
            if (ImGui::MenuItem("Set for Image",       "]"))       SetPaletteOfSelected();
            if (ImGui::MenuItem("Merge Marked into Selected", "*")) MergeMarkedPalettes();
            if (ImGui::MenuItem("Preview Merge Marked into Selected")) OpenPaletteMergePreview();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Shows source-to-target palette mapping and drift before merging.");
            if (ImGui::MenuItem("Delete Palette",      "Del"))     DeletePalette();
            if (ImGui::MenuItem("Rename Palette",      "Shift+R")) OpenRenamePalette(g_doc->plselected);
            ImGui::Separator();
            if (ImGui::MenuItem("Show Histogram"))               { CalculatePaletteHistogram(); g_show_histogram = true; }
            if (ImGui::MenuItem("Clean Up Palette"))             CleanupSelectedPalette();
            if (ImGui::MenuItem("Group Like Colors"))            GroupLikeColorsSelectedPalette();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Clean up, then cluster similar colors so each hue family\n"
                "sits together as its own dark-to-light ramp.");
            if (ImGui::MenuItem("Clean Copy Palette"))           CreateCleanedPaletteCopy();
            if (ImGui::MenuItem("Single-Color Shading..."))      OpenPaletteSingleColorDialog();
            if (ImGui::MenuItem("Inherit Colors from Marked"))   InheritSelectedPaletteFromMarked();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mark the source palette, select the target palette.\n"
                "Sprites using the target are remapped to the nearest source colors.");
            if (ImGui::MenuItem("Merge Duplicate Palettes"))      MergeDuplicatePalettes();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Finds byte-identical palettes and remaps sprites using later\n"
                "duplicates to the first matching palette in the list.");
            if (ImGui::MenuItem("Downscale Palette..."))         OpenPaletteReduceDialog(7);
            if (ImGui::MenuItem("Copy #0 to Opaque Slot"))       CopyPaletteZeroToOpaqueSlot();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Index 0 remains transparent; this copies its RGB into\n"
                "a nonzero palette slot so it can be painted visibly.");
            if (ImGui::MenuItem("Import Palette..."))            OpenFileDialog(FileDialogMode::ImportPalette);
            if (ImGui::MenuItem("Export Palette..."))            OpenFileDialog(FileDialogMode::ExportPalette);
            ImGui::Separator();
            if (ImGui::MenuItem("Mark All")) {
                PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Clear Marks")) {
                PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags&=~1;p=(PAL*)p->nxt_p;}
            }
            if (ImGui::MenuItem("Invert Marks")) {
                PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;}
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            ImGui::MenuItem("Verbose Logging", NULL, &g_verbose);
            ImGui::Separator();
            ImGui::BeginDisabled(g_doc->ilselected < 0);
            if (ImGui::MenuItem("Zoom In", "Ctrl+=")) QueueZoomStep(1);
            if (ImGui::MenuItem("Zoom Out", "Ctrl+-")) QueueZoomStep(-1);
            if (ImGui::MenuItem("Fit Sprite", "Ctrl+0")) QueueZoomFit();
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::MenuItem("Anim Points",     NULL, &g_show_points);
            ImGui::MenuItem("Hitboxes",        NULL, &g_show_hitbox);
            ImGui::MenuItem("DMA Compression", NULL, &g_show_dma_comp);
            ImGui::MenuItem("Anim Scripts / Seqs", NULL, &g_show_seqscr_editor);
            ImGui::Separator();
            ImGui::MenuItem("World View",      NULL,   &g_world_state.enabled);
            if (ImGui::MenuItem("Anipoint Link Workspace", NULL,
                                &AnipointLink().enabled)) {
                if (AnipointLink().enabled)
                    g_world_state.enabled = false;
            }
            if (g_world_state.enabled) {
                if (ImGui::MenuItem("Marked Row Playback", NULL, &g_world_marked_state.marked_play)) {
                    WorldMarkedRestart(g_world_marked_state);
                }
                ImGui::MenuItem("Marked Playback Paused", NULL, &g_world_marked_state.paused);
                ImGui::SetNextItemWidth(80);
                ImGui::SliderFloat("Marked FPS", &g_world_marked_state.fps, 1.0f, 60.0f, "%.1f");
                if (ImGui::MenuItem("Dummy Decap Body", NULL,
                                    &g_world_marked_state.dummy_decap_body)) {
                    g_world_marked_state.dummy_decap_reset = true;
                    g_world_marked_state.hold_end[kWorldDummyDecapSlot] = true;
                    WorldMarkedRestart(g_world_marked_state);
                }
                if (ImGui::BeginMenu("Marked Rows")) {
                    for (int slot = 0; slot < kWorldMarkedMaxTabs; slot++) {
                        ImGui::PushID(slot);
                        char label[64];
                        if (slot == kWorldDummyDecapSlot)
                            snprintf(label, sizeof(label), "Dummy Body Hold Final Frame");
                        else
                            snprintf(label, sizeof(label), "Slot %d Hold Final Frame", slot + 1);
                        if (ImGui::MenuItem(label, NULL, &g_world_marked_state.hold_end[slot])) {
                            WorldMarkedRestart(g_world_marked_state);
                        }
                        bool *mirror = WorldMarkedMirrorFlag(g_world_marked_state, slot);
                        if (mirror) {
                            if (slot == kWorldDummyDecapSlot)
                                snprintf(label, sizeof(label), "Dummy Body Mirror");
                            else
                                snprintf(label, sizeof(label), "Slot %d Mirror", slot + 1);
                            ImGui::MenuItem(label, NULL, mirror);
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndMenu();
                }
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("World W",      &g_world_state.w, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("World H",      &g_world_state.h, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Origin X",     &g_world_state.origin_x, 0, 0);
                ImGui::SetNextItemWidth(80);
                ImGui::InputInt("Origin Y",     &g_world_state.origin_y, 0, 0);
            }
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Verify LOAD2 Packing")) {
                g_load2_report = VerifyLoad2Packing(g_load2_ppp);
                g_load2_selected_idx = -1;
                g_show_load2_verify = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("MK2 Hitboxes (MKSTK.ASM)...")) g_show_mk2 = true;
            if (ImGui::MenuItem("MK2 Fatality Lab...")) g_show_mk2_fatality = true;
            if (ImGui::MenuItem("ASM Animation Viewer...", NULL, &g_show_asm_anim) &&
                g_show_asm_anim && g_asm_anims.empty())
                OpenFileDialog(FileDialogMode::LoadAsmAnim);
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 8));
            if (ImGui::MenuItem("Show Help",  "h"))  g_show_help = true;
            if (ImGui::MenuItem("Debug Info", "F9")) g_show_debug = !g_show_debug;
            ImGui::Separator();
            if (ImGui::MenuItem("About...")) g_show_about = true;
            ImGui::PopStyleVar();
            ImGui::EndMenu();
        }
        /* Right-aligned dirty / filename indicator. Gives users a passive
           reminder that there are unsaved changes — without this, the only
           "this file is modified" signal is the quit-time confirmation. */
        {
            const char *name = (g_doc->fname_s[0] != '\0') ? g_doc->fname_s : "(unsaved)";
            char label[128];
            char zoom_part[32] = "";
            if (g_doc->ilselected >= 0 && !g_world_state.enabled) {
                snprintf(zoom_part, sizeof(zoom_part), "   Zoom %.0f%%",
                         g_zoom_effective * 100.0f);
            }
            snprintf(label, sizeof(label), "%s%s%s",
                     g_dirty ? "* " : "  ",     /* ASCII asterisk — universal 'modified' convention */
                     name, zoom_part);
            float text_w = ImGui::CalcTextSize(label).x + 16.0f;
            float avail_w = ImGui::GetContentRegionAvail().x;
            if (avail_w > text_w) ImGui::SameLine(ImGui::GetCursorPosX() + (avail_w - text_w));
            if (g_dirty) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::TextUnformatted(label);
            if (g_dirty) ImGui::PopStyleColor();
            if (g_dirty && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Unsaved changes — Ctrl+S to save");
            }
        }
        ImGui::PopStyleVar(2);
        ImGui::EndMainMenuBar();
    }

    float menu_h = ImGui::GetFrameHeight();
    float tab_h = DrawDocumentTabBar(menu_h, sw);
    float work_y = menu_h + tab_h;
    float work_h = sh - work_y;
    bool world_sequence_timeline =
        g_world_state.enabled && g_world_marked_state.marked_play;
    g_world_marked_panel_docked = world_sequence_timeline;
    bool hide_bottom_palette = g_world_state.enabled;
    float bottom_palette_h = hide_bottom_palette ? 0.0f : PALETTE_H;
    float canvas_x = TOOLBAR_W;
    float canvas_y = work_y;
    float canvas_w = sw - TOOLBAR_W - PANEL_W;
    float timeline_h = TIMELINE_H;
    float canvas_h = work_h - bottom_palette_h - timeline_h;
    if (world_sequence_timeline) {
        int guide_w = g_world_state.w > 512 ? g_world_state.w : 512;
        int guide_h = g_world_state.h > 254 ? g_world_state.h : 254;
        float world_scale = floorf(canvas_w / (float)guide_w);
        if (world_scale < 2.0f) world_scale = 2.0f;
        float desired_canvas_h = (float)guide_h * world_scale + 36.0f;
        if (desired_canvas_h < 260.0f) desired_canvas_h = 260.0f;

        int lane_count = 0;
        int entry_count = 0;
        if (g_world_marked_state.embedded_active) {
            lane_count = 1;
            entry_count =
                (int)g_world_marked_state.sequence_frames[kWorldEmbeddedSeqScrSlot].size();
        } else {
            for (int doc_idx = 0; doc_idx < document_tab_count(); doc_idx++) {
                Document *doc = document_get(doc_idx);
                bool has_marked_frames = false;
                for (IMG *img = doc ? (IMG *)doc->img_p : NULL;
                     img; img = (IMG *)img->nxt_p) {
                    if ((img->flags & 1) && img->data_p &&
                        img->w > 0 && img->h > 0) {
                        has_marked_frames = true;
                        entry_count++;
                    }
                }
                if (has_marked_frames) lane_count++;
            }
            if (g_world_marked_state.dummy_decap_body)
                lane_count++;
        }
        if (lane_count < 1) lane_count = 1;
        if (entry_count < 1) entry_count = 1;

        float desired_panel_h = 0.0f;
        if (g_world_marked_state.embedded_active) {
            int visible_rows = entry_count < 10 ? entry_count : 10;
            float table_base = g_world_marked_state.embedded_is_script
                             ? 78.0f : 132.0f;
            desired_panel_h = table_base + (float)(visible_rows + 1) * 24.0f;
        } else {
            desired_panel_h = 88.0f + (float)lane_count * 72.0f;
            if (entry_count > 8) desired_panel_h += 24.0f;
        }
        if (desired_panel_h < TIMELINE_H) desired_panel_h = TIMELINE_H;
        if (desired_panel_h > 380.0f) desired_panel_h = 380.0f;

        float usable_h = work_h - bottom_palette_h;
        /* Cap the world canvas so the panel keeps at least its content-driven
           height; shrink the canvas (down to a floor) when it would crowd the
           panel out. */
        if (desired_canvas_h + desired_panel_h > usable_h) {
            desired_canvas_h = usable_h - desired_panel_h;
            if (desired_canvas_h < 220.0f) desired_canvas_h = 220.0f;
        }
        canvas_h = desired_canvas_h;
        /* The docked panel below the world canvas always stretches to fill the
           remaining height down to the screen bottom (or the top of the
           palette bar), so all animation/data for whatever is docked there is
           shown instead of cutting off with a black gap underneath. */
        timeline_h = usable_h - canvas_h;
        if (timeline_h < TIMELINE_H) timeline_h = TIMELINE_H;
    }
    if (canvas_h < 120.0f) canvas_h = 120.0f;

    /* ---- Sync Palette State ---- */
    static Document *last_palette_doc = NULL;
    static int last_ilselected = -2;
    static int last_plselected = -2;
    static void *last_pal_p = (void *)-1;
    static unsigned int last_palcnt = ~0u;
    static unsigned int last_palette_sync_serial = 0;

    bool document_changed = g_doc != last_palette_doc;
    bool palette_head_changed = g_doc->pal_p != last_pal_p;
    bool palette_count_changed = g_doc->palcnt != last_palcnt;
    bool palette_sync_forced = g_palette_sync_serial != last_palette_sync_serial;
    bool palette_list_changed =
        document_changed ||
        palette_head_changed ||
        palette_count_changed ||
        palette_sync_forced;
    bool image_selection_changed = g_doc->ilselected != last_ilselected;

    if (palette_list_changed || image_selection_changed) {
        last_palette_doc = g_doc;
        last_ilselected = g_doc->ilselected;
        last_pal_p = g_doc->pal_p;
        last_palcnt = g_doc->palcnt;
        last_palette_sync_serial = g_palette_sync_serial;
        IMG* img = get_img(g_doc->ilselected);
        bool sync_selection_to_image = document_changed ||
                                       palette_head_changed ||
                                       image_selection_changed;
        if (sync_selection_to_image && img) {
            if ((unsigned)img->palnum < g_doc->palcnt)
                g_doc->plselected = img->palnum;
            else if (g_doc->palcnt == 0)
                g_doc->plselected = -1;
            else if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt)
                g_doc->plselected = (int)g_doc->palcnt - 1;
        } else if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) {
            g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;
        }
    }

    if (palette_list_changed || g_doc->plselected != last_plselected) {
        last_plselected = g_doc->plselected;
        ApplyPalette(g_doc->plselected);
        g_img_tex_idx = -2; /* Force texture rebuild to use new palette */
        reset_palette_adjust_sliders();
        /* Clear multi-select on palette change. Indexes from the previous
           palette don't map cleanly to the new one (different colors at the
           same index), so persisting the selection is misleading. */
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        save_palette_baseline();
    }

    /* Rebuild image texture every frame to pick up palette and data changes.
       When the texture-idx sentinel signals invalidation (set to -2 by any
       pixel-modifying tool), drop the matching timeline thumbnail too. */
    {
        IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
        if (g_img_tex_idx == -2 && g_doc->ilselected >= 0) InvalidateThumb(g_doc->ilselected);
        rebuild_img_texture(img);
        g_img_tex_idx = g_doc->ilselected;
    }

    /* ===== LEFT TOOLBAR ===== */
    float toolbar_bottom_reserved_h =
        bottom_palette_h + (world_sequence_timeline ? 0.0f : timeline_h);
    DrawLeftToolbar(work_y, work_h, toolbar_bottom_reserved_h);

    /* ===== RIGHT PANEL STRIP ===== */
    float panel_x = sw - PANEL_W;
    float panel_y = work_y + 5.0f;
    float panel_h = work_h - bottom_palette_h -
                    (world_sequence_timeline ? 0.0f : timeline_h) - 5.0f;

    ImGui::SetNextWindowPos(ImVec2(panel_x, panel_y));
    ImGui::SetNextWindowSize(ImVec2(PANEL_W, panel_h));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0x06, 0x06, 0x06, 0xFF));
    ImGui::Begin("##panels", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings);
    {
        if (ImGui::BeginTabBar("##right_panel_tabs",
                               ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Assets")) {
        /* --- Image List --- */
        int n_imgs = count_imgs();
        if (ImGui::CollapsingHeader("Images", ImGuiTreeNodeFlags_DefaultOpen)) {
            float list_h = panel_h * 0.30f;
            if (ImGui::BeginListBox("##imglist", ImVec2(-1, list_h))) {
                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                    g_palette_nav = false;
                /* Auto-scroll: when g_doc->ilselected changes (typically via Up/Down
                   keyboard nav, but also Prev/Next-Marked jumps or programmatic
                   selection), make sure the selected row is visible. Without
                   this the scroll bar stays put and the user loses track of
                   where they are in a long sprite list. */
                static int last_scrolled_to = -2;
                bool need_scroll = (g_doc->ilselected != last_scrolled_to);

                struct ImagePanelRow {
                    int idx;
                    IMG *img;
                    std::string name;
                    std::string src;
                    int area;
                    int parent_row;
                    std::string virtual_parent;
                    std::vector<int> children;
                };

                std::vector<ImagePanelRow> rows;
                std::vector<std::string> source_groups;
                rows.reserve(n_imgs);
                for (int i = 0; i < n_imgs; i++) {
                    IMG *img = get_img(i);
                    if (!img) break;
                    const char *src_name = img->src_filename[0] ? img->src_filename : "Workspace";
                    bool have_group = false;
                    for (const std::string &g : source_groups) {
                        if (g == src_name) { have_group = true; break; }
                    }
                    if (!have_group) source_groups.push_back(src_name);
                    rows.push_back({i, img, img->n_s, src_name,
                                    (int)img->w * (int)img->h, -1,
                                    std::string(), {}});
                }
                for (int r = 0; r < (int)rows.size(); r++) {
                    std::string parent_name = InferSubframeParentName(rows[r].name.c_str());
                    bool found_parent = false;
                    if (!parent_name.empty()) {
                        for (int p = 0; p < (int)rows.size(); p++) {
                            if (p == r) continue;
                            if (rows[p].src == rows[r].src && rows[p].name == parent_name) {
                                rows[r].parent_row = p;
                                rows[p].children.push_back(r);
                                found_parent = true;
                                break;
                            }
                        }
                        if (!found_parent)
                            rows[r].virtual_parent = parent_name;
                    } else {
                        std::string numbered_parent;
                        if (strip_trailing_sequence_digits(rows[r].name,
                                                           &numbered_parent)) {
                            for (int p = 0; p < (int)rows.size(); p++) {
                                if (p == r) continue;
                                if (rows[p].src == rows[r].src &&
                                    rows[p].name == numbered_parent) {
                                    rows[r].parent_row = p;
                                    rows[p].children.push_back(r);
                                    found_parent = true;
                                    break;
                                }
                            }
                        }
                    }
                }
                for (int r = 0; r < (int)rows.size(); r++) {
                    if (rows[r].virtual_parent.empty()) continue;
                    int siblings = 0;
                    for (const ImagePanelRow &other : rows) {
                        if (other.src == rows[r].src &&
                            other.virtual_parent == rows[r].virtual_parent)
                            siblings++;
                    }
                    if (siblings < 2)
                        rows[r].virtual_parent.clear();
                }

                auto row_less = [&](int a, int b) {
                    const ImagePanelRow &ra = rows[a];
                    const ImagePanelRow &rb = rows[b];
                    if (g_image_list_sort == ImageListSort::Name && ra.name != rb.name)
                        return g_image_list_sort_desc ? (ra.name > rb.name) : (ra.name < rb.name);
                    if (g_image_list_sort == ImageListSort::Size && ra.area != rb.area)
                        return g_image_list_sort_desc ? (ra.area > rb.area) : (ra.area < rb.area);
                    return g_image_list_sort_desc ? (ra.idx > rb.idx) : (ra.idx < rb.idx);
                };

                auto draw_image_context = [&](int img_idx) {
                    if (ImGui::BeginPopupContextItem("##imgctx")) {
                        g_doc->ilselected = img_idx;
                        IMG *ctx_img = get_img(img_idx);
                        if (ImGui::MenuItem("Mark / Unmark") && ctx_img) { ctx_img->flags ^= 1; }
                        if (ImGui::MenuItem("Rename"))        OpenRenameImage();
                        if (ImGui::MenuItem("Duplicate"))     DuplicateImage();
                        if (ImGui::MenuItem("Resize..."))     OpenResizeSpriteDialog();
                        if (ImGui::MenuItem("Break into Subframes (Auto-Chop)..."))
                            OpenAutoChopDialog();
                        if (ImGui::BeginMenu("Transform")) {
                            DrawSpriteTransformMenuItems();
                            ImGui::EndMenu();
                        }
                        if (ImGui::MenuItem("Trim Bounds")) {
                            doc_undo_push();
                            int n = CropSelectedImageToContent();
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     n > 0 ? "Trimmed selected sprite to non-transparent bounds."
                                           : "Selected sprite already fits, or has no opaque pixels.");
                            g_restore_msg_timer = 4.0f;
                            if (n > 0) g_zoom_reset = true;
                        }
                        if (ImGui::MenuItem("Delete"))        RequestDeleteImage(g_doc->ilselected);
                        if (ImGui::MenuItem("Delete Marked", NULL, false, CountMarkedImages() > 0))
                            RequestDeleteMarkedImages();
                        ImGui::Separator();
                        if (ImGui::MenuItem("Build TGA"))     OpenFileDialog(FileDialogMode::ExportTga);
                        if (ImGui::MenuItem("Set Palette"))   SetPaletteOfSelected();
                        ImGui::EndPopup();
                    }
                };

                auto draw_expand_triangle = [&](bool open) {
                    ImVec2 item_min = ImGui::GetItemRectMin();
                    ImVec2 item_max = ImGui::GetItemRectMax();
                    ImVec2 arrow_min(item_max.x - 18.0f, item_min.y);
                    ImVec2 arrow_max(item_max.x - 2.0f, item_max.y);
                    bool hovered = ImGui::IsMouseHoveringRect(arrow_min, arrow_max);
                    ImDrawList *dl = ImGui::GetWindowDrawList();
                    ImU32 col = hovered ? IM_COL32(255, 235, 140, 255) : IM_COL32(235, 235, 235, 210);
                    float cx = arrow_min.x + 8.0f;
                    float cy = (item_min.y + item_max.y) * 0.5f;
                    if (open) {
                        dl->AddTriangleFilled(ImVec2(cx - 4.0f, cy - 2.0f),
                                              ImVec2(cx + 4.0f, cy - 2.0f),
                                              ImVec2(cx,        cy + 4.0f), col);
                    } else {
                        dl->AddTriangleFilled(ImVec2(cx - 2.0f, cy - 4.0f),
                                              ImVec2(cx - 2.0f, cy + 4.0f),
                                              ImVec2(cx + 4.0f, cy),        col);
                    }
                    return hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
                };

                auto draw_leaf = [&](int row_id, bool subframe) {
                    ImagePanelRow &row = rows[row_id];
                    IMG *img = row.img;
                    bool marked   = (img->flags & 1) != 0;
                    bool selected = (row.idx == g_doc->ilselected);
                    ImGui::PushID(row.idx);
                    if (subframe) ImGui::Indent(18.0f);

                    char label[96];
                    const char *vis_icon = marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    const char *row_icon = subframe
                        ? (g_icon_font_loaded ? ICON_SUBFRAME : ICON_SUBFRAME_TXT)
                        : (g_icon_font_loaded ? ICON_IMAGE : ICON_IMAGE_TXT);
                    snprintf(label, sizeof(label), "%s %s  %s", vis_icon, row_icon, img->n_s);

                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                    }
                    if (ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                        g_doc->ilselected = row.idx;
                        g_palette_nav = false;
                        if (ImGui::IsMouseDoubleClicked(0)) img->flags ^= 1;
                    }
                    if (selected && need_scroll && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                    if (selected && need_scroll) last_scrolled_to = g_doc->ilselected;
                    if (selected) ImGui::PopStyleColor(2);
                    draw_image_context(row.idx);
                    if (subframe) ImGui::Unindent(18.0f);
                    ImGui::PopID();
                };

                auto draw_parent_group = [&](int row_id) {
                    ImagePanelRow &row = rows[row_id];
                    IMG *img = row.img;
                    bool marked   = (img->flags & 1) != 0;
                    bool selected = (row.idx == g_doc->ilselected);
                    ImGui::PushID(row.idx);

                    ImGuiStorage *storage = ImGui::GetStateStorage();
                    ImGuiID open_id = ImGui::GetID("subframes_open");
                    bool open = storage->GetBool(open_id, true);

                    char label[96];
                    const char *vis_icon = marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    const char *img_icon = g_icon_font_loaded ? ICON_IMAGE : ICON_IMAGE_TXT;
                    snprintf(label, sizeof(label), "%s %s  %s", vis_icon, img_icon, img->n_s);

                    if (selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                    }
                    bool clicked = ImGui::Selectable(label, selected, ImGuiSelectableFlags_AllowDoubleClick);
                    bool toggle_clicked = draw_expand_triangle(open);
                    if (toggle_clicked) {
                        open = !open;
                        storage->SetBool(open_id, open);
                    }
                    if (clicked) {
                        g_doc->ilselected = row.idx;
                        g_palette_nav = false;
                        if (ImGui::IsMouseDoubleClicked(0) && !toggle_clicked) img->flags ^= 1;
                    }
                    if (selected && need_scroll && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                    if (selected && need_scroll) last_scrolled_to = g_doc->ilselected;
                    if (selected) ImGui::PopStyleColor(2);
                    draw_image_context(row.idx);

                    if (open) {
                        std::vector<int> child_rows = row.children;
                        std::stable_sort(child_rows.begin(), child_rows.end(), row_less);
                        for (int child_id : child_rows) draw_leaf(child_id, true);
                    }
                    ImGui::PopID();
                };

                auto collect_virtual_children = [&](const std::string &src,
                                                    const std::string &parent,
                                                    std::vector<int> &out) {
                    out.clear();
                    for (int r = 0; r < (int)rows.size(); r++) {
                        if (rows[r].src == src && rows[r].virtual_parent == parent)
                            out.push_back(r);
                    }
                };

                auto virtual_group_stats = [&](const std::string &src,
                                               const std::string &parent,
                                               int *order, int *area) {
                    int best_order = 0x7FFFFFFF;
                    int best_area = 0;
                    for (const ImagePanelRow &row : rows) {
                        if (row.src != src || row.virtual_parent != parent) continue;
                        if (row.idx < best_order) best_order = row.idx;
                        if (row.area > best_area) best_area = row.area;
                    }
                    if (order) *order = (best_order == 0x7FFFFFFF) ? 0 : best_order;
                    if (area) *area = best_area;
                };

                auto draw_virtual_group = [&](const std::string &src,
                                              const std::string &parent) {
                    std::vector<int> child_rows;
                    collect_virtual_children(src, parent, child_rows);
                    if (child_rows.empty()) return;
                    std::stable_sort(child_rows.begin(), child_rows.end(), row_less);

                    bool any_marked = false;
                    bool any_selected = false;
                    for (int child_id : child_rows) {
                        IMG *img = rows[child_id].img;
                        if (img && (img->flags & 1)) any_marked = true;
                        if (rows[child_id].idx == g_doc->ilselected) any_selected = true;
                    }

                    const char *vis_icon = any_marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    char label[96];
                    snprintf(label, sizeof(label), "%s     %s", vis_icon, parent.c_str());

                    ImGui::PushID(src.c_str());
                    ImGui::PushID(parent.c_str());
                    ImGuiStorage *storage = ImGui::GetStateStorage();
                    ImGuiID open_id = ImGui::GetID("virtual_subframes_open");
                    bool open = storage->GetBool(open_id, true);
                    if (any_selected) {
                        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.15f, 0.35f, 0.65f, 1.0f));
                        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImVec4(0.20f, 0.45f, 0.85f, 1.0f));
                    }
                    bool clicked = ImGui::Selectable(label, any_selected);
                    bool toggle_clicked = draw_expand_triangle(open);
                    if (toggle_clicked) {
                        open = !open;
                        storage->SetBool(open_id, open);
                    }
                    if (clicked) {
                        g_doc->ilselected = rows[child_rows[0]].idx;
                        g_palette_nav = false;
                    }
                    if (any_selected) ImGui::PopStyleColor(2);
                    if (open) {
                        for (int child_id : child_rows) draw_leaf(child_id, true);
                    }
                    ImGui::PopID();
                    ImGui::PopID();
                };

                for (const std::string &src_group : source_groups) {
                    ImGui::PushID(src_group.c_str());
                    char group_label[96];
                    snprintf(group_label, sizeof(group_label), "%s  %s",
                             g_icon_font_loaded ? ICON_FOLDER : ICON_FOLDER_TXT,
                             src_group.c_str());
                    bool group_open = ImGui::TreeNodeEx(group_label,
                        ImGuiTreeNodeFlags_DefaultOpen | ImGuiTreeNodeFlags_SpanFullWidth);
                    ImGui::PopID();
                    if (!group_open) continue;

                    struct ImagePanelItem {
                        bool is_virtual;
                        int row_id;
                        std::string parent;
                    };

                    std::vector<ImagePanelItem> items;
                    for (int r = 0; r < (int)rows.size(); r++) {
                        if (rows[r].src == src_group &&
                            rows[r].parent_row < 0 &&
                            rows[r].virtual_parent.empty())
                            items.push_back({false, r, std::string()});
                    }
                    std::vector<std::string> virtual_parents;
                    for (const ImagePanelRow &row : rows) {
                        if (row.src != src_group || row.virtual_parent.empty()) continue;
                        bool seen = false;
                        for (const std::string &parent : virtual_parents) {
                            if (parent == row.virtual_parent) { seen = true; break; }
                        }
                        if (!seen) virtual_parents.push_back(row.virtual_parent);
                    }
                    for (const std::string &parent : virtual_parents)
                        items.push_back({true, -1, parent});

                    auto item_less = [&](const ImagePanelItem &a, const ImagePanelItem &b) {
                        std::string an = a.is_virtual ? a.parent : rows[a.row_id].name;
                        std::string bn = b.is_virtual ? b.parent : rows[b.row_id].name;
                        int aa = 0, ba = 0;
                        int ai = 0, bi = 0;
                        if (a.is_virtual) virtual_group_stats(src_group, a.parent, &ai, &aa);
                        else { ai = rows[a.row_id].idx; aa = rows[a.row_id].area; }
                        if (b.is_virtual) virtual_group_stats(src_group, b.parent, &bi, &ba);
                        else { bi = rows[b.row_id].idx; ba = rows[b.row_id].area; }

                        if (g_image_list_sort == ImageListSort::Name && an != bn)
                            return g_image_list_sort_desc ? (an > bn) : (an < bn);
                        if (g_image_list_sort == ImageListSort::Size && aa != ba)
                            return g_image_list_sort_desc ? (aa > ba) : (aa < ba);
                        return g_image_list_sort_desc ? (ai > bi) : (ai < bi);
                    };
                    std::stable_sort(items.begin(), items.end(), item_less);

                    for (const ImagePanelItem &item : items) {
                        if (item.is_virtual) {
                            draw_virtual_group(src_group, item.parent);
                            continue;
                        }

                        int row_id = item.row_id;
                        ImagePanelRow &row = rows[row_id];
                        if (row.children.empty()) {
                            draw_leaf(row_id, false);
                            continue;
                        }

                        draw_parent_group(row_id);
                    }
                    ImGui::TreePop();
                }
                ImGui::EndListBox();
            }
#if 0
            /* Clean, space-saving Sort dropdown that fits inline */
            char sort_desc[64];
            snprintf(sort_desc, sizeof(sort_desc), "Sort: %s %s##imgsort_btn",
                     g_image_list_sort == ImageListSort::Original ? "Order" :
                     g_image_list_sort == ImageListSort::Name ? "Name" : "Size",
                     g_image_list_sort_desc ? "v" : "^");
            if (ImGui::SmallButton(sort_desc)) {
                ImGui::OpenPopup("image_sort_popup");
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Choose image sorting criteria and direction");
            if (ImGui::BeginPopup("image_sort_popup")) {
                if (ImGui::MenuItem("Order", NULL, g_image_list_sort == ImageListSort::Original)) { g_image_list_sort = ImageListSort::Original; }
                if (ImGui::MenuItem("Name", NULL, g_image_list_sort == ImageListSort::Name)) { g_image_list_sort = ImageListSort::Name; }
                if (ImGui::MenuItem("Size", NULL, g_image_list_sort == ImageListSort::Size)) { g_image_list_sort = ImageListSort::Size; }
                ImGui::Separator();
                if (ImGui::MenuItem("Ascending", NULL, !g_image_list_sort_desc)) { g_image_list_sort_desc = false; }
                if (ImGui::MenuItem("Descending", NULL, g_image_list_sort_desc)) { g_image_list_sort_desc = true; }
                ImGui::EndPopup();
            }
#endif

            /* Mark and edit buttons below list. Keep them in short rows so
               the fixed-width side panel never clips the rightmost actions. */
            int n_marked_imgs = CountMarkedImages();
            
            /* Image List Toolbar & Operations (compact version) */
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
            if (ImGui::Button("+##addimg", ImVec2(24, 20))) { g_show_new_blank_dialog = true; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new blank image (W/H prompt)");
            ImGui::SameLine();

            if (g_doc->ilselected < 0) ImGui::BeginDisabled();
            if (ImGui::Button("Dup##img", ImVec2(44, 20))) DuplicateImage();
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate selected sprite (Ctrl+J)");
            ImGui::SameLine();
            if (ImGui::Button("Del##img", ImVec2(44, 20))) RequestDeleteImage(g_doc->ilselected);
            if (g_doc->ilselected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Delete selected sprite (Del)");
            if (g_doc->ilselected < 0) ImGui::EndDisabled();
            ImGui::SameLine();

            if (ImGui::Button("Operations...##imgops", ImVec2(-1, 20))) {
                ImGui::OpenPopup("image_operations_popup");
            }
            ImGui::PopStyleVar();

            if (ImGui::BeginPopup("image_operations_popup")) {
                if (ImGui::BeginMenu("Marking")) {
                    if (ImGui::MenuItem("Mark All")) { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;} }
                    if (ImGui::MenuItem("Clear All")) { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;} }
                    if (ImGui::MenuItem("Invert Marks")) { IMG *p=(IMG*)g_doc->img_p; while(p){p->flags^=1; p=(IMG*)p->nxt_p;} }
                    if (g_doc->ilselected < 0) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Mark Selected")) { IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1; }
                    if (g_doc->ilselected < 0) ImGui::EndDisabled();
                    ImGui::EndMenu();
                }

                if (g_doc->ilselected < 0) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Trim Selected Sprite (Crop)")) {
                    doc_undo_push();
                    int n = CropSelectedImageToContent();
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             n > 0 ? "Trimmed selected sprite to non-transparent bounds."
                                   : "Selected sprite already fits, or has no opaque pixels.");
                    g_restore_msg_timer = 4.0f;
                    if (n > 0) g_zoom_reset = true;
                }
                if (ImGui::MenuItem("Resize Selected Sprite...")) { OpenResizeSpriteDialog(); }
                if (ImGui::MenuItem("Opacity Gradient...")) { OpenOpacityGradientDialog(); }
                if (g_doc->ilselected < 0) ImGui::EndDisabled();

                ImGui::Separator();

                if (ImGui::BeginMenu("Clipboard")) {
                    if (g_doc->ilselected < 0) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Copy Selection as New Sprite", "Ctrl+Shift+C")) CopySelectionToNewImage();
                    if (ImGui::MenuItem("Cut Selection as New Sprite", "Ctrl+Shift+X")) CutSelectionToNewImage();
                    if (g_doc->ilselected < 0) ImGui::EndDisabled();
                    if (!g_clipboard.valid) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Paste Clipboard as New Sprite", "Ctrl+Shift+V")) PasteClipboardAsNewImage();
                    if (!g_clipboard.valid) ImGui::EndDisabled();
                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu("Bulk Operations")) {
                    if (n_marked_imgs == 0) ImGui::BeginDisabled();
                    if (ImGui::MenuItem("Bulk Resize Marked...")) OpenBulkResizeDialog();
                    if (ImGui::MenuItem("Bulk Rename Marked...")) OpenRenameMarkedImages();
                    if (ImGui::MenuItem("Delete Marked Sprites")) RequestDeleteMarkedImages();
                    if (n_marked_imgs == 0) ImGui::EndDisabled();
                    ImGui::EndMenu();
                }

                bool can_break_subframes = (n_marked_imgs > 0 || g_doc->ilselected >= 0);
                if (!can_break_subframes) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Break Subframes (Auto-Chop)...")) OpenAutoChopDialog();
                if (!can_break_subframes) ImGui::EndDisabled();

                ImGui::EndPopup();
            }
        }

        /* --- Palette List & Color Tools --- */
        DrawRightPanelPaletteEditor(panel_h);

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sprite")) {

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                LabeledValue("Name:", "%.15s", img->n_s);
                LabeledValue("Size:", "%d x %d", (int)img->w, (int)img->h);

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
                    LabeledValue("DMA ROM:", "%d B raw", uncomp_size);
                    LabeledValue("",         "%d B compressed", comp_size);
                }

                PAL *pal = get_pal(img->palnum);
                if (pal) LabeledValue("Pal:", "%d  %.9s", (int)img->palnum, pal->n_s);
                else     LabeledValue("Pal:", "%d", (int)img->palnum);
                if (img->opaltbl_p) LabeledValue("AltPal:", "table %d", (int)(short)img->opals);
                else                LabeledValue("AltPal:", "-");
                if (img->pttbl_p)   LabeledValue("PointTbl:", "%u", (unsigned)img->file_pttblnum);
                else                LabeledValue("PointTbl:", "-");

                LabeledValue("AX/AY:",   "%d, %d", (int)(short)img->anix,  (int)(short)img->aniy);
                LabeledValue("AX2/AY2:", "%d, %d", (int)(short)img->anix2, (int)(short)img->aniy2);
                LabeledValue("AZ2:",     "%d",     (int)(short)img->aniz2);

                char flagbuf[48] = {};
                if (img->flags & 1)  strncat(flagbuf, "Marked ", 47);
                if (img->flags & 2)  strncat(flagbuf, "Loaded ", 47);
                if (img->flags & 4)  strncat(flagbuf, "Changed ", 47);
                if (img->flags & 8)  strncat(flagbuf, "Delete ", 47);
                if (!flagbuf[0])     strncpy(flagbuf, "-", 47);
                LabeledValue("Flags:", "0x%04X  %s", (int)img->flags, flagbuf);

                LabeledValue("DATA:", "%p", img->data_p);

                ImGui::Spacing();
                if (g_clipboard.valid) ImGui::TextDisabled("Clip:   %dx%d pixels", g_clipboard.w, g_clipboard.h);
                ImGui::TextDisabled("Undo:   %d/%d", g_undo_idx + 1, g_undo_count);
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Anim Point Editor --- */
        if (ImGui::CollapsingHeader("Anipts Tools")) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                int ax = (short)img->anix, ay = (short)img->aniy;
                int ax2 = (short)img->anix2, ay2 = (short)img->aniy2, az2 = (short)img->aniz2;
                if (AnimPointSliderInt("X1##ptx",  &ax,  -1024, 1024))
                    set_primary_anipoint_local(img, ax, (int)(short)img->aniy);
                if (AnimPointSliderInt("Y1##pty",  &ay,  -1024, 1024))
                    set_primary_anipoint_local(img, (int)(short)img->anix, ay);
                if (AnimPointSliderInt("X2##ptx2", &ax2, -1024, 1024)) {
                    int cur_y2 = secondary_anipoint_in_use(img) ? (int)(short)img->aniy2 : 0;
                    set_secondary_anipoint_local(img, ax2, cur_y2);
                }
                if (AnimPointSliderInt("Y2##pty2", &ay2, -1024, 1024)) {
                    int cur_x2 = secondary_anipoint_in_use(img) ? (int)(short)img->anix2 : 0;
                    set_secondary_anipoint_local(img, cur_x2, ay2);
                }
                if (AnimPointSliderInt("AZ2##ptz2", &az2, -1024, 1024))
                    set_secondary_anipoint_z_local(img, az2);

                ImGui::Spacing();
                if (ImGui::Button("Default Center", ImVec2(-1, 0))) {
                    set_primary_anipoint_local(img,
                                               (int)img->w / 2,
                                               (int)img->h / 2);
                    clear_secondary_anipoint_local(img);
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Centered anim point for %s and cleared secondary.", img->n_s);
                    g_restore_msg_timer = 3.0f;
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Sets X1/Y1 to the sprite center and clears X2/Y2/AZ2 to -1.");

                bool had_second_point = secondary_anipoint_in_use(img);
                if (!had_second_point) ImGui::BeginDisabled();
                if (ImGui::Button("Clear 2nd Point", ImVec2(-1, 0))) {
                    clear_secondary_anipoint_local(img);
                }
                if (had_second_point && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Clears X2/Y2/AZ2. AZ2 becomes -1.");
                if (!had_second_point) ImGui::EndDisabled();

                ImGui::Spacing();
                if (ImGui::Button("Push to Open Tabs", ImVec2(-1, 0))) {
                    int matched = 0;
                    int docs_changed = 0;
                    std::string pattern;
                    int changed = PushAnipointsToMatchingOpenTabs(img, &matched, &docs_changed, &pattern);
                    if (changed > 0) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Pushed anim points to %d sprite%s in %d tab%s.",
                                 changed, changed == 1 ? "" : "s",
                                 docs_changed, docs_changed == 1 ? "" : "s");
                    } else if (matched > 1) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Anim points already match across %d regex hit%s.",
                                 matched, matched == 1 ? "" : "s");
                    } else {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "No other open-tab sprites match %s.",
                                 pattern.empty() ? "that name" : pattern.c_str());
                    }
                    g_restore_msg_timer = 4.0f;
                }
                if (ImGui::IsItemHovered()) {
                    std::string name = img_name_string(img);
                    std::string key = sprite_family_key(name);
                    std::string pattern = sprite_family_regex_pattern(name);
                    ImGui::SetTooltip("Copies these anim points to open-tab sprites matching %s (%s).",
                                      pattern.c_str(), key.c_str());
                }

                if (ImGui::Button("Mirror Marked to Reverse", ImVec2(-1, 0))) {
                    MirrorMarkedAnipointsToReverseWithToast();
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("For every marked sprite, mirrors X1 and active X2 as width - X. Y/Z stay unchanged.");
                }
            } else {
                ImGui::TextDisabled("No image selected");
            }
        }

        /* --- Two-sprite Anipoint Link workspace --- */
        if (ImGui::CollapsingHeader("Anipoint Link")) {
            AnipointLinkState &link = AnipointLink();
            if (ImGui::Button(link.enabled ? "Close Link Workspace" : "Open Link Workspace",
                              ImVec2(-1, 0))) {
                link.enabled = !link.enabled;
                if (link.enabled) {
                    g_world_state.enabled = false;
                    link.reference_doc_idx = document_active_index();
                    link.reference_img_idx = g_doc ? g_doc->ilselected : -1;
                    if (link.target_doc_idx < 0) {
                        link.target_doc_idx = link.reference_doc_idx;
                        link.target_img_idx = link.reference_img_idx;
                    }
                }
            }
            ImGui::TextWrapped("Stage a Reference and Target sprite from any open IMG tab in the main view. Drag from a reference feature to the matching target feature to set the target anipoint.");
        }

        /* --- Hitbox Editor --- */
        if (ImGui::CollapsingHeader("Hitbox")) {
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("X##hbx",  &g_hitbox_x, -1024, 1024)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("Y##hby",  &g_hitbox_y, -1024, 1024)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("W##hbw",  &g_hitbox_w, 1, 2048)) undo_push();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::SliderInt("H##hbh",  &g_hitbox_h, 1, 2048)) undo_push();

            ImGui::Spacing();
            if (ImGui::Button("Copy ASM to Clipboard", ImVec2(-1, 0))) {
                char buf[128];
                snprintf(buf, sizeof(buf), "\t.word   %d,%d,%d,%d\t; Hitbox X, Y, W, H\n", g_hitbox_x, g_hitbox_y, g_hitbox_w, g_hitbox_h);
                ImGui::SetClipboardText(buf);
            }
        }

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Animation", NULL,
                                g_request_animation_sidebar
                                    ? ImGuiTabItemFlags_SetSelected : 0)) {
        g_request_animation_sidebar = false;
        /* --- Library Info --- */
        if (ImGui::CollapsingHeader("Library")) {
            int altpal_tables = 0;
            int point_tables = 0;
            for (IMG *scan = (IMG *)g_doc->img_p; scan; scan = (IMG *)scan->nxt_p) {
                if (scan->opaltbl_p) altpal_tables++;
                if (scan->pttbl_p) point_tables++;
            }
            ImGui::Text("Images:   %u", g_doc->imgcnt);
            ImGui::Text("Palettes: %u", g_doc->palcnt);
            ImGui::Text("Seqs:     %u", g_doc->seqcnt);
            ImGui::Text("Scripts:  %u", g_doc->scrcnt);
            ImGui::Text("DamTbls:  %u", g_doc->damcnt);
            ImGui::Text("AltPals:  %d", altpal_tables);
            ImGui::Text("PtTbls:   %d", point_tables);
            ImGui::Text("Version:  0x%04X", g_doc->fileversion);
            bool has_anim_blob = g_doc->scrseqmem_p && g_doc->scrseqbytes > 0;
            ImGui::Text("AnimBlob: %u B", g_doc->scrseqbytes);
            if (!has_anim_blob) ImGui::BeginDisabled();
            if (ImGui::Button("View/Edit Anim Data", ImVec2(-1, 0)))
                g_show_seqscr_editor = true;
            if (!has_anim_blob) ImGui::EndDisabled();
        }

        std::vector<SeqScrRecordView> seqscr_records;
        bool seqscr_truncated = false;
        bool has_seqscr_records = SeqScrBuildRecords(seqscr_records,
                                                     &seqscr_truncated);
        static int s_seqscr_panel_doc_idx = -1;
        static int s_seqscr_panel_selected = -1;
        int active_doc_idx_for_seqscr = document_active_index();
        if (s_seqscr_panel_doc_idx != active_doc_idx_for_seqscr) {
            s_seqscr_panel_doc_idx = active_doc_idx_for_seqscr;
            s_seqscr_panel_selected = -1;
        }
        auto find_seqscr_record = [&](int record_index) -> const SeqScrRecordView * {
            for (const SeqScrRecordView &rec : seqscr_records) {
                if (rec.index == record_index) return &rec;
            }
            return NULL;
        };
        auto load_seqscr_record = [&](const SeqScrRecordView &rec,
                                      const char *name) {
            s_seqscr_panel_doc_idx = active_doc_idx_for_seqscr;
            s_seqscr_panel_selected = rec.index;
            const char *kind = rec.script ? "script" : "sequence";
            if (WorldLoadSeqScrRecord(rec.index)) {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Loaded %s '%s' into World View.", kind, name);
            } else {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Could not load %s '%s'.", kind, name);
            }
            g_restore_msg_timer = 4.0f;
        };
        auto copy_seqscr_record_asm = [&](int record_index, const char *name) {
            g_world_marked_state.generated_asm =
                WorldBuildSeqScrAsmExport(record_index);
            ImGui::SetClipboardText(g_world_marked_state.generated_asm.c_str());
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Copied anim ASM for '%s'.", name);
            g_restore_msg_timer = 4.0f;
        };
        auto add_seqscr_record = [&](bool scripts) {
            if (SeqScrAddRecord(scripts)) {
                int new_global = scripts ? (int)(g_doc->seqcnt + g_doc->scrcnt) - 1
                                         : (int)g_doc->seqcnt - 1;
                int local_idx = scripts ? new_global - (int)g_doc->seqcnt : new_global;
                s_seqscr_panel_doc_idx = active_doc_idx_for_seqscr;
                s_seqscr_panel_selected = new_global;
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Added %s %d. Use Edit... to fill in its entries.",
                         scripts ? "script" : "sequence", local_idx);
            } else {
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Could not add %s (anim blob is truncated or out of memory).",
                         scripts ? "script" : "sequence");
            }
            g_restore_msg_timer = 4.0f;
        };
        auto draw_seqscr_name_list = [&](const char *title, bool scripts,
                                         const char *id_part) {
            if (ImGui::CollapsingHeader(title)) {
                float row_h = ImGui::GetTextLineHeightWithSpacing();
                float list_h = row_h * (scripts ? 6.0f : 8.0f);
                float max_list_h = panel_h * 0.24f;
                if (list_h > max_list_h) list_h = max_list_h;
                if (list_h < row_h * 3.0f) list_h = row_h * 3.0f;

                if (!has_seqscr_records) {
                    ImGui::BeginDisabled();
                    char empty_id[48];
                    snprintf(empty_id, sizeof(empty_id), "##%s_empty", id_part);
                    ImGui::BeginListBox(empty_id, ImVec2(-1, list_h));
                    ImGui::TextDisabled("None");
                    ImGui::EndListBox();
                    ImGui::EndDisabled();
                } else {
                    char list_id[48];
                    snprintf(list_id, sizeof(list_id), "##%s_list", id_part);
                    if (ImGui::BeginListBox(list_id, ImVec2(-1, list_h))) {
                        if (ImGui::IsWindowHovered() &&
                            ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                            g_palette_nav = false;
                        bool any = false;
                        for (const SeqScrRecordView &rec : seqscr_records) {
                            if (rec.script != scripts) continue;
                            any = true;
                            int local_idx = scripts ? rec.index - (int)g_doc->seqcnt
                                                    : rec.index;
                            char fallback[32];
                            snprintf(fallback, sizeof(fallback), "%s %d",
                                     scripts ? "Script" : "Sequence", local_idx);
                            const char *name = rec.name[0] ? rec.name : fallback;
                            char label[96];
                            snprintf(label, sizeof(label), "%02d  %.48s%s",
                                     local_idx, name,
                                     rec.truncated ? "  (truncated)" : "");
                            bool selected =
                                s_seqscr_panel_doc_idx == active_doc_idx_for_seqscr &&
                                s_seqscr_panel_selected == rec.index;
                            bool loaded =
                                g_world_marked_state.embedded_active &&
                                g_world_marked_state.embedded_doc_idx ==
                                    active_doc_idx_for_seqscr &&
                                g_world_marked_state.embedded_record_index == rec.index;
                            if (loaded) selected = true;

                            ImGui::PushID(rec.index);
                            if (rec.truncated) ImGui::BeginDisabled();
                            if (loaded)
                                ImGui::PushStyleColor(ImGuiCol_Text,
                                                      ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                            if (ImGui::Selectable(label, selected)) {
                                load_seqscr_record(rec, name);
                            }
                            if (loaded) ImGui::PopStyleColor();
                            if (ImGui::IsItemHovered()) {
                                ImGui::SetTooltip("Load into World View frame sequence");
                            }
                            if (ImGui::BeginPopupContextItem("##seqscr_ctx")) {
                                if (ImGui::MenuItem("Load in World View"))
                                    load_seqscr_record(rec, name);
                                if (ImGui::MenuItem("Copy Anim ASM"))
                                    copy_seqscr_record_asm(rec.index, name);
                                if (ImGui::MenuItem("Save Anim ASM")) {
                                    g_world_marked_state.generated_asm =
                                        WorldBuildSeqScrAsmExport(rec.index);
                                    g_request_save_world_asm = true;
                                }
                                ImGui::Separator();
                                if (ImGui::MenuItem("View/Edit Anim Data"))
                                    g_show_seqscr_editor = true;
                                ImGui::EndPopup();
                            }
                            if (rec.truncated) ImGui::EndDisabled();
                            ImGui::PopID();
                        }
                        if (!any) ImGui::TextDisabled("None");
                        ImGui::EndListBox();
                    }
                }

                const SeqScrRecordView *selected_rec =
                    find_seqscr_record(s_seqscr_panel_selected);
                bool selected_ok = selected_rec && selected_rec->script == scripts &&
                                   !selected_rec->truncated;
                char selected_name_buf[32];
                const char *selected_name = "";
                if (selected_ok) {
                    int local_idx = selected_rec->script
                                  ? selected_rec->index - (int)g_doc->seqcnt
                                  : selected_rec->index;
                    snprintf(selected_name_buf, sizeof(selected_name_buf),
                             "%s %d",
                             selected_rec->script ? "Script" : "Sequence",
                             local_idx);
                    selected_name = selected_rec->name[0]
                                  ? selected_rec->name : selected_name_buf;
                }
                ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                char add_id[48];
                snprintf(add_id, sizeof(add_id), "+##%s_add", id_part);
                if (seqscr_truncated) ImGui::BeginDisabled();
                if (ImGui::Button(add_id, ImVec2(24, 20)))
                    add_seqscr_record(scripts);
                if (ImGui::IsItemHovered() && !seqscr_truncated)
                    ImGui::SetTooltip("Append a new empty %s; edit its entries with Edit...",
                                      scripts ? "script" : "sequence");
                if (seqscr_truncated) ImGui::EndDisabled();
                ImGui::SameLine();

                if (!selected_ok) ImGui::BeginDisabled();
                char load_id[48];
                snprintf(load_id, sizeof(load_id), "Load##%s_load", id_part);
                if (ImGui::Button(load_id, ImVec2(52, 20)))
                    load_seqscr_record(*selected_rec, selected_name);
                ImGui::SameLine();
                char copy_id[48];
                snprintf(copy_id, sizeof(copy_id), "Copy ASM##%s_copy", id_part);
                if (ImGui::Button(copy_id, ImVec2(78, 20)))
                    copy_seqscr_record_asm(selected_rec->index, selected_name);
                if (!selected_ok) ImGui::EndDisabled();
                ImGui::SameLine();
                char edit_id[48];
                snprintf(edit_id, sizeof(edit_id), "Edit...##%s_edit", id_part);
                if (ImGui::Button(edit_id, ImVec2(-1, 20)))
                    g_show_seqscr_editor = true;
                ImGui::PopStyleVar();

                if (seqscr_truncated) {
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.3f, 1.0f),
                                       "SEQSCR blob is truncated.");
                }
            }
        };
        draw_seqscr_name_list("Sequences", false, "seqscr_seq");
        draw_seqscr_name_list("Scripts", true, "seqscr_scr");
        if (has_seqscr_records) {
            bool loaded_embedded =
                g_world_marked_state.embedded_active &&
                g_world_marked_state.embedded_doc_idx == document_active_index() &&
                g_world_marked_state.embedded_record_index >= 0;
            if (!loaded_embedded) ImGui::BeginDisabled();
            if (ImGui::Button("Copy Loaded Anim ASM", ImVec2(-1, 0))) {
                g_world_marked_state.generated_asm =
                    WorldBuildSeqScrAsmExport(g_world_marked_state.embedded_record_index);
                ImGui::SetClipboardText(g_world_marked_state.generated_asm.c_str());
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Copied loaded sequence/script ASM.");
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::Button("Save Loaded Anim ASM", ImVec2(-1, 0))) {
                g_world_marked_state.generated_asm =
                    WorldBuildSeqScrAsmExport(g_world_marked_state.embedded_record_index);
                g_request_save_world_asm = true;
            }
            if (!loaded_embedded) ImGui::EndDisabled();
            if (ImGui::Button("Copy All Anim ASM", ImVec2(-1, 0))) {
                g_world_marked_state.generated_asm = WorldBuildSeqScrAsmExport(-1);
                ImGui::SetClipboardText(g_world_marked_state.generated_asm.c_str());
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         "Copied all embedded sequence/script ASM.");
                g_restore_msg_timer = 4.0f;
            }
            ImGui::TextDisabled("Use Export > Write TBL for marked image records.");
        }
        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();

    /* ===== CANVAS ===== */
    DrawCanvasWindow(canvas_x, canvas_y, canvas_w, canvas_h);

    /* ===== BOTTOM TIMELINE BAR =====
       State (g_is_playing/g_play_speed/g_play_timer/g_timeline_frames/
       g_timeline_play_idx/g_timeline_built_for_imgcnt/g_timeline_onion) is
       at file scope so keyboard shortcuts and other overlays can address it. */

    /* Drop stale frame indices if the underlying image set shrank or was reloaded */
    if (g_doc->imgcnt != g_timeline_built_for_imgcnt) {
        EnsureTimelineHolds();
        std::vector<int> valid_frames;
        std::vector<int> valid_holds;
        valid_frames.reserve(g_timeline_frames.size());
        valid_holds.reserve(g_timeline_holds.size());
        for (size_t i = 0; i < g_timeline_frames.size(); i++) {
            int idx = g_timeline_frames[i];
            if (idx < 0 || (unsigned int)idx >= g_doc->imgcnt) continue;
            valid_frames.push_back(idx);
            valid_holds.push_back(g_timeline_holds[i]);
        }
        g_timeline_frames.swap(valid_frames);
        g_timeline_holds.swap(valid_holds);
        /* Free thumbnail textures past the new end. */
        for (size_t i = g_doc->imgcnt; i < g_thumb_cache.size(); i++) {
            if (g_thumb_cache[i].tex) SDL_DestroyTexture(g_thumb_cache[i].tex);
        }
        if (g_thumb_cache.size() > g_doc->imgcnt) g_thumb_cache.resize(g_doc->imgcnt);
        g_timeline_built_for_imgcnt = g_doc->imgcnt;
        PruneTimelineCompositeSelection();
    }

    /* Build default timeline if empty (fresh file, or after Reset Sequence) */
    if (g_timeline_frames.empty() && g_doc->imgcnt > 0) {
        for (unsigned int i = 0; i < g_doc->imgcnt; i++) {
            IMG *p = get_img(i);
            if (p && (p->flags & 1)) TimelinePushFrame((int)i);
        }
        if (g_timeline_frames.empty()) {
            for (unsigned int i = 0; i < g_doc->imgcnt; i++) TimelinePushFrame((int)i);
        }
    }

    if (g_timeline_play_idx >= (int)g_timeline_frames.size())
        g_timeline_play_idx = 0;

    if (world_sequence_timeline)
        g_is_playing = false;
    
    /* Playback logic */
    if (g_is_playing && !g_timeline_frames.empty()) {
        EnsureTimelineHolds();
        g_play_timer += ImGui::GetIO().DeltaTime;
        float frame_seconds = (float)TimelineHoldAt(g_timeline_play_idx) / g_play_speed;
        if (frame_seconds < 0.001f) frame_seconds = 0.001f;
        if (g_play_timer >= frame_seconds) {
            g_play_timer = 0.0f;
            int n = (int)g_timeline_frames.size();
            int step_delta = 1;
            if (g_timeline_pingpong && n > 1) {
                /* Ping-pong: walk in g_timeline_play_dir and bounce at the
                   endpoints, landing on them once per cycle. e.g. for 7
                   frames the sequence is 0,1,2,3,4,5,6,5,4,3,2,1,0,1,...
                   The bounce happens on the frame we'd otherwise overshoot:
                   when the next step would leave the [0, n-1] range, flip
                   direction and step inward by 2 instead of out by 1. */
                int next = g_timeline_play_idx + g_timeline_play_dir;
                if (next >= n || next < 0) {
                    g_timeline_play_dir = -g_timeline_play_dir;
                    next = g_timeline_play_idx + g_timeline_play_dir;
                    if (next < 0) next = 0;
                    if (next >= n) next = n - 1;
                }
                step_delta = next - g_timeline_play_idx;
            }
            StepTimelinePlayhead(step_delta);
        }
    } else if (!g_is_playing && !g_timeline_frames.empty()) {
        /* Sync play_idx with manual selection if possible */
        if (g_doc->ilselected != g_timeline_frames[g_timeline_play_idx]) {
            for (size_t i = 0; i < g_timeline_frames.size(); i++) {
                if (g_timeline_frames[i] == g_doc->ilselected) {
                    g_timeline_play_idx = (int)i;
                    break;
                }
            }
        }
    }

    float timeline_y = canvas_y + canvas_h;
    float timeline_x = world_sequence_timeline ? canvas_x : 0.0f;
    float timeline_w = world_sequence_timeline ? canvas_w : sw;
    ImGui::SetNextWindowPos(ImVec2(timeline_x, timeline_y));
    ImGui::SetNextWindowSize(ImVec2(timeline_w, timeline_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
    ImGui::Begin("##timeline", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
    {
        if (world_sequence_timeline) {
            DrawWorldMarkedTimelinePanel();
        } else {
        ImGui::Text("Animation Timeline");
        ImGui::SameLine(180);
        
        if (g_is_playing) {
            if (ImGui::Button("\xEE\x81\x8D Stop", ImVec2(80, 0))) { /* U+E04D stop */
                g_is_playing = false;
            }
        } else {
            if (ImGui::Button("\xEE\x80\xB7 Play", ImVec2(80, 0))) { /* U+E037 play_arrow */
                if (!g_is_playing && !g_timeline_frames.empty()) {
                    if (TimelineCompositeReady()) {
                        int p0 = TimelineFramePosition(g_timeline_composite[0]);
                        if (p0 >= 0) g_timeline_play_idx = p0;
                    }
                    g_play_timer = 0.0f;
                    g_is_playing = true;
                    g_doc->ilselected = g_timeline_frames[g_timeline_play_idx];
                    g_zoom_reset = true;
                }
            }
        }
        ImGui::SameLine();
        ImGui::PushItemWidth(120);
        ImGui::SliderFloat("FPS", &g_play_speed, 1.0f, 60.0f, "%.1f");
        ImGui::PopItemWidth();

        ImGui::SameLine();
        int cur_hold = TimelineHoldAt(g_timeline_play_idx);
        ImGui::PushItemWidth(72);
        if (ImGui::InputInt("Hold", &cur_hold, 1, 4))
            TimelineSetHoldAt(g_timeline_play_idx, cur_hold);
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Base ticks to wait before this frame advances. At 12 FPS, Hold 3 lasts 0.25 seconds.");
        
        ImGui::SameLine();
        if (ImGui::Button("Reset Sequence")) {
            TimelineClearFrames();
            ClearTimelineCompositeSelection();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Onion", &g_timeline_onion);
        /* SetTooltip is printf-style; escape the literal % so it isn't read
           as a format specifier (CodeQL cpp/wrong-number-format-arguments). */
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Ghost prev/next timeline frame at 25%% alpha while scrubbing or playing");
        ImGui::SameLine();
        if (ImGui::Checkbox("Ping-Pong", &g_timeline_pingpong)) {
            /* Reset direction so the first cycle after enabling always
               starts forward, regardless of which way we were going. */
            g_timeline_play_dir = 1;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Play forward then reverse and loop (e.g. 1-7 then 7-1, repeating)");
        if (TimelineCompositeReady()) {
            ImGui::SameLine();
            DrawTimelineCompositeLockToggle(0, "Back");
            ImGui::SameLine();
            DrawTimelineCompositeLockToggle(1, "Front");
            ImGui::SameLine();
            bool can_auto_anipts = TimelineAnyCompositeLocked() &&
                                   g_timeline_frames.size() > 1;
            if (!can_auto_anipts) ImGui::BeginDisabled();
            if (ImGui::SmallButton("Auto Anipts")) {
                int changed = AutoCalculateTimelineAnipointsFromLock();
                if (changed > 0) {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Auto-calculated anim points for %d timeline frame%s.",
                             changed, changed == 1 ? "" : "s");
                } else {
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Timeline anim points already match sprite sizes.");
                }
                g_restore_msg_timer = 4.0f;
            }
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Uses the locked frame as the anchor, then offsets each neighboring frame by half the sprite size difference.");
            }
            if (!can_auto_anipts) ImGui::EndDisabled();
        }

        /* Draw a horizontal scrolling list of frames with drag & drop. Buttons
           render a per-frame thumbnail so the user can scan visually instead
           of by numeric index. */
        ImGui::Dummy(ImVec2(0, 4));
        float scr_w = ImGui::GetContentRegionAvail().x;
        float scr_h = ImGui::GetContentRegionAvail().y;

        ImGui::BeginChild("TimelineScrubber", ImVec2(scr_w, scr_h), false, ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoBackground);
        if (!g_timeline_frames.empty()) {
            for (size_t i = 0; i < g_timeline_frames.size(); i++) {
                if (i > 0) ImGui::SameLine(0, 4.0f);
                ImGui::PushID((int)i);

                int img_idx = g_timeline_frames[i];
                char label[32];
                snprintf(label, sizeof(label), "%d", img_idx);

                bool is_current = (int)i == g_timeline_play_idx;
                int composite_slot = TimelineCompositeSlot(img_idx);
                if (is_current) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.5f, 0.8f, 1.0f));
                }

                TimelineThumb *t = EnsureThumb(img_idx);
                bool clicked = false;
                ImVec2 item_min(0, 0), item_max(0, 0);
                if (t && t->tex) {
                    /* Frame: thumb + index label below in same button. We use
                       an ImageButton with the rendered thumbnail and overlay
                       text via the drawlist after. */
                    ImVec2 btn_sz(52, 52);
                    ImVec2 cursor = ImGui::GetCursorScreenPos();
                    if (ImGui::ImageButton(label, (ImTextureID)(intptr_t)t->tex,
                                           btn_sz, ImVec2(0,0), ImVec2(1,1),
                                           ImVec4(0,0,0,0),
                                           is_current ? ImVec4(0.4f,0.7f,1.f,1.f) : ImVec4(1,1,1,1))) {
                        clicked = true;
                    }
                    item_min = ImGui::GetItemRectMin();
                    item_max = ImGui::GetItemRectMax();
                    ImDrawList *fdl = ImGui::GetWindowDrawList();
                    fdl->AddText(ImVec2(cursor.x + 4, cursor.y + 2),
                                 IM_COL32(255,255,255,200), label);
                } else {
                    if (ImGui::Button(label, ImVec2(48, 48))) clicked = true;
                    item_min = ImGui::GetItemRectMin();
                    item_max = ImGui::GetItemRectMax();
                }
                int frame_hold = TimelineHoldAt((int)i);
                if (frame_hold > 1) {
                    char hold_label[16];
                    snprintf(hold_label, sizeof(hold_label), "x%d", frame_hold);
                    ImDrawList *fdl = ImGui::GetWindowDrawList();
                    ImVec2 hold_sz = ImGui::CalcTextSize(hold_label);
                    ImVec2 hold_min(item_max.x - hold_sz.x - 8.0f, item_max.y - hold_sz.y - 6.0f);
                    ImVec2 hold_max(item_max.x - 2.0f, item_max.y - 2.0f);
                    fdl->AddRectFilled(hold_min, hold_max, IM_COL32(0, 0, 0, 185), 2.0f);
                    fdl->AddText(ImVec2(hold_min.x + 3.0f, hold_min.y + 1.0f),
                                 IM_COL32(255, 235, 130, 255), hold_label);
                }
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("Ctrl-click two frames to pair them.\nPlay or Left/Right advances both positions together.\nHold: x%d", frame_hold);
                }
                if (composite_slot >= 0) {
                    ImDrawList *fdl = ImGui::GetWindowDrawList();
                    ImU32 col = (composite_slot == 0)
                        ? IM_COL32(120, 190, 255, 255)
                        : IM_COL32(255, 190, 90, 255);
                    fdl->AddRect(item_min, item_max, col, 0.0f, 0, 3.0f);
                }
                if (clicked) {
                    if (io.KeyCtrl) {
                        ToggleTimelineCompositeFrame(img_idx);
                    } else {
                        ClearTimelineCompositeSelection();
                    }
                    g_timeline_play_idx = (int)i;
                    g_doc->ilselected = img_idx;
                    g_zoom_reset = true;
                }

                if (is_current) {
                    ImGui::PopStyleColor();
                }
                
                if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
                    int payload_idx = (int)i;
                    ImGui::SetDragDropPayload("TIMELINE_FRAME", &payload_idx, sizeof(int));
                    ImGui::Text("Move frame %d", img_idx);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::BeginDragDropTarget()) {
                    if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("TIMELINE_FRAME")) {
                        int src_idx = *(const int*)payload->Data;
                        int dst_idx = (int)i;
                        TimelineMoveFrame(src_idx, dst_idx);
                    }
                    ImGui::EndDragDropTarget();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
        }
    }
    ImGui::End();
    ImGui::PopStyleVar();

    /* ===== BOTTOM PALETTE BAR ===== */
    if (!hide_bottom_palette) {
        ImGui::SetNextWindowPos(ImVec2(0, sh - PALETTE_H));
        ImGui::SetNextWindowSize(ImVec2(sw, PALETTE_H));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(2, 4));
        ImGui::Begin("##palette", NULL,
            ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollbar);
        {
            DrawBottomPaletteBar(ImVec2(sw, PALETTE_H));
        }
        ImGui::End();
        ImGui::PopStyleVar();
    }


    DrawRenameDialog();

    DrawLoad2VerifyDialog();

    DrawPaletteMergeQualityDialog();

    DrawSpriteLayerPanel();

    if (g_request_save_world_asm) { g_request_save_world_asm = false; OpenFileDialog(FileDialogMode::SaveAsmAnim); }
    if (g_request_save_world_project) { g_request_save_world_project = false; OpenFileDialog(FileDialogMode::SaveWorldProject); }
    if (g_request_load_world_project) { g_request_load_world_project = false; OpenFileDialog(FileDialogMode::LoadWorldProject); }
    if (g_request_load_asm)       { g_request_load_asm = false; g_asm_dialog_opponent = false; OpenFileDialog(FileDialogMode::LoadAsmAnim); }
    if (g_request_load_opp_asm)   { g_request_load_opp_asm = false; g_asm_dialog_opponent = true; OpenFileDialog(FileDialogMode::LoadAsmAnim); }
    if (g_request_asm_autoload)     { g_request_asm_autoload = false; AsmProcessAutoload(); }
    if (g_request_asm_opp_autoload) { g_request_asm_opp_autoload = false; AsmProcessOppAutoload(); }
    if (g_request_locate_img)     { g_request_locate_img = false; g_openimg_for_asm = true; OpenFileDialog(FileDialogMode::OpenImg); }
    else if (g_request_locate_opp_img) { g_request_locate_opp_img = false; g_openimg_for_opp = true; OpenFileDialog(FileDialogMode::OpenImg); }

    DrawAsmAnimWindow();

    DrawPaletteHistogramDialog();

    DrawPaletteSingleColorDialog();

    DrawIndexedGradientDialog();

    DrawPaletteReduceDialog();

    DrawMk2HitboxWindow();

    DrawMk2FatalityWindow();

    DrawAutoChopDialog();

    DrawResizeSpriteDialog();
    DrawOpacityGradientDialog();
    DrawInnerStrokeDialog();
    DrawSpriteCleanupDialog();

    DrawBulkResizeDialog();

    DrawSetGroupAnipointsDialog();

    DrawBulkRestoreRegexDialog();

    DrawDeleteImagesConfirm();

    DrawSeqScrEditorWindow();

    DrawDebugInfoModal();

    /* ===== FILE DIALOG ===== */
    DrawFileDialog();

    DrawNewImgConfirm();

    DrawNewBlankImageDialog();

    DrawUnsavedChangesConfirm();

    DrawMk2UnsavedChangesConfirm();

    DrawMk2FatalityUnsavedChangesConfirm();

    DrawHelpModal();

    DrawAboutModal();
    DrawTransientToast(io.DeltaTime);
    DrawVerboseLogWindow();
    finish_sequence_anipoint_edit_if_idle();

    /* Flush to renderer */
}


/* =========================================================
   Extracted list/image mutations from imgui_overlay.cpp
   ========================================================= */

// Extracted from imgui_overlay.cpp: RemapTimelineAfterImageDelete
static void RemapTimelineAfterImageDelete(const std::vector<int> &deleted)
{
    if (deleted.empty()) return;

    auto remap_index = [&](int idx) {
        if (idx < 0) return -1;
        if (std::binary_search(deleted.begin(), deleted.end(), idx)) return -1;
        int shift = (int)(std::lower_bound(deleted.begin(), deleted.end(), idx) - deleted.begin());
        return idx - shift;
    };

    EnsureTimelineHolds();
    std::vector<int> remapped_frames;
    std::vector<int> remapped_holds;
    remapped_frames.reserve(g_timeline_frames.size());
    remapped_holds.reserve(g_timeline_holds.size());
    for (size_t i = 0; i < g_timeline_frames.size(); i++) {
        int idx = remap_index(g_timeline_frames[i]);
        if (idx < 0) continue;
        remapped_frames.push_back(idx);
        remapped_holds.push_back(g_timeline_holds[i]);
    }
    g_timeline_frames.swap(remapped_frames);
    g_timeline_holds.swap(remapped_holds);

    for (int i = 0; i < 2; i++) {
        g_timeline_composite[i] = remap_index(g_timeline_composite[i]);
        if (g_timeline_composite[i] < 0)
            g_timeline_composite_locked[i] = false;
    }
    CompactTimelineCompositeSelection();
    if (g_timeline_composite[0] < 0 || g_timeline_composite[1] < 0)
        ClearTimelineCompositeSelection();

    if (g_timeline_play_idx >= (int)g_timeline_frames.size())
        g_timeline_play_idx = 0;
    g_timeline_built_for_imgcnt = g_doc->imgcnt;
    ClearTimelineThumbCache();
}


// Extracted from imgui_overlay.cpp: DeleteImagesByIndices
int DeleteImagesByIndices(std::vector<int> indices)
{
    g_last_delete_removed_palettes = 0;
    NormalizeImageDeleteIndices(&indices);
    if (indices.empty()) return 0;

    std::vector<int> candidate_palettes;
    candidate_palettes.reserve(indices.size());
    for (int delete_idx : indices) {
        IMG *img = get_img(delete_idx);
        if (img) candidate_palettes.push_back((int)img->palnum);
    }
    std::sort(candidate_palettes.begin(), candidate_palettes.end());
    candidate_palettes.erase(std::unique(candidate_palettes.begin(), candidate_palettes.end()),
                             candidate_palettes.end());

    doc_undo_push();

    IMG *prev = NULL;
    IMG *curr = (IMG *)g_doc->img_p;
    int idx = 0;
    int deleted_count = 0;
    int deleted_before_sel = 0;
    bool sel_was_deleted = false;
    int old_sel = g_doc->ilselected;

    while (curr) {
        bool delete_this = std::binary_search(indices.begin(), indices.end(), idx);
        if (delete_this) {
            IMG *to_delete = curr;
            if (prev) prev->nxt_p = curr->nxt_p;
            else g_doc->img_p = curr->nxt_p;
            curr = (IMG *)curr->nxt_p;
            g_doc->imgcnt--;
            deleted_count++;

            if (idx < old_sel) deleted_before_sel++;
            else if (idx == old_sel) sel_was_deleted = true;

            FreeImg(to_delete);
        } else {
            prev = curr;
            curr = (IMG *)curr->nxt_p;
        }
        idx++;
    }

    if (g_doc->imgcnt == 0) {
        g_doc->ilselected = -1;
    } else {
        int new_sel = old_sel - deleted_before_sel;
        if (sel_was_deleted && new_sel >= (int)g_doc->imgcnt)
            new_sel = (int)g_doc->imgcnt - 1;
        if (new_sel < 0) new_sel = 0;
        if (new_sel >= (int)g_doc->imgcnt) new_sel = (int)g_doc->imgcnt - 1;
        g_doc->ilselected = new_sel;
    }

    RemapTimelineAfterImageDelete(indices);
    int deleted_palettes = 0;
    if (!candidate_palettes.empty() && g_doc->palcnt > 0) {
        std::vector<unsigned char> used((size_t)g_doc->palcnt, 0);
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            int pal_idx = (int)img->palnum;
            if (pal_idx >= 0 && (unsigned int)pal_idx < g_doc->palcnt)
                used[(size_t)pal_idx] = 1;
        }

        std::sort(candidate_palettes.begin(), candidate_palettes.end(), std::greater<int>());
        for (int pal_idx : candidate_palettes) {
            if (pal_idx < 0 || (unsigned int)pal_idx >= g_doc->palcnt) continue;
            if (used[(size_t)pal_idx]) continue;

            PAL *prev_pal = NULL;
            PAL *pal = (PAL *)g_doc->pal_p;
            for (int i = 0; pal && i < pal_idx; i++) {
                prev_pal = pal;
                pal = (PAL *)pal->nxt_p;
            }
            if (!pal) continue;

            if (prev_pal) prev_pal->nxt_p = pal->nxt_p;
            else g_doc->pal_p = pal->nxt_p;
            FreePal(pal);
            g_doc->palcnt--;
            deleted_palettes++;

            for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > pal_idx)
                    img->palnum--;
            }
            if (g_doc->plselected == pal_idx)
                g_doc->plselected = -1;
            else if (g_doc->plselected > pal_idx)
                g_doc->plselected--;
        }
    }
    g_last_delete_removed_palettes = deleted_palettes;

    IMG *sel = get_img(g_doc->ilselected);
    if (sel && (unsigned int)sel->palnum < g_doc->palcnt)
        g_doc->plselected = (int)sel->palnum;
    else if (g_doc->palcnt == 0)
        g_doc->plselected = -1;
    else if (g_doc->plselected < 0 || (unsigned int)g_doc->plselected >= g_doc->palcnt)
        g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    g_palette_nav = false;
    if (deleted_palettes > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Deleted %d sprite%s and %d now-unused palette%s.",
                 deleted_count, deleted_count == 1 ? "" : "s",
                 deleted_palettes, deleted_palettes == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
    }
    return deleted_count;
}


// Extracted from imgui_overlay.cpp: DeleteImage
static int DeleteImage(int idx)
{
    std::vector<int> indices;
    indices.push_back(idx);
    return DeleteImagesByIndices(indices);
}


// Extracted from imgui_overlay.cpp: DeleteMarkedImages
static int DeleteMarkedImages(void)
{
    std::vector<int> indices;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img->flags & 1) indices.push_back(idx);
    }
    return DeleteImagesByIndices(indices);
}


// Extracted from imgui_overlay.cpp: CollectSubframeIndicesForParent
static void CollectSubframeIndicesForParent(int parent_idx, std::vector<int> *out)
{
    if (!out) return;
    out->clear();
    IMG *parent = get_img(parent_idx);
    if (!parent) return;

    std::string parent_name = img_name_string(parent);
    if (parent_name.empty()) return;
    std::string parent_src = parent->src_filename[0] ? parent->src_filename : "Workspace";

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (idx == parent_idx) continue;
        std::string src = img->src_filename[0] ? img->src_filename : "Workspace";
        if (src != parent_src) continue;
        std::string child_name = img_name_string(img);
        bool belongs_to_parent =
            (InferSubframeParentName(child_name.c_str()) == parent_name);
        if (!belongs_to_parent) {
            std::string numbered_parent;
            belongs_to_parent =
                strip_trailing_sequence_digits(child_name, &numbered_parent) &&
                numbered_parent == parent_name;
        }
        if (belongs_to_parent)
            out->push_back(idx);
    }
}


// Extracted from imgui_overlay.cpp: CollectExtraSubframeIndices
static void CollectExtraSubframeIndices(const std::vector<int> &base_indices,
                                        std::vector<int> *extra_indices,
                                        int *parent_count)
{
    if (extra_indices) extra_indices->clear();
    if (parent_count) *parent_count = 0;
    if (!extra_indices || base_indices.empty()) return;

    std::vector<int> base = base_indices;
    NormalizeImageDeleteIndices(&base);

    for (int idx : base) {
        std::vector<int> children;
        CollectSubframeIndicesForParent(idx, &children);

        bool parent_has_extra = false;
        for (int child_idx : children) {
            if (std::binary_search(base.begin(), base.end(), child_idx)) continue;
            if (std::find(extra_indices->begin(), extra_indices->end(), child_idx) != extra_indices->end()) continue;
            extra_indices->push_back(child_idx);
            parent_has_extra = true;
        }
        if (parent_has_extra && parent_count) (*parent_count)++;
    }

    NormalizeImageDeleteIndices(extra_indices);
}


// Extracted from imgui_overlay.cpp: ClearPendingImageDelete
void ClearPendingImageDelete(void)
{
    g_pending_delete_base_indices.clear();
    g_pending_delete_subframe_indices.clear();
    g_pending_delete_parent_name[0] = '\0';
    g_show_delete_images_confirm = false;
}


// Extracted from imgui_overlay.cpp: RequestDeleteImage
void RequestDeleteImage(int idx)
{
    if (idx < 0 || (unsigned int)idx >= g_doc->imgcnt) return;

    std::vector<int> base;
    std::vector<int> extra;
    base.push_back(idx);
    CollectExtraSubframeIndices(base, &extra, NULL);
    if (extra.empty()) {
        DeleteImage(idx);
        return;
    }

    IMG *img = get_img(idx);
    snprintf(g_pending_delete_parent_name, sizeof(g_pending_delete_parent_name),
             "%.15s", img ? img->n_s : "sprite");
    g_pending_delete_base_indices = base;
    g_pending_delete_subframe_indices = extra;
    g_show_delete_images_confirm = true;
}


// Extracted from imgui_overlay.cpp: RequestDeleteMarkedImages
void RequestDeleteMarkedImages(void)
{
    std::vector<int> base;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img->flags & 1) base.push_back(idx);
    }
    NormalizeImageDeleteIndices(&base);
    if (base.empty()) return;

    std::vector<int> extra;
    CollectExtraSubframeIndices(base, &extra, NULL);
    if (extra.empty()) {
        int deleted = DeleteMarkedImages();
        if (g_last_delete_removed_palettes > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d marked sprite%s and %d now-unused palette%s.",
                     deleted, deleted == 1 ? "" : "s",
                     g_last_delete_removed_palettes,
                     g_last_delete_removed_palettes == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Deleted %d marked sprite%s.", deleted, deleted == 1 ? "" : "s");
        }
        g_restore_msg_timer = 4.0f;
        return;
    }

    g_pending_delete_parent_name[0] = '\0';
    g_pending_delete_base_indices = base;
    g_pending_delete_subframe_indices = extra;
    g_show_delete_images_confirm = true;
}


// Extracted from imgui_overlay.cpp: swap_adjacent_img
static void swap_adjacent_img(IMG *before_a, IMG *a, IMG *b)
{
    a->nxt_p = b->nxt_p;
    b->nxt_p = a;
    if (before_a) before_a->nxt_p = b;
    else g_doc->img_p = b;
}


// Extracted from imgui_overlay.cpp: MoveImageUp
void MoveImageUp(void)

{
    if (g_doc->ilselected <= 0) return;
    doc_undo_push();   /* reorders the image list — structural */

    IMG *before_prev = NULL;
    IMG *prev = (IMG *)g_doc->img_p;
    for (int i = 0; prev && i < g_doc->ilselected - 1; i++) {
        before_prev = prev;
        prev = (IMG *)prev->nxt_p;
    }
    if (!prev || !prev->nxt_p) return;
    swap_adjacent_img(before_prev, prev, (IMG *)prev->nxt_p);
    g_doc->ilselected--;
    g_img_tex_idx = -2;
}


// Extracted from imgui_overlay.cpp: MoveImageDown
void MoveImageDown(void)

{
    if (g_doc->ilselected < 0) return;
    doc_undo_push();   /* reorders the image list — structural */

    IMG *before_curr = NULL;
    IMG *curr = (IMG *)g_doc->img_p;
    for (int i = 0; curr && i < g_doc->ilselected; i++) {
        before_curr = curr;
        curr = (IMG *)curr->nxt_p;
    }
    if (!curr || !curr->nxt_p) return;
    swap_adjacent_img(before_curr, curr, (IMG *)curr->nxt_p);
    g_doc->ilselected++;
    g_img_tex_idx = -2;
}


// Extracted from imgui_overlay.cpp: TogglePointTable
void TogglePointTable(void)

{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;
    doc_undo_push();   /* pttbl alloc/free not captured by metadata undo */
    if (img->pttbl_p) {
        free(img->pttbl_p);
        img->pttbl_p = NULL;
    } else {
        AddPointTable(g_doc->ilselected);  /* cdecl-safe wrapper around img_pttbladd */
    }
}


// Extracted from imgui_overlay.cpp: ClearExtraData
void ClearExtraData(void)

{
    if (!g_doc->img_p) return;
    doc_undo_push();   /* clears anipoints + pttbl contents across all images */
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        clear_secondary_anipoint(p);
        if (p->pttbl_p) {
            /* PTTBL is 40 bytes per wmpstruc.inc: 8 dw header + 5 PTBOX
               (4 b each) + 1 PTCBOX (4 b). */
            memset(p->pttbl_p, 0, 40);
        }
    }
    g_img_tex_idx = -2;
}


// Extracted from imgui_overlay.cpp: ClearAll
void ClearAll(void)
{
    /* Delete all images (delete index 0 repeatedly, mirroring img_del(0) loop) */
    while (g_doc->img_p) {
        IMG *cur = (IMG *)g_doc->img_p;
        g_doc->img_p = cur->nxt_p;
        FreeImg(cur);
    }
    g_doc->imgcnt = 0;
    g_doc->ilselected = -1;

    /* Delete all palettes */
    while (g_doc->pal_p) {
        PAL *cur = (PAL *)g_doc->pal_p;
        g_doc->pal_p = cur->nxt_p;
        FreePal(cur);
    }
    g_doc->palcnt = 0;
    g_doc->plselected = -1;

    /* Free sequence/script memory */
    if (g_doc->scrseqmem_p) {
        free(g_doc->scrseqmem_p);
        g_doc->scrseqmem_p = NULL;
        g_doc->scrseqbytes  = 0;
    }
    if (g_doc->damtbl_p) {
        free(g_doc->damtbl_p);
        g_doc->damtbl_p = NULL;
        g_doc->damtblbytes = 0;
    }

    g_doc->seqcnt = 0;
    g_doc->scrcnt = 0;
    g_doc->damcnt = 0;
    g_doc->ilpalloaded = -1;

    /* Reset second image list */
    if (g_doc->img2_p) {
        while (g_doc->img2_p) {
            IMG *cur = (IMG *)g_doc->img2_p;
            g_doc->img2_p = cur->nxt_p;
            FreeImg(cur);
        }
    }
    g_doc->img2cnt     = 0;
    g_doc->il2selected = -1;
    g_doc->il1stprt    = 0;
    g_doc->il21stprt   = 0;

    g_img_tex_idx = -2;
    g_palette_nav = false;
    reset_palette_adjust_sliders();
    g_palette_baseline_nc = 0;
    InvalidatePaletteSync();
}


// Extracted from imgui_overlay.cpp: SwitchImageList
void SwitchImageList(void)

{
    void *tmp_p = g_doc->img_p;  g_doc->img_p = g_doc->img2_p;  g_doc->img2_p = tmp_p;
    unsigned int tmp_cnt = g_doc->imgcnt;  g_doc->imgcnt = g_doc->img2cnt;  g_doc->img2cnt = tmp_cnt;
    int tmp_sel = g_doc->ilselected;  g_doc->ilselected = g_doc->il2selected;  g_doc->il2selected = tmp_sel;
    unsigned int tmp_prt = g_doc->il1stprt;  g_doc->il1stprt = g_doc->il21stprt;  g_doc->il21stprt = tmp_prt;
    g_img_tex_idx = -2;
}



/* =========================================================
   Sprite Transformations Menu Items
   ========================================================= */

void DrawSpriteTransformMenuItems(void)
{
    if (ImGui::MenuItem("Rotate 90 Clockwise"))
        TransformSelectedSprite(SpriteTransformOp::Rotate90CW);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Rotates pixels and hitbox; preserves existing anim points.");
    if (ImGui::MenuItem("Rotate 90 Counterclockwise"))
        TransformSelectedSprite(SpriteTransformOp::Rotate90CCW);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Rotates pixels and hitbox; preserves existing anim points.");
    if (ImGui::MenuItem("Rotate 180"))
        TransformSelectedSprite(SpriteTransformOp::Rotate180);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Rotates pixels, anipoints, and hitbox together.");
    ImGui::Separator();
    if (ImGui::MenuItem("Flip Horizontal / Mirror"))
        TransformSelectedSprite(SpriteTransformOp::FlipHorizontal);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Mirrors pixels, anipoints, and hitbox together.");
    if (ImGui::MenuItem("Flip Vertical"))
        TransformSelectedSprite(SpriteTransformOp::FlipVertical);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Flips pixels, anipoints, and hitbox together.");
}

/* ---- Image texture renderer ---- */
void rebuild_img_texture(IMG *img)
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
        SDL_SetTextureScaleMode(g_img_texture, SDL_ScaleModeNearest);
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

    /* Composite an attached overlay layer directly into the texture for the
       canvas preview (non-destructive — base data_p is untouched). */
    SpriteLayer *L = img_layer(img);
    if (L && L->visible) {
        const unsigned char *lp = layer_pixels(L);
        for (int ly = 0; ly < L->h; ly++) {
            int dy = L->y + ly;
            if (dy < 0 || dy >= h) continue;
            const unsigned char *lrow = lp + (size_t)ly * L->stride;
            for (int lx = 0; lx < L->w; lx++) {
                int dx = L->x + lx;
                if (dx < 0 || dx >= w) continue;
                unsigned char ci = lrow[lx];
                if (ci == 0) continue;
                SDL_Color c = g_palette[ci];
                dst[dy * (pitch / 4) + dx] =
                    (0xFFu << 24) | ((Uint32)c.r << 16) | ((Uint32)c.g << 8) | c.b;
            }
        }
    }
    SDL_UnlockTexture(g_img_texture);
}

/* ---- LOAD2 drift texture ---- */
void update_drift_texture(IMG *img)
{
    int baseline_w = img ? (img->baseline_w ? (int)img->baseline_w : (int)img->w) : 0;
    int baseline_h = img ? (img->baseline_h ? (int)img->baseline_h : (int)img->h) : 0;
    if (!img || !img->data_p || !img->baseline_p || img->w == 0 || img->h == 0 ||
        baseline_w != (int)img->w || baseline_h != (int)img->h) {
        if (g_load2_drift_tex) { SDL_DestroyTexture(g_load2_drift_tex); g_load2_drift_tex = NULL; }
        g_load2_drift_tex_w = g_load2_drift_tex_h = 0;
        return;
    }
    int w = img->w, h = img->h;
    int stride = (w + 3) & ~3;

    if (!g_load2_drift_tex || g_load2_drift_tex_w != w || g_load2_drift_tex_h != h) {
        if (g_load2_drift_tex) SDL_DestroyTexture(g_load2_drift_tex);
        g_load2_drift_tex = SDL_CreateTexture(g_imgui_renderer,
            SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        SDL_SetTextureBlendMode(g_load2_drift_tex, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_load2_drift_tex, SDL_ScaleModeNearest);
        g_load2_drift_tex_w = w;
        g_load2_drift_tex_h = h;
    }
    void *pixels; int pitch;
    if (SDL_LockTexture(g_load2_drift_tex, NULL, &pixels, &pitch) != 0) return;

    const unsigned char *cur  = (const unsigned char *)img->data_p;
    const unsigned char *base = (const unsigned char *)img->baseline_p;
    Uint32 *dst = (Uint32 *)pixels;

    for (int y = 0; y < h; y++) {
        int bl = 0, bt = 0, cl = 0, ct = 0;
        const unsigned char *brow = base + y * stride;
        const unsigned char *crow = cur  + y * stride;
        while (bl < w && brow[bl] == 0) bl++;
        if (bl < w) { int x = w - 1; while (x >= bl && brow[x] == 0) { bt++; x--; } }
        while (cl < w && crow[cl] == 0) cl++;
        if (cl < w) { int x = w - 1; while (x >= cl && crow[x] == 0) { ct++; x--; } }
        bool row_drifts = (bl != cl) || (bt != ct);

        for (int x = 0; x < w; x++) {
            unsigned char ci = cur[y * stride + x];
            SDL_Color c = g_palette[ci];
            Uint32 r = c.r, g = c.g, b = c.b;
            Uint32 a = (ci == 0) ? 0x00u : 0xFFu;

            if (row_drifts) {
                if (a == 0) { r = 200; g = 40; b = 40; a = 90; }
                else { r = (r + 510) / 3; g = g / 3; b = b / 3; }
            }
            dst[y * (pitch / 4) + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SDL_UnlockTexture(g_load2_drift_tex);
}

/* ---- Document tab bar ---- */
static void RequestCloseDocumentTab(int idx)
{
    Document *doc = document_get(idx);
    if (!doc) return;
    if (doc->dirty) {
        ActivateDocumentTab(idx);
        g_pending_action = PendingAction::CloseTab;
        g_pending_tab_index = idx;
        g_show_unsaved_confirm = true;
        return;
    }
    bool closing_active = (idx == document_active_index());
    document_close_tab(idx);
    ResetPerDocumentUiState(false);
    if (closing_active) g_doc_tab_select_request = document_active_index();
}

float DrawDocumentTabBar(float y, float sw)
{
    const float tab_h = ImGui::GetFrameHeight();
    ImGui::SetNextWindowPos(ImVec2(0, y));
    ImGui::SetNextWindowSize(ImVec2(sw, tab_h));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 2));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
    ImGui::Begin("##document_tabs", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
        ImGuiWindowFlags_NoBackground);

    int activate_idx = -1;
    int close_idx = -1;
    bool new_tab = false;
    int active = document_active_index();

    /* Only force ImGui's selected tab when the active document actually changed
       (open / close / new / World View sync), not every frame. Re-asserting
       SetSelected continuously re-scrolls the bar to the active tab, which
       fought the left/right scroll arrows whenever more tabs were open than
       fit across the bar. */
    static int s_last_synced_active = -1;
    bool force_select = (active != s_last_synced_active) ||
                        (g_doc_tab_select_request >= 0);

    ImGuiTabBar *tab_bar_ptr = NULL;
    std::vector<ImGuiID> doc_tab_ids;

    ImGuiTabBarFlags tab_flags = ImGuiTabBarFlags_FittingPolicyScroll |
                                 ImGuiTabBarFlags_Reorderable;
    if (ImGui::BeginTabBar("##img_document_tabs", tab_flags)) {
        tab_bar_ptr = ImGui::GetCurrentTabBar();
        int n = document_tab_count();
        doc_tab_ids.assign((size_t)n, 0);
        for (int i = 0; i < n; i++) {
            Document *doc = document_get(i);
            if (!doc) continue;

            const char *base = doc->fname_s[0] ? doc->fname_s : "Untitled";
            char label[96];
            snprintf(label, sizeof(label), "%s%s##doc_tab_%d",
                     doc->dirty ? "* " : "", base, i);

            bool open = true;
            bool want_select = force_select &&
                               (i == active || i == g_doc_tab_select_request);
            ImGuiTabItemFlags item_flags = want_select ? ImGuiTabItemFlags_SetSelected
                                                       : ImGuiTabItemFlags_None;
            bool visible = ImGui::BeginTabItem(label, &open, item_flags);
            /* Record the ID ImGui assigned this tab so a drag-reorder can be
               mapped back to the document index after EndTabBar. */
            if (tab_bar_ptr && tab_bar_ptr->LastTabItemIdx >= 0 &&
                tab_bar_ptr->LastTabItemIdx < tab_bar_ptr->Tabs.Size)
                doc_tab_ids[(size_t)i] =
                    tab_bar_ptr->Tabs[tab_bar_ptr->LastTabItemIdx].ID;
            bool activated = ImGui::IsItemActivated();
            if (activated && i != active)
                activate_idx = i;
            if (visible)
                ImGui::EndTabItem();
            if (!open)
                close_idx = i;
        }

        if (g_world_state.enabled) {
            auto tab_toggle = [](const char *label, bool *value) {
                bool was_on = *value;
                if (was_on) {
                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_Text));
                    ImGui::PushStyleColor(ImGuiCol_Tab, ImGui::GetStyleColorVec4(ImGuiCol_TabSelected));
                    ImGui::PushStyleColor(ImGuiCol_TabHovered, ImGui::GetStyleColorVec4(ImGuiCol_TabHovered));
                }
                if (ImGui::TabItemButton(label, ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
                    *value = !*value;
                if (was_on) ImGui::PopStyleColor(3);
            };
            tab_toggle(g_world_state.onion ? "Onion: On" : "Onion", &g_world_state.onion);
            tab_toggle(g_world_state.show_borders ? "Borders: On" : "Borders", &g_world_state.show_borders);
            tab_toggle(g_world_state.show_anipoint ? "Anipt: On" : "Anipt", &g_world_state.show_anipoint);
            bool marked_was_on = g_world_marked_state.marked_play;
            tab_toggle(g_world_marked_state.marked_play ? "Marked: On" : "Marked", &g_world_marked_state.marked_play);
            if (marked_was_on != g_world_marked_state.marked_play) {
                WorldMarkedRestart(g_world_marked_state);
            }
            tab_toggle(g_world_marked_state.mirror_active ? "Mirror 1: On" : "Mirror 1", &g_world_marked_state.mirror_active);
            tab_toggle(g_world_marked_state.mirror_other ? "Mirror 2: On" : "Mirror 2", &g_world_marked_state.mirror_other);
        }

        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            new_tab = true;
        ImGui::EndTabBar();
    }
    g_doc_tab_select_request = -1;

    ImGui::End();
    ImGui::PopStyleVar(2);

    /* If the user dragged a tab, ImGui has reordered tab_bar->Tabs by now.
       Map that order back to document indices and apply it to the backing
       store so the new order persists. A reorder rewrites the indices, so the
       click-derived activate/close indices from this frame no longer apply. */
    bool reordered = false;
    if (tab_bar_ptr && (int)doc_tab_ids.size() == document_tab_count()) {
        std::vector<int> new_order;
        new_order.reserve(doc_tab_ids.size());
        for (int t = 0; t < tab_bar_ptr->Tabs.Size; t++) {
            ImGuiID id = tab_bar_ptr->Tabs[t].ID;
            for (int i = 0; i < (int)doc_tab_ids.size(); i++) {
                if (doc_tab_ids[(size_t)i] == id) {
                    new_order.push_back(i);
                    break;
                }
            }
        }
        if ((int)new_order.size() == document_tab_count()) {
            for (int i = 0; i < (int)new_order.size(); i++) {
                if (new_order[i] != i) { reordered = true; break; }
            }
            if (reordered) {
                document_reorder(new_order.data(), (int)new_order.size());
                /* Tab IDs encode the index, so the active document's tab gets a
                   new ID at its new slot; re-assert its selection next frame so
                   the highlight follows it instead of snapping to tab 0. */
                g_doc_tab_select_request = document_active_index();
            }
        }
    }

    if (!reordered) {
        if (activate_idx >= 0)
            ActivateDocumentTab(activate_idx);
        if (close_idx >= 0)
            RequestCloseDocumentTab(close_idx);
    }
    if (new_tab) {
        document_new_tab();
        g_doc_tab_select_request = document_active_index();
        ResetPerDocumentUiState(false);
    }

    s_last_synced_active = document_active_index();
    return tab_h;
}

/* ---- Image management helpers ---- */
void NormalizeImageDeleteIndices(std::vector<int> *indices)
{
    if (!indices) return;
    indices->erase(std::remove_if(indices->begin(), indices->end(),
        [](int idx) { return idx < 0 || (unsigned int)idx >= g_doc->imgcnt; }),
        indices->end());
    std::sort(indices->begin(), indices->end());
    indices->erase(std::unique(indices->begin(), indices->end()), indices->end());
}

void SetIDFromSecondList(void)
{
    mark_dirty();
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;

    if (!img->pttbl_p) {
        AddPointTable(g_doc->ilselected);
        img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
        if (!img || !img->pttbl_p) return;
    }

    /* PTTBL.ID is at struct offset 14 (dw aligned, pack-2) */
    unsigned char *pttbl = (unsigned char *)img->pttbl_p;
    unsigned short new_id = (unsigned short)(g_doc->il2selected + 1);
    pttbl[14] = (unsigned char)(new_id & 0xFF);
    pttbl[15] = (unsigned char)(new_id >> 8);
}

static bool ImageNameExists(const char *name)
{
    if (!name || !*name) return false;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p) {
        char existing[16];
        strncpy(existing, p->n_s, 15);
        existing[15] = '\0';
        if (strcmp(existing, name) == 0) return true;
    }
    return false;
}

void MakeDerivedImageName(const char *base, const char *suffix, char out[16])
{
    char root[16];
    if (base && *base) {
        strncpy(root, base, 15);
        root[15] = '\0';
    } else {
        strncpy(root, "SPRITE", sizeof(root));
    }

    for (int attempt = 0; attempt < 1000; attempt++) {
        char tail[8];
        if (attempt == 0) snprintf(tail, sizeof(tail), "%s", suffix ? suffix : "");
        else              snprintf(tail, sizeof(tail), "%s%d", suffix ? suffix : "", attempt);

        size_t tail_len = strlen(tail);
        size_t budget = (tail_len < 15) ? (15 - tail_len) : 0;
        snprintf(out, 16, "%.*s%s", (int)budget, root, tail);
        if (!ImageNameExists(out)) return;
    }

    snprintf(out, 16, "%.15s", root);
}

void DuplicateImage(void)
{
    IMG *src = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!src) return;

    doc_undo_push();   /* adds a new image — undo must remove it */

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
    if (src->opaltbl_p) {
        dst->opaltbl_p = malloc(16);
        if (!dst->opaltbl_p) goto err;
        memcpy(dst->opaltbl_p, src->opaltbl_p, 16);
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

    strncpy(dst->src_filename, src->src_filename, sizeof(dst->src_filename) - 1);
    dst->src_filename[sizeof(dst->src_filename) - 1] = '\0';
    MakeDerivedImageName(src->n_s, "DUP", dst->n_s);

    /* Select the new image and open rename */
    g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
    g_zoom_reset = true;
    OpenRenameImage();
    return;

err:
    /* Rollback: delete the newly-allocated image */
    if (dst->data_p) free(dst->data_p);
    if (dst->pttbl_p) free(dst->pttbl_p);
    if (dst->opaltbl_p) free(dst->opaltbl_p);
    {
        IMG *prev = NULL;
        IMG *cur = (IMG *)g_doc->img_p;
        while (cur && cur != dst) { prev = cur; cur = (IMG *)cur->nxt_p; }
        if (cur == dst) {
            if (prev) prev->nxt_p = cur->nxt_p;
            else g_doc->img_p = cur->nxt_p;
            g_doc->imgcnt--;
        }
    }
    free(dst);
}

void AddNewBlankImage(int w, int h)
{
    if (w < 1)    w = 1;
    if (w > 1024) w = 1024;
    if (h < 1)    h = 1;
    if (h > 1024) h = 1024;
    mark_dirty();
    IMG *img = AllocImg();
    if (!img) return;

    img->w        = (unsigned short)w;
    img->h        = (unsigned short)h;
    img->flags    = 0;
    img->anix     = 0;
    img->aniy     = 0;
    clear_secondary_anipoint(img);
    img->opals    = (unsigned short)-1;
    img->pttbl_p  = NULL;
    img->palnum   = (g_doc->plselected >= 0) ? (unsigned short)g_doc->plselected : 0;

    unsigned int stride = ((unsigned int)img->w + 3) & ~3;
    unsigned int sz = stride * img->h;
    img->data_p = PoolAlloc(sz);
    if (img->data_p) memset(img->data_p, 0, sz);
    img->baseline_p = PoolAlloc(sz);
    if (img->baseline_p) {
        memset(img->baseline_p, 0, sz);
        img->baseline_w = img->w;
        img->baseline_h = img->h;
    }

    static int next_id = 1;
    snprintf(img->n_s, sizeof(img->n_s), "NEW%d", next_id++);

    if (g_doc->imgcnt > 0) g_doc->ilselected = (int)g_doc->imgcnt - 1;
    g_img_tex_idx = -2;
}

/* ---- Anipoint propagation across tabs ---- */
static std::string regex_escape_main(const std::string &s)
{
    std::string out;
    out.reserve(s.size() * 2);
    for (char ch : s) {
        switch (ch) {
            case '\\': case '.': case '^': case '$': case '|':
            case '(': case ')': case '[': case ']': case '{':
            case '}': case '*': case '+': case '?':
                out.push_back('\\');
                break;
            default:
                break;
        }
        out.push_back(ch);
    }
    return out;
}

static std::string sprite_family_regex_pattern_main(const std::string &name)
{
    if (name.size() > 2)
        return std::string("^..") + regex_escape_main(name.substr(2)) + "$";
    return std::string("^") + regex_escape_main(name) + "$";
}

int PushAnipointsToMatchingOpenTabs(const IMG *src, int *matched_count, int *doc_count, std::string *pattern_out)
{
    if (matched_count) *matched_count = 0;
    if (doc_count) *doc_count = 0;
    if (pattern_out) pattern_out->clear();
    if (!src) return 0;

    std::string src_name = img_name_string(src);
    if (src_name.empty()) return 0;

    std::string pattern = sprite_family_regex_pattern_main(src_name);
    if (pattern_out) *pattern_out = pattern;

    std::regex name_re(pattern, std::regex_constants::ECMAScript | std::regex_constants::icase);
    int changed = 0;
    int matched = 0;
    int docs_changed = 0;

    for (int tab = 0; tab < document_tab_count(); tab++) {
        Document *doc = document_get(tab);
        if (!doc) continue;

        bool doc_touched = false;
        for (IMG *img = (IMG *)doc->img_p; img; img = (IMG *)img->nxt_p) {
            std::string name = img_name_string(img);
            if (name.empty() || !std::regex_match(name, name_re)) continue;

            matched++;
            if (img == src) continue;
            if (img->anix  == src->anix  && img->aniy  == src->aniy &&
                img->anix2 == src->anix2 && img->aniy2 == src->aniy2 &&
                img->aniz2 == src->aniz2)
                continue;

            img->anix  = src->anix;
            img->aniy  = src->aniy;
            img->anix2 = src->anix2;
            img->aniy2 = src->aniy2;
            img->aniz2 = src->aniz2;
            changed++;
            doc_touched = true;
        }

        if (doc_touched) {
            doc->dirty = true;
            docs_changed++;
        }
    }

    if (matched_count) *matched_count = matched;
    if (doc_count) *doc_count = docs_changed;
    return changed;
}

