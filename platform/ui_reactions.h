/*************************************************************
 * platform/ui_reactions.h
 * The React canvas tab: every reaction an opponent has, and the anipoints of
 * each frame in the reaction's sequence.
 *
 * Sits alongside Image / World / Anim / Link as a fifth main-view mode. Where
 * the ASM Animations window shows one animation you already knew the name of,
 * this shows the reaction half of a character's anitab as a sorted, grouped
 * list, next to the per-frame anipoint table you need in order to line an
 * attack up against it. Reads the same AsmAnim data the ASM window and the
 * World View lanes read, and writes its selection back through the same
 * globals, so picking a reaction here arms the lane too.
 *************************************************************/
#ifndef UI_REACTIONS_H
#define UI_REACTIONS_H

#include "imgui.h"

/* Draw the React workspace into the canvas rect. Mirrors DrawSeqScrWorkspace's
   shape: `avail` is the canvas content region, `img_pos` its top-left in screen
   space. Returns true when it consumed the canvas (always, today — the return
   keeps it interchangeable with the other workspace draws). */
bool DrawReactionWorkspace(ImVec2 avail, ImVec2 img_pos, ImGuiIO &io);

#endif /* UI_REACTIONS_H */
