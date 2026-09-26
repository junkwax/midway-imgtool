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
 * another palette's colors, matched shade-for-shade by pixel coverage
 * (palette_transfer.h's BorrowRampByCoverage). This is how UMK3's RAIN1_P
 * becomes an MK2 ninja palette on NINJAS8's sprites. The slots are split
 * into named sections (Costume, Skin, Fans...), each with its own ranges on
 * both palettes and borrowed on its own. A section's layout on one palette
 * can be saved (palette_layout.h) and comes back by itself the next time
 * that palette is used. Without a saved layout, a Costume section is
 * pre-filled by diffing each palette against its closest costume siblings.
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
#include "palette_layout.h"
#include "palette_ramp.h"
#include "palette_transfer.h"
#include "document.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

enum AltCostumeRepoint { RepointNone = 0, RepointSelected, RepointMarked, RepointAll };
enum AltCostumeMode { ModeTint = 0, ModeBorrow };

/* The slots of one section on one palette, and how many pixels each covers
   across that palette and its siblings. Several ranges because a material
   can be broken up: Kitana's fan color sits inside her cloth ramp, so her
   cloth is e.g. 1-5 plus 10-32. */
struct SlotSet {
    std::vector<TransferBlock> ranges;      /* as edited, in order; may overlap */
    int active = 0;                         /* range the grid's clicks edit */
    int anchor = -1;                        /* first click of a range awaiting its end click */
    char text[128] = "";                    /* ranges as typed, "1-5, 10-32" */
    std::vector<int> slots;                 /* ranges flattened: sorted, unique */
    std::vector<double> weight;             /* one per entry of `slots` */
    long pixels = 0;
};

/* A named material, borrowed on its own: its slots on this palette take the
   colors of its slots on the reference. */
struct CostumeSection {
    int id = 0;                             /* stable ImGui id across renames */
    char name[16] = "Costume";
    bool on = true;
    SlotSet dst, ref;
};

/* Per palette: the pooled sprites that weight every section. */
struct SideStats {
    int siblings = 0;                       /* palettes pooled besides this one */
    int preview_img = -1;                   /* a sprite drawn through this layout */
    char layout[32] = "";                   /* saved layout last applied, if any */
};

struct AltCostumeSession {
    bool open = false;
    int src_pal = -1;
    float tol = 30.0f;
    float color[3] = { 0.15f, 0.30f, 0.85f };
    float amount = 1.0f;
    int repoint = RepointNone;
    char name[10] = "";
    bool name_auto = true;                  /* follow the settings until typed over */
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
    std::vector<CostumeSection> sections;
    int next_id = 1;
    int select_tab = -1;                    /* section id to bring forward next frame */
    int shown_tab = -1;                     /* section id whose tab is open */
    int hover_dst = -1, hover_ref = -1;     /* swatch under the mouse this frame */
    bool highlight = true;                  /* dim what the selection does not touch */
    SideStats dst_stats, ref_stats;

    bool save_dst = true;                   /* which palette the Save Layout popup saves */
    char save_name[32] = "";
};

static AltCostumeSession g_ac;
static char g_last_ref_name[10] = "";       /* the reference, remembered across opens */

/* ---- Saved layouts ---- */

static std::vector<PaletteLayout> g_layouts;
static bool g_layouts_loaded = false;

static std::string LayoutPath(void)
{
    char *pref = SDL_GetPrefPath("midway", "imgtool");
    if (!pref) return "palette_layouts.txt";
    std::string path(pref);
    SDL_free(pref);
    return path + "palette_layouts.txt";
}

static void LoadLayouts(void)
{
    if (g_layouts_loaded) return;
    g_layouts_loaded = true;
    FILE *f = fopen(LayoutPath().c_str(), "r");
    if (!f) return;
    char line[4096];
    while (fgets(line, sizeof(line), f)) {
        PaletteLayout l;
        if (ParsePaletteLayoutLine(line, &l)) g_layouts.push_back(l);
    }
    fclose(f);
}

static bool SaveLayouts(void)
{
    FILE *f = fopen(LayoutPath().c_str(), "w");
    if (!f) return false;
    for (const PaletteLayout &l : g_layouts) fprintf(f, "%s\n", FormatPaletteLayoutLine(l).c_str());
    const bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

/* Sort key for names: uppercased, at most `max` characters. */
static std::string UpperName(const char *s, int max)
{
    std::string out;
    for (int i = 0; i < max && s[i]; i++)
        out += s[i] >= 'a' && s[i] <= 'z' ? (char)(s[i] - 'a' + 'A') : s[i];
    return out;
}

static int LayoutIndex(const char *name)
{
    for (size_t i = 0; i < g_layouts.size(); i++)
        if (SameLayoutName(g_layouts[i].name, name)) return (int)i;
    return -1;
}

/* ---- Borrow from palette ---- */

static std::vector<unsigned short> WordsOf(int pal)
{
    return PalettePreviewWords(pal >= 0 ? get_pal(pal) : NULL);
}

static int SideNumc(bool dst) { return (int)(dst ? g_ac.words : g_ac.ref_words).size(); }
static int SidePal(bool dst) { return dst ? g_ac.src_pal : g_ac.ref_pal; }
static SlotSet &SideOf(CostumeSection &s, bool dst) { return dst ? s.dst : s.ref; }

static void SetRanges(SlotSet &side, const std::vector<TransferBlock> &ranges, int active)
{
    side.ranges = ranges;
    side.active = std::max(0, std::min(active, (int)ranges.size() - 1));
    side.anchor = -1;
    FormatSlotRanges(ranges.data(), (int)ranges.size(), side.text, sizeof(side.text));
}

static int SectionIndex(const char *name)
{
    for (size_t i = 0; i < g_ac.sections.size(); i++)
        if (SameLayoutName(g_ac.sections[i].name, name)) return (int)i;
    return -1;
}

static int AddSection(const char *name)
{
    CostumeSection s;
    s.id = g_ac.next_id++;
    snprintf(s.name, sizeof(s.name), "%s", name);
    g_ac.sections.push_back(s);
    return (int)g_ac.sections.size() - 1;
}

/* The costume ramp of `pal`: the slots where it differs from its costume
   variants, i.e. the palettes (same color count) sharing at least 3/4 as
   many slots as the closest one does. The whole palette when none. */
static std::vector<TransferBlock> SuggestCostume(int pal, const std::vector<unsigned short> &words)
{
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
        FindCostumeRun(words.data(), sibs.data(), (int)sibs.size(), (int)words.size(), &f, &l))
        return { { f, l - f + 1 } };
    return { { 1, (int)words.size() - 1 } };
}

/* A slot set's ranges as a per-slot mask (length `n`), clipped to [1, n). */
static void AddToMask(const SlotSet &side, std::vector<char> &mask)
{
    const int n = (int)mask.size();
    for (const TransferBlock &r : side.ranges)
        for (int s = std::max(1, r.start); s < r.start + r.count && s < n; s++) mask[s] = 1;
}

static std::vector<char> RangeMask(const SlotSet &side, int n)
{
    std::vector<char> mask(std::max(n, 1), 0);
    AddToMask(side, mask);
    return mask;
}

/* Pixel coverage of every section's slots on one palette, over every sprite
   on it or on a costume sibling of it — RAIN1_P has two sprites of its own,
   but the UMK3 ninja sprites on SCORP1_P, REP1_P... all show where its gear
   shades land. Siblings are judged by IsCostumeFamily, not outside the
   sections: judged outside them, a Skin-only pass on a palette whose gear
   was already borrowed dropped every sibling (they differ in the gear), so
   skin came out darker or lighter than doing skin first. Also picks the
   largest such sprite to preview. */
static void MeasureSide(bool dst)
{
    const int pal = SidePal(dst);
    const std::vector<unsigned short> &words = dst ? g_ac.words : g_ac.ref_words;
    SideStats &st = dst ? g_ac.dst_stats : g_ac.ref_stats;
    const int n = (int)words.size();
    std::vector<char> any(std::max(n, 1), 0);
    for (CostumeSection &s : g_ac.sections) AddToMask(SideOf(s, dst), any);

    std::vector<char> pooled;
    st.siblings = 0;
    int idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
        bool take = idx == pal;
        if (!take && p->data_p && n > 1) {
            std::vector<unsigned short> w = PalettePreviewWords(p);
            take = IsCostumeFamily(words.data(), n, w.data(), (int)w.size());
            st.siblings += take;
        }
        pooled.push_back(take ? 1 : 0);
    }

    long slot_px[256] = {};
    long best_area = -1;
    st.preview_img = -1;
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
                in_range += v < n && any[v];
            }
        /* Prefer a sprite on the palette itself; else the one showing the
           most of the sections. */
        const long score = in_range + (pn == pal ? (1L << 30) : 0);
        if (in_range > 0 && score > best_area) { best_area = score; st.preview_img = idx; }
    }

    for (CostumeSection &s : g_ac.sections) {
        SlotSet &side = SideOf(s, dst);
        const std::vector<char> mask = RangeMask(side, n);
        side.slots.clear();
        side.weight.clear();
        side.pixels = 0;
        for (int k = 1; k < n; k++) {
            if (!mask[k]) continue;
            side.slots.push_back(k);
            side.weight.push_back((double)slot_px[k]);
            side.pixels += slot_px[k];
        }
    }
}

