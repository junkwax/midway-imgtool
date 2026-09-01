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
#include "anipoint_level.h"
#include "img_util.h"
#include "sprite_resize_ops.h"
#include "img_io.h"
#include "load2_verify.h"
#include "lod_parser.h"
#include "mk2_hitbox.h"
#include "mk2_fatality.h"
#include "ui_bodysplit.h"
#include "ui_autochop.h"
#include "ui_stamp_erase.h"
#include "dma_pack.h"
#include "compat.h"

/* The Sprite tab's Hitbox section, defined lower down beside the other
   panel bodies. It edits the MKSTK.ASM strike record for the selected
   frame's move -- see DrawStrikeBoxPanel. */
static void DrawStrikeBoxPanel(void);

extern "C" { extern struct SDL_Color g_palette[256]; }
extern int g_img_tex_idx;

/* Bulk trim over the marked set, shared by the three menus that offer it so
   they can't drift apart on undo handling or on what the toast reports.
   CropMarkedImagesToContent leaves the snapshot to its caller, since the crop
   rewrites w/h and data_p that the metadata-only undo_push cannot restore. */
static void BulkTrimMarkedToContent(void)
{
    int marked = CountMarkedImages();
    if (marked <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Nothing to trim: no sprites are marked (Space marks one, M marks all).");
        g_restore_msg_timer = 4.0f;
        return;
    }
    doc_undo_push();
    int n = CropMarkedImagesToContent();
    if (n > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Trimmed %d of %d marked sprite%s to their opaque bounds.",
                 n, marked, marked == 1 ? "" : "s");
        g_zoom_reset = true;
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No change: all %d marked sprite%s already fit their opaque "
                 "bounds (or are fully transparent).",
                 marked, marked == 1 ? " does" : "s do");
    }
    g_restore_msg_timer = 4.0f;
}

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

/* ---- Anipoint-outside-bounding-box badge ----
 * Flags a frame whose anipoint falls outside its own 0..w-1 / 0..h-1 box.
 *
 * Deliberately soft, and worth being honest about what it can and cannot do:
 * being outside the box does NOT by itself mean wrong, and it cannot separate
 * a good library from a bad one. MK1SKULL's pre-fix anipoints were off-box,
 * but so are MKDEATH.ASM's spine-rip props, which are correct — they ride
 * match_ani_points and carry their whole offset in the art anipoint on
 * purpose, exactly as that file's PLACEMENT comment prescribes. SPINERIP.IMG
 * badges all 18 of its frames for that reason.
 *
 * So this is a "look here first" mark, not a verdict. The flip preview is
 * what actually answers whether the anchor is wrong. Users working in a
 * library that is legitimately off-box everywhere can silence it from View.
 *
 * Call immediately after the row's Selectable so GetItemRect* describes it.
 * `right_inset` clears any widget already occupying the row's right edge
 * (the subframe expand triangle needs ~22 px). */
static void DrawAnipointBoundsBadge(const IMG *img, float right_inset)
{
    if (!g_show_anipoint_warnings) return;
    if (!img || img->w == 0 || img->h == 0) return;
    AnipointBoundsReport b = anipoint_bounds_report((int)(short)img->anix,
                                                    (int)(short)img->aniy,
                                                    (int)img->w, (int)img->h);
    if (!b.x_outside && !b.y_outside) return;

    ImVec2 mn = ImGui::GetItemRectMin();
    ImVec2 mx = ImGui::GetItemRectMax();
    const char *mark = "!";
    ImVec2 sz = ImGui::CalcTextSize(mark);
    ImVec2 pos(mx.x - right_inset - sz.x, (mn.y + mx.y) * 0.5f - sz.y * 0.5f);
    if (pos.x < mn.x) return;
    ImGui::GetWindowDrawList()->AddText(pos, IM_COL32(255, 183, 77, 255), mark);

    if (ImGui::IsMouseHoveringRect(ImVec2(pos.x - 5.0f, mn.y),
                                   ImVec2(pos.x + sz.x + 3.0f, mx.y))) {
        ImGui::SetTooltip("Anipoint (%d,%d) is outside this sprite's own\n"
                          "0..%d / 0..%d box by %d / %d px, so the art mirrors\n"
                          "far from where it draws unflipped.\n\n"
                          "This is a place to look, not a verdict. A prop\n"
                          "positioned through match_ani_points carries its\n"
                          "whole offset in the anipoint on purpose and is\n"
                          "correct off-box. Turn on Mirror Preview to see\n"
                          "whether the flipped placement actually lands wrong.\n"
                          "Silence this mark under View > Anipoint Warnings.",
                          (int)(short)img->anix, (int)(short)img->aniy,
                          (int)img->w - 1, (int)img->h - 1,
                          b.x_slack, b.y_slack);
    }
}

/* ---- Signed centre-offset readout ----
 * c = anix - (sizex - 1)/2 is the single number that answers "is this
 * anchored on the art, or beside it". For an effect meant to sit *on* its
 * anchor, c is near 0. MK1FIRE1 (w=89, anix=-68) read -112, i.e. the anchor
 * sat 68 px outside its own left edge; the game papered over that with a
 * hand-tuned constant in the fatality sequence, which can never be right
 * because the value negates under h-flip.
 *
 * The out-of-box badge below is deliberately soft: plenty of legitimate art
 * anchors off the sprite. It was still the tell here — every MK1FIRE/MK1SKEL
 * frame tripped it. */
static void DrawAnipointCenterOffsetReadout(const IMG *img)
{
    if (!img) return;
    int ax = (int)(short)img->anix;
    int ay = (int)(short)img->aniy;
    float cx = anipoint_center_offset(ax, (int)img->w);
    float cy = anipoint_center_offset(ay, (int)img->h);

    AnipointBoundsReport bounds =
        anipoint_bounds_report(ax, ay, (int)img->w, (int)img->h);
    bool outside = bounds.x_outside || bounds.y_outside;

    if (outside) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.30f, 1.0f));
    LabeledValue("Ctr off:", "%+.1f, %+.1f", cx, cy);
    if (outside) ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Anipoint offset from the art's own centre: c = a - (size-1)/2.\n"
            "Near 0 means the anchor sits on the art. This value negates\n"
            "exactly under h-flip, so an offset tuned for one facing is\n"
            "wrong by 2*c for the other.\n\n"
            "Flipped anix = %d (%s)",
            anipoint_effective(ax, (int)img->w, true, g_mirror_convention),
            mirror_convention_label(g_mirror_convention));
    }
    if (outside) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.30f, 1.0f));
        ImGui::TextWrapped("Anipoint outside 0..%d / 0..%d by %d / %d px",
                           (int)img->w - 1, (int)img->h - 1,
                           bounds.x_slack, bounds.y_slack);
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Not always wrong — plenty of art anchors off the\n"
                              "sprite on purpose. Worth a look when the frame is\n"
                              "an effect meant to land on its anchor.");
    }
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

/* ---- Stance-referenced anipoint levelling ----
 * Frames added to a shipped library arrive with anix/aniy = 0,0, which anchors
 * them to their own top-left corner; played back they bob against the original
 * art because each frame's pixels sit at a different height inside its box.
 * The shipped STANCE frame knows where the ground is, so it supplies the line
 * everything else stands on. Effects on another palette have no feet and get
 * centred on their own pixels instead. See anipoint_level.h. */
static void LevelUnsetAnipointsFromStance(void)
{
    std::vector<IMG *> imgs;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p)
        imgs.push_back(p);

    AnipointLevelPlan plan = AnipointLevelBuildPlan(imgs);

    if (!plan.has_reference) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No STANCE frame here — nothing to measure a ground line from.");
        g_restore_msg_timer = 5.0f;
        return;
    }

    const char *ref_name = imgs[(size_t)plan.reference_index]->n_s;

    if (plan.entries.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Ground line %d from %.15s; nothing to level (%d already anchored).",
                 plan.ground_line, ref_name, plan.skipped_anchored);
        g_restore_msg_timer = 5.0f;
        return;
    }

    doc_undo_push();
    for (const AnipointLevelPlanEntry &e : plan.entries) {
        IMG *img = imgs[(size_t)e.index];
        img->anix = signed_to_img_word(e.new_anix);
        img->aniy = signed_to_img_word(e.new_aniy);
    }
    mark_dirty();
    /* Thumbnails and the canvas texture both draw the crosshair, so they go
       stale the moment an anchor moves. */
    ClearTimelineThumbCache();
    g_img_tex_idx = -2;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Ground line %d from %.15s: %d frame%s levelled (Y only — X is "
             "authored motion, set it by hand), %d effect%s centred, %d left "
             "alone as already anchored.",
             plan.ground_line, ref_name,
             plan.ground_count, plan.ground_count == 1 ? "" : "s",
             plan.center_count, plan.center_count == 1 ? "" : "s",
             plan.skipped_anchored);
    g_restore_msg_timer = 6.0f;
}


/* ---- Bulk numeric anipoint shift ----
 * Dragging crosshairs is right for authoring one frame and useless for "shift
 * these 22 records by +143" — which is exactly the edit the MK1SKULL fix
 * needed, and why it got scripted outside the tool. */
static bool g_open_anipoint_shift = false;
static AnipointShiftRequest g_anipoint_shift = {
    AnipointShiftScope_Marked, "", 0, 0, true, false
};

void OpenAnipointShiftDialog(void)
{
    /* Seed the pattern from the selected sprite's stem so "MK1FIRE*" is one
       keystroke away rather than something to retype. */
    IMG *img = (g_doc && g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (img && g_anipoint_shift.pattern[0] == '\0') {
        std::string name = trim_sprite_name(img_name_string(img));
        std::string stem;
        if (!strip_trailing_sequence_digits(name, &stem) || stem.empty())
            stem = name;
        snprintf(g_anipoint_shift.pattern, sizeof(g_anipoint_shift.pattern),
                 "%.30s*", stem.c_str());
    }
    g_open_anipoint_shift = true;
}

static void DrawAnipointShiftDialog(void)
{
    if (g_open_anipoint_shift) {
        ImGui::OpenPopup("Shift Anipoints");
        g_open_anipoint_shift = false;
    }
    if (!ImGui::BeginPopupModal("Shift Anipoints", NULL,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    const char *scopes[] = { "Marked frames", "Name pattern", "Selected frame", "All frames" };
    int scope_idx = (int)g_anipoint_shift.scope;
    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("Scope", &scope_idx, scopes, 4))
        g_anipoint_shift.scope = (AnipointShiftScope)scope_idx;

    ImGui::BeginDisabled(g_anipoint_shift.scope != AnipointShiftScope_Pattern);
    ImGui::SetNextItemWidth(200.0f);
    ImGui::InputText("Pattern", g_anipoint_shift.pattern,
                     sizeof(g_anipoint_shift.pattern));
    ImGui::EndDisabled();
    ImGui::TextDisabled("'*' matches any run, '?' one character. Case-insensitive.");

    ImGui::Spacing();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("dX", &g_anipoint_shift.dx);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputInt("dY", &g_anipoint_shift.dy);
    g_anipoint_shift.dx = clamp_int(g_anipoint_shift.dx, -4096, 4096);
    g_anipoint_shift.dy = clamp_int(g_anipoint_shift.dy, -4096, 4096);

    ImGui::Checkbox("Primary (X1/Y1)", &g_anipoint_shift.affect_primary);
    ImGui::SameLine(180.0f);
    ImGui::Checkbox("Secondary (X2/Y2)", &g_anipoint_shift.affect_secondary);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Only frames whose secondary point is actually in use\n"
                          "are touched; the -1 'unused' sentinel is left alone.");

    int matched = 0;
    int would_change = ShiftAnipointsInScope(g_anipoint_shift, false, &matched);
    ImGui::Spacing();
    ImGui::Text("%d frame%s in scope, %d would change.",
                matched, matched == 1 ? "" : "s", would_change);

    /* Show the shift's effect on the first frame in scope, since the whole
       point of the numeric path is that you can't see it on the canvas. */
    {
        int idx = 0;
        for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p, idx++) {
            bool in_scope = false;
            switch (g_anipoint_shift.scope) {
                case AnipointShiftScope_Marked:   in_scope = (p->flags & 1) != 0; break;
                case AnipointShiftScope_Selected: in_scope = (idx == g_doc->ilselected); break;
                case AnipointShiftScope_All:      in_scope = true; break;
                case AnipointShiftScope_Pattern: {
                    char nm[17];
                    memcpy(nm, p->n_s, 16);
                    nm[16] = '\0';
                    in_scope = sprite_name_matches_glob(nm, g_anipoint_shift.pattern);
                    break;
                }
            }
            if (!in_scope) continue;
            int ax = (int)(short)p->anix;
            int ay = (int)(short)p->aniy;
            ImGui::TextDisabled("First: %.15s  %d,%d -> %d,%d   ctr off %+.1f -> %+.1f",
                                p->n_s, ax, ay,
                                ax + g_anipoint_shift.dx, ay + g_anipoint_shift.dy,
                                anipoint_center_offset(ax, (int)p->w),
                                anipoint_center_offset(ax + g_anipoint_shift.dx, (int)p->w));
            break;
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(would_change <= 0);
    if (ImGui::Button("Shift", ImVec2(120, 0))) {
        int applied = ShiftAnipointsInScope(g_anipoint_shift, true, &matched);
        if (applied > 0) {
            mark_dirty();
            g_img_tex_idx = -2;
            ClearTimelineThumbCache();
        }
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Shifted %d anipoint%s by %+d,%+d.",
                 applied, applied == 1 ? "" : "s",
                 g_anipoint_shift.dx, g_anipoint_shift.dy);
        g_restore_msg_timer = 4.0f;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0)))
        ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

