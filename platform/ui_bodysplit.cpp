/*************************************************************
 * platform/ui_bodysplit.cpp
 * "Split Body Parts" dialog.
 *
 * The heuristic in body_split.cpp proposes five rects; this file is the part
 * that lets a human disagree with it. The preview draws the sprite with the
 * proposed boxes on top, each draggable and resizable, and only the Split
 * button commits — creating one child IMG per enabled part, with the parent's
 * anipoint rebased into each child exactly the way Auto-Split does it.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "ui_bodysplit.h"
#include "ui_internal.h"
#include "ui_timeline.h"
#include "body_split.h"
#include "img_format.h"
#include "img_io.h"
#include "img_util.h"
#include "anipoint.h"
#include "world_render.h"
#include "compat.h"

#include <string.h>
#include <vector>
#include <string>

/* ---- Dialog state -------------------------------------------------- */

static bool          g_show_body_split = false;
static BodySplitPlan g_plan;
static int           g_plan_img_idx = -1;   /* sprite the plan was built from */
static bool          g_plan_trim = true;    /* trim each part to its pixels */
static bool          g_plan_clear_parent = false;
static bool          g_plan_unmark_parent = true;
static float         g_preview_zoom = 0.0f; /* 0 = fit */

/* Live drag: which box, and which of its edges/corners. */
enum BodyDragMode {
    BodyDrag_None = 0,
    BodyDrag_Move,
    BodyDrag_TopLeft,
    BodyDrag_TopRight,
    BodyDrag_BottomLeft,
    BodyDrag_BottomRight
};
static int  g_drag_part = -1;
static int  g_drag_mode = BodyDrag_None;
static BodyPartRect g_drag_start_rect;
static ImVec2 g_drag_start_mouse;
static int  g_active_part = BodyPart_Torso;

static const ImU32 kPartColors[BodyPart_Count] = {
    IM_COL32(120, 190, 255, 255),   /* Head  */
    IM_COL32(255, 190,  90, 255),   /* Arm L */
    IM_COL32(120, 230, 150, 255),   /* Torso */
    IM_COL32(230, 130, 230, 255),   /* Arm R */
    IM_COL32(240, 120, 120, 255)    /* Legs  */
};

/* ---- Helpers -------------------------------------------------------- */

static IMG *BodySplitTargetImage(int *out_idx)
{
    int idx = (g_doc && g_doc->ilselected >= 0) ? g_doc->ilselected : -1;
    IMG *img = (idx >= 0) ? get_img(idx) : NULL;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) {
        if (out_idx) *out_idx = -1;
        return NULL;
    }
    if (out_idx) *out_idx = idx;
    return img;
}

bool SelectedImageCanBodySplit(void)
{
    return BodySplitTargetImage(NULL) != NULL;
}

static void BodySplitRebuildPlan(void)
{
    int idx = -1;
    IMG *img = BodySplitTargetImage(&idx);
    g_plan = BodySplitPlan();
    g_plan_img_idx = idx;
    if (!img) return;

    int stride = ((int)img->w + 3) & ~3;
    BuildBodySplitPlan((const unsigned char *)img->data_p,
                       (int)img->w, (int)img->h, stride, &g_plan);
}

void OpenBodySplitDialog(void)
{
    BodySplitRebuildPlan();
    g_preview_zoom = 0.0f;
    g_drag_part = -1;
    g_drag_mode = BodyDrag_None;
    g_active_part = BodyPart_Torso;
    g_show_body_split = true;
}

static void ClampRectToImage(BodyPartRect &r, int w, int h)
{
    if (r.w < 1) r.w = 1;
    if (r.h < 1) r.h = 1;
    if (r.x < 0) r.x = 0;
    if (r.y < 0) r.y = 0;
    if (r.x > w - 1) r.x = w - 1;
    if (r.y > h - 1) r.y = h - 1;
    if (r.x + r.w > w) r.w = w - r.x;
    if (r.y + r.h > h) r.h = h - r.y;
}

/* Cut one part out of the parent into a new IMG at the end of the list. The
   child's anipoint is the parent's, rebased to the child's origin, so the
   pieces still line up when drawn together in World View. */
