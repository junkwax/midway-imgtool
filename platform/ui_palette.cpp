/*************************************************************
 * platform/ui_palette.cpp
 * Palette editor, HSL adjustments, and quantization UI.
 *
 * Part of the Phase C overlay split.
 *************************************************************/
#include "ui_palette.h"
#include "ui_internal.h"
#include "ui_timeline.h" // for InvalidateThumb
#include "img_io.h"      // for g_restore_msg, g_restore_msg_timer
#include "palette_math.h"
#include "color_ops.h"
#include "shim_vid.h"     // for g_palette
#include "img_util.h"
#include "document.h"     // for g_doc

#include <imgui.h>
#include <vector>
#include <algorithm>
#include <cstdio>
#include <cmath>
#include <string>
#include <cstring>
#include <cstdlib>
#include <cctype>

struct PaletteCleanupResult {
    int removed;
    int sorted;
    int moved;
    bool changed;
};


/* ---- Local Palette Reduction / Merge Dialog State ---- */
static bool         g_show_palette_reduce = false;
static int          g_palette_reduce_bpp = 7;
static int          g_palette_reduce_preview_idx = -1;
static SDL_Texture *g_palette_reduce_orig_tex = NULL;
static SDL_Texture *g_palette_reduce_new_tex = NULL;
static int          g_palette_reduce_tex_w = 0;
static int          g_palette_reduce_tex_h = 0;
static int          g_palette_reduce_tex_img = -1;
static int          g_palette_reduce_tex_pal = -1;
static int          g_palette_reduce_tex_bpp = 0;

static bool g_show_palette_merge_quality = false;
static PaletteMergeQuality g_palette_merge_quality = {};
static bool g_palette_merge_preview_only = false;
static bool g_merge_opt_grow = true;
static bool g_merge_opt_perceptual = false;

/* Single-color palette ramp dialog. Preview deliberately writes the palette
   from this private baseline, then Cancel restores it without creating an
   undo step. Apply restores first, snapshots undo, then writes the ramp. */
static bool g_show_palette_single_color = false;
static bool g_palette_single_color_ready = false;
static unsigned char g_palette_single_color_baseline[512] = {};
static int g_palette_single_color_count = 0;
static int g_palette_single_color_idx = -1;
static float g_palette_single_color_rgb[3] = { 1.0f, 0.48f, 0.04f };
static float g_palette_single_color_opacity = 1.0f;
static float g_palette_single_color_brightness = 0.0f;
static float g_palette_single_color_contrast = 0.0f;
static void ApplySingleColorRamp(const unsigned char *baseline, int count,
                                 float r, float g, float b, float opacity,
                                 float brightness, float contrast, unsigned char *out);

/* Multi-stop palette gradient. It changes selected palette words only; sprite
   pixels keep their original index values and therefore their exact mapping. */
static bool g_show_indexed_gradient = false;
static float g_indexed_gradient_colors[11][3] = {};
static bool g_indexed_gradient_color_set[11] = {};
static int g_indexed_gradient_color_count = 2;
static int g_indexed_gradient_palette_idx = -1;
static bool g_indexed_gradient_targets[256] = {};
static unsigned char g_indexed_gradient_baseline[512] = {};
static int g_indexed_gradient_palette_count = 0;
static int g_indexed_gradient_image_idx = -1;
static SDL_Texture *g_indexed_gradient_preview_tex = NULL;
struct IndexedGradientPreset {
    std::string name;
    std::vector<ImVec4> colors;
};
static std::vector<IndexedGradientPreset> g_indexed_gradient_presets;
static bool g_indexed_gradient_presets_loaded = false;
static bool g_indexed_gradient_applied = false;
static bool g_indexed_gradient_open_name_popup = false;
static char g_indexed_gradient_preset_name[96] = {};

static std::string IndexedGradientPresetPath(void)
{
    char *pref = SDL_GetPrefPath("midway", "imgtool");
    if (!pref) return "indexed_gradients.txt";
    std::string path(pref);
    SDL_free(pref);
    path += "indexed_gradients.txt";
    return path;
}

static void LoadIndexedGradientPresets(void)
{
    if (g_indexed_gradient_presets_loaded) return;
    g_indexed_gradient_presets_loaded = true;
    FILE *f = fopen(IndexedGradientPresetPath().c_str(), "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        char *sep1 = strchr(line, '|');
        if (!sep1) continue;
        *sep1++ = '\0';
        char *sep2 = strchr(sep1, '|');
        if (!sep2) continue;
        *sep2++ = '\0';
        int count = atoi(sep1);
        if (!line[0] || count < 2 || count > 11) continue;
        IndexedGradientPreset preset;
        preset.name = line;
        char *token = strtok(sep2, ",");
        while (token && (int)preset.colors.size() < count) {
            unsigned int rgb = 0;
            if (sscanf(token, "%06x", &rgb) != 1) break;
            preset.colors.push_back(ImVec4(((rgb >> 16) & 255) / 255.0f,
                                           ((rgb >> 8) & 255) / 255.0f,
                                           (rgb & 255) / 255.0f, 1.0f));
            token = strtok(NULL, ",");
        }
        if ((int)preset.colors.size() == count)
            g_indexed_gradient_presets.push_back(std::move(preset));
    }
    fclose(f);
}

static bool SaveIndexedGradientPresets(void)
{
    FILE *f = fopen(IndexedGradientPresetPath().c_str(), "w");
    if (!f) return false;
    for (const IndexedGradientPreset &preset : g_indexed_gradient_presets) {
        fprintf(f, "%s|%d|", preset.name.c_str(), (int)preset.colors.size());
        for (size_t i = 0; i < preset.colors.size(); i++) {
            const ImVec4 &c = preset.colors[i];
            int r = (int)lroundf(c.x * 255.0f);
            int g = (int)lroundf(c.y * 255.0f);
            int b = (int)lroundf(c.z * 255.0f);
            fprintf(f, "%s%02X%02X%02X", i ? "," : "", r, g, b);
        }
        fputc('\n', f);
    }
    bool ok = ferror(f) == 0;
    fclose(f);
    return ok;
}

static void SelectIndexedGradientPreset(const IndexedGradientPreset &preset)
{
    g_indexed_gradient_color_count = (int)preset.colors.size();
    for (int i = 0; i < g_indexed_gradient_color_count; i++) {
        g_indexed_gradient_colors[i][0] = preset.colors[i].x;
        g_indexed_gradient_colors[i][1] = preset.colors[i].y;
        g_indexed_gradient_colors[i][2] = preset.colors[i].z;
        g_indexed_gradient_color_set[i] = true;
    }
    g_indexed_gradient_applied = false;
}

/* g_show_histogram is defined in ui_state.cpp */
static float g_histogram_data[256] = {0};
static float g_histogram_max = 0.0f;
static int   g_histogram_img_count = 0;

/* Palette usage serial / cache state */
static unsigned long long g_palette_usage_counts[256] = {0};
static unsigned int       g_palette_usage_serial = 1;
static unsigned int       g_palette_usage_built_serial = 0;
static Document          *g_palette_usage_doc = NULL;
static void              *g_palette_usage_img_head = NULL;
static unsigned int       g_palette_usage_imgcnt_seen = 0;
static int                g_palette_usage_pal_idx = -2;
static int                g_palette_usage_pal_numc = 0;
static int                g_palette_usage_img_count = 0;
static int                g_palette_usage_used_colors = 0;   /* excludes #0 */
static int                g_palette_usage_unused_colors = 0; /* excludes #0 */
static int                g_palette_usage_low_colors = 0;    /* excludes #0 */
static int                g_palette_usage_low_threshold = 8;

enum class PaletteZeroRemapMode { None, Selection, CurrentImage };

/* ---- Private Algorithms & Helpers ---- */

static void make_unique_pal_name(char out[10])
{
    for (int n = 1; n < 100000000; n++) {
        char cand[10];
        snprintf(cand, sizeof(cand), "PAL%d", n);
        bool clash = false;
        for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
            if (strncmp(p->n_s, cand, 10) == 0) { clash = true; break; }
        }
        if (!clash) { memcpy(out, cand, 10); return; }
    }
    out[0] = '\0';
}

/* Derive a numbered name from an existing palette name, e.g. LKALT_P ->
   LKALT1_P -> LKALT2_P. The trailing "_p"/"_P" suffix (the palette marker) is
   kept at the end and the counter is inserted before it; any digits already on
   the stem are stripped first so re-duplicating advances the number instead of
   appending. Names are capped at 9 chars (n_s[10]), so the stem is truncated to
   make room for the counter and suffix. Falls back to PAL<n> for blank names. */
static void make_numbered_pal_name(const char *base, char out[10])
{
    int len = 0;
    while (len < 9 && base[len] != '\0') len++;

    char suffix[3] = {0};
    int core_len = len;
    if (len >= 2 && base[len - 2] == '_' &&
        (base[len - 1] == 'p' || base[len - 1] == 'P')) {
        suffix[0] = '_';
        suffix[1] = base[len - 1];
        core_len = len - 2;
    }
    while (core_len > 0 && base[core_len - 1] >= '0' && base[core_len - 1] <= '9')
        core_len--;

    char stem[10] = {0};
    if (core_len <= 0) {
        memcpy(stem, "PAL", 3);
        core_len = 3;
    } else {
        memcpy(stem, base, (size_t)core_len);
    }

    int suf_len = (int)strlen(suffix);
    for (int n = 1; n < 100000000; n++) {
        char num[12];
        int num_len = snprintf(num, sizeof(num), "%d", n);
        int avail = 9 - num_len - suf_len;          /* room left for the stem */
        if (avail < 0) avail = 0;
        int use_stem = core_len < avail ? core_len : avail;
        char cand[10];
        snprintf(cand, sizeof(cand), "%.*s%s%s", use_stem, stem, num, suffix);
        bool clash = false;
        for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
            if (strncmp(p->n_s, cand, 10) == 0) { clash = true; break; }
        }
        if (!clash) {
            memset(out, 0, 10);
            memcpy(out, cand, strlen(cand));
            return;
        }
    }
    out[0] = '\0';
}

static unsigned short MergedSlotWord(const unsigned char *target_colors, int base_count,
                                     const unsigned short *added, int slot)
{
    if (slot < base_count)
        return (unsigned short)(target_colors[slot * 2] |
                                (target_colors[slot * 2 + 1] << 8));
    return added ? added[slot - base_count] : 0;
}

static void BuildPaletteMergeRemap(const PAL *src, const PAL *target, int base_count,
                                   const unsigned short *added, int added_count,
                                   bool perceptual, unsigned char remap[256])
{
    memset(remap, 0, 256);
    if (!src || !target || !src->data_p || !target->data_p) return;

    int src_count = (int)src->numc;
    if (src_count > 256) src_count = 256;
    const unsigned char *src_colors = (const unsigned char *)src->data_p;
    for (int si = 1; si < src_count; si++) {
        unsigned short sw = (unsigned short)(src_colors[si * 2] |
                                             (src_colors[si * 2 + 1] << 8));
        remap[si] = (unsigned char)FindNearestMergedSlot(target, base_count,
                                                         added, added_count,
                                                         sw, perceptual);
    }
}

static bool PaletteMergeQualityHasDrift(const PaletteMergeQuality &q)
{
    return q.color_drift_pixels > 0 ||
           q.transparent_drift_pixels > 0 ||
           q.invalid_pixels > 0 ||
           q.ppp_warning_images > 0;
}

static bool MergeTargetHasColor(const unsigned char *td, int base_count, unsigned short word)
{
    for (int i = 1; i < base_count; i++) {
        unsigned short w = (unsigned short)(td[i * 2] | (td[i * 2 + 1] << 8));
        if (w == word) return true;
    }
    return false;
}

static int BuildMergeAdditions(int target_idx, int target_base_numc, bool grow,
                               unsigned short added[256], int *overflow_out)
{
    if (overflow_out) *overflow_out = 0;
    PAL *target = (target_idx >= 0) ? get_pal(target_idx) : NULL;
    if (!grow || !target || !target->data_p) return 0;
    if (target_base_numc > 256) target_base_numc = 256;
    const unsigned char *td = (const unsigned char *)target->data_p;
    int free_slots = 256 - target_base_numc;

    int added_count = 0, overflow = 0;
    int pal_idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, pal_idx++) {
        if (!(pal->flags & 1) || pal_idx == target_idx || !pal->data_p || pal->numc == 0)
            continue;
        int src_count = (int)pal->numc;
        if (src_count > 256) src_count = 256;
        const unsigned char *sd = (const unsigned char *)pal->data_p;

        bool used[256] = {false};
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if ((int)img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0)
                continue;
            int stride = (img->w + 3) & ~3;
            const unsigned char *pixels = (const unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++)
                for (int x = 0; x < img->w; x++)
                    used[pixels[y * stride + x]] = true;
        }

        for (int si = 1; si < src_count; si++) {
            if (!used[si]) continue;
            unsigned short w = (unsigned short)(sd[si * 2] | (sd[si * 2 + 1] << 8));
            if (MergeTargetHasColor(td, target_base_numc, w)) continue;
            bool dup = false;
            for (int j = 0; j < added_count; j++)
                if (added[j] == w) { dup = true; break; }
            if (dup) continue;
            if (added_count < free_slots) added[added_count++] = w;
            else overflow++;
        }
    }
    if (overflow_out) *overflow_out = overflow;
    return added_count;
}

static bool BuildMarkedPaletteMergeQuality(PaletteMergeQuality *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->target_idx = g_doc->plselected;
    out->opt_grow = g_merge_opt_grow;
    out->opt_perceptual = g_merge_opt_perceptual;

    PAL *target = (out->target_idx >= 0) ? get_pal(out->target_idx) : NULL;
    if (!target || !target->data_p || target->numc == 0) return false;
    snprintf(out->target_name, sizeof(out->target_name), "%.9s", target->n_s);

    int base_count = (int)target->numc;
    if (base_count > 256) base_count = 256;
    out->target_base_numc = base_count;
    const unsigned char *target_colors = (const unsigned char *)target->data_p;

    out->colors_added = BuildMergeAdditions(out->target_idx, base_count, out->opt_grow,
                                            out->added_words, &out->colors_overflow);
    int final_count = base_count + out->colors_added;

    int ppp_limit = (g_load2_ppp > 0 && g_load2_ppp <= 8) ? (1 << g_load2_ppp) : 0;

    int pal_idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, pal_idx++) {
        if (!(pal->flags & 1) || pal == target || !pal->data_p || pal->numc == 0)
            continue;

        out->source_palettes++;

        unsigned char remap[256];
        BuildPaletteMergeRemap(pal, target, base_count, out->added_words,
                               out->colors_added, out->opt_perceptual, remap);

        int src_count = (int)pal->numc;
        if (src_count > 256) src_count = 256;
        const unsigned char *src_colors = (const unsigned char *)pal->data_p;

        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if ((int)img->palnum != pal_idx) continue;
            out->remapped_images++;
            if (ppp_limit > 0 && (int)pal->numc <= ppp_limit &&
                final_count > ppp_limit)
                out->ppp_warning_images++;

            if (!img->data_p || img->w == 0 || img->h == 0) continue;
            int stride = (img->w + 3) & ~3;
            const unsigned char *pixels = (const unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char ci = pixels[y * stride + x];
                    if (ci == 0) continue;
                    out->affected_pixels++;

                    if ((int)ci >= src_count) {
                        out->invalid_pixels++;
                        out->transparent_drift_pixels++;
                        continue;
                    }

                    unsigned char mapped = remap[ci];
                    if (mapped == 0 || (int)mapped >= final_count) {
                        out->transparent_drift_pixels++;
                        continue;
                    }

                    unsigned short sw = (unsigned short)(src_colors[ci * 2] |
                                                         (src_colors[ci * 2 + 1] << 8));
                    unsigned short dw = MergedSlotWord(target_colors, base_count,
                                                       out->added_words, mapped);
                    int dist = PaletteColorDistance5(sw, dw);
                    out->total_dist += dist;
                    if (dist == 0) {
                        out->exact_pixels++;
                    } else {
                        out->color_drift_pixels++;
                        if (dist > out->max_dist) {
                            out->max_dist = dist;
                            out->max_src_slot = (int)ci;
                            out->max_dst_slot = (int)mapped;
                            snprintf(out->max_palette, sizeof(out->max_palette), "%.9s", pal->n_s);
                            snprintf(out->max_image, sizeof(out->max_image), "%.15s", img->n_s);
                        }
                    }
                }
            }
        }
    }

    return out->source_palettes > 0;
}

static void DrawPaletteMergeMappingPreview(const PaletteMergeQuality &q)
{
    PAL *target = get_pal(q.target_idx);
    if (!target || !target->data_p) return;
    int base_count = q.target_base_numc;
    if (base_count > 256) base_count = 256;
    int final_count = base_count + q.colors_added;
    const unsigned char *target_data = (const unsigned char *)target->data_p;

    ImGui::TextDisabled("Swatch top = source color, bottom = mapped target color "
                        "(green border = added as a new color).");
    ImGui::BeginChild("##pal_merge_preview", ImVec2(560, 210), true);
    int pal_idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, pal_idx++) {
        if (!(pal->flags & 1) || pal_idx == q.target_idx ||
            !pal->data_p || pal->numc <= 1)
            continue;

        ImGui::Text("%.9s -> %.9s", pal->n_s, q.target_name);
        unsigned char remap[256];
        BuildPaletteMergeRemap(pal, target, base_count, q.added_words,
                               q.colors_added, q.opt_perceptual, remap);
        const unsigned char *src_data = (const unsigned char *)pal->data_p;
        int src_count = (int)pal->numc;
        if (src_count > 256) src_count = 256;

        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec2 base = ImGui::GetCursorScreenPos();
        const float sw = 14.0f;
        const float gap = 2.0f;
        const int cols = 16;
        int shown = src_count - 1;
        int rows = (shown + cols - 1) / cols;
        if (rows < 1) rows = 1;

        for (int si = 1; si < src_count; si++) {
            int k = si - 1;
            int row = k / cols;
            int col = k % cols;
            ImVec2 p0(base.x + col * (sw + gap), base.y + row * (sw + gap));
            ImVec2 p1(p0.x + sw, p0.y + sw);

            unsigned char sr = 0, sg = 0, sb = 0;
            pal_word_to_rgb8(src_data + si * 2, &sr, &sg, &sb);
            unsigned char mapped = remap[si];
            bool valid = mapped > 0 && (int)mapped < final_count;
            bool is_added = valid && (int)mapped >= base_count;

            unsigned short src_word =
                (unsigned short)(src_data[si * 2] | (src_data[si * 2 + 1] << 8));
            unsigned short dst_word = valid
                ? MergedSlotWord(target_data, base_count, q.added_words, mapped)
                : 0;
            unsigned char dst_bytes[2] = {
                (unsigned char)(dst_word & 0xFF), (unsigned char)(dst_word >> 8) };
            unsigned char dr = 0, dg = 0, db = 0;
            if (valid) pal_word_to_rgb8(dst_bytes, &dr, &dg, &db);

            dl->AddRectFilled(p0, p1, IM_COL32(sr, sg, sb, 255));
            dl->AddRectFilled(ImVec2(p0.x, p0.y + sw - 4.0f), p1,
                              valid ? IM_COL32(dr, dg, db, 255)
                                    : IM_COL32(255, 0, 0, 255));

            int dist = valid ? PaletteColorDistance5(src_word, dst_word) : 9999;
            ImU32 border = is_added  ? IM_COL32(80, 200, 120, 255)
                          : dist == 0 ? IM_COL32(70, 90, 110, 220)
                          : dist < 36 ? IM_COL32(255, 190, 60, 255)
                                      : IM_COL32(255, 80, 80, 255);
            dl->AddRect(p0, p1, border);

            ImGui::SetCursorScreenPos(p0);
            ImGui::PushID(pal_idx * 1000 + si);
            ImGui::InvisibleButton("##map", ImVec2(sw, sw));
            if (ImGui::IsItemHovered()) {
                if (is_added) {
                    ImGui::SetTooltip("%.9s #%d -> %.9s #%d (added, exact)",
                                      pal->n_s, si, q.target_name, (int)mapped);
                } else if (valid) {
                    ImGui::SetTooltip("%.9s #%d -> %.9s #%d\nRGB drift %.2f",
                                      pal->n_s, si, q.target_name, (int)mapped,
                                      sqrt((double)dist));
                } else {
                    ImGui::SetTooltip("%.9s #%d would map to transparent/invalid",
                                      pal->n_s, si);
                }
            }
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(cols * (sw + gap), rows * (sw + gap)));
        ImGui::Spacing();
    }
    ImGui::EndChild();
}

static bool PalettesAreIdentical(const PAL *a, const PAL *b)
{
    if (!a || !b || !a->data_p || !b->data_p) return false;
    if (a->bitspix != b->bitspix || a->numc != b->numc) return false;
    if (a->numc == 0) return false;
    return memcmp(a->data_p, b->data_p, (size_t)a->numc * 2) == 0;
}

int MergeDuplicatePalettes(void)
{
    int n_pals = count_pals();
    if (n_pals < 2) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No duplicate palettes found.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    std::vector<PAL *> pals;
    std::vector<int> duplicate_to;
    pals.reserve(n_pals);
    duplicate_to.assign(n_pals, -1);
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p)
        pals.push_back(p);

    int duplicates = 0;
    for (int i = 0; i < (int)pals.size(); i++) {
        for (int j = 0; j < i; j++) {
            if (PalettesAreIdentical(pals[i], pals[j])) {
                duplicate_to[i] = j;
                duplicates++;
                break;
            }
        }
    }

    if (duplicates == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No duplicate palettes found.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    doc_undo_push();

    int remapped_images = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        int pal_idx = (int)img->palnum;
        if (pal_idx >= 0 && pal_idx < (int)duplicate_to.size() && duplicate_to[pal_idx] >= 0) {
            img->palnum = (unsigned short)duplicate_to[pal_idx];
            remapped_images++;
        }
    }
    if (g_doc->plselected >= 0 && g_doc->plselected < (int)duplicate_to.size() &&
        duplicate_to[g_doc->plselected] >= 0)
        g_doc->plselected = duplicate_to[g_doc->plselected];

    PAL *prev = NULL;
    PAL *cur = (PAL *)g_doc->pal_p;
    int original_idx = 0;
    int current_idx = 0;
    while (cur) {
        if (original_idx < (int)duplicate_to.size() && duplicate_to[original_idx] >= 0) {
            PAL *to_del = cur;
            if (prev) prev->nxt_p = cur->nxt_p;
            else g_doc->pal_p = cur->nxt_p;
            cur = (PAL *)cur->nxt_p;
            g_doc->palcnt--;

            for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > current_idx) img->palnum--;
            }
            if ((int)g_doc->plselected > current_idx) g_doc->plselected--;

            FreePal(to_del);
        } else {
            prev = cur;
            cur = (PAL *)cur->nxt_p;
            current_idx++;
        }
        original_idx++;
    }

    if ((unsigned)g_doc->plselected >= g_doc->palcnt)
        g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;
    ApplyPalette(g_doc->plselected);
    g_img_tex_idx = -2;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Merged %d duplicate palette%s into first match%s; quality check OK.",
             duplicates, duplicates == 1 ? "" : "s",
             remapped_images ? "" : " (no sprites remapped)");
    g_restore_msg_timer = 4.0f;
    return duplicates;
}

