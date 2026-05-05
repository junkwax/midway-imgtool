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
| `platform/img_io.h` / `img_io.cpp` | File I/O: LoadImgFile, SaveImgFile, TGA/LBM/PNG import/export, RestoreMarkedFromSource, WriteTblFromMarked, WriteIrwFromMarked, verbose_log |
| `platform/lod_parser.h` / `lod_parser.cpp` | LOAD2 .lod manifest parser: batch-loads IMG libraries, sets g_load2_ppp from PPP> directive |
| `platform/load2_verify.h` / `load2_verify.cpp` | LOAD2 packing verifier: detects SAG-misalign edits, drift visualization |
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
- `WriteTblFromMarked(filepath, base_address, mk3_format, include_pal, pad_4bit, align_16bit)` in `platform/img_io.cpp:519`
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

## Point Tables & Hitboxes (IMG Container Format)

From `doc/wimp/wmpstruc.txt` and `doc/load2/load2.hlp`:

### Point Tables (`pttbl_p`)
- Each IMG can optionally have a **point table** — a 40-byte block of 10 `{int x, int y}` points (4 bytes each).
- Stored at the end of the .img file, after image data, palettes, sequences, and scripts.
- IMAGE_disk format field: `pttblnum` (unsigned short at +48). Value `0xFFFF` = no point table.
- In the in-memory `IMG` struct: `pttbl_p` is either `NULL` or points to a `calloc(1, 40)` block.
- Purpose: collision polygons, attachment points, or other per-sprite coordinate data used by the game engine.
- Toggled in WIMP via `PON>` / `POF>` directives (palette-on/palette-off in old docs, but the same keywords control point table visibility).
- Loaded/saved automatically by `LoadImgFile`/`SaveImgFile` via inline read at `img_io.cpp:267-270` and write at `img_io.cpp:532-533`.
- `TogglePointTable()` in `imgui_overlay.cpp:499` allocates/frees `pttbl_p` on the selected image.
- **Trailing name bytes**: The 16-byte raw name field (`file_name_raw[16]`) can carry trailing bytes past the null terminator (e.g. `"JCSTANCE1\0vda  \0"`). These are preserved verbatim during save because LOAD2 hashes the full 16 bytes for IRW sprite allocation — changing them shifts SAG layout. The UI only displays the null-terminated portion.

### Hitboxes (Bounding Box)
- Separate from point tables — stored as screen-space rectangle in the canvas overlay.
- Manipulated via the **Hitbox tool** (toolbar button, `Hb` icon). When active, a cyan rectangle appears on the sprite.
- Hitbox state variables: `g_hitbox_x`, `g_hitbox_y`, `g_hitbox_w`, `g_hitbox_h` (all `static int` in overlay).
- Drag the corners of the cyan rect to resize; drag the whole rect (future) to reposition.
- **Not stored in the IMG file** — the hitbox is a WIMP-level editing annotation. LOAD2 uses the `MON>` / `MOF>` (multipart box) and `BON>` / `BOF>` (collision box) directives in .lod files for actual game-engine hitboxes, which are separate from the WIMP hitbox.
- The IMG file does store `frm` (frame number for animation) and `opals` (alternate palettes), which are distinct from hitboxes.

### IMG File Layout (from LIB_HDR offset onward)
```
IMAGE_disk[imgcnt]      — 50 bytes each (wmpstruc.inc: IMAGE_disk)
PALETTE_disk[palcnt-3]  — 26 bytes each (wmpstruc.inc: PALETTE_disk)
SEQSCR + ENTRY arrays   — sequences/scripts blob
POINT_TABLE[ptcnt]      — 40 bytes each (only if version >= 0x60a)
Alternate palettes      — 16 bytes each (only if version >= 0x61d)
Damage tables           — pointers to seq/scr (only if version >= 0x632)
```

