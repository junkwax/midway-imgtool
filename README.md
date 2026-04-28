# midway-imgtool

Sprite and palette editor for the IMG container files used by 1990s Midway arcade games — Mortal Kombat II/3, NBA Jam, NBA Hangtime, and others. Loads, edits, and saves `.IMG` libraries plus their LBM and TGA siblings, preserving proprietary point-table and animation data from the original tools.

![imgtool image](https://raw.githubusercontent.com/junkwax/midway-imgtool/SDL-main/imgtool.png)

> **Status:** pure C/C++ port. Originally a 1992 DOS tool with ~34,000 lines of x86 assembly — now fully rewritten in C++ with SDL2 + Dear ImGui. No assembler, no DOSBox, no virtualization. 64-bit (x64) default with optional 32-bit builds. Runs on Windows and Linux.

---

## What it does

- **Browse sprites** — open an IMG file and scroll through every sprite frame (Up/Down, PgUp/Dn)
- **Edit pixels** — draw with pencil, pick colors with eyedropper, flood fill, marquee select
- **Manage palettes** — 256-color palettes with 15-bit RGB editing; assign, merge, and delete
- **Palette tools** — palette histogram, delete unused colors, and merge marked palettes
- **Animation points** — edit primary (X/Y) and secondary (X/Y/Z) anchor points for sprites
- **Hitbox editor** — drag-to-resize collision boxes with live overlay on the canvas
- **Dual-file mode** — open two IMGs at once (Tab to swap), copy IDs between them
- **Recent Files** — persistent history of recently opened IMG files for quick access
- **Export / Import** — TGA, LBM, and PNG import/export; Build TGA from marked sprites
- **Data Export** — Write ANILST (assembly animation lists) and MK3-format TBL files
- **Batch operations** — mark sprites (Space) then apply palette, reduce, strip, or dither all at once
- **Pixel restoration** — bulk restore pixel data from a parent sprite across the entire file via Regex or explicit selection
- **Undo / Redo** — 32-level undo stack for animation point and pixel edits
- **Copy / Paste** — copy pixel regions between sprites or across IMGs

Press **`h`** in the app for the full help reference — quickstart guide, keyboard shortcuts, file formats, and the Williams DMA #2 hardware document.

---

## Building from source

Requirements: **CMake 3.20+** and **SDL2 development libraries**. Visual Studio 2022 on Windows, GCC on Linux.

### Quick build (Windows) — auto-downloads everything

```cmd
build.bat              # 64-bit (default)
build.bat x86          # 32-bit
```
```powershell
.\build.ps1             # 64-bit (default)
.\build.ps1 -Arch x86   # 32-bit
```

Both scripts auto-download SDL2 2.30.2 and CMake to `%LOCALAPPDATA%\imgtool-build\`. Output: `build\Release\imgtool.exe`.

### Manual CMake build

**Windows (Visual Studio 2022):**
```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A x64
cmake --build . --config Release
```

**Linux (64-bit):**
```bash
mkdir build && cd build
cmake .. && cmake --build .
```

**macOS (Apple Silicon / Intel):**
```bash
brew install sdl2
mkdir build && cd build
cmake .. -DCMAKE_PREFIX_PATH="$(brew --prefix)" && cmake --build .
```

The build copies `SDL2.dll` (Windows), `it.hlp`, and the Material Symbols icon font next to the executable.

---

## Source structure

| Path | Purpose |
|------|---------|
| `IT/it.c` | Entry point, SDL2 window creation, main loop |
| `platform/imgui_overlay.cpp` | Main UI: menus, toolbar, canvas, panels, palette, modals |
| `platform/img_format.h` | IMG/PAL data structures, allocators, palette helpers |
| `platform/img_io.h` / `img_io.cpp` | File I/O: IMG load/save, TGA/LBM/PNG import/export |
| `platform/shim_vid.c` | SDL2 renderer init, palette tables, VGA emulation |
| `platform/shim_input.c` | Keyboard input, DOS scan code mapping |
| `platform/shim_file.c` | File system emulation, path remapping, old-format conversion |
| `platform/shim_dialog.c` | Native Windows file dialogs, directory persistence |
| `platform/globals.c` | Global state: image/palette lists, selection indices |
| `imgui/` | Dear ImGui v1.91.0 (SDL2 renderer backend only, trimmed) |
| `CMakeLists.txt` | Build config |

---

## File format support

| Format | Load | Save | Notes |
|--------|------|------|-------|
| **IMG** | Yes | Yes | Primary format. Pre-2.x IMGs auto-converted on open. Up to 2000 sprites/palettes per file. |
| **TGA** | Yes | Yes | 8-bit color-mapped Truevision Targa. Bottom-up pixel order. |
| **LBM** | Yes | Yes | IFF/ILBM chunk format. CMAP (palette) + BODY (bitmap). RLE decompression supported. |
| **PNG** | Yes | Yes | Via stb_image/stb_image_write. Auto-quantizes colors to 15-bit palette on import. |

---

## Environment variables

| Variable | Purpose |
|----------|---------|
| `IMGDIR` | Default directory for `.img` files |
| `TGADIR` | Default directory for `.tga` files |
| `MODELS` | Default directory for model files |
| `ITUSR1` / `ITUSR2` / `ITUSR3` | User-defined shortcut directories |

---

## Common keys

| Key | Action |
|-----|--------|
| `Ctrl+O` / `Ctrl+S` | Open / Save IMG |
| `Alt+L` / `Alt+S` | Load / Save LBM |
| `Ctrl+L` | Load TGA |
| `Ctrl+B` | Build TGA from marked images |
| `Space` | Mark / Unmark current image |
| `M` / `m` | Mark all / Clear all marks |
| `Up` / `Down` | Move in image list |
| `PgUp` / `PgDn` | Page up/down image list |
| `Tab` | Swap between image list 1 and 2 |
| `d` / `D` | Double / Halve zoom |
| `Ctrl+Z` / `Ctrl+Y` | Undo / Redo |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / Cut / Paste |
| `h` | Open comprehensive help |
| `F9` | Debug info popup |
| `Esc` | Cancel current action |

Full keyboard reference in the in-app help (`h`).

---

## Status

**`SDL-main` branch** — actively maintained. The original DOSBox build on `main` and `sdl-experimental` have been retired.

**What works:**
- Full 2D sprite editing parity with the DOS tool
- Modern UI (ImGui), file browser, undo/redo, copy/paste, hitbox editor
- All MK1/MK2/NBA-era IMG files load, edit, and save correctly
- PNG import/export with automatic palette quantization
- Cross-platform Windows + Linux
- Pure C/C++ build — no assembler required

**What's missing:**
- 3D model editor (original `it3d.asm` was stubbed; the tool is 2D-only)

---

Issues and PRs welcome at https://github.com/junkwax/midway-imgtool.
