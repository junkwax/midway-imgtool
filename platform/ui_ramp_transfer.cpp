/*************************************************************
 * platform/ui_ramp_transfer.cpp
 * "Transfer to Palette by Ramps..." dialog.
 *
 * Glue over palette_transfer.h: split both palettes into ramp blocks with
 * material_mask.h's DetectPaletteRampBlocks, let a person confirm which
 * source ramp becomes which target ramp, preview the selected sprite before
 * and after, then rewrite every sprite in scope through the one slot map and
 * point it at the target palette. Neither palette is modified.
 *************************************************************/
#include <imgui.h>

#include "ui_ramp_transfer.h"
#include "ui_internal.h"
#include "ui_timeline.h"      /* InvalidateThumb */
#include "ui_undo.h"
#include "img_format.h"
#include "img_io.h"           /* g_restore_msg */
#include "material_mask.h"
#include "palette_transfer.h"
#include "document.h"
#include "ui_palette_preview.h"

#include <cstdio>
#include <cstring>
#include <vector>

/* ---- Session state --------------------------------------------------- */

enum RampTransferScope { ScopeSelected = 0, ScopeMarked, ScopeAll };

struct RampTransferSession {
    bool open = false;
    int src_pal = -1;
    int dst_pal = -1;
    float src_tol = 30.0f;
    float dst_tol = 30.0f;
    int mode = kPaletteTransferRelative;
    int scope = ScopeAll;

    std::vector<unsigned short> src_words, dst_words;
    std::vector<TransferBlock> src_blocks, dst_blocks;
    std::vector<int> pairs;
    std::vector<long> block_usage;   /* opaque pixels per source block, in scope */
    long unblocked_usage = 0;        /* pixels on slots outside every block */
    unsigned char map[256] = {};
    int paired_slots = 0;
    int preview_img = -1;
};

static RampTransferSession g_rt;

static bool InScope(const IMG *img, int idx)
{
    if ((int)img->palnum != g_rt.src_pal) return false;
    switch (g_rt.scope) {
    case ScopeSelected: return idx == g_doc->ilselected;
    case ScopeMarked:   return (img->flags & 1) != 0;
    default:            return true;
    }
}

static int CountInScope(void)
{
    int n = 0, idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++)
        if (InScope(img, idx)) n++;
    return n;
}

static std::vector<TransferBlock> DetectBlocks(const std::vector<unsigned short> &words, float tol)
{
    std::vector<TransferBlock> out;
    if (words.size() <= 1) return out;
    MaterialSeedParams p = MaterialSeedParamsDefault();
    p.tolerance_deg = tol;
    PaletteRampBlock blocks[64];
    int n = DetectPaletteRampBlocks(words.data(), (int)words.size(), p, blocks, 64);
    for (int i = 0; i < n; i++) out.push_back({ blocks[i].start, blocks[i].count });
    return out;
}

static void RebuildMap(void)
{
    g_rt.paired_slots = BuildPaletteTransferMap(
        g_rt.src_words.data(), (int)g_rt.src_words.size(),
        g_rt.src_blocks.data(), (int)g_rt.src_blocks.size(),
        g_rt.dst_words.data(), (int)g_rt.dst_words.size(),
        g_rt.dst_blocks.data(), (int)g_rt.dst_blocks.size(),
        g_rt.pairs.data(), g_rt.mode, g_rt.map);
}

static void RecountUsage(void)
{
    long slot_px[256] = {};
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!InScope(img, idx) || !img->data_p) continue;
        const int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) slot_px[pix[y * stride + x]]++;
    }
    bool covered[256] = {};
    g_rt.block_usage.assign(g_rt.src_blocks.size(), 0);
    for (size_t b = 0; b < g_rt.src_blocks.size(); b++) {
        const TransferBlock &blk = g_rt.src_blocks[b];
        for (int i = blk.start; i < blk.start + blk.count && i < 256; i++) {
            if (i < 1 || covered[i]) continue;
            covered[i] = true;
            g_rt.block_usage[b] += slot_px[i];
        }
    }
    g_rt.unblocked_usage = 0;
    for (int i = 1; i < 256; i++) if (!covered[i]) g_rt.unblocked_usage += slot_px[i];
}

