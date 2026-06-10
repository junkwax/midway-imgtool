/*************************************************************
 * platform/ui_timeline.cpp
 * Timeline frame-model state and operations declared in ui_timeline.h.
 *************************************************************/
#include "ui_timeline.h"
#include "img_format.h"     /* IMG, PAL, get_img, get_pal */
#include "document.h"       /* g_doc */
#include "ui_internal.h"    /* g_imgui_renderer, g_icon_font_loaded, ICON_*, ZOOM_MAX */
#include "world_render.h"   /* BuildWorldSpriteTexture */
#include "anipoint.h"       /* secondary_anipoint_in_use */
#include "anipoint_edit.h"  /* set_primary_anipoint_with_sequence */
#include "img_util.h"       /* img_name_string */

#include <imgui.h>     /* DrawTimelineComposite* */
#include <algorithm>   /* std::swap */
#include <cstdio>      /* snprintf */
#include <cmath>       /* floorf */
#include <string>

std::vector<int> g_timeline_frames;
std::vector<int> g_timeline_holds;   /* base-frame holds per timeline entry */
int              g_timeline_play_idx = 0;

int TimelineFramePosition(int img_idx)
{
    for (size_t i = 0; i < g_timeline_frames.size(); i++) {
        if (g_timeline_frames[i] == img_idx)
            return (int)i;
    }
    return -1;
}

int WrapTimelinePosition(int pos)
{
    int n = (int)g_timeline_frames.size();
    if (n <= 0) return 0;
    pos %= n;
    if (pos < 0) pos += n;
    return pos;
}

int ClampTimelineHold(int hold)
{
    if (hold < 1) return 1;
    if (hold > 120) return 120;
    return hold;
}

void EnsureTimelineHolds(void)
{
    if (g_timeline_holds.size() < g_timeline_frames.size())
        g_timeline_holds.resize(g_timeline_frames.size(), 1);
    else if (g_timeline_holds.size() > g_timeline_frames.size())
        g_timeline_holds.resize(g_timeline_frames.size());
    for (int &hold : g_timeline_holds)
        hold = ClampTimelineHold(hold);
}

void TimelinePushFrame(int img_idx, int hold)
{
    g_timeline_frames.push_back(img_idx);
    g_timeline_holds.push_back(ClampTimelineHold(hold));
}

void TimelineSetFrames(const std::vector<int> &frames)
{
    g_timeline_frames = frames;
    g_timeline_holds.assign(g_timeline_frames.size(), 1);
}

void TimelineClearFrames(void)
{
    g_timeline_frames.clear();
    g_timeline_holds.clear();
    g_timeline_play_idx = 0;
}

int TimelineHoldAt(int pos)
{
    EnsureTimelineHolds();
    if (pos < 0 || pos >= (int)g_timeline_holds.size()) return 1;
    return ClampTimelineHold(g_timeline_holds[pos]);
}

void TimelineSetHoldAt(int pos, int hold)
{
    EnsureTimelineHolds();
    if (pos < 0 || pos >= (int)g_timeline_holds.size()) return;
    g_timeline_holds[pos] = ClampTimelineHold(hold);
}

void TimelineSwapFrames(int a, int b)
{
    EnsureTimelineHolds();
    if (a < 0 || b < 0 ||
        a >= (int)g_timeline_frames.size() ||
        b >= (int)g_timeline_frames.size())
        return;
    std::swap(g_timeline_frames[a], g_timeline_frames[b]);
    std::swap(g_timeline_holds[a], g_timeline_holds[b]);
}

void TimelineMoveFrame(int src_idx, int dst_idx)
{
    EnsureTimelineHolds();
    int n = (int)g_timeline_frames.size();
    if (src_idx < 0 || src_idx >= n || dst_idx < 0 || dst_idx >= n || src_idx == dst_idx)
        return;

    int val = g_timeline_frames[src_idx];
    int hold = g_timeline_holds[src_idx];
    g_timeline_frames.erase(g_timeline_frames.begin() + src_idx);
    g_timeline_holds.erase(g_timeline_holds.begin() + src_idx);
    g_timeline_frames.insert(g_timeline_frames.begin() + dst_idx, val);
    g_timeline_holds.insert(g_timeline_holds.begin() + dst_idx, hold);
    if (g_timeline_play_idx == src_idx) g_timeline_play_idx = dst_idx;
    else if (src_idx < g_timeline_play_idx && dst_idx >= g_timeline_play_idx) g_timeline_play_idx--;
    else if (src_idx > g_timeline_play_idx && dst_idx <= g_timeline_play_idx) g_timeline_play_idx++;
}