/* ---- Compare against an existing .TBL ----
 * MK2's src/*.TBL files are hand-maintained — no build step regenerates them
 * — so an imgtool re-export that changed anipoints would silently desync art
 * from table. mk2-main guards this in build.py Phase D; the check belongs on
 * this end too. */
static bool g_show_tbl_compare = false;
static bool g_tbl_compare_marked_only = true;
static std::string g_tbl_compare_path;
static std::string g_tbl_compare_error;
static std::vector<std::string> g_tbl_compare_warnings;
static std::vector<TblEntry> g_tbl_compare_table;
static std::vector<TblDiffRow> g_tbl_compare_rows;
static TblDiffSummary g_tbl_compare_summary = {0, 0, 0, 0};
static bool g_tbl_compare_hide_matches = true;

static void RecomputeTblCompare(void)
{
    std::vector<TblEntry> img_entries;
    BuildTblEntriesFromDoc(g_tbl_compare_marked_only, img_entries);
    DiffTblEntries(g_tbl_compare_table, img_entries,
                   g_tbl_compare_rows, &g_tbl_compare_summary);
}

void RunTblCompare(const char *path)
{
    g_tbl_compare_path = path ? path : "";
    g_tbl_compare_error.clear();
    g_tbl_compare_warnings.clear();
    g_tbl_compare_table.clear();
    g_tbl_compare_rows.clear();
    g_tbl_compare_summary = TblDiffSummary{0, 0, 0, 0};

    FILE *f = fopen(g_tbl_compare_path.c_str(), "rb");
    if (!f) {
        g_tbl_compare_error = "Could not open the file.";
        g_show_tbl_compare = true;
        return;
    }
    std::string text;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        text.append(buf, n);
    fclose(f);

    if (!ParseTblText(text, g_tbl_compare_table,
                      &g_tbl_compare_warnings, &g_tbl_compare_error)) {
        g_show_tbl_compare = true;
        return;
    }
    RecomputeTblCompare();
    g_show_tbl_compare = true;
}

