/*************************************************************
 * platform/ui_tools.cpp
 * Left toolbar UI and per-tool property widgets.
 *
 * Part of the Phase C overlay split.
 *************************************************************/
#include "ui_tools.h"
#include "ui_internal.h"
#include "ui_canvas.h"
#include "ui_palette.h"
#include "ui_timeline.h"
#include "img_format.h"
#include "document.h"
#include "shim_vid.h"
#include <imgui.h>
#include <cstdio>

void DrawLeftToolbar(float work_y, float work_h, float bottom_reserved_h)
{
    /* ===== LEFT TOOLBAR =====
     * The toolbar column owns the full left edge from just below the tab bar
     * down to the top of any full-width bottom dock (timeline/palette), or to
     * the screen bottom when that dock is hidden in World View. Sizing it dynamically
     * keeps the toolbar background filling that column instead of cutting off
     * partway down. */
    ImGui::SetNextWindowPos(ImVec2(0, work_y + 5.0f));
    ImGui::SetNextWindowSize(ImVec2(76.0f, work_h - bottom_reserved_h - 5.0f)); /* TOOLBAR_W = 76 */
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
                 tool == ActiveTool::Blur || tool == ActiveTool::Smudge ||
                 tool == ActiveTool::ContentErase ||
                 tool == ActiveTool::Eyedropper))
                g_grid_sel.active = false;
        };
        auto tool_button = [&](ActiveTool tool, const char *icon, const char *txt,
                               ImVec4 active_col, const char *tip) {
            place_tool();
            /* ActiveTool::None paints exactly like the pencil (same canvas
               branch, only the brush radius differs), so leaving every button
               dark in the default state told users nothing about what a click
               would do. Light the pencil for None as well. */
            bool tool_lit = (g_active_tool == tool) ||
                            (tool == ActiveTool::Pencil &&
                             g_active_tool == ActiveTool::None);
            ImGui::PushStyleColor(ImGuiCol_Button,
                tool_lit ? active_col : tool_idle);
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
        tool_button(ActiveTool::MagicWand, ICON_T_WAND, "Wd",
                    TOOL_ACTIVE_COL(0.5f,0.2f,0.7f), "Magic Wand Tool (W)\nCtrl-click adds to the current selection");
        tool_button(ActiveTool::Pencil, ICON_T_PENCIL, "Pn",
                    TOOL_ACTIVE_COL(0.7f,0.6f,0.2f), "Pencil (P)\n[ / ] to shrink / grow brush");
        tool_button(ActiveTool::PaintBucket, ICON_T_BUCKET, "Bk",
                    TOOL_ACTIVE_COL(0.7f,0.45f,0.15f), "Paint Bucket (G)");
        tool_button(ActiveTool::VariantPaint, ICON_T_VARIANT, "Vt",
                    TOOL_ACTIVE_COL(0.2f,0.6f,0.7f), "Variant Paint (V)");
        tool_button(ActiveTool::BackgroundEraser, ICON_T_ERASER, "Er",
                    TOOL_ACTIVE_COL(0.7f,0.2f,0.2f), "Smart Eraser (E)");
        tool_button(ActiveTool::CloneStamp, ICON_T_CLONE, "Cl",
                    TOOL_ACTIVE_COL(0.2f,0.6f,0.3f), "Clone Stamp (C)\nAlt+click anchors the source");
        tool_button(ActiveTool::SmartRemap, ICON_T_REMAP, "Rm",
                    TOOL_ACTIVE_COL(0.8f,0.4f,0.1f), "Smart Palette Remapper");
        tool_button(ActiveTool::Blur, ICON_T_BLUR, "Bl",
                    TOOL_ACTIVE_COL(0.35f,0.55f,0.75f),
                    "Blur\nSoftens under the brush by averaging in RGB,\nthen remapping to the nearest palette index.");
        tool_button(ActiveTool::Smudge, ICON_T_SMUDGE, "Sm",
                    TOOL_ACTIVE_COL(0.6f,0.45f,0.75f),
                    "Smudge\nDrags colour along the stroke, like pulling wet paint.");
        tool_button(ActiveTool::ContentErase, ICON_T_HEAL, "Ce",
                    TOOL_ACTIVE_COL(0.75f,0.35f,0.45f),
                    "Content-Aware Eraser\nRemoves the brushed area and heals it from the\nsurrounding pixels instead of punching a hole.");
        tool_button(ActiveTool::Lasso, ICON_T_LASSO, "Ls",
                    TOOL_ACTIVE_COL(0.3f,0.5f,0.8f), "Lasso Selection Tool (L)");
        tool_button(ActiveTool::Eyedropper, ICON_T_DROPPER, "Ey",
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
        action_button(ICON_HITBOX, ICON_HITBOX_TXT, "Toggle Strike Box", false, g_show_hitbox, [&]() {
            g_show_hitbox = !g_show_hitbox;
        });
        action_button(ICON_FLIP_PREVIEW, ICON_FLIP_PREVIEW_TXT,
                      "Mirror Preview (cycles Off / Ghost / Flipped Only)\n"
                      "Draws where the sprite lands when the engine h-flips it.\n"
                      "The art mirrors about the anipoint, so an anchor beside\n"
                      "the art throws the flipped placement twice that far off.",
                      false, g_flip_preview != FlipPreviewMode::Off, [&]() {
            g_flip_preview = (g_flip_preview == FlipPreviewMode::Off)   ? FlipPreviewMode::Ghost
                           : (g_flip_preview == FlipPreviewMode::Ghost) ? FlipPreviewMode::Only
                                                                        : FlipPreviewMode::Off;
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
        /* Fit belongs beside the zoom pair it completes — reaching for the
           menu (or remembering Ctrl+0) to undo a zoom is the common case. */
        action_button(ICON_ZOOM_FIT, ICON_ZOOM_FIT_TXT, "Fit Sprite to Canvas (Ctrl+0)",
                      g_doc->ilselected < 0, false, [&]() {
            QueueZoomFit();
        });
        /* Onion skin sits with the other overlay toggles (points, hitbox,
           mirror) rather than only on the timeline strip, since it is a view
           state you flip while looking at the canvas. */
        action_button(ICON_ONION, ICON_ONION_TXT,
                      "Onion Skin\nGhosts the previous and next timeline frame at 25% alpha\n"
                      "while scrubbing or playing, and draws the previous frame's\n"
                      "anipoint as a dim crosshair for registration.",
                      false, g_timeline_onion, [&]() {
            g_timeline_onion = !g_timeline_onion;
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
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Paint bucket color tolerance.\n0 fills only the exact color clicked;\nhigher values also take nearby shades.\nTransparent and opaque pixels never mix.");
            ImGui::SetCursorPosX(left_x);
            ImGui::Checkbox("C##bucket", &g_bucket_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous fill");
        } else if (g_active_tool == ActiveTool::MagicWand) {
            ImGui::SliderInt("##wand_tol", &g_wand_tolerance, 0, 64);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Magic Wand color tolerance.\n0 selects only the exact color clicked;\nhigher values pull in nearby shades.\nTransparent and opaque pixels never mix.");
            ImGui::SetCursorPosX(left_x);
            ImGui::Checkbox("C##wand", &g_wand_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous selection");
        } else if (g_active_tool == ActiveTool::CloneStamp) {
            ImGui::SliderInt("##clone_brush", &g_clone_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Clone brush radius");
        } else if (g_active_tool == ActiveTool::BackgroundEraser) {
            ImGui::SliderInt("##eraser_tol", &g_eraser_tolerance, 0, 16);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Smart eraser color tolerance.\n0 erases only the exact color clicked;\nhigher values also take nearby shades.");
            ImGui::SetCursorPosX(left_x);
            ImGui::Checkbox("C##eraser", &g_eraser_contiguous);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Contiguous erase");
            ImGui::SameLine();
            ImGui::Checkbox("D##eraser", &g_eraser_defringe);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Defringe edge pixels");
        } else if (g_active_tool == ActiveTool::SmartRemap) {
            ImGui::SliderInt("##remap_tol", &g_remap_tolerance, 0, 16);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Smart remap color tolerance.\n0 repaints only the exact color clicked;\nhigher values also take nearby shades.\nTransparent and opaque pixels never mix.");
        } else if (g_active_tool == ActiveTool::Blur) {
            ImGui::SliderInt("##blur_brush", &g_blur_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Blur brush radius");
            ImGui::SetCursorPosX(left_x);
            ImGui::SliderInt("##blur_str", &g_blur_strength, 1, 100);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Blur strength. Lower values build up over\nrepeated passes instead of flattening at once.");
        } else if (g_active_tool == ActiveTool::Smudge) {
            ImGui::SliderInt("##smudge_brush", &g_smudge_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Smudge brush radius");
            ImGui::SetCursorPosX(left_x);
            ImGui::SliderInt("##smudge_str", &g_smudge_strength, 1, 100);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How much colour the stroke carries along with it.");
        } else if (g_active_tool == ActiveTool::ContentErase) {
            ImGui::SliderInt("##ce_brush", &g_content_erase_brush, 1, 16);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Content-aware eraser radius");
            ImGui::SetCursorPosX(left_x);
            ImGui::SliderInt("##ce_passes", &g_content_erase_passes, 1, 32);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("How far colour is carried inward from the rim.\nMore passes fill bigger holes; 1 only heals the edge.");
        }

        ImGui::Spacing();
        const float sw_sz = 60.0f; // Flush with the double columns of buttons (28 + 4 + 28)
        float picker_anchor_y = 0.0f;
        static bool s_picker_was_open = false;
        {
            SDL_Color &c = g_palette[g_sel_color];
            ImU32 col = IM_COL32(c.r, c.g, c.b, 255);
            ImGui::SetCursorPosX(left_x);
            ImVec2 cp = ImGui::GetCursorScreenPos();
            picker_anchor_y = cp.y;
            /* The square is the handle for the color wheel: clicking it pops the
               picker out to the right of the toolbar. */
            ImGui::InvisibleButton("##active_color_square", ImVec2(sw_sz, 24));
            bool hovered = ImGui::IsItemHovered();
            /* ImGui dismisses the popup during NewFrame when the click lands
               outside it, so by now a "close" click looks identical to an
               "open" one. Remembering last frame's state makes the square a
               real toggle instead of an unconditional reopen. */
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !s_picker_was_open)
                ImGui::OpenPopup("##swatch_picker");
            ImDrawList *dl = ImGui::GetWindowDrawList();
            dl->AddRectFilled(cp, ImVec2(cp.x + sw_sz, cp.y + 24), col);
            dl->AddRect(cp, ImVec2(cp.x + sw_sz, cp.y + 24),
                        hovered ? IM_COL32(255, 255, 255, 220) : IM_COL32(255, 255, 255, 80),
                        0.0f, 0, hovered ? 2.0f : 1.0f);
            if (hovered) ImGui::SetTooltip(
                "Color #%d - click for the color wheel.\n"
                "Right-click a sprite pixel to pick its color instead.", g_sel_color);
        }
        {
            /* Anchor the pop-out beside the square, nudged up if it would run
               off the bottom of the display. */
            const float picker_w = 210.0f;
            const float popup_h_est = 400.0f;
            float py = picker_anchor_y;
            float disp_h = ImGui::GetIO().DisplaySize.y;
            if (py + popup_h_est > disp_h - 8.0f) py = disp_h - popup_h_est - 8.0f;
            if (py < 8.0f) py = 8.0f;
            ImGui::SetNextWindowPos(ImVec2(76.0f + 6.0f, py));
            if (ImGui::BeginPopup("##swatch_picker")) {
                DrawActiveSwatchPickerBody(picker_w);
                ImGui::EndPopup();
            }
            s_picker_was_open = ImGui::IsPopupOpen("##swatch_picker");
        }
        char col_label[8];
        snprintf(col_label, sizeof(col_label), "#%d", g_sel_color);
        ImGui::SetCursorPosX(left_x);
        if (ImGui::SmallButton(col_label)) {
            static int last_col = 1;
            if (g_sel_color == 0) g_sel_color = last_col;
            else { last_col = g_sel_color; g_sel_color = 0; }
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Active color index - click to toggle to/from transparent #0");

        /* Palette-wide hue/saturation/lightness, directly under the square. */
        ImGui::SetCursorPosX(left_x);
        ImVec2 hsl_line = ImGui::GetCursorScreenPos();
        ImGui::GetWindowDrawList()->AddLine(hsl_line, ImVec2(hsl_line.x + sw_sz, hsl_line.y),
                                            ImGui::GetColorU32(ImGuiCol_Separator));
        ImGui::Dummy(ImVec2(sw_sz, 4.0f));
        ImGui::SetCursorPosX(left_x);
        DrawActiveSwatchHslControls(sw_sz);

        #undef TB_LABEL
        #undef TOOL_ACTIVE_COL
    }
    ImGui::End();
    ImGui::PopStyleVar();
}
