# midway-imgtool

Sprite and palette editor for the IMG container files used by 1990s Midway arcade games — Mortal Kombat II/3, NBA Jam, NBA Hangtime, and others. Loads, edits, and saves `.IMG` libraries plus their LBM, TGA, and PNG siblings, preserving proprietary point-table and animation data from the original tools.

![imgtool image](https://raw.githubusercontent.com/junkwax/midway-imgtool/SDL-main/imgtool.png)

> **Status:** pure C/C++ port. Originally a 1992 DOS tool with ~34,000 lines of x86 assembly — now fully rewritten in C++ with SDL2 + Dear ImGui. No assembler, no DOSBox, no virtualization. Runs on Windows (x64/x86), Linux (x64), and macOS (Intel + Apple Silicon).

---

## What it does

Imgtool is a sprite-sheet editor for Midway's IMG format. Open a file, browse hundreds of frames, edit pixels and palettes, then save back out — bit-identical to what the original 1992 tool produced.

### Editing
- **Pixel tools** — pencil, eyedropper, flood fill, marquee, lasso, magic wand, clone stamp, smart eraser, smart remap, auto-sprite chopper
- **Floating paste with Free Transform** — Ctrl+V drops a floating selection that auto-enters scale mode; drag handles to resize (aspect-locked by default, chain icon unlocks), Enter commits
- **Adobe-standard shortcuts** — Ctrl+A select all, Ctrl+D deselect, Ctrl+Shift+I invert, Ctrl+J duplicate, Ctrl+E merge down, Ctrl+T free transform
- **Snap + center guides** — drag-to-move snaps to edges of the underlying content (Shift held) and shows live magenta guides when paste centers on the sprite's axis
- **32-level undo / redo** for pixel + animation point + hitbox edits

### Palettes
- **256-color palettes** with 15-bit RGB editing
- **HSL sliders** — hue / saturation / lightness with live preview, scoped to selected swatches or whole palette
- **Locate-on-canvas highlight** — multi-select swatches (Ctrl/Shift-click) and every pixel that isn't one of the selected colors dims on the canvas, so you can see where a color lives at a glance
- **Cross-file palette clipboard** — copy a palette from one IMG, paste it after opening another
- **Tools** — merge marked, delete unused colors, histogram, duplicate, rename, palette match for PNG import

### Animation
- **Animation points** — primary (X/Y) and secondary (X/Y/Z) anchors per sprite, rendered as DOS-style crosshairs, draggable on the canvas
- **Onion-skin registration** — when onion mode is on, the previous frame's anipoint shows as a dim gray crosshair underneath the active one, matching the original DOS tool
- **Hitbox editor** — drag-to-resize collision boxes with overlay rendering
- **Timeline strip** — Play/Stop, scrubbable thumbnails, drag-to-reorder, FPS slider, ping-pong playback, onion-skin ghosting
- **World View** — fixed playfield canvas (default 400×254) with sprite anchored at its anipoint, for aligning body parts across frames

### MK2 strike-table editor
- **Direct `MKSTK.ASM` editor** (`Tools → MK2 Hitboxes`) — parses the Mortal Kombat 2 source-of-truth strike-table file and edits in place. No `stk.bin` round-trip required; saves write back through the same `.ASM` line buffer, preserving comments, indentation, and symbolic literals like `sf_squeeze`.
- **3-pane navigator** — character → move → fields. Each strike record exposes `x_offset` / `y_offset` / `x_size` / `y_size`, `strike_routine` (raw token), `damage` (split into hit / block bytes), `score` (32-bit), and `sound`.
- **Magenta on-canvas overlay** — the selected move's collision box renders on the active MK2 sprite with drag-to-resize corner handles; coordinates flow straight back into the `.ASM`. The IMG-embedded hitbox overlay suppresses itself while an MK2 move is selected.
- **Per-record undo + unsaved-changes guard** on quit (separate from the IMG dirty flow).

### Import / Export
- **TGA, LBM, PNG** load and save. PNG import quantizes via median-cut in 15-bit RGB space, then maps every pixel by nearest color. Import-into-active-palette mode skips quantization.
- **Build TGA** from marked sprites, **Export PNG**, **ANILST** (assembly animation lists)
- **TBL export** — MK3-format with hardware padding (`/P`), 16-bit align (`/L`), dual-bank (`/E`)
- **IRW export** — raw binary ROM layouts with dynamic BPP (`/B`)
- **LOD parser** — read `.lod` manifests, resolve via `IMGDIR` or override (`/O`)

### File dialog
- **Drag and drop** `.img` / `.png` / `.tga` / `.lbm` files onto the window to open or import
- **Per-category last-dir** memory (IMG / PNG / TGA / LBM each remember their own folder)
- **Preview thumbnails** for PNG and TGA highlighted files
- **Sort** by name / date / size, asc or desc
- **Double-click to open**, **Recent Files** menu

### Validation
- **LOAD2 packing verifier** (`Tools → Verify LOAD2 Packing`) — cross-checks edits against LOAD2's destbits computation. Catches geometry drift, PPP-fallback palette overflow, per-row zero-shape drift that would misalign sprite addresses (SAGs) when the IMG is repacked into the game's IRW/ROM. Each break shows a per-row drift visualization on the sprite.
- **Bulk pixel restoration** — restore pixel data from a parent sprite across the entire file via regex or explicit selection. Three modes: Replace, Diff, Reconstruct-from-Parent (re-paints censored interior pixels using the master as ground truth).
- **Unsaved-changes prompt** before opening another file, so palette / hue edits aren't silently discarded

Press **`h`** in the app for the full reference — keyboard shortcuts, file formats, the Williams DMA #2 hardware document. See [CHANGELOG.md](CHANGELOG.md) for release-by-release feature history.

---

## Building from source

Requirements: **CMake 3.20+** and **SDL2 development libraries**. Visual Studio 2022 on Windows, GCC on Linux, Clang on macOS.

### Quick build (Windows) — auto-downloads everything

```cmd
build.bat              :: 64-bit (default)
build.bat x86          :: 32-bit
```
```powershell
.\build.ps1              # 64-bit (default)
.\build.ps1 -Arch x86    # 32-bit
```

Both scripts auto-download SDL2 2.30.2 and CMake to `%LOCALAPPDATA%\imgtool-build\`. Output: `build\Release\imgtool.exe`.

### Manual CMake build

**Windows (Visual Studio 2022):**
```cmd
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

**Linux:**
```bash
cmake -B build
cmake --build build
```

**macOS (Apple Silicon / Intel):**
```bash
brew install sdl2
cmake -B build -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build
```

The build copies `SDL2.dll` (Windows) and the Material Symbols icon font next to the executable.

---

## Common keys

| Key | Action |
|-----|--------|
| `Ctrl+O` / `Ctrl+S` | Open / Save IMG |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / Cut / Paste |
| `Ctrl+A` / `Ctrl+D` / `Ctrl+Shift+I` | Select All / Deselect / Invert |
| `Ctrl+J` / `Ctrl+E` / `Ctrl+T` | Duplicate / Merge Down / Free Transform |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |
| `P` / `R` / `W` / `L` / `I` | Pencil / Marquee / Magic Wand / Lasso / Eyedropper |
| `Alt+L` / `Alt+S` / `Ctrl+L` / `Ctrl+B` | Load LBM / Save LBM / Load TGA / Build TGA from marked |
| `Space` / `M` / `m` | Mark / Unmark / Mark All / Clear All Marks |
| `Up` / `Down` / `PgUp` / `PgDn` | Navigate image list (or palette list if last clicked) |
| `Tab` | Toggle World View |
| `d` / `D` / `K` | Double zoom / halve zoom / play timeline |
| `Shift+Del` | Delete image |
| `h` / `F9` | Help / Debug info |
| `Esc` | Cancel current action |

Full keyboard reference in the in-app help (`h`).

---

## File format support

| Format | Load | Save | Notes |
|--------|------|------|-------|
| **IMG** | Yes | Yes | Primary format. Pre-2.x IMGs auto-converted on open. Up to 2000 sprites/palettes per file. |
| **TGA** | Yes | Yes | 8-bit color-mapped Truevision Targa. Bottom-up pixel order. |
| **LBM** | Yes | Yes | IFF/ILBM chunk format. CMAP + BODY with RLE decompression. |
| **PNG** | Yes | Yes | Median-cut quantization on import, nearest-color mapping. *Import (Match to Active Palette)* skips quantization. |

---

## Source structure

| Path | Purpose |
|------|---------|
| `platform/main.cpp` | Entry point, SDL2 window creation, main loop |
| `platform/imgui_overlay.cpp` | Main UI: menus, toolbar, canvas, panels, palette, modals |
| `platform/img_format.h` | IMG/PAL data structures, allocators, palette helpers |
| `platform/img_io.{h,cpp}` | File I/O: IMG load/save, TGA/LBM/PNG import/export |
| `platform/mk2_hitbox.{h,cpp}` | MK2 strike-table (MKSTK.ASM) parser/writer |
| `platform/shim_vid.c` | SDL2 renderer init, palette tables |
| `platform/shim_input.c` | Keyboard input, DOS scan code mapping |
| `platform/shim_file.c` | File system emulation, path remapping, old-format conversion |
| `platform/shim_dialog.c` | Native dialogs, directory persistence |
| `platform/globals.c` | Global state: image/palette lists, selection indices |
| `imgui/` | Dear ImGui v1.91.0 (SDL2 renderer backend only, trimmed) |
| `CMakeLists.txt` | Build config |

---

## Environment variables

| Variable | Purpose |
|----------|---------|
| `IMGDIR` | Default directory for `.img` files |
| `TGADIR` | Default directory for `.tga` files |
| `MODELS` | Default directory for model files |
| `ITUSR1` / `ITUSR2` / `ITUSR3` | User-defined shortcut directories |

---

## Status

**`SDL-main` branch** — actively maintained. The original DOSBox build on `main` and the older `sdl-experimental` branch have been retired.

**What works:** Full 2D sprite editing parity with the DOS tool, modern UI, file dialog with previews, all MK1/MK2/MK3/NBA-era IMG files load and save correctly, PNG import with proper quantization, cross-platform Windows/Linux/macOS, pure C/C++ build with no assembler required.

**What's missing:** 3D model editor (the original `it3d.asm` was stubbed; this tool is 2D-only).

---

Issues and PRs welcome at https://github.com/junkwax/midway-imgtool.