/* ---- Timeline thumbnail cache ---- */
std::vector<TimelineThumb> g_thumb_cache;

TimelineThumb *EnsureThumb(int idx)
{
    if (idx < 0 || (unsigned int)idx >= g_doc->imgcnt) return NULL;
    if (g_thumb_cache.size() < g_doc->imgcnt) g_thumb_cache.resize(g_doc->imgcnt, {NULL,0,0,0,0,-1});
    IMG *img = get_img(idx);
    if (!img || !img->data_p || img->w == 0 || img->h == 0) return NULL;
    TimelineThumb &t = g_thumb_cache[idx];
    /* Reuse if source dims and pixels haven't changed since bake (cheap proxy:
       same src dims, same gen). */
    if (t.tex && t.src_w == (int)img->w && t.src_h == (int)img->h) return &t;

    const int MAX = 48;
    float aspect = (float)img->w / (float)img->h;
    int tw, th;
    if (aspect >= 1.0f) { tw = MAX; th = (int)(MAX / aspect); if (th < 1) th = 1; }
    else                { th = MAX; tw = (int)(MAX * aspect); if (tw < 1) tw = 1; }

    if (t.tex) { SDL_DestroyTexture(t.tex); t.tex = NULL; }
    t.tex = SDL_CreateTexture(g_imgui_renderer, SDL_PIXELFORMAT_ARGB8888,
                              SDL_TEXTUREACCESS_STREAMING, tw, th);
    if (!t.tex) return NULL;
    SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t.tex, SDL_ScaleModeNearest);

    void *pixels; int pitch;
    if (SDL_LockTexture(t.tex, NULL, &pixels, &pitch) != 0) {
        SDL_DestroyTexture(t.tex); t.tex = NULL; return NULL;
    }
    Uint32 *dst = (Uint32 *)pixels;
    int src_stride = (img->w + 3) & ~3;
    const unsigned char *sp = (const unsigned char *)img->data_p;
    PAL *pal = get_pal(img->palnum);
    const unsigned char *pd = pal ? (const unsigned char *)pal->data_p : NULL;
    for (int y = 0; y < th; y++) {
        int sy = (int)((float)y * img->h / th);
        for (int x = 0; x < tw; x++) {
            int sx_i = (int)((float)x * img->w / tw);
            unsigned char ci = sp[sy * src_stride + sx_i];
            Uint32 r=180, g=180, b=180, a=255;
            if (ci == 0) {
                /* Transparent — leaves the timeline strip / onion-skin host
                   background showing through. Required for the onion-skin
                   path which composites the thumb over the canvas. */
                a = 0; r = g = b = 0;
            } else if (pd) {
                unsigned short w15 = (unsigned short)(pd[ci*2] | (pd[ci*2+1] << 8));
                r = (((w15 >> 10) & 0x1F) << 3);
                g = (((w15 >>  5) & 0x1F) << 3);
                b = (( w15        & 0x1F) << 3);
            } else {
                r = g = b = 200;
            }
            dst[y * (pitch / 4) + x] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }
    SDL_UnlockTexture(t.tex);
    t.w = tw; t.h = th;
    t.src_w = img->w; t.src_h = img->h;
    return &t;
}

void InvalidateThumb(int idx)
{
    if (idx < 0 || (size_t)idx >= g_thumb_cache.size()) return;
    if (g_thumb_cache[idx].tex) {
        SDL_DestroyTexture(g_thumb_cache[idx].tex);
        g_thumb_cache[idx].tex = NULL;
    }
}

void ClearTimelineThumbCache(void)
{
    for (auto &t : g_thumb_cache) {
        if (t.tex) SDL_DestroyTexture(t.tex);
    }
    g_thumb_cache.clear();
}

/* ---- Composite preview selection + playback ---- */
int  g_timeline_composite[2] = {-1, -1};
bool g_timeline_composite_locked[2] = {false, false};
int  g_timeline_composite_drag_slot = -1;

void ClearTimelineCompositeSelection(void)
{
    g_timeline_composite[0] = -1;
    g_timeline_composite[1] = -1;
    g_timeline_composite_locked[0] = false;
    g_timeline_composite_locked[1] = false;
    g_timeline_composite_drag_slot = -1;
}

