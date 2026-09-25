/*************************************************************
 * platform/ui_palette.h
 * Palette editor, HSL adjustments, and quantization UI.
 *
 * Part of the Phase C overlay split. This module owns the bottom palette
 * swatch grid, palette list, HSL sliders, histogram, color remapping, and
 * palette merge/reduction dialogs.
 *************************************************************/
#pragma once
#include <imgui.h>
#include "document.h"
#include "img_format.h"

#include <vector>

struct PaletteReduceColor {
    int old_idx;
    unsigned char r, g, b;
    double weight;
};

struct PaletteReduceBox {
    std::vector<int> colors;
    int rmin, rmax, gmin, gmax, bmin, bmax;
    double weight;
};

struct PaletteReduceRep {
    unsigned char r, g, b;
    int old_min;
    int luma;
    int sat;
    int hue;
    int family;
};

struct PaletteReductionPlan {
    bool valid;
    int pal_idx;
    int target_bpp;
    int target_numc;
    int old_numc;
    int new_numc;
    int input_colors;
    int output_colors;
    int active_colors;
    int images;
    int pixels;
    int pixels_changed;
    int colors_merged;
    bool quantized;
    unsigned char remap[256];
    unsigned char data[512];
    char error[128];
};

struct PaletteMergeQuality {
    int target_idx;
    char target_name[16];
    int source_palettes;
    int remapped_images;
    int affected_pixels;
    int exact_pixels;
    int color_drift_pixels;
    int transparent_drift_pixels;
    int invalid_pixels;
    int ppp_warning_images;
    long long total_dist;
    int max_dist;
    int max_src_slot;
    int max_dst_slot;
    char max_palette[16];
    char max_image[16];
    bool opt_grow;
    bool opt_perceptual;
    int target_base_numc;
    int colors_added;
    int colors_overflow;
    unsigned short added_words[256];
};

/* ---- Palette UI Widgets ---- */
void DrawBottomPaletteBar(ImVec2 avail);
void DrawPaletteMergeQualityDialog(void);
void DrawPaletteReduceDialog(void);
void DrawPaletteHistogramDialog(void);
void DrawPaletteSingleColorDialog(void);
void DrawIndexedGradientDialog(void);

/* ---- Palette Operations & Helpers ---- */
void AddNewPalette(void);
void DuplicatePalette(void);
int  CountMarkedPalettes(void);
int  CopyPaletteToClipboard(int fallback_idx = -1,  /* marked palettes, else fallback/selected */
                            bool only_fallback = false);
bool PaletteListHasFocus(void);              /* Ctrl+C/V act on whole palettes */
int  PastePaletteFromClipboard(void); /* appends every copied palette */
void SelectPalette(int idx);
void SetPaletteOfSelected(void);
void SetPaletteOfMarked(void);
void DeletePalette(void);
void MergeMarkedPalettes(void);
void OpenPaletteMergePreview(void);
int InheritSelectedPaletteFromMarked(void);
int MergeDuplicatePalettes(void);
void ApplySelectionRemapToMatchingSprites(void);
/* Reserve the selection's palette indices for the selection alone: repoint
   every pixel outside it that shares one at a duplicate slot holding the
   same color, so the frame looks identical but those indices can be
   recolored without touching the rest of the sprite. */
void IsolateSelectionColors(void);
/* Same isolation run across every sprite on the palette, locating the feature
   in each frame with the propagation matcher. Opens a per-frame preview. */
void OpenIsolatePropagatePreview(void);
void DrawIsolatePropagateDialog(void);
void BuildSelectedPaletteUsage(void);
int FindNearestUsedPaletteSlotForUsage(int color_idx, int *dist_out);
void CalculatePaletteHistogram(void);

void hsl_adjust_palette_from_baseline(int hue_deg, int sat_pct, int light_pct);
void hue_shift_palette(int delta_deg);
void save_palette_baseline(void);
void reset_palette_adjust_sliders(void);
void commit_palette_adjustments(void);
void reset_palette_to_baseline(void);
void ClearWorkingPalette(void);
void ApplyPalette(int pal_idx);
void OpenPaletteReduceDialog(int bpp);
void OpenPaletteSingleColorDialog(void);
void OpenIndexedGradientDialog(void);

/* ---- Saved gradient ramps -------------------------------------------
   The 2-11 stop ramps the Indexed Color Gradient stores in the user profile
   (indexed_gradients.txt). They are not that dialog's private property: the
   Sprite Ramp Gradient spends the same ramps on sprite indices instead of
   palette slots, and an artist who saved "torch" once should find it in both
   places. Stops are RGB floats 0..1, in ramp order. */
void EnsureGradientPresetsLoaded(void);
int  GradientPresetCount(void);
const char *GradientPresetName(int idx);
/* Copies at most `max_stops` stops into `out_rgb` (3 floats each). Returns
   stops written, 0 for a bad index. */
int  GradientPresetStops(int idx, float *out_rgb, int max_stops);
/* Add or replace by name, then rewrite the file. False if the file could not
   be written — the in-memory list is still updated. */
bool SaveGradientPreset(const char *name, const float *stops_rgb, int stop_count);

void InvalidatePaletteUsage(void);
bool ensure_palette_numc(PAL *pal, int min_numc);
void CleanupSelectedPalette(void);
void GroupLikeColorsSelectedPalette(void);
void CreateCleanedPaletteCopy(void);
void CopyPaletteZeroToOpaqueSlot(int requested_slot);
void CopyPaletteZeroToOpaqueSlot(void);
void ClearPaletteReducePreviewTextures(void);
void MoveSelectedPaletteColorsToEnd(void);
void ResetPaletteUiState(void);

/* Flip the selected swatches (or the whole palette, minus transparent index 0,
   when nothing is selected) to their RGB555 complement. Self-inverse. */
void InvertSelectedPaletteColors(void);
/* Reverse the order of colors across the selected indices (low-to-high becomes
   high-to-low). Distinct from the RGB complement invert: the colors are the
   same, their positions are not. Pixel data is untouched. */
void ReverseSelectedPaletteOrder(void);

/* Hue-wheel picker for the active swatch, `width` px wide. Drawn inside the
   pop-out anchored to the left toolbar's color square. */
void DrawActiveSwatchPickerBody(float width);

/* Palette-wide H/S/L offsets as a single `width`-px column: hue dial, exact
   hue field, saturation and lightness sliders, reset. */
void DrawActiveSwatchHslControls(float width);

