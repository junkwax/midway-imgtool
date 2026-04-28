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
| `platform/img_io.h` / `img_io.cpp` | File I/O: LoadImgFile, SaveImgFile, TGA/LBM/PNG import/export, RestoreMarkedFromSource, RestoreMarkedFromSourceForce |
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
