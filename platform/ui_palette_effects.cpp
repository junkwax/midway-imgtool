/*************************************************************
 * platform/ui_palette_effects.cpp
 * "Generate Character Palettes" dialog.
 *
 * Every MK2 fighter carries the same set of derived palettes: JCFREZ_P and
 * JCNUFRZ_P for Sub-Zero's freezes, JCELPUP_P / JCELRED_P / JCREDEL_P /
 * JCPURPL_P for the electric hits, and a BLUD palette per costume for gore.
 * The freeze and electric ones are the costume palette pushed through a ramp
 * the artist has already saved in the Indexed Color Gradient (FREEZE,
 * FIVEFRZ, ELEC, BGRDEL_P, REDELI, PURPELI); this dialog runs all of them at
 * once from a right-click on the fighter's palette.
 *
 * The blood palettes are the costumes condensed to make room for the gore
 * colors (torso rips need the blood ramp plus bone) in the last slots, one
 * shared layout for every costume — palette_effects.h's PlanBloodCondense.
 * Gore art cut from the fighter's frames still indexes the full costume, so
 * marked sprites can be remapped into that layout and moved onto the
 * matching BLUD palette in the same step.
 *
 * Every result is a palette of its own; the source palettes stay as they
 * are. A name that already exists is overwritten in place (so re-running
 * after a costume tweak refreshes the set and sprites pointing at it keep
 * pointing at it), or skipped when Replace is off. One Undo step.
 *************************************************************/
#include <imgui.h>

#include "ui_palette_effects.h"
#include "ui_internal.h"
#include "ui_palette.h"         /* GradientPreset*, ApplyPalette */
#include "ui_palette_preview.h"
#include "ui_timeline.h"        /* InvalidateThumb */
#include "ui_undo.h"
#include "img_format.h"
#include "img_io.h"             /* g_restore_msg */
#include "palette_effects.h"
#include "document.h"

#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct GenRow {
    int  effect = -1;           /* kCharacterEffects index, -1 for blood */
    int  src = -1;              /* palette index */
    int  preset = -1;           /* gradient preset, effects only */
    bool on = true;
    bool name_edited = false;
    char name[16] = {};
};

struct RowResult {
    std::vector<unsigned short> words;
    std::string error;          /* blocks the row */
    std::string note;           /* informational */
    int existing = -1;          /* palette with this name, or -1 */
    unsigned bitspix = 0;
};

/* Where the blood lane's colors come from. */
enum BloodSourceKind { BLOOD_FROM_RAMP, BLOOD_FROM_GRADIENT, BLOOD_FROM_PALETTE };

bool                 s_open = false;
const Document      *s_doc = nullptr;
int                  s_source = -1;
CharacterPaletteSet  s_set = CHARACTER_PALETTES_ALL;
char                 s_prefix[8] = {};
std::vector<GenRow>  s_rows;
int                  s_blood_slots = 12;
BloodSourceKind      s_blood_kind = BLOOD_FROM_RAMP;
int                  s_blood_from = -1;     /* gradient or palette index */
bool                 s_move_marked = false;
bool                 s_replace = true;
int                  s_preview = 0;
/* Pixel counts per palette slot, by palette index; taken at open. */
std::vector<std::array<unsigned long long, 256>> s_usage;

/* The condense plan, rebuilt only when its inputs change. */
BloodCondensePlan    s_plan;
bool                 s_plan_ok = false;
std::string          s_plan_key;

int PresetByName(const char *name)
{
    for (int i = 0; i < GradientPresetCount(); i++) {
        const char *p = GradientPresetName(i);
        size_t k = 0;
        while (p[k] && name[k] && toupper((unsigned char)p[k]) == toupper((unsigned char)name[k])) k++;
        if (!p[k] && !name[k]) return i;
    }
    return -1;
}

bool StartsWithNoCase(const char *s, const std::string &prefix)
{
    for (size_t k = 0; k < prefix.size(); k++)
        if (!s[k] || toupper((unsigned char)s[k]) != toupper((unsigned char)prefix[k])) return false;
    return true;
}

bool ContainsNoCase(const char *s, const char *needle)
{
    for (; *s; s++)
        if (StartsWithNoCase(s, needle)) return true;
    return false;
}