static void DrawTblCompareDialog(void)
{
    if (g_show_tbl_compare) ImGui::OpenPopup("TBL Compare");
    ImGui::SetNextWindowSize(ImVec2(720, 520), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal("TBL Compare", &g_show_tbl_compare, 0)) return;

    ImGui::TextWrapped("%s", g_tbl_compare_path.c_str());
    ImGui::Separator();

    if (!g_tbl_compare_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           g_tbl_compare_error.c_str());
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_tbl_compare = false;
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    if (ImGui::Checkbox("Marked sprites only", &g_tbl_compare_marked_only))
        RecomputeTblCompare();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Match what Write TBL exports, which only emits marked\n"
                          "sprites. Uncheck to compare the whole library.");
    ImGui::SameLine();
    ImGui::Checkbox("Hide matching rows", &g_tbl_compare_hide_matches);

    ImGui::Text("%d parsed from TBL   |   %d match, %d differ, %d only in TBL, %d only in IMG",
                (int)g_tbl_compare_table.size(),
                g_tbl_compare_summary.matched,
                g_tbl_compare_summary.differing,
                g_tbl_compare_summary.only_in_tbl,
                g_tbl_compare_summary.only_in_img);

    for (size_t w = 0; w < g_tbl_compare_warnings.size(); w++)
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f), "%s",
                           g_tbl_compare_warnings[w].c_str());

    ImGui::Separator();
    ImGui::BeginChild("##tbl_diff_rows", ImVec2(0, -34), true);
    if (ImGui::BeginTable("##tbl_diff", 4,
                          ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Sprite", ImGuiTableColumnFlags_WidthFixed, 130.0f);
        ImGui::TableSetupColumn("Field", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("TBL");
        ImGui::TableSetupColumn("IMG");
        ImGui::TableHeadersRow();

        for (size_t r = 0; r < g_tbl_compare_rows.size(); r++) {
            const TblDiffRow &row = g_tbl_compare_rows[r];
            if (g_tbl_compare_hide_matches && row.kind == TblDiff_Match) continue;

            if (row.kind == TblDiff_OnlyInTbl || row.kind == TblDiff_OnlyInImg ||
                row.kind == TblDiff_Match) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::TableNextColumn();
                if (row.kind == TblDiff_Match) {
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "ok");
                } else {
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.35f, 1.0f),
                                       row.kind == TblDiff_OnlyInTbl ? "TBL only"
                                                                     : "IMG only");
                }
                ImGui::TableNextColumn();
                ImGui::TableNextColumn();
                continue;
            }

            for (size_t f = 0; f < row.fields.size(); f++) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (f == 0) ImGui::TextUnformatted(row.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.45f, 1.0f), "%s",
                                   row.fields[f].field.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.fields[f].tbl_value.c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.fields[f].img_value.c_str());
            }
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    if (ImGui::Button("Copy Report", ImVec2(120, 0))) {
        std::string report = FormatTblDiffReport(g_tbl_compare_path,
                                                 g_tbl_compare_rows,
                                                 g_tbl_compare_summary);
        ImGui::SetClipboardText(report.c_str());
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Copied TBL diff report.");
        g_restore_msg_timer = 3.0f;
    }
    ImGui::SameLine();
    if (ImGui::Button("Close", ImVec2(100, 0))) {
        g_show_tbl_compare = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

/* Directory of `doc` with any trailing separator removed, so "C:\data" and
   "C:\data\\" compare equal. */
static std::string DocDirKey(const Document *doc)
{
    if (!doc) return std::string();
    std::string dir(doc->fpath_s);
    while (!dir.empty() && (dir[dir.size() - 1] == '\\' || dir[dir.size() - 1] == '/'))
        dir.erase(dir.size() - 1);
    return dir;
}

/* Full "dir\file" path, or just the filename when the document has no dir. */
static std::string DocFullPathLabel(const Document *doc)
{
    if (!doc || doc->fname_s[0] == '\0') return std::string();
    std::string dir = DocDirKey(doc);
    if (dir.empty()) return std::string(doc->fname_s);
#ifdef _WIN32
    return dir + "\\" + doc->fname_s;
#else
    return dir + "/" + doc->fname_s;
#endif
}

/* What the title bar shows: the whole path. A bare filename cannot tell you
   which UGMO8.IMG you are editing, and that matters most exactly when it is
   easiest to get wrong -- two copies of a file open from different folders. */
static std::string DocTitleName(const Document *doc)
{
    if (!doc || doc->fname_s[0] == '\0') return std::string("(unsaved)");
    return DocFullPathLabel(doc);
}

/* Fit `prefix + path` into `max_w` by dropping whole leading directories and
   marking the cut with an ellipsis, so a path too long for the menu bar
   degrades to its last few folders rather than overflowing or being clipped
   mid-word. Whole components only -- half a folder name is just noise -- and
   the filename survives even when nothing else fits. */
static std::string ElidePathLabel(const std::string &prefix,
                                  const std::string &path, float max_w)
{
    std::string full = prefix + path;
    if (max_w <= 0.0f || ImGui::CalcTextSize(full.c_str()).x <= max_w)
        return full;

    std::string best = full;
    size_t pos = 0;
    for (;;) {
        size_t next = path.find_first_of("\\/", pos);
        if (next == std::string::npos) break;
        pos = next + 1;
        if (pos >= path.size()) break;
        best = prefix + "..." + path.substr(pos);
        if (ImGui::CalcTextSize(best.c_str()).x <= max_w)
            break;
    }
    return best;
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
        bool doc_changed = (g_doc != g_prev_render_doc);
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
        /* The arrow-key nudge belongs to one sprite in one document. Landing on
           that sprite is how it gets armed (paste / canvas resize set the index
           just before this runs), so only a move *away* from it disarms. */
        if (doc_changed || g_content_nudge_img != g_doc->ilselected)
            g_content_nudge_img = -1;
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

    /* Adobe-standard selection shortcuts.

       Gated on the Image canvas actually being in front. select_all() sets a
       pixel marquee over the SELECTED SPRITE, which the other canvas modes do
       not draw in sprite space -- pressed in World View it left a stray
       marquee sitting in the corner of the scene, over nothing, because the
       rectangle is in sprite pixels and World View is not.

       Ctrl+D stays live everywhere: clearing a selection you cannot see is
       exactly what someone who has ended up with a stray one wants, and it
       cannot create anything. */
    bool image_canvas = ImageCanvasActive();
    if (image_canvas && ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_A, route)) select_all();
    if (ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D, route)) deselect_all();
    if (image_canvas &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_I, route)) invert_selection();
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
    /* E and C were advertised in the v2.x changelog when Smart Eraser and
       Clone Stamp landed, but the bindings themselves were never wired — the
       tools stayed toolbar-only. Both bare keys are otherwise unused (only
       Ctrl+E / Ctrl+C / Ctrl+Shift+C are taken), so the advertised shortcuts
       cost nothing to honor. Neither tool has a floating-paste meaning, so
       unlike H/V/L they need no g_pasted guard. */
    if (ImGui::Shortcut(ImGuiKey_E, route)) {
        g_active_tool = (g_active_tool == ActiveTool::BackgroundEraser)
                            ? ActiveTool::None : ActiveTool::BackgroundEraser;
    }
    if (ImGui::Shortcut(ImGuiKey_C, route)) {
        g_active_tool = (g_active_tool == ActiveTool::CloneStamp)
                            ? ActiveTool::None : ActiveTool::CloneStamp;
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
    /* Del with a live selection erases what is inside it, the way every paint
       program behaves — deleting the whole sprite out from under a marquee is
       never what that keystroke means. Gated on the selection actually being
       drawn (same condition the canvas uses) so an invisible leftover
       g_grid_sel can't swallow the sprite-delete key. Shift+Del above stays
       the unconditional delete-sprite escape hatch. */
    bool selection_visible =
        g_grid_sel.active && !g_palette_nav &&
        (g_active_tool == ActiveTool::Marquee ||
         g_active_tool == ActiveTool::MagicWand ||
         g_active_tool == ActiveTool::Lasso);
    if (!popup_using_keyboard && !io.WantTextInput && !io.KeyCtrl && !io.KeyShift && !io.KeyAlt &&
        (ImGui::Shortcut(ImGuiKey_Delete, route) ||
         (selection_visible && ImGui::Shortcut(ImGuiKey_Backspace, route)))) {
        if (selection_visible) {
            int cleared = ClearSelectionPixels();
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     cleared > 0 ? "Cleared %d pixel%s inside the selection."
                                 : "Selection was already empty.",
                     cleared, cleared == 1 ? "" : "s");
            g_restore_msg_timer = 3.0f;
        } else if (g_palette_nav) {
            DeletePalette();
        } else {
            RequestDeleteImage(g_doc->ilselected);
        }
    }

    /* Tool Intercepts. Esc/Enter have a three-level priority: transform
       takes precedence, then floating paste, then marquee. */
    if (ImGui::Shortcut(ImGuiKey_Escape, route)) {
        if (g_xform.active)         { xform_cancel(); }
        else if (g_pasted.active)   { g_pasted.active = false; g_pasted.dragging = false; }
        else if (g_grid_sel.active) { g_grid_sel.active = false; }
        else if (g_content_nudge_img >= 0) { g_content_nudge_img = -1; }
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
    /* Same deal one step later: after Paste as New Sprite (or a canvas resize)
       the art is already committed, so there is no floating rect — but the
       arrows still belong to positioning it, not to walking the image list off
       the sprite that was just created. */
    bool content_nudge_armed = !g_pasted.active && !popup_using_keyboard &&
                               !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt &&
                               g_content_nudge_img >= 0 &&
                               g_content_nudge_img == g_doc->ilselected;
    if (content_nudge_armed) {
        int step = io.KeyShift ? 10 : 1;
        ImGuiKeyChord mod = io.KeyShift ? ImGuiMod_Shift : 0;
        int dx = 0, dy = 0;
        if (ImGui::Shortcut(mod | ImGuiKey_LeftArrow, route))  dx -= step;
        if (ImGui::Shortcut(mod | ImGuiKey_RightArrow, route)) dx += step;
        if (ImGui::Shortcut(mod | ImGuiKey_UpArrow, route))    dy -= step;
        if (ImGui::Shortcut(mod | ImGuiKey_DownArrow, route))  dy += step;
        if (dx || dy) NudgeSelectedSpriteContent(dx, dy);
    }

    /* Image Operations. Space marks whichever list has the keyboard: the Anim
       frame browser when it was the last one clicked, else the image list. */
    if (ImGui::Shortcut(ImGuiKey_Space, route)) {
        if (SeqScrFrameNavActive()) {
            SeqScrToggleSelectedMark();
        } else {
            IMG *img = get_img(g_doc->ilselected);
            if (img) { img->flags ^= 1; mark_dirty(); }
        }
    }
    /* Timeline play/pause (K = standard video editor convention). */
    if (ImGui::Shortcut(ImGuiKey_K, route)) imgtool_toggle_timeline_play();
    /* Left/Right scrub the animation timeline, or the marked-tab World View
       sequence when that preview is active. */
    /* A merely focused numeric widget must not steal Left/Right in World View.
       Active drags and text entry still block navigation normally. */
    bool world_like_mode = g_world_state.enabled || g_seqscr_workspace;
    bool widget_using_keyboard = popup_using_keyboard || ImGui::IsAnyItemActive() ||
                                 (!world_like_mode && ImGui::IsAnyItemFocused()) ||
                                 io.WantTextInput;
    if (!g_pasted.active && !content_nudge_armed && !widget_using_keyboard &&
        !io.KeyCtrl && !io.KeyShift && !io.KeyAlt) {
        if (ImGui::Shortcut(ImGuiKey_LeftArrow, route)) {
            if (world_like_mode && g_world_marked_state.marked_play) StepWorldMarkedSequence(g_world_marked_state, -1);
            else StepTimelinePlayhead(-1);
        }
        if (ImGui::Shortcut(ImGuiKey_RightArrow, route)) {
            if (world_like_mode && g_world_marked_state.marked_play) StepWorldMarkedSequence(g_world_marked_state, 1);
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
    if (!g_pasted.active && !content_nudge_armed && !popup_using_keyboard &&
        !widget_using_keyboard && SeqScrFrameNavActive()) {
        /* The Anim frame browser was the last list clicked: Up/Down walk its
           frames and each highlight previews in the workspace corner box. */
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route))
            SeqScrStepFrameSelection(1);
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route))
            SeqScrStepFrameSelection(-1);
    } else if (!g_pasted.active && !content_nudge_armed && !popup_using_keyboard &&
        !widget_using_keyboard && g_seqscr_workspace &&
        WorldEmbeddedSeqScrActive(g_world_marked_state)) {
        /* In the Sequence/Script workspace, Up/Down step through the loaded
           record's entries (selecting each target sprite) instead of walking
           the main image list, so you can scrub it without clicking. */
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route))
            StepWorldEmbeddedSeqScrEntry(g_world_marked_state, 1);
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route))
            StepWorldEmbeddedSeqScrEntry(g_world_marked_state, -1);
    } else if (!g_pasted.active && !content_nudge_armed && !popup_using_keyboard &&
               g_palette_nav && g_doc->palcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            SelectPalette((g_doc->plselected + 1) % (int)g_doc->palcnt);
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            SelectPalette((g_doc->plselected <= 0) ? (int)g_doc->palcnt - 1 : g_doc->plselected - 1);
            g_zoom_reset = true;
        }
    } else if (!g_pasted.active && !content_nudge_armed && !popup_using_keyboard &&
               g_doc->imgcnt > 0) {
        if (ImGui::Shortcut(ImGuiKey_DownArrow, route)) {
            g_doc->ilselected = (g_doc->ilselected + 1) % (int)g_doc->imgcnt;
            g_zoom_reset = true;
        }
        if (ImGui::Shortcut(ImGuiKey_UpArrow, route)) {
            g_doc->ilselected = (g_doc->ilselected <= 0) ? (int)g_doc->imgcnt - 1 : g_doc->ilselected - 1;
            g_zoom_reset = true;
        }
    }
    /* Tab toggles World View mode (anipoint alignment workspace). From the
       Sequence/Script workspace it returns to the pixel editor rather than
       flipping a mode that is not currently showing. */
    if (ImGui::Shortcut(ImGuiKey_Tab, route)) {
        AnipointLink().enabled = false;
        if (g_seqscr_workspace || g_reactions_workspace) {
            g_seqscr_workspace = false;
            g_reactions_workspace = false;
            g_world_state.enabled = false;
        } else {
            g_world_state.enabled = !g_world_state.enabled;
        }
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
                {
                    /* World View exports need a live composited scene, so they
                       stay disabled until the marked sequence is running. */
                    bool world_live = g_world_state.enabled && g_world_marked_state.marked_play;
                    if (ImGui::MenuItem("World View PNG (Current Tick)...", NULL, false, world_live))
                        OpenFileDialog(FileDialogMode::ExportWorldPng);
                    if (ImGui::IsItemHovered() && !world_live)
                        ImGui::SetTooltip("Enable World View and start the marked frame sequence first.");
                    if (ImGui::MenuItem("World View PNG Sequence...", NULL, false, world_live))
                        OpenFileDialog(FileDialogMode::ExportWorldPngSeq);
                    if (ImGui::IsItemHovered() && !world_live)
                        ImGui::SetTooltip("Enable World View and start the marked frame sequence first.");
                }
                if (ImGui::MenuItem("Palette..."))                     OpenFileDialog(FileDialogMode::ExportPalette);
                ImGui::Separator();
                if (ImGui::MenuItem("Save LBM", "Alt+S"))        OpenFileDialog(FileDialogMode::SaveLbm);
                if (ImGui::MenuItem("Save Marked LBM"))          OpenFileDialog(FileDialogMode::SaveMarkedLbm);
                if (ImGui::MenuItem("Save TGA"))                 OpenFileDialog(FileDialogMode::SaveTga);
                ImGui::Separator();
                if (ImGui::MenuItem("Build TGA from Marked", "Ctrl+B")) OpenFileDialog(FileDialogMode::ExportTga);
                if (ImGui::MenuItem("Write ANILST..."))                OpenFileDialog(FileDialogMode::WriteAniLst);
                if (ImGui::MenuItem("Write TBL..."))                   OpenFileDialog(FileDialogMode::WriteTbl);
                if (ImGui::MenuItem("Compare Against TBL..."))         OpenFileDialog(FileDialogMode::CompareTbl);
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Diff a checked-in .TBL against this library instead of\n"
                    "overwriting it. Hand-maintained tables have no build step\n"
                    "to regenerate them, so a re-export that moved an anipoint\n"
                    "would desync art from table silently.");
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
            ImGui::MenuItem("Add Pasted Colors to Palette", NULL, &g_paste_import_colors);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "When the clipboard came from another palette, copy the colors it\n"
                "needs into the target palette's unused indices instead of remapping\n"
                "them to the nearest existing color. Only fills slots the declared\n"
                "bit depth already covers and no sprite is drawing with.");
            if (ImGui::MenuItem("Clear Selection Contents", "Del", false,
                                g_grid_sel.active && g_doc->ilselected >= 0)) {
                int cleared = ClearSelectionPixels();
                snprintf(g_restore_msg, sizeof(g_restore_msg),
                         cleared > 0 ? "Cleared %d pixel%s inside the selection."
                                     : "Selection was already empty.",
                         cleared, cleared == 1 ? "" : "s");
                g_restore_msg_timer = 3.0f;
            }
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
                "then press Enter or click outside it to cut those pixels to transparent.\n"
                "The removed target pixels replace the clipboard and retain this frame's palette.");
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
            if (ImGui::MenuItem("Split Body Parts...", NULL, false, SelectedImageCanBodySplit()))
                OpenBodySplitDialog();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Detect head, arms, torso, and legs from the sprite's silhouette,\n"
                "adjust the proposed boxes, then cut each into its own sprite\n"
                "with the anipoint rebased so the pieces still line up.");
            if (ImGui::MenuItem("Resize Sprite...", NULL, false, g_doc->ilselected >= 0)) OpenResizeSpriteDialog();
            if (ImGui::MenuItem("Canvas Size...", NULL, false, g_doc->ilselected >= 0)) OpenCanvasSizeDialog();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Change the frame without touching the art: the sprite keeps its\n"
                "exact pixels and size, the canvas grows or crops around it.\n"
                "Anipoints and the hitbox move with the art.");
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
            {
                int marked = CountMarkedImages();
                char label[64];
                snprintf(label, sizeof(label), "Trim Marked Bounds (Crop) (%d)", marked);
                if (ImGui::MenuItem(label, NULL, false, marked > 0)) BulkTrimMarkedToContent();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Trim each marked image to its non-transparent bounding box,\n"
                    "as one undo step. Anipoints are adjusted so the on-screen\n"
                    "position is unchanged.");
            }
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
            if (ImGui::MenuItem("Erase Copied Object...", NULL, false,
                                StampEraseAvailable()))
                OpenStampEraseDialog();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Copy an object once (Ctrl+C), then subtract it from every frame\n"
                "it was composited into. Each frame is searched for the object's\n"
                "own pixels — no anipoints, no lining anything up — and only\n"
                "pixels that match it exactly are cleared, so whatever was painted\n"
                "over the top survives. Built for pulling a reused sprite back out\n"
                "from under an effect (UMK3SKEL18 under the flames in UMK3FIRE).");
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
            if (ImGui::MenuItem("Shift Anipoints by dX/dY...")) {
                OpenAnipointShiftDialog();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Adds a numeric offset to a whole run of frames — marked set,\n"
                "name pattern (MK1FIRE*), selection, or all — as one undo step.");
            if (ImGui::MenuItem("Mirror Marked Anipoints to Reverse")) {
                MirrorMarkedAnipointsToReverseWithToast();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Mirrors X anipoints on marked sprites about the sprite width.\n"
                "Y/Z values stay unchanged. Convention: %s.\n"
                "Change it under View > Mirror Preview.",
                mirror_convention_label(g_mirror_convention));
            if (ImGui::MenuItem("Level Unset Anipoints from Stance")) {
                LevelUnsetAnipointsFromStance();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Gives frames that never got an anipoint (still 0,0) a sensible\n"
                "one, using the STANCE frame's footing as the reference.\n\n"
                "Same palette as the stance: stood on its ground line, so added\n"
                "frames stop bobbing against the shipped art.\n"
                "Other palettes (effects): centred on their own pixels.\n\n"
                "Frames that already carry an anipoint are never touched, even\n"
                "when they disagree with the stance.");
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
            if (ImGui::MenuItem("Isolate Selection Colors"))                  IsolateSelectionColors();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Reserves the palette indices the selection draws with for the\n"
                "selection alone. Pixels elsewhere in this sprite that share one\n"
                "are repointed at a duplicate slot of the same color, so the frame\n"
                "looks identical and those indices can be recolored on their own.\n"
                "Grows the palette when it has to; other frames are untouched.");
            if (ImGui::MenuItem("Isolate Selection Colors Across Frames..."))  OpenIsolatePropagatePreview();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "The same isolation, run over every sprite on this palette. Finds\n"
                "the feature in each frame the way Remap Similar Regions does, then\n"
                "reserves the indices once for all of them. Previews per frame first;\n"
                "frames it cannot find the feature in are flagged and left off.");
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
            ImGui::MenuItem("Group Tabs by Name", NULL, &g_group_doc_tabs);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Past %d open tabs, fold tabs sharing a name stem into one --\n"
                    "BARAKA1/2/3.IMG become a single BARAKA tab, and so do copies\n"
                    "of one 8.3 name opened from different folders.\n\n"
                    "Click a folded tab to expand the group in place; click its\n"
                    "v header to fold it back. A stem with only one file is\n"
                    "always an ordinary tab.\n\n"
                    "Below %d tabs nothing is folded. Dragging tabs to reorder\n"
                    "is off while a group is folded.", kDocTabGroupMin,
                    kDocTabGroupMin);
            ImGui::MenuItem("Show Subframe Swap Tool", NULL,
                            &g_world_show_subframe_tool);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Show the Subframes/Tick/Swap strip above a World View row's\n"
                    "frame thumbnails. It replaces the selected sprite with a\n"
                    "single composite of its child subframes, from a tick onward.\n\n"
                    "Off by default: it only appears when the selected sprite\n"
                    "happens to have subframes, so it arrives unannounced and\n"
                    "pushes the thumbnails down.");
            ImGui::Separator();
            ImGui::BeginDisabled(g_doc->ilselected < 0);
            if (ImGui::MenuItem("Zoom In", "Ctrl+=")) QueueZoomStep(1);
            if (ImGui::MenuItem("Zoom Out", "Ctrl+-")) QueueZoomStep(-1);
            if (ImGui::MenuItem("Fit Sprite", "Ctrl+0")) QueueZoomFit();
            ImGui::EndDisabled();
            ImGui::Separator();
            ImGui::MenuItem("Anim Points",     NULL, &g_show_points);
            ImGui::MenuItem("Strike Box",      NULL, &g_show_hitbox);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Draw the MKSTK.ASM collision box of the move the\n"
                                  "selected frame belongs to, and drag its corners.\n"
                                  "Edit the numbers in Sprite > Hitbox.");
            ImGui::MenuItem("Anipoint Warnings", NULL, &g_show_anipoint_warnings);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Amber ! on image-list rows whose anipoint sits outside the\n"
                "sprite's own box. Off-box is not wrong by itself — props\n"
                "placed through match_ani_points are off-box on purpose — so\n"
                "libraries built that way badge every row. Turn this off there.");
            if (ImGui::BeginMenu("Mirror Preview")) {
                if (ImGui::MenuItem("Off", NULL, g_flip_preview == FlipPreviewMode::Off))
                    g_flip_preview = FlipPreviewMode::Off;
                if (ImGui::MenuItem("Ghost (both placements)", NULL,
                                    g_flip_preview == FlipPreviewMode::Ghost))
                    g_flip_preview = FlipPreviewMode::Ghost;
                if (ImGui::MenuItem("Flipped only", NULL,
                                    g_flip_preview == FlipPreviewMode::Only))
                    g_flip_preview = FlipPreviewMode::Only;
                ImGui::Separator();
                ImGui::TextDisabled("Mirror math: %s",
                                    mirror_convention_label(g_mirror_convention));
                if (ImGui::MenuItem("ani2 — multipart (w - x)", NULL,
                                    g_mirror_convention == MirrorConvention_Ani2))
                    g_mirror_convention = MirrorConvention_Ani2;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MKUTIL.ASM ani2. What multipart records and\n"
                                      "flip_multi/match_ani_points apply, and what\n"
                                      "imgtool has always used.");
                if (ImGui::MenuItem("ganiof — single-part (w - 1 - x)", NULL,
                                    g_mirror_convention == MirrorConvention_Ganiof))
                    g_mirror_convention = MirrorConvention_Ganiof;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("MKDISP.ASM ganiof. One pixel tighter than ani2.\n"
                                      "Invisible on a flame column, very visible on a\n"
                                      "13 px bone strip.");
                ImGui::EndMenu();
            }
            ImGui::MenuItem("DMA Compression", NULL, &g_show_dma_comp);
            ImGui::MenuItem("Anim Scripts / Seqs (Raw Data)", NULL,
                            &g_show_seqscr_editor);
            ImGui::Separator();
            if (ImGui::MenuItem("World View", NULL, &g_world_state.enabled)) {
                if (g_world_state.enabled) {
                    g_seqscr_workspace = false;
                    AnipointLink().enabled = false;
                    g_reactions_workspace = false;
                }
            }
            if (ImGui::MenuItem("Sequence / Script Workspace", NULL,
                                &g_seqscr_workspace)) {
                if (g_seqscr_workspace) {
                    g_world_state.enabled = false;
                    AnipointLink().enabled = false;
                    g_reactions_workspace = false;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Build and preview this IMG's embedded SEQSCR sequences\n"
                "and scripts in their own animation workspace.");
            ImGui::MenuItem("World Reference Figure", NULL,
                            &g_world_state.show_reference);
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Standing-fighter outline at the shared World View anchor.\n"
                "Size and placement live on the World View toolbar's\n"
                "Reference checkbox (right-click it).");
            if (ImGui::MenuItem("Anipoint Link Workspace", NULL,
                                &AnipointLink().enabled)) {
                if (AnipointLink().enabled) {
                    g_world_state.enabled = false;
                    g_seqscr_workspace = false;
                    g_reactions_workspace = false;
                }
            }
            if (ImGui::MenuItem("Reactions Workspace", NULL,
                                &g_reactions_workspace)) {
                if (g_reactions_workspace) {
                    g_world_state.enabled = false;
                    g_seqscr_workspace = false;
                    AnipointLink().enabled = false;
                }
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Every reaction in a loaded character ASM, grouped, with the\n"
                "anipoints of each frame in the reaction's sequence.");
            if (g_world_state.enabled) {
                if (ImGui::MenuItem("Marked Row Playback", NULL, &g_world_marked_state.marked_play)) {
                    WorldMarkedRestart(g_world_marked_state);
                }
                ImGui::MenuItem("Marked Playback Paused", NULL, &g_world_marked_state.paused);
                /* The tick rate is hardware and this was a 1..60 slider on
                   it; see WorldDrawTickRateReadout. Speed is Ticks/frame. */
                ImGui::TextDisabled("Marked tick rate: %.4f Hz", kMk2TickHz);
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
            /* Canvas zoom rides just left of the filename. It used to be
               drawn at the canvas window's own top-left corner, which stacked
               a dim grey percentage on top of the "Image" view tab — neither
               was readable. Only the Image canvas has a zoom worth reporting,
               and only once it is off fit-to-window. */
            char zoom_label[32];
            zoom_label[0] = 0;
            bool image_canvas = ImageCanvasActive();
            if (image_canvas && !g_zoom_fit && g_doc->ilselected >= 0)
                snprintf(zoom_label, sizeof(zoom_label), "%.0f%%", g_zoom * 100.0f);

            float zoom_w = zoom_label[0]
                         ? ImGui::CalcTextSize(zoom_label).x + 16.0f : 0.0f;
            float avail_w = ImGui::GetContentRegionAvail().x;

            /* ASCII asterisk — universal 'modified' convention. The path is
               measured against what is actually left of the menu bar, so it
               shows in full when it fits and sheds leading folders when it
               does not. std::string rather than a fixed buffer: fpath_s alone
               is 1024 bytes. */
            std::string label = ElidePathLabel(g_dirty ? "* " : "  ",
                                               DocTitleName(g_doc),
                                               avail_w - zoom_w - 16.0f);
            float text_w = ImGui::CalcTextSize(label.c_str()).x + 16.0f;
            if (avail_w > text_w + zoom_w)
                ImGui::SameLine(ImGui::GetCursorPosX() + (avail_w - text_w - zoom_w));
            if (zoom_label[0]) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.72f, 0.28f, 1.0f));
                ImGui::TextUnformatted(zoom_label);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Canvas zoom. Ctrl+wheel over the canvas to change it.");
                ImGui::SameLine(0.0f, 16.0f);
            }
            if (g_dirty) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.7f, 0.2f, 1.0f));
            ImGui::TextUnformatted(label.c_str());
            if (g_dirty) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) {
                /* The path is worth showing on hover even when it is not
                   ambiguous — it answers "which file is this" without a trip
                   through the File menu. */
                std::string tip = DocFullPathLabel(g_doc);
                if (tip.empty()) tip = "Not saved to a file yet";
                if (g_dirty) tip += "\n\nUnsaved changes — Ctrl+S to save";
                ImGui::SetTooltip("%s", tip.c_str());
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
    /* The Sequence/Script workspace lays out its own viewport, inspector, and
       entry table inside the canvas, so it wants the whole canvas rect and
       neither the palette strip nor the sprite timeline underneath it. */
    g_world_marked_panel_docked = world_sequence_timeline || g_seqscr_workspace;
    bool hide_bottom_palette = g_world_state.enabled || g_seqscr_workspace ||
                               g_reactions_workspace;
    float bottom_palette_h = hide_bottom_palette ? 0.0f : PALETTE_H;
    float canvas_x = TOOLBAR_W;
    float canvas_y = work_y;
    float canvas_w = sw - TOOLBAR_W - PANEL_W;
    /* The sprite timeline is a World View instrument: it stages a frame
       sequence and plays it against the world canvas. On the Image tab it only
       ate a strip of editing height, so the strip is drawn while World View
       owns the canvas and nowhere else. The playback clock further down runs
       regardless of the strip, so a sequence started in World keeps ticking
       when you flip over to Image to paint. */
    float timeline_h = g_world_state.enabled ? TIMELINE_H : 0.0f;
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
        if (lane_count < 1) lane_count = 1;
        if (entry_count < 1) entry_count = 1;

        float desired_panel_h = 88.0f + (float)lane_count * 72.0f;
        if (entry_count > 8) desired_panel_h += 24.0f;
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
        /* Mirror of the canvas view tabs: picking Animation here puts the
           canvas on Anim, just as picking Anim there brings this panel
           forward. Tracked as a selection transition because ImGui queues tab
           selection a frame ahead of the tab body running. */
        enum { kPanelTabAssets = 0, kPanelTabSprite, kPanelTabAnimation };
        static int last_panel_tab = -1;
        int panel_tab = last_panel_tab;
        if (ImGui::BeginTabBar("##right_panel_tabs",
                               ImGuiTabBarFlags_FittingPolicyScroll)) {
        if (ImGui::BeginTabItem("Assets")) {
        panel_tab = kPanelTabAssets;
        /* --- Image List --- */
        int n_imgs = count_imgs();
        bool images_open = ImGui::CollapsingHeader("Images", ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Drag a row onto another to reorder the sprites in the file.\n"
            "Alt+PgUp / Alt+PgDn nudge the selected one instead.\n"
            "Reordering is one undo step and carries the animation timeline with it.");
        if (images_open) {
            float list_h = panel_h * 0.30f;
            if (ImGui::BeginListBox("##imglist", ImVec2(-1, list_h))) {
                if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
                    g_palette_nav = false;
                    g_seqscr_frame_nav = false;
                }
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

                /* ---- Drag-to-reorder ----
                   Only in Original ascending order. Under a Name or Size sort
                   the visible order is not the document order, so a drop would
                   move the sprite to a position the list cannot show it in —
                   the row would jump somewhere else the moment it re-sorted.
                   The move is deferred to after the list is submitted: applying
                   it mid-loop would leave every row below it holding a stale
                   index for the rest of the frame. */
                const char *kImageRowPayload = "IMGLIST_ROW";
                bool reorder_enabled = (g_image_list_sort == ImageListSort::Original &&
                                        !g_image_list_sort_desc);
                int pending_move_from = -1;
                int pending_move_to = -1;

                auto image_row_drag_drop = [&](int img_idx) {
                    if (!reorder_enabled) return;
                    if (ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload(kImageRowPayload, &img_idx, sizeof(int));
                        IMG *drag_img = get_img(img_idx);
                        ImGui::Text("Move #%d  %s", img_idx,
                                    drag_img ? drag_img->n_s : "");
                        ImGui::EndDragDropSource();
                    }
                    if (ImGui::BeginDragDropTarget()) {
                        const ImGuiPayload *p = ImGui::AcceptDragDropPayload(kImageRowPayload);
                        if (p && p->DataSize == (int)sizeof(int)) {
                            pending_move_from = *(const int *)p->Data;
                            pending_move_to = img_idx;
                        }
                        ImGui::EndDragDropTarget();
                    }
                };

                auto draw_image_context = [&](int img_idx) {
                    if (ImGui::BeginPopupContextItem("##imgctx")) {
                        g_doc->ilselected = img_idx;
                        IMG *ctx_img = get_img(img_idx);
                        if (ImGui::MenuItem("Mark / Unmark") && ctx_img) { ctx_img->flags ^= 1; }
                        if (ImGui::MenuItem("Rename"))        OpenRenameImage();
                        if (ImGui::MenuItem("Duplicate"))     DuplicateImage();
                        if (ImGui::MenuItem("Resize..."))     OpenResizeSpriteDialog();
                        if (ImGui::MenuItem("Canvas Size...")) OpenCanvasSizeDialog();
                        if (ImGui::MenuItem("Break into Subframes (Auto-Chop)..."))
                            OpenAutoChopDialogForImage(img_idx);
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
                        {
                            int marked = CountMarkedImages();
                            char label[48];
                            snprintf(label, sizeof(label), "Trim Marked Bounds (%d)", marked);
                            if (ImGui::MenuItem(label, NULL, false, marked > 0))
                                BulkTrimMarkedToContent();
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
                        g_seqscr_frame_nav = false;
                        if (ImGui::IsMouseDoubleClicked(0)) img->flags ^= 1;
                    }
                    if (selected && need_scroll && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                    if (selected && need_scroll) last_scrolled_to = g_doc->ilselected;
                    if (selected) ImGui::PopStyleColor(2);
                    image_row_drag_drop(row.idx);
                    DrawAnipointBoundsBadge(img, 6.0f);
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
                    /* Collapsed by default: a file like BOSS4 has hundreds of
                       chopped pieces, and expanding every group on open buries
                       the frames you actually animate. */
                    bool open = storage->GetBool(open_id, false);

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
                        g_seqscr_frame_nav = false;
                        if (ImGui::IsMouseDoubleClicked(0) && !toggle_clicked) img->flags ^= 1;
                    }
                    if (selected && need_scroll && !ImGui::IsItemVisible()) ImGui::SetScrollHereY(0.5f);
                    if (selected && need_scroll) last_scrolled_to = g_doc->ilselected;
                    if (selected) ImGui::PopStyleColor(2);
                    image_row_drag_drop(row.idx);
                    /* Clear the expand triangle, which owns item_max.x-18..-2. */
                    DrawAnipointBoundsBadge(img, 24.0f);
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

                    /* This name has no record of its own in the IMG — it is
                       inferred from the piece names (BGBIGFIST1A/1B/... imply
                       BGBIGFIST1). Give it the folder icon so it reads as the
                       group it is, rather than an unlabelled gap where every
                       other row has an icon. */
                    const char *vis_icon = any_marked ? (g_icon_font_loaded ? ICON_VIS : ICON_VIS_TXT) : "   ";
                    const char *grp_icon = g_icon_font_loaded ? ICON_FOLDER : ICON_FOLDER_TXT;
                    char label[96];
                    snprintf(label, sizeof(label), "%s %s  %s  (%d)", vis_icon, grp_icon,
                             parent.c_str(), (int)child_rows.size());

                    ImGui::PushID(src.c_str());
                    ImGui::PushID(parent.c_str());
                    ImGuiStorage *storage = ImGui::GetStateStorage();
                    ImGuiID open_id = ImGui::GetID("virtual_subframes_open");
                    bool open = storage->GetBool(open_id, false);
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
                        g_seqscr_frame_nav = false;
                    }
                    if (ImGui::IsItemHovered() && !toggle_clicked) {
                        ImGui::SetTooltip("%s has no record of its own in this IMG — only its %d pieces.\n"
                                          "Clicking selects the first piece; right-click to act on the whole group.",
                                          parent.c_str(), (int)child_rows.size());
                    }
                    if (ImGui::BeginPopupContextItem("##virtual_group_ctx")) {
                        ImGui::TextDisabled("%s  (%d pieces, no parent record)",
                                            parent.c_str(), (int)child_rows.size());
                        ImGui::Separator();
                        if (ImGui::MenuItem("Mark All Pieces")) {
                            for (int cid : child_rows)
                                if (rows[cid].img) rows[cid].img->flags |= 1;
                            mark_dirty();
                        }
                        if (ImGui::MenuItem("Unmark All Pieces")) {
                            for (int cid : child_rows)
                                if (rows[cid].img) rows[cid].img->flags &= ~1;
                            mark_dirty();
                        }
                        ImGui::Separator();
                        if (ImGui::MenuItem("Select First Piece")) {
                            g_doc->ilselected = rows[child_rows[0]].idx;
                            g_palette_nav = false;
                        }
                        ImGui::EndPopup();
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

                if (pending_move_from >= 0 && pending_move_to >= 0) {
                    IMG *moved = get_img(pending_move_from);
                    char moved_name[16] = {0};
                    if (moved) {
                        strncpy(moved_name, moved->n_s, sizeof(moved_name) - 1);
                        moved_name[sizeof(moved_name) - 1] = '\0';
                    }
                    if (MoveImageToIndex(pending_move_from, pending_move_to)) {
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Moved %s from #%d to #%d.",
                                 moved_name, pending_move_from, pending_move_to);
                        g_restore_msg_timer = 3.0f;
                    }
                }
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
                if (ImGui::MenuItem("Canvas Size...")) { OpenCanvasSizeDialog(); }
                if (ImGui::MenuItem("Opacity Gradient...")) { OpenOpacityGradientDialog(); }
                if (g_doc->ilselected < 0) ImGui::EndDisabled();
                {
                    int marked = CountMarkedImages();
                    char label[64];
                    snprintf(label, sizeof(label), "Trim Marked Sprites (Crop) (%d)", marked);
                    if (ImGui::MenuItem(label, NULL, false, marked > 0))
                        BulkTrimMarkedToContent();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Crop every marked sprite to its opaque bounds as one undo\n"
                        "step. Anipoints move with the art so nothing shifts on screen.");
                }

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
                if (ImGui::MenuItem("Split Body Parts...", NULL, false, SelectedImageCanBodySplit()))
                    OpenBodySplitDialog();

                ImGui::EndPopup();
            }
        }

        /* --- Palette List & Color Tools --- */
        DrawRightPanelPaletteEditor(panel_h);

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Sprite")) {
        panel_tab = kPanelTabSprite;

        /* --- Properties --- */
        if (ImGui::CollapsingHeader("Properties", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                LabeledValue("Name:", "%.15s", img->n_s);
                LabeledValue("Size:", "%d x %d", (int)img->w, (int)img->h);

                /* ROM cost, modelled on LOAD2 rather than estimated: the old
                   readout assumed 8bpp and a byte per kept pixel, which
                   overstated packed art badly (MK2 ships PPP> 6) and ignored
                   the run-unit quantization. See platform/dma_pack.cpp. */
                if (img->data_p && img->w > 0 && img->h > 0) {
                    int stride = ((int)img->w + 3) & ~3;
                    const unsigned char *pixels = (const unsigned char *)img->data_p;
                    int bpp = Load2BppForImage(img);
                    DmaPackResult pack = DmaAnalyzeSprite(pixels, (int)img->w,
                                                          (int)img->h, stride,
                                                          bpp, true);
                    unsigned long raw_b = DmaBitsToBytes(pack.raw_bits);
                    unsigned long packed_b = DmaBitsToBytes(pack.packed_bits);

                    LabeledValue("DMA ROM:", "%lu B raw @ %dbpp", raw_b, pack.bpp);
                    if (pack.compressed) {
                        int saved = raw_b ? (int)(100 - (packed_b * 100 / raw_b)) : 0;
                        LabeledValue("", "%lu B packed  (-%d%%)", packed_b, saved);
                    } else {
                        LabeledValue("", "%lu B packed  (uncompressed)", packed_b);
                    }
                    if (ImGui::IsItemHovered()) {
                        if (pack.compressed)
                            ImGui::SetTooltip(
                                "What LOAD2 would emit for this sprite.\n"
                                "Zero compression on: leading runs in units of %d px,\n"
                                "trailing in units of %d px, 8 bits of lead/trail\n"
                                "header per line. DMA control word 0x%04X.",
                                pack.lead_factor, pack.trail_factor,
                                (unsigned)pack.control_word);
                        else
                            ImGui::SetTooltip(
                                "What LOAD2 would emit for this sprite.\n"
                                "Not zero-compressed: %s.\n"
                                "DMA control word 0x%04X.",
                                pack.skip_reason ? pack.skip_reason : "unknown",
                                (unsigned)pack.control_word);
                    }

                    /* The depth is the lever worth surfacing: crossing a
                       power-of-two boundary cuts every frame on this palette,
                       and nothing else in the pipeline comes close. */
                    int tight = DmaSuperBpp(pixels, (int)img->w, (int)img->h,
                                            stride, bpp);
                    if (tight < pack.bpp)
                        LabeledValue("", "fits %dbpp (-%d%%)", tight,
                                     (int)(100 - (long)tight * 100 / pack.bpp));
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
                DrawAnipointCenterOffsetReadout(img);

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
        if (ImGui::CollapsingHeader("Anipts Tools", ImGuiTreeNodeFlags_DefaultOpen)) {
            IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
            if (img) {
                int ax = (short)img->anix, ay = (short)img->aniy;
                int ax2 = (short)img->anix2, ay2 = (short)img->aniy2, az2 = (short)img->aniz2;
                /* Captioned row per point rather than one full-width bar per
                   component: the three second-point fields only fit across the
                   280 px panel when the heading gets its own line, and the
                   grouping is what tells you which numbers move together. */
                const float kAniFieldW = 58.0f;
                auto ani_field = [&](const char *caption, const char *id,
                                     int *value) {
                    ImGui::TextUnformatted(caption);
                    ImGui::SameLine(0.0f, 3.0f);
                    bool changed = AnimPointDragInt(id, value, -1024, 1024, kAniFieldW);
                    ImGui::SameLine(0.0f, 8.0f);
                    return changed;
                };

                ImGui::TextUnformatted("Point 1  (anchor)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Primary anipoint (anix/aniy): the pixel the game pins\n"
                                      "to the object's position when it draws this frame.\n"
                                      "Drag a field, or click it and use Left/Right to nudge\n"
                                      "one pixel at a time.");
                ImGui::Indent(10.0f);
                if (ani_field("X", "##ptx", &ax))
                    set_primary_anipoint_local(img, ax, (int)(short)img->aniy);
                if (ani_field("Y", "##pty", &ay))
                    set_primary_anipoint_local(img, (int)(short)img->anix, ay);
                ImGui::NewLine();
                ImGui::Unindent(10.0f);

                ImGui::TextUnformatted("Point 2  (optional)");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Secondary anipoint (anix2/aniy2/aniz2), -1/-1/-1 when\n"
                                      "unused. Z is the depth this piece sorts at against the\n"
                                      "other pieces of a multi-piece frame.");
                ImGui::Indent(10.0f);
                if (ani_field("X", "##ptx2", &ax2)) {
                    int cur_y2 = secondary_anipoint_in_use(img) ? (int)(short)img->aniy2 : 0;
                    set_secondary_anipoint_local(img, ax2, cur_y2);
                }
                if (ani_field("Y", "##pty2", &ay2)) {
                    int cur_x2 = secondary_anipoint_in_use(img) ? (int)(short)img->anix2 : 0;
                    set_secondary_anipoint_local(img, cur_x2, ay2);
                }
                if (ani_field("Z", "##ptz2", &az2))
                    set_secondary_anipoint_z_local(img, az2);
                ImGui::NewLine();
                ImGui::Unindent(10.0f);

                ImGui::Spacing();
                DrawAnipointCenterOffsetReadout(img);

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

                /* The pieces of a chop hold their position as
                   parent - offset, and nothing re-checks that against the
                   art. This puts the art back in charge. */
                {
                    /* Recalculating is a parent-level operation, but the thing
                       you have selected is usually the piece -- the audit and
                       the image list both name it. Making you go hunt for the
                       parent first is friction for nothing, so a selected
                       subframe retargets to its parent and gets fixed along
                       with its siblings. */
                    IMG *recalc_img = img;
                    IMG *owner = find_subframe_parent(g_doc, img);
                    if (owner) recalc_img = owner;
                    int kids = count_subframes(g_doc, recalc_img);
                    std::string recalc_name =
                        trim_sprite_name(img_name_string(recalc_img));

                    ImGui::BeginDisabled(kids <= 0);
                    if (ImGui::Button(owner ? "Recalculate This Piece from Parent"
                                            : "Recalculate Subframes from Parent",
                                      ImVec2(-1, 0))) {
                        SubframeRecalcReport r =
                            recalc_subframe_anipoints_from_parent(g_doc, recalc_img);
                        if (r.changed > 0)
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     "Re-anchored %d of %d subframe%s from %s "
                                     "(%d exact, %d approximate).",
                                     r.changed, r.considered,
                                     r.considered == 1 ? "" : "s",
                                     recalc_name.c_str(),
                                     r.exact, r.approximate);
                        else if (r.considered > 0)
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     "All %d subframe%s already sit where %s "
                                     "says.", r.considered,
                                     r.considered == 1 ? "" : "s",
                                     recalc_name.c_str());
                        else
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     "This sprite has no subframes.");
                        g_restore_msg_timer = 5.0f;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                        if (kids <= 0)
                            ImGui::SetTooltip("This sprite has no subframes, and\n"
                                              "is not a piece of anything in this\n"
                                              "file. Chop it first.");
                        else if (owner)
                            ImGui::SetTooltip("This is a piece of %s. Re-anchor\n"
                                              "that frame's %d piece%s from where\n"
                                              "their art really sits inside it.",
                                              recalc_name.c_str(), kids,
                                              kids == 1 ? "" : "s");
                        else
                            ImGui::SetTooltip("Find each of the %d subframe's\n"
                                              "pixels inside this sprite's own\n"
                                              "bitmap and set its anipoint to\n"
                                              "parent - offset. A piece already in\n"
                                              "the right place is left alone.",
                                              kids);
                    }
                    ImGui::EndDisabled();
                }

                /* World View placement is preview state until it is baked:
                   dragging a frame there writes a local dX/dY that never
                   reaches the file. These fold it into the sprite's own
                   anipoint, so what you lined up is what saves. */
                ImGui::Spacing();
                {
                    WorldMarkedSequenceState &wstate = g_world_marked_state;
                    int wslot = -1, wentry = -1;
                    bool in_world = WorldMarkedFindEntryForImage(
                        wstate, document_active_index(), g_doc->ilselected,
                        &wslot, &wentry);

                    ImGui::BeginDisabled(!in_world);
                    if (ImGui::Button("Inherit Position from World View", ImVec2(-1, 0))) {
                        /* Baking rewrites anipoints in place. Snapshot first so
                           a bake that lands the wrong way is one Ctrl+Z, not a
                           hand-typed restore. */
                        doc_undo_push();
                        int conflicts = 0;
                        int changed = WorldMarkedBakeEntryOffsets(wstate, wslot,
                                                                  wentry, &conflicts);
                        if (changed > 0) InvalidateThumb(g_doc->ilselected);
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 changed > 0
                                     ? "Baked this frame's World View offset into its anipoint."
                                     : "This frame sits at its own anipoint already.");
                        g_restore_msg_timer = 4.0f;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(in_world
                            ? "Slot %d, entry %d: add that entry's local dX/dY to this\n"
                              "sprite's anipoint and clear the offset. The sprite does\n"
                              "not move on screen — the placement just becomes real."
                            : "This sprite is not in any World View lane.\n"
                              "Mark its row and turn on Marked in World View first.",
                            wslot + 1, wentry + 1);

                    if (ImGui::Button("Inherit All from World View Slot", ImVec2(-1, 0))) {
                        doc_undo_push();
                        int conflicts = 0;
                        int changed = WorldMarkedBakeEntryOffsets(wstate, wslot,
                                                                  -1, &conflicts);
                        ClearTimelineThumbCache();
                        if (conflicts > 0)
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     "Baked %d anipoint%s. %d entr%s skipped: the same "
                                     "sprite appears more than once with its own offset.",
                                     changed, changed == 1 ? "" : "s", conflicts,
                                     conflicts == 1 ? "y was" : "ies were");
                        else
                            snprintf(g_restore_msg, sizeof(g_restore_msg),
                                     changed > 0
                                         ? "Baked %d World View offset%s into anipoints."
                                         : "Nothing to bake — this slot has no offsets.",
                                     changed, changed == 1 ? "" : "s");
                        g_restore_msg_timer = 5.0f;
                    }
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                        ImGui::SetTooltip(in_world
                            ? "Do the same for every entry in slot %d, so a whole\n"
                              "animation's staging lands in the IMGs at once."
                            : "This sprite is not in any World View lane.",
                            wslot + 1);
                    ImGui::EndDisabled();
                }

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
                    ImGui::SetTooltip("For every marked sprite, mirrors X1 and active X2 about the\n"
                                      "sprite's own width. Y/Z stay unchanged.\n"
                                      "Convention: %s — %s\n"
                                      "Change it under View > Mirror Preview.",
                                      mirror_convention_label(g_mirror_convention),
                                      mirror_convention_source(g_mirror_convention));
                }

                if (ImGui::Button("Shift Anipoints...", ImVec2(-1, 0)))
                    OpenAnipointShiftDialog();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Add a numeric dX/dY to a whole run of frames at once\n"
                                      "(marked set, name pattern like MK1FIRE*, or all).\n"
                                      "Applies as a single undo step.");
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

        /* Defined further down, next to the other panel bodies. */
        /* --- Hitbox Editor ---
           This used to be four sliders over app-wide globals with a clipboard
           button: nothing loaded them, nothing saved them, and they belonged
           to no sprite. The box that actually exists is the MKSTK.ASM strike
           record, so the panel edits that -- for whichever move the selected
           frame belongs to -- and Save writes the .ASM back. */
        if (ImGui::CollapsingHeader("Hitbox")) {
            DrawStrikeBoxPanel();
        }

        ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Animation", NULL,
                                g_request_animation_sidebar
                                    ? ImGuiTabItemFlags_SetSelected : 0)) {
        panel_tab = kPanelTabAnimation;
        g_request_animation_sidebar = false;
        /* --- Frames across the numbered IMG set ---
           Sits above Library because it is what this tab is for now: pick the
           character's frames, mark them, push them into a sequence. Sequence
           and script editing itself lives in the canvas Anim tab. */
        if (ImGui::CollapsingHeader("Frames", ImGuiTreeNodeFlags_DefaultOpen))
            DrawSeqScrFrameBrowser(panel_h * 0.42f);

        /* --- Library Info --- */
        if (ImGui::CollapsingHeader("Library", ImGuiTreeNodeFlags_DefaultOpen)) {
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
            ImGui::Text("AnimBlob: %u B", g_doc->scrseqbytes);
            /* Sequence/script lists, ASM export, and the raw-data editor moved
               to the canvas Anim tab, which has room to show them properly. */
            ImGui::TextDisabled("Sequences and scripts: canvas > Anim tab.");
        }

        ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
        }
        /* Link already pairs with this panel, so switching here while the Link
           workspace is open leaves the canvas alone instead of kicking the
           user out of it. */
        if (panel_tab != last_panel_tab && panel_tab == kPanelTabAnimation &&
            !AnipointLink().enabled) {
            g_seqscr_workspace = true;
            g_world_state.enabled = false;
        }
        last_panel_tab = panel_tab;
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

    /* The Sequence/Script workspace fills the canvas rect to the screen
       bottom, so there is no timeline strip to draw under it. */
    if (timeline_h > 0.0f) {
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
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Ticks per second. MK2 runs at %.1f.", kMk2TickHz);
        ImGui::SameLine();
        if (ImGui::SmallButton("Game##timeline_game_fps")) g_play_speed = kMk2TickHz;
        ImGui::PopItemWidth();

        ImGui::SameLine();
        int cur_hold = TimelineHoldAt(g_timeline_play_idx);
        ImGui::PushItemWidth(72);
        if (ImGui::InputInt("Hold", &cur_hold, 1, 4))
            TimelineSetHoldAt(g_timeline_play_idx, cur_hold);
        ImGui::PopItemWidth();
        if (ImGui::IsItemHovered()) {
            /* Spell the timing out from the live rate rather than a canned
               example: the slider beside this defaults to hardware speed, so a
               fixed "at 12 FPS" line described a rate the timeline was not
               running at. */
            int shown = ClampTimelineHold(cur_hold);
            float rate = g_play_speed > 0.0f ? g_play_speed : kMk2TickHz;
            ImGui::SetTooltip(
                "Base ticks to wait before this frame advances. This is the\n"
                "number you write into the ASM, not a preview-only speed.\n\n"
                "At %.1f ticks/sec, a hold of %d lasts %.0f ms (%.1f fps).\n"
                "New frames start at %d.",
                rate, shown, 1000.0f * (float)shown / rate, rate / (float)shown,
                kDefaultTimelineHold);
        }
        
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
    }

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

    DrawIsolatePropagateDialog();

    DrawSpriteLayerPanel();

    if (g_request_save_world_asm) { g_request_save_world_asm = false; OpenFileDialog(FileDialogMode::SaveAsmAnim); }
    if (g_request_save_world_project) { g_request_save_world_project = false; OpenFileDialog(FileDialogMode::SaveWorldProject); }
    if (g_request_load_world_project) { g_request_load_world_project = false; OpenFileDialog(FileDialogMode::LoadWorldProject); }
    if (g_request_append_world_project) { g_request_append_world_project = false; OpenFileDialog(FileDialogMode::AppendWorldProject); }
    if (g_request_load_world_bg)      { g_request_load_world_bg = false; OpenFileDialog(FileDialogMode::LoadWorldBdd); }
    if (g_request_save_world_png) { g_request_save_world_png = false; OpenFileDialog(FileDialogMode::ExportWorldPng); }
    if (g_request_save_world_png_seq) { g_request_save_world_png_seq = false; OpenFileDialog(FileDialogMode::ExportWorldPngSeq); }
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

    DrawBodySplitDialog();

    DrawResizeSpriteDialog();
    DrawCanvasSizeDialog();
    DrawOpacityGradientDialog();
    DrawInnerStrokeDialog();
    DrawSpriteCleanupDialog();
    DrawStampEraseDialog();

    DrawBulkResizeDialog();

    DrawSetGroupAnipointsDialog();

    DrawAnipointShiftDialog();

    DrawTblCompareDialog();

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


