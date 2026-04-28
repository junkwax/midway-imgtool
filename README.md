# midway-imgtool

Editor for the IMG container files used by 1990s Midway arcade games (Mortal Kombat II/3, NBA Jam, NBA Hangtime, etc.). Loads, edits, and saves the original `.IMG` libraries plus their LBM and TGA siblings; preserves the proprietary point-table and animation data the original tools produced.

![imgtool image](https://raw.githubusercontent.com/JUNKWAX/midway-imgtool/sdl-experimental/imgtool.png)

> **Status:** pure C/C++ native port. Originally a 1992 DOS tool with ~34,000 lines of x86 assembly — now fully rewritten in C++ with SDL2 + Dear ImGui. No assembler, no DOSBox, no virtualization. Runs on Windows and Linux.

---

## Architecture

- **ImGui overlay** (`platform/imgui_overlay.cpp`) — menu bar, toolbar, sprite canvas, image/palette lists, properties panel, palette editor, hitbox editor, file browser, debug popup, undo/redo, clipboard.
- **File I/O** — IMG load/save, LBM load/save, TGA load/save, all implemented in portable C++ using standard `fopen`/`fread`/`fwrite`.
- **Shim layer** (`platform/shim_*.c`) — SDL2 windowing, input handling, native file dialogs.
- **dear imgui** (`imgui/`, v1.91.0) — UI rendering.
- **No assembly** — the 32,000+ lines of x86 assembly from the original 1992 Watcom tool have been fully ported to C++. Builds as a standard CMake project with no assembler required.

---

## Building from source

Requirements: CMake 3.20+, SDL2 development libraries.

**Windows (Visual Studio 2022):**
```cmd
mkdir build && cd build
cmake .. -G "Visual Studio 17 2022" -A Win32
cmake --build . --config Release
```

**Linux:**
```bash
mkdir build && cd build
cmake .. && cmake --build .
```

The build copies `SDL2.dll` (Windows), `it.hlp`, and the bundled `assets/MaterialSymbolsSharp-Regular.ttf` icon font next to the executable.

---

## Running

Drop the contents of the `Release/` folder anywhere and run `imgtool.exe`. No install, no registry changes.

**New users:** read [QUICKSTART.md](QUICKSTART.md) — it walks through opening a real `.IMG`, reading the UI, and making your first edit.

Press `h` in the app for the live key reference. `F9` opens the **Debug Info** popup. `Help > About` shows the build timestamp and git revision.

### Common keys

| Key | Action |
|---|---|
| `Ctrl+O` / `Ctrl+S` | Open / save IMG |
| `Alt+L` / `Alt+S` | Load / save LBM |
| `Ctrl+L` | Load TGA |
| `Ctrl+B` | Build TGA from marked images |
| `Ctrl+Z` / `Ctrl+Y` | Undo / redo |
| `Ctrl+C` / `Ctrl+X` / `Ctrl+V` | Copy / cut / paste |
| `Space` | Mark / unmark current image |
| `M` / `m` | Mark all / clear all marks |
| `h` | Help |
| `F9` | Debug info |
| `Esc` | Cancel current action |

---

## Environment variables

| Var | Purpose |
|---|---|
| `IMGDIR` | Default directory for `.img` files |
| `TGADIR` | Default directory for `.tga` files |
| `MODELS` | Default directory for model files |
| `ITUSR1` / `ITUSR2` / `ITUSR3` | User-defined shortcut directories |

---

## File format notes

IMG files are later built into IRW data for ROMs using the `.LOD` files and `load2.exe`. Pre-2.x IMG files are auto-converted on open.

For deeper format details see [FILE_FORMATS.md](FILE_FORMATS.md). `DMA2.txt` is the original Williams DMA #2 hardware reference document.

---

## Status

`sdl-experimental` branch — actively maintained. The original DOSBox build lives on `main`.

**What works:**
- Full 2D sprite editing parity with the DOS tool
- Modern UI (ImGui), file browser, undo/redo, copy/paste, hitbox editor
- All MK1/MK2/NBA-era IMG files load, edit, and save correctly
- Cross-platform (Windows + Linux)
- Pure C/C++ build — no assembler required

**What's missing:**
- 3D model editor (original `it3d.asm` was stubbed; the tool is 2D-only)
- macOS / ARM support (untested)

Issues / PRs welcome at https://github.com/junkwax/midway-imgtool.
