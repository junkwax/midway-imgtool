# Midway Imgtool Refactoring Plan

`midway-imgtool` is a successful modernization of a 1992 DOS codebase. The main
maintainability problem is `platform/imgui_overlay.cpp` — a ~22,900-line
"super-module" holding nearly all UI rendering, dialog state, and image
manipulation logic. This document describes the strategy for breaking it up and
tracks slice-by-slice progress.

## Current checkpoint - June 9, 2026

Branch: `refactor/overlay-split`.
Last committed refactor slice:
`340be75 refactor(overlay): extract pure HSL palette adjustment into color_ops`.

The `color_ops` refactor was patch-staged out of the mixed working tree and
committed on its own, verified by a clean Release build of the refactor-only
state. The overlay still owns undo, dirty state, active palette selection, and
`SDL_Color g_palette[]`; `color_ops` only transforms raw 15-bit palette buffers.

The working tree remains intentionally dirty with **feature work only** (still
being iterated, deliberately uncommitted) in `platform/imgui_overlay.cpp`:

- Added `Operations -> Bulk Resize Marked...`.
- Changed Break into Subframes to offer best horizontal and best vertical
  splits, each scanning for the best LOAD2/ZCOM savings and ignoring cuts that
  would leave either side at 5 px or less.
- Made primary/secondary anipoint edits propagate by numbered animation
  sequence, including inferred subframes. Example: changing `JCWALK1` by `-10`
  on an axis applies the same delta to `JCWALK2`, etc.
- Fixed mirrored World View movement so mirrored frames drag/invert on the
  expected axis.
- Added app session restore: on shutdown, disk-backed open IMG tabs are written
  to `<exe_dir>/imgtool_session.txt`; on next launch they reopen and the prior
  active tab is reselected. Missing/unreadable files are skipped. Recent Files
  are preserved during automatic restore.
- Removed the duplicated RGB sliders from `Color Tools`; RGB editing now lives
  only under the quick `Color` section.

The full mixed tree (features + color_ops) was previously verified green:
`build.ps1` + `ctest` passed for both the session-restore feature state and the
color_ops-extracted state.

Suggested next steps:

1. Land the feature work as its own commit(s) once it stops moving — it is
   independent of the refactor now that `color_ops` is committed.
2. Do not start Phase B globals surgery until the feature work is committed or
   deliberately shelved (avoid mixing a large global migration with live
   feature edits in the same file).
3. To keep Phase A momentum without touching shared UI state, extract the next
   pure helpers — e.g. `EdgeColorStrongVariant` / `FindInwardEdgeReplacement`
   edge-replacement helpers into `color_ops` (or a sibling `image_ops`).

Environment notes (noisy but nonblocking): git commands may warn about
`C:\Users\xbx\.config\git\ignore` permission; `build.ps1` may print
`'vswhere.exe' is not recognized...` after its success banner, yet binaries are
produced and tests pass.

## Guiding principles (learned the hard way)

A previous automated attempt failed by trying to extract all ~860 file-scope
`static` globals at once with a regex/text script. It misparsed C++ (`const int
X = 8;` vs. a default argument `f(int n = 8)`), broke the whole translation unit
simultaneously, never compiled in between, and never committed — leaving a
non-building tree. The replacement strategy:

1. **The compiler is the source of truth.** Never extract symbols with regex or
   text-munging scripts; move code and let the build verify it.
2. **Reverse the risk order — easy and safe first.** Extract self-contained,
   global-free *pure* functions before touching shared global state.
3. **One slice per commit, build green every time.** A crash or bad slice then
   costs one slice, never the whole effort. Work on a dedicated branch.
4. **No behavior changes during a split** — pure code motion only.

## Slice checklist

Each slice: pick a cohesive group → move to new `.cpp`/`.h` → un-`static` +
declare in header → include from `imgui_overlay.cpp` → add to `CMakeLists.txt`
→ **build green → commit**.

