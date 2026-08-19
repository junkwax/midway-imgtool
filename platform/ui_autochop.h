/*************************************************************
 * platform/ui_autochop.h
 * Auto-Chop and Auto-Split subsystem interfaces.
 *************************************************************/
#ifndef UI_AUTOCHOP_H
#define UI_AUTOCHOP_H

#include "img_format.h"
#include "ui_internal.h" /* For AutoChopPreview */

/* Bits per pixel LOAD2 will pack this sprite's data at: the PPP> directive
   when one is in force (overridden by the palette's own depth if the palette
   has more colors than PPP> can address), otherwise the palette's depth. */
int Load2BppForImage(const IMG *img);

void DrawAutoChopDialog(void);
void OpenAutoChopDialog(void);
/* Open the dialog scoped to one sprite, for the image-list right-click menu:
   picking a row there names a target, and the marked set must not override it. */
void OpenAutoChopDialogForImage(int img_idx);
void BuildAutoChopTargetSummary(AutoChopPreview *out);
void AutoChopSetThreeBandSize(void);
bool SelectedImageWillAutoChop(void);
bool BuildBestAutoSplitPreviewForImage(const IMG *img, bool vertical, AutoChopPreview *out);
bool BuildAutoChopPreviewForImage(const IMG *img, AutoChopPreview *out);
void DrawAutoChopPreviewRects(ImDrawList *dl, const AutoChopPreview &out, ImVec2 img_pos, float sx, float sy, bool fill);

#endif /* UI_AUTOCHOP_H */