static int FindMarkedPaletteExcept(int except_idx)
{
    int idx = 0;
    for (PAL *pal = (PAL *)g_doc->pal_p; pal; pal = (PAL *)pal->nxt_p, idx++) {
        if (idx != except_idx && (pal->flags & 1))
            return idx;
    }
    return -1;
}

int InheritSelectedPaletteFromMarked(void)
{
    int target_idx = g_doc->plselected;
    PAL *target = (target_idx >= 0) ? get_pal(target_idx) : NULL;
    int source_idx = FindMarkedPaletteExcept(target_idx);
    PAL *source = (source_idx >= 0) ? get_pal(source_idx) : NULL;

    if (!target || !target->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a target palette first.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }
    if (!source || !source->data_p || source->numc <= 1) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark one source palette, then select the palette to inherit into.");
        g_restore_msg_timer = 5.0f;
        return 0;
    }

    commit_palette_adjustments();

    unsigned char remap[256] = {0};
    const unsigned char *target_data = (const unsigned char *)target->data_p;
    int target_colors = target->numc;
    if (target_colors > 256) target_colors = 256;
    for (int i = 1; i < target_colors; i++) {
        unsigned short w = (unsigned short)(target_data[i * 2] | (target_data[i * 2 + 1] << 8));
        remap[i] = (unsigned char)FindNearestPaletteSlot(source, w);
    }

    int source_colors = source->numc;
    if (source_colors > 256) source_colors = 256;
    size_t source_bytes = (size_t)source_colors * 2;
    void *new_data = malloc(source_bytes);
    if (!new_data) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not allocate inherited palette data.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }
    memcpy(new_data, source->data_p, source_bytes);

    doc_undo_push();

    int pixels_changed = 0;
    int images_touched = 0;
    int img_idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        if ((int)img->palnum != target_idx) continue;
        images_touched++;
        if (img->data_p && img->w > 0 && img->h > 0) {
            int stride = (img->w + 3) & ~3;
            unsigned char *pix = (unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *p = pix + y * stride + x;
                    unsigned char mapped = (*p < target_colors) ? remap[*p] : 0;
                    if (*p != 0 && *p != mapped) {
                        *p = mapped;
                        pixels_changed++;
                    }
                }
            }
        }
        InvalidateThumb(img_idx);
    }

    free(target->data_p);
    target->data_p = new_data;
    target->numc = (unsigned short)source_colors;
    target->bitspix = source->bitspix;
    target->pad = source->pad;

    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    if (g_sel_color >= (int)target->numc)
        g_sel_color = target->numc > 1 ? (int)target->numc - 1 : 0;
    ApplyPalette(target_idx);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Inherited %.9s into %.9s: %d image%s, %d pixel%s remapped.",
             source->n_s, target->n_s,
             images_touched, images_touched == 1 ? "" : "s",
             pixels_changed, pixels_changed == 1 ? "" : "s");
    g_restore_msg_timer = 5.0f;
    return pixels_changed;
}

static void palette_writeback(int color_idx)
{
    mark_dirty();
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (color_idx < 0 || color_idx >= (int)pal->numc) return;

    SDL_Color &c = g_palette[color_idx];
    rgb8_to_pal_word(c.r, c.g, c.b, (unsigned char *)pal->data_p + color_idx * 2);
}

struct PaletteColorClipboard {
    bool valid = false;
    SDL_Color color = {0, 0, 0, 255};
};
static PaletteColorClipboard g_palette_color_clipboard;

void CopySelectedPaletteColor(void)
{
    if (g_sel_color < 0 || g_sel_color >= 256) return;
    g_palette_color_clipboard.color = g_palette[g_sel_color];
    g_palette_color_clipboard.valid = true;
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Copied palette color #%d (%d, %d, %d).", g_sel_color,
             g_palette[g_sel_color].r, g_palette[g_sel_color].g,
             g_palette[g_sel_color].b);
    g_restore_msg_timer = 3.0f;
}

bool PastePaletteColorAt(int color_idx)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!g_palette_color_clipboard.valid || !pal || !pal->data_p ||
        color_idx < 0 || color_idx >= (int)pal->numc)
        return false;
    if (g_palette[color_idx].r == g_palette_color_clipboard.color.r &&
        g_palette[color_idx].g == g_palette_color_clipboard.color.g &&
        g_palette[color_idx].b == g_palette_color_clipboard.color.b)
        return true;
    doc_undo_push();
    g_palette[color_idx] = g_palette_color_clipboard.color;
    palette_writeback(color_idx);
    InvalidatePaletteUsage();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Pasted copied color into palette index #%d.", color_idx);
    g_restore_msg_timer = 3.0f;
    return true;
}

/* ---- Multi-slot palette clipboard ----------------------------------
   Carries a set of swatches *with their index positions* so they can be
   dropped into a different palette at exactly the same indices — the point
   being that sprites keep rendering correctly, since their pixels reference
   indices, not colors.

   Distinct from the two clipboards that already exist: g_palette_color_clipboard
   holds one loose color pasteable at any index, and g_pal_clipboard holds a
   whole palette pasted as a new one. */
struct PaletteSlotClipboard {
    bool valid = false;
    int  count = 0;
    int  max_index = -1;
    bool has[256] = {};
    SDL_Color color[256] = {};
    char source_name[16] = {0};
};
static PaletteSlotClipboard g_palette_slot_clipboard;

int PaletteSlotClipboardCount(void)
{
    return g_palette_slot_clipboard.valid ? g_palette_slot_clipboard.count : 0;
}

int PaletteSlotClipboardMaxIndex(void)
{
    return g_palette_slot_clipboard.valid ? g_palette_slot_clipboard.max_index : -1;
}

const char *PaletteSlotClipboardSource(void)
{
    return g_palette_slot_clipboard.source_name;
}

/* How many swatches a copy would take right now: the Ctrl/Shift multi-select
   if there is one, otherwise the single highlighted swatch. */
int CountSelectedPaletteSlots(void)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return 0;
    int limit = (int)pal->numc < 256 ? (int)pal->numc : 256;
    int n = 0;
    for (int i = 0; i < limit; i++)
        if (g_palette_selection[i]) n++;
    if (n == 0 && g_sel_color >= 0 && g_sel_color < limit) n = 1;
    return n;
}

int CopySelectedPaletteSlots(void)
{
    commit_palette_adjustments();
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a palette first.");
        g_restore_msg_timer = 3.0f;
        return 0;
    }

    int limit = (int)pal->numc < 256 ? (int)pal->numc : 256;
    PaletteSlotClipboard clip;
    for (int i = 0; i < limit; i++) {
        if (!g_palette_selection[i]) continue;
        clip.has[i] = true;
        clip.color[i] = g_palette[i];
        clip.max_index = i;
        clip.count++;
    }
    /* Nothing multi-selected: fall back to the highlighted swatch so the
       command is never a silent no-op. */
    if (clip.count == 0 && g_sel_color >= 0 && g_sel_color < limit) {
        clip.has[g_sel_color] = true;
        clip.color[g_sel_color] = g_palette[g_sel_color];
        clip.max_index = g_sel_color;
        clip.count = 1;
    }
    if (clip.count == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Ctrl+click palette swatches to select colors first.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    clip.valid = true;
    snprintf(clip.source_name, sizeof(clip.source_name), "%.9s", pal->n_s);
    g_palette_slot_clipboard = clip;

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Copied %d color%s from %.9s (indices up to #%d).",
             clip.count, clip.count == 1 ? "" : "s", pal->n_s, clip.max_index);
    g_restore_msg_timer = 4.0f;
    return clip.count;
}

int PastePaletteSlotsAtSameIndices(int target_pal_idx)
{
    const PaletteSlotClipboard &clip = g_palette_slot_clipboard;
    if (!clip.valid || clip.count == 0) return 0;

    /* Flush any pending HSL slider edits into the selected palette before
       touching palette data, whichever palette we are about to write. */
    commit_palette_adjustments();
    int target_idx = (target_pal_idx >= 0) ? target_pal_idx : g_doc->plselected;
    PAL *pal = (target_idx >= 0) ? get_pal(target_idx) : NULL;
    if (!pal) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a target palette first.");
        g_restore_msg_timer = 3.0f;
        return 0;
    }
    /* Targeting a palette other than the selected one must not drag the live
       working table (or the current sprite's palette assignment) with it. */
    bool target_is_selected = (target_idx == g_doc->plselected);

    doc_undo_push();

    /* Pasting "at the same index" is only meaningful if the index exists, so
       grow a short target rather than dropping the tail colors. Undo covers
       the size change if that wasn't wanted. */
    int old_numc = (int)pal->numc;
    int need = clip.max_index + 1;
    bool grew = false;
    if (need > old_numc || !pal->data_p) {
        if (ensure_palette_numc(pal, need > old_numc ? need : old_numc))
            grew = (int)pal->numc > old_numc;
    }
    if (!pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not grow the target palette.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    int limit = (int)pal->numc < 256 ? (int)pal->numc : 256;
    unsigned char *pd = (unsigned char *)pal->data_p;
    int pasted = 0, skipped = 0;
    for (int i = 0; i < 256; i++) {
        if (!clip.has[i]) continue;
        if (i >= limit) { skipped++; continue; }
        rgb8_to_pal_word(clip.color[i].r, clip.color[i].g, clip.color[i].b,
                         pd + i * 2);
        pasted++;
    }

    /* Reload the live table from the PAL so any slots created by the grow are
       consistent with what was actually written. Only meaningful when the
       target is the palette the live table currently mirrors. */
    if (target_is_selected) {
        ApplyPalette(target_idx);
        save_palette_baseline();
        reset_palette_adjust_sliders();
    }
    InvalidatePaletteUsage();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
    mark_dirty();

    if (skipped > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Pasted %d color%s into %.9s at the same indices; %d skipped.",
                 pasted, pasted == 1 ? "" : "s", pal->n_s, skipped);
    } else if (grew) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Pasted %d color%s into %.9s at the same indices (grew %d -> %d colors).",
                 pasted, pasted == 1 ? "" : "s", pal->n_s, old_numc, (int)pal->numc);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Pasted %d color%s into %.9s at the same indices.",
                 pasted, pasted == 1 ? "" : "s", pal->n_s);
    }
    g_restore_msg_timer = 5.0f;
    return pasted;
}

int PastePaletteSlotsAppendToEnd(int target_pal_idx)
{
    const PaletteSlotClipboard &clip = g_palette_slot_clipboard;
    if (!clip.valid || clip.count == 0) return 0;

    commit_palette_adjustments();
    int target_idx = (target_pal_idx >= 0) ? target_pal_idx : g_doc->plselected;
    PAL *pal = (target_idx >= 0) ? get_pal(target_idx) : NULL;
    if (!pal) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a target palette first.");
        g_restore_msg_timer = 3.0f;
        return 0;
    }
    bool target_is_selected = (target_idx == g_doc->plselected);

    int old_numc = (int)pal->numc;
    if (old_numc < 0) old_numc = 0;
    int room = 256 - old_numc;
    if (room <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "%.9s already holds 256 colors — nothing to append to.", pal->n_s);
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    doc_undo_push();

    int want = clip.count < room ? clip.count : room;
    if (!ensure_palette_numc(pal, old_numc + want) || !pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Could not grow the target palette.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }

    /* Ascending source index, so a copied ramp keeps its dark-to-light order
       when it lands at the tail. */
    unsigned char *pd = (unsigned char *)pal->data_p;
    int appended = 0;
    for (int i = 0; i < 256 && appended < want; i++) {
        if (!clip.has[i]) continue;
        rgb8_to_pal_word(clip.color[i].r, clip.color[i].g, clip.color[i].b,
                         pd + (old_numc + appended) * 2);
        appended++;
    }

    if (target_is_selected) {
        ApplyPalette(target_idx);
        save_palette_baseline();
        reset_palette_adjust_sliders();
    }
    InvalidatePaletteUsage();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
    mark_dirty();

    int dropped = clip.count - appended;
    if (dropped > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Appended %d color%s to %.9s (#%d..#%d); %d did not fit in 256.",
                 appended, appended == 1 ? "" : "s", pal->n_s,
                 old_numc, old_numc + appended - 1, dropped);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Appended %d color%s to %.9s at #%d..#%d (%d -> %d colors).",
                 appended, appended == 1 ? "" : "s", pal->n_s,
                 old_numc, old_numc + appended - 1, old_numc, (int)pal->numc);
    }
    g_restore_msg_timer = 5.0f;
    return appended;
}

bool ApplyEyedropperColorToLockedSwatches(int source_color_idx)
{
    if (source_color_idx < 0 || source_color_idx >= 256) return false;
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return false;
    int locked = 0;
    for (int i = 0; i < (int)pal->numc && i < 256; i++)
        if (g_palette_selection[i]) locked++;
    if (locked == 0) return false;
    SDL_Color sampled = g_palette[source_color_idx];
    doc_undo_push();
    for (int i = 0; i < (int)pal->numc && i < 256; i++) {
        if (!g_palette_selection[i]) continue;
        g_palette[i] = sampled;
        palette_writeback(i);
    }
    InvalidatePaletteUsage();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Eyedropper updated %d locked palette swatch%s.",
             locked, locked == 1 ? "" : "es");
    g_restore_msg_timer = 3.0f;
    return true;
}

/* Right-click-on-canvas eyedropper for the Single-Color Shading window.
   Only handles the click (returning true) while that window is open, so
   the canvas's normal fill-color eyedropper still works otherwise. */
bool PickColorForSingleColorDialog(int source_color_idx)
{
    if (!g_show_palette_single_color) return false;
    if (source_color_idx < 0 || source_color_idx >= 256) return false;
    PAL *pal = (g_doc && g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return false;

    SDL_Color sampled = g_palette[source_color_idx];
    g_palette_single_color_rgb[0] = sampled.r / 255.0f;
    g_palette_single_color_rgb[1] = sampled.g / 255.0f;
    g_palette_single_color_rgb[2] = sampled.b / 255.0f;

    int n = (int)pal->numc;
    if (n > 256) n = 256;
    if (g_palette_single_color_ready && g_palette_single_color_idx == g_doc->plselected &&
        g_palette_single_color_count == n) {
        ApplySingleColorRamp(g_palette_single_color_baseline, n,
                             g_palette_single_color_rgb[0],
                             g_palette_single_color_rgb[1],
                             g_palette_single_color_rgb[2],
                             g_palette_single_color_opacity,
                             g_palette_single_color_brightness,
                             g_palette_single_color_contrast,
                             (unsigned char *)pal->data_p);
        ApplyPalette(g_doc->plselected);
        g_img_tex_idx = -2;
    }
    return true;
}

static int palette_sort_hue(int r, int g, int b)
{
    int maxv = r > g ? (r > b ? r : b) : (g > b ? g : b);
    int minv = r < g ? (r < b ? r : b) : (g < b ? g : b);
    int delta = maxv - minv;
    if (delta == 0) return 0;

    int h;
    if (maxv == r) {
        h = 256 * (g - b) / delta;
        if (h < 0) h += 1536;
    } else if (maxv == g) {
        h = 512 + 256 * (b - r) / delta;
    } else {
        h = 1024 + 256 * (r - g) / delta;
    }
    if (h < 0) h += 1536;
    if (h >= 1536) h -= 1536;
    return h;
}

/* Gradient: one global dark->light ramp (hues interleave by brightness).
   GroupByColor: cluster each hue family together, each as its own dark->light
   ramp, so "like colors" sit next to each other instead of being scattered. */
enum class PaletteSortMode { Gradient, GroupByColor };

static std::vector<int> BuildGradientPaletteOrder(PAL *pal, const bool used[256],
                                                  PaletteSortMode mode = PaletteSortMode::Gradient)
{
    struct PaletteSortColor {
        int old_idx;
        int r, g, b;
        int luma;
        int sat;
        int hue;
        int family;
    };

    std::vector<PaletteSortColor> colors;
    if (!pal || !pal->data_p) return {};

    int max_colors = pal->numc;
    if (max_colors > 256) max_colors = 256;
    const unsigned char *pd = (const unsigned char *)pal->data_p;

    for (int i = 1; i < max_colors; i++) {
        if (!used[i]) continue;
        unsigned char r, g, b;
        pal_word_to_rgb8(pd + i * 2, &r, &g, &b);
        PaletteSortColor c;
        c.old_idx = i;
        c.r = r;
        c.g = g;
        c.b = b;
        c.luma = c.r * 54 + c.g * 183 + c.b * 19;
        int maxv = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
        int minv = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
        c.sat = maxv - minv;
        c.hue = palette_sort_hue(c.r, c.g, c.b);
        c.family = (c.sat <= 18) ? 0 : 1 + ((c.hue + 32) % 1536) / 64;
        colors.push_back(c);
    }

    std::sort(colors.begin(), colors.end(), [mode](const PaletteSortColor& a, const PaletteSortColor& b) {
        if (mode == PaletteSortMode::GroupByColor) {
            /* Cluster by hue family first, then ramp dark->light within it. */
            if (a.family != b.family) return a.family < b.family;
            if (a.luma != b.luma) return a.luma < b.luma;
            if (a.sat != b.sat) return a.sat < b.sat;
            if (a.hue != b.hue) return a.hue < b.hue;
        } else {
            if (a.luma != b.luma) return a.luma < b.luma;
            if (a.sat != b.sat) return a.sat < b.sat;
            if (a.family != b.family) return a.family < b.family;
            if (a.hue != b.hue) return a.hue < b.hue;
        }
        if (a.r != b.r) return a.r < b.r;
        if (a.g != b.g) return a.g < b.g;
        if (a.b != b.b) return a.b < b.b;
        return a.old_idx < b.old_idx;
    });

    std::vector<int> order;
    order.reserve(colors.size());
    for (const PaletteSortColor& c : colors)
        order.push_back(c.old_idx);
    return order;
}

static int BuildPaletteUsedMask(int pal_idx, int old_numc, bool used[256])
{
    memset(used, 0, sizeof(bool) * 256);
    used[0] = true;
    if (old_numc > 256) old_numc = 256;

    int referenced_pixels = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        unsigned short stride = (img->w + 3) & ~3;
        unsigned char *pixels = (unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char idx = pixels[y * stride + x];
                if (idx < old_numc) used[idx] = true;
                referenced_pixels++;
            }
        }
    }

    if (referenced_pixels == 0) {
        for (int i = 1; i < old_numc; i++) used[i] = true;
    }
    return referenced_pixels;
}

static PaletteCleanupResult DeleteUnusedPaletteColors(PaletteSortMode mode = PaletteSortMode::Gradient)
{
    PaletteCleanupResult result = {0, 0, 0, false};
    if (g_doc->plselected < 0) return result;
    PAL *pal = get_pal(g_doc->plselected);
    if (!pal || !pal->data_p) return result;

    bool used[256] = {false};
    used[0] = true;
    int old_numc = pal->numc;
    if (old_numc > 256) old_numc = 256;

    BuildPaletteUsedMask(g_doc->plselected, old_numc, used);

    std::vector<int> order = BuildGradientPaletteOrder(pal, used, mode);
    int new_numc = (int)order.size() + 1;
    result.sorted = (int)order.size();

    result.removed = old_numc - new_numc;
    if (result.removed < 0) result.removed = 0;
    for (int i = 0; i < (int)order.size(); i++) {
        if (order[i] != i + 1) result.moved++;
    }

    if (result.removed == 0 && result.moved == 0) return result;
    result.changed = true;
    doc_undo_push();

    unsigned char remap[256] = {0};
    for (int i = 0; i < (int)order.size(); i++) {
        remap[order[i]] = (unsigned char)(i + 1);
    }
    if (g_sel_color > 0 && g_sel_color < 256)
        g_sel_color = remap[g_sel_color] ? remap[g_sel_color] : 0;

    unsigned char *colors = (unsigned char *)pal->data_p;
    unsigned char old_colors[512] = {0};
    memcpy(old_colors, colors, (size_t)old_numc * 2);
    for (int i = 0; i < (int)order.size(); i++) {
        int old_idx = order[i];
        int new_idx = i + 1;
        colors[new_idx * 2 + 0] = old_colors[old_idx * 2 + 0];
        colors[new_idx * 2 + 1] = old_colors[old_idx * 2 + 1];
    }

    for (int i = new_numc; i < old_numc; i++) {
        colors[i * 2 + 0] = 0;
        colors[i * 2 + 1] = 0;
    }

    pal->numc = (unsigned short)new_numc;

    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if (img->palnum == g_doc->plselected && img->data_p && img->w > 0 && img->h > 0) {
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *idx = &pixels[y * stride + x];
                    *idx = remap[*idx];
                }
            }
        }
        img = (IMG *)img->nxt_p;
    }

    ApplyPalette(g_doc->plselected);

    g_img_tex_idx = -2;
    mark_dirty();
    return result;
}

static int find_free_palette_slot_for_zero(PAL *pal, int pal_idx)
{
    if (!pal) return -1;

    int n = pal->numc;
    if (n < 0) n = 0;
    if (n < 256) return n <= 0 ? 1 : n;

    bool used[256] = {false};
    used[0] = true;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if ((int)img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pix = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++)
            for (int x = 0; x < img->w; x++)
                used[pix[y * stride + x]] = true;
    }

    for (int i = 1; i < 256; i++)
        if (!used[i]) return i;

    unsigned short zero_word = pal_word_or_black(pal, 0);
    for (int i = 1; i < 256; i++)
        if (pal_word_or_black(pal, i) == zero_word) return i;

    return -1;
}

