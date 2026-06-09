/*************************************************************
 * platform/ui_timeline.cpp
 * Timeline frame-model state and operations declared in ui_timeline.h.
 *************************************************************/
#include "ui_timeline.h"

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