int PaletteByName(const char *name)
{
    int idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++)
        if (strncmp(p->n_s, name, 10) == 0) return idx;
    return -1;
}

void CountUsage(void)
{
    s_usage.assign((size_t)g_doc->palcnt, {});
    for (IMG *im = (IMG *)g_doc->img_p; im; im = (IMG *)im->nxt_p) {
        if (!im->data_p || im->palnum >= s_usage.size()) continue;
        std::array<unsigned long long, 256> &u = s_usage[im->palnum];
        const unsigned char *px = (const unsigned char *)im->data_p;
        int stride = (im->w + 3) & ~3;
        for (int y = 0; y < (int)im->h; y++)
            for (int x = 0; x < (int)im->w; x++) u[px[y * stride + x]]++;
    }
}

unsigned long long PalettePixels(int pal_idx)
{
    if (pal_idx < 0 || pal_idx >= (int)s_usage.size()) return 0;
    unsigned long long n = 0;
    for (int s = 1; s < 256; s++) n += s_usage[pal_idx][s];
    return n;
}

void SetName(GenRow &r, const std::string &name)
{
    snprintf(r.name, sizeof(r.name), "%s", name.c_str());
}

/* An existing gore palette is the best default for the lane's colors: the
   torso-rip bone and blood are the same for every fighter. Then a saved
   gradient called BLOOD / BLUD / GORE, then the built-in ramp. */
void PickDefaultBloodSource(void)
{
    const std::string prefix = s_prefix;
    int idx = 0, other = -1, own = -1;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
        if (!p->data_p || p->numc < 3) continue;
        if (!ContainsNoCase(p->n_s, "BLUD") && !ContainsNoCase(p->n_s, "BLD")) continue;
        if (StartsWithNoCase(p->n_s, prefix)) { if (own < 0) own = idx; }
        else if (other < 0) other = idx;
    }
    int pal = other >= 0 ? other : own;
    if (pal >= 0) { s_blood_kind = BLOOD_FROM_PALETTE; s_blood_from = pal; return; }
    for (const char *name : { "BLOOD", "BLUD", "GORE" }) {
        int g = PresetByName(name);
        if (g >= 0) { s_blood_kind = BLOOD_FROM_GRADIENT; s_blood_from = g; return; }
    }
    s_blood_kind = BLOOD_FROM_RAMP;
    s_blood_from = -1;
}

/* The lane's colors, s_blood_slots of them, or an error. */
bool BloodWords(std::vector<unsigned short> &out, std::string &error)
{
    out.assign((size_t)s_blood_slots, 0);
    if (s_blood_kind == BLOOD_FROM_GRADIENT) {
        float stops[11 * 3];
        int n = GradientPresetStops(s_blood_from, stops, 11);
        if (n < 2) { error = "Choose a saved gradient for the blood colors"; return false; }
        SampleGradientWords(stops, n, s_blood_slots, out.data());
    } else if (s_blood_kind == BLOOD_FROM_PALETTE) {
        PAL *p = get_pal(s_blood_from);
        if (!p || !p->data_p) { error = "Choose a palette for the blood colors"; return false; }
        std::vector<unsigned short> w = PalettePreviewWords(p);
        if ((int)w.size() - 1 < s_blood_slots) {
            error = std::string(p->n_s) + " has fewer colors than the blood lane";
            return false;
        }
        for (int i = 0; i < s_blood_slots; i++) out[i] = w[w.size() - s_blood_slots + i];
    } else {
        FillBloodRamp(out.data(), 0, s_blood_slots);
    }
    return true;
}

std::string BloodSourceLabel(void)
{
    if (s_blood_kind == BLOOD_FROM_GRADIENT && s_blood_from >= 0)
        return std::string("Gradient ") + GradientPresetName(s_blood_from);
    if (s_blood_kind == BLOOD_FROM_PALETTE)
        if (PAL *p = get_pal(s_blood_from))
            return std::string("Last colors of ") + p->n_s;
    return "Built-in red ramp";
}

/* Rows for the current prefix. Checkbox and preset choices survive a prefix
   change; names follow it unless typed over. */