static bool SectionReady(const CostumeSection &s)
{
    return s.on && !s.dst.slots.empty() && !s.ref.slots.empty();
}

static void Borrow(void)
{
    g_ac.out_words = g_ac.words;
    if (g_ac.ref_words.size() < 2 || g_ac.words.size() < 2) return;
    for (const CostumeSection &s : g_ac.sections) {
        if (!SectionReady(s)) continue;
        BorrowRampByCoverage(g_ac.words.data(), s.dst.slots.data(),
                             g_ac.weigh ? s.dst.weight.data() : nullptr, (int)s.dst.slots.size(),
                             g_ac.ref_words.data(), s.ref.slots.data(),
                             g_ac.weigh ? s.ref.weight.data() : nullptr, (int)s.ref.slots.size(),
                             g_ac.out_words.data());
    }
}

/* Put a saved layout's sections on one palette. Sections it names are
   created as needed; other sections lose their slots on that palette, and a
   section left with no slots on either palette goes away. The palette is
   remembered on the layout so it comes back by itself next time. */
static void ApplyLayout(bool dst, int li)
{
    if (li < 0 || li >= (int)g_layouts.size()) return;
    PaletteLayout &l = g_layouts[li];
    const int numc = SideNumc(dst);
    for (CostumeSection &s : g_ac.sections) SetRanges(SideOf(s, dst), {}, 0);
    for (const PaletteLayoutSection &ls : l.sections) {
        int k = SectionIndex(ls.name.c_str());
        if (k < 0) k = AddSection(ls.name.c_str());
        TransferBlock buf[32];
        const int n = ParseSlotRanges(ls.ranges.c_str(), numc, buf, 32);
        SetRanges(SideOf(g_ac.sections[k], dst), std::vector<TransferBlock>(buf, buf + std::max(n, 0)), 0);
    }
    for (size_t i = g_ac.sections.size(); i-- > 0;)
        if (g_ac.sections[i].dst.ranges.empty() && g_ac.sections[i].ref.ranges.empty())
            g_ac.sections.erase(g_ac.sections.begin() + i);
    if (g_ac.sections.empty()) AddSection("Costume");

    SideStats &st = dst ? g_ac.dst_stats : g_ac.ref_stats;
    snprintf(st.layout, sizeof(st.layout), "%s", l.name.c_str());
    PAL *p = get_pal(SidePal(dst));
    if (p) {
        bool listed = false;
        for (const std::string &n : l.palettes) listed |= SameLayoutName(n, p->n_s);
        if (!listed) { l.palettes.push_back(p->n_s); SaveLayouts(); }
    }
}

