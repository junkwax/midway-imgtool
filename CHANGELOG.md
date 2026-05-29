# Changelog

All notable changes to midway-imgtool. Tags carry the version, e.g. `v2.3.0`.

The workflow at [.github/workflows/c-cpp.yml](.github/workflows/c-cpp.yml)
extracts the section matching the pushed tag and uses it as the GitHub
Release body. Keep new entries near the top of the file under a new
`## [vX.Y.Z]` header — anchor exactly as `## [v2.3.0]` (square brackets
included) so the extractor matches.

## [v3.8.0] — Canvas zoom, paste compositing, and UI cleanup

Feature release for a cleaner editing workspace, more predictable canvas
navigation, and Photoshop-style paste placement controls.

### Canvas And Zoom
- **Centered zoom behavior** — zoom in/out now stays centered on the canvas
  instead of drifting toward the cursor or stale pan offsets.
- **Higher zoom ceiling** — sprite zoom now reaches 128x for close pixel
  cleanup work.
- **Mouse-wheel canvas scroll** — the wheel pans vertically by default, while
  `Ctrl+wheel` still zooms from the canvas center.
- **Pan clamping** — when a sprite fits on an axis, that axis recenters instead
  of leaving the art stranded off-center.

### Paste And Transform
- **Visible floating paste preview** — pasted sprites start centered on the
  target and render at full opacity by default so resize/placement work is
  visible before commit.
- **Moveable transform box** — while Free Transform is active, dragging inside
  the box moves the floating paste; handles still scale and the top dot rotates.
- **Paste blend controls** — floating paste now exposes opacity plus Normal,
  Dissolve, Darken, Multiply, Color Burn, Linear Burn, Lighten, Screen, Color
  Dodge, Overlay, Soft Light, Hard Light, Difference, and Exclusion modes.
- **Overwrite-by-default commit** — Normal 100% paste overwrites opaque source
  pixels exactly; palette index 0 remains transparent.

### UI Layout
- **Two-column toolbar** — grey editing tools now sit in the left toolbar column,
  while blue IMGTOOL actions (mark, points, hitbox, resize, zoom, undo/redo)
  sit in the right column.
- **Right-panel ordering** — `Color` is now always open directly beneath
  `Palettes`, followed by `Anipts` for quick alignment edits.
- **External rotate buttons** — sprite rotate buttons sit beside the sprite in
  their original side-by-side layout instead of covering the artwork.

## [v3.7.0] — Sprite sheet imports and actor likeness tools

Feature release for faster sprite cleanup, palette-safe paste/import workflows,
and deterministic actor-likeness transfer between marked sprites.

### Sprite Editing
- **Marked likeness transfer** — one marked source sprite can now apply its
  actor/costume likeness to the selected target while preserving the target
  pose, silhouette, and digitized shading.
- **Material-aware likeness pass** — transfer now separates head/skin, blue
  robe, lower cloth, dark edge, and gold accent regions, adds source
  microtexture, and cleans bad green/olive palette outliers.
- **Softer hard-stroke removal** — edge cleanup now works from the true outer
  edge, removes the harsh 1px stroke, and repaints saved edge pixels from the
  nearest strong inward color instead of chewing into the sprite.
- **Blank-area marquee start** — dragging from transparent canvas space now
  starts a selection cutout workflow automatically.

### Canvas And Paste
- **Sprite zoom controls** — toolbar buttons, View menu items, mouse wheel,
  `Ctrl+=`, `Ctrl+-`, and `Ctrl+0` now zoom in/out or fit the current sprite.
- **Palette-matched paste** — copied pixels carry source palette metadata and
  remap to the destination palette when pasted into a different palette.
- **External rotate handle** — pasted/free-transform sprites now rotate from an
  outside-corner handle so the controls do not cover the art.

### Import And File Dialog
- **Sprite sheet import** — PNG/JPG/TGA sheets can be split into individual
  palette-matched frames using island detection, padding, and tight crop
  options.
- **Sprite sheet debug CLI** — `--debug-spritesheet` writes detection artifacts
  and reports accepted frames for tuning sheet import behavior.
- **GIF/PNG preview thumbnails** — the file picker now builds previews from
  highlighted image files directly, not only after clicking the filename.
