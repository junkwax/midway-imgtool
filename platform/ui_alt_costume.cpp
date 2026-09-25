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
 *
 * "Borrow from palette" is the other color source: the recolored slots take
 * another palette's costume ramp, matched shade-for-shade by pixel coverage
 * (palette_transfer.h's BorrowRampByCoverage). This is how UMK3's RAIN1_P
 * becomes an MK2 ninja palette on NINJAS8's sprites. Both slot ranges are
 * pre-filled by diffing each palette against its closest costume sibling.
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
#include "palette_transfer.h"
#include "document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

enum AltCostumeRepoint { RepointNone = 0, RepointSelected, RepointMarked, RepointAll };
enum AltCostumeMode { ModeTint = 0, ModeBorrow };

/* One side of a borrow: a palette, the slot range holding its costume, and
   how many pixels each of those slots covers across it and its siblings. */
struct BorrowSide {
    int first = 1, last = 1;
    std::vector<double> weight;             /* one per slot in [first, last] */
    long pixels = 0;
    int siblings = 0;                       /* palettes pooled besides this one */
    int preview_img = -1;                   /* a sprite drawn through this layout */
};

struct AltCostumeSession {
    bool open = false;
    int src_pal = -1;
    float tol = 30.0f;
    float color[3] = { 0.15f, 0.30f, 0.85f };
    float amount = 1.0f;
    int repoint = RepointNone;
    char name[10] = "";
    int mode = ModeTint;

    std::vector<unsigned short> words;      /* source palette */
    std::vector<unsigned short> out_words;  /* source with picked ramps recolored */
    std::vector<PaletteRampBlock> blocks;
    std::vector<char> picked;               /* one per block */
    std::vector<long> usage;                /* pixels per block, all sprites on source */
    int preview_img = -1;

    int ref_pal = -1;                       /* Borrow: palette the colors come from */
    std::vector<unsigned short> ref_words;
    bool weigh = true;                      /* match by coverage, else by rank */
    BorrowSide dst, ref;
};

static AltCostumeSession g_ac;

/* ---- Borrow from palette ---- */

static std::vector<unsigned short> WordsOf(int pal)
{
    return PalettePreviewWords(pal >= 0 ? get_pal(pal) : NULL);
}

/* Pre-fill a side's range with the costume ramp: the slots where `pal`
   differs from its costume variants. The variants are the palettes (same
   color count) sharing at least 3/4 as many slots as the closest one does. */
static void SuggestRange(int pal, const std::vector<unsigned short> &words, BorrowSide &side)
{
    side.first = 1;
    side.last = (int)words.size() - 1;
    std::vector<std::vector<unsigned short>> cand;
    std::vector<int> shared;
    int best = 0, idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
        if (idx == pal || !p->data_p) continue;
        std::vector<unsigned short> w = PalettePreviewWords(p);
        if (w.size() != words.size()) continue;
        const int n = CountSharedSlots(words.data(), w.data(), (int)w.size());
        if (n <= 0) continue;
        best = std::max(best, n);
        cand.push_back(std::move(w));
        shared.push_back(n);
    }
    std::vector<const unsigned short *> sibs;
    for (size_t i = 0; i < cand.size(); i++)
        if (shared[i] * 4 >= best * 3) sibs.push_back(cand[i].data());
    int f, l;
    if (!sibs.empty() &&
        FindCostumeRun(words.data(), sibs.data(), (int)sibs.size(), (int)words.size(), &f, &l)) {
        side.first = f;
        side.last = l;
    }
}

/* Pixel coverage of each slot in the side's range, over every sprite on
   `pal` or on a costume sibling of it — RAIN1_P has two sprites of its own,
   but the UMK3 ninja sprites on SCORP1_P, REP1_P... all show where its gear
   shades land. Also picks the largest such sprite to preview. */
static void MeasureSide(int pal, const std::vector<unsigned short> &words, BorrowSide &side)
{
    const int n = (int)words.size();
    side.first = std::max(1, std::min(side.first, n - 1));
    side.last = std::max(side.first, std::min(side.last, n - 1));

    std::vector<char> pooled;
    side.siblings = 0;
    int idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
        bool take = idx == pal;
        if (!take && p->data_p) {
            std::vector<unsigned short> w = PalettePreviewWords(p);
            take = IsCostumeSibling(words.data(), n, w.data(), (int)w.size(), side.first, side.last);
            side.siblings += take;
        }
        pooled.push_back(take ? 1 : 0);
    }

    long slot_px[256] = {};
    long best_area = -1;
    side.preview_img = -1;
    idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        const int pn = (int)img->palnum;
        if (pn < 0 || pn >= (int)pooled.size() || !pooled[pn] || !img->data_p) continue;
        const int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        long in_range = 0;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++) {
                const unsigned char v = pix[y * stride + x];
                slot_px[v]++;
                in_range += v >= side.first && v <= side.last;
            }
        /* Prefer a sprite on the palette itself; else the one showing the
           most of the costume. */
        const long score = in_range + (pn == pal ? (1L << 30) : 0);
        if (in_range > 0 && score > best_area) { best_area = score; side.preview_img = idx; }
    }
    side.weight.assign(side.last - side.first + 1, 0.0);
    side.pixels = 0;
    for (int s = side.first; s <= side.last; s++) {
        side.weight[s - side.first] = (double)slot_px[s];
        side.pixels += slot_px[s];
    }
}

