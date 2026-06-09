/*************************************************************
 * platform/ui_timeline.cpp
 * Timeline frame-model state and operations declared in ui_timeline.h.
 *************************************************************/
#include "ui_timeline.h"
#include "img_format.h"   /* IMG, PAL, get_img, get_pal */
#include "document.h"     /* g_doc */
#include "ui_internal.h"  /* g_imgui_renderer */

#include <algorithm>   /* std::swap */

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