- **Multi-file image imports** — PNG, matched-PNG, GIF, and sprite sheet import
  dialogs support selecting multiple files in one pass.

### Tests And Repo Hygiene
- **Sprite sheet regression test** — CMake now builds and registers a
  `sprite_sheet_import_test` target for the detector.
- **Ignored scratch outputs** — local prototype output and test scratch folders
  are ignored by git.

## [v3.6.1] — Palette readout cleanup

Patch release for making the new palette usage information easier to read.

### Palette Editing
- **Readable 8bpp palette bar** — the bottom palette area now reserves a clear
  header band for selected color, RGB, usage counts, free colors, low-use
  counts, and marker legend instead of drawing text over the swatches.
- **Full 256-color grid fit** — the palette grid now adapts column count and
  swatch size to show full 8bpp palettes cleanly across window sizes.

## [v3.6.0] — Fatality workspace and palette forensics

Workflow release for editing MK2 fatality source, finding spare palette
slots, and building multi-actor World View timing drafts.

### MK2 Fatality Lab
- **Source-backed fatality browser** — new helpers parse MK2 source files for
  fatality command blocks, input combo tables, and animation labels while
  preserving the original assembly line buffers for save.
- **Fatality staging workspace** — staged fatality assets can open attacker and
  victim IMG sets into marked World View lanes for side-by-side timing work.

### World View
- **Marked-lane sequence editor** — each marked IMG tab lane can now duplicate
  or remove sequence entries, edit per-entry delay ticks, and scrub directly
  to an entry by clicking its thumbnail.
- **Local anipoint deltas** — sequence entries carry `dAX` / `dAY` offsets, so
  repeated uses of the same sprite can have different preview anipoints
  without editing the sprite record itself.
- **Visibility gates** — `Show@` hides an entry until the selected global tick,
  useful for actor/object reveals partway through a synced fatality preview.
- **Richer ASM draft export** — World View ASM export now includes hidden-frame
  placeholders and aligned `*_local_anipts` tables for local anipoint data.

### Palette Editing
- **Palette usage indicators** — the active palette strip marks unused nonzero
  colors and low-use colors across sprites that reference the palette, with
  tooltips showing counts and nearest used-color candidates.
- **Similar-region remap** — selected pixels from tools like Magic Wand can be
  remapped across matching same-palette sprites, helping separate shared
  regions such as hair and pants without hand-editing every frame.
- **Palette cleanup on sprite delete** — deleting sprites now removes palettes
  that are no longer referenced by any remaining sprite and remaps later
  palette indices safely.
- **Palette merge preview** — marked-palette merges can be inspected before
  applying, including source-to-target color mapping and drift.

### Sprite Editing
- **Hard-stroke remover** — marked sprites can automatically remove thin
  1-2px high-contrast outline/matte strokes around transparent edges.
- **Copy/Cut to new sprite** — selected regions can be copied or cut directly
  into newly created sprites.
- **Safer secondary anipoints** — imports and resize/mirror helpers now use the
  proper unused secondary-anipoint sentinel instead of treating zero as
  missing data.
- **Subframe name shortening** — auto-chopped child names preserve suffix room
  more reliably when parent names are near the IMG name-length limit.

## [v3.5.0] — Palette safety and subframe polish

Follow-up polish for sprite hierarchy display and safer palette operations.

### Image List
- **Parent row alignment** — real parent sprites stay aligned with normal image
  rows and keep the image icon while subframes indent underneath.
- **Subframe hierarchy icon** — child pieces use a compact hierarchy glyph
  instead of the normal image icon.

### Palette Editing
- **Palette undo coverage** — palette add, delete, duplicate, paste, import,
  rename, assignment, merge, cleanup, downscale, HSL/RGB edits, and #0 remap
  tools now push real undo actions that restore palette records and affected
  sprite pixels together.
- **Palette delete remap fix** — deleting a palette now preserves image palette
  assignments by remapping affected images to the adjacent surviving palette
  and decrementing later indices.
- **Palette merge quality check** — merging marked palettes now previews color
  drift, transparency drift, invalid source indices, and LOAD2 PPP risk before
  applying the merge.
- **Safer palette merge remap** — nonzero sprite pixels prefer nonzero target
  palette slots to avoid accidental transparent holes.