/* Palettes or tolerances changed: re-detect blocks, re-suggest pairs. */
static void Redetect(void)
{
    g_rt.src_words = PalettePreviewWords(g_rt.src_pal >= 0 ? get_pal(g_rt.src_pal) : NULL);
    g_rt.dst_words = PalettePreviewWords(g_rt.dst_pal >= 0 ? get_pal(g_rt.dst_pal) : NULL);
    g_rt.src_blocks = DetectBlocks(g_rt.src_words, g_rt.src_tol);
    g_rt.dst_blocks = DetectBlocks(g_rt.dst_words, g_rt.dst_tol);
    g_rt.pairs.assign(g_rt.src_blocks.size(), -1);
    if (!g_rt.src_blocks.empty())
        SuggestTransferPairs(g_rt.src_words.data(), (int)g_rt.src_words.size(),
                             g_rt.src_blocks.data(), (int)g_rt.src_blocks.size(),
                             g_rt.dst_words.data(), (int)g_rt.dst_words.size(),
                             g_rt.dst_blocks.data(), (int)g_rt.dst_blocks.size(),
                             g_rt.pairs.data());
    RecountUsage();
    RebuildMap();
}

void OpenRampTransferDialog(void)
{
    IMG *sel = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!sel) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a sprite on the palette to transfer from.");
        g_restore_msg_timer = 4.0f;
        return;
    }
    RampTransferSession fresh;
    fresh.src_tol = g_rt.src_tol;   /* tolerances and mode carry across opens */
    fresh.dst_tol = g_rt.dst_tol;
    fresh.mode = g_rt.mode;
    g_rt = fresh;
    g_rt.open = true;
    g_rt.src_pal = (int)sel->palnum;
    g_rt.preview_img = g_doc->ilselected;

    int idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++)
        if (idx != g_rt.src_pal && (p->flags & 1)) { g_rt.dst_pal = idx; break; }
    if (g_rt.dst_pal < 0 && g_doc->plselected >= 0 && g_doc->plselected != g_rt.src_pal)
        g_rt.dst_pal = g_doc->plselected;

    Redetect();
}

/* ---- Commit ---------------------------------------------------------- */

static void Commit(void)
{
    if (g_rt.dst_pal < 0 || g_rt.dst_pal == g_rt.src_pal) return;
    if (CountInScope() == 0) return;
    if (!doc_undo_push()) return;

    int images = 0;
    long pixels = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (!InScope(img, idx)) continue;
        if (img->data_p && img->w > 0 && img->h > 0)
            pixels += ApplyPaletteTransferMap((unsigned char *)img->data_p, img->w, img->h,
                                              (img->w + 3) & ~3, g_rt.map);
        img->palnum = (unsigned short)g_rt.dst_pal;
        InvalidateThumb(idx);
        images++;
    }

    PAL *src = get_pal(g_rt.src_pal);
    PAL *dst = get_pal(g_rt.dst_pal);
    g_doc->plselected = g_rt.dst_pal;
    ApplyPalette(g_rt.dst_pal);
    g_img_tex_idx = -2;
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Moved %d sprite%s from %.9s to %.9s by ramps: %ld pixel%s remapped.",
             images, images == 1 ? "" : "s", src ? src->n_s : "?", dst ? dst->n_s : "?",
             pixels, pixels == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    g_rt.open = false;
}

/* ---- Dialog ------------------------------------------------------------ */

