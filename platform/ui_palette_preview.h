/*************************************************************
 * platform/ui_palette_preview.h
 * Small drawing helpers shared by the palette-rework dialogs (Transfer to
 * Palette by Ramps, Alternate Costume): palette swatch strips and a sprite
 * rendered through an arbitrary palette / index map for before-after panes.
 *************************************************************/
#ifndef UI_PALETTE_PREVIEW_H
#define UI_PALETTE_PREVIEW_H

#include <vector>

struct PAL;
struct IMG;

/* Decode a PAL's packed 15-bit words (at most 256). */
std::vector<unsigned short> PalettePreviewWords(const PAL *pal);

/* One row of `count` swatches from `words` starting at slot `start`. When
   `via_map` is given, each source slot is drawn as the `map_words` color it
   maps to instead. Shows at most 24; hovering says how many were cut. */
void PalettePreviewStrip(const std::vector<unsigned short> &words, int start, int count,
                         const unsigned char *via_map = nullptr,
                         const std::vector<unsigned short> *map_words = nullptr);

/* The whole of `words` as clickable swatches, 16 to a row, with slots
   outside [first, last] dimmed. Hovering a swatch names its slot and color.
   Left-click moves `first` there, right-click moves `last` (either one drags
   the other along if they would cross). Slot 0 is shown but not pickable.
   Returns true when the range changed. */
bool PalettePickerGrid(const char *id, const std::vector<unsigned short> &words,
                       int *first, int *last);

/* A labelled, centred, nearest-scaled pane of `img` drawn through `words`
   (after `map`, when given). `slot` picks one of four cached textures so
   several panes can coexist in a frame. */
void PalettePreviewSprite(int slot, const char *label, const IMG *img,
                          const std::vector<unsigned short> &words,
                          const unsigned char *map, float pane_w, float pane_h);

#endif /* UI_PALETTE_PREVIEW_H */