void BuildRows(void)
{
    std::vector<GenRow> old;
    old.swap(s_rows);
    const std::string prefix = s_prefix;
    const bool effects_on = s_set != CHARACTER_PALETTES_BLOOD;
    const bool blood_on = s_set != CHARACTER_PALETTES_EFFECTS;

    for (int e = 0; e < CHARACTER_EFFECT_COUNT; e++) {
        GenRow r;
        r.effect = e;
        r.src = s_source;
        r.preset = PresetByName(kCharacterEffects[e].preset);
        r.on = effects_on && r.preset >= 0;
        SetName(r, EffectPaletteName(prefix, kCharacterEffects[e].suffix));
        for (const GenRow &o : old)
            if (o.effect == e) {
                r.on = o.on; r.preset = o.preset;
                if (o.name_edited) { memcpy(r.name, o.name, sizeof(r.name)); r.name_edited = true; }
            }
        s_rows.push_back(r);
    }

    /* Costumes: the right-clicked palette first, then every other palette
       of this fighter that is not itself a generated one. */
    std::vector<int> costumes;
    PAL *src = get_pal(s_source);
    if (src && !IsGeneratedPaletteName(src->n_s, prefix)) costumes.push_back(s_source);
    if (!prefix.empty()) {
        int idx = 0;
        for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
            if (idx == s_source || IsGeneratedPaletteName(p->n_s, prefix)) continue;
            if (StartsWithNoCase(p->n_s, prefix)) costumes.push_back(idx);
        }
    }
    for (int c : costumes) {
        PAL *p = get_pal(c);
        GenRow r;
        r.src = c;
        /* Only costumes laid out like the source share its condense plan. */
        r.on = blood_on && src && p->numc == src->numc;
        std::string name = BloodPaletteName(prefix, p->n_s);
        /* JCMAIN_P and JCALT_P would both be JCBLUD_P: number the later one. */
        for (int k = 2; ; k++) {
            bool taken = false;
            for (const GenRow &o : s_rows) taken |= o.effect < 0 && name == o.name;
            if (!taken) break;
            name = prefix + "BLUD" + std::to_string(k) + "_P";
        }
        SetName(r, name);
        for (const GenRow &o : old)
            if (o.effect < 0 && o.src == c) {
                r.on = o.on;
                if (o.name_edited) { memcpy(r.name, o.name, sizeof(r.name)); r.name_edited = true; }
            }
        s_rows.push_back(r);
    }
    if (s_preview >= (int)s_rows.size()) s_preview = 0;
}

/* One plan over every ticked costume with the source's color count, using
   the pixels of every sprite drawn with any of them. */
void UpdatePlan(void)
{
    PAL *src = get_pal(s_source);
    std::string key = std::to_string(s_blood_slots);
    std::vector<int> pals;
    for (const GenRow &r : s_rows) {
        PAL *p = get_pal(r.src);
        if (r.effect >= 0 || !r.on || !p || !src || p->numc != src->numc) continue;
        pals.push_back(r.src);
        key += "," + std::to_string(r.src);
    }
    if (key == s_plan_key) return;
    s_plan_key = key;
    s_plan_ok = false;
    if (pals.empty() || !src) return;

    std::vector<std::vector<unsigned short>> words;
    std::vector<const unsigned short *> ptrs;
    unsigned long long usage[256] = {};
    for (int idx : pals) {
        words.push_back(PalettePreviewWords(get_pal(idx)));
        if (idx < (int)s_usage.size())
            for (int s = 0; s < 256; s++) usage[s] += s_usage[idx][s];
    }
    for (const auto &w : words) ptrs.push_back(w.data());
    s_plan_ok = PlanBloodCondense(ptrs.data(), (int)ptrs.size(), (int)src->numc, usage,
                                  s_blood_slots, s_plan);
}

