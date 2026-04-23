# ImGui Migration Progress

## Phase 1: Complete ✅

The midway-imgtool UI has been modernized to use ImGui, replacing the DOS-era right-click menu system with a proper menu bar, while keeping all the original assembly image manipulation code intact.

### What Changed

**New Files:**
- `imgui/` — Vendored Dear ImGui v1.91.0
- `platform/imgui_overlay.h` — C-callable ImGui interface
- `platform/imgui_overlay.cpp` — ImGui initialization, menus, and panels

**Modified Files:**
- `CMakeLists.txt` — Added CXX language, ImGui sources, include paths
- `platform/shim_vid.c/h` — Renderer integration, ImGui init/render calls
- `platform/shim_input.c/h` — Event routing to ImGui, key injection

**No changes to:**
- Any `.asm` files (itos.asm, itimg.asm, it3d.asm, etc.)
- `IT/it.c` (C main entry point)
- Image manipulation core logic

### Current Capabilities

- **Menu bar** with File (Open/Save/Quit), Edit, Image, View, Help
- **Keyboard shortcuts** (Ctrl+O, Ctrl+S, Ctrl+Q, etc.) via ImGui menus
- **Canvas display** — VGA texture rendered inside ImGui window
- **Event handling** — SDL events fed to ImGui for UI interaction
- **Key injection** — Menu items can trigger asm handlers via synthetic keystrokes

### Build & Run

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Output: `%LOCALAPPDATA%\imgtool-build\build\Release\imgtool.exe`

The executable is **288 KB** with no external dependencies beyond SDL2.dll.

---

## Phase 2: Complete ✅

All asm gadget/menu system replaced with full ImGui panels.

### Features Implemented

1. ✅ **Image List Panel** (right sidebar)
   - Scrollable list of images from asm `img_p` / `imgcnt`
   - Click to select, updates `g_selected_image_idx`
   - Injects up/down arrow keys to trigger asm selection
   - Shows image count and names

2. ✅ **Palette Swatches Panel** (bottom)
   - 16×16 grid of 256 colors from `g_palette[256]`
   - Click to select active color
   - R/G/B sliders for editing colors
   - Colors directly update `g_palette[]` for next render

3. ✅ **Properties Panel** (right sidebar)
   - Current image name, dimensions, palette index
   - Animation point coordinates
   - Marked flag status
   - Parses from asm IMAGE struct memory layout

4. ✅ **Palette List Panel** (right sidebar)
   - Reads from asm `pal_p` linked list after export in Phase 3a
   - Click to select palette
   - Right-click context menu for operations

5. ✅ **UI Suppression** (Phase 3a)
   - Suppresses asm right-click menu when ImGui panels are active
   - Uses ImGui's `WantCaptureMouse` flag to hide mousey==0 signals

### Implementation Notes

**Calling back to asm:**
- All asm actions are already triggered by keys in the `key_t` dispatch table
- Use `shim_key_inject()` to push synthetic keys when menu items are clicked
- No new asm functions needed

**Accessing asm data:**
- Image list and palette list are exported globals (`img_p`, `imgcnt`, etc.)
- IMAGE struct layout is defined in `IT/it.inc` — read offsets to extract data in C
- VGA palette is `g_palette[256]` — already accessible

**Mouse canvas interaction:**
- When user clicks on the ImGui canvas, translate ImGui coords → VGA coords
- Write to `shim_ecx/edx/ebx` (mouse relay globals)
- Let asm main loop pick them up on next iteration via `shim_mouse_read`

### Architectural Notes

The rendering pipeline is:

```
g_vga_plane (asm writes)
    ↓
shim_vid_present():
  deplanarize → ARGB8888 staging buffer
  SDL_UpdateTexture(s_texture, ...)
  SDL_RenderCopy(s_texture → full viewport)
  imgui_overlay_newframe()
  imgui_overlay_render()  ← ImGui menu bar, panels, canvas image
  SDL_RenderPresent()
```

ImGui composites on top via the SDL_Renderer API. The canvas texture is passed to ImGui as `ImGui::Image((ImTextureID)s_texture, size)`.

No changes to how the asm draws — it continues writing indexed pixels into `g_vga_plane` exactly as before. ImGui just presents that texture inside a window alongside other panels.

---

## Phase 4: Animation Point Editor & Palette Operations — Complete ✅

### Phase 4a: Point Dragging
1. ✅ **Click detection** — 10px hit radius on both animation points
2. ✅ **Hover feedback** — Points change color (orange/cyan) when hovering
3. ✅ **Drag-to-move** — Click and drag primary/secondary points to new positions
4. ✅ **Real-time updates** — anix/aniy/anix2/aniy2 update directly in IMG struct
5. ✅ **Bounds clamping** — Points constrained to canvas bounds (0-639 x, 0-399 y)

### Phase 4b: Point Editor Panel
1. ✅ **View menu toggle** — "Point Editor" panel displays coordinates and sliders
2. ✅ **Coordinate display** — Shows current X/Y values for both points
3. ✅ **Slider controls** — Fine-grained adjustment via ImGui SliderInt widgets
4. ✅ **Nudge control** — Configurable arrow key increment (1-10 pixels)
5. ✅ **Visual feedback** — Coordinate labels and usage instructions

### Phase 4c: Palette Operations
1. ✅ **Rename functionality** — Palette names written directly to PAL->n_s in memory
2. ⚠️ **Delete/Merge** — Dialogs present, marked as "requires asm-side support"
3. ✅ **UI scaffolding** — Buttons disabled with explanatory text

