# midway-imgtool

A modern SDL2/ImGui port of the 1992 Midway Image Tool — a proprietary DOS sprite editor used by Midway Games artists for arcade titles (Mortal Kombat I/II/III, NBA Jam, NBA Hangtime, etc.). `.IMG` files are binary sprite libraries containing sprites, palettes, collision data, and animation metadata destined for graphics ROMs.

**Active branch:** `sdl-experimental` (original DOSBox build on `main`)

## Architecture

- **~34K lines of original x86 assembly** (`IT/itimg.asm`, `IT/itos.asm`) — file I/O, IMG/PAL format parsing, sprite rendering, palette ops
- **Modern C++/SDL2/ImGui overlay** (`platform/imgui_overlay.cpp`, ~2854 lines) — menu bar, toolbar, canvas, property panels, palette editor, hitbox editor, file browser, undo/redo, clipboard, batch operations
- **Shim layer** (`platform/shim_vid.c`, `shim_input.c`, `shim_file.c`, `shim_dialog.c`) — translates DOS INT 21h / VGA / BIOS calls to SDL2
- **MASM thunks** (`platform/*_thunks.asm`) — marshal registers between MSVC `cdecl` and asm `flat,syscall` convention
- **dear imgui v1.91.0** (`imgui/`) — UI rendering
- **32-bit only** — locked to x86 until assembly core is ported to C++

## What's Complete

- Full IMG load/save for MK1/MK2/NBA formats with auto-conversion of pre-2.x files
- Modern ImGui UI replacing the DOS gadget system
- Menu bar (File/Edit/Image/Palette/View/Programming/Help)
- Sprite canvas with checkerboard transparency
- Right panel: image list, palette list, properties, anim points, hitbox editor
- Palette editor (256-color swatch grid, RGB sliders, histogram, writeback)
- File browser with native Windows open/save dialogs and in-app fallback
- Recent files menu (persisted to `imgtool_recent.txt`)
- Undo/Redo (32-level stack: anim points, dimensions, palette, flags, hitbox)
- Clipboard with copy/cut/paste pixel data and marquee grid-selection
- Hitbox editor with corner-drag handles
- Animation point editor (two points, drag on canvas)
- Batch operations: Strip Edge (DMA prep), Dither Replace, Least-Squares Reduce, RestoreMarkedFromSource, Build TGA (with ANF manifest), Write ANILST
- Set palette for selected/marked images
- Rename image/palette/marked images (prepend or numeric-suffix modes)
- Delete image/palette/marked images, move image up/down
- Mark/unmark operations (all, clear, invert)
- Toggle point table, clear extra data
- Delete unused palette colors
- DMA compression visualization overlay
- Debug info popup (F9) — LIB_HDR, IMAGE, PALETTE runtime fields
- About popup with build timestamp and git commit hash
- Help modal with keyboard reference
- Crash handler (minidump on Windows, signal handler on Linux)
- Cross-platform Windows (MSVC+MASM) and Linux (GCC+JWasm) 32-bit builds
- LBM and TGA import/export
- Dual-image-list switching, Set ID from 2nd list
- Library info and mutable properties inspector

## Items Left to Finish

### Major Missing Features

1. **3D Model Editor** — `it3d.asm` (16,342 lines) and `ittex.asm` (945 lines) are replaced with empty stubs (`platform/it3d_stub.asm`). No 3D model editing, texture-mapped polygon rendering, or 3D world loading/saving.

2. **64-bit / ARM / macOS builds** — blocked by the ~34K lines of x86 assembly. Requires a full C++ port of `itimg.asm` and `itos.asm`.

3. **Incomplete C++ port of assembly core** — many UI operations are in C++, but core IMG/PAL format parsing and sprite rendering remain in assembly. Remaining asm-dependent operations:
   - `ilst_duplicate`, `plst_duplicate` (no C++ wrapper)
   - `plst_merge` (calls asm via thunk)
   - Point-table operations (partially via thunk)
   - Palette merge
   - Rename operations (C++ wrappers exist but call asm)

4. **Unsaved changes confirmation is disabled** — `imgui_overlay_check_unsaved_and_quit()` always returns 1, bypassing the unsaved-changes dialog.

### Partial/Stubbed Features

5. **No Linux native file dialog** — `shim_filereq_impl()` on non-Windows always returns cancel. Falls back to the asm in-app file browser.

6. **Hitbox data not persisted** — hitbox values are editor-only globals, not written to IMG structures. They reset on reload.

7. **Undo does not cover pixel edits** — copy/paste, strip, dither, etc. are irreversible via undo. Only anim points, dimensions, palette, flags, and hitbox are tracked.

8. **Missing file formats** — VDA, MODEL, USR1/USR2/USR3 not implemented. TGA/LBM support described as "basic, may need enhancement."

9. **Damage tables zeroed on save** — sequence/script/damage table data not persisted.

### Known Issues

10. **Sprite texture rebuilt every frame** — `rebuild_img_texture()` locks/unlocks an SDL streaming texture on each frame. Performance concern for large sprites.

11. **Clipboard uses heap malloc** — data won't survive `img_clearall` (separate from asm memory pool).

12. **JWasm patching required for Linux** — standard JWasm has a flat-model `.data` section bug. Users must build from patched source.