RowResult Evaluate(int row_idx, const std::vector<unsigned short> &blood, const std::string &blood_error)
{
    const GenRow &r = s_rows[row_idx];
    RowResult out;
    PAL *src = get_pal(r.src);
    if (!src || !src->data_p || !src->numc) { out.error = "Source palette is gone"; return out; }
    out.words = PalettePreviewWords(src);
    int n = (int)out.words.size();
    out.bitspix = src->bitspix;

    if (r.effect >= 0) {
        float stops[11 * 3];
        int count = r.preset >= 0 ? GradientPresetStops(r.preset, stops, 11) : 0;
        if (count < 2) {
            out.error = std::string("No saved gradient named ") + kCharacterEffects[r.effect].preset;
            return out;
        }
        FitPaletteToGradient(out.words.data(), n, nullptr, stops, count, out.words.data());
    } else {
        PAL *main = get_pal(s_source);
        if (main && src->numc != main->numc) {
            char buf[96];
            snprintf(buf, sizeof(buf), "%u colors, %s has %u", src->numc, main->n_s, main->numc);
            out.error = buf;
            return out;
        }
        if (!blood_error.empty()) { out.error = blood_error; return out; }
        if (!s_plan_ok) { out.error = "Palette too small for the blood lane"; return out; }
        BuildBloodPalette(s_plan, out.words.data(), blood.data(), out.words.data());
    }

    const size_t len = strlen(r.name);
    if (!len) { out.error = "Needs a name"; return out; }
    if (len > PALETTE_NAME_MAX) { out.error = "Name longer than 9 characters"; return out; }
    for (int i = 0; i < (int)s_rows.size(); i++)
        if (i != row_idx && s_rows[i].on && strcmp(s_rows[i].name, r.name) == 0) {
            out.error = "Same name as another row";
            return out;
        }
    out.existing = PaletteByName(r.name);
    if (out.existing >= 0) {
        for (const GenRow &o : s_rows)
            if (o.on && o.src == out.existing) { out.error = "Would overwrite a source palette"; return out; }
        if (!s_replace) { out.error = "Already exists (Replace is off)"; return out; }
        /* Sprites already on an old BLUD palette index its old layout. */
        if (r.effect < 0 && PalettePixels(out.existing))
            out.note = "sprites already use it; check them after";
    }
    return out;
}

void Strip(const std::vector<unsigned short> &words, float width, float height)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    int n = (int)words.size();
    if (n > 0) {
        float w = width / (float)n;
        for (int i = 0; i < n; i++) {
            unsigned short c = words[i];
            int r = ((c >> 10) & 31) * 255 / 31, g = ((c >> 5) & 31) * 255 / 31, b = (c & 31) * 255 / 31;
            ImVec2 a(p.x + w * (float)i, p.y), z(p.x + w * (float)(i + 1), p.y + height);
            dl->AddRectFilled(a, z, i == 0 ? IM_COL32(40, 40, 40, 255) : IM_COL32(r, g, b, 255));
        }
    }
    ImGui::Dummy(ImVec2(width, height));
}

const IMG *PreviewSprite(int pal_idx)
{
    /* Prefer a marked sprite (the gore art about to be moved), then the
       selected one, then any sprite drawn with the palette. */
    for (IMG *im = (IMG *)g_doc->img_p; im; im = (IMG *)im->nxt_p)
        if ((im->flags & 1) && (int)im->palnum == pal_idx && im->data_p) return im;
    IMG *sel = g_doc->ilselected >= 0 ? get_img(g_doc->ilselected) : nullptr;
    if (sel && (int)sel->palnum == pal_idx) return sel;
    for (IMG *im = (IMG *)g_doc->img_p; im; im = (IMG *)im->nxt_p)
        if ((int)im->palnum == pal_idx && im->data_p) return im;
    return nullptr;
}

/* Marked sprites drawn with a ticked costume, i.e. what "move" would touch. */
int MarkedGoreSprites(void)
{
    int n = 0;
    for (IMG *im = (IMG *)g_doc->img_p; im; im = (IMG *)im->nxt_p) {
        if (!(im->flags & 1) || !im->data_p) continue;
        for (const GenRow &r : s_rows)
            if (r.effect < 0 && r.on && r.src == (int)im->palnum) { n++; break; }
    }
    return n;
}