/* Where an index that pointed into the pre-move list points afterwards.
   Erase-then-insert semantics, matching TimelineMoveFrame. */
static int RemapIndexForImageMove(int idx, int from, int to)
{
    if (idx < 0) return idx;
    if (idx == from) return to;
    if (from < idx && to >= idx) return idx - 1;
    if (from > idx && to <= idx) return idx + 1;
    return idx;
}

/* Move one sprite to another position in the document's image list.
 *
 * The single reorder primitive: drag-and-drop in the image list and the
 * Alt+PgUp/PgDn nudges all land here, so they cannot drift apart on what they
 * fix up afterwards. That fix-up is the whole reason this is not a two-line
 * pointer swap — plenty of state stores *image indices*, and every one of them
 * silently points at a different sprite once the list shifts underneath it. */
bool MoveImageToIndex(int from, int to)
{
    int n = (int)g_doc->imgcnt;
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;

    doc_undo_push();   /* reorders the image list — structural */

    /* Unlink. */
    IMG *before_from = NULL;
    IMG *node = (IMG *)g_doc->img_p;
    for (int i = 0; node && i < from; i++) {
        before_from = node;
        node = (IMG *)node->nxt_p;
    }
    if (!node) return false;
    if (before_from) before_from->nxt_p = node->nxt_p;
    else             g_doc->img_p = node->nxt_p;
    node->nxt_p = NULL;

    /* Relink so the sprite ends up at `to` in the resulting list. The chain is
       one shorter here, which is exactly what makes `to` mean the same thing
       as a vector erase-then-insert. */
    if (to == 0) {
        node->nxt_p = g_doc->img_p;
        g_doc->img_p = node;
    } else {
        IMG *prev = (IMG *)g_doc->img_p;
        for (int i = 0; prev && prev->nxt_p && i < to - 1; i++)
            prev = (IMG *)prev->nxt_p;
        if (!prev) {           /* only reachable if the list was empty */
            node->nxt_p = g_doc->img_p;
            g_doc->img_p = node;
        } else {
            node->nxt_p = prev->nxt_p;
            prev->nxt_p = node;
        }
    }

    g_doc->ilselected = RemapIndexForImageMove(g_doc->ilselected, from, to);

    /* The animation timeline stores image indices, not sprites, so without
       this a reorder re-points every frame in a built timeline at whatever
       sprite now occupies its slot. */
    for (int &frame : g_timeline_frames)
        frame = RemapIndexForImageMove(frame, from, to);
    for (int i = 0; i < 2; i++)
        g_timeline_composite[i] = RemapIndexForImageMove(g_timeline_composite[i], from, to);

    /* Thumbnails are cached by image index, so every entry from min(from,to)
       onward now shows the wrong sprite. */
    ClearTimelineThumbCache();
    g_img_tex_idx = -2;
    return true;
}