static bool CreateBodyPartImage(IMG *parent, const BodyPartRect &rect,
                                const char *child_name)
{
    if (!parent || !parent->data_p || rect.w <= 0 || rect.h <= 0) return false;

    IMG *child = AllocImg();
    if (!child) return false;

    auto unlink_child = [&]() {
        IMG *prev = NULL;
        IMG *cur = (IMG *)g_doc->img_p;
        while (cur && cur != child) { prev = cur; cur = (IMG *)cur->nxt_p; }
        if (cur == child) {
            if (prev) prev->nxt_p = cur->nxt_p;
            else      g_doc->img_p = cur->nxt_p;
            if (g_doc->imgcnt > 0) g_doc->imgcnt--;
        }
        FreeImg(child);
    };

    child->w = (unsigned short)rect.w;
    child->h = (unsigned short)rect.h;
    child->palnum = parent->palnum;
    child->flags = 0;
    child->opals = parent->opals;
    if (parent->opaltbl_p) {
        child->opaltbl_p = malloc(16);
        if (!child->opaltbl_p) { unlink_child(); return false; }
        memcpy(child->opaltbl_p, parent->opaltbl_p, 16);
    }
    child->anix = signed_to_img_word((int)(short)parent->anix - rect.x);
    child->aniy = signed_to_img_word((int)(short)parent->aniy - rect.y);
    clear_secondary_anipoint(child);

    int src_stride = ((int)parent->w + 3) & ~3;
    int dst_stride = (rect.w + 3) & ~3;
    child->data_p = PoolAlloc((size_t)dst_stride * (size_t)rect.h);
    if (!child->data_p) { unlink_child(); return false; }

    const unsigned char *src = (const unsigned char *)parent->data_p;
    unsigned char *dst = (unsigned char *)child->data_p;
    for (int y = 0; y < rect.h; y++) {
        memcpy(dst + (size_t)y * dst_stride,
               src + (size_t)(rect.y + y) * src_stride + rect.x,
               (size_t)rect.w);
    }

    strncpy(child->src_filename, parent->src_filename,
            sizeof(child->src_filename) - 1);
    child->src_filename[sizeof(child->src_filename) - 1] = '\0';
    strncpy(child->n_s, child_name && child_name[0] ? child_name : "BODYPART",
            sizeof(child->n_s) - 1);
    child->n_s[sizeof(child->n_s) - 1] = '\0';
    return true;
}

/* Blank every part rect out of the parent, so the pieces plus the leftover
   parent still add up to the original artwork instead of duplicating it. */
static void ClearPartsFromParent(IMG *parent, const BodySplitPlan &plan)
{
    if (!parent || !parent->data_p) return;
    int stride = ((int)parent->w + 3) & ~3;
    unsigned char *px = (unsigned char *)parent->data_p;
    for (int i = 0; i < BodyPart_Count; i++) {
        const BodyPartRect &r = plan.parts[i];
        if (!r.enabled) continue;
        for (int y = r.y; y < r.y + r.h && y < (int)parent->h; y++) {
            if (y < 0) continue;
            for (int x = r.x; x < r.x + r.w && x < (int)parent->w; x++) {
                if (x < 0) continue;
                px[(size_t)y * stride + x] = 0;
            }
        }
    }
}

static int ApplyBodySplit(void)
{
    int idx = -1;
    IMG *img = BodySplitTargetImage(&idx);
    if (!img) return 0;

    BodySplitPlan plan = g_plan;
    if (g_plan_trim) {
        int stride = ((int)img->w + 3) & ~3;
        TrimBodySplitPlan((const unsigned char *)img->data_p,
                          (int)img->w, (int)img->h, stride, &plan);
    }

    int enabled = 0;
    for (int i = 0; i < BodyPart_Count; i++)
        if (plan.parts[i].enabled) enabled++;
    if (enabled == 0) return 0;

    if (!doc_undo_push()) return 0;

    int created = 0;
    int first_created_idx = -1;
    for (int i = 0; i < BodyPart_Count; i++) {
        const BodyPartRect &r = plan.parts[i];
        if (!r.enabled) continue;
        char child_name[24];
        BodyPartChildName(img->n_s, i, child_name, sizeof(child_name));
        if (CreateBodyPartImage(img, r, child_name)) {
            created++;
            if (first_created_idx < 0) first_created_idx = (int)g_doc->imgcnt - 1;
        }
    }

    if (created > 0) {
        if (g_plan_clear_parent) ClearPartsFromParent(img, plan);
        if (g_plan_unmark_parent) img->flags &= ~1;
        InvalidateThumb(idx);
        if (first_created_idx >= 0) g_doc->ilselected = first_created_idx;
        g_img_tex_idx = -2;
        g_zoom_reset = true;
        mark_dirty();
    }
    return created;
}

/* ---- Interactive preview -------------------------------------------- */