static void Borrow(void)
{
    g_ac.out_words = g_ac.words;
    if (g_ac.ref_words.size() < 2 || g_ac.words.size() < 2) return;
    std::vector<int> ds, rs;
    for (int s = g_ac.dst.first; s <= g_ac.dst.last; s++) ds.push_back(s);
    for (int s = g_ac.ref.first; s <= g_ac.ref.last; s++) rs.push_back(s);
    BorrowRampByCoverage(g_ac.words.data(), ds.data(), g_ac.weigh ? g_ac.dst.weight.data() : nullptr,
                         (int)ds.size(),
                         g_ac.ref_words.data(), rs.data(), g_ac.weigh ? g_ac.ref.weight.data() : nullptr,
                         (int)rs.size(), g_ac.out_words.data());
}

static void Recolor(void);

static void SetReference(int pal)
{
    g_ac.ref_pal = pal;
    g_ac.ref_words = WordsOf(pal);
    if (g_ac.ref_words.size() >= 2) {
        SuggestRange(pal, g_ac.ref_words, g_ac.ref);
        MeasureSide(pal, g_ac.ref_words, g_ac.ref);
    }
    Recolor();
}

static void Recolor(void)
{
    if (g_ac.mode == ModeBorrow) { Borrow(); return; }
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
    fresh.mode = g_ac.mode;
    fresh.weigh = g_ac.weigh;
    const int prev_ref = g_ac.ref_pal != pal && get_pal(g_ac.ref_pal) ? g_ac.ref_pal : -1;
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

    /* Borrow's destination range, and its coverage over this palette's
       costume siblings. A master palette with no sprites of its own (MK2's
       ORIGP) previews on a sibling's sprite: same layout, same slots. */
    SuggestRange(pal, g_ac.words, g_ac.dst);
    MeasureSide(pal, g_ac.words, g_ac.dst);
    if (g_ac.preview_img < 0) g_ac.preview_img = g_ac.dst.preview_img;
    if (prev_ref >= 0) {
        g_ac.ref_pal = prev_ref;
        g_ac.ref_words = WordsOf(prev_ref);
        SuggestRange(prev_ref, g_ac.ref_words, g_ac.ref);
        MeasureSide(prev_ref, g_ac.ref_words, g_ac.ref);
    }

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

/* A slot range can run to 64 shades; PalettePreviewStrip cuts at 24, so lay
   it out 16 to a row. */
static void RangeStrip(const std::vector<unsigned short> &words, const BorrowSide &side)
{
    for (int s = side.first; s <= side.last; s += 16)
        PalettePreviewStrip(words, s, std::min(16, side.last - s + 1));
}

static bool RangeInput(const char *id, BorrowSide &side, int numc)
{
    int v[2] = { side.first, side.last };
    ImGui::SetNextItemWidth(130.0f);
    if (!ImGui::InputInt2(id, v)) return false;
    side.first = std::max(1, std::min(v[0], numc - 1));
    side.last = std::max(side.first, std::min(v[1], numc - 1));
    return true;
}

static void DrawBorrowControls(bool &changed)
{
    const int n = (int)g_ac.words.size();
    ImGui::TextUnformatted("Recolor slots");
    ImGui::SameLine(110.0f);
    if (RangeInput("##ac_dst", g_ac.dst, n)) { MeasureSide(g_ac.src_pal, g_ac.words, g_ac.dst); changed = true; }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "First and last slot of this palette's costume ramp. Pre-filled\n"
        "with the slots that differ from its closest sibling palette\n"
        "(SCORP_P vs SUB_P: 1-32).");
    ImGui::SameLine();
    ImGui::TextDisabled("%ld px across %d palette(s)", g_ac.dst.pixels, g_ac.dst.siblings + 1);
    if (PalettePickerGrid("##ac_dst_pick", g_ac.words, &g_ac.dst.first, &g_ac.dst.last)) {
        MeasureSide(g_ac.src_pal, g_ac.words, g_ac.dst);
        changed = true;
    }

    ImGui::Spacing();
    ImGui::TextUnformatted("Colors from");
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(200.0f);
    PAL *ref = g_ac.ref_pal >= 0 ? get_pal(g_ac.ref_pal) : NULL;
    char label[48];
    if (ref) snprintf(label, sizeof(label), "%.9s (%d colors)", ref->n_s, (int)ref->numc);
    else     snprintf(label, sizeof(label), "(choose a palette)");
    if (ImGui::BeginCombo("##ac_ref", label, ImGuiComboFlags_HeightLarge)) {
        int idx = 0;
        for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
            if (idx == g_ac.src_pal) continue;
            char item[48];
            snprintf(item, sizeof(item), "%.9s (%d)##acr%d", p->n_s, (int)p->numc, idx);
            if (ImGui::Selectable(item, idx == g_ac.ref_pal)) SetReference(idx);
        }
        ImGui::EndCombo();
    }
    if (ref && g_ac.ref_words.size() >= 2) {
        ImGui::TextUnformatted("Its slots");
        ImGui::SameLine(110.0f);
        if (RangeInput("##ac_ref_range", g_ac.ref, (int)g_ac.ref_words.size())) {
            MeasureSide(g_ac.ref_pal, g_ac.ref_words, g_ac.ref);
            changed = true;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "The reference palette's costume ramp. Leave out flat fills that\n"
            "belong to another material: RAIN1_P's 49-63 are all one color,\n"
            "the pants, and would drag the gear toward black.");
        ImGui::SameLine();
        ImGui::TextDisabled("%ld px across %d palette(s)", g_ac.ref.pixels, g_ac.ref.siblings + 1);
        if (PalettePickerGrid("##ac_ref_pick", g_ac.ref_words, &g_ac.ref.first, &g_ac.ref.last)) {
            MeasureSide(g_ac.ref_pal, g_ac.ref_words, g_ac.ref);
            changed = true;
        }

        ImGui::Spacing();
        changed |= ImGui::Checkbox("Match by pixel coverage", &g_ac.weigh);
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "On: a shade covering 10%% of this palette's costume pixels takes\n"
            "the color covering the same 10%% of the reference's, counted over\n"
            "each palette's sprites and its costume siblings' sprites.\n"
            "Off: shades are spread evenly by rank, brightest to darkest.");

        ImGui::Spacing();
        ImGui::TextUnformatted("Becomes");
        RangeStrip(g_ac.out_words, g_ac.dst);
    } else {
        ImGui::TextDisabled("Pick the palette whose costume colors to borrow.");
    }
}

