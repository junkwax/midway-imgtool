/*************************************************************
 * platform/ui_stamp_erase.cpp
 *
 * "Erase Copied Object" — copy an object once, then subtract it from every
 * frame it was composited into.
 *
 * The workflow this exists for: UMK3FIRE.IMG holds UMK3SKEL18, a clean
 * skeleton, and UMK3SKEL4..17, the same skeleton with a flame painted over it.
 * Copy the clean frame, open this dialog, and each flame frame is searched for
 * the skeleton's own pixels — no anipoint bookkeeping, no lining anything up —
 * and the skeleton is subtracted wherever the two agree exactly. Where the
 * flame covers it the indices differ, so those pixels stay and the flame comes
 * out whole.
 *
 * The matching itself is in platform/stamp_erase.{h,cpp} and is unit-tested;
 * this file is the dialog around it: scope, scoring thresholds, the per-frame
 * report, and the single undoable apply.
 *************************************************************/
#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include "ui_stamp_erase.h"
#include "ui_internal.h"
#include "ui_timeline.h"
#include "img_format.h"
#include "img_io.h"
#include "stamp_erase.h"
#include "compat.h"

#include <string>
#include <vector>

/* ---- Dialog state ---- */

enum StampScope {
    StampScope_Marked = 0,
    StampScope_All,
    StampScope_Selected,
};

enum StampAlign {
    StampAlign_Auto = 0,     /* search the whole frame by content */
    StampAlign_NearAnipoint, /* search a window around the anipoint placement */
    StampAlign_Anipoint,     /* trust the anipoints exactly */
};

struct StampEraseRow {
    int            img_idx;
    std::string    name;
    StampMatch     match;
    bool           enabled;
    bool           palette_differs;
    unsigned short palnum;
};

static bool s_open = false;
static int  s_scope = StampScope_Marked;
static int  s_align = StampAlign_Auto;
static int  s_radius = 12;
/* Percent of the object that must be found before a frame is ticked. 20 rather
   than something confident-sounding because real composited art does not score
   in the nineties: measured against UMK3FIRE, the frames that genuinely hold
   the skeleton land at 21-35% (the flame recolors most of it) while frames
   that do not hold it sit under 12%. The gap is what identifies a hit, not the
   absolute number. */
static int  s_threshold = 20;
static bool s_skip_source = true;
static bool s_same_palette_only = true;

static std::vector<StampEraseRow> s_rows;
static bool s_dirty_scan = true;   /* parameters changed; rescan before drawing */
static int  s_scanned_frames = 0;

/* Parameters that change what the scan finds. The threshold is deliberately
   not in here: it only re-sorts frames into "found" and "not found", which is
   a filter over results we already have. */
struct StampScanKey {
    int scope, align, radius;
    int skip_source, same_palette_only;
    int doc_uid, clipboard_w, clipboard_h;
};
static StampScanKey s_scan_key = {};

static bool ScanKeyEqual(const StampScanKey &a, const StampScanKey &b)
{
    return a.scope == b.scope && a.align == b.align && a.radius == b.radius &&
           a.skip_source == b.skip_source &&
           a.same_palette_only == b.same_palette_only &&
           a.doc_uid == b.doc_uid &&
           a.clipboard_w == b.clipboard_w && a.clipboard_h == b.clipboard_h;
}

bool StampEraseAvailable(void)
{
    return g_clipboard.valid && g_clipboard.data_p &&
           g_clipboard.w > 0 && g_clipboard.h > 0 && g_clipboard.has_opaque;
}

static StampBuf ClipboardStamp(void)
{
    StampBuf b;
    b.pixels = (const unsigned char *)g_clipboard.data_p;
    b.w = (int)g_clipboard.w;
    b.h = (int)g_clipboard.h;
    b.stride = (int)g_clipboard.stride;
    return b;
}

static StampBuf ImageStamp(const IMG *img)
{
    StampBuf b;
    b.pixels = (const unsigned char *)img->data_p;
    b.w = (int)img->w;
    b.h = (int)img->h;
    b.stride = ((int)img->w + 3) & ~3;
    return b;
}

/* Where the anipoints say the clipboard's content lands in this frame. The
   clipboard remembers its source's anipoint and where the copied art sat
   relative to it, so the same relationship reproduced against this frame's
   anipoint is the no-search answer. */