static int copy_palette_zero_color_to_slot(int requested_slot)
{
    int pal_idx = g_doc->plselected;
    PAL *pal = (pal_idx >= 0) ? get_pal(pal_idx) : NULL;
    if (!pal) return -1;

    int slot = requested_slot;
    if (slot < 0) slot = find_free_palette_slot_for_zero(pal, pal_idx);
    if (slot <= 0 || slot >= 256) return -1;

    commit_palette_adjustments();

    unsigned short zero_word = pal_word_or_black(pal, 0);
    int old_numc = (int)pal->numc;
    if (!ensure_palette_numc(pal, slot + 1)) return -1;
    if ((int)pal->numc != old_numc) mark_dirty();

    unsigned char *pd = (unsigned char *)pal->data_p;
    unsigned short old_word = (unsigned short)(pd[slot * 2] | (pd[slot * 2 + 1] << 8));
    if (old_word != zero_word) {
        pd[slot * 2 + 0] = (unsigned char)(zero_word & 0xFF);
        pd[slot * 2 + 1] = (unsigned char)(zero_word >> 8);
        mark_dirty();
    }

    g_sel_color = slot;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    ApplyPalette(pal_idx);
    save_palette_baseline();
    g_img_tex_idx = -2;
    return slot;
}

static int count_zero_pixels_for_remap(IMG *img, bool selection_only)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return 0;
    if (selection_only && !g_grid_sel.active) return 0;

    int count = 0;
    int stride = (img->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            if (pix[y * stride + x] == 0 &&
                (!selection_only || selection_contains_pixel(img, x, y)))
                count++;
        }
    }
    return count;
}

static int remap_zero_pixels_to_slot(IMG *img, int slot, bool selection_only)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return 0;
    if (slot <= 0 || slot >= 256) return 0;
    if (selection_only && !g_grid_sel.active) return 0;

    int changed = 0;
    int stride = (img->w + 3) & ~3;
    unsigned char *pix = (unsigned char *)img->data_p;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char *p = pix + y * stride + x;
            if (*p == 0 && (!selection_only || selection_contains_pixel(img, x, y))) {
                *p = (unsigned char)slot;
                changed++;
            }
        }
    }
    return changed;
}

static void CopyPaletteZeroAndRemap(PaletteZeroRemapMode mode, int requested_slot)
{
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    bool selection_only = (mode == PaletteZeroRemapMode::Selection);
    int pending = (mode == PaletteZeroRemapMode::None) ? 0
                : count_zero_pixels_for_remap(img, selection_only);

    doc_undo_push();
    int slot = copy_palette_zero_color_to_slot(requested_slot);
    if (slot < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No opaque palette slot is available for color #0.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int changed = 0;
    if (mode != PaletteZeroRemapMode::None && pending > 0) {
        changed = remap_zero_pixels_to_slot(img, slot, selection_only);
        if (changed > 0) {
            mark_dirty();
            g_img_tex_idx = -2;
        }
    }

    if (mode == PaletteZeroRemapMode::Selection) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 changed > 0
                     ? "Copied #0 to index %d and remapped %d selected transparent pixel%s."
                     : "Copied #0 to index %d; no selected #0 pixels to remap.",
                 slot, changed, changed == 1 ? "" : "s");
    } else if (mode == PaletteZeroRemapMode::CurrentImage) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 changed > 0
                     ? "Copied #0 to index %d and remapped %d transparent pixel%s."
                     : "Copied #0 to index %d; current sprite has no #0 pixels to remap.",
                 slot, changed, changed == 1 ? "" : "s");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Copied transparent color #0 to opaque palette index %d.", slot);
    }
    g_restore_msg_timer = 5.0f;
}

static int palette_reduce_target_numc(int bpp)
{
    if (bpp < 4) bpp = 4;
    if (bpp > 8) bpp = 8;
    return 1 << bpp;
}

static void palette_reduce_update_rep_fields(PaletteReduceRep *rep)
{
    if (!rep) return;
    rep->luma = rep->r * 54 + rep->g * 183 + rep->b * 19;
    int maxv = rep->r > rep->g ? (rep->r > rep->b ? rep->r : rep->b) : (rep->g > rep->b ? rep->g : rep->b);
    int minv = rep->r < rep->g ? (rep->r < rep->b ? rep->r : rep->b) : (rep->g < rep->b ? rep->g : rep->b);
    rep->sat = maxv - minv;
    rep->hue = palette_sort_hue(rep->r, rep->g, rep->b);
    rep->family = (rep->sat <= 18) ? 0 : 1 + ((rep->hue + 32) % 1536) / 64;
}

static void palette_reduce_compute_box(PaletteReduceBox *box,
                                       const std::vector<PaletteReduceColor> &colors)
{
    if (!box || box->colors.empty()) return;
    const PaletteReduceColor &first = colors[box->colors[0]];
    box->rmin = box->rmax = first.r;
    box->gmin = box->gmax = first.g;
    box->bmin = box->bmax = first.b;
    box->weight = 0.0;

    for (int ci : box->colors) {
        const PaletteReduceColor &c = colors[ci];
        if (c.r < box->rmin) box->rmin = c.r;
        if (c.r > box->rmax) box->rmax = c.r;
        if (c.g < box->gmin) box->gmin = c.g;
        if (c.g > box->gmax) box->gmax = c.g;
        if (c.b < box->bmin) box->bmin = c.b;
        if (c.b > box->bmax) box->bmax = c.b;
        box->weight += c.weight;
    }
}

static double palette_reduce_box_score(const PaletteReduceBox &box)
{
    if (box.colors.size() <= 1) return -1.0;
    int rr = box.rmax - box.rmin;
    int gr = box.gmax - box.gmin;
    int br = box.bmax - box.bmin;
    int range = rr > gr ? (rr > br ? rr : br) : (gr > br ? gr : br);
    if (range <= 0) return -1.0;
    return (double)range * (box.weight > 1.0 ? box.weight : 1.0);
}

static bool palette_reduce_split_box(std::vector<PaletteReduceBox> &boxes,
                                     const std::vector<PaletteReduceColor> &colors,
                                     int box_idx)
{
    if (box_idx < 0 || box_idx >= (int)boxes.size()) return false;
    PaletteReduceBox box = boxes[box_idx];
    if (box.colors.size() <= 1) return false;

    int rr = box.rmax - box.rmin;
    int gr = box.gmax - box.gmin;
    int br = box.bmax - box.bmin;
    int channel = 0;
    if (gr >= rr && gr >= br) channel = 1;
    else if (br >= rr && br >= gr) channel = 2;

    std::stable_sort(box.colors.begin(), box.colors.end(),
        [&](int ai, int bi) {
            const PaletteReduceColor &a = colors[ai];
            const PaletteReduceColor &b = colors[bi];
            int av = channel == 0 ? a.r : (channel == 1 ? a.g : a.b);
            int bv = channel == 0 ? b.r : (channel == 1 ? b.g : b.b);
            if (av != bv) return av < bv;
            return a.old_idx < b.old_idx;
        });

    double half = box.weight * 0.5;
    double acc = 0.0;
    int split = (int)box.colors.size() / 2;
    for (int i = 0; i < (int)box.colors.size(); i++) {
        acc += colors[box.colors[i]].weight;
        if (acc >= half) {
            split = i + 1;
            break;
        }
    }
    if (split <= 0) split = 1;
    if (split >= (int)box.colors.size()) split = (int)box.colors.size() - 1;

    PaletteReduceBox a = {};
    PaletteReduceBox b = {};
    a.colors.assign(box.colors.begin(), box.colors.begin() + split);
    b.colors.assign(box.colors.begin() + split, box.colors.end());
    palette_reduce_compute_box(&a, colors);
    palette_reduce_compute_box(&b, colors);

    boxes[box_idx] = a;
    boxes.push_back(b);
    return true;
}

static void palette_reduce_make_reps(const std::vector<PaletteReduceBox> &boxes,
                                     const std::vector<PaletteReduceColor> &colors,
                                     std::vector<PaletteReduceRep> &reps)
{
    reps.clear();
    reps.reserve(boxes.size());
    for (const PaletteReduceBox &box : boxes) {
        if (box.colors.empty()) continue;
        double rs = 0.0, gs = 0.0, bs = 0.0, ws = 0.0;
        int old_min = 999;
        for (int ci : box.colors) {
            const PaletteReduceColor &c = colors[ci];
            double w = c.weight > 0.0 ? c.weight : 1.0;
            rs += (double)c.r * w;
            gs += (double)c.g * w;
            bs += (double)c.b * w;
            ws += w;
            if (c.old_idx < old_min) old_min = c.old_idx;
        }
        if (ws <= 0.0) ws = 1.0;
        int r = (int)(rs / ws + 0.5);
        int g = (int)(gs / ws + 0.5);
        int b = (int)(bs / ws + 0.5);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;

        PaletteReduceRep rep = {};
        rep.r = (unsigned char)r;
        rep.g = (unsigned char)g;
        rep.b = (unsigned char)b;
        rep.old_min = old_min;
        palette_reduce_update_rep_fields(&rep);
        reps.push_back(rep);
    }

    std::sort(reps.begin(), reps.end(), [](const PaletteReduceRep &a, const PaletteReduceRep &b) {
        if (a.luma != b.luma) return a.luma < b.luma;
        if (a.sat != b.sat) return a.sat < b.sat;
        if (a.family != b.family) return a.family < b.family;
        if (a.hue != b.hue) return a.hue < b.hue;
        return a.old_min < b.old_min;
    });
}

static int palette_reduce_nearest_rep(const std::vector<PaletteReduceRep> &reps,
                                      unsigned char r, unsigned char g, unsigned char b)
{
    int best = 0;
    int best_d = 0x7fffffff;
    for (int i = 0; i < (int)reps.size(); i++) {
        int dr = (int)r - (int)reps[i].r;
        int dg = (int)g - (int)reps[i].g;
        int db = (int)b - (int)reps[i].b;
        int d = dr * dr + dg * dg + db * db;
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

static void palette_reduce_refine_reps(std::vector<PaletteReduceRep> &reps,
                                       const std::vector<PaletteReduceColor> &colors)
{
    int n = (int)reps.size();
    if (n <= 0 || colors.empty()) return;

    for (int iter = 0; iter < 8; iter++) {
        std::vector<double> rs(n, 0.0), gs(n, 0.0), bs(n, 0.0), ws(n, 0.0);
        std::vector<int> old_min(n, 999);

        for (const PaletteReduceColor &c : colors) {
            int best = palette_reduce_nearest_rep(reps, c.r, c.g, c.b);
            double w = c.weight > 0.0 ? c.weight : 1.0;
            rs[best] += (double)c.r * w;
            gs[best] += (double)c.g * w;
            bs[best] += (double)c.b * w;
            ws[best] += w;
            if (c.old_idx < old_min[best]) old_min[best] = c.old_idx;
        }

        bool changed = false;
        for (int i = 0; i < n; i++) {
            if (ws[i] <= 0.0) continue;
            int r = (int)(rs[i] / ws[i] + 0.5);
            int g = (int)(gs[i] / ws[i] + 0.5);
            int b = (int)(bs[i] / ws[i] + 0.5);
            if (r < 0) r = 0; if (r > 255) r = 255;
            if (g < 0) g = 0; if (g > 255) g = 255;
            if (b < 0) b = 0; if (b > 255) b = 255;
            if (reps[i].r != (unsigned char)r ||
                reps[i].g != (unsigned char)g ||
                reps[i].b != (unsigned char)b) {
                changed = true;
            }
            reps[i].r = (unsigned char)r;
            reps[i].g = (unsigned char)g;
            reps[i].b = (unsigned char)b;
            if (old_min[i] != 999) reps[i].old_min = old_min[i];
            palette_reduce_update_rep_fields(&reps[i]);
        }
        if (!changed) break;
    }
}

static bool BuildPaletteReductionPlan(int pal_idx, int target_bpp,
                                      PaletteReductionPlan *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    if (target_bpp < 4) target_bpp = 4;
    if (target_bpp > 8) target_bpp = 8;
    out->pal_idx = pal_idx;
    out->target_bpp = target_bpp;
    out->target_numc = palette_reduce_target_numc(target_bpp);

    for (int i = 0; i < 256; i++) out->remap[i] = 0;

    PAL *pal = get_pal(pal_idx);
    if (!pal || !pal->data_p) {
        snprintf(out->error, sizeof(out->error), "No palette selected.");
        return false;
    }

    int old_numc = (int)pal->numc;
    if (old_numc > 256) old_numc = 256;
    if (old_numc <= 0) {
        snprintf(out->error, sizeof(out->error), "Selected palette has no colors.");
        return false;
    }

    out->old_numc = old_numc;
    out->input_colors = old_numc;
    const unsigned char *src = (const unsigned char *)pal->data_p;

    double usage[256] = {};
    bool active[256] = {};
    active[0] = true;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        out->images++;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pixels = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char idx = pixels[y * stride + x];
                if (idx < old_numc) {
                    usage[idx] += 1.0;
                    active[idx] = true;
                }
                out->pixels++;
            }
        }
    }

    if (out->images == 0) {
        for (int i = 1; i < old_numc; i++) {
            active[i] = true;
            usage[i] = 1.0;
        }
    }

    for (int i = 1; i < old_numc; i++)
        if (active[i]) out->active_colors++;

    if (old_numc <= out->target_numc) {
        memcpy(out->data, src, (size_t)old_numc * 2);
        for (int i = 0; i < old_numc; i++) out->remap[i] = (unsigned char)i;
        out->new_numc = old_numc;
        out->output_colors = old_numc;
        out->colors_merged = 0;
    } else if (out->active_colors <= out->target_numc - 1) {
        out->data[0] = src[0];
        out->data[1] = src[1];
        out->remap[0] = 0;
        int dst_idx = 1;
        for (int i = 1; i < old_numc; i++) {
            if (!active[i]) continue;
            out->remap[i] = (unsigned char)dst_idx;
            out->data[dst_idx * 2 + 0] = src[i * 2 + 0];
            out->data[dst_idx * 2 + 1] = src[i * 2 + 1];
            dst_idx++;
        }
        out->new_numc = dst_idx;
        out->output_colors = out->new_numc;
        out->colors_merged = old_numc - out->new_numc;
    } else {
        std::vector<PaletteReduceColor> colors;
        colors.reserve((size_t)out->active_colors);
        for (int i = 1; i < old_numc; i++) {
            if (!active[i]) continue;
            unsigned char r, g, b;
            pal_word_to_rgb8(src + i * 2, &r, &g, &b);
            PaletteReduceColor c = {};
            c.old_idx = i;
            c.r = r;
            c.g = g;
            c.b = b;
            c.weight = usage[i] > 0.0 ? usage[i] : 1.0;
            colors.push_back(c);
        }

        int target_opaque = out->target_numc - 1;
        std::vector<PaletteReduceBox> boxes;
        PaletteReduceBox root = {};
        for (int i = 0; i < (int)colors.size(); i++) root.colors.push_back(i);
        palette_reduce_compute_box(&root, colors);
        boxes.push_back(root);

        while ((int)boxes.size() < target_opaque) {
            int best = -1;
            double best_score = -1.0;
            for (int i = 0; i < (int)boxes.size(); i++) {
                double score = palette_reduce_box_score(boxes[i]);
                if (score > best_score) {
                    best_score = score;
                    best = i;
                }
            }
            if (best < 0) break;
            if (!palette_reduce_split_box(boxes, colors, best)) break;
        }

        std::vector<PaletteReduceRep> reps;
        palette_reduce_make_reps(boxes, colors, reps);
        palette_reduce_refine_reps(reps, colors);
        if ((int)reps.size() > target_opaque) reps.resize(target_opaque);
        std::sort(reps.begin(), reps.end(), [](const PaletteReduceRep &a, const PaletteReduceRep &b) {
            if (a.luma != b.luma) return a.luma < b.luma;
            if (a.sat != b.sat) return a.sat < b.sat;
            if (a.family != b.family) return a.family < b.family;
            if (a.hue != b.hue) return a.hue < b.hue;
            return a.old_min < b.old_min;
        });

        out->data[0] = src[0];
        out->data[1] = src[1];
        out->remap[0] = 0;
        out->new_numc = (int)reps.size() + 1;
        out->output_colors = out->new_numc;
        out->colors_merged = old_numc - out->new_numc;
        out->quantized = true;

        for (int i = 0; i < (int)reps.size(); i++) {
            rgb8_to_pal_word(reps[i].r, reps[i].g, reps[i].b,
                             out->data + (i + 1) * 2);
        }

        for (int i = 1; i < old_numc; i++) {
            unsigned char r, g, b;
            pal_word_to_rgb8(src + i * 2, &r, &g, &b);
            int best = palette_reduce_nearest_rep(reps, r, g, b);
            out->remap[i] = (unsigned char)(best + 1);
        }
    }

    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if (img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0) continue;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pixels = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char idx = pixels[y * stride + x];
                unsigned char mapped = out->remap[idx];
                if (mapped != idx) out->pixels_changed++;
            }
        }
    }

    out->valid = true;
    return true;
}

static int FindPalettePreviewImage(int pal_idx, int prefer_idx)
{
    IMG *prefer = get_img(prefer_idx);
    if (prefer && prefer->palnum == pal_idx && prefer->data_p &&
        prefer->w > 0 && prefer->h > 0)
        return prefer_idx;

    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if (img->palnum == pal_idx && img->data_p && img->w > 0 && img->h > 0)
            return idx;
    }
    return -1;
}

static int StepPalettePreviewImage(int pal_idx, int start_idx, int delta)
{
    int n = count_imgs();
    if (n <= 0) return -1;
    int idx = start_idx;
    if (idx < 0 || idx >= n) idx = delta >= 0 ? -1 : 0;
    for (int step = 0; step < n; step++) {
        idx += delta;
        if (idx < 0) idx = n - 1;
        if (idx >= n) idx = 0;
        IMG *img = get_img(idx);
        if (img && img->palnum == pal_idx && img->data_p &&
            img->w > 0 && img->h > 0)
            return idx;
    }
    return -1;
}

static SDL_Texture *BuildPaletteReductionTexture(const IMG *img, const PAL *pal,
                                                 const PaletteReductionPlan *plan,
                                                 bool reduced)
{
    if (!g_imgui_renderer || !img || !img->data_p || img->w == 0 || img->h == 0)
        return NULL;

    SDL_Texture *tex = SDL_CreateTexture(g_imgui_renderer,
        SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, img->w, img->h);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);

    void *pixels = NULL;
    int pitch = 0;
    if (SDL_LockTexture(tex, NULL, &pixels, &pitch) != 0) {
        SDL_DestroyTexture(tex);
        return NULL;
    }

    const unsigned char *pal_data = reduced ? plan->data : (const unsigned char *)pal->data_p;
    int pal_colors = reduced ? plan->new_numc : (int)pal->numc;
    if (pal_colors > 256) pal_colors = 256;
    int stride = (img->w + 3) & ~3;
    const unsigned char *src_pixels = (const unsigned char *)img->data_p;
    Uint32 *dst = (Uint32 *)pixels;
    for (int y = 0; y < img->h; y++) {
        for (int x = 0; x < img->w; x++) {
            unsigned char ci = src_pixels[y * stride + x];
            if (reduced) ci = plan->remap[ci];
            Uint32 a = ci == 0 ? 0u : 0xFFu;
            unsigned char r = 160, g = 160, b = 160;
            if (pal_data && ci < pal_colors) {
                pal_word_to_rgb8(pal_data + ci * 2, &r, &g, &b);
            }
            dst[y * (pitch / 4) + x] =
                (a << 24) | ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
        }
    }
    SDL_UnlockTexture(tex);
    return tex;
}

void ClearPaletteReducePreviewTextures(void)
{
    if (g_palette_reduce_orig_tex) {
        SDL_DestroyTexture(g_palette_reduce_orig_tex);
        g_palette_reduce_orig_tex = NULL;
    }
    if (g_palette_reduce_new_tex) {
        SDL_DestroyTexture(g_palette_reduce_new_tex);
        g_palette_reduce_new_tex = NULL;
    }
    g_palette_reduce_tex_w = 0;
    g_palette_reduce_tex_h = 0;
    g_palette_reduce_tex_img = -1;
    g_palette_reduce_tex_pal = -1;
    g_palette_reduce_tex_bpp = 0;
}

static void RebuildPaletteReducePreviewTextures(const PaletteReductionPlan &plan)
{
    IMG *img = get_img(g_palette_reduce_preview_idx);
    PAL *pal = get_pal(plan.pal_idx);
    if (!img || !pal || !plan.valid) {
        ClearPaletteReducePreviewTextures();
        return;
    }

    if (g_palette_reduce_orig_tex && g_palette_reduce_new_tex &&
        g_palette_reduce_tex_img == g_palette_reduce_preview_idx &&
        g_palette_reduce_tex_pal == plan.pal_idx &&
        g_palette_reduce_tex_bpp == plan.target_bpp &&
        g_palette_reduce_tex_w == (int)img->w &&
        g_palette_reduce_tex_h == (int)img->h) {
        return;
    }

    ClearPaletteReducePreviewTextures();
    g_palette_reduce_orig_tex = BuildPaletteReductionTexture(img, pal, &plan, false);
    g_palette_reduce_new_tex = BuildPaletteReductionTexture(img, pal, &plan, true);
    g_palette_reduce_tex_img = g_palette_reduce_preview_idx;
    g_palette_reduce_tex_pal = plan.pal_idx;
    g_palette_reduce_tex_bpp = plan.target_bpp;
    g_palette_reduce_tex_w = img->w;
    g_palette_reduce_tex_h = img->h;
}