/* Fill one palette's sections: its saved layout when there is one; else,
   for the reference, keep the current slots if the new reference has the
   same color count as the old (likely the same layout: RAIN1_P -> RAIN2_P);
   else a Costume section from the sibling diff. */
static void FillSide(bool dst, int prev_numc)
{
    LoadLayouts();
    PAL *p = get_pal(SidePal(dst));
    const int numc = SideNumc(dst);
    SideStats &st = dst ? g_ac.dst_stats : g_ac.ref_stats;
    const int li = p ? FindLayoutForPalette(g_layouts, p->n_s, numc) : -1;
    if (li >= 0) { ApplyLayout(dst, li); return; }
    if (!dst && prev_numc == numc) {
        bool any = false;
        for (CostumeSection &s : g_ac.sections) any |= !s.ref.ranges.empty();
        if (any) return;
    }
    st.layout[0] = '\0';
    int k = SectionIndex("Costume");
    if (k < 0) k = g_ac.sections.empty() ? AddSection("Costume") : 0;
    SetRanges(SideOf(g_ac.sections[k], dst), SuggestCostume(SidePal(dst), dst ? g_ac.words : g_ac.ref_words), 0);
}

static void SaveLayoutFromSide(bool dst, const char *name)
{
    PaletteLayout l;
    l.name = CleanLayoutField(name, 31);
    if (l.name.empty()) return;
    l.numc = SideNumc(dst);
    for (const CostumeSection &s : g_ac.sections) {
        const SlotSet &side = dst ? s.dst : s.ref;
        if (side.ranges.empty()) continue;
        char text[128];
        FormatSlotRanges(side.ranges.data(), (int)side.ranges.size(), text, sizeof(text));
        l.sections.push_back({ s.name, text });
    }
    const int li = LayoutIndex(l.name.c_str());
    if (li >= 0) l.palettes = g_layouts[li].palettes;
    PAL *p = get_pal(SidePal(dst));
    if (p) {
        bool listed = false;
        for (const std::string &n : l.palettes) listed |= SameLayoutName(n, p->n_s);
        if (!listed) l.palettes.push_back(p->n_s);
    }
    if (li >= 0) g_layouts[li] = l;
    else g_layouts.push_back(l);
    SideStats &st = dst ? g_ac.dst_stats : g_ac.ref_stats;
    snprintf(st.layout, sizeof(st.layout), "%s", l.name.c_str());
    const bool ok = SaveLayouts();
    snprintf(g_restore_msg, sizeof(g_restore_msg), ok ? "Saved palette layout %s (%d section(s))."
                                                      : "Could not write the palette layout file.",
             l.name.c_str(), (int)l.sections.size());
    g_restore_msg_timer = 4.0f;
}

static void Recolor(void);

/* ---- Naming the new palette ---- */

/* Case-blind: RAIN_P and rain_p would be the same label to the toolchain. */
static bool PalNameTaken(const char *name)
{
    auto up = [](char c) { return c >= 'a' && c <= 'z' ? (char)(c - 'a' + 'A') : c; };
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
        int i = 0;
        while (i < 10 && up(p->n_s[i]) == up(name[i]) && name[i]) i++;
        if (i == 10 || up(p->n_s[i]) == up(name[i])) return true;
    }
    return false;
}

/* `name` itself when no loaded palette has it, else the next free numbered
   form (RAIN_P -> RAIN1_P -> ... the same counter Duplicate Palette uses). */
static void UniquePalName(const char *name, char out[10])
{
    char cand[10];
    snprintf(cand, sizeof(cand), "%.9s", name);
    if (!PalNameTaken(cand)) { memcpy(out, cand, 10); return; }
    make_numbered_pal_name(cand, out);
}