void DrawRampTransferDialog(void)
{
    static const char *kTitle = "Transfer to Palette by Ramps";
    if (g_rt.open && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    ImGui::SetNextWindowSize(ImVec2(900, 640), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal(kTitle, &g_rt.open, ImGuiWindowFlags_NoSavedSettings))
        return;

    PAL *src = get_pal(g_rt.src_pal);
    if (!src) {
        ImGui::TextUnformatted("The source palette no longer exists.");
        if (ImGui::Button("Close", ImVec2(100, 0))) { g_rt.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        return;
    }

    /* -- Palettes -- */
    ImGui::Text("From:  %.9s  (%d colors)", src->n_s, (int)g_rt.src_words.size());
    ImGui::SameLine(280.0f);
    ImGui::TextUnformatted("To:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200.0f);
    PAL *dst = g_rt.dst_pal >= 0 ? get_pal(g_rt.dst_pal) : NULL;
    char dst_label[48];
    if (dst) snprintf(dst_label, sizeof(dst_label), "%.9s (%d colors)", dst->n_s, (int)dst->numc);
    else     snprintf(dst_label, sizeof(dst_label), "(choose a palette)");
    if (ImGui::BeginCombo("##rt_dst", dst_label)) {
        int idx = 0;
        for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
            if (idx == g_rt.src_pal) continue;
            char item[48];
            snprintf(item, sizeof(item), "%.9s (%d)##rtp%d", p->n_s, (int)p->numc, idx);
            if (ImGui::Selectable(item, idx == g_rt.dst_pal)) { g_rt.dst_pal = idx; Redetect(); }
        }
        ImGui::EndCombo();
    }

    /* -- Options -- */
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Source split", &g_rt.src_tol, 5.0f, 90.0f, "%.0f deg")) Redetect();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "How far a color's hue may turn before a new ramp starts.\n"
        "Lower splits ramps apart; higher merges them.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Target split", &g_rt.dst_tol, 5.0f, 90.0f, "%.0f deg")) Redetect();

    ImGui::TextUnformatted("Shading:");
    ImGui::SameLine();
    if (ImGui::RadioButton("Relative", g_rt.mode == kPaletteTransferRelative)) { g_rt.mode = kPaletteTransferRelative; RebuildMap(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Darkest shade maps to the target ramp's darkest, lightest to lightest,\n"
        "everything in between spread proportionally. Best when the two games\n"
        "light the material differently.");
    ImGui::SameLine();
    if (ImGui::RadioButton("Match brightness", g_rt.mode == kPaletteTransferLuma)) { g_rt.mode = kPaletteTransferLuma; RebuildMap(); }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Each shade takes the target ramp color closest in brightness.\n"
        "Best when both ramps cover the same light-to-dark range.");
    ImGui::SameLine(0.0f, 30.0f);
    ImGui::TextUnformatted("Apply to:");
    ImGui::SameLine();
    int old_scope = g_rt.scope;
    ImGui::RadioButton("Selected", &g_rt.scope, ScopeSelected); ImGui::SameLine();
    ImGui::RadioButton("Marked", &g_rt.scope, ScopeMarked);     ImGui::SameLine();
    ImGui::RadioButton("All on source", &g_rt.scope, ScopeAll);
    if (g_rt.scope != old_scope) RecountUsage();

    ImGui::Separator();

    /* -- Ramp pairing table + preview side by side -- */
    const float kPreviewW = 300.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##rt_table", ImVec2(-kPreviewW, -footer), true);
    if (!dst) {
        ImGui::TextDisabled("Pick a target palette. Marking it before opening preselects it.");
    } else if (g_rt.src_blocks.empty()) {
        ImGui::TextDisabled("No ramps found in the source palette.");
    } else if (ImGui::BeginTable("##rt_pairs", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("Source ramp", ImGuiTableColumnFlags_WidthFixed, 24 * 11.0f + 8);
        ImGui::TableSetupColumn("Pixels", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Target ramp", ImGuiTableColumnFlags_WidthFixed, 110);
        ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (size_t b = 0; b < g_rt.src_blocks.size(); b++) {
            const TransferBlock &sb = g_rt.src_blocks[b];
            const bool unused = g_rt.block_usage[b] == 0;
            ImGui::PushID((int)b);
            ImGui::TableNextRow();

            ImGui::TableNextColumn();
            if (unused) ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.45f);
            ImGui::Text("Slots %d-%d", sb.start, sb.start + sb.count - 1);
            PalettePreviewStrip(g_rt.src_words, sb.start, sb.count);

            ImGui::TableNextColumn();
            ImGui::Text("%ld", g_rt.block_usage[b]);
            if (unused) ImGui::PopStyleVar();

            ImGui::TableNextColumn();
            int cur = g_rt.pairs[b];
            char cur_label[32];
            if (cur >= 0 && cur < (int)g_rt.dst_blocks.size())
                snprintf(cur_label, sizeof(cur_label), "%d-%d", g_rt.dst_blocks[cur].start,
                         g_rt.dst_blocks[cur].start + g_rt.dst_blocks[cur].count - 1);
            else
                snprintf(cur_label, sizeof(cur_label), "Nearest color");
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##pair", cur_label, ImGuiComboFlags_HeightLarge)) {
                if (ImGui::Selectable("Nearest color (any ramp)", cur < 0)) { g_rt.pairs[b] = -1; RebuildMap(); }
                for (size_t d = 0; d < g_rt.dst_blocks.size(); d++) {
                    const TransferBlock &db = g_rt.dst_blocks[d];
                    char item[32];
                    snprintf(item, sizeof(item), "%3d-%-3d##d%d", db.start, db.start + db.count - 1, (int)d);
                    if (ImGui::Selectable(item, cur == (int)d, 0, ImVec2(70, 14))) {
                        g_rt.pairs[b] = (int)d;
                        RebuildMap();
                    }
                    ImGui::SameLine();
                    PalettePreviewStrip(g_rt.dst_words, db.start, db.count);
                }
                ImGui::EndCombo();
            }
            if (cur >= 0 && cur < (int)g_rt.dst_blocks.size())
                PalettePreviewStrip(g_rt.dst_words, g_rt.dst_blocks[cur].start, g_rt.dst_blocks[cur].count);

            ImGui::TableNextColumn();
            ImGui::TextDisabled("each source slot becomes");
            PalettePreviewStrip(g_rt.src_words, sb.start, sb.count, g_rt.map, &g_rt.dst_words);
            ImGui::PopID();
        }
        ImGui::EndTable();
        if (g_rt.unblocked_usage > 0)
            ImGui::TextDisabled("%ld pixel(s) on slots outside any ramp use the nearest color.",
                                g_rt.unblocked_usage);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##rt_preview", ImVec2(0, -footer), true);
    IMG *pv = g_rt.preview_img >= 0 ? get_img(g_rt.preview_img) : NULL;
    if (pv && (int)pv->palnum == g_rt.src_pal) {
        const float pane_h = (ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2) * 0.5f;
        const float pane_w = ImGui::GetContentRegionAvail().x;
        PalettePreviewSprite(0, "Before", pv, g_rt.src_words, nullptr, pane_w, pane_h);
        if (dst) PalettePreviewSprite(1, "After", pv, g_rt.dst_words, g_rt.map, pane_w, pane_h);
    } else {
        ImGui::TextDisabled("Preview needs the selected\nsprite on the source palette.");
    }
    ImGui::EndChild();

    /* -- Footer -- */
    const int in_scope = CountInScope();
    ImGui::Text("%d sprite%s will be remapped and pointed at %.9s. Neither palette is changed.",
                in_scope, in_scope == 1 ? "" : "s", dst ? dst->n_s : "the target");
    ImGui::BeginDisabled(!dst || in_scope == 0);
    if (ImGui::Button("Apply", ImVec2(120, 0))) Commit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) g_rt.open = false;
    if (!g_rt.open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