### Sprite Editing
- **File dialog keyboard focus** — Up/Down and timeline arrow shortcuts no
  longer leak through while a modal file dialog or popup is open.
- **Rotate icon orientation** — canvas rotate glyphs now visually match the
  clockwise/counterclockwise actions more closely.
- **Horizontal subframe defaults** — Break into Subframes defaults to a taller
  slice height, matching the horizontal split pattern seen in MK2 character
  data.
- **Subframe split preview** — Break into Subframes now overlays the generated
  piece bounds on the canvas and reports a live LOAD2 ZCOM bit estimate while
  tuning grid size and trimming.

## [v3.4.0] — Sprite import, subframes, and palette inheritance

Workflow release for batch-importing art, organizing chopped sprites, and
moving palette work between related libraries.

### Sprite Editing
- **Multi-file PNG/GIF import** — the custom file dialog now supports
  Ctrl-click and Shift-click selection for PNG/GIF import modes.
- **Canvas rotate controls** — opened sprites show small clockwise and
  counterclockwise rotate buttons in the top-right of the canvas.
- **Bulk rename** — marked sprites can be renamed sequentially, with optional
  tailing of existing names.
- **Break into Subframes** — marked sprites, or the selected sprite when none
  are marked, can be split into Midway-style A/B/C pieces with anipoints
  recalculated for in-game alignment.

### Image List
- **Visual subframe folders** — sprites named like `BASE1A`, `BASE1B`, and
  `BASE1C` are grouped as collapsible visual folders without writing hierarchy
  data back into the IMG file.
- **Image sorting** — the image panel can sort by original order, name, or
  sprite size.

### Palette Editing
- **Palette inheritance** — selected palettes can inherit colors from a marked
  source palette, remapping sprites to the nearest matching source colors.
- **Duplicate palette merge** — byte-identical duplicate palettes can be merged
  into the first matching palette while remapping sprites safely.
- **Single palette mark button** — palette controls now include a selected
  palette mark toggle alongside mark-all controls.

## [v3.3.0] — Palette bpp downscale and transform tools

Focused art-editing release for reducing palette budgets and tightening
sprite/timeline alignment workflows.

### Palette Editing
- **Palette downscale preview** — selected palettes can be downscaled to
  8, 7, 6, 5, or 4 bpp with a side-by-side sprite preview before applying.
- **Smart reduction logic** — used colors are preserved exactly when they fit
  the target bpp; otherwise the reducer uses weighted color merging and remaps
  every sprite that references the palette.

### Sprite Editing
- **Lossless sprite transforms** — selected sprites can rotate or flip while
  keeping pixels, anipoints, and hitbox state aligned.

### Animation Timeline
- **Composite pair controls** — paired timeline sprites can stay grouped during
  playback/scrubbing, with lock toggles and drag-to-adjust anipoint alignment
  in the composite preview.

## [v3.2.0] — Timeline composite preview

Focused animation-alignment workflow release.

### Animation Timeline
- **Anipoint composite preview** — Ctrl-click two frames in the animation
  timeline to show a read-only canvas preview of both sprites combined by their
  primary animation points, with secondary animation points and per-frame
  outlines drawn for registration checks.

### Sprite Alignment
- **Reverse-facing anipoint mirror** — marked sprites can mirror their primary
  and active secondary X anipoints as `width - x`, matching World View's
  view-only reverse-facing comparison.

### Palette Editing
- **Transparent color relocation** — palette index 0's RGB value can be copied
  into a safe opaque slot, with optional remap tools for the current selection
  or sprite so formerly transparent pixels can be painted visibly.

## [v3.1.0] — Sprite resize, IMG tabs, and World View pairing

Workflow release for editing multiple IMG libraries side by side and comparing
animation alignment across tabs.

### Sprite Editing
- **Resize Sprite** — added a resize dialog with width/height and percent
  scaling controls, optional aspect-ratio locking, nearest-neighbor palette-ID
  preservation, quality remap mode, and a quality-plus-smallest-bytes mode that
  trims transparent bounds.
- **Anim-point batch push** — selected Anim Point sliders now nudge by one with
  Left/Right, and a new push button copies the selected sprite's anim points to
  regex-matched same-name sprites across open tabs, e.g. `^..HEADHOLE1$`.