static void AnipointPlacement(const IMG *img, int *out_dx, int *out_dy)
{
    int rel_x = g_clipboard.origin_x - (int)(short)g_clipboard.anix;
    int rel_y = g_clipboard.origin_y - (int)(short)g_clipboard.aniy;
    *out_dx = (int)(short)img->anix + rel_x;
    *out_dy = (int)(short)img->aniy + rel_y;
}

static bool FrameInScope(const IMG *img, int idx)
{
    switch (s_scope) {
        case StampScope_Marked:   return (img->flags & 1) != 0;
        case StampScope_Selected: return idx == g_doc->ilselected;
        case StampScope_All:      default: return true;
    }
}

static void RunScan(void)
{
    s_rows.clear();
    s_scanned_frames = 0;
    if (!StampEraseAvailable() || !g_doc) return;

    StampBuf stamp = ClipboardStamp();
    int total = StampOpaqueCount(stamp);
    if (total <= 0) return;

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!img->data_p || img->w == 0 || img->h == 0) continue;
        if (!FrameInScope(img, idx)) continue;

        char name[16];
        strncpy(name, img->n_s, 15);
        name[15] = '\0';

        if (s_skip_source && g_clipboard.source_name[0] &&
            strncmp(name, g_clipboard.source_name, 15) == 0)
            continue;

        bool pal_differs = (img->palnum != g_clipboard.palnum);
        if (s_same_palette_only && pal_differs) continue;

        StampBuf target = ImageStamp(img);
        StampMatch m;
        if (s_align == StampAlign_Auto) {
            m = StampSearchAuto(stamp, target, 0);
        } else {
            int dx, dy;
            AnipointPlacement(img, &dx, &dy);
            m = (s_align == StampAlign_NearAnipoint)
                    ? StampSearchWindow(stamp, target, dx, dy, s_radius)
                    : StampScoreAt(stamp, target, dx, dy);
        }

        s_scanned_frames++;
        if (!m.valid || m.matched <= 0) continue;

        StampEraseRow row;
        row.img_idx = idx;
        row.name = name;
        row.match = m;
        row.enabled = false;
        row.palette_differs = pal_differs;
        row.palnum = img->palnum;
        s_rows.push_back(row);
    }
}

/* Tick the rows the threshold says hold the object. Runs after a scan and
   whenever the threshold moves, so the slider stays a live filter. */
static void ApplyThresholdToRows(void)
{
    float bar = (float)s_threshold / 100.0f;
    for (size_t i = 0; i < s_rows.size(); i++)
        s_rows[i].enabled = StampMatchScore(s_rows[i].match) >= bar;
}

void OpenStampEraseDialog(void)
{
    if (!StampEraseAvailable()) return;
    if (CountMarkedImages() == 0 && s_scope == StampScope_Marked)
        s_scope = StampScope_All;
    s_rows.clear();
    s_dirty_scan = true;
    s_open = true;
}

static int ApplyStampErase(int *out_frames)
{
    int frames = 0, pixels = 0;
    bool pushed = false;
    StampBuf stamp = ClipboardStamp();

    for (size_t i = 0; i < s_rows.size(); i++) {
        const StampEraseRow &row = s_rows[i];
        if (!row.enabled) continue;
        IMG *img = get_img(row.img_idx);
        if (!img || !img->data_p) continue;

        if (!pushed) {
            /* One snapshot for the whole sweep: this is a single edit as far
               as the user is concerned, and per-frame undo steps would make
               backing it out a chore. */
            doc_undo_push();
            pushed = true;
        }

        int stride = ((int)img->w + 3) & ~3;
        int cleared = StampEraseAt((unsigned char *)img->data_p,
                                   (int)img->w, (int)img->h, stride,
                                   stamp, row.match.dx, row.match.dy);
        if (cleared <= 0) continue;
        pixels += cleared;
        frames++;
        InvalidateThumb(row.img_idx);
    }

    if (frames > 0) {
        mark_dirty();
        g_img_tex_idx = -2;
    }
    if (out_frames) *out_frames = frames;
    return pixels;
}

