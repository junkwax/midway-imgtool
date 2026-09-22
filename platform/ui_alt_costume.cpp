/*************************************************************
 * platform/ui_alt_costume.cpp
 * "Alternate Costume..." dialog.
 *
 * RADRED_P and RADBLU_P are identical at 46 of 64 slots: the alternate
 * costume is the same palette with the cloth ramp swapped (palette_ramp.h's
 * header has the numbers). This dialog is that operation. Split the palette
 * into ramps (material_mask.h's DetectPaletteRampBlocks), tick the ones to
 * recolor, pick a color, and ColorizeRamp rebuilds only those slots
 * with each shade's brightness kept. The result is a NEW palette; the
 * source and every sprite's pixels are left alone, optionally repointing
 * some sprites at the new one.
 *************************************************************/
#include <imgui.h>

#include "ui_alt_costume.h"
#include "ui_internal.h"
#include "ui_palette.h"        /* save_palette_baseline */
#include "ui_palette_preview.h"
#include "ui_timeline.h"       /* InvalidateThumb */
#include "ui_undo.h"
#include "img_format.h"
#include "img_io.h"            /* g_restore_msg */
#include "material_mask.h"
#include "palette_ramp.h"
#include "document.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

enum AltCostumeRepoint { RepointNone = 0, RepointSelected, RepointMarked, RepointAll };

struct AltCostumeSession {
    bool open = false;
    int src_pal = -1;
    float tol = 30.0f;
    float color[3] = { 0.15f, 0.30f, 0.85f };
    float amount = 1.0f;
    int repoint = RepointNone;
    char name[10] = "";

    std::vector<unsigned short> words;      /* source palette */
    std::vector<unsigned short> out_words;  /* source with picked ramps recolored */
    std::vector<PaletteRampBlock> blocks;
    std::vector<char> picked;               /* one per block */
    std::vector<long> usage;                /* pixels per block, all sprites on source */
    int preview_img = -1;
};

static AltCostumeSession g_ac;

static void Recolor(void)
{
    g_ac.out_words = g_ac.words;
    const unsigned char r = (unsigned char)std::lround(g_ac.color[0] * 255.0f);
    const unsigned char g = (unsigned char)std::lround(g_ac.color[1] * 255.0f);
    const unsigned char b = (unsigned char)std::lround(g_ac.color[2] * 255.0f);
    for (size_t i = 0; i < g_ac.blocks.size(); i++) {
        if (!g_ac.picked[i]) continue;
        const PaletteRampBlock &blk = g_ac.blocks[i];
        for (int k = 0; k < blk.count; k++) {
            const int slot = blk.start + k;
            if (slot < 1 || slot >= (int)g_ac.out_words.size()) continue;
            const unsigned short w = g_ac.out_words[slot];
            RampColor c = { (unsigned char)((w >> 10) & 0x1F), (unsigned char)((w >> 5) & 0x1F),
                            (unsigned char)(w & 0x1F) };
            ColorizeRamp(&c, 1, r, g, b, g_ac.amount, &c);
            g_ac.out_words[slot] = RampColorWord(c);
        }
    }
}

static void Redetect(void)
{
    g_ac.blocks.clear();
    if (g_ac.words.size() > 1) {
        MaterialSeedParams p = MaterialSeedParamsDefault();
        p.tolerance_deg = g_ac.tol;
        PaletteRampBlock buf[64];
        const int n = DetectPaletteRampBlocks(g_ac.words.data(), (int)g_ac.words.size(), p, buf, 64);
        g_ac.blocks.assign(buf, buf + n);
    }
    g_ac.picked.assign(g_ac.blocks.size(), 0);

    long slot_px[256] = {};
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if ((int)img->palnum != g_ac.src_pal || !img->data_p) continue;
        const int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) slot_px[pix[y * stride + x]]++;
    }
    g_ac.usage.assign(g_ac.blocks.size(), 0);
    for (size_t i = 0; i < g_ac.blocks.size(); i++)
        for (int k = 0; k < g_ac.blocks[i].count; k++) {
            const int slot = g_ac.blocks[i].start + k;
            if (slot >= 1 && slot < 256) g_ac.usage[i] += slot_px[slot];
        }
    Recolor();
}