The palette defaults (#0=black, #1=red, #2=white) are never stored on disk — the PALETTE_disk array starts at index 3. `NUMDEFPAL=3`.

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

### LOD Manifest Parser (`platform/lod_parser.{h,cpp}`)
- Parses LOAD2 `.lod` manifest files for batch-loading IMG libraries.
- **File > Open LOD...** opens the file dialog; on confirm, the `.lod` is parsed and all referenced IMG files are loaded in sequence.
- Dialog includes **Force Override Directory (/O)** input to override paths specified in the `.lod` file.
- Recognizes all LOAD2 keywords: `PPP>`, `--->`, `***>`, `GLO>`, `ASM>`, `ROM>`, `ZON>/ZOF>`, `CON>/COF>`, `PON>/POF>`, `XON>/XOF>`, `FRM>`, `BBB>`, `UFN>`, `UGL>`, `UNI>`, `IHDR`, `MON>/MOF>`, `BON>/BOF>`, `ZAL>`.
- Only `PPP>` is acted upon — sets `g_load2_ppp` for the LOAD2 packing verifier. All other directives are parsed and skipped.
- IMG paths without a directory component are resolved via the `IMGDIR` environment variable (falls back to current working directory if unset).
- First IMG clears the workspace (`ClearAll`); subsequent IMGs append (mirrors LOAD2 `/A` append behavior).
- Toast shows: `"Loaded N IMG(s) from LOD (PPP set)"` on success, error message on failure.
- Dispatch is in `imgui_overlay.cpp:1615` (Save button handler, before the generic fpath/fname-using `else` block).
- Entry struct in `LodManifest` stores both the raw `.lod` line and the IMGDIR-resolved full path.
- Public API: `ParseLodFile(const char *lod_path, const char *override_dir)` returns a `LodManifest` with entries, `ppp_value`, and error state.

### IRW Export (`platform/img_io.cpp:999`)
- Writes LOAD2-compatible raw binary IRW files from marked sprites.
- **File > Export > Write IRW...** opens the dialog with options:
  - **ROM Base Address** — starting address in DMA2 image memory (default `0x02000000`)
  - **Bits Per Pixel** — Auto (Image Data), Auto (Palette Size) `/B`, or Fixed slider 1-8.
  - **Align to 16-bit boundary (/L)** — checkbox, ensures each sprite starts on a word boundary
- Format: `IRW_HEADER` (version + magic `0x64`) followed by per-sprite `IRW_RECORD` (start_addr, byte_count, checksum, etc.) + pixel data packed at bpp into 16-bit words via `irw_write_bits`.
- Auto-appends `.IRW` if no extension given.
- UI state variables: `g_irw_bpp`, `g_irw_base_address`, `g_irw_align_16bit`

### Paste tool — Photoshop-style selection overlay
- **Marquee** (`R` or toolbar button): click-drag on canvas to draw green selection rectangle.
- **Copy** (`Ctrl+C`): copies selected region (or entire image if no selection).
- **Cut** (`Ctrl+X`): copy + delete source image.
- **Paste** (`Ctrl+V`): clipboard pixels render as a **semi-transparent preview** draped over the sprite. Click-drag the preview to position it (release keeps it floating). **Click outside the preview** on the sprite to confirm placement. `Enter` to confirm, `Esc` to cancel.
- `apply_pasted_region()` at `imgui_overlay.cpp:2429` does a strict bounds check then row-by-row memcpy from clipboard to image data.
- Clipboard stores raw 8-bit palette-index pixels (`CopiedImage` struct, no palette metadata).

### Canvas input fix (`imgui_overlay.cpp:3594`)
- `io.WantCaptureMouse` was always true (unclear root cause). Fixed by adding `&& !ImGui::IsWindowHovered()` — canvas input is only blocked when ImGui has captured the mouse AND the mouse is not over the canvas window. This allows pencil drawing while modals still properly intercept clicks.

### File dialog extension filters (`imgui_overlay.cpp:1301`)
- `GetDirectoryFiles()` takes an `ext_filter` parameter. `GetDialogExtension(mode)` maps each `FileDialogMode` to the correct extension (IMG, LOD, LBM, TGA, PNG, ASM, TBL, IRW). Directories are always shown regardless of filter.

### LOD path fallback resolution (`imgui_overlay.cpp:1694`)
- When loading IMG files from a .LOD, the loader tries three directories in order:
  1. The path as written in the .LOD file
  2. The .LOD file's own directory (handles portability — LOD and IMGs in same folder)
  3. The `IMGDIR` environment variable
- Tracks `imgcnt` before/after each `LoadImgFile()` call to report accurate load counts.

### PNG import color fix (`img_io.cpp:1601,1646`)
- Green channel was masked with `0xFC` (6 bits) instead of `0xF8` (5 bits) during 15-bit RGB555 quantization. This caused green values to bleed into the blue channel space, corrupting imported palette colors. Both the histogram-building and pixel-remapping passes are fixed.

### Verbose logging (`img_io.cpp:22-37`)
- **View > Verbose Logging** toggles global `g_verbose`. When on, `verbose_log(fmt, ...)` outputs to `OutputDebugString` (Windows) or `stderr` (Linux).
- Current log points: `LoadImgFile` (path, header stats, final counts), `ImportPng` (path, dimensions, palette size), `ExportPng` (path), `OpenLod` (entry count, PPP value). Not on by default due to `WIN32_EXECUTABLE` flag (no console window).
