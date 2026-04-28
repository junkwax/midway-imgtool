# AGENTS.md — Session Memory for AI Assistants

## Project
**midway-imgtool** — Editor for 1990s Midway arcade IMG container files (MK2/MK3, NBA Jam, etc.).
Pure C/C++ port of a ~34k-line x86 assembly DOS tool, now SDL2 + Dear ImGui.

## Build Commands

**Windows (VS 2022):**
```cmd
build.bat              # 64-bit (default)
build.bat x86          # 32-bit
```
```powershell
.\build.ps1             # 64-bit (default)
.\build.ps1 -Arch x86   # 32-bit
```
Output: `%LOCALAPPDATA%\imgtool-build\build\Release\imgtool.exe`

**Linux:**
```bash
mkdir build && cd build && cmake .. && cmake --build .
```

## Key Files

| Path | Purpose |
|------|---------|
| `IT/it.c` | Entry point, main loop, SDL2 init |
| `platform/imgui_overlay.cpp` | Main UI: menus, toolbar, canvas, panels, palette bar, modals, keyboard shortcuts |
| `platform/img_format.h` | IMG/PAL structs, allocators, palette helpers, get_img/get_pal, PoolAlloc |
| `platform/img_io.h` / `img_io.cpp` | File I/O: LoadImgFile, SaveImgFile, TGA/LBM/PNG import/export, RestoreMarkedFromSource, RestoreMarkedFromSourceForce, WriteTblFromMarked, WriteAnilstFromMarked |
| `platform/shim_vid.c` | SDL2 renderer, palette tables, VGA shadow buffer |
| `platform/shim_input.c` | Keyboard input, DOS scan code mapping |
| `platform/shim_file.c` | DOS file system emulation, path remapping, old-format IMG conversion |
| `platform/shim_dialog.c` | Native Windows file dialogs, directory persistence |
| `platform/globals.c` | Global state: image/palette linked lists, selection indices, file paths |
| `platform/compat.h` | Windows/Linux abstraction: types, MessageBox, _snprintf, path separators |
| `CMakeLists.txt` | Build config, SDL2 dependency, post-build copy steps |
| `IT/it.hlp` | Help text for DOS-style help system (unused by ImGui UI) |
| `imgui/` | Dear ImGui v1.91.0 (trimmed: only SDL2 renderer backend) |

## Code Patterns

### Dirty flag for unsaved changes
- `g_dirty = true` set by `undo_push()`, pixel edits, palette edits, paste, delete, etc.
- `g_dirty = false` on load and explicit Ctrl+S save
- Quit check uses `g_dirty` to decide whether to show "Unsaved Changes" popup

### Quit flow
- Menu Quit or window X → `g_pending_quit = true`
- Render checks `g_dirty` → opens "Unsaved Changes" popup if dirty
- Save/Discard close popup, `should_quit()` returns true → main loop exits cleanly
- Cancel clears `g_pending_quit`
- No hard `ExitProcess()` calls in quit path anymore

### Restore from source (Operations menu)
- **Restore from Selected (pixel-diff)**: Copies source pixels only where strip is transparent AND source has color. Palette must match.
- **Bulk Restore from Source (overwrite)**: Clears marked strips with `memset(0)`, then repaints from source. No palette check.
- Mapping: `sx = x + src.anix - strip.anix` (strip local → source local, note: signs were inverted in a previous version)

### Texture rendering
- Texture rebuilt EVERY frame from `img->data_p` via `rebuild_img_texture()`
- `g_img_tex_idx` tracks last rendered image index (for zoom reset, not for triggering rebuilds)
- `g_canvas_texture` exists for init compat, not displayed

### TBL Export (File > Export > Write TBL...)
- `WriteTblFromMarked(filepath, base_address, mk3_format, include_pal)` in `platform/img_io.cpp:519`
- Writes assembly-format `.TBL` files from marked images (Hex output, e.g. `0FFBDH`)
- **MK2 format** (default): `.word W,H,ANIX,ANIY` — 4-value header
- **MK3 format** (checkbox): `.word W,H,ANIX,ANIY,ANIX2,ANIY2,ANIZ2` — 7-value header with secondary animation points
- **ROM address**: `.long` = `base_address + (file_oset * 8)` (bit-addressable for TMS34010)
- **Palette name** (checkbox): `.long PAL_NAME` auto-resolved via `get_pal(p->palnum)`
- UI state variables: `g_tbl_base_address` (default `0x02000000`), `g_tbl_export_mk3_format`, `g_tbl_export_palette`
- `get_pal(idx)` helper in `platform/img_format.h:201` walks the PAL linked list by index
- IMG struct debug fields used: `anix2`, `aniy2`, `aniz2`, `file_oset`, `palnum`, `flags`
- **Menu item**: `platform/imgui_overlay.cpp:2182` — `File > Export > Write TBL...`
- **Dialog UI** (only shown in WriteTbl mode): `platform/imgui_overlay.cpp:1298-1303`
  - `ImGui::InputScalar` for hex ROM base address
  - `ImGui::Checkbox` for MK3 7-value format (with `SetItemTooltip`)
  - `ImGui::Checkbox` for palette name inclusion
- **Save button**: `platform/imgui_overlay.cpp:1333` — appends `.TBL` if no extension, calls `WriteTblFromMarked`

## Known Pitfalls

- **DO NOT remove `ImGui::Render()` + `ImGui_ImplSDLRenderer2_RenderDrawData()`** — this breaks render
- Linux GCC requires `goto` targets to not skip variable initializations (unlike MSVC)
- `_snprintf` macro maps to `snprintf` on Linux via `compat.h` — must include `compat.h`
- `g_img_tex_idx` is defined in `img_io.cpp` (not in overlay) — declared `extern` in `img_io.h`

## Help System
- Press `h` → scrollable Help modal (700x500, embedded text)
- Contains: Quickstart, Keyboard Reference, File Formats, DMA2 Hardware Reference
- No external help files needed — all hardcoded in `g_help_text` raw string literal

## Current Branch

`SDL-main` — actively developed. Default branch on GitHub.

## Build Architecture

64-bit (x64) by default. 32-bit via `build.bat x86` or `.\build.ps1 -Arch x86`.

## Releases

- v2.0.0 published at https://github.com/junkwax/midway-imgtool/releases/tag/v2.0.0
- Contains both `imgtool-x64.zip` and `imgtool-x86.zip`

## CI

- `.github/workflows/c-cpp.yml` — Windows (x64 + Win32) and Linux builds on push/PR

## Git Workflow

- Commit style: short descriptive messages, all lowercase
- Branch: `SDL-main`
