/*************************************************************
 * platform/color_ops.h
 * Pure color operations for palette editing.
 *
 * These helpers operate on raw palette buffers and caller-owned output
 * storage. They do not touch UI globals, document state, or undo history.
 *************************************************************/
#pragma once

/* Apply absolute HSL adjustments to a 15-bit RGB555 baseline palette.
 *
 * `baseline_words` is `count * 2` bytes of packed palette words.
 * `selected_mask` may be NULL. If it contains any selected entry, only those
 * entries are adjusted and unselected colors are copied back from baseline.
 * `out_words` receives `count * 2` packed words and may alias neither input
 * nor output RGB assumptions. `out_rgb` is optional `count * 3` bytes of
 * 8-bit RGB triples for UI preview/work palettes.
 */
void HslAdjustPaletteWordsFromBaseline(const unsigned char *baseline_words,
                                       int count,
                                       const bool *selected_mask,
                                       int hue_deg,
                                       int sat_pct,
                                       int light_pct,
                                       unsigned char *out_words,
                                       unsigned char *out_rgb);
