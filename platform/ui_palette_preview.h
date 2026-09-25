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

/* What a click on a PalettePickerGrid asked for; -1 fields were not asked. */
struct PalettePick {
    int set_first = -1;   /* click: start the active range here */
    int set_last = -1;    /* right-click: end the active range here */
    int add = -1;         /* Ctrl+click: new one-slot range here */
    int remove = -1;      /* Ctrl+right-click: drop the range holding this slot */
};

/* The whole of `words` as clickable swatches, 16 to a row. Slots where
   `selected[s]` is zero are dimmed; the active range's ends are outlined in
   white. Hovering a swatch names its slot and color. Slot 0 is shown but
   not pickable. Returns true when `out` holds a request. */
bool PalettePickerGrid(const char *id, const std::vector<unsigned short> &words,
                       const char *selected, int active_first, int active_last,
                       PalettePick *out);

/* A labelled, centred, nearest-scaled pane of `img` drawn through `words`
   (after `map`, when given). `slot` picks one of four cached textures so
   several panes can coexist in a frame. */
void PalettePreviewSprite(int slot, const char *label, const IMG *img,
                          const std::vector<unsigned short> &words,
                          const unsigned char *map, float pane_w, float pane_h);

#endif /* UI_PALETTE_PREVIEW_H */