void OpenAltCostumeDialog(void)
{
    int pal = g_doc->plselected;
    if (pal < 0 && g_doc->ilselected >= 0) {
        IMG *sel = get_img(g_doc->ilselected);
        if (sel) pal = (int)sel->palnum;
    }
    PAL *src = pal >= 0 ? get_pal(pal) : NULL;
    if (!src || !src->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select the palette to make a costume from.");
        g_restore_msg_timer = 4.0f;
        return;
    }
    AltCostumeSession fresh;
    fresh.tol = g_ac.tol;             /* tolerance and color carry across opens */
    std::memcpy(fresh.color, g_ac.color, sizeof(fresh.color));
    fresh.amount = g_ac.amount;
    g_ac = fresh;
    g_ac.open = true;
    g_ac.src_pal = pal;
    g_ac.words = PalettePreviewWords(src);
    snprintf(g_ac.name, sizeof(g_ac.name), "%.7s_A", src->n_s);

    /* Preview the selected sprite when it is on this palette, else the first
       one that is. */
    int idx = 0;
    IMG *sel = g_doc->ilselected >= 0 ? get_img(g_doc->ilselected) : NULL;
    if (sel && (int)sel->palnum == pal) g_ac.preview_img = g_doc->ilselected;
    else
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++)
            if ((int)img->palnum == pal) { g_ac.preview_img = idx; break; }

    Redetect();
}

static void Commit(void)
{
    PAL *src = get_pal(g_ac.src_pal);
    if (!src || g_ac.out_words.empty()) return;
    if (!doc_undo_push()) return;

    PAL *pal = AllocPal();
    if (!pal) return;
    pal->flags   = 0;
    pal->bitspix = src->bitspix;
    pal->numc    = src->numc;
    pal->pad     = 0;
    snprintf(pal->n_s, sizeof(pal->n_s), "%.9s", g_ac.name[0] ? g_ac.name : "COSTUME");
    const size_t bytes = (size_t)src->numc * 2;
    unsigned char *buf = (unsigned char *)PoolAlloc(bytes);
    if (!buf) return;
    std::memcpy(buf, src->data_p, bytes);   /* slots past 256 (never drawn) copied verbatim */
    for (size_t i = 0; i < g_ac.out_words.size(); i++) {
        buf[i*2+0] = (unsigned char)(g_ac.out_words[i] & 0xFF);
        buf[i*2+1] = (unsigned char)(g_ac.out_words[i] >> 8);
    }
    pal->data_p = buf;
    const int new_idx = (int)g_doc->palcnt - 1;

    int repointed = 0, idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if ((int)img->palnum != g_ac.src_pal) continue;
        bool take = g_ac.repoint == RepointAll ||
                    (g_ac.repoint == RepointMarked && (img->flags & 1)) ||
                    (g_ac.repoint == RepointSelected && idx == g_doc->ilselected);
        if (!take) continue;
        img->palnum = (unsigned short)new_idx;
        InvalidateThumb(idx);
        repointed++;
    }

    g_doc->plselected = new_idx;
    ApplyPalette(new_idx);
    save_palette_baseline();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg), "Created costume palette %.9s from %.9s%s",
             pal->n_s, src->n_s, repointed ? "; sprites repointed." : ".");
    g_restore_msg_timer = 5.0f;
    g_ac.open = false;
}