void Generate(void)
{
    std::vector<unsigned short> blood;
    std::string blood_error;
    BloodWords(blood, blood_error);
    std::vector<RowResult> results;
    std::vector<int> rows;
    for (int i = 0; i < (int)s_rows.size(); i++) {
        if (!s_rows[i].on) continue;
        RowResult res = Evaluate(i, blood, blood_error);
        if (!res.error.empty()) continue;
        rows.push_back(i);
        results.push_back(std::move(res));
    }
    if (rows.empty()) return;

    doc_undo_push();
    int added = 0, replaced = 0;
    std::vector<int> dest_of(s_usage.size() + 1, -1);   /* costume palette -> its BLUD */
    for (size_t k = 0; k < rows.size(); k++) {
        const GenRow &r = s_rows[rows[k]];
        const RowResult &res = results[k];
        PAL *src = get_pal(r.src);
        const int n = (int)res.words.size();
        unsigned char *buf = (unsigned char *)PoolAlloc((size_t)n * 2);
        if (!src || !buf) { free(buf); continue; }
        for (int i = 0; i < n; i++) {
            buf[i * 2] = (unsigned char)(res.words[i] & 0xff);
            buf[i * 2 + 1] = (unsigned char)(res.words[i] >> 8);
        }
        int dst_idx = res.existing;
        PAL *dst = dst_idx >= 0 ? get_pal(dst_idx) : nullptr;
        if (dst) {
            free(dst->data_p);
            replaced++;
        } else {
            dst = AllocPal();
            if (!dst) { free(buf); continue; }
            dst_idx = (int)g_doc->palcnt - 1;
            dst->flags = (unsigned char)(src->flags & ~1u);   /* not marked */
            dst->pad = 0;
            snprintf(dst->n_s, sizeof(dst->n_s), "%s", r.name);
            added++;
        }
        dst->data_p = buf;
        dst->numc = (unsigned short)n;
        dst->bitspix = (unsigned char)res.bitspix;
        if (r.effect < 0 && r.src < (int)dest_of.size()) dest_of[r.src] = dst_idx;
    }

    int moved = 0;
    if (s_move_marked && s_plan_ok) {
        int idx = 0;
        for (IMG *im = (IMG *)g_doc->img_p; im; im = (IMG *)im->nxt_p, idx++) {
            if (!(im->flags & 1) || !im->data_p || im->palnum >= dest_of.size()) continue;
            int dst = dest_of[im->palnum];
            if (dst < 0) continue;
            unsigned char *px = (unsigned char *)im->data_p;
            int stride = (im->w + 3) & ~3;
            for (int y = 0; y < (int)im->h; y++)
                for (int x = 0; x < (int)im->w; x++) px[y * stride + x] = s_plan.map[px[y * stride + x]];
            im->palnum = (unsigned short)dst;
            InvalidateThumb(idx);
            moved++;
        }
    }

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    InvalidatePaletteSync();
    InvalidatePaletteUsage();
    g_img_tex_idx = -2;
    mark_dirty();
    if (moved)
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Generated %d palettes (%d new, %d replaced), moved %d sprite%s",
                 added + replaced, added, replaced, moved, moved == 1 ? "" : "s");
    else
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Generated %d palette%s (%d new, %d replaced)",
                 added + replaced, added + replaced == 1 ? "" : "s", added, replaced);
    g_restore_msg_timer = 4.0f;
    s_open = false;
}

/* One table of rows: either the effect rows or the blood rows. Returns the
   number of rows that will be generated. */