- **Geometry-safe undo** — sprite-resize operations now capture full image
  state, including dimensions, anipoints, palette metadata, and pixel data, so
  geometry edits undo cleanly without disturbing normal paint undo.
- **LOAD2 baseline dimensions** — load-time baseline width/height are tracked
  with the pristine pixel snapshot so resized sprites are reported accurately by
  the packing verifier and drift overlay.

### Multi-IMG Tabs
- **IMG tabs** — File > Open now opens each IMG/LOD into its own tab, reusing
  the initial empty tab when possible and activating an already-open tab when
  the same path is opened again.
- **Per-tab dirty state** — unsaved-change prompts now operate per open
  document, including tab close and quit flows, while the editor keeps the
  legacy `g_doc` access pattern through a new document container.
- **Native ImGui tab strip** — the document strip uses ImGui tabs and trailing
  tab buttons while avoiding the earlier selection tug-of-war by switching
  documents only on tab activation.

### World View
- **Marked-tab playback** — World View can play marked animations from the
  active tab together with the first other marked tab, anchored to the same
  world origin for anipoint comparison.
- **View-only mirroring** — mirror toggles for the active and paired World View
  sprites let opposite-facing animations be compared without modifying image
  pixels or anipoints.
- **World View tab controls** — Onion, Marked, Mirror Active, and Mirror Paired
  controls are exposed as separate trailing tab buttons instead of living in the
  View dropdown.

## [v3.0.0] — MK3 IMG support

Major compatibility release for Mortal Kombat 3 / Ultimate MK3-era IMG
workflows.

### IMG Compatibility
- **Long project paths** — expanded the document path buffer and removed
  hardcoded 64-byte path copies, so MK3 assets in normal source-tree paths
  open directly from the GUI and CLI.
- **MK3 WIMP 0x0634 libraries** — verified `MKPOWER.IMG` loads, exports,
  and runs through the LOAD2 packing verifier from its original project path.
- **LOD `PPP> 0` handling** — `.LOD` parsing now distinguishes an explicit
  `PPP> 0` from a missing `PPP>` directive, preserving MK3 manifests that
  deliberately use palette/native bpp.

## [v2.10.0] — Sprite workflow tools and CLI packaging

Focused workflow release for faster sprite extraction and scriptable build
pipelines. No IMG file-format changes.

### Sprite Editing
- **Cut to New Sprite** — `Ctrl+Shift+X` cuts the current marquee/lasso/wand
  selection into a freshly-created sprite, preserving palette and anipoint
  alignment from the source frame.
- **Paste as New Sprite** — `Ctrl+Shift+V` creates a new sprite directly from
  the pixel clipboard, using tight transparent bounds and unique derived
  names.
- **Duplicate and Trim buttons** — the Images panel now exposes one-click
  `Dup`, `Trim`, and `Paste+` controls. Trim removes dead transparent border
  space from the selected sprite without dirtying already-tight frames.

### Command Line Interface
- **Console CLI target** — builds now produce `imgtool-cli`, a console
  subsystem binary for scripts and CI. Release artifacts include it on
  Windows, Linux, and macOS.
- **Stricter headless commands** — CLI commands now reject unknown options,
  report missing inputs and failed IMG loads, and print a full command
  reference from `--help`.
- **LOD automation polish** — `--build-lod` accepts `--override-dir=DIR`,
  validates that all referenced IMGs loaded, and handles relative output paths
  safely after internal directory changes.

## [v2.9.0] — Headless CLI, GIF import, palette workflows

Automation and art-cleanup release. Adds a headless command-line mode
for build pipelines, GIF and palette import/export, new paint workflows
for variant-heavy sprites, and a round of release-hardening fixes. No
IMG file-format changes.

### Command Line Interface
- **Open on launch** — `imgtool [file]` opens an `.img`, `.png`,
  `.tga`, `.lbm`, or `.gif` file immediately on application start.
- **Headless data exports** — `--export-tbl <input.img> <output.tbl>`
  writes MK2/MK3 assembly tables with `--mk3`, `--include-pal`,
  `--padding`, `--align-16`, `--dual-bank`, `--bank=N`, and
  `--base=HEX`; `--export-irw <input.img> <output.irw>` writes raw
  ROM layouts with `--bpp=N`, `--no-align`, and `--base=HEX`;
  `--export-anilst <input.img> <output.asm>` writes assembly animation
  lists.
