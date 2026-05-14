# Changelog

All notable changes to midway-imgtool. Tags carry the version, e.g. `v2.3.0`.

The workflow at [.github/workflows/c-cpp.yml](.github/workflows/c-cpp.yml)
extracts the section matching the pushed tag and uses it as the GitHub
Release body. Keep new entries near the top of the file under a new
`## [vX.Y.Z]` header — anchor exactly as `## [v2.3.0]` (square brackets
included) so the extractor matches.

## [v2.6.0] — Drag-drop, palette highlight, Eyedropper, multi-doc plumbing

Workflow polish round plus the structural refactor that unlocks multiple
open IMGs in a future release. No file format changes; everything saves
and loads bit-identical to v2.5.0.

### Drag and drop file open
- **Drop `.img` onto the window** to open it (routes through the same
  unsaved-changes guard as File → Open). Drop `.png`, `.tga`, or `.lbm`
  to import into the active document.
- **Empty workspace bootstrap** — dropping a PNG/TGA/LBM with no IMG
  open silently creates a fresh IMG first, giving the import a palette
  context to land in. No more "open something first" friction.

### Palette → canvas highlight
- Multi-selecting palette swatches (Ctrl/Shift-click in the swatch grid)
  now **dims every pixel on the canvas that isn't one of the selected
  colors**, leaving the matching pixels at full brightness. Answers
  "where does this color live in the sprite?" at a glance.
- Single-color Alt-click isolation still works the same way; the new
  multi-select path is the same render code, generalized.
- Dim wash changed from pure black to muted indigo so it stays visible
  against dark fighter sprites where the old wash blended in.

### Eyedropper tool
- New explicit **Eyedropper tool** on the toolbar with the `I` shortcut
  (Photoshop convention). Left-click in this mode picks the underlying
  palette color and highlights it in the swatch grid.
- Right-click eyedropper still works in any tool mode (unchanged).

### Anipoint crosshair
- Anipoints are now drawn as **DOS-style + crosshairs** instead of the
  v2.5 solid circles. Primary anipoint is white, secondary is cyan,
  hover state brightens to yellow. Matches the registration-mark idiom
  of the original 1992 tool.
- When onion-skin is on, the **previous frame's anipoint** shows
  underneath as a dim gray crosshair — the same registration reference
  the DOS tool drew.
- Bug fix: anipoint drag no longer paints pencil strokes underneath
  the moving cursor.
- Bug fix: phantom diagonal line drawn from anipoint 1 to off-screen
  when the secondary anipoint sentinel was (-1, -1) — was casting the
  signed -1 to unsigned 0xFFFF.

### Toolbar polish
- **Icon refresh** — Lasso, Magic Wand, Smart Eraser, Clone Stamp,
  Smart Remap, Eyedropper, and Hitbox toggles now use Material Symbols
  glyphs that actually match their tool (lasso loop, sparkle wand,
  eraser block, control-point duplicate, palette, slanted dropper,
  activity zone).
- Icons vertically centered in their buttons (Material Symbols' empty
  descender space was biasing them upward).

### Internal: document refactor
- Per-document state (`img_p`, `pal_p`, image/palette counts, selection
  indices, file paths, sequence/script memory) is now a `Document`
  struct in [platform/document.h](platform/document.h) reached through
  a single `g_doc` pointer. ~550 callsites across 8 files renamed from
  bare globals to `g_doc->X`.
- Sets the stage for multi-tab editing — Phase 3 is just swapping
  `g_doc` between entries in a tabs container.

### Cleanup
- Em-dashes and other non-ASCII glyphs in tooltips were rendering as
  `?` in the default ImGui font; replaced with ASCII equivalents.
- Removed obsolete `/alternatename` linker pragmas that aliased C-side
  `_fpath_s` to long-gone ASM symbols.

## [v2.5.0] — Paste pipeline overhaul, Free Transform, file dialog

Major round centered on the paste workflow plus a much-improved file
dialog. The headline: pasting is now an Adobe-style scale-and-place
operation, and the file dialog grew thumbnails, sort options, and
double-click-to-open.

### Free Transform & paste pipeline
- **Ctrl+T Free Transform** — eight handles (4 corners + 4 edge midpoints)
  scale the floating paste. Aspect-locked by default; click the chain
  icon to unlock, or hold Shift to invert the lock for one drag.
  Enter / Ctrl+T commits (nearest-neighbor resample of the clipboard);
  Esc reverts.
- **Paste auto-enters transform** so every Ctrl+V drops you straight
  into resize mode. Clicking outside the rect is a one-shot
  commit-transform-and-apply-paste, matching Photoshop.
- **Tight-bbox crop on cut / copy** — the clipboard is automatically
  trimmed to its non-transparent content. A small motif inside a large
  marquee no longer pastes off-center.
- **Scale-to-fit on paste** — if the clipboard is bigger than the
  target sprite, nearest-neighbor downscale to fit (preserving aspect
  ratio). Toast tells the user what happened.
- **Center snap + passive centering guide** — drag the floating paste
  near the sprite's center to snap (Shift held), and a magenta guide
  always lights up when the paste rect's center is exactly on the
  sprite's center axis even without Shift.

### Adobe-standard shortcuts
- `Ctrl+A` Select All, `Ctrl+D` Deselect, `Ctrl+Shift+I` Invert Selection
- `Ctrl+J` Duplicate (image or floating paste), `Ctrl+E` Merge Down
- `Ctrl+T` Free Transform, `Shift+Del` Delete image (replaces old Ctrl+D)
- All five new entries land in the Edit menu with the correct accelerator
  labels and proper enabled/disabled state.

### File dialog
- **Preview thumbnails** for highlighted PNG and TGA files (192px,
  nearest-neighbor scaled, cached by full path). IMG and LBM previews
  are a follow-up — their loaders would need refactoring to write into
  a sandbox buffer first.
- **Sort by Name / Date / Size** with asc/desc toggle. Directories
  always sort first regardless of key. Persistent across opens.
- **Double-click to open** matches native OS file dialogs.
- **Per-category last-dir** memory — IMG / PNG / TGA / LBM each
  remember their own folder. Switching from "Save IMG" to "Import PNG"
  now lands in the PNG folder, not the IMG folder.

### Timeline
- **Ping-Pong playback** — new checkbox next to Onion. Plays forward
  then reverse and loops (e.g. frames 1→7→1→7→…) instead of wrapping.

### Quality of life
- **Image list auto-scrolls** to keep the selected sprite in view when
  navigating with Up/Down past the viewport edge.
- **Palette-list click commits** — clicking a palette now writes it
  onto the active sprite's palnum, so the choice sticks across sprite
  switches. Previously it was preview-only.
- **About dialog** shows the version, build date, git commit, ImGui
  version, and SDL2 version. Window title now reads `IMGTOOL v2.5.0`.

### Cleanup
- `IT/it.c` → `platform/main.cpp`, converted to C++. Dead code stripped:
  `mempool_*` externs, unused SIGINT handler, six write-only env-var
  buffers, two unused float conversion helpers.
- `IT/` directory removed entirely; `vcpkg.json` removed (unused by
  the build); `it.hlp` removed (the `h` key uses the embedded help text).
- macOS `_NSGetExecutablePath` properly used instead of the Linux-only
  `/proc/self/exe` fallback.

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