// Extracted from imgui_overlay.cpp: MoveImageUp
void MoveImageUp(void)

{
    if (g_doc->ilselected <= 0) return;
    MoveImageToIndex(g_doc->ilselected, g_doc->ilselected - 1);
}


// Extracted from imgui_overlay.cpp: MoveImageDown
void MoveImageDown(void)

{
    if (g_doc->ilselected < 0) return;
    MoveImageToIndex(g_doc->ilselected, g_doc->ilselected + 1);
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

/* ---- Strike-box panel (Sprite tab > Hitbox) ---------------------------
   The numeric half of the canvas overlay. Both edit one thing: the MKSTK.ASM
   record for the move the selected frame belongs to. There is deliberately no
   second, IMG-local hitbox any more -- one box, one place it is stored, one
   Save that writes it where the game reads it. */
static void DrawStrikeBoxPanel(void)
{
    if (g_mk2_doc.records.empty()) {
        ImGui::TextWrapped("No strike table loaded. Open MKSTK.ASM in "
                           "Tools > MK2 Hitboxes to edit collision boxes.");
        return;
    }

    ImGui::Checkbox("Follow selected frame##mk2_follow", &g_mk2_follow_frame);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Point this panel at whichever move the selected sprite\\n"
                          "belongs to, worked out from the character ASM: the\\n"
                          "animation that draws the frame names the strike\\n"
                          "(a_jchikick -> stk_jchikick). Off, the move stays where\\n"
                          "you put it in the MK2 Hitboxes window.");

    /* What the selected sprite resolved to, and why. */
    Document *doc = document_get(document_active_index());
    std::string anim;
    bool bound = false;
    int rec = -1;
    if (doc)
        rec = Mk2StrikeRecordForSprite(doc->uid, doc->ilselected, &anim, &bound);

    IMG *sel = (g_doc && g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    std::string sprite = sel ? img_name_string(sel) : std::string();

    /* Following puts the panel selection on the resolved record, so the
       manual picker in the MK2 window and this panel never disagree. Not
       following, the panel edits whatever that window has selected. */
    int edit_rec = g_mk2_follow_frame && rec >= 0 ? rec : Mk2CurrentRecord();

    if (edit_rec < 0) {
        if (anim.empty() && !sprite.empty())
            ImGui::TextWrapped("%s is not drawn by any animation in the loaded "
                               "character ASM, so there is no move to find a box "
                               "for. Load the character's MK*.ASM in the ASM "
                               "Animations window.", sprite.c_str());
        else if (!anim.empty())
            ImGui::TextWrapped("%s belongs to %s, which has no strike box under "
                               "any name imgtool recognises.",
                               sprite.c_str(), anim.c_str());
        else
            ImGui::TextWrapped("Select a sprite, or pick a move in the MK2 "
                               "Hitboxes window.");
    }

    /* Binding control: offered whenever we know the animation, so a wrong
       guess can be corrected as easily as a missing one. */
    if (!anim.empty()) {
        std::string existing = Mk2BoundStrikeFor(anim.c_str());
        ImGui::TextDisabled("%s", anim.c_str());
        ImGui::SameLine();
        if (bound) ImGui::TextDisabled("(bound)");
        else if (rec >= 0) ImGui::TextDisabled("(matched by name)");
        else ImGui::TextDisabled("(unmatched)");

        const char *preview = existing.empty() ? "Bind to move..."
                                               : existing.c_str();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::BeginCombo("##mk2_bind", preview)) {
            std::vector<std::string> labels;
            if (g_mk2_char_idx >= 0 &&
                g_mk2_char_idx < (int)g_mk2_doc.char_tables.size())
                labels = g_mk2_doc.char_tables[g_mk2_char_idx].moves;
            else
                for (size_t i = 0; i < g_mk2_doc.records.size(); i++)
                    labels.push_back(g_mk2_doc.records[i].label);

            if (!existing.empty() && ImGui::Selectable("(clear binding)")) {
                Mk2SetStrikeBinding(anim.c_str(), NULL);
                std::string err;
                if (!Mk2SaveStrikeBindings(&err))
                    snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", err.c_str());
                else
                    snprintf(g_restore_msg, sizeof(g_restore_msg),
                             "Cleared the binding for %s.", anim.c_str());
                g_restore_msg_timer = 4.0f;
            }
            for (size_t i = 0; i < labels.size(); i++) {
                bool is_sel = (labels[i] == existing);
                if (ImGui::Selectable(labels[i].c_str(), is_sel)) {
                    Mk2SetStrikeBinding(anim.c_str(), labels[i].c_str());
                    std::string err;
                    if (!Mk2SaveStrikeBindings(&err))
                        snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", err.c_str());
                    else
                        snprintf(g_restore_msg, sizeof(g_restore_msg),
                                 "Bound %s to %s, remembered beside MKSTK.ASM.",
                                 anim.c_str(), labels[i].c_str());
                    g_restore_msg_timer = 5.0f;
                }
            }
            ImGui::EndCombo();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Pin this animation to a move when the names do not\\n"
                              "line up (stk_jc_split has no a_jc_split). Written to\\n"
                              "MKSTK.imgtool beside the source, so it survives a\\n"
                              "restart and travels with the .ASM.");
        ImGui::Separator();
    }

    if (edit_rec < 0 || edit_rec >= (int)g_mk2_doc.records.size()) return;

    const mk2::StrikeRecord &r = g_mk2_doc.records[(size_t)edit_rec];
    ImGui::Text("%s", r.label.c_str());
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("MKSTK.ASM line %d", r.label_line);

    /* x/y are signed offsets from the fighter's own origin, w/h a size, which
       is why they are not clamped to the sprite's box: a strike legitimately
       reaches outside the art. */
    struct { int field; const char *label; } rows[4] = {
        { mk2::F_X_OFFSET, "X##stk_x" },
        { mk2::F_Y_OFFSET, "Y##stk_y" },
        { mk2::F_X_SIZE,   "W##stk_w" },
        { mk2::F_Y_SIZE,   "H##stk_h" },
    };
    for (int i = 0; i < 4; i++) {
        const mk2::StrikeField &f = r.fields[rows[i].field];
        int v = f.has_value ? (int)f.value : 0;
        ImGui::SetNextItemWidth(-1);
        ImGui::BeginDisabled(!f.has_value);
        if (ImGui::InputInt(rows[i].label, &v, 1, 8)) {
            mk2::undo_push(&g_mk2_doc, edit_rec, true);
            mk2::set_value(&g_mk2_doc, edit_rec, rows[i].field, v);
        }
        ImGui::EndDisabled();
        if (!f.has_value && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("This field is a symbol (%s), not a number -- edit it\\n"
                              "in the MK2 Hitboxes window.", f.raw.c_str());
    }

    /* Damage is one word, two bytes: hit in the high half, block in the low. */
    const mk2::StrikeField &dmg = r.fields[mk2::F_DAMAGE];
    if (dmg.has_value) {
        int hit = mk2::damage_hit((int)dmg.value);
        int blk = mk2::damage_block((int)dmg.value);
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("Hit dmg##stk_hit", &hit, 1, 8)) {
            if (hit < 0) hit = 0;
            if (hit > 255) hit = 255;
            mk2::undo_push(&g_mk2_doc, edit_rec, true);
            mk2::set_value(&g_mk2_doc, edit_rec, mk2::F_DAMAGE,
                           mk2::pack_damage(hit, blk));
        }
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputInt("Block dmg##stk_blk", &blk, 1, 8)) {
            if (blk < 0) blk = 0;
            if (blk > 255) blk = 255;
            mk2::undo_push(&g_mk2_doc, edit_rec, true);
            mk2::set_value(&g_mk2_doc, edit_rec, mk2::F_DAMAGE,
                           mk2::pack_damage(hit, blk));
        }
    }

    ImGui::Spacing();
    ImGui::BeginDisabled(!g_mk2_doc.dirty);
    if (ImGui::Button("Save MKSTK.ASM", ImVec2(-1, 0))) {
        std::string err;
        if (mk2::save(&g_mk2_doc, &err)) {
            snprintf(g_restore_msg, sizeof(g_restore_msg), "Saved MKSTK.ASM.");
        } else {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Save failed: %s", err.c_str());
        }
        g_restore_msg_timer = 5.0f;
    }
    ImGui::EndDisabled();
    if (g_mk2_doc.dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.2f, 1.0f), "*");
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip("Writes every edited record back into MKSTK.ASM,\\n"
                          "leaving untouched lines, comments and symbolic\\n"
                          "literals exactly as they were.");
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
        g_pending_tab_uid = doc->uid;
        g_show_unsaved_confirm = true;
        return;
    }
    bool closing_active = (idx == document_active_index());
    document_close_tab(idx);
    ResetPerDocumentUiState(false);
    if (closing_active) g_doc_tab_select_request = document_active_index();
}