static bool ApplyPaletteReductionPlan(const PaletteReductionPlan &plan)
{
    if (!plan.valid) return false;
    PAL *pal = get_pal(plan.pal_idx);
    if (!pal || !pal->data_p) return false;

    doc_undo_push();

    int old_numc = (int)pal->numc;
    if (old_numc > 256) old_numc = 256;
    memcpy(pal->data_p, plan.data, (size_t)plan.new_numc * 2);
    for (int i = plan.new_numc; i < old_numc; i++) {
        ((unsigned char *)pal->data_p)[i * 2 + 0] = 0;
        ((unsigned char *)pal->data_p)[i * 2 + 1] = 0;
    }
    pal->numc = (unsigned short)plan.new_numc;
    pal->bitspix = (unsigned char)plan.target_bpp;

    int img_idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        if (img->palnum != plan.pal_idx || !img->data_p || img->w == 0 || img->h == 0)
            continue;
        int stride = (img->w + 3) & ~3;
        unsigned char *pixels = (unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            for (int x = 0; x < img->w; x++) {
                unsigned char *idx = pixels + y * stride + x;
                *idx = plan.remap[*idx];
            }
        }
        InvalidateThumb(img_idx);
    }

    if (g_sel_color >= plan.new_numc)
        g_sel_color = plan.new_numc > 1 ? plan.new_numc - 1 : 0;
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    ApplyPalette(plan.pal_idx);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;
    mark_dirty();
    return true;
}

static void DrawPaletteReduceSwatches(const unsigned char *data, int numc)
{
    if (!data || numc <= 0) {
        ImGui::TextDisabled("No colors");
        return;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    const float sw = 13.0f;
    const float gap = 1.0f;
    const int cols = 16;
    int rows = (numc + cols - 1) / cols;
    if (rows < 1) rows = 1;

    for (int i = 0; i < numc; i++) {
        int row = i / cols;
        int col = i % cols;
        ImVec2 a(p.x + col * (sw + gap), p.y + row * (sw + gap));
        ImVec2 b(a.x + sw, a.y + sw);
        unsigned char r, g, bch;
        pal_word_to_rgb8(data + i * 2, &r, &g, &bch);
        dl->AddRectFilled(a, b, IM_COL32(r, g, bch, 255));
        dl->AddRect(a, b, i == 0 ? IM_COL32(95, 95, 95, 200)
                                  : IM_COL32(0, 0, 0, 160));
    }
    ImGui::Dummy(ImVec2(cols * (sw + gap), rows * (sw + gap)));
}

static void DrawPaletteReduceImageColumn(const char *title, SDL_Texture *tex,
                                         IMG *img, float max_w, float max_h)
{
    ImGui::TextUnformatted(title);
    if (!tex || !img || img->w == 0 || img->h == 0) {
        ImGui::TextDisabled("No preview");
        return;
    }

    float scale_x = max_w / (float)img->w;
    float scale_y = max_h / (float)img->h;
    float scale = scale_x < scale_y ? scale_x : scale_y;
    if (scale > 8.0f) scale = 8.0f;
    if (scale <= 0.0f) scale = 1.0f;
    ImVec2 sz((float)img->w * scale, (float)img->h * scale);
    ImGui::Image((ImTextureID)(intptr_t)tex, sz);
}

/* ---- Public Interface Implementations ---- */

void AddNewPalette(void)
{
    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = 0;
    pal->bitspix = 8;
    pal->numc    = 256;
    pal->pad     = 0;
    make_unique_pal_name(pal->n_s);

    unsigned char *buf = (unsigned char *)PoolAlloc(512);
    if (!buf) return;
    pal->data_p = buf;
    memset(buf, 0, 512);

    g_doc->plselected = (int)g_doc->palcnt - 1;
    IMG *cur = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (cur) cur->palnum = (unsigned short)g_doc->plselected;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
}

void DuplicatePalette(void)
{
    PAL *src = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!src || !src->data_p) return;

    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = src->flags;
    pal->bitspix = src->bitspix;
    pal->numc    = src->numc;
    pal->pad     = 0;
    make_numbered_pal_name(src->n_s, pal->n_s);

    unsigned int col_sz = (unsigned int)pal->numc * 2;
    unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
    if (!buf) return;
    pal->data_p = buf;
    memcpy(buf, src->data_p, col_sz);

    g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
}

void CopyPaletteToClipboard(void)
{
    PAL *src = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!src || !src->data_p || src->numc == 0) return;

    unsigned int col_sz = (unsigned int)src->numc * 2;
    unsigned char *buf = (unsigned char *)malloc(col_sz);
    if (!buf) return;
    memcpy(buf, src->data_p, col_sz);

    if (g_pal_clipboard.valid && g_pal_clipboard.data) free(g_pal_clipboard.data);
    g_pal_clipboard.valid   = true;
    g_pal_clipboard.numc    = src->numc;
    g_pal_clipboard.bitspix = src->bitspix;
    memcpy(g_pal_clipboard.n_s, src->n_s, 10);
    g_pal_clipboard.data    = buf;
}

void PastePaletteFromClipboard(void)
{
    if (!g_pal_clipboard.valid || !g_pal_clipboard.data || g_pal_clipboard.numc == 0) return;

    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = 0;
    pal->numc    = g_pal_clipboard.numc;
    /* Keep the source's declared depth, but derive one when the clipboard
       carries none rather than defaulting every paste to 8bpp. */
    pal->bitspix = g_pal_clipboard.bitspix
                 ? g_pal_clipboard.bitspix
                 : (unsigned char)PaletteBppForColorCount((int)g_pal_clipboard.numc);
    pal->pad     = 0;
    memcpy(pal->n_s, g_pal_clipboard.n_s, 10);

    unsigned int col_sz = (unsigned int)pal->numc * 2;
    unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
    if (!buf) return;
    pal->data_p = buf;
    memcpy(buf, g_pal_clipboard.data, col_sz);

    if (g_doc->palcnt > 0) g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
}

void SelectPalette(int idx)
{
    if (idx < 0 || (unsigned)idx >= g_doc->palcnt) return;
    commit_palette_adjustments();
    IMG *cur = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    bool assign_to_image = cur && cur->palnum != (unsigned short)idx;
    if (assign_to_image) doc_undo_push();
    g_doc->plselected = idx;
    g_palette_nav = true;
    if (assign_to_image) {
        cur->palnum = (unsigned short)idx;
        g_img_tex_idx = -2;
    }
}

void SetPaletteOfSelected(void)
{
    if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) return;
    IMG *img = (g_doc->ilselected >= 0) ? get_img(g_doc->ilselected) : NULL;
    if (!img) return;
    doc_undo_push();
    img->palnum = (unsigned short)g_doc->plselected;
    g_img_tex_idx = -2;
}

void SetPaletteOfMarked(void)
{
    if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) return;
    bool any = false;
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p)
        if (p->flags & 1) { any = true; break; }
    if (!any) return;
    doc_undo_push();
    for (IMG *p = (IMG *)g_doc->img_p; p; p = (IMG *)p->nxt_p)
        if (p->flags & 1) p->palnum = (unsigned short)g_doc->plselected;
    g_img_tex_idx = -2;
}

static void RemapImagePalettesAfterPaletteDelete(int deleted_idx, int fallback_idx)
{
    int img_idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, img_idx++) {
        int pal_idx = (int)img->palnum;
        int new_idx = pal_idx;

        if (pal_idx == deleted_idx) {
            new_idx = fallback_idx;
        } else if (pal_idx > deleted_idx) {
            new_idx = pal_idx - 1;
        }

        if (g_doc->palcnt == 0) {
            new_idx = 0;
        } else if (new_idx < 0) {
            new_idx = 0;
        } else if ((unsigned)new_idx >= g_doc->palcnt) {
            new_idx = (int)g_doc->palcnt - 1;
        }

        if (new_idx != pal_idx) {
            img->palnum = (unsigned short)new_idx;
            InvalidateThumb(img_idx);
        }
    }
}

void DeletePalette(void)
{
    if (g_doc->plselected < 0 || (unsigned)g_doc->plselected >= g_doc->palcnt) return;
    commit_palette_adjustments();
    doc_undo_push();

    int deleted_idx = g_doc->plselected;
    PAL *prev = NULL;
    PAL *curr = (PAL *)g_doc->pal_p;
    for (int i = 0; curr && i < deleted_idx; i++) {
        prev = curr;
        curr = (PAL *)curr->nxt_p;
    }
    if (!curr) return;

    if (prev) prev->nxt_p = curr->nxt_p;
    else g_doc->pal_p = curr->nxt_p;
    g_doc->palcnt--;

    int fallback_idx = -1;
    if (g_doc->palcnt > 0)
        fallback_idx = (deleted_idx < (int)g_doc->palcnt) ? deleted_idx : (int)g_doc->palcnt - 1;

    RemapImagePalettesAfterPaletteDelete(deleted_idx, fallback_idx);
    g_doc->plselected = fallback_idx;

    if (curr->data_p) free(curr->data_p);
    free(curr);

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    InvalidatePaletteSync();
    g_img_tex_idx = -2;
}

static void MergeMarkedPalettes(bool force_quality_merge)
{
    PAL *sel = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!sel || !sel->data_p || sel->numc == 0) return;

    bool any_marked = false;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p)
        if ((p->flags & 1) && p != sel) { any_marked = true; break; }
    if (!any_marked) return;

    PaletteMergeQuality quality = {};
    if (!BuildMarkedPaletteMergeQuality(&quality)) return;
    g_palette_merge_quality = quality;

    if (!force_quality_merge) {
        g_palette_merge_preview_only = false;
        g_show_palette_merge_quality = true;
        return;
    }

    doc_undo_push();

    sel->flags &= ~1;

    int base_count = (int)sel->numc;
    if (base_count > 256) base_count = 256;
    if (quality.colors_added > 0) {
        int new_numc = base_count + quality.colors_added;
        unsigned char *nd =
            (unsigned char *)realloc(sel->data_p, (size_t)new_numc * 2);
        if (nd) {
            sel->data_p = nd;
            for (int j = 0; j < quality.colors_added; j++) {
                unsigned short w = quality.added_words[j];
                nd[(base_count + j) * 2 + 0] = (unsigned char)(w & 0xFF);
                nd[(base_count + j) * 2 + 1] = (unsigned char)((w >> 8) & 0xFF);
            }
            sel->numc = (unsigned short)new_numc;
            /* Merging can push the count past what the declared depth can
               address; widen so the palette still describes itself. */
            if (PaletteBppTooSmall(sel->bitspix, new_numc))
                sel->bitspix = (unsigned char)PaletteBppForColorCount(new_numc);
        }
    }

    PAL *pal = (PAL *)g_doc->pal_p;
    while (pal) {
        if (!(pal->flags & 1) || pal == sel || !pal->data_p || pal->numc == 0) {
            pal = (PAL *)pal->nxt_p;
            continue;
        }

        unsigned short  src_numc   = pal->numc;

        unsigned char remap[256] = {0};
        BuildPaletteMergeRemap(pal, sel, (int)sel->numc, NULL, 0,
                               quality.opt_perceptual, remap);

        int pal_idx = 0;
        for (PAL *q = (PAL *)g_doc->pal_p; q && q != pal; q = (PAL *)q->nxt_p) pal_idx++;

        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
            if (img->palnum != pal_idx) continue;
            img->palnum = (unsigned short)g_doc->plselected;

            if (!img->data_p || img->w == 0 || img->h == 0) continue;
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            int src_count = (int)src_numc;
            if (src_count > 256) src_count = 256;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *px = pixels + y * stride + x;
                    if (*px != 0) *px = ((int)*px < src_count) ? remap[*px] : 0;
                }
            }
        }

        pal = (PAL *)pal->nxt_p;
    }

    PAL *prev = NULL;
    PAL *cur  = (PAL *)g_doc->pal_p;
    int del_idx = 0;

    while (cur) {
        if (cur->flags & 1) {
            PAL *to_del = cur;
            if (prev) prev->nxt_p = cur->nxt_p;
            else g_doc->pal_p = cur->nxt_p;
            cur = (PAL *)cur->nxt_p;
            g_doc->palcnt--;

            for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
                if ((int)img->palnum > del_idx) img->palnum--;
            }
            if ((int)g_doc->plselected > del_idx) g_doc->plselected--;

            if (to_del->data_p) free(to_del->data_p);
            free(to_del);
        } else {
            prev = cur;
            cur = (PAL *)cur->nxt_p;
            del_idx++;
        }
    }

    if ((unsigned)g_doc->plselected >= g_doc->palcnt)
        g_doc->plselected = g_doc->palcnt ? (int)g_doc->palcnt - 1 : -1;

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    reset_palette_adjust_sliders();
    g_img_tex_idx = -2;

    char added_note[48] = "";
    if (quality.colors_added > 0)
        snprintf(added_note, sizeof(added_note), " (+%d color%s)",
                 quality.colors_added, quality.colors_added == 1 ? "" : "s");

    int drift_pixels = quality.color_drift_pixels +
                       quality.transparent_drift_pixels;
    if (drift_pixels > 0) {
        double max_drift = sqrt((double)quality.max_dist);
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Merged %d palette%s%s; quality drift on %d/%d pixel%s (max %.1f).",
                 quality.source_palettes, quality.source_palettes == 1 ? "" : "s",
                 added_note, drift_pixels, quality.affected_pixels,
                 drift_pixels == 1 ? "" : "s", max_drift);
    } else if (quality.ppp_warning_images > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Merged %d palette%s%s; no visual drift, PPP warning on %d image%s.",
                 quality.source_palettes, quality.source_palettes == 1 ? "" : "s",
                 added_note, quality.ppp_warning_images,
                 quality.ppp_warning_images == 1 ? "" : "s");
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Merged %d palette%s%s; quality check OK (no visual drift).",
                 quality.source_palettes, quality.source_palettes == 1 ? "" : "s",
                 added_note);
    }
    g_restore_msg_timer = 5.0f;
}

void MergeMarkedPalettes(void)
{
    MergeMarkedPalettes(false);
}

void OpenPaletteMergePreview(void)
{
    PaletteMergeQuality quality = {};
    if (!BuildMarkedPaletteMergeQuality(&quality)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Mark one source palette, then select the target palette.");
        g_restore_msg_timer = 4.0f;
        return;
    }
    g_palette_merge_quality = quality;
    g_palette_merge_preview_only = true;
    g_show_palette_merge_quality = true;
}

void BuildSelectedPaletteUsage(void)
{
    int pal_idx = g_doc ? g_doc->plselected : -1;
    PAL *pal = (pal_idx >= 0) ? get_pal(pal_idx) : NULL;
    int pal_numc = (pal && pal->data_p) ? (int)pal->numc : 0;
    if (pal_numc < 0) pal_numc = 0;
    if (pal_numc > 256) pal_numc = 256;

    bool needs_rebuild =
        g_palette_usage_doc != g_doc ||
        g_palette_usage_img_head != (g_doc ? g_doc->img_p : NULL) ||
        g_palette_usage_imgcnt_seen != (g_doc ? g_doc->imgcnt : 0) ||
        g_palette_usage_pal_idx != pal_idx ||
        g_palette_usage_pal_numc != pal_numc ||
        g_palette_usage_built_serial != g_palette_usage_serial;
    if (!needs_rebuild) return;

    memset(g_palette_usage_counts, 0, sizeof(g_palette_usage_counts));
    g_palette_usage_doc = g_doc;
    g_palette_usage_img_head = g_doc ? g_doc->img_p : NULL;
    g_palette_usage_imgcnt_seen = g_doc ? g_doc->imgcnt : 0;
    g_palette_usage_pal_idx = pal_idx;
    g_palette_usage_pal_numc = pal_numc;
    g_palette_usage_img_count = 0;
    g_palette_usage_used_colors = 0;
    g_palette_usage_unused_colors = 0;
    g_palette_usage_low_colors = 0;
    g_palette_usage_built_serial = g_palette_usage_serial;

    if (!g_doc || pal_idx < 0 || !pal || !pal->data_p || pal_numc <= 0)
        return;

    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p) {
        if ((int)img->palnum != pal_idx || !img->data_p || img->w == 0 || img->h == 0)
            continue;

        g_palette_usage_img_count++;
        int stride = (img->w + 3) & ~3;
        const unsigned char *pixels = (const unsigned char *)img->data_p;
        for (int y = 0; y < img->h; y++) {
            const unsigned char *row = pixels + y * stride;
            for (int x = 0; x < img->w; x++)
                g_palette_usage_counts[row[x]]++;
        }
    }

    if (g_palette_usage_low_threshold < 1) g_palette_usage_low_threshold = 1;
    for (int i = 1; i < pal_numc; i++) {
        unsigned long long count = g_palette_usage_counts[i];
        if (count == 0) {
            g_palette_usage_unused_colors++;
        } else {
            g_palette_usage_used_colors++;
            if (count <= (unsigned long long)g_palette_usage_low_threshold)
                g_palette_usage_low_colors++;
        }
    }
}

int FindNearestUsedPaletteSlotForUsage(int color_idx, int *dist_out)
{
    if (dist_out) *dist_out = 0;
    BuildSelectedPaletteUsage();

    PAL *pal = (g_palette_usage_pal_idx >= 0) ? get_pal(g_palette_usage_pal_idx) : NULL;
    if (!pal || !pal->data_p ||
        color_idx <= 0 || color_idx >= g_palette_usage_pal_numc)
        return -1;

    const unsigned char *pd = (const unsigned char *)pal->data_p;
    unsigned short target =
        (unsigned short)(pd[color_idx * 2] | (pd[color_idx * 2 + 1] << 8));
    int best = -1;
    int best_dist = 0x7FFFFFFF;

    for (int i = 1; i < g_palette_usage_pal_numc; i++) {
        if (i == color_idx || g_palette_usage_counts[i] == 0)
            continue;
        unsigned short w = (unsigned short)(pd[i * 2] | (pd[i * 2 + 1] << 8));
        int dist = PaletteColorDistance5(target, w);
        if (dist < best_dist) {
            best = i;
            best_dist = dist;
            if (dist == 0) break;
        }
    }

    if (dist_out && best >= 0) *dist_out = best_dist;
    return best;
}

void CalculatePaletteHistogram(void)
{
    memset(g_histogram_data, 0, sizeof(g_histogram_data));
    g_histogram_max = 0.0f;
    g_histogram_img_count = 0;

    BuildSelectedPaletteUsage();
    g_histogram_img_count = g_palette_usage_img_count;
    for (int i = 0; i < 256; i++)
        g_histogram_data[i] = (float)g_palette_usage_counts[i];

    for (int i = 1; i < 256; i++) {
        if (g_histogram_data[i] > g_histogram_max) {
            g_histogram_max = g_histogram_data[i];
        }
    }
    if (g_histogram_max == 0.0f) g_histogram_max = 1.0f;
}

void hsl_adjust_palette_from_baseline(int hue_deg, int sat_pct, int light_pct)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return;
    if (g_palette_baseline_nc == 0) return;
    mark_dirty();

    int n = (int)pal->numc;
    if (n > 256) n = 256;
    if (n > g_palette_baseline_nc) n = g_palette_baseline_nc;

    unsigned char rgb[256 * 3];
    HslAdjustPaletteWordsFromBaseline(g_palette_baseline, n,
                                      g_palette_selection,
                                      hue_deg, sat_pct, light_pct,
                                      (unsigned char *)pal->data_p,
                                      rgb);

    for (int i = 0; i < n; i++) {
        g_palette[i].r = rgb[i * 3 + 0];
        g_palette[i].g = rgb[i * 3 + 1];
        g_palette[i].b = rgb[i * 3 + 2];
    }
}

void hue_shift_palette(int /*delta_deg*/)
{
    hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
}

void save_palette_baseline(void)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) { g_palette_baseline_nc = 0; return; }
    int nc = (int)pal->numc;
    if (nc > 256) nc = 256;
    memcpy(g_palette_baseline, pal->data_p, nc * 2);
    g_palette_baseline_nc = nc;
}

void reset_palette_adjust_sliders(void)
{
    g_hue_slider = 0;
    g_hue_last   = 0;
    g_sat_slider = 0;
    g_sat_last   = 0;
    g_light_slider = 0;
    g_light_last   = 0;
}

void commit_palette_adjustments(void)
{
    save_palette_baseline();
    reset_palette_adjust_sliders();
}

void reset_palette_to_baseline(void)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p || g_palette_baseline_nc == 0) return;
    int nc = (int)pal->numc;
    if (nc > 256) nc = 256;
    if (nc > g_palette_baseline_nc) nc = g_palette_baseline_nc;
    memcpy(pal->data_p, g_palette_baseline, nc * 2);
    ApplyPalette(g_doc->plselected);
    reset_palette_adjust_sliders();
    mark_dirty();
}

void ClearWorkingPalette(void)
{
    for (int i = 0; i < 256; i++) {
        g_palette[i].r = 0;
        g_palette[i].g = 0;
        g_palette[i].b = 0;
        g_palette[i].a = 255;
    }
}

void ApplyPalette(int pal_idx)
{
    if (pal_idx < 0) {
        ClearWorkingPalette();
        return;
    }
    PAL *pal = get_pal(pal_idx);
    if (!pal || !pal->data_p) {
        ClearWorkingPalette();
        return;
    }
    const unsigned char *src = (const unsigned char *)pal->data_p;
    int n = pal->numc;
    if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
        pal_word_to_rgb8(src + i * 2, &g_palette[i].r, &g_palette[i].g, &g_palette[i].b);
        g_palette[i].a = 255;
    }
    for (int i = n; i < 256; i++) {
        g_palette[i].r = 0;
        g_palette[i].g = 0;
        g_palette[i].b = 0;
        g_palette[i].a = 255;
    }
}

void OpenPaletteReduceDialog(int bpp)
{
    commit_palette_adjustments();
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a palette first.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    if (bpp < 4) bpp = 4;
    if (bpp > 8) bpp = 8;
    g_palette_reduce_bpp = bpp;
    g_palette_reduce_preview_idx = FindPalettePreviewImage(g_doc->plselected, g_doc->ilselected);
    ClearPaletteReducePreviewTextures();
    g_show_palette_reduce = true;
}

void InvalidatePaletteUsage(void)
{
    g_palette_usage_serial++;
    if (g_palette_usage_serial == 0) {
        g_palette_usage_serial = 1;
        g_palette_usage_built_serial = 0;
    }
}

