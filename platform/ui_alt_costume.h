/*************************************************************
 * platform/ui_alt_costume.h
 * "Alternate Costume..." dialog: copy a palette and recolor only the ramps
 * picked (the cloth, a trim) to a new hue, keeping their shading — Midway's
 * RADRED_P -> RADBLU_P operation. Sprite pixels are never touched.
 *************************************************************/
#ifndef UI_ALT_COSTUME_H
#define UI_ALT_COSTUME_H

/* Open on the selected palette (or the selected sprite's, when no palette
   is selected). */
void OpenAltCostumeDialog(void);

/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawAltCostumeDialog(void);

#endif /* UI_ALT_COSTUME_H */