/* A name that says what the palette is, in the 9-character NAME_P form:
   borrowing from RAIN1_P gives RAIN_P (then RAIN5_P, if RAIN_P..RAIN4_P are
   loaded); tinting SCORP_P blue gives SCORBLU_P. */
static void AutoName(void)
{
    char stem[8] = "";
    PAL *ref = g_ac.mode == ModeBorrow && g_ac.ref_pal >= 0 ? get_pal(g_ac.ref_pal) : NULL;
    PAL *src = get_pal(g_ac.src_pal);
    if (ref) {
        CostumeNameStem(ref->n_s, 7, stem);
    } else if (src) {
        char s4[5];
        CostumeNameStem(src->n_s, 4, s4);
        snprintf(stem, sizeof(stem), "%s%s", s4,
                 HueTag((int)std::lround(g_ac.color[0] * 255.0f), (int)std::lround(g_ac.color[1] * 255.0f),
                        (int)std::lround(g_ac.color[2] * 255.0f)));
    }
    if (!stem[0]) snprintf(stem, sizeof(stem), "COSTUME");
    char cand[10];
    snprintf(cand, sizeof(cand), "%.7s_P", stem);
    UniquePalName(cand, g_ac.name);
}

/* Palette names are A-Z, 0-9 and '_' (ImGuiInputTextFlags_CharsUppercase
   has already folded a-z). */
static int PalNameCharFilter(ImGuiInputTextCallbackData *d)
{
    const ImWchar c = d->EventChar;
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ? 0 : 1;
}

static void SetReference(int pal)
{
    const int prev_numc = (int)g_ac.ref_words.size();
    g_ac.ref_pal = pal;
    g_ac.ref_words = WordsOf(pal);
    PAL *p = get_pal(pal);
    if (p) snprintf(g_last_ref_name, sizeof(g_last_ref_name), "%.9s", p->n_s);
    if (g_ac.ref_words.size() >= 2) {
        FillSide(false, prev_numc);
        MeasureSide(true);                  /* a layout may have added sections */
        MeasureSide(false);
    }
    if (g_ac.name_auto) AutoName();
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
    g_ac = fresh;
    g_ac.open = true;
    g_ac.src_pal = pal;
    g_ac.words = PalettePreviewWords(src);

    /* Preview the selected sprite when it is on this palette, else the first
       one that is. */
    int idx = 0;
    IMG *sel = g_doc->ilselected >= 0 ? get_img(g_doc->ilselected) : NULL;
    if (sel && (int)sel->palnum == pal) g_ac.preview_img = g_doc->ilselected;
    else
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++)
            if ((int)img->palnum == pal) { g_ac.preview_img = idx; break; }

    /* Borrow's sections on this palette, and their coverage over its costume
       siblings. A master palette with no sprites of its own (MK2's ORIGP)
       previews on a sibling's sprite: same layout, same slots. */
    FillSide(true, 0);
    MeasureSide(true);
    if (g_ac.preview_img < 0) g_ac.preview_img = g_ac.dst_stats.preview_img;

    /* The last reference, found by name so it survives files loading in a
       different order. */
    idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p && g_last_ref_name[0]; p = (PAL *)p->nxt_p, idx++)
        if (idx != pal && strncmp(p->n_s, g_last_ref_name, 10) == 0) { SetReference(idx); break; }
    AutoName();

    Redetect();
}

static void Commit(void)
{
    PAL *src = get_pal(g_ac.src_pal);
    if (!src || g_ac.out_words.empty()) return;
    /* A typed name that is blank or already loaded gets the next free
       number rather than a second palette under the same name. */
    char final_name[10];
    if (!g_ac.name[0]) AutoName();
    UniquePalName(g_ac.name, final_name);
    const bool renamed = std::strncmp(final_name, g_ac.name, 10) != 0;
    if (!doc_undo_push()) return;

    PAL *pal = AllocPal();
    if (!pal) return;
    pal->flags   = 0;
    pal->bitspix = src->bitspix;
    pal->numc    = src->numc;
    pal->pad     = 0;
    std::memset(pal->n_s, 0, sizeof(pal->n_s));
    std::memcpy(pal->n_s, final_name, 9);
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
    snprintf(g_restore_msg, sizeof(g_restore_msg), "Created costume palette %.9s from %.9s%s%s",
             pal->n_s, src->n_s, renamed ? " (name was taken, numbered)" : "",
             repointed ? "; sprites repointed." : ".");
    g_restore_msg_timer = 5.0f;
    g_ac.open = false;
}

/* Each of the slot set's ranges, 16 swatches to a row (PalettePreviewStrip
   cuts at 24). */
static void RangeStrip(const std::vector<unsigned short> &words, const SlotSet &side)
{
    for (const TransferBlock &r : side.ranges) {
        const int last = r.start + r.count - 1;
        for (int s = r.start; s <= last; s += 16)
            PalettePreviewStrip(words, s, std::min(16, last - s + 1));
    }
}

/* The ranges as editable text, "1-5, 10-32". Text that does not parse keeps
   the last good ranges (and turns red) until it does. */