int RowTable(const char *id, bool effects, std::vector<RowResult> &results)
{
    int ready = 0;
    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingFixedFit;
    if (!ImGui::BeginTable(id, effects ? 6 : 5, flags)) return 0;
    ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 24);
    ImGui::TableSetupColumn(effects ? "Effect" : "Costume", ImGuiTableColumnFlags_WidthFixed, 150);
    if (effects) ImGui::TableSetupColumn("Gradient", ImGuiTableColumnFlags_WidthFixed, 110);
    ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthFixed, 96);
    ImGui::TableSetupColumn("Result", ImGuiTableColumnFlags_WidthFixed, 200);
    ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    for (int i = 0; i < (int)s_rows.size(); i++) {
        GenRow &r = s_rows[i];
        if ((r.effect >= 0) != effects) continue;
        const RowResult &res = results[i];
        ImGui::PushID(i);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Checkbox("##on", &r.on);
        ImGui::TableNextColumn();
        PAL *src = get_pal(r.src);
        const char *label = effects ? kCharacterEffects[r.effect].label : (src ? src->n_s : "?");
        if (ImGui::Selectable(label, s_preview == i)) s_preview = i;
        if (effects) {
            ImGui::TableNextColumn();
            ImGui::SetNextItemWidth(-1);
            if (ImGui::BeginCombo("##preset", r.preset >= 0 ? GradientPresetName(r.preset) : "(none)")) {
                for (int p = 0; p < GradientPresetCount(); p++)
                    if (ImGui::Selectable(GradientPresetName(p), p == r.preset)) {
                        r.preset = p;
                        r.on = true;
                        s_preview = i;
                    }
                ImGui::EndCombo();
            }
        }
        ImGui::TableNextColumn();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::InputText("##name", r.name, PALETTE_NAME_MAX + 1, ImGuiInputTextFlags_CharsUppercase))
            r.name_edited = true;
        ImGui::TableNextColumn();
        Strip(res.words, 200, ImGui::GetFrameHeight() - 4);
        ImGui::TableNextColumn();
        if (!r.on)                     ImGui::TextDisabled("off");
        else if (!res.error.empty())   ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s", res.error.c_str());
        else {
            ready++;
            ImGui::TextUnformatted(res.existing >= 0 ? "replaces" : "new");
            if (!res.note.empty()) { ImGui::SameLine(); ImGui::TextDisabled("%s", res.note.c_str()); }
        }
        ImGui::PopID();
    }
    ImGui::EndTable();
    return ready;
}

void BloodSourceCombo(void)
{
    ImGui::SetNextItemWidth(220);
    if (!ImGui::BeginCombo("Blood colors", BloodSourceLabel().c_str())) return;
    if (ImGui::Selectable("Built-in red ramp", s_blood_kind == BLOOD_FROM_RAMP)) {
        s_blood_kind = BLOOD_FROM_RAMP;
        s_blood_from = -1;
    }
    ImGui::SeparatorText("Last colors of a palette");
    int idx = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p, idx++) {
        ImGui::PushID(idx);
        if (ImGui::Selectable(p->n_s, s_blood_kind == BLOOD_FROM_PALETTE && s_blood_from == idx)) {
            s_blood_kind = BLOOD_FROM_PALETTE;
            s_blood_from = idx;
        }
        ImGui::PopID();
    }
    if (GradientPresetCount()) ImGui::SeparatorText("Saved gradient");
    for (int g = 0; g < GradientPresetCount(); g++) {
        ImGui::PushID(1000 + g);
        if (ImGui::Selectable(GradientPresetName(g), s_blood_kind == BLOOD_FROM_GRADIENT && s_blood_from == g)) {
            s_blood_kind = BLOOD_FROM_GRADIENT;
            s_blood_from = g;
        }
        ImGui::PopID();
    }
    ImGui::EndCombo();
}

} // namespace

void OpenCharacterPaletteGenerator(int pal_idx, CharacterPaletteSet set)
{
    PAL *pal = get_pal(pal_idx);
    if (!g_doc || !pal || !pal->data_p) return;
    EnsureGradientPresetsLoaded();
    s_doc = g_doc;
    s_source = pal_idx;
    s_set = set;
    snprintf(s_prefix, sizeof(s_prefix), "%s", CharacterPrefixOf(pal->n_s).c_str());
    s_rows.clear();
    s_preview = set == CHARACTER_PALETTES_BLOOD ? CHARACTER_EFFECT_COUNT : 0;
    s_plan_key.clear();
    CountUsage();
    BuildRows();
    PickDefaultBloodSource();
    s_open = true;
}