### Additional Fixes
1. ✅ **Suppress gadget region** — Clear old DOS gadget drawings (y=0..10) before render
2. ✅ **Canvas cleanup** — No more stale gadget frames behind ImGui panels

## Phase 3: UI Suppression & Polish — Complete ✅

### Phase 3a: Palette Exports & UI Suppression
1. ✅ **Export asm palette globals** — Added public directives to itimg.asm for `pal_p`, `palcnt`, `plselected`
2. ✅ **Full palette list panel** — Reads from asm PAL linked list, click to select
3. ✅ **Suppress asm right-click menu** — When ImGui is active, mousey==0 is offset to prevent menu

### Phase 3b: Palette Operations
1. ✅ **Right-click context menu** — On palette list items, shows Rename/Delete/Merge
2. ✅ **Palette Rename dialog** — Text input, updates pal->n_s
3. ✅ **Delete confirmation** — Warns before deletion
4. ✅ **Merge target selector** — List dialog to choose target palette for merge

### Phase 3c: Visual Animation Point Editor
1. ✅ **Animation points display** — View menu toggle for "Animation Points"
2. ✅ **Primary point visualization** — Red circle at (anix, aniy) on canvas
3. ✅ **Secondary point visualization** — Green circle at (anix2, aniy2) if present
4. ✅ **Visual line connector** — Yellow line between two points for reference
5. ✅ **Auto-scaling overlay** — Points scale with canvas resize/zoom

### Future Work (Phase 5+)
1. **Palette delete/merge** — Asm-side linked list management to unlink/merge palettes
2. **Hitbox editor** — Visual collision box editor with corner drag handles
3. **Point table visualization** — Draw all points in pttbl_p if populated
4. **Undo/redo system** — Full edit history with Ctrl+Z / Ctrl+Y
5. **Image operations** — Move/copy/delete images via ImGui dialogs
6. **Keyboard navigation** — Arrow key nudging, Ctrl+Z undo, Delete to clear

---

## Testing Checklist: Phase 4

**Point Editor Functionality:**
- [x] View → Animation Points toggle shows/hides point overlay
- [x] Red circle appears at primary point (anix, aniy) on canvas
- [x] Green circle appears at secondary point (anix2, aniy2) on canvas
- [x] Yellow line connects two points
- [x] Points change color on hover (orange/cyan) indicating clickability
- [x] Click-drag primary point updates anix/aniy in real-time
- [x] Click-drag secondary point updates anix2/aniy2
- [x] Points clamp to canvas bounds (0-639 x, 0-399 y)

**Point Editor Panel:**
- [x] View → Point Editor toggle opens coordinate editor panel
- [x] Sliders adjust point X/Y values (0-639 x, 0-399 y)
- [x] Coordinate labels show current values
- [x] Nudge amount slider configurable (1-10 pixels)

**Palette Operations:**
- [x] Right-click palette list shows context menu
- [x] Rename dialog opens with current name in text box
- [x] New palette names persist in memory and display
- [x] Delete dialog shows confirmation (button disabled pending asm support)
- [x] Merge dialog shows target selector (button disabled pending asm support)

**Gadget Suppression:**
- [x] Old DOS gadgets (y=0..10) no longer visible behind ImGui panels
- [x] Canvas appears clean without stale gadget frames

## Testing Checklist: Phase 3

**Core Functionality:**
- [x] ImGui menu bar with File/Edit/Image/View/Help
- [x] File → Open/Save work via native dialogs
- [x] Image list panel populates and click-selects
- [x] Properties panel displays image metadata
- [x] Palette swatches 16×16 grid with R/G/B sliders
- [x] Ctrl+O, Ctrl+S, Ctrl+Q keyboard shortcuts work

**Phase 3a: Palette Management:**
- [x] Palette list panel populates from asm exports
- [x] Palette list click-selects
- [x] Asm right-click menu suppressed when ImGui is active

**Phase 3b: Palette Operations:**
- [x] Right-click palette → context menu appears
- [x] Rename dialog: input new name, updates display
- [x] Delete dialog: confirmation before removal
- [x] Merge dialog: select target palette

**Phase 3c: Animation Points:**
- [x] View → Animation Points toggle visible
- [x] Canvas displays red dot at primary anipt (anix, aniy)
- [x] Canvas displays green dot at secondary anipt (anix2, aniy2)
- [x] Yellow line connects the two points
- [x] Points scale correctly with canvas resize

---

## Commit History

- **1834f58** — Phase 4c: Wire palette rename to actual asm structure update
- **1698eba** — Phase 4b: Add Point Editor panel with sliders and nudge control
- **2102006** — Phase 4a: Implement point dragging on animation point editor
- **d27a8da** — Fix: Suppress old DOS gadget region from appearing behind ImGui panels
- **0c9049a** — Document Phase 3 completion
- **7a9471d** — Phase 3c: Add visual animation point editor overlay on canvas
- **9674baa** — Phase 3b: Add palette operations dialogs (rename, delete, merge)
- **f18e027** — Phase 3a: Export palette globals and suppress asm UI when ImGui is active
- **a09c208** — Phase 2: Complete ImGui UI with panels
- **54198c6** — Phase 1: Add ImGui overlay

---

## What's Next (Phase 4+)

1. **Point dragging** — Click-drag animation points to move them
2. **Hitbox editor** — Visual collision box editor with corner drag handles
3. **Palette operations completion** — Wire rename/delete/merge to actual asm calls via key injection
4. **Color palette API** — Export asm functions to apply palette changes back to asm state
5. **Point table visualization** — If pttbl_p is populated, draw all points/edges
6. **Undo/redo system** — Track image edits and provide full undo stack
7. **Keyboard shortcuts** — Arrow keys to nudge points, Delete to clear
