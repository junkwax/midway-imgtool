/*************************************************************
 * platform/ui_autochop.h
 * Auto-Chop and Auto-Split subsystem interfaces.
 *************************************************************/
#ifndef UI_AUTOCHOP_H
#define UI_AUTOCHOP_H

#include "img_format.h"
#include "ui_internal.h" /* For AutoChopPreview */

void DrawAutoChopDialog(void);
void OpenAutoChopDialog(void);
void BuildAutoChopTargetSummary(AutoChopPreview *out);
void AutoChopSetThreeBandSize(void);
bool SelectedImageWillAutoChop(void);
bool BuildBestAutoSplitPreviewForImage(const IMG *img, bool vertical, AutoChopPreview *out);
bool BuildAutoChopPreviewForImage(const IMG *img, AutoChopPreview *out);
void DrawAutoChopPreviewRects(ImDrawList *dl, const AutoChopPreview &out, ImVec2 img_pos, float sx, float sy, bool fill);

#endif /* UI_AUTOCHOP_H */
