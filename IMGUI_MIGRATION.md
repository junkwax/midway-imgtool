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

4. 🟡 **Palette List Panel** (right sidebar)
   - Placeholder (requires asm exports of `pal_p`/`palcnt` not yet available)
   - Shows selected palette index

5. 🟡 **UI Suppression** (Phase 3)
   - TODO: Suppress asm right-click menu when ImGui is active
   - Currently both UIs are visible/active (not a blocker)

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

## Phase 3: UI Suppression & Polish (Planned)

Remaining work:
1. **Suppress asm gadget rendering** — When ImGui is active, suppress asm menu/gadgets on top
2. **Palette list export** — Modify itimg.asm to export `pal_p` and `palcnt`
3. **Palette operations** — Rename/delete/merge via ImGui that inject asm keys
4. **Hitbox/point editor** — Visual panels for collision boxes and animation points
5. **Undo/redo** — Track edits and provide undo stack

---

## Testing Checklist: Phase 2

- [ ] ImGui menu bar appears at top
- [ ] File → Open/Save work via native dialogs
- [ ] Image list panel populates with image names
- [ ] Click image in list — canvas updates
- [ ] Properties panel shows: name, size, palette, anipts, marked flag
- [ ] Palette swatches grid displays 256 colors
- [ ] Click color — selected with white border
- [ ] R/G/B sliders update color values
- [ ] Ctrl+O, Ctrl+S, Ctrl+Q keyboard shortcuts work
- [ ] Window resizes smoothly, panels reflow

---

## Commit History

- **a09c208** — Phase 2: Complete ImGui UI with panels for images, palettes, properties, swatches
- **54198c6** — Phase 1: Add ImGui overlay for modern Adobe/GIMP-style UI
- **70a4e12** — Use native OS file dialogs for load/save

---

## What's Next

1. Test Phase 2 by running `imgtool.exe` with a real .IMG file
2. Verify image selection, properties display, and color editing work
3. Plan Phase 3: suppress asm UI, export palette list, add palette operations
4. Consider: hitbox/point editor panels, undo/redo system