static bool RangeText(const char *id, SlotSet &side, int numc)
{
    TransferBlock buf[32];
    const bool bad = ParseSlotRanges(side.text, numc, buf, 32) < 0;
    if (bad) ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 110, 110, 255));
    ImGui::SetNextItemWidth(170.0f);
    const bool edited = ImGui::InputText(id, side.text, sizeof(side.text));
    if (bad) ImGui::PopStyleColor();
    if (!edited) return false;
    const int n = ParseSlotRanges(side.text, numc, buf, 32);
    if (n < 0) return false;
    side.ranges.assign(buf, buf + n);
    side.active = std::max(0, n - 1);
    side.anchor = -1;
    return true;
}

/* The whole palette as a picker over one section's ranges. */
static bool RangeGrid(const char *id, const std::vector<unsigned short> &words, SlotSet &side,
                      int *hover_out)
{
    const int n = (int)words.size();
    const std::vector<char> mask = RangeMask(side, n);
    const bool has = !side.ranges.empty();
    const TransferBlock cur = has ? side.ranges[side.active] : TransferBlock{ -1, 0 };
    PalettePick pick;
    const bool asked = PalettePickerGrid(id, words, mask.data(), cur.start, cur.start + cur.count - 1,
                                         side.anchor >= 0, &pick);
    if (hover_out && pick.hover >= 0) *hover_out = pick.hover;
    if (!asked) return false;

    std::vector<TransferBlock> r = side.ranges;
    int active = side.active;
    int anchor = -1;                        /* stays -1 unless a range was just started */
    const int end_at = side.anchor >= 0 ? (pick.click >= 0 ? pick.click : pick.set_last) : -1;
    if (pick.add >= 0 && pick.add < n && mask[pick.add]) {
        /* Ctrl+click on a selected slot unselects just that slot, splitting
           any range that holds it: 10-32 minus 20 is 10-19, 21-32. */
        const int s = pick.add;
        std::vector<TransferBlock> out;
        int new_active = -1;
        for (int i = 0; i < (int)r.size(); i++) {
            const int a = r[i].start, b = r[i].start + r[i].count - 1;
            if (s < a || s > b) { out.push_back(r[i]); if (i == active) new_active = (int)out.size() - 1; continue; }
            if (a < s) out.push_back({ a, s - a });
            if (s < b) out.push_back({ s + 1, b - s });
            if (i == active) new_active = (int)out.size() - 1;
        }
        r = out;
        active = std::max(0, new_active);
    } else if (end_at >= 0) {
        /* Second click: the range runs from the first click to this one,
           whichever order they came in. */
        const int a = std::min(side.anchor, end_at), b = std::max(side.anchor, end_at);
        if (!has) { r.push_back({ a, b - a + 1 }); active = (int)r.size() - 1; }
        else      r[active] = { a, b - a + 1 };
    } else if (pick.add >= 0 || (pick.click >= 0 && !has)) {
        /* Ctrl+click (or a first click with no ranges yet): a new range. */
        const int s = pick.add >= 0 ? pick.add : pick.click;
        r.push_back({ s, 1 });
        active = (int)r.size() - 1;
        anchor = s;
    } else if (pick.click >= 0) {
        /* First click: restart the active range here; the next click ends it. */
        r[active] = { pick.click, 1 };
        anchor = pick.click;
    } else if (pick.set_last >= 0) {
        /* Right-click: end the active range here. */
        const int s = pick.set_last;
        if (!has) {
            r.push_back({ s, 1 });
            active = (int)r.size() - 1;
        } else {
            const int first = std::min(r[active].start, s);
            r[active] = { first, s - first + 1 };
        }
    } else if (pick.remove >= 0) {
        /* The active range if it holds the slot, else the latest that does. */
        auto holds = [&](const TransferBlock &b) {
            return pick.remove >= b.start && pick.remove < b.start + b.count;
        };
        int k = holds(r[active]) ? active : -1;
        for (int i = (int)r.size() - 1; k < 0 && i >= 0; i--)
            if (holds(r[i])) k = i;
        if (k < 0) return false;
        r.erase(r.begin() + k);
        if (active >= k) active--;
    }
    SetRanges(side, r, active);
    side.anchor = anchor;
    return true;
}

