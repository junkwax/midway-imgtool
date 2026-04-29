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

## Recent additions

### LOAD2 packing verifier (`platform/load2_verify.{h,cpp}`)
- Detects edits that would misalign SAGs after LOAD2 repacks the saved IMG.
- Three checks per image, mirroring `doc/load2/load2.c:2299-2547` + `doc/load2/zcom.c`:
  1. **Geometry drift**: w/h changes vs `baseline_p` snapshot taken at file load.
  2. **Palette ppp fallback**: `pal->numc > (1 << ppp)` triggers LOAD2's silent bpp-fallback at `load2.c:2303-2307` → destbits jumps ~33%, every following SAG misaligns. Default ppp=6 (MK2MIL.LOD).
  3. **Zero-shape drift**: per-row leading/trailing zero-count comparison (matches `zcom_analysis` per-row scan). Recolor edits that don't change silhouette are silent by design.
- Public API: `VerifyLoad2Packing(int ppp)` returns `L2Report` with `L2Issue` list. `VerifyLoad2BeforeSave(int ppp)` runs the check and pops a `g_restore_msg` toast if any breaking issues found; called from `SaveImgFile` (advisory only — save proceeds either way).
- UI: `Tools → Verify LOAD2 Packing` opens a modal with per-issue breakdown, click-to-jump-to-image navigation, editable PPP field.
- **Drift visualization**: clicking a Break-severity issue triggers `update_drift_texture()` which renders the sprite with drifting rows tinted red so the user sees exactly which scanlines need attention. Skipped for warnings.

### Bulk Restore from Regex — three modes
`Operations → Bulk Restore via Regex` dialog. Mode selected via radio buttons; dispatched in `imgui_overlay.cpp:~3687`.
1. **Replace** (`ExecuteBulkRestorePairs`): zeros child, fills parent rect verbatim. Original behavior. Lossy on per-piece detail outside the parent rect.
2. **Diff** (`ExecuteBulkRestoreDiff`): copies parent pixels where parent CURRENT ≠ parent BASELINE. Propagates session edits to children only.
3. **Reconstruct from Parent** (`ExecuteBulkRestoreReconstruct`, new): copies parent pixels where parent ≠ 0 AND child ≠ 0 AND child ≠ parent. The 3-way AND preserves both silhouettes — never blanks a child pixel, never extends a child silhouette. Use case: restore censored/blacked-out regions in shipping art (e.g. OTOMIX logo on Cage's pants in MK2 CAGE1.IMG).
   - Critical: `child != 0` guard prevents silhouette extension, which would cause zero-shape drift under `ZON+PPP` packing. Without it, master sprites that are wider than child silhouettes would extend the children → SAG misalign.

### OOB coverage guard for Bulk Restore preview
- `BulkRestoreMatch` extended with `covered_pixels` / `total_pixels` fields (`img_io.h`).
- `ComputeBulkRestoreCoverage(matches)` in `img_io.cpp` rect-clips the parent footprint after applying the same `dx = parent->anix - child->anix` shift the executors use, then stores the in-bounds pixel count.
- Called after sorting matches in the regex modal. Rows with coverage < 100% render in red with `(N% covered)` suffix; orange banner at top counts partials; "Deselect Partial" button mass-skips them.
- The byte-copy itself is unchanged — still pixel-perfect within the overlap region. The guard is preview-only.

### World View mode (`g_world_view`, Tab toggle)
- Alternative canvas render mode for cross-frame anipoint alignment. Replicates the DOS imgtool's "world canvas" workflow.
- Renders a fixed-size black canvas (default 400×254 = arcade playfield) with the sprite anchored at `(world_origin - sprite.anipoint)`. Up/Down arrows flick frames; canvas stays put, so the user can eyeball whether anipoints land in the same world position across frames.
- Left-drag adjusts `anix`/`aniy` (sprite-follows-cursor convention — anipoint decreases as you drag right/down).
- **Onion-skin** option draws the previous frame faintly underneath, useful for body-attachment alignment (e.g. NBA player heads onto body sprites).
- Coord readout overlay shows `[idx] name anix=N aniy=N world=WxH`.
- View menu exposes: World W, World H, Origin X, Origin Y, Onion-skin toggle.
- Pixel editing tools (paint, marquee, anim-point handles, hitboxes, DMA overlay, grid-selection) are auto-disabled while in World View — the existing canvas pipeline is gated `else` of the World View branch.

### Cursor up/down sprite list navigation
- `ImGuiKey_UpArrow` / `ImGuiKey_DownArrow` advance/retreat `ilselected` (wraps at ends) and trigger `g_zoom_reset = true`. Matches DOS imgtool muscle memory.
- Uses `ImGuiInputFlags_RouteGlobal` so it skips text-input fields automatically.

### Idle-friendly main loop (`IT/it.c`)
- Replaced uncapped `SDL_PollEvent` busy-loop with `SDL_WaitEventTimeout(&e, 16)`. Idle CPU drops from ~100% of one core to near-zero. Still renders one frame after every input change so ImGui hover delays and popup fades stay smooth (16ms = one frame at 60Hz worst case).
- Important on macOS where energy/thermal reporting surfaces idle CPU prominently.