void DrawAltCostumeDialog(void)
{
    static const char *kTitle = "Alternate Costume";
    if (g_ac.open && !ImGui::IsPopupOpen(kTitle)) ImGui::OpenPopup(kTitle);
    ImGui::SetNextWindowSize(ImVec2(860, 600), ImGuiCond_Once);
    if (!ImGui::BeginPopupModal(kTitle, &g_ac.open, ImGuiWindowFlags_NoSavedSettings))
        return;

    PAL *src = get_pal(g_ac.src_pal);
    if (!src) {
        ImGui::TextUnformatted("The source palette no longer exists.");
        if (ImGui::Button("Close", ImVec2(100, 0))) { g_ac.open = false; ImGui::CloseCurrentPopup(); }
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("From:  %.9s  (%d colors)", src->n_s, (int)g_ac.words.size());
    ImGui::SameLine(280.0f);
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("New palette name", g_ac.name, sizeof(g_ac.name), ImGuiInputTextFlags_CharsUppercase);

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Ramp split", &g_ac.tol, 5.0f, 90.0f, "%.0f deg")) Redetect();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "How far a color's hue may turn before a new ramp starts.\n"
        "Lower if the cloth is merged with the skin; higher if one\n"
        "material is split in two.");

    ImGui::Separator();

    const float kSide = 300.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##ac_left", ImVec2(-kSide, -footer), true);
    bool changed = false;
    ImGui::TextUnformatted("New color");
    changed |= ImGui::ColorEdit3("##ac_color", g_ac.color, ImGuiColorEditFlags_PickerHueWheel);
    ImGui::SetNextItemWidth(200.0f);
    changed |= ImGui::SliderFloat("Amount", &g_ac.amount, 0.0f, 1.0f, "%.2f");
    ImGui::TextDisabled("Tick the ramps to recolor. Each shade keeps its brightness.");

    if (ImGui::BeginTable("##ac_blocks", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
        ImGui::TableSetupColumn("Ramp", ImGuiTableColumnFlags_WidthFixed, 24 * 11.0f + 8);
        ImGui::TableSetupColumn("Pixels", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Becomes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < g_ac.blocks.size(); i++) {
            const PaletteRampBlock &blk = g_ac.blocks[i];
            ImGui::PushID((int)i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            bool on = g_ac.picked[i] != 0;
            if (ImGui::Checkbox("##pick", &on)) { g_ac.picked[i] = on ? 1 : 0; changed = true; }
            ImGui::TableNextColumn();
            ImGui::Text("Slots %d-%d", blk.start, blk.start + blk.count - 1);
            PalettePreviewStrip(g_ac.words, blk.start, blk.count);
            ImGui::TableNextColumn();
            ImGui::Text("%ld", g_ac.usage[i]);
            ImGui::TableNextColumn();
            if (g_ac.picked[i]) {
                ImGui::TextDisabled(" ");
                PalettePreviewStrip(g_ac.out_words, blk.start, blk.count);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (changed) Recolor();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##ac_preview", ImVec2(0, -footer), true);
    IMG *pv = g_ac.preview_img >= 0 ? get_img(g_ac.preview_img) : NULL;
    if (pv && (int)pv->palnum == g_ac.src_pal) {
        const float pane_h = (ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * 2) * 0.5f;
        const float pane_w = ImGui::GetContentRegionAvail().x;
        PalettePreviewSprite(2, "Before", pv, g_ac.words, nullptr, pane_w, pane_h);
        PalettePreviewSprite(3, "After", pv, g_ac.out_words, nullptr, pane_w, pane_h);
    } else {
        ImGui::TextDisabled("No sprite uses this palette\nto preview on.");
    }
    ImGui::EndChild();

    ImGui::TextUnformatted("Point at the new palette:");
    ImGui::SameLine();
    ImGui::RadioButton("None", &g_ac.repoint, RepointNone);         ImGui::SameLine();
    ImGui::RadioButton("Selected", &g_ac.repoint, RepointSelected); ImGui::SameLine();
    ImGui::RadioButton("Marked", &g_ac.repoint, RepointMarked);     ImGui::SameLine();
    ImGui::RadioButton("All on source", &g_ac.repoint, RepointAll);
    bool any_picked = false;
    for (char p : g_ac.picked) any_picked |= p != 0;
    ImGui::BeginDisabled(!any_picked);
    if (ImGui::Button("Create Palette", ImVec2(140, 0))) Commit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) g_ac.open = false;
    if (!g_ac.open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
