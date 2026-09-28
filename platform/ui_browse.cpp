/*************************************************************
 * platform/ui_browse.cpp
 * See ui_browse.h. A toolbar row (cell size, name labels, marked-only, name
 * filter) above a clipped grid of sprite thumbnails.
 *************************************************************/
#include "ui_browse.h"

#include "ui_internal.h"   /* g_doc, g_imgui_renderer, g_zoom_reset, g_browse_workspace */
#include "img_format.h"    /* IMG, PAL, get_pal */
#include "img_util.h"      /* img_name_string */

#include <SDL.h>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* ---- Thumbnail cache ----
   Separate from the timeline's 48 px cache: the grid wants bigger cells, and
   bakes each thumb at the cell size it is drawn at (never above the sprite's
   own size -- small sprites are upscaled by the renderer with nearest). */
struct BrowseThumb {
    SDL_Texture *tex = nullptr;
    int w = 0, h = 0;               /* texture dims */
    int cell = 0;                   /* cell size it was baked for */
    int src_w = 0, src_h = 0;
    const void *src_data = nullptr;
    int last_used = 0;              /* ImGui frame count */
};

static std::vector<BrowseThumb> s_thumbs;
static const Document *s_thumbs_doc = nullptr;
static int s_thumb_live = 0;

/* Past this many live textures, anything not drawn in the last couple of
   frames is released. Keeps a scroll through thousands of frames bounded. */
static const int kThumbBudget = 600;
/* Bakes per frame. A fresh page of a big grid fills in over a few frames
   instead of stalling one. */
static const int kBakesPerFrame = 48;

static void destroy_thumb(BrowseThumb &t)
{
    if (t.tex) { SDL_DestroyTexture(t.tex); t.tex = nullptr; s_thumb_live--; }
}

void BrowseInvalidateThumb(int idx)
{
    if (idx < 0 || (size_t)idx >= s_thumbs.size()) return;
    destroy_thumb(s_thumbs[idx]);
}

void BrowseClearThumbCache(void)
{
    for (auto &t : s_thumbs) destroy_thumb(t);
    s_thumbs.clear();
    s_thumb_live = 0;
}

/* Box-filtered bake: each thumb pixel averages the opaque source pixels it
   covers, and its alpha is their coverage. Nearest sampling drops thin
   outlines and single-pixel details, which is exactly what you are scanning
   for. */
static bool bake_thumb(BrowseThumb &t, IMG *img, int cell)
{
    destroy_thumb(t);
    int sw = (int)img->w, sh = (int)img->h;
    int tw = sw, th = sh;
    if (tw > cell || th > cell) {
        if (sw >= sh) { tw = cell; th = (int)((long long)sh * cell / sw); }
        else          { th = cell; tw = (int)((long long)sw * cell / sh); }
        if (tw < 1) tw = 1;
        if (th < 1) th = 1;
    }
    t.tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STATIC, tw, th);
    if (!t.tex) return false;
    s_thumb_live++;
    SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t.tex, SDL_ScaleModeNearest);

    Uint32 lut[256];
    PAL *pal = get_pal(img->palnum);
    const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : nullptr;
    int numc = pal ? (int)pal->numc : 0;
    for (int i = 0; i < 256; i++) {
        Uint32 r = 200, g = 200, b = 200;
        if (pd && i < numc) {
            unsigned short w15 = (unsigned short)(pd[i * 2] | (pd[i * 2 + 1] << 8));
            r = ((w15 >> 10) & 0x1F) << 3;
            g = ((w15 >>  5) & 0x1F) << 3;
            b = ( w15        & 0x1F) << 3;
        }
        lut[i] = (r << 16) | (g << 8) | b;
    }

    std::vector<Uint32> out((size_t)tw * th);
    const unsigned char *sp = (const unsigned char *)img->data_p;
    int stride = (sw + 3) & ~3;
    for (int y = 0; y < th; y++) {
        int y0 = (int)((long long)y * sh / th);
        int y1 = (int)((long long)(y + 1) * sh / th);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < tw; x++) {
            int x0 = (int)((long long)x * sw / tw);
            int x1 = (int)((long long)(x + 1) * sw / tw);
            if (x1 <= x0) x1 = x0 + 1;
            unsigned rs = 0, gs = 0, bs = 0, n = 0, total = 0;
            for (int yy = y0; yy < y1; yy++) {
                const unsigned char *row = sp + (size_t)yy * stride;
                for (int xx = x0; xx < x1; xx++) {
                    total++;
                    unsigned char ci = row[xx];
                    if (!ci) continue;
                    Uint32 c = lut[ci];
                    rs += (c >> 16) & 0xFF; gs += (c >> 8) & 0xFF; bs += c & 0xFF;
                    n++;
                }
            }
            Uint32 px = 0;
            if (n) {
                Uint32 a = (Uint32)(255u * n / total);
                px = (a << 24) | ((rs / n) << 16) | ((gs / n) << 8) | (bs / n);
            }
            out[(size_t)y * tw + x] = px;
        }
    }
    SDL_UpdateTexture(t.tex, nullptr, out.data(), tw * 4);
    t.w = tw; t.h = th;
    t.cell = cell;
    t.src_w = sw; t.src_h = sh;
    t.src_data = img->data_p;
    return true;
}