/* ---- Document tab grouping ----------------------------------------------
   fname_s is a DOS 8.3 BASENAME, so a library opened across several folders
   fills the bar with tabs reading the same thing, and a character's frames
   arrive as BARAKA1/2/3.IMG -- a dozen tabs that are really one subject. Files
   sharing a name stem collapse into a single tab that names whichever member
   is in front; clicking it lists the rest.

   The stem is the filename with its extension and any trailing digits
   removed. Only stems with more than one member group -- a lone file is still
   just its own tab, which is what makes this safe to leave on. */
static std::string DocTabStem(const Document *doc)
{
    std::string s = (doc && doc->fname_s[0]) ? doc->fname_s : "Untitled";
    size_t dot = s.find_last_of('.');
    if (dot != std::string::npos && dot > 0) s.resize(dot);
    /* Done here rather than through strip_trailing_sequence_digits, which
       runs trim_sprite_name first -- that is tuned for sprite names, not
       filenames, and is not this function's business. */
    size_t end = s.size();
    while (end > 0 && isdigit((unsigned char)s[end - 1])) end--;
    if (end > 0 && end < s.size()) s.resize(end);
    return s;
}

/* On by default. Grouping only engages past kDocTabGroupMin open tabs and
   only for a stem that actually has several files behind it, so an ordinary
   session never sees it. */
