# Midway Imgtool Refactoring Plan

`midway-imgtool` is a successful modernization of a 1992 DOS codebase. The main
maintainability problem is `platform/imgui_overlay.cpp` — a ~22,900-line
"super-module" holding nearly all UI rendering, dialog state, and image
manipulation logic. This document describes the strategy for breaking it up and
tracks slice-by-slice progress.

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
- [ ] **`color_ops`** — HSL/quantize/dither color math that takes raw buffers
      and palettes (no `g_doc` / UI globals).
- [ ] **Leaf helpers of the big image ops** — pull the pure inner helpers out of
      Strip Edge / Dither Replace / Least-Squares Reduce / the ASM-port block
      (e.g. `FindInwardEdgeReplacement`, `EdgeColorStrongVariant`). The
      `...MarkedImages` wrappers stay in the UI layer until Phase B exists,
      because they walk `g_doc` and set status globals.

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