- **Headless format conversions** — `--export-png <input.img>
  <output_dir>` unpacks sprites to individual PNG files, while
  `--build-tga <input.img> <output.tga>` packs all marked sprites into a
  single TGA spritesheet.
- **LOD and validation tools** — `--build-lod <manifest.lod>
  <output.img>` builds an IMG directly from a `.lod` manifest, and
  `--verify-load2 <input.img>` runs the LOAD2 packing verifier with
  non-zero exit status on breaking alignment issues.
- **Usage dialog** — `--help`, `-h`, and `/?` show a native usage
  message for GUI launches.

### Import / Export
- **GIF import** — File > Import > GIF File loads single-frame or
  multi-frame GIFs into IMG sprites, quantizes all imported frames into
  a shared 15-bit palette, and exposes blend mode plus opacity controls
  for compositing animation frames before import.
- **Palette import/export** — raw Midway `.PAL` data and Adobe `.ACT`
  RGB palettes can be imported as new palettes or exported from the
  active palette. Palette dialogs remember their own directory and
  default export names from the selected palette.
- **GIF-aware path dispatch** — drag/drop and launch-by-path now route
  `.gif` files through the same import path as the file dialog.
- **File dialog preview** — the existing PNG/TGA thumbnail pane now
  previews GIFs via `stb_image`.

### Paint And Selection Tools
- **Paint Bucket tool** — new `G` toolbar mode fills the clicked color
  with the current swatch. Tolerance is palette-index based, with a
  contiguous flood mode or global replace mode.
- **Variant Paint tool** — new `V` toolbar mode paints only the current
  image's palette appearance while preserving the original color in all
  other palettes through automatically-created shadow slots. The same
  logic can be applied to the current selection from the Operations menu
  or palette properties.
- **Overlay frame extraction** — selected opaque pixels can be split or
  copied into a new transparent overlay frame that preserves the source
  sprite's geometry, anipoints, palette, flags, and alternate-palette
  metadata. Split mode clears the source pixels and participates in
  pixel undo.

### Palette Workflow
- **Clean Up Palette** replaces the older unused-color delete action
  with a cleanup pass that preserves transparent index 0, removes unused
  entries, sorts active colors into visible brightness ramps, and remaps
  every sprite using that palette.
- **Clean Copy Palette** creates a new sorted/trimmed palette without
  remapping existing sprites, useful when comparing a cleaned ramp
  against the original palette.
- **Palette adjustment commits** — RGB/HSL edits now commit their
  preview baseline when changing image, palette, or swatch selection, so
  subsequent resets and multi-select changes do not resurrect stale
  preview data. Applying a palette also clears stale SDL swatches past
  `pal->numc`.

### Hardening
- **CodeQL fixes** — corrected a real `src_filename` overflow in chopped
  image creation, widened several allocation/mask-size multiplications
  to `size_t`, and escaped a literal percent sign in an ImGui tooltip
  format string.
- **GIF decoder fix** — patched the bundled `stb_image` GIF loader so
  the two-frame-back pointer references the correct output layer during
  animated GIF decoding.
- **Repository hygiene** — stopped tracking the local `last.md` scratch
  file; the existing `*.md` ignore now covers it.

## [v2.8.0] — Editor polish, parser correctness, layout-independent shortcuts

Twenty-commit round focused on tightening everything Phase 7 introduced.
No IMG file-format changes. Two real parser bugs in the MK2 editor were
caught and fixed by a new round-trip test; the editor gains undo / redo
/ reload / search / auto-character-pick / unsaved-quit guard; pencil
becomes a fully discoverable tool; menus and Help are aligned with what
the code actually does.

### MK2 strike-table editor
- **Undo / Redo** — per-record snapshot stack inside `mk2::Document`
  (bounded to 200 entries, drag-coalescing). Ctrl+Z and Ctrl+Shift+Z
  fire only while the panel has focus so they don't hijack the main
  canvas's pixel-undo path. Toolbar buttons mirror the keyboard.
- **Reload button** — re-reads MKSTK.ASM from the previously-resolved
  source path (not the input box, so a stray edit there can't redirect
  the reload). Drops in-memory edits and undo/redo history.