/* Returns the cached thumb, baking it when stale and the per-frame budget
   allows; NULL means "not ready yet" (drawn as a placeholder). */
static BrowseThumb *get_thumb(int idx, IMG *img, int cell, int *bakes_left)
{
    if (!img->data_p || img->w == 0 || img->h == 0) return nullptr;
    if ((size_t)idx >= s_thumbs.size()) s_thumbs.resize(idx + 1);
    BrowseThumb &t = s_thumbs[idx];
    t.last_used = ImGui::GetFrameCount();
    /* A thumb baked for a smaller cell looks soft when the cell grows; one
       baked larger is still sharp, so only rebake on growth. */
    bool fresh = t.tex && t.src_w == (int)img->w && t.src_h == (int)img->h &&
                 t.src_data == img->data_p &&
                 (t.cell >= cell || (t.w == t.src_w && t.h == t.src_h));
    if (fresh) return &t;
    if (*bakes_left <= 0) return t.tex ? &t : nullptr;
    (*bakes_left)--;
    return bake_thumb(t, img, cell) ? &t : nullptr;
}

static void evict_stale_thumbs(void)
{
    if (s_thumb_live <= kThumbBudget) return;
    int now = ImGui::GetFrameCount();
    for (auto &t : s_thumbs)
        if (t.tex && now - t.last_used > 2) destroy_thumb(t);
}

static bool name_matches(const std::string &name, const char *filter)
{
    if (!filter[0]) return true;
    size_t fl = strlen(filter);
    if (fl > name.size()) return false;
    for (size_t i = 0; i + fl <= name.size(); i++) {
        size_t k = 0;
        while (k < fl && tolower((unsigned char)name[i + k]) ==
                         tolower((unsigned char)filter[k])) k++;
        if (k == fl) return true;
    }
    return false;
}