static void DrawBodySplitPreview(IMG *img)
{
    const float kHandle = 5.0f;     /* corner grab radius, screen px */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float pane_w = avail.x;
    float pane_h = avail.y;
    if (pane_w < 64.0f) pane_w = 64.0f;
    if (pane_h < 64.0f) pane_h = 64.0f;

    float scale = g_preview_zoom;
    if (scale <= 0.0f) {
        scale = pane_w / (float)img->w;
        float fit_y = pane_h / (float)img->h;
        if (fit_y < scale) scale = fit_y;
        if (scale > 8.0f) scale = 8.0f;
        if (scale < 0.25f) scale = 0.25f;
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    origin.x += (pane_w - img->w * scale) * 0.5f;
    if (origin.x < ImGui::GetCursorScreenPos().x)
        origin.x = ImGui::GetCursorScreenPos().x;

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 img_max(origin.x + img->w * scale, origin.y + img->h * scale);

    /* Checkerboard so transparent regions read as transparent. */
    const float kCheck = 8.0f;
    dl->PushClipRect(origin, img_max, true);
    for (float y = origin.y; y < img_max.y; y += kCheck) {
        for (float x = origin.x; x < img_max.x; x += kCheck) {
            bool odd = (((int)((x - origin.x) / kCheck) +
                         (int)((y - origin.y) / kCheck)) & 1) != 0;
            dl->AddRectFilled(ImVec2(x, y),
                              ImVec2(x + kCheck, y + kCheck),
                              odd ? IM_COL32(58, 58, 62, 255)
                                  : IM_COL32(42, 42, 46, 255));
        }
    }
    dl->PopClipRect();

    SDL_Texture *tex = BuildWorldSpriteTexture(g_doc, img, 255);
    if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, origin, img_max);
    dl->AddRect(origin, img_max, IM_COL32(90, 90, 96, 255));

    auto to_screen = [&](int ix, int iy) {
        return ImVec2(origin.x + ix * scale, origin.y + iy * scale);
    };
    auto to_image = [&](ImVec2 p, float *ix, float *iy) {
        *ix = (p.x - origin.x) / scale;
        *iy = (p.y - origin.y) / scale;
    };

    /* An invisible button over the image owns the mouse for dragging. */
    ImGui::SetCursorScreenPos(origin);
    ImGui::InvisibleButton("##body_split_canvas",
                           ImVec2(img->w * scale, img->h * scale),
                           ImGuiButtonFlags_MouseButtonLeft);
    bool canvas_hovered = ImGui::IsItemHovered();
    ImVec2 mouse = ImGui::GetIO().MousePos;

    /* Hit-test topmost-first so a small box on top of a big one still wins. */
    int hover_part = -1;
    int hover_mode = BodyDrag_None;
    if (canvas_hovered && g_drag_mode == BodyDrag_None) {
        for (int i = BodyPart_Count - 1; i >= 0 && hover_part < 0; i--) {
            const BodyPartRect &r = g_plan.parts[i];
            if (!r.enabled) continue;
            ImVec2 mn = to_screen(r.x, r.y);
            ImVec2 mx = to_screen(r.x + r.w, r.y + r.h);
            struct Corner { ImVec2 p; int mode; } corners[4] = {
                { mn,                     BodyDrag_TopLeft },
                { ImVec2(mx.x, mn.y),     BodyDrag_TopRight },
                { ImVec2(mn.x, mx.y),     BodyDrag_BottomLeft },
                { mx,                     BodyDrag_BottomRight }
            };
            for (int c = 0; c < 4; c++) {
                float dx = mouse.x - corners[c].p.x;
                float dy = mouse.y - corners[c].p.y;
                if (dx * dx + dy * dy <= kHandle * kHandle * 4.0f) {
                    hover_part = i;
                    hover_mode = corners[c].mode;
                    break;
                }
            }
            if (hover_part < 0 &&
                mouse.x >= mn.x && mouse.x <= mx.x &&
                mouse.y >= mn.y && mouse.y <= mx.y) {
                hover_part = i;
                hover_mode = BodyDrag_Move;
            }
        }
    }

    if (hover_part >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_drag_part = hover_part;
        g_drag_mode = hover_mode;
        g_drag_start_rect = g_plan.parts[hover_part];
        g_drag_start_mouse = mouse;
        g_active_part = hover_part;
    }

    if (g_drag_mode != BodyDrag_None && g_drag_part >= 0) {
        if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            float sx, sy, cx, cy;
            to_image(g_drag_start_mouse, &sx, &sy);
            to_image(mouse, &cx, &cy);
            int dx = (int)(cx - sx + (cx >= sx ? 0.5f : -0.5f));
            int dy = (int)(cy - sy + (cy >= sy ? 0.5f : -0.5f));

            BodyPartRect r = g_drag_start_rect;
            int right = r.x + r.w;
            int bottom = r.y + r.h;
            switch (g_drag_mode) {
                case BodyDrag_Move:
                    r.x += dx; r.y += dy;
                    break;
                case BodyDrag_TopLeft:
                    r.x += dx; r.y += dy;
                    r.w = right - r.x; r.h = bottom - r.y;
                    break;
                case BodyDrag_TopRight:
                    r.y += dy;
                    r.w = (right + dx) - r.x; r.h = bottom - r.y;
                    break;
                case BodyDrag_BottomLeft:
                    r.x += dx;
                    r.w = right - r.x; r.h = (bottom + dy) - r.y;
                    break;
                case BodyDrag_BottomRight:
                    r.w = (right + dx) - r.x; r.h = (bottom + dy) - r.y;
                    break;
                default: break;
            }
            /* A corner dragged past its opposite edge flips the rect about
               that edge instead of collapsing to a sliver: a negative width
               already describes the span [x+w, x). */
            if (r.w < 1) { r.x += r.w; r.w = -r.w; if (r.w < 1) r.w = 1; }
            if (r.h < 1) { r.y += r.h; r.h = -r.h; if (r.h < 1) r.h = 1; }
            ClampRectToImage(r, (int)img->w, (int)img->h);
            g_plan.parts[g_drag_part] = r;
        } else {
            g_drag_part = -1;
            g_drag_mode = BodyDrag_None;
        }
    }

    /* Boxes on top of the sprite. */
    for (int i = 0; i < BodyPart_Count; i++) {
        const BodyPartRect &r = g_plan.parts[i];
        if (!r.enabled) continue;
        ImVec2 mn = to_screen(r.x, r.y);
        ImVec2 mx = to_screen(r.x + r.w, r.y + r.h);
        ImU32 col = kPartColors[i];
        bool active = (i == g_active_part);
        dl->AddRectFilled(mn, mx, (col & 0x00FFFFFF) | (active ? 0x33000000 : 0x1A000000));
        dl->AddRect(mn, mx, col, 0.0f, 0, active ? 2.0f : 1.0f);
        for (int c = 0; c < 4; c++) {
            ImVec2 p((c & 1) ? mx.x : mn.x, (c & 2) ? mx.y : mn.y);
            dl->AddRectFilled(ImVec2(p.x - kHandle * 0.5f, p.y - kHandle * 0.5f),
                              ImVec2(p.x + kHandle * 0.5f, p.y + kHandle * 0.5f),
                              col);
        }
        dl->AddText(ImVec2(mn.x + 3.0f, mn.y + 2.0f), col, BodyPartName(i));
    }

    /* The neck/waist rows the heuristic chose, for context while editing. */
    if (g_plan.valid) {
        ImU32 guide = IM_COL32(255, 255, 255, 60);
        ImVec2 n0 = to_screen(0, g_plan.neck_y);
        ImVec2 w0 = to_screen(0, g_plan.waist_y);
        dl->AddLine(n0, ImVec2(img_max.x, n0.y), guide);
        dl->AddLine(w0, ImVec2(img_max.x, w0.y), guide);
    }

    if (canvas_hovered) {
        float mx_img, my_img;
        to_image(mouse, &mx_img, &my_img);
        ImGui::SetTooltip("%d, %d", (int)mx_img, (int)my_img);
    }
}