void CompactTimelineCompositeSelection(void)
{
    if (g_timeline_composite[0] < 0 && g_timeline_composite[1] >= 0) {
        g_timeline_composite[0] = g_timeline_composite[1];
        g_timeline_composite[1] = -1;
        g_timeline_composite_locked[0] = g_timeline_composite_locked[1];
        g_timeline_composite_locked[1] = false;
    }
    if (g_timeline_composite[0] == g_timeline_composite[1]) {
        g_timeline_composite[1] = -1;
        g_timeline_composite_locked[1] = false;
    }
}

void DrawTimelineCompositeLockToggle(int slot, const char *name)
{
    if (slot < 0 || slot > 1) return;

    bool locked = g_timeline_composite_locked[slot];
    ImVec4 bg = locked ? ImVec4(0.45f, 0.30f, 0.12f, 1.0f)
                       : ImVec4(0.10f, 0.38f, 0.42f, 1.0f);
    ImVec4 hover = locked ? ImVec4(0.58f, 0.39f, 0.16f, 1.0f)
                          : ImVec4(0.14f, 0.50f, 0.55f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, bg);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, hover);

    char label[64];
    const char *icon = locked
        ? (g_icon_font_loaded ? ICON_LOCK : ICON_LOCK_TXT)
        : (g_icon_font_loaded ? ICON_UNLOCK : ICON_UNLOCK_TXT);
    snprintf(label, sizeof(label), "%s %s: %s##timeline_lock_%d",
             icon, name, locked ? "Locked" : "Free", slot);
    if (ImGui::SmallButton(label)) {
        g_timeline_composite_locked[slot] = !locked;
        if (g_timeline_composite_locked[slot] &&
            g_timeline_composite_drag_slot == slot)
            g_timeline_composite_drag_slot = -1;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(locked
            ? "%s sprite is locked. Click to allow anipoint dragging."
            : "%s sprite can be dragged in the composite preview. Click to lock it.",
            name);
    }

    ImGui::PopStyleColor(2);
}

/* ---- Composite preview rendering + playback flag ---- */
bool g_is_playing = false;

/* Drag interaction state for the composite preview (preview-private). */
static ImVec2         g_timeline_composite_drag_mouse(0.0f, 0.0f);
static unsigned short g_timeline_composite_drag_anix = 0;
static unsigned short g_timeline_composite_drag_aniy = 0;