/* "Layout: [saved layouts v] Save... Delete" for one palette. */
static void LayoutRow(const char *label, bool dst, bool &changed)
{
    SideStats &st = dst ? g_ac.dst_stats : g_ac.ref_stats;
    const int numc = SideNumc(dst);
    ImGui::PushID(dst ? "dst_layout" : "ref_layout");
    ImGui::TextUnformatted(label);
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::BeginCombo("##layout", st.layout[0] ? st.layout : "(no saved layout)",
                          ImGuiComboFlags_HeightLarge)) {
        int shown = 0;
        std::vector<int> by_name((size_t)g_layouts.size());
        for (size_t k = 0; k < by_name.size(); k++) by_name[k] = (int)k;
        std::stable_sort(by_name.begin(), by_name.end(), [](int a, int b) {
            return UpperName(g_layouts[a].name.c_str(), 32) < UpperName(g_layouts[b].name.c_str(), 32);
        });
        for (int i : by_name) {
            const PaletteLayout &l = g_layouts[i];
            if (l.numc > 0 && numc > 0 && l.numc != numc) continue;
            shown++;
            const bool sel = SameLayoutName(l.name, st.layout);
            char item[64];
            snprintf(item, sizeof(item), "%s  (%d section%s)##lay%d", l.name.c_str(),
                     (int)l.sections.size(), l.sections.size() == 1 ? "" : "s", (int)i);
            if (ImGui::Selectable(item, sel)) {
                ApplyLayout(dst, (int)i);
                MeasureSide(true);
                MeasureSide(false);
                changed = true;
            }
            if (sel) {
                ImGui::SetItemDefaultFocus();
                if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
            }
        }
        if (!shown) ImGui::TextDisabled("No saved layouts for %d colors yet.", numc);
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Which slots of this palette hold which section. Saved layouts\n"
        "load by themselves for every palette they have been used with.");
    ImGui::SameLine();
    if (ImGui::SmallButton("Save...")) {
        g_ac.save_dst = dst;
        if (st.layout[0]) snprintf(g_ac.save_name, sizeof(g_ac.save_name), "%s", st.layout);
        else {
            PAL *p = get_pal(SidePal(dst));
            char stem[10] = "";
            if (p) CostumeNameStem(p->n_s, 9, stem);
            snprintf(g_ac.save_name, sizeof(g_ac.save_name), "%s", stem[0] ? stem : "LAYOUT");
        }
        ImGui::OpenPopup("Save Palette Layout");
    }
    ImGui::SameLine();
    const int li = st.layout[0] ? LayoutIndex(st.layout) : -1;
    ImGui::BeginDisabled(li < 0);
    if (ImGui::SmallButton("Delete")) {
        g_layouts.erase(g_layouts.begin() + li);
        SaveLayouts();
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Deleted palette layout %s.", st.layout);
        g_restore_msg_timer = 4.0f;
        st.layout[0] = '\0';
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(
        "Forget this saved layout. The sections stay as they are.");

    if (ImGui::BeginPopupModal("Save Palette Layout", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Save the sections of %s as a layout:",
                    get_pal(SidePal(g_ac.save_dst)) ? get_pal(SidePal(g_ac.save_dst))->n_s : "?");
        for (const CostumeSection &s : g_ac.sections) {
            const SlotSet &side = g_ac.save_dst ? s.dst : s.ref;
            if (!side.ranges.empty()) ImGui::BulletText("%s = %s", s.name, side.text);
        }
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
        ImGui::SetNextItemWidth(220.0f);
        const bool enter = ImGui::InputText("Name", g_ac.save_name, sizeof(g_ac.save_name),
                                            ImGuiInputTextFlags_EnterReturnsTrue);
        const bool exists = LayoutIndex(CleanLayoutField(g_ac.save_name, 31).c_str()) >= 0;
        if (exists) ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "Replaces the saved layout of that name.");
        const bool empty = CleanLayoutField(g_ac.save_name, 31).empty();
        ImGui::BeginDisabled(empty);
        if (ImGui::Button("Save", ImVec2(100, 0)) || (enter && !empty)) {
            SaveLayoutFromSide(g_ac.save_dst, g_ac.save_name);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

static void DrawSection(CostumeSection &s, bool &changed)
{
    const bool has_ref = g_ac.ref_pal >= 0 && g_ac.ref_words.size() >= 2;
    changed |= ImGui::Checkbox("Borrow this section", &s.on);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    ImGui::InputText("##sec_name", s.name, sizeof(s.name));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Section name. Sections pair up by name, so Skin here takes the\n"
        "colors of Skin on the reference, whatever slots each one uses.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_ac.sections.size() <= 1);
    bool remove = ImGui::SmallButton("Remove section");
    ImGui::EndDisabled();

    ImGui::TextUnformatted("Recolor slots");
    ImGui::SameLine(110.0f);
    bool dst_edit = RangeText("##sec_dst", s.dst, (int)g_ac.words.size());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "This section's slots on this palette, as ranges: 1-5, 10-32.\n"
        "Leave out another material's slots inside a ramp, like Kitana's\n"
        "fan color (give the fans their own section instead).");
    ImGui::SameLine();
    ImGui::TextDisabled("%ld px across %d palette(s)", s.dst.pixels, g_ac.dst_stats.siblings + 1);
    dst_edit |= RangeGrid("##sec_dst_pick", g_ac.words, s.dst, &g_ac.hover_dst);
    if (dst_edit) { MeasureSide(true); changed = true; }

    if (has_ref) {
        ImGui::Spacing();
        ImGui::TextUnformatted("Its slots");
        ImGui::SameLine(110.0f);
        bool ref_edit = RangeText("##sec_ref", s.ref, (int)g_ac.ref_words.size());
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "The same section's slots on the reference palette. Leave out flat\n"
            "fills and other materials: RAIN1_P's 49-63 are all one color,\n"
            "the pants, and would drag the gear toward black.");
        ImGui::SameLine();
        ImGui::TextDisabled("%ld px across %d palette(s)", s.ref.pixels, g_ac.ref_stats.siblings + 1);
        ref_edit |= RangeGrid("##sec_ref_pick", g_ac.ref_words, s.ref, &g_ac.hover_ref);
        if (ref_edit) { MeasureSide(false); changed = true; }

        ImGui::Spacing();
        if (SectionReady(s)) {
            ImGui::TextUnformatted("Becomes");
            RangeStrip(g_ac.out_words, s.dst);
        } else if (s.on) {
            ImGui::TextDisabled("Give this section slots on both palettes to borrow it.");
        }
    }

    if (remove) {
        for (size_t i = 0; i < g_ac.sections.size(); i++)
            if (g_ac.sections[i].id == s.id) { g_ac.sections.erase(g_ac.sections.begin() + i); break; }
        MeasureSide(true);
        if (has_ref) MeasureSide(false);
        changed = true;
    }
}