/* ---- Dialog ---------------------------------------------------------- */

void DrawBodySplitDialog(void)
{
    if (g_show_body_split) ImGui::OpenPopup("Split Body Parts");
    ImGui::SetNextWindowSize(ImVec2(760, 520), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal("Split Body Parts", &g_show_body_split,
                                ImGuiWindowFlags_NoSavedSettings))
        return;

    int idx = -1;
    IMG *img = BodySplitTargetImage(&idx);
    if (!img) {
        ImGui::TextUnformatted("Select a sprite with pixels first.");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_body_split = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }
    /* Following the selection keeps the dialog honest if the user clicks a
       different sprite in the list while it is open. */
    if (idx != g_plan_img_idx) BodySplitRebuildPlan();

    ImGui::Text("%.15s  (%dx%d)", img->n_s, (int)img->w, (int)img->h);
    ImGui::SameLine();
    if (g_plan.used_proportional_fallback)
        ImGui::TextColored(ImVec4(1.0f, 0.66f, 0.30f, 1.0f),
                           "  no clear neck/waist found - bands are proportional guesses");
    else
        ImGui::TextDisabled("  neck row %d, waist row %d", g_plan.neck_y, g_plan.waist_y);

    ImGui::Separator();

    const float kSidePane = 268.0f;
    ImGui::BeginChild("##body_split_preview",
                      ImVec2(-kSidePane, -ImGui::GetFrameHeightWithSpacing() * 1.6f),
                      true);
    DrawBodySplitPreview(img);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##body_split_side",
                      ImVec2(kSidePane - 8.0f, -ImGui::GetFrameHeightWithSpacing() * 1.6f),
                      true);

    int stride = ((int)img->w + 3) & ~3;
    int enabled_parts = 0;
    for (int i = 0; i < BodyPart_Count; i++) {
        ImGui::PushID(i);
        BodyPartRect &r = g_plan.parts[i];

        ImVec4 col = ImGui::ColorConvertU32ToFloat4(kPartColors[i]);
        ImGui::PushStyleColor(ImGuiCol_Text, col);
        if (ImGui::Checkbox("##on", &r.enabled)) {
            if (r.enabled && (r.w <= 0 || r.h <= 0)) {
                r.x = g_plan.content_x;
                r.y = g_plan.content_y;
                r.w = g_plan.content_w > 0 ? g_plan.content_w : (int)img->w;
                r.h = g_plan.content_h > 0 ? g_plan.content_h : (int)img->h;
            }
            g_active_part = i;
        }
        ImGui::SameLine();
        if (ImGui::Selectable(BodyPartName(i), g_active_part == i, 0,
                              ImVec2(60.0f, 0.0f)))
            g_active_part = i;
        ImGui::PopStyleColor();

        if (r.enabled) {
            ImGui::SameLine();
            char name[24];
            BodyPartChildName(img->n_s, i, name, sizeof(name));
            ImGui::TextDisabled("%s", name);

            ImGui::SetNextItemWidth(-1.0f);
            int xywh[4] = { r.x, r.y, r.w, r.h };
            if (ImGui::DragInt4("##rect", xywh, 0.4f)) {
                r.x = xywh[0]; r.y = xywh[1];
                r.w = xywh[2]; r.h = xywh[3];
                ClampRectToImage(r, (int)img->w, (int)img->h);
                g_active_part = i;
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("X, Y, W, H");

            int px = BodyRectOpaquePixels((const unsigned char *)img->data_p,
                                          (int)img->w, (int)img->h, stride, r);
            if (px == 0)
                ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f), "empty - will be skipped");
            else
                ImGui::TextDisabled("%d px", px);
            enabled_parts++;
        }
        ImGui::Separator();
        ImGui::PopID();
    }

    ImGui::Checkbox("Trim parts to their pixels", &g_plan_trim);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Shrink each box onto its own artwork before cutting.\n"
                          "Anipoints are rebased accordingly, so the pieces still line up.");
    ImGui::Checkbox("Erase parts from the original", &g_plan_clear_parent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Blank the cut regions in the source sprite so the pieces\n"
                          "plus what's left add up to the original, instead of duplicating it.");
    ImGui::Checkbox("Unmark the original", &g_plan_unmark_parent);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Clears the source sprite's mark, matching Auto-Split,\n"
                          "so exports pick up the pieces rather than the whole body.");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##zoom", &g_preview_zoom, 0.0f, 8.0f,
                       g_preview_zoom <= 0.0f ? "Zoom: fit" : "Zoom: %.1fx");

    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("Re-detect", ImVec2(110, 0))) BodySplitRebuildPlan();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Throw away edits and run the detector again.");
    ImGui::SameLine();
    if (ImGui::Button("Trim Now", ImVec2(110, 0))) {
        TrimBodySplitPlan((const unsigned char *)img->data_p,
                          (int)img->w, (int)img->h, stride, &g_plan);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Apply the trim to the boxes right now so you can see the result.");

    ImGui::SameLine();
    ImGui::BeginDisabled(enabled_parts == 0);
    if (ImGui::Button("Split", ImVec2(120, 0))) {
        int created = ApplyBodySplit();
        if (created > 0) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Split %.15s into %d body part%s.", img->n_s, created,
                     created == 1 ? "" : "s");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Nothing to split - every part box was empty.");
        }
        g_restore_msg_timer = 4.0f;
        if (created > 0) {
            g_show_body_split = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_body_split = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%d part%s selected", enabled_parts,
                        enabled_parts == 1 ? "" : "s");

    ImGui::EndPopup();
}