bool ensure_palette_numc(PAL *pal, int min_numc)
{
    if (!pal || min_numc <= 0 || min_numc > 256) return false;
    if (!pal->data_p) {
        pal->data_p = PoolAlloc(512);
        if (!pal->data_p) return false;
        pal->numc = (unsigned short)min_numc;
        pal->bitspix = (unsigned char)PaletteBppForColorCount(min_numc);
        return true;
    }

    if (pal->numc >= min_numc) return true;

    unsigned char *old_data = (unsigned char *)pal->data_p;
    unsigned int old_bytes = (unsigned int)pal->numc * 2;
    unsigned char *new_data = (unsigned char *)malloc(512);
    if (!new_data) return false;
    memset(new_data, 0, 512);
    if (old_bytes > 0) memcpy(new_data, old_data, old_bytes);
    free(old_data);
    pal->data_p = new_data;
    pal->numc = (unsigned short)min_numc;
    /* Growing past what the declared depth can address would leave the palette
       describing itself incorrectly, so raise bitspix to fit. Never lower it
       here — shrinking depth is the explicit Recalculate BPP command's job. */
    if (PaletteBppTooSmall(pal->bitspix, min_numc))
        pal->bitspix = (unsigned char)PaletteBppForColorCount(min_numc);
    return true;
}

/* ---- BPP repair -----------------------------------------------------
   Palettes imported before the depth was derived from the color count (and
   any hand-edited ones) can carry a bitspix that doesn't match how many
   colors they actually hold — usually 8 when the art is 4/5/6bpp, which makes
   the TBL/IRW/LOAD2 exports pack wider than they need to. These recompute it
   from numc without touching any color. */

int PaletteBppMismatchCount(void)
{
    int n = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
        if (p->numc == 0) continue;
        if ((int)p->bitspix != PaletteBppForColorCount((int)p->numc)) n++;
    }
    return n;
}

int RecalculateSelectedPaletteBpp(void)
{
    PAL *pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || pal->numc == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Select a palette first.");
        g_restore_msg_timer = 3.0f;
        return 0;
    }
    int want = PaletteBppForColorCount((int)pal->numc);
    if ((int)pal->bitspix == want) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "%.9s already correct: %d colors, %d bpp.",
                 pal->n_s, (int)pal->numc, want);
        g_restore_msg_timer = 4.0f;
        return 0;
    }
    doc_undo_push();
    int was = (int)pal->bitspix;
    pal->bitspix = (unsigned char)want;
    InvalidatePaletteSync();
    mark_dirty();
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "%.9s: %d bpp -> %d bpp for %d colors.",
             pal->n_s, was, want, (int)pal->numc);
    g_restore_msg_timer = 4.0f;
    return 1;
}

int RecalculateAllPaletteBpp(void)
{
    if (PaletteBppMismatchCount() == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Every palette's BPP already matches its color count.");
        g_restore_msg_timer = 4.0f;
        return 0;
    }
    doc_undo_push();
    int changed = 0, lowered = 0, raised = 0;
    for (PAL *p = (PAL *)g_doc->pal_p; p; p = (PAL *)p->nxt_p) {
        if (p->numc == 0) continue;
        int want = PaletteBppForColorCount((int)p->numc);
        if ((int)p->bitspix == want) continue;
        if (want < (int)p->bitspix) lowered++; else raised++;
        p->bitspix = (unsigned char)want;
        changed++;
    }
    if (changed > 0) {
        InvalidatePaletteSync();
        mark_dirty();
    }
    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Recalculated BPP on %d palette%s (%d narrowed, %d widened).",
             changed, changed == 1 ? "" : "s", lowered, raised);
    g_restore_msg_timer = 5.0f;
    return changed;
}

void CleanupSelectedPalette(void)
{
    commit_palette_adjustments();
    PaletteCleanupResult r = DeleteUnusedPaletteColors();
    if (r.changed) {
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        save_palette_baseline();
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Cleaned %d unused, sorted %d active color%s, moved %d.",
                 r.removed, r.sorted, r.sorted == 1 ? "" : "s", r.moved);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Palette already sorted (%d active color%s).",
                 r.sorted, r.sorted == 1 ? "" : "s");
    }
    g_restore_msg_timer = 4.0f;
}

void GroupLikeColorsSelectedPalette(void)
{
    commit_palette_adjustments();
    PaletteCleanupResult r = DeleteUnusedPaletteColors(PaletteSortMode::GroupByColor);
    if (r.changed) {
        memset(g_palette_selection, 0, sizeof(g_palette_selection));
        save_palette_baseline();
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Grouped %d color%s by family (cleaned %d unused, moved %d).",
                 r.sorted, r.sorted == 1 ? "" : "s", r.removed, r.moved);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Palette already grouped (%d active color%s).",
                 r.sorted, r.sorted == 1 ? "" : "s");
    }
    g_restore_msg_timer = 4.0f;
}

void CreateCleanedPaletteCopy(void)
{
    commit_palette_adjustments();

    int src_idx = g_doc->plselected;
    PAL *src = (src_idx >= 0) ? get_pal(src_idx) : NULL;
    if (!src || !src->data_p) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No palette selected to clean-copy.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int old_numc = src->numc;
    if (old_numc > 256) old_numc = 256;
    if (old_numc <= 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Selected palette has no colors.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    bool used[256];
    int referenced_pixels = BuildPaletteUsedMask(src_idx, old_numc, used);
    std::vector<int> order = BuildGradientPaletteOrder(src, used);
    int new_numc = (int)order.size() + 1;
    if (new_numc <= 1) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No active colors to clean-copy.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    int moved = 0;
    for (int i = 0; i < (int)order.size(); i++)
        if (order[i] != i + 1) moved++;

    doc_undo_push();
    PAL *pal = (PAL *)AllocPal();
    if (!pal) return;

    pal->flags   = src->flags;
    pal->bitspix = src->bitspix;
    pal->numc    = (unsigned short)new_numc;
    pal->pad     = 0;
    make_numbered_pal_name(src->n_s, pal->n_s);

    unsigned char *buf = (unsigned char *)PoolAlloc((size_t)new_numc * 2);
    if (!buf) return;
    pal->data_p = buf;

    const unsigned char *old_colors = (const unsigned char *)src->data_p;
    buf[0] = old_colors[0];
    buf[1] = old_colors[1];
    for (int i = 0; i < (int)order.size(); i++) {
        int old_idx = order[i];
        int new_idx = i + 1;
        buf[new_idx * 2 + 0] = old_colors[old_idx * 2 + 0];
        buf[new_idx * 2 + 1] = old_colors[old_idx * 2 + 1];
    }

    g_doc->plselected = (int)g_doc->palcnt - 1;
    ApplyPalette(g_doc->plselected);
    memset(g_palette_selection, 0, sizeof(g_palette_selection));
    save_palette_baseline();
    g_img_tex_idx = -2;
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Created clean palette copy: sorted %d colors, moved %d%s.",
             (int)order.size(), moved,
             referenced_pixels == 0 ? " (palette-only)" : "");
    g_restore_msg_timer = 5.0f;
}

void CopyPaletteZeroToOpaqueSlot(int requested_slot)
{
    doc_undo_push();
    int slot = copy_palette_zero_color_to_slot(requested_slot);
    if (slot < 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No opaque palette slot is available for color #0.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Copied transparent color #0 to opaque palette index %d.", slot);
    g_restore_msg_timer = 4.0f;
}

void CopyPaletteZeroToOpaqueSlot(void)
{
    CopyPaletteZeroToOpaqueSlot(-1);
}

void MoveSelectedPaletteColorsToEnd(void)
{
    if (g_doc->plselected < 0) return;
    PAL *pal = get_pal(g_doc->plselected);
    if (!pal || !pal->data_p) return;

    int N = (int)pal->numc;
    if (N <= 1) return;

    std::vector<int> new_to_old;
    new_to_old.push_back(0); // Index 0 must always stay at 0 (transparency).

    // First, add all unselected active colors (1 to N-1)
    for (int i = 1; i < N; i++) {
        if (!g_palette_selection[i]) {
            new_to_old.push_back(i);
        }
    }

    // Then, add all selected colors (1 to N-1)
    int selected_count = 0;
    for (int i = 1; i < N; i++) {
        if (g_palette_selection[i]) {
            new_to_old.push_back(i);
            selected_count++;
        }
    }

    if (selected_count == 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "No palette colors selected. Use Ctrl+click to select colors first.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    // Check if any colors actually changed position
    bool changed = false;
    for (int i = 0; i < N; i++) {
        if (new_to_old[i] != i) {
            changed = true;
            break;
        }
    }

    if (!changed) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Selected colors are already at the end of the palette.");
        g_restore_msg_timer = 4.0f;
        return;
    }

    doc_undo_push();

    // Create the remap table
    unsigned char remap[256];
    for (int i = 0; i < 256; i++) {
        remap[i] = (unsigned char)i;
    }
    for (int new_idx = 0; new_idx < N; new_idx++) {
        int old_idx = new_to_old[new_idx];
        remap[old_idx] = (unsigned char)new_idx;
    }

    // Reorder the palette color data
    unsigned char old_colors[512];
    memcpy(old_colors, pal->data_p, (size_t)N * 2);
    unsigned char *colors = (unsigned char *)pal->data_p;
    for (int new_idx = 0; new_idx < N; new_idx++) {
        int old_idx = new_to_old[new_idx];
        colors[new_idx * 2 + 0] = old_colors[old_idx * 2 + 0];
        colors[new_idx * 2 + 1] = old_colors[old_idx * 2 + 1];
    }

    // Update g_sel_color
    if (g_sel_color >= 0 && g_sel_color < N) {
        g_sel_color = remap[g_sel_color];
    }

    // Update g_palette_selection mask
    bool new_selection[256] = {false};
    for (int i = 0; i < N; i++) {
        if (g_palette_selection[i]) {
            new_selection[remap[i]] = true;
        }
    }
    memcpy(g_palette_selection, new_selection, sizeof(g_palette_selection));

    // Remap pixel indices in all images that use this palette
    int remapped_images = 0;
    IMG *img = (IMG *)g_doc->img_p;
    while (img) {
        if ((int)img->palnum == g_doc->plselected && img->data_p && img->w > 0 && img->h > 0) {
            remapped_images++;
            unsigned short stride = (img->w + 3) & ~3;
            unsigned char *pixels = (unsigned char *)img->data_p;
            for (int y = 0; y < img->h; y++) {
                for (int x = 0; x < img->w; x++) {
                    unsigned char *idx = &pixels[y * stride + x];
                    *idx = remap[*idx];
                }
            }
        }
        img = (IMG *)img->nxt_p;
    }

    ApplyPalette(g_doc->plselected);
    save_palette_baseline();
    g_img_tex_idx = -2;
    mark_dirty();

    snprintf(g_restore_msg, sizeof(g_restore_msg),
             "Moved %d selected color%s to the end. Remapped %d sprite%s.",
             selected_count, selected_count == 1 ? "" : "s",
             remapped_images, remapped_images == 1 ? "" : "s");
    g_restore_msg_timer = 4.0f;
}

/* ---- Drawing Implementations ---- */


void DrawBottomPaletteBar(ImVec2 avail)
{
    float sw = avail.x;
    ImDrawList *dl   = ImGui::GetWindowDrawList();
    ImVec2      pos0 = ImGui::GetCursorScreenPos();
    float       gap  = 1.0f;
    float       header_h = 34.0f;
    int         pal_cols = 16;
    float       swatch = 7.0f;
    const int   col_options[] = {64, 48, 32, 16};
    float       grid_h_avail = PALETTE_H - header_h - 10.0f;
    float       grid_w_avail = sw - 8.0f;
    for (int opt : col_options) {
        int rows = (256 + opt - 1) / opt;
        float h_fit = floorf((grid_h_avail - gap * (float)(rows - 1)) / (float)rows);
        float w_fit = floorf((grid_w_avail - gap * (float)(opt - 1)) / (float)opt);
        float size = h_fit < w_fit ? h_fit : w_fit;
        if (size > 16.0f) size = 16.0f;
        if (size >= 7.0f) {
            pal_cols = opt;
            swatch = size;
            break;
        }
    }
    float       row_h = swatch + gap;
    float       col_w = swatch + gap;
    ImVec2      grid_pos(pos0.x, pos0.y + header_h);
    BuildSelectedPaletteUsage();
    PAL        *usage_pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    int         usage_numc = g_palette_usage_pal_numc;
    int         candidate_colors = usage_numc > 0 ? usage_numc - 1 : 0;
    SDL_Color  &selc = g_palette[g_sel_color];
    unsigned long long sel_use =
        (g_sel_color >= 0 && g_sel_color < 256) ? g_palette_usage_counts[g_sel_color] : 0;

    dl->AddRectFilled(pos0, ImVec2(pos0.x + sw, pos0.y + header_h - 3.0f),
                      IM_COL32(8, 8, 8, 245));
    ImGui::SetCursorScreenPos(ImVec2(pos0.x + 6.0f, pos0.y + 1.0f));
    ImGui::Text("Pal %d %.12s   #%d  R:%d G:%d B:%d",
                g_doc->plselected,
                usage_pal ? usage_pal->n_s : "",
                g_sel_color, selc.r, selc.g, selc.b);
    if (sw >= 980.0f) {
        ImGui::SameLine();
        ImGui::TextDisabled("selected %llu px", (unsigned long long)sel_use);
        ImGui::SameLine();
        ImGui::TextDisabled("%d/%d used   %d free   %d low <= %d",
                            g_palette_usage_used_colors,
                            candidate_colors,
                            g_palette_usage_unused_colors,
                            g_palette_usage_low_colors,
                            g_palette_usage_low_threshold);
    }
    ImGui::SetCursorScreenPos(ImVec2(pos0.x + 6.0f, pos0.y + 18.0f));
    if (sw < 980.0f) {
        ImGui::TextDisabled("selected %llu px   %d/%d used   %d free   %d low <= %d",
                            (unsigned long long)sel_use,
                            g_palette_usage_used_colors,
                            candidate_colors,
                            g_palette_usage_unused_colors,
                            g_palette_usage_low_colors,
                            g_palette_usage_low_threshold);
    } else {
        ImVec2 legend_pos = ImGui::GetCursorScreenPos();
        dl->AddTriangleFilled(ImVec2(legend_pos.x, legend_pos.y + 2.0f),
                              ImVec2(legend_pos.x + 8.0f, legend_pos.y + 2.0f),
                              ImVec2(legend_pos.x + 8.0f, legend_pos.y + 10.0f),
                              IM_COL32(0, 220, 255, 235));
        ImGui::SetCursorScreenPos(ImVec2(legend_pos.x + 14.0f, legend_pos.y));
        ImGui::TextDisabled("unused");
        ImGui::SameLine();
        ImVec2 dot_pos = ImGui::GetCursorScreenPos();
        dl->AddCircleFilled(ImVec2(dot_pos.x + 5.0f, dot_pos.y + 7.0f),
                            3.0f, IM_COL32(255, 185, 40, 255), 8);
        ImGui::SetCursorScreenPos(ImVec2(dot_pos.x + 14.0f, dot_pos.y));
        ImGui::TextDisabled("low use");
    }

    for (int i = 0; i < 256; i++) {
        int row = i / pal_cols, col = i % pal_cols;
        ImVec2 p0(grid_pos.x + col * col_w, grid_pos.y + row * row_h);
        ImVec2 p1(p0.x + swatch, p0.y + swatch);
        SDL_Color c = g_palette[i];
        bool in_palette = (i < usage_numc);
        unsigned long long use_count = g_palette_usage_counts[i];
        dl->AddRectFilled(p0, p1, IM_COL32(c.r, c.g, c.b, 255));

        if (i == g_sel_color)
            dl->AddRect(p0, p1, IM_COL32(255,255,255,255), 0, 0, 1.5f);
        else if (i == 0)
            dl->AddRect(p0, p1, IM_COL32(80,80,80,120), 0, 0, 0.5f);
        if (g_palette_selection[i]) {
            dl->AddRect(ImVec2(p0.x + 2, p0.y + 2),
                        ImVec2(p1.x - 2, p1.y - 2),
                        IM_COL32(255, 255, 0, 255), 0, 0, 1.5f);
        }

        if (g_isolate_color == i) {
            dl->AddRect(p0, p1, IM_COL32(255, 0, 255, 255), 0, 0, 2.0f);
        }

        if (in_palette && i > 0 && use_count == 0) {
            float tri = swatch < 10.0f ? 5.0f : 7.0f;
            ImVec2 t0(p1.x - tri, p0.y);
            ImVec2 t1(p1.x, p0.y);
            ImVec2 t2(p1.x, p0.y + tri);
            dl->AddTriangleFilled(t0, t1, t2, IM_COL32(0, 220, 255, 235));
            float inset = swatch < 10.0f ? 1.0f : 2.0f;
            dl->AddLine(ImVec2(p0.x + inset, p1.y - inset),
                        ImVec2(p1.x - inset, p0.y + inset),
                        IM_COL32(0, 0, 0, 220), 1.25f);
            dl->AddLine(ImVec2(p0.x + inset, p1.y - inset),
                        ImVec2(p1.x - inset, p0.y + inset),
                        IM_COL32(255, 255, 255, 235), 0.75f);
        } else if (in_palette && i > 0 &&
                   use_count <= (unsigned long long)g_palette_usage_low_threshold) {
            float r = swatch < 10.0f ? 1.6f : 2.2f;
            ImVec2 dot(p1.x - r - 1.0f, p0.y + r + 1.0f);
            dl->AddCircleFilled(dot, r + 0.7f, IM_COL32(0, 0, 0, 210), 8);
            dl->AddCircleFilled(dot, r, IM_COL32(255, 185, 40, 255), 8);
        }

        ImGui::SetCursorScreenPos(p0);
        ImGui::InvisibleButton(("##sw" + std::to_string(i)).c_str(), ImVec2(swatch, swatch));
        if (ImGui::IsItemClicked()) {
            ImGuiIO &cio = ImGui::GetIO();
            if (cio.KeyAlt) {
                g_isolate_color = (g_isolate_color == i) ? -1 : i;
            } else if (cio.KeyCtrl) {
                commit_palette_adjustments();
                g_palette_selection[i] = !g_palette_selection[i];
            } else if (cio.KeyShift) {
                commit_palette_adjustments();
                int start = g_sel_color < i ? g_sel_color : i;
                int end = g_sel_color < i ? i : g_sel_color;
                for (int j = start; j <= end; j++) g_palette_selection[j] = true;
                g_sel_color = i;
            } else {
                commit_palette_adjustments();
                memset(g_palette_selection, 0, sizeof(g_palette_selection));
                g_sel_color = i;
            }
        }
        if (ImGui::BeginPopupContextItem(("##swctx" + std::to_string(i)).c_str())) {
            if (ImGui::MenuItem("Copy This Color")) {
                g_sel_color = i;
                CopySelectedPaletteColor();
            }
            if (!g_palette_color_clipboard.valid) ImGui::BeginDisabled();
            if (ImGui::MenuItem("Paste Copied Color Here"))
                PastePaletteColorAt(i);
            if (!g_palette_color_clipboard.valid) ImGui::EndDisabled();
            ImGui::Separator();
            {
                /* Multi-slot copy/paste: keeps index positions, so the same
                   colors can be dropped into another palette without
                   recoloring any sprite that references those indices. */
                int sel_slots = CountSelectedPaletteSlots();
                char copy_label[64];
                snprintf(copy_label, sizeof(copy_label),
                         "Copy Selected Colors (%d)", sel_slots);
                if (ImGui::MenuItem(copy_label, NULL, false, sel_slots > 0))
                    CopySelectedPaletteSlots();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Copies every Ctrl/Shift-selected swatch along with its\n"
                                      "index, or just this one if nothing is multi-selected.");

                int clip_slots = PaletteSlotClipboardCount();
                char paste_label[64];
                snprintf(paste_label, sizeof(paste_label),
                         "Paste Colors at Same Indices (%d)", clip_slots);
                if (ImGui::MenuItem(paste_label, NULL, false, clip_slots > 0))
                    PastePaletteSlotsAtSameIndices();
                if (clip_slots > 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Writes the %d copied color%s from %.9s into this palette\n"
                                      "at their original indices (up to #%d), growing it if needed.",
                                      clip_slots, clip_slots == 1 ? "" : "s",
                                      PaletteSlotClipboardSource(),
                                      PaletteSlotClipboardMaxIndex());

                char append_label[64];
                snprintf(append_label, sizeof(append_label),
                         "Paste Colors at End (%d)", clip_slots);
                if (ImGui::MenuItem(append_label, NULL, false, clip_slots > 0))
                    PastePaletteSlotsAppendToEnd();
                if (clip_slots > 0 && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Appends them after this palette's last color instead,\n"
                                      "so nothing already using those indices is recolored.");
            }
            ImGui::Separator();
            if (i == 0) {
                if (ImGui::MenuItem("Copy #0 to Free Opaque Slot")) {
                    CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None, -1);
                }
            } else {
                if (ImGui::MenuItem("Copy #0 Color Here")) {
                    CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None, i);
                }
            }
            ImGui::Separator();
            if (!g_grid_sel.active) ImGui::BeginDisabled();
            if (ImGui::MenuItem(i == 0 ? "Copy #0 + Remap Selection"
                                       : "Copy #0 Here + Remap Selection")) {
                CopyPaletteZeroAndRemap(PaletteZeroRemapMode::Selection, i == 0 ? -1 : i);
            }
            if (!g_grid_sel.active) ImGui::EndDisabled();
            if (ImGui::MenuItem(i == 0 ? "Copy #0 + Remap Current Sprite"
                                       : "Copy #0 Here + Remap Current Sprite")) {
                CopyPaletteZeroAndRemap(PaletteZeroRemapMode::CurrentImage, i == 0 ? -1 : i);
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Move Selected Colors to End")) {
                MoveSelectedPaletteColorsToEnd();
            }
            ImGui::EndPopup();
        }
        if (ImGui::IsItemHovered()) {
            ImGui::BeginTooltip();
            ImGui::Text("Palette index %d", i);
            if (!in_palette) {
                ImGui::TextDisabled("Outside selected palette (%d colors)", usage_numc);
            } else if (i == 0) {
                ImGui::TextDisabled("Index 0 pixels are transparent");
                ImGui::Text("Usage: %llu pixel%s",
                            (unsigned long long)use_count,
                            use_count == 1 ? "" : "s");
            } else {
                if (use_count == 0) {
                    ImGui::TextColored(ImVec4(0.35f, 0.90f, 1.0f, 1.0f),
                                       "Unused by sprites using this palette");
                } else {
                    ImGui::Text("Usage: %llu pixel%s across %d sprite%s",
                                (unsigned long long)use_count,
                                use_count == 1 ? "" : "s",
                                g_palette_usage_img_count,
                                g_palette_usage_img_count == 1 ? "" : "s");
                    if (use_count <= (unsigned long long)g_palette_usage_low_threshold) {
                        ImGui::TextColored(ImVec4(1.0f, 0.74f, 0.24f, 1.0f),
                                           "Low-use candidate");
                    }
                }
                int nearest_dist = 0;
                int nearest = FindNearestUsedPaletteSlotForUsage(i, &nearest_dist);
                if (nearest >= 0) {
                    ImGui::TextDisabled("Nearest used color: #%d (distance %.1f)",
                                        nearest, sqrt((double)nearest_dist));
                }
            }
            ImGui::TextDisabled("Right-click: #0 relocation tools");
            ImGui::TextDisabled("Alt+click: isolate this color in the canvas");
            ImGui::TextDisabled("Ctrl/Shift+click: multi-select — selected swatches stay lit, rest dim on canvas");
            ImGui::EndTooltip();
        }
    }
}

