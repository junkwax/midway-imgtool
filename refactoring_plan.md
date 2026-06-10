# Midway Imgtool Refactoring Plan

`midway-imgtool` is a successful modernization of a 1992 DOS codebase. The main
maintainability problem is `platform/imgui_overlay.cpp` — a ~22,900-line
"super-module" holding nearly all UI rendering, dialog state, and image
manipulation logic. This document describes the strategy for breaking it up and
tracks slice-by-slice progress.

## Current checkpoint - June 10, 2026

Branch: `refactor/overlay-split` (76 commits ahead of `SDL-main`). Working tree
clean. `imgui_overlay.cpp` is down to 20,531 lines; `ui_canvas.cpp` is now 2,342
lines. Full app builds; all 7 `ctest` suites pass.

**Phase A (pure-logic extraction): done & unit-tested** — `palette_math`,
`sprite_resize_ops`, `color_ops` (HSL core), `image_ops` (edge/stroke).

**Phase B (globals foundation): in place, demand-driven** — `ui_internal.h`
(shared extern decls + the `ICON_*` glyphs, `ZOOM_MAX`, and the shared
`mark_dirty()` service) + `ui_state.cpp` (definitions home). Globals migrate
only when a function move needs them across TUs — *not* in a big up-front sweep
(the ~1000-line "Editor state" block is intentionally left in place).

**Phase C (UI subsystems): first subsystem complete, second started** —
`ui_timeline` now owns the whole timeline (frame model, thumbnail cache,
composite selection/playback, and all timeline rendering incl. the composite
preview). `ui_canvas` has started with zoom/pan helpers, canvas rotate-button layout/draw helpers, canvas chrome drawing (checkerboard, pixel grid, zoom badge, pixel hover highlight), anipoint hit-testing/crosshair drawing, IMG hitbox and MK2 strike-box overlay drawing, DMA compression and color-isolation overlay drawing, the single-sprite
World View canvas, World View config state, onion-skin texture cache, and marked World View
constants, playback/sequence/mirror/dummy-decap/drag/ASM-popup state,
string/model helpers, sequence tick/sync/edit helpers, and dummy-decap timing
reset helpers, plus the marked-lane model, marked-frame collection, and
source/dummy-decap lane collection/builders, lane playback resolver, and shared World View
layout helper used by single and marked World View rendering. It also owns
marked World View orchestration/scene drawing, marked-lane panel wrapper/layout/header controls, per-lane edit controls, thumbnail strip, drag handling, sprite/tag/status drawing, and
render-rect bookkeeping, the marked World View ASM text generator/preview popup,
selected dummy-decap body assignment, and ASM-driven marked-lane construction.
It also owns marked-lane sequence refresh after order/duplicate/delete/reset edits.
Supporting
modules extracted along the way:
`world_render` (sprite→texture), `anipoint` (pure predicates + sequence-name
parsing), `anipoint_edit` (sequence-propagating setters), `img_util`
(`img_name_string`, `signed_to_img_word`).

The accumulated user feature work (bulk resize, smarter subframe cuts, anim
propagation, mirrored World View fix, session restore, RGB-slider de-dup) is
committed (`fc4edeb`).

### Module inventory (platform/)

| Module | What it owns | Tested |
|--------|--------------|--------|
| `palette_math.{h,cpp}` | 15-bit palette-word math, color distance, nearest-slot | ✅ |
| `color_ops.{h,cpp}` | HSL palette adjustment (raw buffers) | ✅ |
| `image_ops.{h,cpp}` | edge/stroke pixel analysis | ✅ |
| `sprite_resize_ops.{h,cpp}` | nearest/quality resize resampling | ✅ |
| `img_util.{h,cpp}` | `img_name_string`, `signed_to_img_word` | ✅ |
| `anipoint.{h,cpp}` | secondary-anipoint predicates/mutators, seq-name parsing | ✅ |
| `anipoint_edit.{h,cpp}` | sequence-propagating anipoint setters + undo coalescing | — |
| `world_render.{h,cpp}` | `doc_get_img`, `doc_get_pal`, `BuildWorldSpriteTexture`, temp-tex pool | — |
| `ui_timeline.{h,cpp}` | timeline frame model, thumbs, composite, playback, preview | — |
| `ui_canvas.{h,cpp}` | zoom/pan helpers, canvas rotate-button geometry/drawing, canvas chrome drawing (checkerboard, pixel grid, zoom badge, pixel hover highlight), anipoint hit-testing/crosshair drawing, IMG hitbox and MK2 strike-box overlay drawing, DMA compression and color-isolation overlay drawing, single-sprite World View canvas, World View config/layout state, onion texture cache, marked World View orchestration/scene drawing, marked World View lane model/constants/playback+sequence+mirror+dummy-decap+drag+ASM-popup state/string/model helpers, marked-frame collection, source/dummy-decap lane collection/builders, ASM lane builder, lane playback resolver/sequence refresh, selected dummy-decap assignment, panel wrapper/layout/header controls, per-lane edit controls, thumbnail strip, drag, sprite/tag/status drawing, render rects + ASM generation/preview | — |
| `ui_internal.h` / `ui_state.cpp` | shared overlay state foundation + `mark_dirty`, `ICON_*` | — |

