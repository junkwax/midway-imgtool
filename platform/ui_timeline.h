/*************************************************************
 * platform/ui_timeline.h
 * Animation-timeline frame model: the ordered list of frames and the
 * per-frame hold counts, plus the operations that mutate them.
 *
 * Phase C of the overlay split (refactoring_plan.md) — the first UI subsystem
 * lifted out of imgui_overlay.cpp. This is just the frame model: it has no
 * ImGui or document (g_doc) coupling. The timeline rendering, playback, and
 * composite-preview code stays in the overlay for now and calls in here.
 *************************************************************/
#pragma once
#include <vector>
#include <SDL.h>

/* Ordered timeline frames (image indices) and matching per-frame hold counts;
   g_timeline_play_idx is the current playhead position into g_timeline_frames. */
extern std::vector<int> g_timeline_frames;
extern std::vector<int> g_timeline_holds;
extern int              g_timeline_play_idx;

int  TimelineFramePosition(int img_idx);   /* position of img_idx, or -1 */
int  WrapTimelinePosition(int pos);        /* wrap pos into [0, size) */
int  ClampTimelineHold(int hold);          /* clamp to [1, 120] */
void EnsureTimelineHolds(void);            /* keep holds[] sized to frames[] */
void TimelinePushFrame(int img_idx, int hold = 1);
void TimelineSetFrames(const std::vector<int> &frames);
void TimelineClearFrames(void);
int  TimelineHoldAt(int pos);
void TimelineSetHoldAt(int pos, int hold);
void TimelineSwapFrames(int a, int b);
void TimelineMoveFrame(int src_idx, int dst_idx);

/* ---- Timeline thumbnail cache ----
   Per-image thumbnails for the timeline strip, keyed by image index. Building
   a thumbnail reads the document (g_doc/get_img/get_pal) and the SDL renderer,
   so unlike the frame model these helpers are coupled to overlay state. */
struct TimelineThumb {
    SDL_Texture *tex;
    int w, h;          /* thumbnail texture dims */
    int src_w, src_h;  /* source image dims at time of bake */
    int gen;           /* matches g_img_tex_idx at bake time */
};
extern std::vector<TimelineThumb> g_thumb_cache;

/* Build (or rebuild) the thumbnail for image idx; returns the entry or NULL. */
TimelineThumb *EnsureThumb(int idx);
/* Drop the cached texture for image idx (entry stays, tex set to NULL). */
void InvalidateThumb(int idx);
/* Destroy all cached thumbnail textures and clear the cache. */
void ClearTimelineThumbCache(void);

/* ---- Composite preview selection + playback ----
   The composite pair (two image indices shown anipoint-aligned in the preview)
   and the playhead-advance logic. The preview *rendering* (DrawTimelineComposite*)
   and the public play-toggle thunk stay in the overlay and call in here. */
extern int  g_timeline_composite[2];        /* Ctrl-click pair; -1 = empty slot */
extern bool g_timeline_composite_locked[2];
extern int  g_timeline_composite_drag_slot; /* slot being dragged, or -1 */

void ClearTimelineCompositeSelection(void);
void CompactTimelineCompositeSelection(void);
void PruneTimelineCompositeSelection(void);   /* drop indices past g_doc->imgcnt */
int  TimelineCompositeSlot(int img_idx);      /* 0/1 if selected, else -1 */
bool TimelineCompositeReady(void);            /* two distinct slots chosen */
bool TimelineAnyCompositeLocked(void);
bool AdvanceTimelineComposite(int delta);     /* step the locked composite pair */
void StepTimelinePlayhead(int delta);
void ToggleTimelineCompositeFrame(int img_idx);

/* Small ImGui control: a lock/free toggle button for composite slot 0/1.
   (The large composite-preview rendering stays in the overlay for now because
   it depends on World-View sprite texturing and anipoint editing.) */
void DrawTimelineCompositeLockToggle(int slot, const char *name);