bool DrawTimelineCompositePreview(ImVec2 avail, ImVec2 img_pos)
{
    PruneTimelineCompositeSelection();
    if (!TimelineCompositeReady()) return false;

    int back_idx = g_timeline_composite[0];
    int front_idx = g_timeline_composite[1];
    IMG *back = get_img(back_idx);
    IMG *front = get_img(front_idx);
    if (!back || !front || !back->data_p || !front->data_p ||
        back->w == 0 || back->h == 0 || front->w == 0 || front->h == 0)
        return false;

    struct Placement {
        IMG *img;
        int left, top, right, bottom;
        ImU32 outline;
        unsigned char alpha;
    };

    Placement p[2] = {
        { back,
          -(int)(short)back->anix,  -(int)(short)back->aniy,
          -(int)(short)back->anix + (int)back->w,
          -(int)(short)back->aniy + (int)back->h,
          IM_COL32(120, 190, 255, 220), 185 },
        { front,
          -(int)(short)front->anix, -(int)(short)front->aniy,
          -(int)(short)front->anix + (int)front->w,
          -(int)(short)front->aniy + (int)front->h,
          IM_COL32(255, 190, 90, 230), 255 }
    };

    int min_x = 0, min_y = 0, max_x = 0, max_y = 0;
    for (int i = 0; i < 2; i++) {
        if (p[i].left   < min_x) min_x = p[i].left;
        if (p[i].top    < min_y) min_y = p[i].top;
        if (p[i].right  > max_x) max_x = p[i].right;
        if (p[i].bottom > max_y) max_y = p[i].bottom;
        if (secondary_anipoint_in_use(p[i].img)) {
            int sx2 = p[i].left + (int)(short)p[i].img->anix2;
            int sy2 = p[i].top  + (int)(short)p[i].img->aniy2;
            if (sx2 < min_x) min_x = sx2;
            if (sy2 < min_y) min_y = sy2;
            if (sx2 > max_x) max_x = sx2;
            if (sy2 > max_y) max_y = sy2;
        }
    }

    int union_w = max_x - min_x;
    int union_h = max_y - min_y;
    if (union_w <= 0 || union_h <= 0) return false;

    const float margin = 24.0f;
    float fit_x = (avail.x - margin * 2.0f) / (float)union_w;
    float fit_y = (avail.y - margin * 2.0f) / (float)union_h;
    float scale = fit_x < fit_y ? fit_x : fit_y;
    if (scale > 1.0f) scale = floorf(scale);
    if (scale < 0.25f) scale = 0.25f;
    if (scale > ZOOM_MAX) scale = ZOOM_MAX;

    float cw = union_w * scale;
    float ch = union_h * scale;
    ImVec2 cpos(img_pos.x + (avail.x - cw) * 0.5f,
                img_pos.y + (avail.y - ch) * 0.5f);
    ImVec2 cend(cpos.x + cw, cpos.y + ch);
    ImVec2 anchor(cpos.x + (0 - min_x) * scale,
                  cpos.y + (0 - min_y) * scale);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    float cs = 8.0f * scale;
    if (cs < 6.0f) cs = 6.0f;
    for (float y = cpos.y; y < cend.y; y += cs) {
        for (float x = cpos.x; x < cend.x; x += cs) {
            int row = (int)((y - cpos.y) / cs);
            int col = (int)((x - cpos.x) / cs);
            ImU32 col32 = ((row + col) & 1) ? IM_COL32(70, 70, 70, 255) : IM_COL32(40, 40, 40, 255);
            float x2 = x + cs; if (x2 > cend.x) x2 = cend.x;
            float y2 = y + cs; if (y2 > cend.y) y2 = cend.y;
            dl->AddRectFilled(ImVec2(x, y), ImVec2(x2, y2), col32);
        }
    }

    auto draw_crosshair = [&](ImVec2 pt, ImU32 col, float len, float thick) {
        dl->AddLine(ImVec2(pt.x - len, pt.y), ImVec2(pt.x + len, pt.y), col, thick);
        dl->AddLine(ImVec2(pt.x, pt.y - len), ImVec2(pt.x, pt.y + len), col, thick);
        dl->AddCircleFilled(pt, 1.5f, col);
    };

    SDL_Texture *tex[2] = {
        BuildWorldSpriteTexture(g_doc, p[0].img, p[0].alpha),
        BuildWorldSpriteTexture(g_doc, p[1].img, p[1].alpha)
    };
    ImVec2 sprite_min[2] = {};
    ImVec2 sprite_max[2] = {};
    bool sprite_valid[2] = {false, false};

    for (int i = 0; i < 2; i++) {
        if (!tex[i]) continue;
        ImVec2 sp(cpos.x + (p[i].left - min_x) * scale,
                  cpos.y + (p[i].top - min_y) * scale);
        ImVec2 se(sp.x + p[i].img->w * scale,
                  sp.y + p[i].img->h * scale);
        sprite_min[i] = sp;
        sprite_max[i] = se;
        sprite_valid[i] = true;
        dl->AddImage((ImTextureID)(intptr_t)tex[i], sp, se);
        dl->AddRect(sp, se, p[i].outline, 0.0f, 0, 1.0f);

        if (secondary_anipoint_in_use(p[i].img)) {
            ImVec2 s2(sp.x + (short)p[i].img->anix2 * scale,
                      sp.y + (short)p[i].img->aniy2 * scale);
            draw_crosshair(s2, p[i].outline, 9.0f, 1.2f);
            dl->AddLine(anchor, s2, p[i].outline, 1.0f);
        }
    }

    draw_crosshair(anchor, IM_COL32(255, 230, 80, 255), 15.0f, 1.6f);
    dl->AddRect(cpos, cend, IM_COL32(210, 210, 210, 160), 0.0f, 0, 1.0f);

    ImGuiIO &io = ImGui::GetIO();
    ImVec2 mouse = io.MousePos;
    bool over_lock_control = false;
    auto mouse_in_rect = [&](ImVec2 a, ImVec2 b) -> bool {
        return mouse.x >= a.x && mouse.x < b.x && mouse.y >= a.y && mouse.y < b.y;
    };

    std::string back_name = img_name_string(back);
    std::string front_name = img_name_string(front);
    char buf[224];
    snprintf(buf, sizeof(buf), "Composite  [%d] %s  +  [%d] %s",
             back_idx, back_name.c_str(), front_idx, front_name.c_str());
    ImVec2 label_sz = ImGui::CalcTextSize(buf);
    float label_w = label_sz.x + 10.0f;
    if (label_w > cw) label_w = cw;
    float header_h = (cend.y - cpos.y >= 48.0f) ? 48.0f : 20.0f;
    dl->AddRectFilled(cpos, ImVec2(cend.x, cpos.y + header_h),
                      IM_COL32(0, 0, 0, 170));
    dl->AddRectFilled(cpos, ImVec2(cpos.x + label_w, cpos.y + 20.0f),
                      IM_COL32(0, 0, 0, 180));
    dl->AddText(ImVec2(cpos.x + 5.0f, cpos.y + 3.0f),
                IM_COL32(235, 235, 235, 255), buf);

    auto draw_lock_button = [&](int slot, const char *name, ImVec2 pos) {
        char lbuf[48];
        bool locked = g_timeline_composite_locked[slot];
        const char *icon = locked
            ? (g_icon_font_loaded ? ICON_LOCK : ICON_LOCK_TXT)
            : (g_icon_font_loaded ? ICON_UNLOCK : ICON_UNLOCK_TXT);
        snprintf(lbuf, sizeof(lbuf), "%s %s: %s", icon, name,
                 locked ? "Locked" : "Free");
        ImVec2 text_sz = ImGui::CalcTextSize(lbuf);
        ImVec2 pad(6.0f, 3.0f);
        ImVec2 a(pos.x, pos.y);
        ImVec2 b(pos.x + text_sz.x + pad.x * 2.0f,
                 pos.y + text_sz.y + pad.y * 2.0f);
        bool hover = mouse_in_rect(a, b);
        over_lock_control = over_lock_control || hover;
        ImU32 bg = locked
            ? IM_COL32(90, 70, 40, hover ? 245 : 215)
            : IM_COL32(25, 75, 75, hover ? 245 : 210);
        dl->AddRectFilled(a, b, bg, 3.0f);
        dl->AddRect(a, b, IM_COL32(230, 230, 230, 180), 3.0f);
        dl->AddText(ImVec2(a.x + pad.x, a.y + pad.y),
                    IM_COL32(245, 245, 245, 255), lbuf);
        if (hover && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_timeline_composite_locked[slot] = !g_timeline_composite_locked[slot];
            if (g_timeline_composite_locked[slot] &&
                g_timeline_composite_drag_slot == slot)
                g_timeline_composite_drag_slot = -1;
        }
        return b.x - a.x;
    };

    ImVec2 lock_pos(cpos.x + 5.0f, cpos.y + 24.0f);
    float lock_w = draw_lock_button(0, "Back", lock_pos);
    draw_lock_button(1, "Front", ImVec2(lock_pos.x + lock_w + 6.0f, lock_pos.y));

    auto hit_test_sprite = [&](int slot) -> bool {
        if (slot < 0 || slot > 1 || g_timeline_composite_locked[slot] || !sprite_valid[slot])
            return false;
        if (!mouse_in_rect(sprite_min[slot], sprite_max[slot]))
            return false;
        IMG *img = p[slot].img;
        int px = (int)((mouse.x - sprite_min[slot].x) / scale);
        int py = (int)((mouse.y - sprite_min[slot].y) / scale);
        if (px < 0 || py < 0 || px >= (int)img->w || py >= (int)img->h)
            return false;
        int stride = (img->w + 3) & ~3;
        const unsigned char *data = (const unsigned char *)img->data_p;
        return data && data[py * stride + px] != 0;
    };

    int hover_slot = -1;
    if (!over_lock_control) {
        if (hit_test_sprite(1)) hover_slot = 1;
        else if (hit_test_sprite(0)) hover_slot = 0;
    }
    if (hover_slot >= 0) {
        dl->AddRect(sprite_min[hover_slot], sprite_max[hover_slot],
                    IM_COL32(255, 255, 255, 230), 0.0f, 0, 2.0f);
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
    }

    if (!over_lock_control && hover_slot >= 0 &&
        ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        g_is_playing = false;
        g_timeline_composite_drag_slot = hover_slot;
        g_timeline_composite_drag_mouse = mouse;
        g_timeline_composite_drag_anix = p[hover_slot].img->anix;
        g_timeline_composite_drag_aniy = p[hover_slot].img->aniy;
        g_doc->ilselected = g_timeline_composite[hover_slot];
        g_zoom_reset = true;
    }

    if (g_timeline_composite_drag_slot >= 0) {
        int slot = g_timeline_composite_drag_slot;
        IMG *drag_img = p[slot].img;
        if (!drag_img || g_timeline_composite_locked[slot] ||
            !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            g_timeline_composite_drag_slot = -1;
        } else {
            int dx = (int)((mouse.x - g_timeline_composite_drag_mouse.x) / scale);
            int dy = (int)((mouse.y - g_timeline_composite_drag_mouse.y) / scale);
            int nx = (int)(short)g_timeline_composite_drag_anix - dx;
            int ny = (int)(short)g_timeline_composite_drag_aniy - dy;
            if (nx < -32768) nx = -32768; if (nx > 32767) nx = 32767;
            if (ny < -32768) ny = -32768; if (ny > 32767) ny = 32767;
            g_doc->ilselected = g_timeline_composite[slot];
            set_primary_anipoint_with_sequence(drag_img, nx, ny);
        }
    }

    ImGui::Dummy(ImVec2(avail.x, avail.y));
    return true;
}