static void DrawBorrowControls(bool &changed)
{
    LoadLayouts();
    ImGui::TextUnformatted("Colors from");
    ImGui::SameLine(110.0f);
    ImGui::SetNextItemWidth(200.0f);
    PAL *ref = g_ac.ref_pal >= 0 ? get_pal(g_ac.ref_pal) : NULL;
    char label[48];
    if (ref) snprintf(label, sizeof(label), "%.9s (%d colors)", ref->n_s, (int)ref->numc);
    else     snprintf(label, sizeof(label), "(choose a palette)");
    if (ImGui::BeginCombo("##ac_ref", label, ImGuiComboFlags_HeightLarge)) {
        /* A-Z by name; the file order means nothing when picking by name. */
        std::vector<int> by_name;
        for (int i = 0; i < (int)g_doc->palcnt; i++)
            if (i != g_ac.src_pal && get_pal(i)) by_name.push_back(i);
        std::stable_sort(by_name.begin(), by_name.end(), [](int a, int b) {
            return UpperName(get_pal(a)->n_s, 10) < UpperName(get_pal(b)->n_s, 10);
        });
        for (int idx : by_name) {
            PAL *p = get_pal(idx);
            const bool sel = idx == g_ac.ref_pal;
            char item[48];
            snprintf(item, sizeof(item), "%.9s (%d)##acr%d", p->n_s, (int)p->numc, idx);
            if (ImGui::Selectable(item, sel)) { SetReference(idx); changed = true; }
            /* Open scrolled to the current pick, not the top of the list. */
            if (sel) {
                ImGui::SetItemDefaultFocus();
                if (ImGui::IsWindowAppearing()) ImGui::SetScrollHereY(0.5f);
            }
        }
        ImGui::EndCombo();
    }

    LayoutRow("This palette", true, changed);
    const bool has_ref = ref && g_ac.ref_words.size() >= 2;
    if (has_ref) LayoutRow("Reference", false, changed);
    ImGui::Spacing();

    if (ImGui::BeginTabBar("##sections", ImGuiTabBarFlags_AutoSelectNewTabs |
                                         ImGuiTabBarFlags_FittingPolicyScroll)) {
        for (size_t i = 0; i < g_ac.sections.size(); i++) {
            CostumeSection &s = g_ac.sections[i];
            char tab[40];
            snprintf(tab, sizeof(tab), "%s%s###sec%d", s.name[0] ? s.name : "(unnamed)",
                     SectionReady(s) ? "" : " *", s.id);
            ImGuiTabItemFlags flags = s.id == g_ac.select_tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (ImGui::BeginTabItem(tab, nullptr, flags)) {
                g_ac.shown_tab = s.id;
                ImGui::PushID(s.id);
                const size_t before = g_ac.sections.size();
                DrawSection(s, changed);
                ImGui::PopID();
                ImGui::EndTabItem();
                if (g_ac.sections.size() != before) break;   /* removed: `s` is gone */
            }
        }
        g_ac.select_tab = -1;
        if (ImGui::TabItemButton("+", ImGuiTabItemFlags_Trailing | ImGuiTabItemFlags_NoTooltip)) {
            static const char *kNames[] = { "Skin", "Trim", "Hair", "Weapon", "Belt", "Boots" };
            char name[16] = "";
            for (const char *n : kNames)
                if (SectionIndex(n) < 0) { snprintf(name, sizeof(name), "%s", n); break; }
            for (int k = 2; !name[0]; k++) {
                char cand[16];
                snprintf(cand, sizeof(cand), "Section %d", k);
                if (SectionIndex(cand) < 0) snprintf(name, sizeof(name), "%s", cand);
            }
            g_ac.select_tab = g_ac.sections[AddSection(name)].id;
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a section (skin, trim, fans...)");
        ImGui::EndTabBar();
    }

    if (!has_ref) {
        ImGui::TextDisabled("Pick the palette whose colors to borrow.");
        return;
    }
    ImGui::Spacing();
    changed |= ImGui::Checkbox("Match by pixel coverage", &g_ac.weigh);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "On: a shade covering 10%% of a section's pixels on this palette takes\n"
        "the color covering the same 10%% of that section on the reference,\n"
        "counted over each palette's sprites and its costume siblings' sprites.\n"
        "Off: shades are spread evenly by rank, brightest to darkest.");
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
    ImGui::SetNextWindowSize(ImVec2(900, 680), ImGuiCond_Once);
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
    if (ImGui::InputText("New palette name", g_ac.name, sizeof(g_ac.name),
                         ImGuiInputTextFlags_CharsUppercase | ImGuiInputTextFlags_CallbackCharFilter,
                         PalNameCharFilter))
        g_ac.name_auto = false;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Up to 9 characters: A-Z, 0-9, _. Named after the reference\n"
        "palette (or the source and tint color) until you type your own.");
    ImGui::SameLine();
    ImGui::BeginDisabled(g_ac.name_auto);
    if (ImGui::SmallButton("Auto")) { g_ac.name_auto = true; AutoName(); }
    ImGui::EndDisabled();
    if (!g_ac.name_auto && g_ac.name[0] && PalNameTaken(g_ac.name)) {
        char next[10];
        UniquePalName(g_ac.name, next);
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.3f, 1.0f), "taken; saves as %.9s", next);
    }

    ImGui::Separator();

    const float kSide = 300.0f;
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.2f;
    ImGui::BeginChild("##ac_left", ImVec2(-kSide, -footer), true);
    bool changed = false;
    changed |= ImGui::RadioButton("Tint with a color", &g_ac.mode, ModeTint);
    ImGui::SameLine();
    changed |= ImGui::RadioButton("Borrow from palette", &g_ac.mode, ModeBorrow);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Take the recolored slots' colors from another palette, section by\n"
        "section, e.g. UMK3's RAIN1_P onto MK2's ninja layout. Shades are\n"
        "matched by how many pixels they cover, not by brightness.");
    ImGui::Separator();
    g_ac.hover_dst = g_ac.hover_ref = -1;   /* the grids set these while hovered */
    if (g_ac.mode == ModeBorrow) DrawBorrowControls(changed);
    else                         DrawTintControls(changed);
    if (changed) {
        Recolor();
        if (g_ac.name_auto) AutoName();   /* mode, reference or tint color may have changed */
    }
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##ac_preview", ImVec2(0, -footer), true);
    IMG *pv = g_ac.preview_img >= 0 ? get_img(g_ac.preview_img) : NULL;
    IMG *rv = g_ac.mode == ModeBorrow && g_ac.ref_pal >= 0 && g_ac.ref_stats.preview_img >= 0
              ? get_img(g_ac.ref_stats.preview_img) : NULL;
    ImGui::Checkbox("Highlight selection", &g_ac.highlight);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Before and Reference show the selected colors in full and dim\n"
        "everything else. Hovering a swatch shows just that one color.");

    /* What Before and Reference highlight: the swatch being hovered, else
       the open section's slots (Borrow) or the ticked ramps (Tint). */
    char hl_dst[256] = {}, hl_ref[256] = {};
    const CostumeSection *open_sec = NULL;
    for (const CostumeSection &s : g_ac.sections)
        if (s.id == g_ac.shown_tab) open_sec = &s;
    if (g_ac.mode == ModeBorrow) {
        if (g_ac.hover_dst >= 0) hl_dst[g_ac.hover_dst & 255] = 1;
        else if (open_sec) for (int k : open_sec->dst.slots) hl_dst[k & 255] = 1;
        if (g_ac.hover_ref >= 0) hl_ref[g_ac.hover_ref & 255] = 1;
        else if (open_sec) for (int k : open_sec->ref.slots) hl_ref[k & 255] = 1;
    } else {
        for (size_t i = 0; i < g_ac.blocks.size(); i++)
            if (g_ac.picked[i])
                for (int k = 0; k < g_ac.blocks[i].count; k++) hl_dst[(g_ac.blocks[i].start + k) & 255] = 1;
    }
    bool any_dst = false, any_ref = false;
    for (int k = 1; k < 256; k++) { any_dst |= hl_dst[k] != 0; any_ref |= hl_ref[k] != 0; }
    const char *dst_mask = g_ac.highlight && any_dst ? hl_dst : nullptr;
    const char *ref_mask = g_ac.highlight && any_ref ? hl_ref : nullptr;

    const int panes = rv ? 3 : 2;
    const float pane_h = (ImGui::GetContentRegionAvail().y - ImGui::GetTextLineHeightWithSpacing() * panes) / panes;
    const float pane_w = ImGui::GetContentRegionAvail().x;
    if (pv) {
        PalettePreviewSprite(2, "Before", pv, g_ac.words, nullptr, pane_w, pane_h, dst_mask);
        PalettePreviewSprite(3, "After", pv, g_ac.out_words, nullptr, pane_w, pane_h);
    } else {
        ImGui::TextDisabled("No sprite uses this palette\nto preview on.");
    }
    if (rv) PalettePreviewSprite(1, "Reference", rv, g_ac.ref_words, nullptr, pane_w, pane_h, ref_mask);
    ImGui::EndChild();

    ImGui::TextUnformatted("Point at the new palette:");
    ImGui::SameLine();
    ImGui::RadioButton("None", &g_ac.repoint, RepointNone);         ImGui::SameLine();
    ImGui::RadioButton("Selected", &g_ac.repoint, RepointSelected); ImGui::SameLine();
    ImGui::RadioButton("Marked", &g_ac.repoint, RepointMarked);     ImGui::SameLine();
    ImGui::RadioButton("All on source", &g_ac.repoint, RepointAll);
    bool any_picked = false;
    for (char p : g_ac.picked) any_picked |= p != 0;
    if (g_ac.mode == ModeBorrow) {
        any_picked = false;
        if (g_ac.ref_words.size() >= 2)
            for (const CostumeSection &s : g_ac.sections) any_picked |= SectionReady(s);
    }
    ImGui::BeginDisabled(!any_picked);
    if (ImGui::Button("Create Palette", ImVec2(140, 0))) Commit();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(120, 0))) g_ac.open = false;
    if (!g_ac.open) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}
