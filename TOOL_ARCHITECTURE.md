# Image Tool (IT) — Complete Architecture Overview

**Project:** Midway arcade game sprite and 3D model editor  
**Time period:** 1992-1994 (Watcom C, x86 DOS4GW → MSVC/SDL2 modern port)  
**Total code:** ~34,305 lines of assembly across 4 modules  
**Platform:** Originally DOS/VGA 320×200, now SDL2/Win32/Linux

---

## Module Breakdown

### **1. ITOS.ASM** — Operating System & GUI Framework (8,202 lines)

**Role:** Core abstraction layer providing all OS, memory, video, and UI services to the other modules.

**Major Subsystems:**

#### Memory Management
- `mem_init`, `mem_alloc`, `mem_alloc0`, `mem_free` — Linked-list style pool allocator
- `mem_copy`, `mem_duplicate`, `mem_debug` — Utilities
- Pool structure: NXT (8-byte header) linked list

#### Video & VGA Palette Control
- `vid_setvmode` — Set VGA video mode (320×200 256-color)
- `vid_initvgapal`, `vid_setvgapal18`, `vid_setvgapal15` — Palette I/O (15-bit & 18-bit color)
- `vid_chain4off` — Disable VGA chain-4 mode for direct pixel access
- Screen buffer pointer: `scrnbuf`

#### File Requester (File Browser Dialog)
- `filereq_open`, `filereq_key`, `filereq_gads` — Modal file/directory picker
- `filereq_envtopath` — Convert env var to directory path
- `filereq_setfmatch` — Set file name glob (*.IMG, *.TGA, etc.)
- `dir_scan`, `dir_insert`, `dir_prt` — Directory enumeration & display

#### Gadgets & UI System
- **Buttons:** `gad_drawall`, `gad_draw`, `gad_chkmouse` — Clickable buttons
- **Scrollers:** `gad_mousescroller` — Scroll bar interaction
- **String input:** `gadstr_init`, `gadstr_key`, `gadstr_prt` — Text field editing
- **Dialogs:**
  - `msgbox_open`, `msgbox_key` — Message box (OK button)
  - `entrybox_open`, `entrybox_gads` — Yes/No confirmation
  - `strbox_open`, `strbox_gads` — String input dialog

#### Menus
- `menu_init`, `menu_main`, `menu_draw` — Cascading menus
- Menu structure defined in ITIMG.asm (`menu_t`, `menuitem_t`)

#### Drawing Primitives (Rasterization)
- `box_draw`, `box_drawshaded` — Rectangles with 3D shading effect
- `box_drawmany` — Batch drawing
- `boxh_draw`, `boxh_drawclip` — Horizontal lines with clipping
- `line_draw`, `line_drawclip` — Lines with clipping
- `crossh_draw`, `crossh_drawsml` — Crosshair cursors
- `scr_clr` — Clear screen buffer

#### Palette Block (Palette Editor Widget)
- `palblk_init`, `palblk_draw`, `palblk_draw1strgb` — Color picker UI
- `palblk_togtruc` — Toggle true color display
- `palblk_sortbrt`, `palblk_sortrgb` — Sort palette by brightness or RGB
- `palblk_adjrgb`, `palblk_movecolor` — Edit individual colors
- `palblk_setvgapal`, `palblk_setslavepal` — Commit color changes to hardware

#### Input Handling
- `mouse_reset`, `mouse_draw`, `mouse_erase` — Mouse cursor management
- `handler_ctrlbrk`, `handler_cerror` — Signal handlers (Ctrl+C, disk errors)

#### Text Output & String Utilities
- **Formatted printing:**
  - `prt_dec`, `prt_dec3srj` — Decimal integers (right-justified)
  - `prt_hexword`, `prt_hexdump` — Hex dumps
  - `prt_binword` — Binary output
  - `prt_xystr` — Print at XY coordinate
- **String manipulation:**
  - `strcpy`, `strcpylen` — Copy strings
  - `strsrch`, `strjoin` — Search, concatenate
  - `strcmp`, `strncmp` — Compare
  - `stratoi`, `stritoa` — String ↔ integer conversion
  - `stradddefext` — Add default file extension