- **Browse… button + last-dir memory** — native Win32 file picker
  filtered to `.ASM`. Picked directory persists per-category alongside
  the existing IMG/PNG/TGA/LBM dirs. Defaults to empty path (was
  hardcoded to one developer's machine).
- **Move filter** — case-insensitive substring search box in the
  middle pane so finding a move across ~200 records doesn't require
  scrolling.
- **Auto-pick character from IMG filename** — prefix table maps the
  loaded sprite's filename (CAGE / HATHED / KANG / NINJAS / RAID /
  TSUNG / JAXPRO / NUJAX / MKJXARMS / KAT / BIGGORO / GOROSIZE) to the
  two-letter character code MKSTK.ASM uses. Silent no-op when nothing
  matches.
- **Unsaved-changes guard on quit** — separate modal from the IMG dirty
  flow since MK2 writes a different file (MKSTK.ASM).
- **Stale-status auto-clear** — "Loaded N moves" / "Saved" disappears
  on the first edit after a clean baseline; error messages stay sticky
  until the next action resolves them.

### MK2 parser correctness
- **Case-insensitive `.word` / `.long` regex** — MKSTK.ASM has 11
  uppercase `.WORD` lines (e.g. on `stk_jxuppercut`). Previous parser
  silently dropped them, shifted subsequent fields by a slot, and lost
  the trailing `sound` field on 43 records.
- **Stacked `stk_*` labels** — multiple consecutive `stk_*` labels with
  no `.word`/`.long` between them (e.g. `stk_sazap1` immediately
  followed by `stk_sazap4`) are aliases for the same strike record.
  Previously the first label allocated empty records and consumed the
  data for the second. Now tracks `pending_aliases`: every
  freshly-labeled record that hasn't seen field data yet receives the
  same field references when data finally arrives.
- **`mk2_roundtrip_test`** — new CMake test target (guarded by
  `-DMK2_MKSTK_PATH=…`). Loads, verifies every record has all 8 fields
  tracked, then does load → save A → load A → save B and asserts
  A == B byte for byte. Both parser bugs above were caught by this
  test on first run.

### Pencil tool
- **Explicit `ActiveTool::Pencil` mode** with toolbar button and the
  Photoshop `P` shortcut. Toolbar Open/Save icons removed (File menu
  and Ctrl+O / Ctrl+S unchanged).
- **`[` / `]` shrink / grow brush radius** (1..16). Photoshop convention.
- **Cursor preview tracks the selected palette color** — brush ring for
  radius > 1, offset crosshair for radius = 1. Both have a black halo so
  they stay legible against same-colored pixels.
- **Per-stroke pixel undo / redo** — replaces the previous single-slot
  swap (which silently overwrote itself on each new stroke and had no
  redo). Up to 32 strokes back, with branch-on-edit. Covers Pencil,
  Clone Stamp, Smart Eraser, Smart Remap, Shift+click Flood Fill.
- **Undo/Redo unification** — toolbar buttons, Edit menu items, and the
  Ctrl+Z / Ctrl+Y shortcuts all route through `DoUndo` / `DoRedo` so the
  controls correctly light up after a paint stroke.

### Menu and shortcut audit
- **Wired previously-vestigial palette shortcuts as real:** `[` Set
  Palette for Marked, `]` Set for Image, `*` Merge Marked (typed
  character — works on any keyboard layout), `Shift+R` Rename Palette,
  `Del` Delete Palette. `[` and `]` yield to Pencil brush size when
  Pencil is active.
- **Wired four previously-vestigial Image shortcuts as real:** `Ctrl+R`
  Rename Image, `Ctrl+P` Toggle Point Table, `Alt+PgUp` / `Alt+PgDn`
  Move Up / Down in list.
- **Stripped misleading hints** that advertised keys without bindings:
  `a` Append, `i` Set ID from 2nd List, `Alt+C` Clear Extra Data,
  `Left`/`Right` Jump Prev/Next Marked, `Tab` World View.