void DrawBrowseWorkspace(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io)
{
    (void)avail; (void)img_pos;
    static int  s_cell = 96;
    static bool s_names = true;
    static bool s_marked_only = false;
    static char s_filter[64] = "";
    static int  s_last_sel = -2;

    if (s_thumbs_doc != g_doc) {
        BrowseClearThumbCache();
        s_thumbs_doc = g_doc;
        s_last_sel = -2;
    }

    /* Walk the linked list once: get_img(idx) is itself a list walk, and
       calling it per cell turns a big library quadratic. */
    struct Cell { int idx; IMG *img; };
    std::vector<Cell> cells;
    int total = 0, marked = 0;
    {
        int idx = 0;
        for (IMG *img = (IMG *)g_doc->img_p; img; img = (IMG *)img->nxt_p, idx++) {
            total++;
            if (img->flags & 1) marked++;
            if (s_marked_only && !(img->flags & 1)) continue;
            if (s_filter[0] && !name_matches(img_name_string(img), s_filter)) continue;
            cells.push_back({idx, img});
        }
    }
    if ((int)s_thumbs.size() > total) {
        for (size_t i = total; i < s_thumbs.size(); i++) destroy_thumb(s_thumbs[i]);
        s_thumbs.resize(total);
    }

    /* ---- Toolbar ---- */
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Size");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(140.0f);
    ImGui::SliderInt("##browse_cell", &s_cell, 32, 256, "%d px");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Thumbnail size. Ctrl+wheel over the grid also resizes.");
    ImGui::SameLine();
    ImGui::Checkbox("Names", &s_names);
    ImGui::SameLine();
    ImGui::Checkbox("Marked only", &s_marked_only);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputTextWithHint("##browse_filter", "Filter by name", s_filter, sizeof(s_filter));
    ImGui::SameLine();
    if (cells.size() == (size_t)total)
        ImGui::TextDisabled("%d sprites, %d marked", total, marked);
    else
        ImGui::TextDisabled("%d of %d sprites, %d marked", (int)cells.size(), total, marked);

    /* ---- Grid ---- */
    const float gap = 6.0f;
    const float label_h = s_names ? ImGui::GetTextLineHeight() + 2.0f : 0.0f;
    ImGuiWindowFlags child_flags = io.KeyCtrl ? ImGuiWindowFlags_NoScrollWithMouse : 0;
    ImGui::BeginChild("##browse_grid", ImVec2(0, 0), ImGuiChildFlags_None, child_flags);

    if (io.KeyCtrl && io.MouseWheel != 0.0f &&
        ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) {
        s_cell += io.MouseWheel > 0 ? 16 : -16;
        if (s_cell < 32) s_cell = 32;
        if (s_cell > 256) s_cell = 256;
    }

    float cell = (float)s_cell;
    float row_h = cell + label_h + gap;
    float width = ImGui::GetContentRegionAvail().x;
    int cols = (int)((width + gap) / (cell + gap));
    if (cols < 1) cols = 1;
    int rows = ((int)cells.size() + cols - 1) / cols;

    /* Follow the selection when it moves from outside the grid (Up/Down, the
       sprite list, another view) so the highlighted cell is always on screen. */
    int sel = g_doc->ilselected;
    if (sel != s_last_sel) {
        for (size_t i = 0; i < cells.size(); i++) {
            if (cells[i].idx != sel) continue;
            float y = (float)(i / cols) * row_h;
            float top = ImGui::GetScrollY();
            float view_h = ImGui::GetWindowHeight();
            if (y < top || y + row_h > top + view_h)
                ImGui::SetScrollY(y - (view_h - row_h) * 0.5f);
            break;
        }
        s_last_sel = sel;
    }

    if (cells.empty()) {
        ImGui::TextDisabled(total ? "No sprites match." : "No sprites in this IMG.");
    }

    ImDrawList *dl = ImGui::GetWindowDrawList();
    int bakes_left = kBakesPerFrame;
    /* Zero item spacing so each row advances by exactly row_h, which is what
       the clipper was told. */
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    ImGuiListClipper clipper;
    clipper.Begin(rows, row_h);
    while (clipper.Step()) {
        for (int r = clipper.DisplayStart; r < clipper.DisplayEnd; r++) {
            ImVec2 row_pos = ImGui::GetCursorScreenPos();
            for (int c = 0; c < cols; c++) {
                size_t ci = (size_t)r * cols + c;
                if (ci >= cells.size()) break;
                int idx = cells[ci].idx;
                IMG *img = cells[ci].img;
                ImVec2 p0(row_pos.x + c * (cell + gap), row_pos.y);
                ImVec2 p1(p0.x + cell, p0.y + cell);

                ImGui::SetCursorScreenPos(p0);
                ImGui::PushID(idx);
                ImGui::InvisibleButton("##cell", ImVec2(cell, cell + label_h));
                bool hovered = ImGui::IsItemHovered();
                if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                    if (io.KeyCtrl) {
                        img->flags ^= 1;
                    } else {
                        g_doc->ilselected = idx;
                        g_zoom_reset = true;
                        s_last_sel = idx;   /* clicked in view: don't re-center */
                    }
                }
                if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    !io.KeyCtrl) {
                    g_doc->ilselected = idx;
                    g_zoom_reset = true;
                    g_browse_workspace = false;   /* open on the Image tab */
                }
                ImGui::PopID();

                bool is_sel = idx == sel;
                bool is_marked = (img->flags & 1) != 0;
                dl->AddRectFilled(p0, p1, hovered ? IM_COL32(0x2A, 0x2A, 0x30, 0xFF)
                                                  : IM_COL32(0x1A, 0x1A, 0x1E, 0xFF));

                BrowseThumb *t = get_thumb(idx, img, s_cell, &bakes_left);
                if (t && t->tex) {
                    /* Fit the sprite's own aspect into the cell. */
                    float scale = cell / (float)(img->w > img->h ? img->w : img->h);
                    float dw = img->w * scale, dh = img->h * scale;
                    ImVec2 q0(p0.x + (cell - dw) * 0.5f, p0.y + (cell - dh) * 0.5f);
                    dl->AddImage((ImTextureID)(intptr_t)t->tex, q0,
                                 ImVec2(q0.x + dw, q0.y + dh));
                } else if (img->data_p && img->w && img->h) {
                    ImVec2 ts = ImGui::CalcTextSize("...");
                    dl->AddText(ImVec2(p0.x + (cell - ts.x) * 0.5f, p0.y + (cell - ts.y) * 0.5f),
                                IM_COL32(120, 120, 120, 255), "...");
                }

                char idx_label[16];
                snprintf(idx_label, sizeof(idx_label), "%d", idx);
                dl->AddText(ImVec2(p0.x + 3, p0.y + 1), IM_COL32(255, 255, 255, 150), idx_label);

                if (is_marked) {
                    ImVec2 m0(p1.x - 10, p0.y + 3), m1(p1.x - 3, p0.y + 10);
                    dl->AddRectFilled(m0, m1, IM_COL32(255, 210, 60, 255), 1.0f);
                }
                if (is_sel)
                    dl->AddRect(ImVec2(p0.x - 1, p0.y - 1), ImVec2(p1.x + 1, p1.y + 1),
                                IM_COL32(90, 170, 255, 255), 0.0f, 0, 2.5f);
                else if (is_marked)
                    dl->AddRect(p0, p1, IM_COL32(255, 210, 60, 160));
                else if (hovered)
                    dl->AddRect(p0, p1, IM_COL32(160, 160, 170, 200));

                if (s_names) {
                    std::string name = img_name_string(img);
                    ImVec2 l0(p0.x, p1.y + 1);
                    dl->PushClipRect(l0, ImVec2(p1.x, l0.y + label_h), true);
                    dl->AddText(l0, is_sel ? IM_COL32(140, 200, 255, 255)
                                           : IM_COL32(200, 200, 200, 255), name.c_str());
                    dl->PopClipRect();
                }

                if (hovered) {
                    PAL *pal = get_pal(img->palnum);
                    char pal_name[12] = "";
                    if (pal) snprintf(pal_name, sizeof(pal_name), "%.10s", pal->n_s);
                    ImGui::SetTooltip("[%d] %s\n%d x %d   anipoint %d,%d\npalette %s%s\n\n"
                                      "Click: select   Ctrl-click: mark\n"
                                      "Double-click: open on the Image tab",
                                      idx, img_name_string(img).c_str(),
                                      (int)img->w, (int)img->h,
                                      (int)(short)img->anix, (int)(short)img->aniy,
                                      pal ? pal_name : "(none)",
                                      is_marked ? "\nmarked" : "");
                }
            }
            ImGui::SetCursorScreenPos(row_pos);
            ImGui::Dummy(ImVec2(width, row_h));
        }
    }
    clipper.End();
    ImGui::PopStyleVar();
    ImGui::EndChild();

    evict_stale_thumbs();
}