### For the next agent — how to continue

1. **Build/verify each slice**: `.\build.ps1 -BuildRoot C:\tmp\imgtool-build-<name>`
   then run `ctest` against `<BuildRoot>\build` with `-C Release`. Keep every
   commit green. One cohesive slice per commit.
2. **Per-slice loop**: survey a function's deps with `grep` (collisions across
   TUs? forward decls? which callers are outside the cluster?), move it to a
   module, un-`static` + declare in the header, drop a `/* now lives in ... */`
   breadcrumb in `imgui_overlay.cpp`, add to `CMakeLists.txt`, build, commit.
3. **Demand-driven globals**: only share a global (extern in a header, def in
   `ui_state.cpp`, or migrate into the owning module) when a moved function needs
   it across TUs. Keep compile-time constants as header constants, not `extern`.
4. **Don't force coupling**: if a function depends on still-overlay-private
   helpers, extract *those* first as their own slices rather than dragging them
   along (this is how the timeline composite preview was eventually unblocked).
5. **Next subsystems** (each multi-slice): keep shaving regular edit-canvas
   helpers into `ui_canvas`; then `ui_palette` (palette editor) and `ui_tools`.

Environment notes (noisy but nonblocking): git commands may warn about
`C:\Users\xbx\.config\git\ignore` permission; `build.ps1` may print
`'vswhere.exe' is not recognized...` after its success banner, yet binaries are
produced and tests pass. Unit tests for self-contained modules plug into the
`foreach(pure_test ...)` list in `CMakeLists.txt`.

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

### Phase B — Globals foundation (incremental, compiler-verified)

- [x] Create `platform/ui_internal.h` (shared internal header) +
      `platform/ui_state.cpp` (definitions home). Foundation in place.
- [x] Migrate SDL/zoom-pan scalar group and the undo-snapshot group (proves both
      the scalar and struct-backed patterns).
- [x] Promote shared services to the foundation as needed: `mark_dirty()`,
      `ZOOM_MAX`, and the `ICON_*` glyph vocabulary now live in `ui_internal.h`.
- [ ] **Remaining globals: demand-driven.** Move each global to `ui_state.cpp`
      (its type to `ui_internal.h` if struct-backed) only when a Phase C function
      move needs it across translation units — not in a big up-front sweep.
      Preserve initializers exactly; keep compile-time constants as header
      constants rather than `extern`.

### Phase C — UI section modules (carries Phase B migration with it)

Split along the existing `/* ---- section ---- */ ` markers, one module at a
time, building green after each; pull each function's required globals into the
foundation as you go:

- [x] `ui_timeline` — animation timeline: frame model, thumbnail cache,
      composite selection/playback, and all timeline rendering (composite
      preview + lock toggle). **Complete.**
- [x] Supporting leaf modules extracted while doing the above:
      `world_render` (sprite→SDL texture + temp-texture pool), `anipoint`
      (predicates + sequence-name parsing + secondary mutators), `anipoint_edit`
      (sequence-propagating setters), `img_util` (name + word clamp).
- [ ] `ui_canvas` — canvas render, pan/zoom, World View. **Started:** the
      zoom/pan helpers, canvas rotate-button layout/draw helpers, canvas chrome
      drawing (checkerboard, pixel grid, zoom badge, pixel hover highlight),
      anipoint hit-testing/crosshair drawing, IMG hitbox and MK2 strike-box overlay drawing, DMA compression and
      color-isolation overlay drawing, single-sprite World View canvas, World View config/layout state, onion-skin
      texture cache, marked World View constants,
      playback/sequence/mirror/dummy-decap/drag/ASM-popup state, string/model
      helpers, sequence tick/sync/edit helpers, and dummy-decap timing reset
      helpers, plus the marked-lane model, marked-frame collection, and
      source/dummy-decap lane collection/builders, ASM lane builder, lane playback resolver/sequence
      refresh, selected dummy-decap assignment, per-lane edit controls, thumbnail
      strip, marked World View orchestration/scene drawing, and sprite panel wrapper/layout/header controls/drag handling, tag/status
      drawing, render rect bookkeeping, and ASM export text generation/preview
      now live in `ui_canvas`; most regular edit-canvas interaction/rendering
      remains in `imgui_overlay.cpp`.
- [ ] `ui_palette` — palette editor, HSL sliders, histogram, color picking.
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
