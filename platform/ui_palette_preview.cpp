/*************************************************************
 * platform/ui_palette_preview.cpp
 * Swatch strips and before/after sprite panes for the palette-rework
 * dialogs. See ui_palette_preview.h.
 *************************************************************/
#include <imgui.h>

#include "ui_palette_preview.h"
#include "ui_internal.h"      /* g_imgui_renderer */
#include "img_format.h"

static const float kSwatch = 11.0f;
static const int kMaxShown = 24;

static ImU32 WordColor(unsigned short w)
{
    const int r5 = (w >> 10) & 0x1F, g5 = (w >> 5) & 0x1F, b5 = w & 0x1F;
    return IM_COL32((r5 << 3) | (r5 >> 2), (g5 << 3) | (g5 >> 2), (b5 << 3) | (b5 >> 2), 255);
}

std::vector<unsigned short> PalettePreviewWords(const PAL *pal)
{
    std::vector<unsigned short> words;
    if (!pal || !pal->data_p || pal->numc <= 0) return words;
    const int n = pal->numc > 256 ? 256 : pal->numc;
    words.resize(n);
    const unsigned char *pb = (const unsigned char *)pal->data_p;
    for (int i = 0; i < n; i++)
        words[i] = (unsigned short)(pb[i*2] | (pb[i*2+1] << 8));
    return words;
}

void PalettePreviewStrip(const std::vector<unsigned short> &words, int start, int count,
                         const unsigned char *via_map, const std::vector<unsigned short> *map_words)
{
    const int shown = count > kMaxShown ? kMaxShown : count;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (int k = 0; k < shown; k++) {
        const int slot = start + k;
        unsigned short w = 0;
        if (via_map && map_words) {
            const int m = (slot >= 0 && slot < 256) ? via_map[slot] : 0;
            if (m < (int)map_words->size()) w = (*map_words)[m];
        } else if (slot >= 0 && slot < (int)words.size()) {
            w = words[slot];
        }
        ImVec2 a(o.x + k * kSwatch, o.y), b(a.x + kSwatch - 1.0f, a.y + kSwatch + 3.0f);
        dl->AddRectFilled(a, b, WordColor(w));
    }
    ImGui::Dummy(ImVec2(kMaxShown * kSwatch, kSwatch + 3.0f));
    if (count > kMaxShown && ImGui::IsItemHovered())
        ImGui::SetTooltip("%d more slot(s) not shown", count - kMaxShown);
}

bool PalettePickerGrid(const char *id, const std::vector<unsigned short> &words,
                       int *first, int *last)
{
    const int n = (int)words.size();
    if (n <= 0 || !first || !last) return false;
    const float cell = 14.0f;
    const int cols = 16, rows = (n + cols - 1) / cols;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(id, ImVec2(cols * cell, rows * cell),
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    int hover = -1;
    if (ImGui::IsItemHovered()) {
        const ImVec2 m = ImGui::GetIO().MousePos;
        const int cx = (int)((m.x - o.x) / cell), cy = (int)((m.y - o.y) / cell);
        if (cx >= 0 && cx < cols && cy >= 0 && cy < rows && cy * cols + cx < n) hover = cy * cols + cx;
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    for (int s = 0; s < n; s++) {
        const ImVec2 a(o.x + (s % cols) * cell, o.y + (s / cols) * cell);
        const ImVec2 b(a.x + cell - 1.0f, a.y + cell - 1.0f);
        dl->AddRectFilled(a, b, WordColor(words[s]));
        if (s < *first || s > *last) dl->AddRectFilled(a, b, IM_COL32(20, 20, 24, 170));
        if (s == *first || s == *last) dl->AddRect(a, b, IM_COL32(255, 255, 255, 255));
        if (s == hover) dl->AddRect(ImVec2(a.x - 1, a.y - 1), ImVec2(b.x + 1, b.y + 1),
                                    IM_COL32(255, 220, 60, 255), 0.0f, 0, 2.0f);
    }

    if (hover < 0) return false;
    const unsigned short w = words[hover];
    ImGui::SetTooltip("Slot %d  (%d,%d,%d)%s\nClick: start here   Right-click: end here", hover,
                      (w >> 10) & 0x1F, (w >> 5) & 0x1F, w & 0x1F,
                      hover == *first ? "  first" : hover == *last ? "  last" : "");
    if (hover < 1) return false;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && hover != *first) {
        *first = hover;
        if (*last < hover) *last = hover;
        return true;
    }
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && hover != *last) {
        *last = hover;
        if (*first > hover) *first = hover;
        return true;
    }
    return false;
}

