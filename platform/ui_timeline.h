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
