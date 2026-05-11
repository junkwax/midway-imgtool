# Changelog

All notable changes to midway-imgtool. Tags carry the version, e.g. `v2.3.0`.

The workflow at [.github/workflows/c-cpp.yml](.github/workflows/c-cpp.yml)
extracts the section matching the pushed tag and uses it as the GitHub
Release body. Keep new entries near the top of the file under a new
`## [vX.Y.Z]` header — anchor exactly as `## [v2.3.0]` (square brackets
included) so the extractor matches.

## [v2.4.0] — PNG import rewrite & palette workflow

Targeted fixes to the parts of the tool that were silently destructive or
silently lossy. The headline: PNG import is no longer broken.

### PNG import
- **Median-cut quantization** in 15-bit RGB space. Previously the import
  capped the unique-color histogram at 4096 entries and picked the
  palette by raw frequency, with exact-match-only pixel mapping. Any PNG
  with anti-aliasing or smooth gradients lost most of its colors to
  index 0 (transparent). Now: full histogram across all 32768 possible
  15-bit colors, pixel-weighted bucket splits on the widest channel,
  and every opaque pixel maps to the nearest palette entry by Euclidean
  RGB distance. The per-source-color lookup is cached so megapixel PNGs
  stay fast.
- **Import (Match to Active Palette)** is now actually wired up. The
  function existed but had no dispatch — the menu item did nothing.
  Now imports a PNG and maps every pixel into the currently-selected
  image's palette, no new palette created.

### Palette workflow
- **Cross-file palette clipboard.** Copy / Paste buttons next to
  Add/Merge/Dup/Del. The clipboard is malloc-backed (not pool-backed)
  so it survives File→Open — lift a palette out of one IMG and paste
  it into another.
- **Palette-list click now commits.** Clicking a palette in the list,
  or Up/Down with palette-nav active, now writes the choice onto the
  active sprite's `palnum`. Previously it was preview-only; switching
  sprite snapped the selection back to the new sprite's palette,
  making the click appear to "reset". Undoable, dirty-tracked.
- **Reset → Reset HSL.** Renamed with a tooltip clarifying that the
  button resets the three sliders and the palette baseline, not the
  whole palette.

### Unsaved-changes guard
- **Confirm dialog now also fires on File→Open / Open Recent / Open LOD**
  via a PendingAction queue. Previously palette / HSL edits could be
  silently thrown away by reflex-opening another IMG. Quit-time
  behavior is unchanged.
- **Import paths now mark dirty.** Append / Load LBM / Load TGA /
  Import PNG / Import PNG Match all previously left the dirty flag
  alone, so freshly-imported work could disappear on the next
  File→Open. Fixed.

### UI polish
- **Properties panel column alignment.** New `LabeledValue()` helper
  renders label + value via `SameLine(col_x)` instead of hand-padded
  format strings. Fixes the DMA ROM row that was drifting two columns
  left of the others, and survives future label changes.

### Internal
- **`mark_dirty()` helper** replaces 25 scattered `g_dirty = true`
  writes. Single entry point for future side-effects (auto-backup
  hook, dirty-bit tracing, etc).



A large round of pixel-art tools and palette workflow. Highlights:

### Tools
- **Lasso (L)** — real freehand selection. Drag a polygon, release, get
  a mask-aware pixel selection that copy/paste honors.
- **Magic Wand (W)** — tolerance slider + contiguous/global toggle.
- **Smart Eraser (E)** — formerly "Background Eraser", now a chroma-key
  remove with tolerance and an optional defringe pass that averages
  the 1px halo of blue/green-screen spill on digitized actors.
- **Clone Stamp (C)** — Alt+click to anchor, round-disc brush with
  radius slider, cyan source crosshair, white brush ring at cursor.
- **Smart Remap** — paint over a color to swap it for the selected
  index; respects a tolerance slider; target resets on mouse release.
- **Auto-Sprite Chopper** — slice marked images into a hardware grid,
  trim transparent borders, recalculate ANIX/ANIY. Suffix-overflow
  bug for >26 columns and >9 rows fixed.

### Animation Timeline
- Bottom strip with Play/Stop, FPS slider, frame thumbnails (per-image
  texture cache), drag-and-drop reorder.
- `K` plays/pauses (Premiere/DaVinci convention; Spacebar stays bound
  to toggle-mark on current image).
- `Ctrl+Left/Right` nudges the playhead frame within the timeline order.
- **Onion-skin toggle** — ghosts prev (cool blue) and next (warm orange)
  frames behind the live sprite, anipoint-aligned.

### Palette workflow
- **Saturation slider** (-100..+100%) and **Lightness slider** (-100..+100%)
  alongside the existing Hue slider. Make a yellow more yellow, fade
  toward white, drop toward black.
- **Multi-select honored.** Ctrl-click toggles a swatch's membership;
  Shift-click extends a range; HSL sliders affect only the selected
  subset (or the whole palette if nothing is selected). Hint text
  above the sliders calls out which mode you're in.
- All three sliders rebuild from a baseline snapshot each tick, so
  Reset is just "sliders to 0" and saturation +50 then -50 returns
  exactly to original.
- **Color isolation overlay** — Alt+click a swatch to dim every pixel
  that isn't that index. Click again to release.
- **Dirty indicator** on the menu bar. Amber `● filename` when the
  file has unsaved changes; plain grey filename when clean.

### Selections, paste, snap
- **Snap-to-Content.** Hold Shift while dragging a pasted floating
  selection — the paste rect snaps to the underlying sprite's
  non-transparent bbox. Threshold is screen-pixel scaled (so it feels
  the same at every zoom). Magenta cross-canvas guides flash when a
  snap is locked.

### One-shot operations (Operations menu)
- **Crop Marked to Content** — trim each marked image to its
  non-transparent bbox; anipoints adjusted so the on-screen position
  is unchanged.
- **Defringe Marked Edges** — one-pass 8-neighborhood average over
  every opaque pixel that touches a transparent neighbor. Kills the
  bluescreen halo on digitized actors.
- **Align Marked Anipoints to Selected** — set every marked image's
  anipoint to match the currently selected image's anipoint. For
  anchoring heads, hands, weapon hilts across many frames.

### Refactor
- 11 modal dialogs (Rename, LOAD2 Verify, Histogram, Auto-Chop, Bulk
  Restore, Debug Info, New IMG, Unsaved Changes, Help, About,
  Verbose Log) lifted out of `imgui_overlay_render` into named
  helpers. Render function shed 634 lines.

### Bug fixes
- Clone Stamp source no longer leaks across image switches (was
  reading pixels at coords from the previous image).
- Lasso polygon and snap bbox cache reset when switching images.
- Timeline strip height fixed (was clipping the bottom of thumbnails
  after the strip went from numeric buttons to 48px tiles).
- Onion-skin thumbnails no longer paint a checkerboard over
  transparent regions (the checker was useful only for the timeline
  strip; the texture is now honestly transparent on palette index 0).
- Palette multi-select is cleared when switching images or palettes,
  matching user intent of "this is a fresh context".
- Ctrl+click on an already-selected swatch now visibly deselects
  (the white "current color" border no longer hides the disappearance
  of the yellow "selected" border; Ctrl+click also no longer
  reassigns the active color, matching Photoshop's convention).

## [v2.2.0] and earlier

See git history. CHANGELOG started at v2.3.0.