#### File Operations & Help
- `dos_read4`, `dos_write4` — Low-level file I/O
- `file_renamebkup` — Backup old file before overwrite
- `help_main`, `cfg_load`, `cfg_save` — Help system & config persistence

**Data Structures:**
- Memory pool (linked-list allocator)
- Gadget definitions (buttons, scrollers, etc.)
- VGA palette (256 entries, 15-bit or 18-bit color)
- Configuration struct (CFG)
- Mouse state & position
- File requester state (directory, file list, selection)

---

### **2. ITIMG.ASM** — Image & Palette Management Engine (8,816 lines)

**Role:** Complete 2D sprite/animation editor with file I/O for IMG, TGA, and LBM formats.

**Major Subsystems:**

#### Image Management (Sprites)
- `imgmode_init`, `imgmode_setcfg` — Initialize image editor mode
- `img_clearall` — Clear all images from memory
- `img_loadnew`, `img_load` — Load IMG file (with format auto-conversion)
- `img_save`, `img_saveraw` — Save IMG library or raw pixel data
- `img_alloc`, `img_del` — Allocate/deallocate image records
- `img_findsel`, `img_find` — Find image by index or search
- `img_draw`, `img_drawscaled` — Render sprite at various zoom levels
- `img_prt` — Print image info to screen
- `img_loadpal` — Load palette for current image

#### Image List UI (Scrollable List)
- `ilst_gads`, `ilst_select` — Gadget definitions & selection
- `ilst_prt`, `ilst_prt1l`, `ilst_prt1l_ll` — Render image list on screen
- `ilst_keys` — Handle keyboard input in image list
- Navigation: `ilst_kup`, `ilst_kdn`, `ilst_kpup`, `ilst_kpdn` — Arrow keys, Page Up/Down
- `ilst_nxtlstkey`, `ilst_nxtlst` — Tab to next image list (world/image/seq/scr)

#### Image Editing Operations
- `ilst_rename`, `ilst_renamemrkd` — Rename image(s)
- `ilst_delete`, `ilst_deletemrkd` — Delete image(s)
- `ilst_duplicate` — Clone image with new name
- `ilst_moveup`, `ilst_movedn` — Reorder images in list
- `ilst_setpal`, `ilst_setpalmrkd` — Assign palette to image(s)
- `ilst_buildtgamrkd` — Build TGA from marked images

#### Image Processing (Batch Operations)
- `ilst_leastsqmrkd` — Least-squares color reduction (shrink palette)
- `ilst_striplowmrkd` — Strip low-occurrence colors
- `ilst_striprngmrkd`, `ilst_stripmrkd` — Strip color ranges from images
- `ilst_ditherrepmrkd` — Apply dithering to replace colors

#### File Format Handlers
- **LBM (Interchange/IFF):**
  - `ilst_loadlbm`, `loadlbm` — Load LBM file as new image
  - `ilst_savelbm`, `ilst_savelbmmrkd`, `savelbm` — Save image(s) as LBM
- **TGA (TrueVision Targa):**
  - `ilst_loadtga`, `loadtga` — Load TGA as new image
  - `ilst_savetga`, `savetga` — Save image as TGA
- Format conversion to/from 8-bit indexed color using current palette

#### Palette Management
- `pal_alloc`, `pal_del` — Allocate/deallocate palette records
- `pal_findsel`, `pal_find` — Find palette by index
- `pal_makemergemap` — Build color remapping for palette merge

#### Palette List UI
- `plst_gads`, `plst_select` — Gadget defs & selection
- `plst_prt`, `plst_prt1l`, `plst_prt1l_ll` — Render palette list
- `plst_keys` — Handle keyboard input
- Navigation: `plst_kup`, `plst_kdn`, `plst_kpup`, `plst_kpdn`

#### Palette Editing Operations
- `plst_rename` — Rename palette
- `plst_duplicate` — Clone palette
- `plst_merge` — Merge palettes (with color remapping)
- `plst_histogram` — Analyze color usage
- `plst_delunusedcols` — Remove unused colors
- `plst_loadpblk`, `plst_savepblk` — Load/save palette block from/to file