### Phase A — Pure algorithms (lowest risk, do first)

- [x] **`palette_math`** — 15-bit palette-word helpers (`palette_word_at`,
      `palette_word_distance_sq`, `nearest_palette_index_for_word`,
      `pal_word_or_black`, `rgb_to_word15`), plus the distance/nearest-slot
      helpers (`PaletteColorDistance5`, `PaletteColorDistance5W`,
      `FindNearestPaletteSlot`, `FindNearestMergedSlot`).
- [x] **`sprite_resize_ops`** — full-sprite resize resampling helpers
      (`ResizeSpritePixelsNearest`, `ResizeSpritePixelsQuality`) with the UI
      palette fallback passed in explicitly; dialog/undo/hitbox state remains
      in the overlay.
- [x] **`color_ops` HSL core** — baseline-driven HSL palette adjustment now
      takes raw packed palette words, an optional selection mask, and caller
      output buffers (no `g_doc` / UI globals).
- [ ] **`color_ops` follow-ups** — quantize/dither color math that takes raw
      buffers and palettes (no `g_doc` / UI globals).
- [x] **`image_ops` edge helpers** — pure indexed-image edge/stroke analysis
      (`StrokeWordLuma8`, `EdgeBufferTransparent`,
      `EdgeBufferTransparentNeighbors`, `EdgeColorStrongVariant`,
      `FindInwardEdgeReplacement`) backing Strip Edge / Hard Stroke Remover.
      The `...MarkedImages` wrappers stay in the UI layer until Phase B exists,
      because they walk `g_doc` and set status globals.
- [ ] **More leaf helpers of the big image ops** — Dither Replace /
      Least-Squares Reduce / the ASM-port block still have pure inner helpers to
      pull into `image_ops`.

### Phase B — Globals foundation (the hard step, do carefully by hand)

- [ ] Create `platform/ui_internal.h` (one shared internal header) +
      `platform/ui_state.cpp` (one home for the file-scope state, with
      initializers preserved) declaring the remaining shared globals `extern`.
      This is the prerequisite that unlocks moving UI code, and the step the
      prior attempt botched — do it incrementally against the compiler.

### Phase C — UI section modules (mechanical once Phase B lands)

Split along the existing `/* ---- section ---- */ ` markers, one module at a
time, building green after each:

- [ ] `ui_canvas` — canvas render, pan/zoom, World View.
- [ ] `ui_palette` — palette editor, HSL sliders, histogram, color picking.
- [ ] `ui_timeline` — animation timeline, thumbnails, playback.
- [ ] `ui_tools` — toolbars and per-tool interaction (pencil, fill, lasso,
      free transform, clone, smart remap).
- [ ] `ui_modals` — export/import dialogs and confirmation prompts.
- [ ] `ui_main` — frame layout, dockspace, menu bar (whatever remains).

## Later phases (after the split)

- **Decouple business logic from UI** (overlaps Phase A): grow `image_ops` /
  `palette_math` into the home for flood fill, median-cut quantization, smart
  remap, lasso mask generation, free-transform scaling — pure buffer-in/out so
  they are unit-testable without an ImGui context.
- **State management**: encapsulate `g_doc` and `globals.c` state behind an
  `AppController`/`Workspace` passed into render functions, instead of reaching
  into globals. Replace raw arrays/manual memory with `std::vector` /
  `std::unique_ptr` where it does not conflict with the strict `IMG`/`PAL`
  binary layouts.
- **Formalize file I/O**: split `img_io.cpp` into per-format loaders/savers
  (`io_img`, `io_png`, `io_tga`, `io_lbm`, `io_gif`) behind a clean interface
  that returns a document/error and never touches the UI.
- **Legacy shim cleanup**: audit `shim_vid.c` / `shim_input.c` / `shim_file.c`
  / `shim_dialog.c`; replace temporary DOS crutches with `std::filesystem` /
  direct SDL2 calls where the legacy logic no longer needs them.
