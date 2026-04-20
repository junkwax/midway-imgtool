# midway-imgtool (SDL2 Experimental Port)

IMGTOOL for editing and creating IMG container files used by various Midway games.

![imgtool image](https://raw.githubusercontent.com/JUNKWAX/midway-imgtool/sdl-experimental/main.png)

> **Note:** This is an experimental native port for Windows and Linux using SDL2.
> It no longer requires DOSBox, DOS4GW, or an ET4000 BIOS to run.

---

## Getting it running

### Option 1 — build from source

See [BUILD.md](BUILD.md) for full instructions. Short version:

**Windows (VS 2022 + MASM):**
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```
Output: `%LOCALAPPDATA%\imgtool-build\build\Release\imgtool.exe`

**Linux (GCC + patched JWasm, 32-bit):**
```bash
mkdir build-linux && cd build-linux
cmake .. && cmake --build .
```
Output: `build-linux/imgtool`

The build scripts copy `SDL2.dll` (Windows) and `it.hlp` next to the executable
automatically. Both files must stay next to `imgtool.exe` for the program to
work — `SDL2.dll` for rendering, `it.hlp` for the in-app help screen.

### Option 2 — run a prebuilt binary

Drop `imgtool.exe`, `SDL2.dll`, and `it.hlp` into any folder and double-click
`imgtool.exe`. No install, no registry changes, no `C:\BIN` directory needed
(the DOS build required one; this one does not).

---

## Usage

**New to imgtool?** Read [QUICKSTART.md](QUICKSTART.md) first — it walks
through opening a real `.IMG`, reading the UI, and making your first edit.

When the window opens you'll see a blank VGA-style canvas. Key commands:

| Key | Action |
|---|---|
| `l` | Load a `.img` file (opens the built-in file browser) |
| `s` | Save the current `.img` |
| `h` | Show help (reads `it.hlp` from next to the exe) |
| `f` | Redraw screen |
| `Esc` | Cancel current action |
| `F1` | Toggle between IMG mode and world builder |
| `Tab` | Swap image lists |
| `Space` | Mark / unmark current image |
| `d` / `D` | Double / halve image view size |
| Right-click top row of pixels | Open the main menu |

> The main menu is hidden — you must right-click the **top edge** of the window
> to open it. This is inherited from the original DOS UI.

Press `h` in the app for the full, up-to-date key reference (palette keys,
hitbox editor, anipts, etc.).

---

## Environment variables

The tool respects these optional env vars for default directories (same names
as the original DOS build):

| Var | Purpose |
|---|---|
| `IMGDIR` | Default directory for `.img` files |
| `TGADIR` | Default directory for `.tga` files |
| `MODELS` | Default directory for model files |
| `ITUSR1` / `ITUSR2` / `ITUSR3` | User-defined shortcut directories |

Unset = current working directory is used.

---

## Notes on the file format

IMG files are later built into IRW data for ROMs using the `.LOD` files and
`load2.exe`. That toolchain generates `IMGPAL*.ASM` / `IMGTBL*.ASM` plus `.tbl`
and `.glo` files. If you change an IMG file **early** in the ROM, all graphics
ROMs and your whole game project need to be rebuilt.

Ancient pre-2.x IMG files are auto-converted on open; a MessageBox confirms
the conversion so you can re-save in the current format.

---

## Hitboxes

Two rows of buttons, visible only in default zoom view.

- **ON/OFF** — toggle hitbox display.
- **Top row** — set hitboxes (numbers 1–5). Top-row **DEL** deletes the
  selected hitbox.
- **Bottom row DEL** — deletes the primary hitbox (usually #1) and lets you
  redraw it. With both sets on, DEL clears everything and lets you redraw all
  five.

---

## Status

This is the `sdl-experimental` branch. The DOS/DOSBox build still lives on
`main` if you need it.