#### Mark Operations (Tag/Untag for Batch)
- `ilmrk_clrall`, `ilmrk_setall`, `ilmrk_invertall` — Image marks
- `plmrk_clrall`, `plmrk_setall`, `plmrk_invertall` — Palette marks

#### Collision/Hitbox Management
- **Multi-point boxes (polygon vertices):**
  - `mpbox_gads` — Gadget defs for multi-point editor
  - `mpbox_add`, `mpbox_delcur` — Add/delete vertices
  - `mpbox_nxtused`, `mpbox_togon` — Iterate & toggle visibility
- **Color boxes (palette range selection):**
  - `cbox_gads` — Gadget defs
  - `cbox_add`, `cbox_delcur` — Add/delete color ranges
  - `cbox_togon`, `cbox_copytomrkd` — Toggle & copy to marked images

#### Main Window & Screen Update
- `iwin_gads` — Window gadget defs
- `iwin_keys` — Window keyboard input
- `iwin_showscale` — Display zoom level indicator
- `iwin_clr` — Clear image window
- `scrn_update`, `main_redraw` — Refresh display
- `main_clear` — Clear canvas

#### File I/O Entry Points
- `main_loadi`, `main_appendi` — Load/append IMG file (keyboard shortcut: L, A)
- `main_savei`, `main_saveiraw` — Save IMG or raw pixels (S, Ctrl+S variant)

**Data Structures:**
- IMG linked list (sprites with animation points, palettes, hitboxes)
- PAL linked list (color palettes with color data)
- SEQSCR records (animation sequences with frame lists)
- ANIM_ENTRY (frame definitions with timing & delta offsets)
- PTTBL (point table with 5 collision boxes + 1 centered box)
- Mark flags per image & palette
- Config state (current image, palette, zoom level, etc.)

---

### **3. IT3D.ASM** — 3D Model Editor (16,342 lines) [Largest Module]

**Role:** Full-featured 3D polygon model editor with mesh editing, texture mapping, lighting, and animation.

**Major Subsystems:**

#### Initialization & Core
- `_3d_editorinit` — Initialize 3D editor mode
- `_3d_palinit` — Setup 3D rendering palette
- `_3d_scrn_update` — Update 3D viewport
- `_3d_main_exit_stub`, `_3d_help_main_stub` — Mode hooks

#### Gadgets & UI
- `_3d_main_gads`, `face_gads` — UI gadget definitions
- `flrcbar_draw` — Face/rotation control bar
- `facep_prtsettings` — Display face properties (texture, lighting, flags)
- `facep_showfullimg` — Show full texture in preview

#### View Control & Navigation
- **Keyboard shortcuts (WASD-style):**
  - `view_kup`, `view_kdn` — Pan up/down
  - `view_klft`, `view_krgt` — Pan left/right
  - `view_kalft`, `view_kargt` — Pan alternate (arrows)
  - `view_kcup`, `view_kcdn` — Rotate camera pitch
  - `view_kcrot` — Rotate camera roll
  - `view_zoom*` (multiple zoom functions) — Zoom in/out, fit-to-window
  - `dmode_nxt` — Cycle display modes (wireframe, shaded, textured, etc.)

#### Polygon & Face Editing
- `face_select` — Select polygon face
- `face_move`, `face_rotate`, `face_scale` — Transform selected face
- `face_setmat` — Set material/lighting properties
- `face_gettx` — Get texture coordinates

#### World & Model Management
- **File I/O:**
  - `world_loadreq`, `world_savereq` — File requester dialogs
  - `world_load`, `world_save` — Load/save world file
  - `world_clear` — Clear all models
  - `world_copy` — Clone world
  - `world_nxtmdl` — Iterate through models
- **Model operations:**
  - `mdl_new`, `mdl_load`, `mdl_save`, `mdl_delete` — Model CRUD
- **Object (mesh) operations:**
  - `obj_new`, `obj_load`, `obj_save`, `obj_delete` — Object CRUD

#### Geometry & Rendering
- 3D transformation matrices (rotate, scale, translate)
- Vertex & face data structures
- Lighting calculations (ambient, diffuse, specular)
- Perspective projection & clipping
- Call-out to `tex_drawface` (from ITTEX.ASM) for textured rendering