- **Help modal rewritten** to match what's actually bound. The old
  text mixed DOS-tool keys (`d/D/F11/F12` zoom, `t` true-palette,
  `Ctrl+Y`/`Ctrl+Z` for anim-point clears that now collide with
  Redo/Undo, Eyedropper as E when it's actually I, `'`/`/` palette
  nav, `i` ID-from-2nd-list, `Alt+U/D/L/R` and `Ctrl+U/D/L/R` anipoint
  moves) with one inverted (M/m which sets/clears all marks — the
  hint was backward). New Help also has a dedicated MK2 Strike-Table
  Editor section.
- **README keys table synced** to the same ground truth as Help.

### Canvas polish
- **World View hides overlays** — anipoint crosshair, IMG hitbox, and
  MK2 hitbox box all suppress while World View is on. World View
  re-anchors the sprite relative to its anipoint, so on-sprite markers
  would otherwise float detached from the playfield rectangle.
- **About modal** uses a 2-column table for the key/value rows
  (Version / Built / Commit / Dear ImGui / SDL2). ImGui's proportional
  font made the previous space-padded labels drift.

### New IMG creation
- **New-image dialog** prompts for width and height before allocating
  (1..1024 clamped). Replaces the hardcoded 32x32. Defaults persist
  between opens. Enter commits, Esc cancels.
- **File → New** bootstrap still uses 32x32 to keep the path identical.

### Source hygiene
- **Trademarked character names stripped from source** — the MK2
  auto-pick map now uses only the two-letter codes MKSTK.ASM itself
  defines (no fighter names in code or comments). README, About
  dialog, and CHANGELOG keep factual references under nominative fair
  use.

## [v2.7.0] — Phase 7: MK2 hitbox editor, Pencil tool, New IMG bootstrap

Phase 7 wrap-up. Adds the long-planned MK2 strike-table editor (parses
and writes `MKSTK.ASM` directly, with a magenta on-canvas hitbox
overlay), a proper Pencil tool with the Adobe `P` shortcut, and makes
File → New leave you in a usable starting state. No IMG file-format
changes.

### MK2 strike-table editor
- New **Tools → MK2 Hitboxes (MKSTK.ASM)...** panel. Parses
  `mk2-main/src/MKSTK.ASM` directly as the source of truth — no
  `stk.bin` or `MKSTK.LST` dependency. Three-pane layout:
  character → move → fields.
- Edit fields by number (x/y/w/h, score) or by raw token
  (`strike_routine`, `sound` — preserves symbolic identifiers like
  `sf_squeeze`). `damage` is split into hit and block bytes.
- **Save** rewrites `MKSTK.ASM` in place, preserving comments,
  indentation, and symbolic literals. Hex literals stay hex,
  decimal stays decimal.
- **Magenta hitbox overlay** on the main canvas shows the selected
  move's collision box at `(x_offset, y_offset, x_size, y_size)`
  with drag-to-resize corner handles that write straight back to the
  `.ASM` line buffer. Overlay stays visible while the MK2 panel has
  focus. The IMG-embedded hitbox overlay auto-hides when an MK2
  move is selected so the two systems don't pile on top of each
  other.
- Implemented in [platform/mk2_hitbox.h](platform/mk2_hitbox.h) and
  [platform/mk2_hitbox.cpp](platform/mk2_hitbox.cpp).

### Pencil tool
- New explicit **Pencil tool** on the toolbar with the `P` shortcut
  (Photoshop convention). Single click paints one pixel at the
  current palette index; drag paints a continuous line. Shift+click
  flood-fills (carried over from the previous no-tool paint mode).
- Same paint code path as the v2.6 "no active tool" behavior, just
  promoted to a real `ActiveTool::Pencil` mode so it's discoverable
  and the toolbar highlight tells you which tool is active.

### New IMG from scratch
- **File → New** now bootstraps a usable starting document — one
  default 256-color palette and one 32×32 blank image — instead of
  leaving you with an empty void where palette and image lists were
  both empty.
- Undo and dirty state are reset after the bootstrap so the new doc
  starts pristine.

### Palette additions polish
- **Add Palette** / **Duplicate Palette** are now reachable from the
  Palette menu and the right-click context menu (previously only on
  the button row).
- Both now push undo, auto-select the new palette, and commit it onto
  the active image. New palettes get a unique `PAL%d` default name
  instead of an empty name.

### Toolbar cleanup
- Removed the **Open** and **Save** icons from the left toolbar.
  Loading and saving stays accessible via the File menu and the
  `Ctrl+O` / `Ctrl+S` shortcuts. Lets the toolbar focus on actual
  paint/selection tools.

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