void DrawPaletteMergeQualityDialog(void)
{
    if (g_show_palette_merge_quality) ImGui::OpenPopup("Palette Merge Quality Check");
    if (!ImGui::BeginPopupModal("Palette Merge Quality Check", &g_show_palette_merge_quality,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    const PaletteMergeQuality &q = g_palette_merge_quality;

    bool changed = false;
    changed |= ImGui::Checkbox("Grow target (add used source colors to free slots)",
                               &g_merge_opt_grow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Append source colors the sprites actually use into the\n"
                          "target's empty slots (lossless) instead of approximating\n"
                          "them. Falls back to nearest match when slots run out.");
    changed |= ImGui::Checkbox("Perceptual color match (luma-weighted)",
                               &g_merge_opt_perceptual);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Weight nearest-color search by how the eye perceives\n"
                          "brightness (green > red > blue) for closer-looking matches.");
    if (changed)
        BuildMarkedPaletteMergeQuality(&g_palette_merge_quality);
    ImGui::Separator();

    int drift_pixels = q.color_drift_pixels + q.transparent_drift_pixels;
    double avg_drift = q.affected_pixels > 0
        ? sqrt((double)q.total_dist / (double)q.affected_pixels)
        : 0.0;
    double max_drift = sqrt((double)q.max_dist);

    ImGui::Text("Target: %s", q.target_name);
    ImGui::Text("Marked palettes: %d   Images: %d   Nonzero pixels: %d",
                q.source_palettes, q.remapped_images, q.affected_pixels);
    if (q.opt_grow && (q.colors_added > 0 || q.colors_overflow > 0)) {
        ImGui::TextColored(ImVec4(0.5f, 0.85f, 0.6f, 1.0f),
                           "Target grows: %d -> %d colors (+%d added)",
                           q.target_base_numc, q.target_base_numc + q.colors_added,
                           q.colors_added);
        if (q.colors_overflow > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f),
                               "%d color%s could not fit (no free slots) - approximated",
                               q.colors_overflow, q.colors_overflow == 1 ? "" : "s");
    }
    ImGui::Separator();

    if (drift_pixels == 0 && q.ppp_warning_images == 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.0f),
                           "No visual drift detected.");
    } else {
        if (q.color_drift_pixels > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.35f, 1.0f),
                               "Color drift: %d pixel%s",
                               q.color_drift_pixels,
                               q.color_drift_pixels == 1 ? "" : "s");
            ImGui::Text("Average 5-bit RGB drift: %.2f   Max: %.2f",
                        avg_drift, max_drift);
            if (q.max_dist > 0) {
                ImGui::Text("Worst: %.9s / %.15s  color %d -> %d",
                            q.max_palette, q.max_image,
                            q.max_src_slot, q.max_dst_slot);
            }
        }
        if (q.transparent_drift_pixels > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                               "Transparency drift: %d pixel%s would become index 0",
                               q.transparent_drift_pixels,
                               q.transparent_drift_pixels == 1 ? "" : "s");
        }
        if (q.invalid_pixels > 0) {
            ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                               "Out-of-range source indices: %d pixel%s",
                               q.invalid_pixels,
                               q.invalid_pixels == 1 ? "" : "s");
        }
        if (q.ppp_warning_images > 0) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.82f, 0.35f, 1.0f));
            ImGui::TextWrapped("LOAD2 PPP risk: %d image%s would move to a palette over the current PPP limit",
                               q.ppp_warning_images,
                               q.ppp_warning_images == 1 ? "" : "s");
            ImGui::PopStyleColor();
        }
    }

    ImGui::Spacing();
    DrawPaletteMergeMappingPreview(q);

    ImGui::Spacing();
    const char *merge_label = drift_pixels > 0 ? "Merge Anyway" : "Merge";
    if (ImGui::Button(merge_label, ImVec2(120, 0))) {
        g_show_palette_merge_quality = false;
        g_palette_merge_preview_only = false;
        ImGui::CloseCurrentPopup();
        MergeMarkedPalettes(true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_palette_merge_quality = false;
        g_palette_merge_preview_only = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawPaletteReduceDialog(void)
{
    if (g_show_palette_reduce) {
        ImGui::SetNextWindowSize(ImVec2(760, 560), ImGuiCond_Appearing);
        ImGui::OpenPopup("Downscale Palette");
    }
    if (!ImGui::BeginPopupModal("Downscale Palette", &g_show_palette_reduce,
                                ImGuiWindowFlags_NoSavedSettings)) {
        if (!g_show_palette_reduce) ClearPaletteReducePreviewTextures();
        return;
    }

    int pal_idx = g_doc->plselected;
    PAL *pal = (pal_idx >= 0) ? get_pal(pal_idx) : NULL;
    if (!pal || !pal->data_p) {
        ImGui::TextDisabled("No palette selected");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_palette_reduce = false;
            ClearPaletteReducePreviewTextures();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    PaletteReductionPlan plan = {};
    BuildPaletteReductionPlan(pal_idx, g_palette_reduce_bpp, &plan);

    ImGui::Text("%.9s", pal->n_s);
    ImGui::SameLine();
    ImGui::TextDisabled("%u colors, %u bpp", pal->numc, pal->bitspix);

    bool changed_bpp = false;
    for (int bpp = 8; bpp >= 4; bpp--) {
        char label[32];
        snprintf(label, sizeof(label), "%d bpp (%d)", bpp, 1 << bpp);
        if (bpp != 8) ImGui::SameLine();
        if (ImGui::RadioButton(label, g_palette_reduce_bpp == bpp)) {
            g_palette_reduce_bpp = bpp;
            changed_bpp = true;
        }
    }
    if (changed_bpp) {
        ClearPaletteReducePreviewTextures();
        BuildPaletteReductionPlan(pal_idx, g_palette_reduce_bpp, &plan);
    }

    if (!plan.valid) {
        ImGui::TextDisabled("%s", plan.error[0] ? plan.error : "Unable to build reduction preview.");
        if (ImGui::Button("Close", ImVec2(100, 0))) {
            g_show_palette_reduce = false;
            ClearPaletteReducePreviewTextures();
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
        return;
    }

    const char *reduce_mode = plan.quantized ? "weighted merge"
                            : (plan.old_numc == plan.new_numc ? "unchanged"
                                                               : "exact pack");
    ImGui::TextDisabled("Target max: %d colors. Active: %d opaque. Result: %d colors. Mode: %s.",
                        plan.target_numc, plan.active_colors,
                        plan.new_numc, reduce_mode);
    ImGui::TextDisabled("Affected: %d image%s, %d px remapped.",
                        plan.images, plan.images == 1 ? "" : "s",
                        plan.pixels_changed);
    ImGui::Separator();

    g_palette_reduce_preview_idx = FindPalettePreviewImage(pal_idx, g_palette_reduce_preview_idx);
    IMG *preview_img = get_img(g_palette_reduce_preview_idx);
    if (preview_img) {
        if (ImGui::SmallButton("<##pal_reduce_prev")) {
            g_palette_reduce_preview_idx =
                StepPalettePreviewImage(pal_idx, g_palette_reduce_preview_idx, -1);
            ClearPaletteReducePreviewTextures();
            preview_img = get_img(g_palette_reduce_preview_idx);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(">##pal_reduce_next")) {
            g_palette_reduce_preview_idx =
                StepPalettePreviewImage(pal_idx, g_palette_reduce_preview_idx, 1);
            ClearPaletteReducePreviewTextures();
            preview_img = get_img(g_palette_reduce_preview_idx);
        }
        ImGui::SameLine();
        std::string pname = img_name_string(preview_img);
        ImGui::TextDisabled("[%d] %s", g_palette_reduce_preview_idx, pname.c_str());

        RebuildPaletteReducePreviewTextures(plan);
        float max_w = 330.0f;
        float max_h = 320.0f;
        if (ImGui::BeginTable("##pal_reduce_preview_table", 2,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            DrawPaletteReduceImageColumn("Current", g_palette_reduce_orig_tex, preview_img, max_w, max_h);
            ImGui::TableNextColumn();
            DrawPaletteReduceImageColumn("Downscaled", g_palette_reduce_new_tex, preview_img, max_w, max_h);
            ImGui::EndTable();
        }
    } else {
        if (ImGui::BeginTable("##pal_reduce_swatch_table", 2,
                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame)) {
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Current");
            DrawPaletteReduceSwatches((const unsigned char *)pal->data_p, plan.old_numc);
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Downscaled");
            DrawPaletteReduceSwatches(plan.data, plan.new_numc);
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    bool can_apply = plan.valid &&
        (plan.old_numc != plan.new_numc ||
         plan.pixels_changed > 0 ||
         (int)pal->bitspix != plan.target_bpp);
    ImGui::BeginDisabled(!can_apply);
    if (ImGui::Button("OK", ImVec2(100, 0))) {
        commit_palette_adjustments();
        PaletteReductionPlan apply_plan = {};
        if (BuildPaletteReductionPlan(pal_idx, g_palette_reduce_bpp, &apply_plan) &&
            ApplyPaletteReductionPlan(apply_plan)) {
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     "Downscaled palette %.9s to %d bpp: %d -> %d colors, %d px remapped.",
                     pal->n_s, apply_plan.target_bpp, apply_plan.old_numc,
                     apply_plan.new_numc, apply_plan.pixels_changed);
            g_restore_msg_timer = 5.0f;
        }
        g_show_palette_reduce = false;
        ClearPaletteReducePreviewTextures();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(100, 0))) {
        g_show_palette_reduce = false;
        ClearPaletteReducePreviewTextures();
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
    if (!g_show_palette_reduce) ClearPaletteReducePreviewTextures();
}

static unsigned char ApplyBrightnessContrast(float v, float brightness, float contrast)
{
    /* Contrast pivots around mid-gray, then brightness is a flat offset.
       contrast_factor: 0 -> unchanged, +1 -> doubled spread, -1 -> flat gray. */
    float contrast_factor = 1.0f + contrast;
    if (contrast_factor < 0.0f) contrast_factor = 0.0f;
    v = (v - 127.5f) * contrast_factor + 127.5f;
    v += brightness * 255.0f;
    if (v < 0.0f) v = 0.0f;
    if (v > 255.0f) v = 255.0f;
    return (unsigned char)lroundf(v);
}

static void ApplySingleColorRamp(const unsigned char *baseline, int count,
                                 float r, float g, float b, float opacity,
                                 float brightness, float contrast, unsigned char *out)
{
    if (!baseline || !out || count <= 0) return;
    if (r < 0.0f) r = 0.0f; if (r > 1.0f) r = 1.0f;
    if (g < 0.0f) g = 0.0f; if (g > 1.0f) g = 1.0f;
    if (b < 0.0f) b = 0.0f; if (b > 1.0f) b = 1.0f;
    if (opacity < 0.0f) opacity = 0.0f; if (opacity > 1.0f) opacity = 1.0f;
    if (brightness < -1.0f) brightness = -1.0f; if (brightness > 1.0f) brightness = 1.0f;
    if (contrast < -1.0f) contrast = -1.0f; if (contrast > 1.0f) contrast = 1.0f;
    for (int i = 0; i < count; i++) {
        unsigned short word = palette_word_at(baseline, i);
        if (i == 0) { /* transparent index remains untouched */
            out[0] = (unsigned char)word;
            out[1] = (unsigned char)(word >> 8);
            continue;
        }
        int sr = (word >> 10) & 31;
        int sg = (word >> 5) & 31;
        int sb = word & 31;
        /* Preserve each original swatch's perceptual brightness; the chosen
           color supplies hue/saturation, yielding a usable shade ramp. */
        float light = (30.0f * sr + 59.0f * sg + 11.0f * sb) / (100.0f * 31.0f);
        float shade_r = 255.0f * r * light;
        float shade_g = 255.0f * g * light;
        float shade_b = 255.0f * b * light;
        /* Opacity blends the shaded ramp back toward the untouched original
           swatch color, so it acts as an effect-strength control. */
        float orig_r = sr * (255.0f / 31.0f);
        float orig_g = sg * (255.0f / 31.0f);
        float orig_b = sb * (255.0f / 31.0f);
        float mix_r = orig_r + (shade_r - orig_r) * opacity;
        float mix_g = orig_g + (shade_g - orig_g) * opacity;
        float mix_b = orig_b + (shade_b - orig_b) * opacity;
        unsigned char rr = ApplyBrightnessContrast(mix_r, brightness, contrast);
        unsigned char gg = ApplyBrightnessContrast(mix_g, brightness, contrast);
        unsigned char bb = ApplyBrightnessContrast(mix_b, brightness, contrast);
        rgb8_to_pal_word(rr, gg, bb, out + i * 2);
    }
}

void OpenPaletteSingleColorDialog(void)
{
    g_show_palette_single_color = true;
    g_palette_single_color_ready = false;
}

void DrawPaletteSingleColorDialog(void)
{
    if (!g_show_palette_single_color) return;

    /* A regular floating window (not a modal popup) so the canvas stays
       interactive underneath it — needed for the right-click eyedropper. */
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
    bool open = true;
    if (!ImGui::Begin("Single-Color Shading", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    if (!open) {
        /* Closed via the window's X button: restore, same as Cancel. */
        PAL *close_pal = (g_palette_single_color_idx >= 0) ? get_pal(g_palette_single_color_idx) : NULL;
        if (close_pal && close_pal->data_p && g_palette_single_color_ready) {
            memcpy(close_pal->data_p, g_palette_single_color_baseline, (size_t)g_palette_single_color_count * 2);
            ApplyPalette(g_palette_single_color_idx);
            g_img_tex_idx = -2;
        }
        g_palette_single_color_ready = false;
        g_show_palette_single_color = false;
        ImGui::End();
        return;
    }

    PAL *pal = (g_doc && g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) {
        ImGui::TextDisabled("Select a palette first.");
        if (ImGui::Button("Close")) { g_show_palette_single_color = false; }
        ImGui::End();
        return;
    }
    int n = (int)pal->numc;
    if (n > 256) n = 256;
    if (!g_palette_single_color_ready || g_palette_single_color_idx != g_doc->plselected ||
        g_palette_single_color_count != n) {
        memcpy(g_palette_single_color_baseline, pal->data_p, (size_t)n * 2);
        g_palette_single_color_count = n;
        g_palette_single_color_idx = g_doc->plselected;
        g_palette_single_color_ready = true;
    }

    ImGui::TextWrapped("Choose the tree's base color. The preview keeps the source palette's brightness per swatch, so its shadows and highlights become one coherent color ramp. Right-click a sprite in the canvas to sample its color.");
    bool changed = ImGui::ColorPicker3("Base color", g_palette_single_color_rgb,
                                        ImGuiColorEditFlags_DisplayRGB |
                                        ImGuiColorEditFlags_InputRGB);

    /* Hex text entry, kept in sync with the picker except while the user is
       actively typing in it. */
    static char s_hex_buf[16] = "";
    static bool s_hex_active = false;
    if (!s_hex_active) {
        snprintf(s_hex_buf, sizeof(s_hex_buf), "#%02X%02X%02X",
                 (int)lroundf(g_palette_single_color_rgb[0] * 255.0f),
                 (int)lroundf(g_palette_single_color_rgb[1] * 255.0f),
                 (int)lroundf(g_palette_single_color_rgb[2] * 255.0f));
    }
    ImGui::SetNextItemWidth(100);
    bool hex_submit = ImGui::InputText("Hex", s_hex_buf, sizeof(s_hex_buf),
                                       ImGuiInputTextFlags_EnterReturnsTrue |
                                       ImGuiInputTextFlags_CharsUppercase);
    s_hex_active = ImGui::IsItemActive();
    if (hex_submit || ImGui::IsItemDeactivatedAfterEdit()) {
        const char *s = s_hex_buf;
        if (*s == '#') s++;
        unsigned int rv, gv, bv;
        if (sscanf(s, "%2x%2x%2x", &rv, &gv, &bv) == 3) {
            g_palette_single_color_rgb[0] = rv / 255.0f;
            g_palette_single_color_rgb[1] = gv / 255.0f;
            g_palette_single_color_rgb[2] = bv / 255.0f;
            changed = true;
        }
    }

    changed |= ImGui::SliderFloat("Opacity", &g_palette_single_color_opacity, 0.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Brightness", &g_palette_single_color_brightness, -1.0f, 1.0f, "%.2f");
    changed |= ImGui::SliderFloat("Contrast", &g_palette_single_color_contrast, -1.0f, 1.0f, "%.2f");
    if (changed) {
        ApplySingleColorRamp(g_palette_single_color_baseline, n,
                             g_palette_single_color_rgb[0],
                             g_palette_single_color_rgb[1],
                             g_palette_single_color_rgb[2],
                             g_palette_single_color_opacity,
                             g_palette_single_color_brightness,
                             g_palette_single_color_contrast,
                             (unsigned char *)pal->data_p);
        ApplyPalette(g_doc->plselected);
        g_img_tex_idx = -2;
    }
    ImGui::TextDisabled("Live preview — index 0 stays transparent.");
    ImGui::Separator();
    if (ImGui::Button("Apply", ImVec2(110, 0))) {
        /* Put the original back before taking undo so Ctrl+Z returns to it. */
        memcpy(pal->data_p, g_palette_single_color_baseline, (size_t)n * 2);
        doc_undo_push();
        ApplySingleColorRamp(g_palette_single_color_baseline, n,
                             g_palette_single_color_rgb[0],
                             g_palette_single_color_rgb[1],
                             g_palette_single_color_rgb[2],
                             g_palette_single_color_opacity,
                             g_palette_single_color_brightness,
                             g_palette_single_color_contrast,
                             (unsigned char *)pal->data_p);
        ApplyPalette(g_doc->plselected);
        g_img_tex_idx = -2;
        mark_dirty();
        commit_palette_adjustments();
        g_palette_single_color_ready = false;
        g_show_palette_single_color = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0))) {
        memcpy(pal->data_p, g_palette_single_color_baseline, (size_t)n * 2);
        ApplyPalette(g_doc->plselected);
        g_img_tex_idx = -2;
        g_palette_single_color_ready = false;
        g_show_palette_single_color = false;
    }
    ImGui::End();
}

void OpenIndexedGradientDialog(void)
{
    PAL *pal = (g_doc && g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
    if (!pal || !pal->data_p) return;
    g_indexed_gradient_palette_idx = g_doc->plselected;
    g_indexed_gradient_image_idx = g_doc->ilselected;
    g_indexed_gradient_palette_count = (int)pal->numc > 256 ? 256 : (int)pal->numc;
    memcpy(g_indexed_gradient_baseline, pal->data_p,
           (size_t)g_indexed_gradient_palette_count * 2);
    memset(g_indexed_gradient_targets, 0, sizeof(g_indexed_gradient_targets));
    for (int i = 1; i < g_indexed_gradient_palette_count; i++)
        g_indexed_gradient_targets[i] = g_palette_selection[i];
    g_indexed_gradient_color_count = 2;
    memset(g_indexed_gradient_colors, 0, sizeof(g_indexed_gradient_colors));
    memset(g_indexed_gradient_color_set, 0, sizeof(g_indexed_gradient_color_set));
    g_indexed_gradient_applied = false;
    LoadIndexedGradientPresets();
    g_show_indexed_gradient = true;
}

static void IndexedGradientRgb(const unsigned char *data, int idx, int *r, int *g, int *b)
{
    unsigned short word = (unsigned short)(data[idx * 2] | (data[idx * 2 + 1] << 8));
    *r = ((word >> 10) & 31) * 255 / 31;
    *g = ((word >> 5) & 31) * 255 / 31;
    *b = (word & 31) * 255 / 31;
}

static int BuildIndexedGradientPalette(unsigned char out[512])
{
    if (!out || g_indexed_gradient_color_count < 2) return -1;
    for (int i = 0; i < g_indexed_gradient_color_count; i++)
        if (!g_indexed_gradient_color_set[i]) return -1;
    memcpy(out, g_indexed_gradient_baseline,
           (size_t)g_indexed_gradient_palette_count * 2);

    int min_luma = 0x7fffffff, max_luma = -1, target_count = 0;
    int luma[256] = {};
    for (int i = 1; i < g_indexed_gradient_palette_count; i++) {
        if (!g_indexed_gradient_targets[i]) continue;
        int r, g, b;
        IndexedGradientRgb(g_indexed_gradient_baseline, i, &r, &g, &b);
        luma[i] = 30 * r + 59 * g + 11 * b;
        if (luma[i] < min_luma) min_luma = luma[i];
        if (luma[i] > max_luma) max_luma = luma[i];
        target_count++;
    }
    if (target_count == 0) return -1;
    int changed = 0;
    for (int i = 1; i < g_indexed_gradient_palette_count; i++) {
        if (!g_indexed_gradient_targets[i]) continue;
        float t = max_luma > min_luma
                ? (float)(luma[i] - min_luma) / (float)(max_luma - min_luma)
                : 0.5f;
        float scaled = t * (float)(g_indexed_gradient_color_count - 1);
        int seg = (int)floorf(scaled);
        if (seg >= g_indexed_gradient_color_count - 1) seg = g_indexed_gradient_color_count - 2;
        float f = scaled - (float)seg;
        int rgb[3];
        for (int c = 0; c < 3; c++)
            rgb[c] = (int)lroundf(255.0f * (g_indexed_gradient_colors[seg][c] +
                     (g_indexed_gradient_colors[seg + 1][c] - g_indexed_gradient_colors[seg][c]) * f));
        unsigned short word = (unsigned short)(((rgb[0] >> 3) << 10) |
                                               ((rgb[1] >> 3) << 5) |
                                                (rgb[2] >> 3));
        unsigned short old = (unsigned short)(out[i * 2] | (out[i * 2 + 1] << 8));
        if (word != old) changed++;
        out[i * 2] = (unsigned char)(word & 0xff);
        out[i * 2 + 1] = (unsigned char)(word >> 8);
    }
    return changed;
}

static int ApplyIndexedGradientToPalette(void)
{
    PAL *pal = get_pal(g_indexed_gradient_palette_idx);
    if (!pal || !pal->data_p) return 0;
    unsigned char result[512] = {};
    int changed = BuildIndexedGradientPalette(result);
    if (changed < 0) return 0;
    doc_undo_push();
    memcpy(pal->data_p, result, (size_t)g_indexed_gradient_palette_count * 2);
    ApplyPalette(g_indexed_gradient_palette_idx);
    save_palette_baseline();
    InvalidatePaletteSync();
    InvalidatePaletteUsage();
    g_img_tex_idx = -2;
    mark_dirty();
    return changed;
}

static SDL_Texture *BuildIndexedGradientPreview(void)
{
    if (g_indexed_gradient_preview_tex) {
        SDL_DestroyTexture(g_indexed_gradient_preview_tex);
        g_indexed_gradient_preview_tex = NULL;
    }
    IMG *img = get_img(g_indexed_gradient_image_idx);
    if (!g_imgui_renderer || !img || !img->data_p ||
        (int)img->palnum != g_indexed_gradient_palette_idx) return NULL;
    unsigned char preview_pal[512] = {};
    if (BuildIndexedGradientPalette(preview_pal) < 0) return NULL;
    g_indexed_gradient_preview_tex = SDL_CreateTexture(
        g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
        img->w, img->h);
    if (!g_indexed_gradient_preview_tex) return NULL;
    SDL_SetTextureBlendMode(g_indexed_gradient_preview_tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(g_indexed_gradient_preview_tex, SDL_ScaleModeNearest);
    void *locked = NULL; int pitch = 0;
    if (SDL_LockTexture(g_indexed_gradient_preview_tex, NULL, &locked, &pitch) != 0) {
        SDL_DestroyTexture(g_indexed_gradient_preview_tex);
        g_indexed_gradient_preview_tex = NULL;
        return NULL;
    }
    int stride = (img->w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    for (int y = 0; y < (int)img->h; y++) {
        Uint32 *row = (Uint32 *)((unsigned char *)locked + y * pitch);
        for (int x = 0; x < (int)img->w; x++) {
            unsigned char ci = src[y * stride + x];
            int r = 0, g = 0, b = 0;
            if (ci < g_indexed_gradient_palette_count)
                IndexedGradientRgb(preview_pal, ci, &r, &g, &b);
            row[x] = (ci == 0 ? 0u : 0xff000000u) |
                     ((Uint32)r << 16) | ((Uint32)g << 8) | (Uint32)b;
        }
    }
    SDL_UnlockTexture(g_indexed_gradient_preview_tex);
    return g_indexed_gradient_preview_tex;
}

void DrawIndexedGradientDialog(void)
{
    if (!g_show_indexed_gradient) {
        if (g_indexed_gradient_preview_tex) {
            SDL_DestroyTexture(g_indexed_gradient_preview_tex);
            g_indexed_gradient_preview_tex = NULL;
        }
        return;
    }
    ImGui::SetNextWindowSize(ImVec2(470, 0), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Indexed Color Gradient", &g_show_indexed_gradient,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }
    PAL *pal = get_pal(g_indexed_gradient_palette_idx);
    if (!pal || !pal->data_p) {
        ImGui::TextDisabled("The original palette is no longer available.");
        ImGui::End();
        return;
    }
    int target_count = 0;
    for (int i = 1; i < g_indexed_gradient_palette_count; i++)
        if (g_indexed_gradient_targets[i]) target_count++;
    ImGui::TextWrapped("The %d palette color%s selected before opening will be fitted to this ramp by original brightness. Palette slots and every sprite pixel index stay in exactly the same place.",
                       target_count, target_count == 1 ? "" : "s");
    if (!g_indexed_gradient_presets.empty()) {
        ImGui::SeparatorText("Saved gradients");
        const float swatch_w = 56.0f, swatch_h = 56.0f;
        for (size_t pi = 0; pi < g_indexed_gradient_presets.size(); pi++) {
            const IndexedGradientPreset &preset = g_indexed_gradient_presets[pi];
            ImGui::PushID((int)pi);
            if (pi > 0 && (pi % 5) != 0) ImGui::SameLine();
            ImGui::BeginGroup();
            ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##gradient_preset", ImVec2(swatch_w, swatch_h)))
                SelectIndexedGradientPreset(preset);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            int n = (int)preset.colors.size();
            const int slices = 32;
            for (int i = 0; i < slices; i++) {
                float t = (float)i / (float)(slices - 1);
                float scaled = t * (float)(n - 1);
                int seg = (int)floorf(scaled);
                if (seg >= n - 1) seg = n - 2;
                float f = scaled - (float)seg;
                ImVec4 c;
                c.x = preset.colors[seg].x + (preset.colors[seg + 1].x - preset.colors[seg].x) * f;
                c.y = preset.colors[seg].y + (preset.colors[seg + 1].y - preset.colors[seg].y) * f;
                c.z = preset.colors[seg].z + (preset.colors[seg + 1].z - preset.colors[seg].z) * f;
                c.w = 1.0f;
                float x0 = p.x + swatch_w * (float)i / (float)slices;
                float x1 = p.x + swatch_w * (float)(i + 1) / (float)slices;
                dl->AddRectFilled(ImVec2(x0, p.y), ImVec2(x1 + 1.0f, p.y + swatch_h),
                                  ImGui::ColorConvertFloat4ToU32(c));
            }
            dl->AddRect(p, ImVec2(p.x + swatch_w, p.y + swatch_h),
                        ImGui::IsItemHovered() ? IM_COL32(255, 220, 90, 255)
                                               : IM_COL32(150, 150, 150, 255),
                        2.0f, 0, ImGui::IsItemHovered() ? 2.0f : 1.0f);
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + swatch_w);
            ImGui::TextUnformatted(preset.name.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Use %s (%d colors)", preset.name.c_str(), n);
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    ImGui::Text("Gradient colors (%d / 11)", g_indexed_gradient_color_count);
    bool all_set = true;
    for (int i = 0; i < g_indexed_gradient_color_count; i++) {
        ImGui::PushID(i);
        ImGui::Text("%d", i + 1); ImGui::SameLine();
        ImGui::SetNextItemWidth(250);
        bool color_changed = ImGui::ColorEdit3("##color", g_indexed_gradient_colors[i],
                                               ImGuiColorEditFlags_PickerHueWheel |
                                               ImGuiColorEditFlags_DisplayRGB);
        if (color_changed || ImGui::IsItemActivated())
            g_indexed_gradient_color_set[i] = true;
        if (!g_indexed_gradient_color_set[i]) {
            all_set = false;
            ImGui::SameLine(); ImGui::TextDisabled("empty");
        }
        if (g_indexed_gradient_color_count > 2) {
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                for (int j = i; j + 1 < g_indexed_gradient_color_count; j++) {
                    memcpy(g_indexed_gradient_colors[j], g_indexed_gradient_colors[j + 1], sizeof(g_indexed_gradient_colors[j]));
                    g_indexed_gradient_color_set[j] = g_indexed_gradient_color_set[j + 1];
                }
                g_indexed_gradient_color_count--; i--;
            }
        }
        ImGui::PopID();
    }
    if (g_indexed_gradient_color_count >= 11) ImGui::BeginDisabled();
    if (ImGui::Button("Add color")) {
        memset(g_indexed_gradient_colors[g_indexed_gradient_color_count], 0,
               sizeof(g_indexed_gradient_colors[g_indexed_gradient_color_count]));
        g_indexed_gradient_color_set[g_indexed_gradient_color_count] = false;
        g_indexed_gradient_color_count++;
    }
    if (g_indexed_gradient_color_count >= 11) ImGui::EndDisabled();
    ImGui::TextDisabled("Click a swatch to open its color wheel. All colors must be chosen before Apply.");
    ImGui::SeparatorText("Preview");
    SDL_Texture *preview = all_set ? BuildIndexedGradientPreview() : NULL;
    IMG *preview_img = get_img(g_indexed_gradient_image_idx);
    if (preview && preview_img) {
        const float max_w = 400.0f, max_h = 260.0f;
        float scale_w = max_w / (float)preview_img->w;
        float scale_h = max_h / (float)preview_img->h;
        float scale = scale_w < scale_h ? scale_w : scale_h;
        if (scale > 8.0f) scale = 8.0f;
        if (scale < 1.0f) scale = 1.0f;
        ImVec2 size((float)preview_img->w * scale,
                    (float)preview_img->h * scale);
        ImGui::Image((ImTextureID)(intptr_t)preview, size);
    } else {
        ImGui::TextDisabled(all_set ? "Preview unavailable for this sprite."
                                    : "Choose every gradient color to preview the result.");
    }
    ImGui::Separator();
    if (!all_set || target_count == 0) ImGui::BeginDisabled();
    if (ImGui::Button("Apply", ImVec2(110, 0))) {
        int changed = ApplyIndexedGradientToPalette();
        g_indexed_gradient_applied = true;
        snprintf(g_restore_msg, sizeof(g_restore_msg), "Gradient recolored %d palette slot%s; sprite indices unchanged.", changed, changed == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
        /* Keep the configured colors and tool open for further refinement. */
    }
    if (!all_set || target_count == 0) ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!all_set || !g_indexed_gradient_applied);
    if (ImGui::Button("Save Gradient...", ImVec2(130, 0))) {
        g_indexed_gradient_preset_name[0] = '\0';
        g_indexed_gradient_open_name_popup = true;
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
        ImGui::SetTooltip(g_indexed_gradient_applied
            ? "Save this ramp to your user profile for reuse."
            : "Apply the gradient before saving it as a preset.");
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(110, 0))) g_show_indexed_gradient = false;

    if (g_indexed_gradient_open_name_popup) {
        ImGui::OpenPopup("Name Gradient");
        g_indexed_gradient_open_name_popup = false;
    }
    if (ImGui::BeginPopupModal("Name Gradient", NULL,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Name this gradient preset:");
        ImGui::SetNextItemWidth(300.0f);
        bool submitted = ImGui::InputText("##gradient_name",
                                          g_indexed_gradient_preset_name,
                                          sizeof(g_indexed_gradient_preset_name),
                                          ImGuiInputTextFlags_EnterReturnsTrue |
                                          ImGuiInputTextFlags_AutoSelectAll);
        if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere(-1);
        char *begin = g_indexed_gradient_preset_name;
        while (*begin && std::isspace((unsigned char)*begin)) begin++;
        char *end = begin + strlen(begin);
        while (end > begin && std::isspace((unsigned char)end[-1])) *--end = '\0';
        for (char *p = begin; *p; p++) if (*p == '|') *p = '-';
        bool has_name = *begin != '\0';
        ImGui::BeginDisabled(!has_name);
        if (ImGui::Button("Save", ImVec2(100, 0)) || (submitted && has_name)) {
            IndexedGradientPreset preset;
            preset.name = begin;
            for (int i = 0; i < g_indexed_gradient_color_count; i++)
                preset.colors.push_back(ImVec4(g_indexed_gradient_colors[i][0],
                                               g_indexed_gradient_colors[i][1],
                                               g_indexed_gradient_colors[i][2], 1.0f));
            bool replaced = false;
            for (IndexedGradientPreset &existing : g_indexed_gradient_presets) {
                if (_stricmp(existing.name.c_str(), preset.name.c_str()) == 0) {
                    existing = preset;
                    replaced = true;
                    break;
                }
            }
            if (!replaced) g_indexed_gradient_presets.push_back(std::move(preset));
            bool saved = SaveIndexedGradientPresets();
            snprintf(g_restore_msg, sizeof(g_restore_msg),
                     saved ? "Saved gradient preset '%s'." : "Could not save gradient preset.",
                     begin);
            g_restore_msg_timer = 4.0f;
            if (saved) ImGui::CloseCurrentPopup();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(100, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void DrawPaletteHistogramDialog(void)
{
    if (g_show_histogram) ImGui::OpenPopup("Palette Histogram");
    if (!ImGui::BeginPopupModal("Palette Histogram", &g_show_histogram, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::Text("Images using this palette: %d", g_histogram_img_count);
    ImGui::Text("Max occurrences (excluding index 0): %.0f", g_histogram_max);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p = ImGui::GetCursorScreenPos();
    float width = 512.0f;
    float height = 200.0f;
    dl->AddRectFilled(p, ImVec2(p.x + width, p.y + height), IM_COL32(20, 20, 20, 255));
    dl->AddRect(p, ImVec2(p.x + width, p.y + height), IM_COL32(100, 100, 100, 255));

    for (int i = 0; i < 256; i++) {
        float val = g_histogram_data[i];
        if (val > 0.0f) {
            float bar_h = (val / g_histogram_max) * height;
            if (bar_h < 1.0f) bar_h = 1.0f;
            if (i == 0) bar_h = height;
            ImVec2 p0(p.x + i * 2.0f, p.y + height - bar_h);
            ImVec2 p1(p.x + (i + 1) * 2.0f, p.y + height);
            SDL_Color c = g_palette[i];
            dl->AddRectFilled(p0, p1, IM_COL32(c.r, c.g, c.b, i == 0 ? 80 : 255));
        }
    }

    ImGui::Dummy(ImVec2(width, height));
    ImGui::Spacing();
    if (ImGui::Button("Close", ImVec2(100, 0))) {
        g_show_histogram = false;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void DrawRightPanelPaletteEditor(float panel_h)
{
    int n_pals = count_pals();
    if (ImGui::CollapsingHeader("Palettes", ImGuiTreeNodeFlags_DefaultOpen)) {
        float list_h = panel_h * 0.22f;
        if (ImGui::BeginListBox("##pallist", ImVec2(-1, list_h))) {
            if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                g_palette_nav = true;
            for (int i = 0; i < n_pals; i++) {
                PAL *pal = get_pal(i);
                if (!pal) break;
                bool sel    = (i == g_doc->plselected);
                bool marked = (pal->flags & 1) != 0;
                ImGui::PushID(1000 + i);
                char label[16];
                if (marked) snprintf(label, sizeof(label), "* %s", pal->n_s);
                else        snprintf(label, sizeof(label), "  %s", pal->n_s);

                if (sel) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.3f, 1.0f, 0.3f, 1.0f));
                if (ImGui::Selectable(label, sel)) {
                    SelectPalette(i);
                }
                if (sel) ImGui::PopStyleColor();

                if (ImGui::BeginPopupContextItem("##palctx")) {
                    if (ImGui::MenuItem("Mark / Unmark"))             pal->flags ^= 1;
                    {
                        /* Right-clicking a row targets that row, which is not
                           necessarily the selected palette — paste into it in
                           place rather than selecting it, since SelectPalette
                           would also reassign the current sprite to it. */
                        int clip_slots = PaletteSlotClipboardCount();
                        char paste_label[72];
                        snprintf(paste_label, sizeof(paste_label),
                                 "Paste %d Copied Color%s at Same Indices",
                                 clip_slots, clip_slots == 1 ? "" : "s");
                        if (ImGui::MenuItem(paste_label, NULL, false, clip_slots > 0))
                            PastePaletteSlotsAtSameIndices(i);
                        if (clip_slots > 0 && ImGui::IsItemHovered())
                            ImGui::SetTooltip("From %.9s, into this palette at the original indices.\n"
                                              "Leaves the palette selection and sprite assignment alone.",
                                              PaletteSlotClipboardSource());

                        snprintf(paste_label, sizeof(paste_label),
                                 "Paste %d Copied Color%s at End",
                                 clip_slots, clip_slots == 1 ? "" : "s");
                        if (ImGui::MenuItem(paste_label, NULL, false, clip_slots > 0))
                            PastePaletteSlotsAppendToEnd(i);
                        if (clip_slots > 0 && ImGui::IsItemHovered())
                            ImGui::SetTooltip("Appended after this palette's last color, so the\n"
                                              "indices it already uses keep their colors.");
                    }
                    ImGui::Separator();
                    if (ImGui::MenuItem("Add New"))                   AddNewPalette();
                    if (ImGui::MenuItem("Duplicate"))                 DuplicatePalette();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Set for Image",         "]"))       SetPaletteOfSelected();
                    if (ImGui::MenuItem("Set for Marked Images", "["))       SetPaletteOfMarked();
                    if (ImGui::MenuItem("Merge Marked into Selected", "*"))  MergeMarkedPalettes();
                    if (ImGui::MenuItem("Preview Merge"))                    OpenPaletteMergePreview();
                    ImGui::Separator();
                    if (ImGui::MenuItem("Clean Up Palette")) CleanupSelectedPalette();
                    if (ImGui::MenuItem("Group Like Colors")) GroupLikeColorsSelectedPalette();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Clean up, then cluster similar colors so each hue family\n"
                        "sits together as its own dark-to-light ramp.");
                    if (ImGui::MenuItem("Clean Copy Palette")) CreateCleanedPaletteCopy();
                    if (ImGui::MenuItem("Single-Color Shading...")) OpenPaletteSingleColorDialog();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Choose one base color and turn every opaque palette swatch\n"
                        "into a matching dark-to-light shade of that color.");
                    if (ImGui::MenuItem("Inherit Colors from Marked")) InheritSelectedPaletteFromMarked();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Mark the source palette, select the target palette.\n"
                        "Sprites using the target are remapped to nearest source colors.");
                    if (ImGui::MenuItem("Merge Duplicate Palettes")) MergeDuplicatePalettes();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Remaps sprites using later duplicate palettes to the first matching palette.");
                    {
                        /* Reports the current value so it's obvious whether
                           this row actually needs fixing. */
                        int want = pal->numc ? PaletteBppForColorCount((int)pal->numc) : 0;
                        bool wrong = pal->numc && (int)pal->bitspix != want;
                        char label[72];
                        if (wrong) snprintf(label, sizeof(label),
                                            "Recalculate BPP (%u -> %d for %u colors)",
                                            pal->bitspix, want, pal->numc);
                        else       snprintf(label, sizeof(label),
                                            "Recalculate BPP (already %u)", pal->bitspix);
                        if (ImGui::MenuItem(label, NULL, false, wrong)) {
                            SelectPalette(i);
                            RecalculateSelectedPaletteBpp();
                        }
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip("Set the declared bits-per-pixel from the color count.\n"
                                              "Colors are untouched; TBL/IRW/LOAD2 exports pack at this depth.");
                    }
                    if (ImGui::MenuItem("Downscale Palette...")) OpenPaletteReduceDialog(7);
                    if (ImGui::MenuItem("Copy #0 to Opaque Slot")) CopyPaletteZeroToOpaqueSlot();
                    if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                        "Copies palette index 0's RGB into a nonzero slot.\n"
                        "Pixels with index 0 still remain transparent.");
                    if (ImGui::MenuItem("Import Palette...")) OpenFileDialog(FileDialogMode::ImportPalette);
                    if (ImGui::MenuItem("Export Palette...")) OpenFileDialog(FileDialogMode::ExportPalette);
                    if (ImGui::MenuItem("Show Histogram")) { CalculatePaletteHistogram(); g_show_histogram = true; }
                    if (ImGui::MenuItem("Rename", "Shift+R")) OpenRenamePalette(i);
                    if (ImGui::MenuItem("Delete", "Del")) DeletePalette();
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
            ImGui::EndListBox();
        }
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
        if (ImGui::Button("+##addpal", ImVec2(24, 20))) { AddNewPalette(); }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add a new blank 256-color palette");
        ImGui::SameLine();

        if (g_doc->plselected < 0) ImGui::BeginDisabled();
        if (ImGui::Button("Dup##pal", ImVec2(44, 20))) DuplicatePalette();
        if (g_doc->plselected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Duplicate selected palette");
        ImGui::SameLine();
        if (ImGui::Button("Del##pal", ImVec2(44, 20))) DeletePalette();
        if (g_doc->plselected >= 0 && ImGui::IsItemHovered()) ImGui::SetTooltip("Delete selected palette");
        if (g_doc->plselected < 0) ImGui::EndDisabled();
        ImGui::SameLine();

        if (ImGui::Button("Operations...##palops", ImVec2(-1, 20))) {
            ImGui::OpenPopup("palette_operations_popup");
        }
        ImGui::PopStyleVar();

        if (ImGui::BeginPopup("palette_operations_popup")) {
            if (ImGui::BeginMenu("Marking")) {
                if (ImGui::MenuItem("Mark All")) { PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags|=1; p=(PAL*)p->nxt_p;} }
                if (ImGui::MenuItem("Clear All")) { PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags&=~1; p=(PAL*)p->nxt_p;} }
                if (ImGui::MenuItem("Invert Marks")) { PAL *p=(PAL*)g_doc->pal_p; while(p){p->flags^=1; p=(PAL*)p->nxt_p;} }
                if (g_doc->plselected < 0) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Mark/Unmark Selected")) { PAL *p=get_pal(g_doc->plselected); if(p) p->flags^=1; }
                if (g_doc->plselected < 0) ImGui::EndDisabled();
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu("Clipboard & Files")) {
                {
                    int sel_slots = CountSelectedPaletteSlots();
                    int clip_slots = PaletteSlotClipboardCount();
                    char label[72];
                    snprintf(label, sizeof(label), "Copy Selected Colors (%d)", sel_slots);
                    if (ImGui::MenuItem(label, NULL, false, sel_slots > 0))
                        CopySelectedPaletteSlots();
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("Ctrl/Shift+click swatches to select several, then copy them\n"
                                          "with their index positions.");
                    snprintf(label, sizeof(label),
                             "Paste Colors at Same Indices (%d)", clip_slots);
                    if (ImGui::MenuItem(label, NULL, false, clip_slots > 0))
                        PastePaletteSlotsAtSameIndices();
                    if (clip_slots > 0 && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Into the selected palette, from %.9s, keeping index #s\n"
                                          "(up to #%d). Grows the palette if it is too short.",
                                          PaletteSlotClipboardSource(),
                                          PaletteSlotClipboardMaxIndex());
                    snprintf(label, sizeof(label),
                             "Paste Colors at End (%d)", clip_slots);
                    if (ImGui::MenuItem(label, NULL, false, clip_slots > 0))
                        PastePaletteSlotsAppendToEnd();
                    if (clip_slots > 0 && ImGui::IsItemHovered())
                        ImGui::SetTooltip("Appends them after the selected palette's last color,\n"
                                          "leaving every existing index untouched.");
                    ImGui::Separator();
                }
                if (ImGui::MenuItem("Copy Palette to Clipboard")) CopyPaletteToClipboard();
                if (!g_pal_clipboard.valid) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Paste Palette from Clipboard")) PastePaletteFromClipboard();
                if (g_pal_clipboard.valid && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Paste clipboard palette as new (%s, %d colors)",
                                      g_pal_clipboard.n_s, (int)g_pal_clipboard.numc);
                if (!g_pal_clipboard.valid) ImGui::EndDisabled();
                ImGui::Separator();
                if (ImGui::MenuItem("Export Palette...")) OpenFileDialog(FileDialogMode::ExportPalette);
                ImGui::EndMenu();
            }

            ImGui::Separator();

            {
                int wrong = PaletteBppMismatchCount();
                char label[80];
                snprintf(label, sizeof(label), "Recalculate BPP on Selected Palette");
                if (ImGui::MenuItem(label, NULL, false, g_doc->plselected >= 0))
                    RecalculateSelectedPaletteBpp();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Derive the declared bits-per-pixel from the color count.\n"
                                      "Colors are untouched; TBL/IRW/LOAD2 exports pack at this depth.");
                snprintf(label, sizeof(label),
                         wrong ? "Recalculate BPP on All Palettes (%d wrong)"
                               : "Recalculate BPP on All Palettes (all correct)", wrong);
                if (ImGui::MenuItem(label, NULL, false, wrong > 0))
                    RecalculateAllPaletteBpp();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Fixes palettes imported before the depth was derived\n"
                                      "from the color count — typically stamped 8bpp regardless.");
            }
            ImGui::Separator();

            if (ImGui::MenuItem("Merge Marked into Selected")) MergeMarkedPalettes();
            if (ImGui::MenuItem("Preview Merge...")) OpenPaletteMergePreview();

            ImGui::Separator();

            if (ImGui::BeginMenu("Utilities")) {
                if (ImGui::MenuItem("Clean Up Palette")) CleanupSelectedPalette();
                if (ImGui::MenuItem("Group Like Colors")) GroupLikeColorsSelectedPalette();
                if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                    "Clean up, then cluster similar colors so each hue family\n"
                    "sits together as its own dark-to-light ramp.");
                if (ImGui::MenuItem("Clean Copy Palette")) CreateCleanedPaletteCopy();
                if (ImGui::MenuItem("Merge Duplicate Palettes")) MergeDuplicatePalettes();
                if (ImGui::MenuItem("Inherit Colors from Marked")) InheritSelectedPaletteFromMarked();
                ImGui::Separator();
                if (ImGui::MenuItem("Downscale Palette (Bpp)...")) OpenPaletteReduceDialog(7);
                if (ImGui::MenuItem("Copy #0 to Opaque Slot")) CopyPaletteZeroToOpaqueSlot();
                if (ImGui::MenuItem("Move Selected Colors to End")) MoveSelectedPaletteColorsToEnd();
                ImGui::EndMenu();
            }

            ImGui::EndPopup();
        }
    }

    if (ImGui::CollapsingHeader("Color Tools", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto begin_palette_drag_undo = []() {
            if (!g_palette_drag_undo_active) {
                doc_undo_push();
                g_palette_drag_undo_active = true;
            }
        };
        SDL_Color &col = g_palette[g_sel_color];
        int r = col.r, g = col.g, b = col.b;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("R");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##tool_rgb_r", &r, 0, 255)) {
            begin_palette_drag_undo();
            col.r = (unsigned char)r;
            palette_writeback(g_sel_color);
            commit_palette_adjustments();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("G");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##tool_rgb_g", &g, 0, 255)) {
            begin_palette_drag_undo();
            col.g = (unsigned char)g;
            palette_writeback(g_sel_color);
            commit_palette_adjustments();
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("B");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##tool_rgb_b", &b, 0, 255)) {
            begin_palette_drag_undo();
            col.b = (unsigned char)b;
            palette_writeback(g_sel_color);
            commit_palette_adjustments();
        }
        if (g_palette_drag_undo_active && !ImGui::IsAnyItemActive())
            g_palette_drag_undo_active = false;
        ImGui::Separator();

        PAL *active_pal = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
        bool can_copy_zero = active_pal && active_pal->data_p;
        if (!can_copy_zero) ImGui::BeginDisabled();
        if (g_sel_color == 0) {
            if (ImGui::SmallButton("Copy #0 to Free Slot")) {
                CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None, -1);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Copies the RGB stored at transparent index 0 into\n"
                "the first safe nonzero palette slot and selects it.");
        } else {
            if (ImGui::SmallButton("Copy #0 Here")) {
                CopyPaletteZeroAndRemap(PaletteZeroRemapMode::None, g_sel_color);
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(
                "Copies transparent index 0's RGB into the selected\n"
                "nonzero swatch. Existing pixels using this swatch change color.");
        }
        bool has_selection = g_grid_sel.active;
        if (!has_selection) ImGui::BeginDisabled();
        if (ImGui::SmallButton("Remap Selection")) {
            CopyPaletteZeroAndRemap(PaletteZeroRemapMode::Selection,
                                    g_sel_color > 0 ? g_sel_color : -1);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Copies #0 to an opaque slot, then changes selected\n"
            "pixels with index 0 to that slot.");
        if (!has_selection) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::SmallButton("Remap Sprite")) {
            CopyPaletteZeroAndRemap(PaletteZeroRemapMode::CurrentImage,
                                    g_sel_color > 0 ? g_sel_color : -1);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(
            "Copies #0 to an opaque slot, then changes every index-0\n"
            "pixel in the current sprite to that slot. Transparent padding\n"
            "will become opaque too.");
        if (!can_copy_zero) ImGui::EndDisabled();
        ImGui::Separator();
        bool any_sel = false;
        for (int psi = 0; psi < 256; psi++) if (g_palette_selection[psi]) { any_sel = true; break; }
        if (any_sel) {
            int n_sel = 0;
            for (int psi = 0; psi < 256; psi++) if (g_palette_selection[psi]) n_sel++;
            ImGui::TextDisabled("HSL adjustments target %d selected swatch%s (Ctrl/Shift-click to add).",
                                n_sel, n_sel == 1 ? "" : "es");
        } else {
            ImGui::TextDisabled("HSL adjustments target the whole palette. Ctrl/Shift-click swatches to scope to a subset.");
        }
        ImGui::Text("Hue");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##hue", &g_hue_slider, -180, 180)) {
            begin_palette_drag_undo();
            g_hue_last = g_hue_slider;
            hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
        }
        ImGui::Text("Saturation");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##sat", &g_sat_slider, -100, 100, "%d%%")) {
            begin_palette_drag_undo();
            g_sat_last = g_sat_slider;
            hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("-100 = grayscale, +100 = fully saturated. Makes a yellow more yellow at positive values.");
        ImGui::Text("Lightness");
        ImGui::SetNextItemWidth(-1);
        if (ImGui::SliderInt("##light", &g_light_slider, -100, 100, "%d%%")) {
            begin_palette_drag_undo();
            g_light_last = g_light_slider;
            hsl_adjust_palette_from_baseline(g_hue_slider, g_sat_slider, g_light_slider);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("-100 = black, +100 = white.");
        if (ImGui::SmallButton("Reset HSL")) {
            doc_undo_push();
            reset_palette_adjust_sliders();
            reset_palette_to_baseline();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Reset Hue/Saturation/Lightness sliders to 0 and restore the palette baseline.");
        ImGui::SameLine();
        if (ImGui::SmallButton("New from HSL")) {
            PAL *src = (g_doc->plselected >= 0) ? get_pal(g_doc->plselected) : NULL;
            if (src && src->data_p) {
                doc_undo_push();
                PAL *pal = (PAL *)AllocPal();
                if (pal) {
                    pal->flags   = src->flags;
                    pal->bitspix = src->bitspix;
                    pal->numc    = src->numc;
                    pal->pad     = 0;
                    memcpy(pal->n_s, src->n_s, 10);
                    unsigned int col_sz = (unsigned int)pal->numc * 2;
                    unsigned char *buf = (unsigned char *)PoolAlloc(col_sz);
                    if (buf) {
                        pal->data_p = buf;
                        memcpy(buf, src->data_p, col_sz);
                        if (g_doc->palcnt > 0) g_doc->plselected = (int)g_doc->palcnt - 1;
                        ApplyPalette(g_doc->plselected);
                        commit_palette_adjustments();
                        mark_dirty();
                    }
                }
            }
        }
        if (g_palette_drag_undo_active && !ImGui::IsAnyItemActive())
            g_palette_drag_undo_active = false;
        ImGui::Separator();
        if (ImGui::SmallButton("Variant Selection")) ApplyVariantToSelection();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use the current swatch as the target-palette color,\n"
                              "but keep selected pixels visually unchanged on other palettes.");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remap Similar")) ApplySelectionRemapToMatchingSprites();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Use the current selection as a sample, then remap likely\n"
                              "matching regions in every same-palette sprite to the\n"
                              "current swatch index.");
        if (ImGui::SmallButton("Split Overlay")) SplitSelectionToOverlayFrame(true);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Move selected opaque pixels into a new transparent overlay frame.");
    }
}

void ResetPaletteUiState(void)
{
    g_show_palette_reduce = false;
    g_palette_reduce_preview_idx = -1;
    ClearPaletteReducePreviewTextures();
}



/* =========================================================
   Extracted Selection Propagation from imgui_overlay.cpp
   ========================================================= */
struct SelectionPropagateSample {
    int src_idx;
    int pal_idx;
    int target_idx;
    bool source_colors[256];
    std::vector<std::pair<int,int>> exact_pixels;
    std::vector<std::pair<int,int>> rel_seeds;
    int area;
    int min_x, min_y, max_x, max_y;
    double rel_cx, rel_cy;
};

struct SelectionPropagateMatch {
    int img_idx;
    std::vector<std::pair<int,int>> pixels;
};

struct SelectionComponentStats {
    int area;
    int min_x, min_y, max_x, max_y;
    long long sum_x, sum_y;
};

static bool BuildSelectionPropagateSample(SelectionPropagateSample *sample,
                                          char *err, size_t err_sz)
{
    if (err && err_sz) err[0] = '\0';
    if (!sample) return false;
    sample->src_idx = g_doc->ilselected;
    sample->pal_idx = -1;
    sample->target_idx = g_sel_color;
    memset(sample->source_colors, 0, sizeof(sample->source_colors));
    sample->exact_pixels.clear();
    sample->rel_seeds.clear();
    sample->area = 0;
    sample->min_x = sample->min_y = 0x7FFFFFFF;
    sample->max_x = sample->max_y = -1;
    sample->rel_cx = 0.0;
    sample->rel_cy = 0.0;

    IMG *src = (sample->src_idx >= 0) ? get_img(sample->src_idx) : NULL;
    if (!src || !src->data_p || src->w == 0 || src->h == 0) {
        snprintf(err, err_sz, "Select a source sprite first.");
        return false;
    }
    if (!g_grid_sel.active) {
        snprintf(err, err_sz, "Select the feature first, then propagate it.");
        return false;
    }
    if (g_sel_color <= 0 || g_sel_color >= 256) {
        snprintf(err, err_sz, "Pick a non-transparent destination swatch first.");
        return false;
    }

    sample->pal_idx = (int)src->palnum;
    PAL *pal = get_pal(sample->pal_idx);
    if (!pal || !pal->data_p) {
        snprintf(err, err_sz, "Source sprite has no usable palette.");
        return false;
    }

    int stride = (src->w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)src->data_p;
    long long sum_x = 0;
    long long sum_y = 0;

    for (int y = 0; y < src->h; y++) {
        for (int x = 0; x < src->w; x++) {
            if (!selection_contains_pixel(src, x, y)) continue;
            unsigned char ci = pix[y * stride + x];
            if (ci == 0 || ci == (unsigned char)g_sel_color) continue;
            sample->source_colors[ci] = true;
            sample->exact_pixels.push_back({x, y});
            sum_x += x;
            sum_y += y;
            if (x < sample->min_x) sample->min_x = x;
            if (x > sample->max_x) sample->max_x = x;
            if (y < sample->min_y) sample->min_y = y;
            if (y > sample->max_y) sample->max_y = y;
        }
    }

    sample->area = (int)sample->exact_pixels.size();
    if (sample->area <= 0) {
        snprintf(err, err_sz,
                 "Selection has no source-colored opaque pixels to remap.");
        return false;
    }

    sample->rel_cx = (double)sum_x / (double)sample->area - (double)(short)src->anix;
    sample->rel_cy = (double)sum_y / (double)sample->area - (double)(short)src->aniy;

    int seed_limit = 768;
    int step = sample->area > seed_limit
        ? (sample->area + seed_limit - 1) / seed_limit
        : 1;
    sample->rel_seeds.reserve((size_t)((sample->area + step - 1) / step));
    for (int i = 0; i < sample->area; i += step) {
        int x = sample->exact_pixels[i].first;
        int y = sample->exact_pixels[i].second;
        sample->rel_seeds.push_back({x - (int)(short)src->anix,
                                     y - (int)(short)src->aniy});
    }
    return true;
}

static bool SelectionPropagateColorMatch(const SelectionPropagateSample &sample,
                                         unsigned char ci)
{
    return ci != 0 && ci != (unsigned char)sample.target_idx &&
           sample.source_colors[ci];
}

static bool FindNearestSelectionSeedPixel(IMG *img,
                                          const SelectionPropagateSample &sample,
                                          int cx, int cy, int radius,
                                          int *out_x, int *out_y)
{
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return false;
    int w = img->w;
    int h = img->h;
    int stride = (w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;

    int best_x = -1;
    int best_y = -1;
    int best_d2 = 0x7FFFFFFF;
    int x0 = cx - radius; if (x0 < 0) x0 = 0;
    int y0 = cy - radius; if (y0 < 0) y0 = 0;
    int x1 = cx + radius; if (x1 >= w) x1 = w - 1;
    int y1 = cy + radius; if (y1 >= h) y1 = h - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            if (!SelectionPropagateColorMatch(sample, pix[y * stride + x]))
                continue;
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 < best_d2) {
                best_d2 = d2;
                best_x = x;
                best_y = y;
                if (d2 == 0) {
                    if (out_x) *out_x = best_x;
                    if (out_y) *out_y = best_y;
                    return true;
                }
            }
        }
    }

    if (best_x < 0) return false;
    if (out_x) *out_x = best_x;
    if (out_y) *out_y = best_y;
    return true;
}

static void FloodSelectionPropagateComponent(IMG *img,
                                             const SelectionPropagateSample &sample,
                                             int sx, int sy,
                                             std::vector<unsigned char> &visited,
                                             std::vector<std::pair<int,int>> &out,
                                             SelectionComponentStats *stats)
{
    if (!img || !img->data_p || !stats) return;
    int w = img->w;
    int h = img->h;
    int stride = (w + 3) & ~3;
    const unsigned char *pix = (const unsigned char *)img->data_p;
    if (sx < 0 || sy < 0 || sx >= w || sy >= h) return;
    if (visited[(size_t)sy * w + sx]) return;
    if (!SelectionPropagateColorMatch(sample, pix[sy * stride + sx])) return;

    stats->area = 0;
    stats->min_x = stats->min_y = 0x7FFFFFFF;
    stats->max_x = stats->max_y = -1;
    stats->sum_x = stats->sum_y = 0;

    std::vector<std::pair<int,int>> stack;
    stack.push_back({sx, sy});
    visited[(size_t)sy * w + sx] = 1;

    while (!stack.empty()) {
        std::pair<int,int> pt = stack.back();
        stack.pop_back();
        int x = pt.first;
        int y = pt.second;

        out.push_back(pt);
        stats->area++;
        stats->sum_x += x;
        stats->sum_y += y;
        if (x < stats->min_x) stats->min_x = x;
        if (x > stats->max_x) stats->max_x = x;
        if (y < stats->min_y) stats->min_y = y;
        if (y > stats->max_y) stats->max_y = y;

        const int dx[4] = {0, 1, 0, -1};
        const int dy[4] = {-1, 0, 1, 0};
        for (int i = 0; i < 4; i++) {
            int nx = x + dx[i];
            int ny = y + dy[i];
            if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
            size_t off = (size_t)ny * w + nx;
            if (visited[off]) continue;
            if (!SelectionPropagateColorMatch(sample, pix[ny * stride + nx]))
                continue;
            visited[off] = 1;
            stack.push_back({nx, ny});
        }
    }
}

static bool SelectionPropagateComponentLooksLikely(IMG *img,
                                                   const SelectionPropagateSample &sample,
                                                   const SelectionComponentStats &stats)
{
    if (!img || stats.area <= 0 || sample.area <= 0) return false;
    double ratio = (double)stats.area / (double)sample.area;
    if (ratio < 0.04 || ratio > 8.0) return false;

    double cx = (double)stats.sum_x / (double)stats.area - (double)(short)img->anix;
    double cy = (double)stats.sum_y / (double)stats.area - (double)(short)img->aniy;
    double dx = cx - sample.rel_cx;
    double dy = cy - sample.rel_cy;
    double dist = sqrt(dx * dx + dy * dy);

    int sample_w = sample.max_x - sample.min_x + 1;
    int sample_h = sample.max_y - sample.min_y + 1;
    if (sample_w < 1) sample_w = 1;
    if (sample_h < 1) sample_h = 1;
    double reach = (double)(sample_w > sample_h ? sample_w : sample_h) * 2.5 + 12.0;
    if (reach < 28.0) reach = 28.0;
    return dist <= reach;
}

static std::vector<std::pair<int,int>>
FindSelectionPropagationPixels(IMG *img, const SelectionPropagateSample &sample,
                               int img_idx)
{
    std::vector<std::pair<int,int>> result;
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return result;

    if (img_idx == sample.src_idx) {
        result = sample.exact_pixels;
        return result;
    }

    int sample_w = sample.max_x - sample.min_x + 1;
    int sample_h = sample.max_y - sample.min_y + 1;
    int radius = (sample_w > sample_h ? sample_w : sample_h) / 2;
    if (radius < 6) radius = 6;
    if (radius > 18) radius = 18;

    int w = img->w;
    int h = img->h;
    std::vector<unsigned char> visited((size_t)w * h, 0);
    for (const auto &seed : sample.rel_seeds) {
        int ex = (int)(short)img->anix + seed.first;
        int ey = (int)(short)img->aniy + seed.second;
        int sx = 0, sy = 0;
        if (!FindNearestSelectionSeedPixel(img, sample, ex, ey, radius, &sx, &sy))
            continue;
        if (visited[(size_t)sy * w + sx])
            continue;

        std::vector<std::pair<int,int>> component;
        SelectionComponentStats stats = {};
        FloodSelectionPropagateComponent(img, sample, sx, sy,
                                         visited, component, &stats);
        if (component.empty())
            continue;
        if (!SelectionPropagateComponentLooksLikely(img, sample, stats))
            continue;
        result.insert(result.end(), component.begin(), component.end());
    }
    return result;
}

void ApplySelectionRemapToMatchingSprites(void)
{
    SelectionPropagateSample sample = {};
    char err[160];
    if (!BuildSelectionPropagateSample(&sample, err, sizeof(err))) {
        snprintf(g_restore_msg, sizeof(g_restore_msg), "%s", err);
        g_restore_msg_timer = 4.0f;
        return;
    }

    std::vector<SelectionPropagateMatch> matches;
    int scanned = 0;
    int idx = 0;
    for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
        if ((int)img->palnum != sample.pal_idx ||
            !img->data_p || img->w == 0 || img->h == 0)
            continue;
        scanned++;
        std::vector<std::pair<int,int>> pts =
            FindSelectionPropagationPixels(img, sample, idx);
        if (!pts.empty())
            matches.push_back({idx, std::move(pts)});
    }

    if (matches.empty()) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "No matching regions found in %d same-palette sprite%s.",
                 scanned, scanned == 1 ? "" : "s");
        g_restore_msg_timer = 4.0f;
        return;
    }

    if (!doc_undo_push()) return;

    PAL *pal = get_pal(sample.pal_idx);
    if (!ensure_palette_numc(pal, sample.target_idx + 1)) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Could not extend palette to index %d.", sample.target_idx);
        g_restore_msg_timer = 4.0f;
        return;
    }

    int changed_pixels = 0;
    int changed_images = 0;
    for (SelectionPropagateMatch &m : matches) {
        IMG *img = get_img(m.img_idx);
        if (!img || !img->data_p) continue;
        int stride = (img->w + 3) & ~3;
        unsigned char *pix = (unsigned char *)img->data_p;
        int image_changed = 0;
        for (const auto &pt : m.pixels) {
            int x = pt.first;
            int y = pt.second;
            if (x < 0 || y < 0 || x >= (int)img->w || y >= (int)img->h)
                continue;
            unsigned char *p = pix + y * stride + x;
            if (!SelectionPropagateColorMatch(sample, *p))
                continue;
            *p = (unsigned char)sample.target_idx;
            image_changed++;
        }
        if (image_changed > 0) {
            changed_pixels += image_changed;
            changed_images++;
            InvalidateThumb(m.img_idx);
        }
    }

    if (changed_pixels > 0) {
        ApplyPalette(sample.pal_idx);
        save_palette_baseline();
        g_img_tex_idx = -2;
        mark_dirty();
    }

    if (changed_pixels > 0) {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Remapped %d likely-matching pixel%s in %d/%d sprite%s to #%d.",
                 changed_pixels, changed_pixels == 1 ? "" : "s",
                 changed_images, scanned, scanned == 1 ? "" : "s",
                 sample.target_idx);
    } else {
        snprintf(g_restore_msg, sizeof(g_restore_msg),
                 "Matching regions were already using #%d.", sample.target_idx);
    }
    g_restore_msg_timer = 5.0f;
}