void DrawStampEraseDialog(void)
{
    if (s_open) {
        ImGui::OpenPopup("Erase Copied Object");
        CenterNextModal();
    }
    if (!ImGui::BeginPopupModal("Erase Copied Object", &s_open,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        if (!s_open) s_rows.clear();
        return;
    }
    if (!StampEraseAvailable()) {
        ImGui::TextUnformatted("The clipboard is empty. Copy the object first (Ctrl+C).");
        if (ImGui::Button("Close", ImVec2(90, 0))) { s_open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        return;
    }

    StampBuf stamp = ClipboardStamp();
    int stamp_opaque = StampOpaqueCount(stamp);

    ImGui::Text("Object: %.15s   %dx%d   %d opaque pixel%s",
                g_clipboard.source_name[0] ? g_clipboard.source_name : "clipboard",
                (int)g_clipboard.w, (int)g_clipboard.h,
                stamp_opaque, stamp_opaque == 1 ? "" : "s");
    ImGui::TextDisabled("Found by its own pixels, then subtracted only where the frame\n"
                        "agrees exactly — anything painted over it is left behind.");
    ImGui::Separator();

    bool changed = false;

    ImGui::TextUnformatted("Search");
    ImGui::SameLine(110.0f);
    int marked = CountMarkedImages();
    char marked_label[48];
    snprintf(marked_label, sizeof(marked_label), "Marked (%d)", marked);
    changed |= ImGui::RadioButton(marked_label, &s_scope, StampScope_Marked);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("All frames", &s_scope, StampScope_All);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Selected only", &s_scope, StampScope_Selected);

    ImGui::TextUnformatted("Locate by");
    ImGui::SameLine(110.0f);
    changed |= ImGui::RadioButton("Pixels (auto)", &s_align, StampAlign_Auto);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Finds the object anywhere in the frame with no hint at all: pixels\n"
        "from all over it vote on where it would have to be, the winning\n"
        "placements are scored properly, and the best is walked to its exact\n"
        "position. Survives most of the object being painted over, and does\n"
        "not care that the frames are different sizes.");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Near anipoint", &s_align, StampAlign_NearAnipoint);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Starts from where the anipoints put the object and searches outward.\n"
        "Use when the art repeats and the auto search picks the wrong copy.");
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Anipoint exactly", &s_align, StampAlign_Anipoint);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "No search at all: the object is placed where the anipoints say it goes.");

    if (s_align == StampAlign_NearAnipoint) {
        ImGui::SetNextItemWidth(200.0f);
        changed |= ImGui::SliderInt("Search radius (px)", &s_radius, 1, 64);
    }

    changed |= ImGui::Checkbox("Skip the frame it was copied from", &s_skip_source);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "The source frame matches itself perfectly and would be wiped clean.\n"
        "Matched on sprite name (%.15s).",
        g_clipboard.source_name[0] ? g_clipboard.source_name : "unnamed");
    changed |= ImGui::Checkbox("Same palette only", &s_same_palette_only);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Pixel indices only mean the same color within one palette. Frames on\n"
        "another palette are skipped unless you turn this off.");

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::SliderInt("Counts as found at", &s_threshold, 1, 100, "%d%%"))
        ApplyThresholdToRows();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "How much of the object has to survive in a frame before it is ticked.\n"
        "Do not expect high numbers: heavy compositing recolors most of an\n"
        "object, so a frame that genuinely holds it often scores 20-35%% while\n"
        "one that does not sits in the single digits. It is the gap that\n"
        "identifies a hit. Erasing is per-pixel either way — a low score means\n"
        "less is removed, never that the wrong thing is.");

    StampScanKey key;
    key.scope = s_scope;
    key.align = s_align;
    key.radius = s_radius;
    key.skip_source = s_skip_source ? 1 : 0;
    key.same_palette_only = s_same_palette_only ? 1 : 0;
    key.doc_uid = (int)document_uid(document_active_index());
    key.clipboard_w = (int)g_clipboard.w;
    key.clipboard_h = (int)g_clipboard.h;
    if (changed || s_dirty_scan || !ScanKeyEqual(key, s_scan_key)) {
        s_scan_key = key;
        s_dirty_scan = false;
        RunScan();
        ApplyThresholdToRows();
    }

    ImGui::Separator();

    int ticked = 0, ticked_pixels = 0;
    for (size_t i = 0; i < s_rows.size(); i++) {
        if (!s_rows[i].enabled) continue;
        ticked++;
        ticked_pixels += s_rows[i].match.matched;
    }

    ImGui::Text("%d of %d searched frame%s hold the object; %d ticked (%d pixel%s).",
                (int)s_rows.size(), s_scanned_frames,
                s_scanned_frames == 1 ? "" : "s",
                ticked, ticked_pixels, ticked_pixels == 1 ? "" : "s");

    if (ImGui::SmallButton("Tick all")) {
        for (size_t i = 0; i < s_rows.size(); i++) s_rows[i].enabled = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Tick none")) {
        for (size_t i = 0; i < s_rows.size(); i++) s_rows[i].enabled = false;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Reset to threshold")) ApplyThresholdToRows();

    ImVec2 table_size(560.0f, 260.0f);
    if (ImGui::BeginTable("##stamp_erase_rows", 5,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_ScrollY, table_size)) {
        ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Found", ImGuiTableColumnFlags_WidthFixed, 64.0f);
        ImGui::TableSetupColumn("At", ImGuiTableColumnFlags_WidthFixed, 88.0f);
        ImGui::TableSetupColumn("Erases", ImGuiTableColumnFlags_WidthFixed, 66.0f);
        ImGui::TableSetupColumn("Covered", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < s_rows.size(); i++) {
            StampEraseRow &row = s_rows[i];
            float score = StampMatchScore(row.match);
            ImGui::TableNextRow();
            ImGui::PushID((int)i);

            ImGui::TableNextColumn();
            ImGui::Checkbox("##on", &row.enabled);
            ImGui::SameLine();
            /* Clicking the name walks the editor to that frame so the match
               can be eyeballed before committing to it. */
            if (ImGui::Selectable(row.name.c_str(), row.img_idx == g_doc->ilselected,
                                  ImGuiSelectableFlags_None)) {
                g_doc->ilselected = row.img_idx;
                g_img_tex_idx = -2;
                g_zoom_reset = true;
            }
            if (row.palette_differs && ImGui::IsItemHovered())
                ImGui::SetTooltip("This frame is on a different palette (%d vs %d),\n"
                                  "so identical indices may not be identical colors.",
                                  (int)row.palnum, (int)g_clipboard.palnum);

            ImGui::TableNextColumn();
            ImVec4 tint = score >= 0.999f ? ImVec4(0.45f, 1.00f, 0.45f, 1.0f)
                        : score >= (float)s_threshold / 100.0f
                                    ? ImVec4(0.60f, 0.85f, 0.60f, 1.0f)
                                    : ImVec4(0.95f, 0.65f, 0.35f, 1.0f);
            ImGui::TextColored(tint, "%.0f%%", score * 100.0f);

            ImGui::TableNextColumn();
            ImGui::Text("%d, %d", row.match.dx, row.match.dy);

            ImGui::TableNextColumn();
            ImGui::Text("%d", row.match.matched);

            ImGui::TableNextColumn();
            /* Opaque object pixels that fell inside this frame at all. Short
               of the total means the object hangs off an edge here. */
            ImGui::Text("%d/%d", row.match.covered, row.match.total);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }

    if (s_rows.empty()) {
        ImGui::TextDisabled("%s", s_scanned_frames == 0
            ? "No frames in scope. Mark some sprites, or switch to All frames."
            : "The object was not found in any of them.");
    }

    ImGui::Separator();
    ImGui::BeginDisabled(ticked == 0);
    char apply_label[64];
    snprintf(apply_label, sizeof(apply_label), "Erase from %d frame%s",
             ticked, ticked == 1 ? "" : "s");
    if (ImGui::Button(apply_label, ImVec2(180, 0))) {
        int frames = 0;
        int pixels = ApplyStampErase(&frames);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 frames > 0
                     ? "Erased the copied object from %d frame%s (%d pixel%s cleared)."
                     : "Nothing was erased; the matched pixels had already changed.",
                 frames, frames == 1 ? "" : "s", pixels, pixels == 1 ? "" : "s");
        g_restore_msg_timer = 5.0f;
        s_rows.clear();
        s_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(90, 0))) {
        s_rows.clear();
        s_open = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}