#### Animation & Sequences
- Frame data per animation sequence
- Sequence timing & playback
- Interpolation between keyframes

**Data Structures:**
- 3D world with multiple models
- Models with vertex lists & face definitions
- Face data (3D vertices, texture coordinates, material, lighting)
- Animation sequences with frame data
- View state (camera position, rotation, zoom, pan)
- Material/lighting parameters (RGB, specularity, etc.)

---

### **4. ITTEX.ASM** — Texture Mapping Rasterizer (945 lines)

**Role:** Low-level scanline rasterizer for rendering textured 3D polygons. Called by IT3D to render individual faces.

**Major Functions:**

- **`tex_drawface`** — Entry point
  - Takes: texture image (IMG), 4 screen vertices (TPTS), 4 texture coordinates (TIV)
  - Prepares data for rasterization
  - Calls `poly_drawface`

- **`poly_drawface`** — Main rasterization loop
  - Scanline-by-scanline polygon fill
  - Interpolates texture coordinates across each scanline
  - Applies shading, dithering, effects

- **`poly_setup_`** — Edge initialization
  - Find min/max Y vertices
  - Initialize left/right edge tracking

- **`poly_linesetup_`** — Per-scanline edge update
  - Track active edges as Y scanline advances
  - Calculate slope deltas for X coordinate interpolation

- **`tex_srclinesetup_`** — Texture coordinate derivatives
  - Calculate `dU/dX`, `dV/dX` for current scanline
  - Setup texture lookup offsets

- **`tex_copyline`** — Output scanline
  - Loop through X pixels
  - Lookup texture pixel (U, V coordinates)
  - Apply shading/dithering/zero-suppress effects
  - Write to video buffer

**Rendering Modes:**
- `M_TM` — Texture mapped (lookup texture pixel)
- `M_SH` — Shaded (interpolated lighting)
- `M_SHTM` — Shaded texture (texture × lighting)

**Effects:**
- **Zero-suppress** — Treat pixel value 0 as transparent
- **Dithering** — Error diffusion or patterned dither
- **Shading** — Gouraud-style interpolated lighting

**Key Concepts:**
- Fixed-point math (FRAC=21 bits fractional precision)
- Sub-pixel accuracy for smooth interpolation
- Edge-walking algorithm (DDA-style)
- Direct 256-color VGA framebuffer output

**Data Structures:**
- TIV (Texture Internal Vertices) — texture U,V × 4 vertices (fixed-point)
- TPTS (Texture Points) — screen X,Y,Z × 4 vertices
- Edge state (slopes, intercepts, active edge list)
- Dither lookup tables (`pix_t`)

---

## **Overall Architecture & Call Flow**

```
┌──────────────────────────────────────────────┐
│            Main Application Loop             │
│           (ITOS.ASM: _osmain)                │
│  - Event loop (keyboard, mouse, redraw)      │
│  - Dispatch to mode handlers                 │
└──────────┬───────────────────────────────────┘
           │
     ┌─────┴──────────────────┬──────────────┐
     │                        │              │
┌────▼──────────────┐  ┌─────▼────────────┐  │
│  ITIMG (2D Mode)  │  │ IT3D (3D Mode)   │  │
│ Image/Pal Editor  │  │  Model Editor    │  │
│                   │  │                  │  │
│ - Image list      │  │ - Model/mesh     │  │
│ - Palette editor  │  │ - Face editing   │  │
│ - Format I/O      │  │ - View control   │  │
│ - Sprite canvas   │  │ - Lighting       │  │
│                   │  │ - Texture map    │  │
└─────────────────────┬────┬───────────────┘  │
                      │    │                  │
                      │  ┌─▼──────────────┐   │
                      │  │ ITTEX (Render) │   │
                      │  │                │   │
                      │  │ tex_drawface   │   │
                      │  │ poly_drawface  │   │
                      │  │ tex_copyline   │   │
                      │  └────────────────┘   │
                      │                       │
                      └──────────────────────┘
                              │
                    ┌─────────▼─────────────┐
                    │   ITOS (OS Services)  │
                    │                       │
                    │ - Memory management   │
                    │ - Video (VGA, SDL2)   │
                    │ - File I/O            │
                    │ - UI gadgets/dialogs  │
                    │ - Text & drawing      │
                    │ - Keyboard/mouse      │
                    └───────────────────────┘
```

