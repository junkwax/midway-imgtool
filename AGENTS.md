# AGENTS.md — Session Memory for AI Assistants

## Project
**midway-imgtool** — Editor for 1990s Midway arcade IMG container files (MK2/MK3, NBA Jam, etc.).
Pure C/C++ port of a ~34k-line x86 assembly DOS tool, now SDL2 + Dear ImGui.

## Build Commands

**Windows (VS 2022, x86):**
```cmd
build.bat
```
or
```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Both scripts auto-download SDL2 and CMake, output to `%LOCALAPPDATA%\imgtool-build\build\Release\imgtool.exe`.

**Linux:**
```bash
mkdir build && cd build && cmake .. && cmake --build .
```

## Key Files

| Path | Purpose |
|------|---------|
| `platform/imgui_overlay.cpp` | Main UI: menus, toolbar, canvas, palette, hitbox editor, file browser |
| `platform/img_format.h` | IMG/PAL data structures, allocators, palette helpers |
| `platform/img_io.h` / `img_io.cpp` | File I/O: IMG load/save, TGA/LBM/PNG import/export |
| `platform/shim_*.c` | SDL2 windowing, input, file dialogs |
| `CMakeLists.txt` | Build config, requires SDL2 |
| `IT/` | Help file and legacy data |
| `imgui/` | Dear ImGui v1.91.0 (bundled) |

## Known Pitfalls

- **DO NOT remove `ImGui::Render()` + `ImGui_ImplSDLRenderer2_RenderDrawData()` from the end of `imgui_overlay_render()`** — this breaks sprite loading and rendering. The commit `72900a5` removed them and was reverted by `efbc545`.
- The app links against SDL2's SDL_render API (not OpenGL). The render sequence is: draw all ImGui windows → `ImGui::Render()` → `ImGui_ImplSDLRenderer2_RenderDrawData()`.

## Current Branch

`SDL-main` — actively developed. The old DOSBox build and `sdl-experimental` branch have been retired.

## Current HEAD

`5834b7e` ("final: ASM removal, new main loop, app icon, window title") — **confirmed working**. Palette and canvas render correctly.

Commits `b7db52e`, `72900a5`, and `efbc545` were reverted — they introduced palette/canvas rendering regressions.

## Build Architecture

64-bit (x64) by default. Both `build.bat` and `build.ps1` accept an arch parameter:
```cmd
build.bat          REM default: x64
build.bat x86       REM 32-bit
```
```powershell
.\build.ps1 -Arch x86   REM 32-bit
```

## Git Workflow

- Commit style: short descriptive messages, all lowercase
- Use `--no-verify` sparingly (pre-commit hooks may exist)
