/*************************************************************
 * platform/ui_tools.cpp
 * Left toolbar UI and per-tool property widgets.
 *
 * Part of the Phase C overlay split.
 *************************************************************/
#include "ui_tools.h"
#include "ui_internal.h"
#include "ui_canvas.h"
#include "ui_timeline.h"
#include "img_format.h"
#include "document.h"
#include "shim_vid.h"
#include <imgui.h>
#include <cstdio>

void DrawLeftToolbar(float work_y, float work_h)
{
    /* ===== LEFT TOOLBAR ===== */
    ImGui::SetNextWindowPos(ImVec2(0, work_y + 5.0f));
    ImGui::SetNextWindowSize(ImVec2(76.0f, work_h - 112.0f - 108.0f - 5.0f)); /* TOOLBAR_W = 76, PALETTE_H = 112, TIMELINE_H = 108 */
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8.0f, 6.0f));
    ImGui::Begin("##toolbar", NULL,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
    {
        ImVec2 btn(76.0f - 12, 76.0f - 12);
        #define TB_LABEL(icon, txt) (g_icon_font_loaded ? (icon) : (txt))

#define TOOL_ACTIVE_COL(r,g,b) ImVec4((r), (g), (b), 1.0f)
        btn = ImVec2(28.0f, 28.0f);
        float left_x = 8.0f;
        float right_x = left_x + btn.x + 4.0f;
        float tool_y = ImGui::GetCursorPosY();
        float action_y = tool_y;
        ImVec4 tool_idle(0.25f, 0.25f, 0.25f, 1.0f);
        ImVec4 action_idle(0.10f, 0.24f, 0.48f, 1.0f);
        ImVec4 action_active(0.12f, 0.42f, 0.78f, 1.0f);

        auto place_tool = [&]() {
            ImGui::SetCursorPos(ImVec2(right_x, tool_y));
            tool_y += btn.y + 4.0f;
        };
        auto place_action = [&]() {
            ImGui::SetCursorPos(ImVec2(left_x, action_y));
            action_y += btn.y + 4.0f;
        };
        auto toggle_tool = [&](ActiveTool tool) {
            if (g_active_tool == tool) g_active_tool = ActiveTool::None;
            else g_active_tool = tool;
            if (tool == ActiveTool::Lasso) g_lasso_points.clear();
            if (g_active_tool == ActiveTool::None &&
                (tool == ActiveTool::Marquee || tool == ActiveTool::MagicWand ||
                 tool == ActiveTool::Lasso || tool == ActiveTool::BackgroundEraser ||
                 tool == ActiveTool::CloneStamp || tool == ActiveTool::SmartRemap ||
                 tool == ActiveTool::Eyedropper))
                g_grid_sel.active = false;
        };
        auto tool_button = [&](ActiveTool tool, const char *icon, const char *txt,
                               ImVec4 active_col, const char *tip) {
            place_tool();
            ImGui::PushStyleColor(ImGuiCol_Button,
                g_active_tool == tool ? active_col : tool_idle);
            if (ImGui::Button(TB_LABEL(icon, txt), btn)) toggle_tool(tool);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };
        auto action_button = [&](const char *icon, const char *txt,
                                 const char *tip, bool disabled,
                                 bool active, auto on_click) {
            place_action();
            ImGui::PushStyleColor(ImGuiCol_Button, active ? action_active : action_idle);
            if (disabled) ImGui::BeginDisabled();
            if (ImGui::Button(TB_LABEL(icon, txt), btn)) on_click();
            if (disabled) ImGui::EndDisabled();
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);
        };

        tool_button(ActiveTool::Marquee, ICON_MARQUEE, ICON_MARQUEE_TXT,
                    TOOL_ACTIVE_COL(0.2f,0.4f,0.7f), "Marquee Select Tool (R)");
        tool_button(ActiveTool::MagicWand, "\xEF\x8C\x9F", "Wd",
                    TOOL_ACTIVE_COL(0.5f,0.2f,0.7f), "Magic Wand Tool (W)\nCtrl-click adds to the current selection");
        tool_button(ActiveTool::Pencil, "\xEE\x8F\x89", "Pn",
                    TOOL_ACTIVE_COL(0.7f,0.6f,0.2f), "Pencil (P)\n[ / ] to shrink / grow brush");
        tool_button(ActiveTool::PaintBucket, "\xEE\x8E\xAE", "Bk",
                    TOOL_ACTIVE_COL(0.7f,0.45f,0.15f), "Paint Bucket (G)");
        tool_button(ActiveTool::VariantPaint, "\xEE\x90\x8A", "Vt",
                    TOOL_ACTIVE_COL(0.2f,0.6f,0.7f), "Variant Paint (V)");
        tool_button(ActiveTool::BackgroundEraser, "\xEE\x9B\x90", "Er",
                    TOOL_ACTIVE_COL(0.7f,0.2f,0.2f), "Smart Eraser");
        tool_button(ActiveTool::CloneStamp, "\xEE\x8E\xBB", "Cl",
                    TOOL_ACTIVE_COL(0.2f,0.6f,0.3f), "Clone Stamp");
        tool_button(ActiveTool::SmartRemap, "\xEE\x90\x8A", "Rm",
                    TOOL_ACTIVE_COL(0.8f,0.4f,0.1f), "Smart Palette Remapper");
        tool_button(ActiveTool::Lasso, "\xEE\xAC\x83", "Ls",
                    TOOL_ACTIVE_COL(0.3f,0.5f,0.8f), "Lasso Selection Tool (L)");
        tool_button(ActiveTool::Eyedropper, "\xEF\x8D\x91", "Ey",
                    TOOL_ACTIVE_COL(0.6f,0.7f,0.2f), "Eyedropper Tool (I)");

        action_button(ICON_MARK, ICON_MARK_TXT, "Mark/Unmark (Space)", false, false, [&]() {
            IMG *img = get_img(g_doc->ilselected); if (img) img->flags ^= 1;
        });
        action_button(ICON_MARK_ALL, ICON_MARK_ALL_TXT, "Set All Marks (M)", false, false, [&]() {
            IMG *p=(IMG*)g_doc->img_p; while(p){p->flags|=1; p=(IMG*)p->nxt_p;}
        });
        action_button(ICON_CLEAR, ICON_CLEAR_TXT, "Clear All Marks (m)", false, false, [&]() {
            IMG *p=(IMG*)g_doc->img_p; while(p){p->flags&=~1; p=(IMG*)p->nxt_p;}
        });
        action_button(ICON_POINTS, ICON_POINTS_TXT, "Toggle Anim Points", false, g_show_points, [&]() {
            g_show_points = !g_show_points;
        });
        action_button(ICON_HITBOX, ICON_HITBOX_TXT, "Toggle Hitbox", false, g_show_hitbox, [&]() {
            g_show_hitbox = !g_show_hitbox;
        });
        action_button(ICON_RESIZE, ICON_RESIZE_TXT, "Resize Sprite", g_doc->ilselected < 0, false, [&]() {
            OpenResizeSpriteDialog();
        });
        action_button(ICON_ZOOM_IN, ICON_ZOOM_IN_TXT, "Zoom In (Ctrl+=)", g_doc->ilselected < 0, false, [&]() {
            QueueZoomStep(1);
        });
        action_button(ICON_ZOOM_OUT, ICON_ZOOM_OUT_TXT, "Zoom Out (Ctrl+-)", g_doc->ilselected < 0, false, [&]() {
            QueueZoomStep(-1);
        });
        action_button(ICON_UNDO, ICON_UNDO_TXT, "Undo (Ctrl+Z)", !CanUndo(), false, [&]() {
            DoUndo();
        });
        action_button(ICON_REDO, ICON_REDO_TXT, "Redo (Ctrl+Y)", !CanRedo(), false, [&]() {
            DoRedo();
        });

        ImGui::SetCursorPosY((tool_y > action_y ? tool_y : action_y) + 2.0f);
        ImGui::SetCursorPosX(left_x);
        ImVec2 line_start = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(line_start, ImVec2(line_start.x + 60.0f, line_start.y), ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(60.0f, 4.0f));

        ImGui::SetCursorPosX(left_x);
        ImGui::SetNextItemWidth(60.0f);
        if (g_active_tool == ActiveTool::Pencil) {
            ImGui::SliderInt("##pencil_brush", &g_pencil_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pencil brush radius");
        } else if (g_active_tool == ActiveTool::VariantPaint) {
            ImGui::SliderInt("##variant_brush", &g_variant_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Variant brush radius");
        } else if (g_active_tool == ActiveTool::PaintBucket) {
            ImGui::SliderInt("##bucket_tol", &g_bucket_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Paint bucket tolerance");
            ImGui::SetCursorPosX(left_x);
            ImGui::Checkbox("C##bucket", &g_bucket_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous fill");
        } else if (g_active_tool == ActiveTool::MagicWand) {
            ImGui::SliderInt("##wand_tol", &g_wand_tolerance, 0, 64);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Magic Wand strength");
            ImGui::SetCursorPosX(left_x);
            ImGui::Checkbox("C##wand", &g_wand_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous selection");
        } else if (g_active_tool == ActiveTool::CloneStamp) {
            ImGui::SliderInt("##clone_brush", &g_clone_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clone brush radius");
        } else if (g_active_tool == ActiveTool::BackgroundEraser) {
            ImGui::SliderInt("##eraser_tol", &g_eraser_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart eraser tolerance");
            ImGui::SetCursorPosX(left_x);
            ImGui::Checkbox("C##eraser", &g_eraser_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous erase");
            ImGui::SameLine();
            ImGui::Checkbox("D##eraser", &g_eraser_defringe);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Defringe edge pixels");
        } else if (g_active_tool == ActiveTool::SmartRemap) {
            ImGui::SliderInt("##remap_tol", &g_remap_tolerance, 0, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smart remap tolerance");
        }

        ImGui::Spacing();
        {
            SDL_Color &c = g_palette[g_sel_color];
            ImU32 col = IM_COL32(c.r, c.g, c.b, 255);
            ImGui::SetCursorPosX(left_x);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            float sw_sz = 60.0f; // Flush with the double columns of buttons (28 + 4 + 28)
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(cp, ImVec2(cp.x + sw_sz, cp.y + 24), col);
            dl->AddRect(cp, ImVec2(cp.x + sw_sz, cp.y + 24), IM_COL32(255,255,255,80));
            ImGui::Dummy(ImVec2(sw_sz, 24));
        }
        char col_label[8];
        snprintf(col_label, sizeof(col_label), "#%d", g_sel_color);
        ImGui::SetCursorPosX(left_x);
        if (ImGui::SmallButton(col_label)) {
            static int last_col = 1;
            if (g_sel_color == 0) g_sel_color = last_col;
            else { last_col = g_sel_color; g_sel_color = 0; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Active color index (right-click sprite to pick)");

        #undef TB_LABEL
        #undef TOOL_ACTIVE_COL
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