bool g_group_doc_tabs = true;

/* Stems the user has expanded, by name. A collapsed group is one tab; an
   expanded one lays its members out as ordinary tabs behind a header that
   folds them back. Kept across frames because the group list is rebuilt
   every frame from the documents. */
static std::vector<std::string> s_expanded_stems;

static bool DocGroupExpanded(const std::string &stem)
{
    for (const std::string &e : s_expanded_stems)
        if (e == stem) return true;
    return false;
}

static void DocGroupToggle(const std::string &stem)
{
    for (size_t i = 0; i < s_expanded_stems.size(); i++) {
        if (s_expanded_stems[i] == stem) {
            s_expanded_stems.erase(s_expanded_stems.begin() + (long)i);
            return;
        }
    }
    s_expanded_stems.push_back(stem);
}

struct DocTabGroup {
    std::string stem;
    std::vector<int> members;   /* document indices, in tab order */
};

static void BuildDocTabGroups(std::vector<DocTabGroup> &groups)
{
    groups.clear();
    for (int i = 0; i < document_tab_count(); i++) {
        Document *d = document_get(i);
        if (!d) continue;
        std::string stem = DocTabStem(d);
        bool placed = false;
        for (DocTabGroup &g : groups) {
            if (g.stem == stem) { g.members.push_back(i); placed = true; break; }
        }
        if (!placed) {
            DocTabGroup g;
            g.stem = stem;
            g.members.push_back(i);
            groups.push_back(g);
        }
    }
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

    /* Clicks are recorded as document uids, not slot indices: a drag-reorder in
       the same frame renumbers the slots before the click is acted on. */
    unsigned int activate_uid = 0;
    unsigned int close_uid = 0;
    bool new_tab = false;
    int active = document_active_index();
    /* Set while any stem has more than one file behind it. Tab order no
       longer matches document order then, so the drag-reorder mapping below
       has to sit out -- reordering a group would be reordering a set. */
    bool any_group = false;
    /* Set by a click on a collapsed group; applied after EndTabBar so the
       tab list is not rebuilt underneath ImGui mid-bar. */
    std::string expand_stem;

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
    /* Document tabs get the cool/blue treatment; the view-mode tabs directly
       beneath them are tinted purple. The two bars sit in the same corner, so
       without distinct colours "which file" and "which view" read alike. */
    ImGui::PushStyleColor(ImGuiCol_Tab,         ImVec4(0.10f, 0.16f, 0.24f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabHovered,  ImVec4(0.22f, 0.45f, 0.72f, 1.00f));
    ImGui::PushStyleColor(ImGuiCol_TabSelected, ImVec4(0.18f, 0.38f, 0.62f, 1.00f));
    if (ImGui::BeginTabBar("##img_document_tabs", tab_flags)) {
        tab_bar_ptr = ImGui::GetCurrentTabBar();
        int n = document_tab_count();
        doc_tab_ids.assign((size_t)n, 0);

        std::vector<DocTabGroup> groups;
        if (g_group_doc_tabs && n > kDocTabGroupMin) {
            BuildDocTabGroups(groups);
        } else {
            for (int i = 0; i < n; i++) {
                DocTabGroup g;
                g.members.push_back(i);
                groups.push_back(g);
            }
        }
        for (const DocTabGroup &g : groups)
            if (g.members.size() > 1) { any_group = true; break; }

        int select_slot = (g_doc_tab_select_request >= 0)
                        ? g_doc_tab_select_request : active;

        for (int gi = 0; gi < (int)groups.size(); gi++) {
            const DocTabGroup &g = groups[(size_t)gi];
            bool grouped = g.members.size() > 1;
            bool expanded = grouped && DocGroupExpanded(g.stem);

            /* An expanded group is a header that folds it back, then its
               members as ordinary tabs. TabItemButton, not TabItem: the header
               is an action, and making it selectable would give the group a
               second way to be "current" that no document stands behind. */
            if (expanded) {
                char hdr[96];
                /* Distinct from the collapsed tab's ###doc_group_<stem>. The
                   two never coexist in a frame, but reusing one ID across a
                   TabItem and a TabItemButton would have ImGui carrying tab
                   state between two widgets of different kinds. */
                snprintf(hdr, sizeof(hdr), "v %s (%d)###doc_grouphdr_%s",
                         g.stem.c_str(), (int)g.members.size(), g.stem.c_str());
                if (ImGui::TabItemButton(hdr, ImGuiTabItemFlags_NoTooltip))
                    DocGroupToggle(g.stem);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("%d files named %s* -- click to fold",
                                      (int)g.members.size(), g.stem.c_str());
            }

            /* Collapsed draws one tab standing for the whole group; expanded
               draws every member the way a lone file is drawn. */
            int member_count = expanded ? (int)g.members.size() : 1;

            for (int mi = 0; mi < member_count; mi++) {
                int cur;
                bool wants_select = false;
                if (expanded) {
                    cur = g.members[(size_t)mi];
                    wants_select = (cur == select_slot);
                } else {
                    /* Whichever member is active fronts the group; with none
                       active that is the first. Picking one activates it, so
                       it becomes the front one on its own -- there is no
                       separate remembered choice to fall out of step with. */
                    cur = g.members[0];
                    for (int m : g.members)
                        if (m == active) { cur = m; break; }
                    for (int m : g.members)
                        if (m == select_slot) { wants_select = true; break; }
                    if (wants_select) {
                        for (int m : g.members)
                            if (m == select_slot) { cur = m; break; }
                    }
                }

                Document *doc = document_get(cur);
                if (!doc) continue;
                const char *base = doc->fname_s[0] ? doc->fname_s : "Untitled";

                /* `###` keys the tab on the uid alone, so the visible half is
                   free to change. With `##` the ID hashed the whole label: the
                   moment an edit added the dirty asterisk, ImGui saw the old
                   tab vanish and a brand-new one appear, dropped the selection,
                   and fell back to whichever tab it had highlighted least
                   recently -- the active document had not changed, so nothing
                   ever put the highlight back.

                   A COLLAPSED group keys on its stem instead, which is what
                   stays put as members open and close underneath it. An
                   expanded member is a file again and keys on its uid. */
                char label[160];
                if (grouped && !expanded)
                    snprintf(label, sizeof(label), "> %s%s  (%d)###doc_group_%s",
                             doc->dirty ? "* " : "", base,
                             (int)g.members.size(), g.stem.c_str());
                else
                    snprintf(label, sizeof(label), "%s%s###doc_tab_uid_%u",
                             doc->dirty ? "* " : "", base, doc->uid);

                bool open = true;
                /* Exactly one SetSelected per frame. A pending request outranks
                   the current active index; when two tabs claimed it the later
                   submission silently won. */
                bool want_select = force_select && wants_select;
                ImGuiTabItemFlags item_flags = want_select
                                             ? ImGuiTabItemFlags_SetSelected
                                             : ImGuiTabItemFlags_None;
                bool visible = ImGui::BeginTabItem(label, &open, item_flags);
                /* Record the ID ImGui assigned this tab so a drag-reorder can
                   be mapped back to the document index after EndTabBar. A
                   collapsed group records it against the member it fronts,
                   which is what the selection fallback should resolve to. */
                if (tab_bar_ptr && tab_bar_ptr->LastTabItemIdx >= 0 &&
                    tab_bar_ptr->LastTabItemIdx < tab_bar_ptr->Tabs.Size)
                    doc_tab_ids[(size_t)cur] =
                        tab_bar_ptr->Tabs[tab_bar_ptr->LastTabItemIdx].ID;
                bool activated = ImGui::IsItemActivated();
                if (activated && cur != active)
                    activate_uid = doc->uid;
                /* One click both fronts the group and opens it. A collapsed
                   group that needed some second gesture to open would just be
                   a tab that hides files. */
                if (grouped && !expanded && ImGui::IsItemClicked())
                    expand_stem = g.stem;
                if (grouped && !expanded && ImGui::IsItemHovered() &&
                    !ImGui::IsItemActive()) {
                    ImGui::BeginTooltip();
                    ImGui::Text("%d files named %s* -- click to open",
                                (int)g.members.size(), g.stem.c_str());
                    ImGui::Separator();
                    for (int m : g.members) {
                        Document *md = document_get(m);
                        if (!md) continue;
                        ImGui::Text("%s%s%s", m == active ? "> " : "   ",
                                    md->dirty ? "* " : "",
                                    md->fname_s[0] ? md->fname_s : "Untitled");
                    }
                    ImGui::EndTooltip();
                }
                if (visible)
                    ImGui::EndTabItem();
                if (!open)
                    close_uid = doc->uid;
            }
        }

        /* The World View toggles used to live here, on the document strip.
           They belong with the view that owns them, so they moved to the
           canvas view-mode tab bar — see DrawCanvasWindow. */

        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip))
            new_tab = true;
        ImGui::EndTabBar();
    }

    /* Applied here rather than inside the loop: expanding rewrites the tab
       list, and doing that while the bar is still being submitted would have
       ImGui reconciling a set of tabs that changed mid-frame. */
    if (!expand_stem.empty())
        DocGroupToggle(expand_stem);
    ImGui::PopStyleColor(3);
    g_doc_tab_select_request = -1;

    ImGui::End();
    ImGui::PopStyleVar(2);

    /* If the user dragged a tab, ImGui has reordered tab_bar->Tabs by now.
       Map that order back to document indices and apply it to the backing
       store so the new order persists. Tab IDs are uid-keyed, so ImGui's order
       and the document order agree afterwards — applying the permutation to
       both is not a double-shuffle. */
    bool reordered = false;
    if (tab_bar_ptr && !any_group &&
        (int)doc_tab_ids.size() == document_tab_count()) {
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
            if (reordered)
                document_reorder(new_order.data(), (int)new_order.size());
        }
    }

    /* Uid-keyed, so these survive the reorder above. */
    if (activate_uid) {
        int idx = document_index_of_uid(activate_uid);
        if (idx >= 0) ActivateDocumentTab(idx);
    }
    if (close_uid) {
        int idx = document_index_of_uid(close_uid);
        if (idx >= 0) RequestCloseDocumentTab(idx);
    }
    if (new_tab) {
        document_new_tab();
        g_doc_tab_select_request = document_active_index();
        ResetPerDocumentUiState(false);
    }

    /* Last line of defence: whatever ImGui is drawing as the selected tab is
       what the user sees, so the active document follows it. This catches
       selection changes we do not raise ourselves — keyboard nav onto the bar,
       ImGui picking a neighbour when the selected tab disappears — which
       otherwise leave the highlight and the canvas showing different files
       with no way back short of clicking around. Skipped on any frame we
       already drove the selection, since ImGui applies a click one frame
       later and reading it early would bounce the document straight back. */
    if (tab_bar_ptr && !force_select && !reordered && !new_tab &&
        !activate_uid && !close_uid && tab_bar_ptr->SelectedTabId != 0) {
        for (int i = 0; i < (int)doc_tab_ids.size(); i++) {
            if (doc_tab_ids[(size_t)i] != tab_bar_ptr->SelectedTabId) continue;
            if (i != document_active_index()) ActivateDocumentTab(i);
            break;
        }
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

