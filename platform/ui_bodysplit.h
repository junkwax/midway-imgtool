/*************************************************************
 * platform/ui_bodysplit.h
 * "Split Body Parts" dialog: auto-detect head / arms / torso / legs, let the
 * user correct the proposed boxes, then cut them into child IMGs.
 *************************************************************/
#ifndef UI_BODYSPLIT_H
#define UI_BODYSPLIT_H

/* Seed the plan from the selected sprite and show the dialog. */
void OpenBodySplitDialog(void);
/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawBodySplitDialog(void);
/* True when the selected sprite has enough opaque pixels to attempt a split. */
bool SelectedImageCanBodySplit(void);

#endif /* UI_BODYSPLIT_H */
