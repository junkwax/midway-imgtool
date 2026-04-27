# midway-imgtool (SDL2 Experimental Port)

Editor for the IMG container files used by 1990s Midway arcade games (Mortal Kombat II/3, NBA Jam, NBA Hangtime, etc.). Loads, edits, and saves the original `.IMG` libraries plus their LBM and TGA siblings; preserves the proprietary point-table and animation data the original tools produced.

![imgtool image](https://raw.githubusercontent.com/JUNKWAX/midway-imgtool/sdl-experimental/imgtool.png)

> **Status:** experimental native port. Replaces DOSBox / DOS4GW / ET4000 BIOS with SDL2 + Dear ImGui. Runs on Windows and Linux without virtualization.

---

## Architecture

This is a **hybrid build**. The 1992 Watcom-era assembly core (~34,000 lines across `itos.asm`, `itimg.asm`, `it3d.asm`, `ittex.asm`) is still compiled in and still does all the heavy lifting — file I/O, IMG/PAL data structures, point tables, batch operations. A modern C++/SDL2 layer wraps it:

- **ImGui overlay** (`platform/imgui_overlay.cpp`) — menu bar, panels, toolbar, palette editor, hitbox editor, native file browser, debug popup.
- **Shim layer** (`platform/shim_*.c`) — translates DOS INT 21h, VGA register pokes, and BIOS mouse/keyboard calls into SDL2 equivalents.
- **MASM thunks** (`platform/*_thunks.asm`) — small register-marshaling stubs so MSVC's cdecl C++ can call into the asm core's `flat,syscall` routines without trashing callee-saved registers.

A handful of operations have been re-implemented natively in C++ (Strip Edge, Least-Squares Reduce, Dither Replace, Build TGA, Write ANILST, Delete Unused Colors, palette histogram), but the bulk of the engine is still asm. Consequence: build is locked to **32-bit x86** for now.

The 3D editor (`it3d.asm`, ~16k lines) is stubbed out and not exposed in the UI.

---

## Building from source

See [BUILD.md](BUILD.md) for full instructions. Short version:

**Windows (VS 2022 + MASM):**
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Output: `%LOCALAPPDATA%\imgtool-build\build\Release\imgtool.exe`

**Linux (GCC multilib + JWasm/UASM, 32-bit):**
```bash
mkdir build-linux && cd build-linux
cmake .. && cmake --build .
```
Output: `build-linux/imgtool`

The build copies `SDL2.dll` (Windows), `it.hlp`, and the bundled `assets/MaterialSymbolsSharp-Regular.ttf` icon font next to the executable. All three need to stay alongside `imgtool.exe` for full functionality (the icon font is optional — without it the toolbar falls back to short text labels).

---

## Running

Drop the contents of the `Release/` folder anywhere and double-click `imgtool.exe`. No install, no registry changes.

**New users:** read [QUICKSTART.md](QUICKSTART.md) — it walks through opening a real `.IMG`, reading the UI, and making your first edit.

Press `h` in the app for the live key reference. `F9` opens the **Debug Info** popup (file header + record details: OSET, LIB, FRM, PTTBLNUM). `Help > About` shows the build timestamp and git revision.

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

Optional — same names the DOS build used.

| Var | Purpose |
|---|---|
| `IMGDIR` | Default directory for `.img` files |
| `TGADIR` | Default directory for `.tga` files |
| `MODELS` | Default directory for model files |
| `ITUSR1` / `ITUSR2` / `ITUSR3` | User-defined shortcut directories |

Unset = current working directory.

---

## File format notes

IMG files are later built into IRW data for ROMs using the `.LOD` files and `load2.exe`. That toolchain generates `IMGPAL*.ASM` / `IMGTBL*.ASM` plus `.tbl` / `.glo` files. If you change an IMG file **early** in the ROM, all graphics ROMs and your whole game project need to be rebuilt.

Pre-2.x IMG files are auto-converted on open; a MessageBox confirms the conversion so you can re-save in the current format.

For deeper format details see [FILE_FORMATS.md](FILE_FORMATS.md). For an architectural map of the original tool see [TOOL_ARCHITECTURE.md](TOOL_ARCHITECTURE.md). The two `DMA*.txt` files at the repo root are the original Williams Z-Unit / DMA #2 hardware reference docs — relevant if you're ever generating IRW control words downstream.

---

## Hitboxes

Toggle from the toolbar (`Hb` button or `Hitbox` icon). Drag the corner handles to resize; the four outer points snap to the sprite's pixel grid. Hitbox data is stored inside the IMG container alongside the sprite. MK2-era hitboxes use a separate file path and aren't editable in this view.

---

## Status

`sdl-experimental` branch — actively maintained. The original DOSBox build still lives on `main` if you need it.

**What works:**
- Full 2D sprite editing parity with the DOS tool
- Modern UI (ImGui), file browser, undo/redo, copy/paste, hitbox editor
- All MK1/MK2/NBA-era IMG files load, edit, and save correctly
- Cross-platform (Windows + Linux x86)

**What's missing or partial:**
- 3D model editor (`it3d.asm`) — stubbed, not exposed
- 64-bit build — blocked on porting `itimg.asm` and `itos.asm` to C++
- macOS / ARM — same blocker
- Several batch operations still call into the asm core (rename/delete/duplicate, palette merge, point-table operations); native C++ ports are incremental

Issues / PRs welcome at https://github.com/junkwax/midway-imgtool.