void PruneTimelineCompositeSelection(void)
{
    for (int i = 0; i < 2; i++) {
        int idx = g_timeline_composite[i];
        if (idx < 0 || (unsigned int)idx >= g_doc->imgcnt) {
            g_timeline_composite[i] = -1;
            g_timeline_composite_locked[i] = false;
            if (g_timeline_composite_drag_slot == i)
                g_timeline_composite_drag_slot = -1;
        }
    }
    CompactTimelineCompositeSelection();
}

int TimelineCompositeSlot(int img_idx)
{
    if (g_timeline_composite[0] == img_idx) return 0;
    if (g_timeline_composite[1] == img_idx) return 1;
    return -1;
}

bool TimelineCompositeReady(void)
{
    return g_timeline_composite[0] >= 0 && g_timeline_composite[1] >= 0 &&
           g_timeline_composite[0] != g_timeline_composite[1];
}

bool TimelineAnyCompositeLocked(void)
{
    return TimelineCompositeReady() &&
           (g_timeline_composite_locked[0] || g_timeline_composite_locked[1]);
}

bool AdvanceTimelineComposite(int delta)
{
    if (!TimelineCompositeReady() || g_timeline_frames.empty()) return false;

    int p0 = TimelineFramePosition(g_timeline_composite[0]);
    int p1 = TimelineFramePosition(g_timeline_composite[1]);
    if (p0 < 0 || p1 < 0) {
        ClearTimelineCompositeSelection();
        return false;
    }

    p0 = WrapTimelinePosition(p0 + delta);
    p1 = WrapTimelinePosition(p1 + delta);
    g_timeline_composite[0] = g_timeline_frames[p0];
    g_timeline_composite[1] = g_timeline_frames[p1];
    g_timeline_play_idx = p0;
    g_doc->ilselected = g_timeline_composite[0];
    g_zoom_reset = true;
    return true;
}