static SDL_Texture *g_tex[4] = {};
static int g_tex_w[4] = {}, g_tex_h[4] = {};

static SDL_Texture *SpriteTexture(int slot, const IMG *img, const std::vector<unsigned short> &words,
                                  const unsigned char *map)
{
    const int w = img->w, h = img->h;
    if (w <= 0 || h <= 0 || !img->data_p) return nullptr;
    if (!g_tex[slot] || g_tex_w[slot] != w || g_tex_h[slot] != h) {
        if (g_tex[slot]) SDL_DestroyTexture(g_tex[slot]);
        g_tex[slot] = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                                        SDL_TEXTUREACCESS_STREAMING, w, h);
        g_tex_w[slot] = w; g_tex_h[slot] = h;
        if (!g_tex[slot]) return nullptr;
        SDL_SetTextureBlendMode(g_tex[slot], SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(g_tex[slot], SDL_ScaleModeNearest);
    }
    void *pixels; int pitch;
    if (SDL_LockTexture(g_tex[slot], NULL, &pixels, &pitch) != 0) return g_tex[slot];
    const int stride = (w + 3) & ~3;
    const unsigned char *src = (const unsigned char *)img->data_p;
    for (int y = 0; y < h; y++) {
        Uint32 *row = (Uint32 *)((unsigned char *)pixels + (size_t)y * pitch);
        for (int x = 0; x < w; x++) {
            int ci = src[y * stride + x];
            if (map) ci = map[ci];
            if (ci == 0 || ci >= (int)words.size()) { row[x] = 0; continue; }
            const unsigned short cw = words[ci];
            const int r5 = (cw >> 10) & 0x1F, g5 = (cw >> 5) & 0x1F, b5 = cw & 0x1F;
            row[x] = 0xFF000000u | ((Uint32)((r5 << 3) | (r5 >> 2)) << 16) |
                     ((Uint32)((g5 << 3) | (g5 >> 2)) << 8) | (Uint32)((b5 << 3) | (b5 >> 2));
        }
    }
    SDL_UnlockTexture(g_tex[slot]);
    return g_tex[slot];
}

void PalettePreviewSprite(int slot, const char *label, const IMG *img,
                          const std::vector<unsigned short> &words,
                          const unsigned char *map, float pane_w, float pane_h)
{
    if (slot < 0 || slot > 3 || !img) return;
    ImGui::BeginGroup();
    ImGui::TextUnformatted(label);
    const int w = img->w > 0 ? img->w : 1, h = img->h > 0 ? img->h : 1;
    float scale = pane_w / (float)w;
    if (pane_h / (float)h < scale) scale = pane_h / (float)h;
    if (scale > 4.0f) scale = 4.0f;
    ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(o, ImVec2(o.x + pane_w, o.y + pane_h), IM_COL32(24, 24, 28, 255));
    ImVec2 a(o.x + (pane_w - w * scale) * 0.5f, o.y + (pane_h - h * scale) * 0.5f);
    SDL_Texture *tex = SpriteTexture(slot, img, words, map);
    if (tex) dl->AddImage((ImTextureID)(intptr_t)tex, a, ImVec2(a.x + w * scale, a.y + h * scale));
    ImGui::Dummy(ImVec2(pane_w, pane_h));
    ImGui::EndGroup();
}