**Module Dependencies:**
1. **ITIMG** → ITOS (for all UI, memory, file I/O)
2. **IT3D** → ITOS (for UI, memory, file I/O)
3. **IT3D** → ITTEX (for face rendering via `tex_drawface`)
4. **ITTEX** → (none; pure rasterization logic)

**Shared Resources:**
- Screen framebuffer (`scrnbuf`) — 320×200, 256-color palette
- VGA palette (256 entries)
- Memory pool (allocated by ITOS)
- Configuration struct (shared state)

---

## **Key Algorithms & Techniques**

### Image Processing
- **Least-squares color reduction** — Shrink palette while minimizing visual error
- **Dithering** — Error diffusion or ordered dither for color quantization
- **Palette merging** — Remap images to new palette with minimal color shift

### 3D Rendering
- **Perspective projection** — Transform 3D vertices to screen coordinates
- **Scanline polygon fill** — Edge-walking DDA rasterizer
- **Texture mapping** — Bilinear interpolation of U,V coordinates across face
- **Gouraud shading** — Interpolated vertex colors across face
- **Fixed-point math** — 21-bit fractional precision for smooth interpolation

### UI & Input
- **Gadget system** — Reusable button, scroller, string input widgets
- **Modal dialogs** — File requester, message boxes, text entry
- **Menu dispatch** — Cascading menu with keyboard/mouse input
- **Scrollable lists** — Image & palette list views with mark/select

---

## **Code Statistics**

| Module | Lines | Role | Complexity |
|--------|-------|------|-----------|
| **ITOS.ASM** | 8,202 | OS/GUI framework | High (multi-subsystem) |
| **ITIMG.ASM** | 8,816 | 2D editor | Medium (list UI + file I/O) |
| **IT3D.ASM** | 16,342 | 3D editor | Very high (3D math + mesh ops) |
| **ITTEX.ASM** | 945 | Rasterizer | Very high (fixed-point math) |
| **Total** | **34,305** | Complete tool | **Ultra-high** |

---

## **Capabilities Summary**

✅ **2D Sprite Editing**
- Load/save IMG libraries (up to 2000 sprites)
- Palette management (up to 2000 palettes)
- Animation point editing (primary, secondary, 3D point)
- Collision box editing (5 per sprite)
- Export/import as TGA, LBM
- Batch operations (mark, rename, delete, color reduce, dither)

✅ **3D Model Editing**
- Load/save 3D worlds with multiple models
- Polygon mesh editing
- Face/texture selection & transformation
- Lighting & material properties
- Animated sequences with frame interpolation
- Real-time textured polygon rendering

✅ **File Format Support**
- IMG (primary, with auto-conversion of pre-2.x files)
- TGA (24-bit bitmap)
- LBM (IFF/ILBM)

✅ **UI System**
- File requester with directory navigation
- Modal dialogs (message, entry, string)
- Cascading menus
- Gadget-based UI (buttons, scrollers, text fields)
- Keyboard & mouse input
- Scrollable lists with mark/select

✅ **Rendering**
- Real-time textured 3D polygon rendering
- Fixed-point rasterization (smooth, sub-pixel accurate)
- Gouraud shading, dithering, transparency effects
- VGA 320×200 256-color display

---

## **SDL2 Port Status**

**Completed:**
- ✅ Core image/palette management (ITIMG subsystems)
- ✅ File I/O for IMG, TGA, LBM formats
- ✅ ImGui overlay for modern UI (replaces DOS gadgets)
- ✅ SDL2 video layer (replaces VGA)

**Not Yet Ported:**
- ❌ IT3D (3D model editor) — Large module, not converted
- ❌ ITTEX (texture rasterizer) — Not ported (would require 3D support)
- ⚠️ Some ITOS subsystems (file requester, palette block UI) partially integrated into ImGui

**Architecture:** The SDL2 port focuses on 2D sprite editing (ITIMG) and replaces the DOS UI (ITOS) with modern ImGui, while preserving the 1992 assembly core for compatibility and sprite rendering.