static void DrawTintControls(bool &changed)
{
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("Ramp split", &g_ac.tol, 5.0f, 90.0f, "%.0f deg")) Redetect();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "How far a color's hue may turn before a new ramp starts.\n"
        "Lower if the cloth is merged with the skin; higher if one\n"
        "material is split in two.");

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

    ImGui::Separator();

    const float kSide = 300.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##ac_left", ImVec2(-kSide, -footer), true);
    bool changed = false;
    changed |= ImGui::RadioButton("Tint with a color", &g_ac.mode, ModeTint);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Borrow from palette", &g_ac.mode, ModeBorrow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Take the recolored slots' colors from another palette's costume\n"
        "ramp, e.g. UMK3's RAIN1_P onto MK2's ninja layout. Shades are\n"
        "matched by how many pixels they cover, not by brightness.");
    ImGui::Separator();
    if (g_ac.mode == ModeBorrow) DrawBorrowControls(changed);
    else                         DrawTintControls(changed);
    if (changed) Recolor();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##ac_preview", ImVec2(0, -footer), true);
    IMG *pv = g_ac.preview_img >= 0 ? get_img(g_ac.preview_img) : NULL;
    IMG *rv = g_ac.mode == ModeBorrow && g_ac.ref_pal >= 0 && g_ac.ref.preview_img >= 0
              ? get_img(g_ac.ref.preview_img) : NULL;
    const int panes = rv ? 3 : 2;
    const float pane_h = (ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * panes) / panes;
    const float pane_w = ImGui::GetContentRegionAvail().x;
    if (pv) {
        PalettePreviewSprite(2, "Before", pv, g_ac.words, nullptr, pane_w, pane_h);
        PalettePreviewSprite(3, "After", pv, g_ac.out_words, nullptr, pane_w, pane_h);
    } else {
        ImGui::TextDisabled("No sprite uses this palette\nto preview on.");
    }
    if (rv) PalettePreviewSprite(1, "Reference", rv, g_ac.ref_words, nullptr, pane_w, pane_h);
    ImGui::EndChild();

    ImGui::TextUnformatted("Point at the new palette:");
    ImGui::SameLine();
    ImGui::RadioButton("None", &g_ac.repoint, RepointNone);         ImGui::SameLine();
    ImGui::RadioButton("Selected", &g_ac.repoint, RepointSelected); ImGui::SameLine();
    ImGui::RadioButton("Marked", &g_ac.repoint, RepointMarked);     ImGui::SameLine();
    ImGui::RadioButton("All on source", &g_ac.repoint, RepointAll);
    bool any_picked = false;
    for (char p : g_ac.picked) any_picked |= p != 0;
    if (g_ac.mode == ModeBorrow) any_picked = g_ac.ref_words.size() >= 2;
    ImGui::BeginDisabled(!any_picked);
    if (ImGui::Button("Create Palette", ImVec2(140, 0))) Commit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) g_ac.open = false;
    if (!g_ac.open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
