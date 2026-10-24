/*************************************************************
 * platform/ui_palette_effects.h
 * "Generate Character Palettes" dialog: from one fighter's palette, build
 * the freeze / electric palettes (FREZ, NUFRZ, ELPUP, ELRED, REDEL, PURPL)
 * out of the saved Indexed Color Gradient ramps, plus a BLUD palette for
 * each of the fighter's costume palettes. palette_effects.h has the rules.
 *************************************************************/
#ifndef UI_PALETTE_EFFECTS_H
#define UI_PALETTE_EFFECTS_H

enum CharacterPaletteSet {
    CHARACTER_PALETTES_ALL,
    CHARACTER_PALETTES_EFFECTS,
    CHARACTER_PALETTES_BLOOD,
};

/* Open on palette `pal_idx` (the right-clicked row). `set` picks which rows
   start ticked; every row stays available in the dialog. */
void OpenCharacterPaletteGenerator(int pal_idx, CharacterPaletteSet set);

/* Draw it. Safe to call every frame; does nothing while hidden. */
void DrawCharacterPaletteGenerator(void);

#endif /* UI_PALETTE_EFFECTS_H */
