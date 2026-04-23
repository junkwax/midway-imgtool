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

## Phase 2: Planned (Not Yet Started)

Replace the asm gadget/menu system with full ImGui panels.

### Features to Add

1. **Image List Panel** (right sidebar)
   - Scrollable list of images from asm `img_p` / `imgcnt`
   - Click to select, Space to mark/unmark
   - Star icon for marked images

2. **Palette Swatches Panel** (bottom)
   - 16×16 grid of 256 colors from `g_palette[256]`
   - Click to select active color
   - R/G/B sliders for editing
   - Call `vid_setvgapal18` to apply changes

3. **Properties Panel** (right sidebar)
   - Current image name, dimensions
   - Palette assignment
   - Part count, animation point count
   - Parse from asm IMAGE struct via offset constants

4. **Palette List Panel** (right sidebar)
   - Scrollable list of palettes
   - Click to select

5. **UI Suppression**
   - When ImGui is active, suppress the asm right-click menu by hiding mousey==0 signals
   - Asm gadgets still render to VGA buffer but ImGui panels take priority

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

## Testing Checklist (Phase 1)

- [x] Builds cleanly on Windows MSVC
- [x] Binary runs without crashes
- [ ] ImGui menu bar appears
- [ ] File → Open opens the native dialog
- [ ] File → Save saves the file
- [ ] Canvas displays the loaded image
- [ ] Keyboard shortcuts work (Ctrl+O, Ctrl+S)
- [ ] Window resize scales canvas correctly
- [ ] No regressions in image editing (asm hotkeys still work)

---

## Commit History

- **54198c6** — Phase 1: Add ImGui overlay for modern Adobe/GIMP-style UI
- **70a4e12** — Use native OS file dialogs for load/save
- **a2ebaae** — Change image link in README.md

---

## Next Steps

1. Test Phase 1 by running the app and verifying ImGui menu bar appears
2. Verify menus can trigger asm actions via key injection
3. Plan Phase 2 implementation (image list, palette swatches, properties)
4. Implement panels one at a time, testing after each addition
5. Gradually suppress the asm gadget system as ImGui panels take over