void DrawCharacterPaletteGenerator(void)
{
    if (!s_open) return;
    PAL *source = get_pal(s_source);
    if (g_doc != s_doc || !source) { s_open = false; return; }

    ImGui::SetNextWindowSize(ImVec2(820, 700), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Generate Character Palettes", &s_open)) { ImGui::End(); return; }

    ImGui::Text("Source: %s  (%u colors)", source->n_s, source->numc);
    ImGui::SameLine(0, 24);
    ImGui::SetNextItemWidth(60);
    if (ImGui::InputText("Fighter prefix", s_prefix, 5, ImGuiInputTextFlags_CharsUppercase))
        BuildRows();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Names start with this (JC -> JCFREZ_P). Costume palettes for\n"
        "the blood rows are every palette whose name starts with it.");

    UpdatePlan();
    std::vector<unsigned short> blood;
    std::string blood_error;
    BloodWords(blood, blood_error);
    std::vector<RowResult> results;
    results.reserve(s_rows.size());
    for (int i = 0; i < (int)s_rows.size(); i++) results.push_back(Evaluate(i, blood, blood_error));

    ImGui::SeparatorText("Freeze / electric palettes");
    ImGui::TextDisabled("%s fitted by brightness onto each saved Indexed Color Gradient ramp.", source->n_s);
    int ready = RowTable("##effects", true, results);

    ImGui::SeparatorText("Blood palettes");
    ImGui::SetNextItemWidth(90);
    if (ImGui::InputInt("Blood colors needed", &s_blood_slots)) {
        int max_slots = (int)source->numc - 2;
        if (s_blood_slots > max_slots) s_blood_slots = max_slots;
        if (s_blood_slots < 1) s_blood_slots = 1;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Slots the gore needs at the end of the palette: the blood ramp, plus\n"
        "bone for torso rips. MK2 uses 9-13 (Kitana 12, Johnny Cage 13).");
    ImGui::SameLine(0, 16);
    BloodSourceCombo();
    Strip(blood, ImGui::GetContentRegionAvail().x * 0.5f, 10);
    if (s_plan_ok) {
        ImGui::TextDisabled("Costume packed into %d of %d slots: %d unused slot%s dropped, %d merged",
                            (int)s_plan.reps.size(), s_plan.numc - s_plan.blood - 1,
                            s_plan.dropped, s_plan.dropped == 1 ? "" : "s", s_plan.merged);
        if (s_plan.changed_pixels) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(%llu px change color)", s_plan.changed_pixels);
        }
    }
    ready += RowTable("##blood", false, results);
    int gore = MarkedGoreSprites();
    ImGui::BeginDisabled(gore == 0 || !s_plan_ok);
    char move_label[96];
    snprintf(move_label, sizeof(move_label), "Move %d marked sprite%s onto the blood palettes",
             gore, gore == 1 ? "" : "s");
    ImGui::Checkbox(move_label, &s_move_marked);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(
        "Gore art cut from the fighter (a ripped torso) still indexes the full\n"
        "costume. This remaps its pixels into the packed layout and puts each\n"
        "sprite on the BLUD palette of the costume it was drawn with.");

    ImGui::Spacing();
    ImGui::Checkbox("Replace palettes that already have these names", &s_replace);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
        "Overwrite in place, so sprites already using JCFREZ_P keep using it.\n"
        "Off: rows whose name exists are skipped.");

    if (s_preview >= 0 && s_preview < (int)s_rows.size()) {
        const GenRow &r = s_rows[s_preview];
        const RowResult &res = results[s_preview];
        PAL *src = get_pal(r.src);
        ImGui::SeparatorText("Preview");
        ImGui::Text("%s -> %s", src ? src->n_s : "?", r.name);
        float w = ImGui::GetContentRegionAvail().x;
        std::vector<unsigned short> before = src ? PalettePreviewWords(src) : std::vector<unsigned short>();
        Strip(before, w, 12);
        Strip(res.words, w, 12);
        const IMG *img = PreviewSprite(r.src);
        if (img && !res.words.empty() && res.error.empty()) {
            /* A blood palette's slots moved: draw the sprite remapped. */
            const unsigned char *map = r.effect < 0 && s_plan_ok ? s_plan.map : nullptr;
            float pane = (w - 8) * 0.5f;
            float pane_h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.5f;
            if (pane_h < 80) pane_h = 80;
            PalettePreviewSprite(0, "Before", img, before, nullptr, pane, pane_h);
            ImGui::SameLine();
            PalettePreviewSprite(1, map ? "After (remapped)" : "After", img, res.words, map, pane, pane_h);
        } else if (!img) {
            ImGui::TextDisabled("No sprite uses %s.", src ? src->n_s : "this palette");
        }
    }

    ImGui::Separator();
    char label[48];
    snprintf(label, sizeof(label), "Generate %d Palette%s", ready, ready == 1 ? "" : "s");
    ImGui::BeginDisabled(ready == 0);
    if (ImGui::Button(label, ImVec2(180, 0))) Generate();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0))) s_open = false;
    ImGui::End();
}
