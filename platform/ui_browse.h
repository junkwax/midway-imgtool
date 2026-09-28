/*************************************************************
 * platform/ui_browse.h
 * The Browse canvas tab: a scrollable thumbnail grid of every sprite in the
 * current IMG, for flicking through a large library by eye instead of by name.
 *
 * Sits alongside Image / World / Anim / Link / React as another main-view mode.
 * It edits nothing: clicking a cell selects that sprite (the same ilselected
 * every other view reads), Ctrl-click marks it, and double-click opens it on
 * the Image tab. Thumbnails are baked lazily for the cells on screen only and
 * dropped again once they scroll away, so a library of thousands of frames
 * stays cheap. Anything that already calls InvalidateThumb() /
 * ClearTimelineThumbCache() after an edit refreshes this grid too.
 *************************************************************/
#ifndef UI_BROWSE_H
#define UI_BROWSE_H

#include "imgui.h"

/* Draw the Browse workspace into the canvas rect. Same shape as the other
   workspace draws: `avail` is the canvas content region, `img_pos` its
   top-left in screen space. */
void DrawBrowseWorkspace(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io);

/* Drop the baked thumbnail for image `idx` / for every image. Called from the
   timeline cache's own invalidators so existing edit paths need no changes. */
void BrowseInvalidateThumb(int idx);
void BrowseClearThumbCache(void);

#endif /* UI_BROWSE_H */
