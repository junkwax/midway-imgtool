# Current Status: Adobe-Feature Audit Pass

The full Adobe-style toolkit shipped in `5231472` plus drag-drop reorder and
snap-to-content in `f1d9635` has now been audited end-to-end. Every bug found
in the audit was fixed; every UX gap from `ADOBE_FEATURES_PROPOSAL.md` was
closed; six additional "magic" features were added on top.

## Bugs Fixed
- **Lasso was dead code.** Replaced the rectangular-fallback stub with a real
  freehand polygon selector. While dragging, vertices accumulate and render
  as a magenta polyline. On release, the polygon is rasterized into the
  existing `g_grid_sel.pixel_mask` (scanline even-odd test), so copy/paste
  pipelines pick it up for free.
- **Clone Stamp leaked across image switches.** A `g_prev_ilselected` watcher
  at the top of `imgui_overlay_render` resets clone/source state, smart-remap
  target, and the snap bbox cache whenever the active image changes.
- **Clone Stamp was single-pixel only.** Now uses a round-disc brush with a
  radius slider (1..16) in the tool-options strip.
- **Clone Stamp had no source crosshair.** Cyan `+` is now drawn at the
  anchor while the tool is active; a white brush ring follows the cursor
  when radius > 1.
- **"Background Eraser" was a paint bucket of index 0.** Replaced with a
  proper `SmartErase` that honors a tolerance slider (palette-index
  distance), a contiguous/global toggle, and an optional defringe pass that
  averages the 1px halo of bluescreen spill into the surrounding skin.
- **Smart Remap had zero tolerance and never reset.** Now respects a
  tolerance slider; target color is captured on click, painted as long as
  the mouse is down, and forgotten on release. Captured target swatch shown
  inline in the tool-options strip.
- **Auto-Chop suffix overflowed.** Columns past 26 used `'A'+c` which spilled
  past `'Z'`. Now wraps via a leading letter (AA..AZ, BA..BZ, ...). Row
  suffix is sized by `strlen` so double-digit row counts trim the base name
  correctly instead of silently truncating.

## UX Gaps Closed
- **Magic Wand tolerance + contiguous toggle.** Tool-options strip exposes
  both. Global mode walks the whole image; contiguous mode keeps the original
  4-connected flood.
- **Timeline play/pause shortcut.** `K` toggles playback (Premiere/DaVinci
  convention). Spacebar stays bound to toggle-mark on the current image.
- **Timeline thumbnails.** Frame buttons now render a 48px thumbnail of each
  image via a per-image SDL_Texture cache; the index label still draws as a
  small overlay so scrubbing by number still works. Cache is invalidated
  on pixel edits and on image-count change.
- **Timeline reorder via keyboard.** `Ctrl+Left/Right` swaps the play-head
  frame with its neighbor in the timeline order.
- **Snap-to-Content polish.** Threshold is now in screen pixels (`6 / scale`
  per axis) so it feels the same at every zoom level. The underlying-sprite
  bbox is cached on Shift-press instead of recomputed every frame.
- **Snap guides.** Magenta vertical/horizontal lines render across the
  canvas while a snap is locked in, so the user can see which edge they
  anchored to.

## "Adobe Magic" Additions (new features)
- **Defringe Marked Edges** (Operations menu). One-pass 8-neighborhood
  average over every opaque pixel that touches a transparent one. Run twice
  for a 2px erode. Designed for cleaning up blue/green spill on digitized
  actor sprites after chroma removal.
- **Crop Marked to Content** (Operations menu). Trims each marked image to
  its non-transparent bbox and adjusts anipoints so the on-screen position
  is unchanged.
- **Align Marked Anipoints to Selected** (Operations menu). Sets the
  anipoint of every marked image to match the currently selected image's
  anipoint — quick way to anchor a head, hand, or weapon hilt across many
  frames.
- **Timeline onion-skin.** Toggle on the timeline bar; ghosts the previous
  (cool blue tint) and next (warm orange tint) frames behind the current
  one, anipoint-aligned. Works during playback or manual scrub.
- **Mask-aware copy/paste from Lasso/Magic Wand.** The copy path already
  honored `g_grid_sel.is_mask`; the Lasso work above now produces those
  masks, so freehand-cut and paste work end-to-end with non-rectangular
  selections.
- **Color isolation overlay.** `Alt+click` any palette swatch to dim every
  pixel that isn't that palette index (scanline-coalesced for speed).
  `Alt+click` again to clear. A magenta border highlights the isolated
  swatch in the palette strip.

## Deferred
- **Snap to adjacent frames.** Listed in the audit as Gap #14. The hard
  question is *which* neighbor frames count — previous in `g_timeline_frames`,
  all marked, or every frame in the file? Each has different semantics for
  the `_1A`/`_1B` chopping workflow. Punted to a follow-up so the design
  decision isn't rushed.

## Files Touched
- `platform/imgui_overlay.cpp` — tools, timeline, snap, isolation overlay
- `platform/img_io.cpp` + `platform/img_io.h` — Defringe, Crop, Align ops

## Build
Verified clean against VS 2022 x64 Release. SDL2 2.32.10, no new external
dependencies.