void StepTimelinePlayhead(int delta)
{
    if (g_timeline_frames.empty()) return;

    if (AdvanceTimelineComposite(delta))
        return;

    g_timeline_play_idx = WrapTimelinePosition(g_timeline_play_idx + delta);
    g_doc->ilselected = g_timeline_frames[g_timeline_play_idx];
    g_zoom_reset = true;
}

void ToggleTimelineCompositeFrame(int img_idx)
{
    if (img_idx < 0 || (unsigned int)img_idx >= g_doc->imgcnt) return;

    int slot = TimelineCompositeSlot(img_idx);
    if (slot >= 0) {
        g_timeline_composite[slot] = -1;
        g_timeline_composite_locked[slot] = false;
        if (g_timeline_composite_drag_slot == slot)
            g_timeline_composite_drag_slot = -1;
        CompactTimelineCompositeSelection();
        return;
    }

    if (g_timeline_composite[0] < 0) {
        g_timeline_composite[0] = img_idx;
        g_timeline_composite_locked[0] = false;
    } else if (g_timeline_composite[1] < 0) {
        g_timeline_composite[1] = img_idx;
        g_timeline_composite_locked[1] = false;
    } else {
        g_timeline_composite[0] = g_timeline_composite[1];
        g_timeline_composite[1] = img_idx;
        g_timeline_composite_locked[0] = g_timeline_composite_locked[1];
        g_timeline_composite_locked[1] = false;
    }
}
