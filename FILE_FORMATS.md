# Image Tool (IT) — File Format Reference

**Last updated:** 2026-04-23  
**Based on:** wmpstruc.inc, itimg.asm, it.c source code

---

## Overview

The **IMAGE TOOL** is a **sprite/animation library editor** for Midway arcade games (primarily **Mortal Kombat** series). It does NOT work with ISO disc images (.img files). Instead, `.IMG` files are **binary sprite libraries** containing:

- **Sprites** — individual images with animation metadata
- **Palettes** — color tables (8-bit indexed color, 256 colors max)
- **Animation sequences** — frame lists with timing and deltas
- **Point tables** — collision/hitbox data, 3D animation points
- **Scripts** — game behavior data

---

## File Formats

### 1. **IMG** — Image Library (Primary Format)

**Purpose:** Main sprite/animation library container  
**Structure:** Binary format, little-endian, struct-packed (align 2)

```
LIB_HDR (28 bytes)
├─ IMGCNT (word)     — number of IMAGE records
├─ PALCNT (word)     — number of PALETTE records
├─ OSET (dword)      — file offset to IMAGE/PALETTE data
├─ VERSION (word)    — format version (0x0634 = Wimp V6.34)
├─ SEQCNT (word)     — size of sequence pool (bytes)
├─ SCRCNT (word)     — size of script pool (bytes)
├─ DAMCNT (word)     — number of damage tables
├─ TEMP (word)       — 0xABCD = version valid marker
├─ BUFSCR[4] (bytes) — script buffer assignments
└─ spare/padding

IMAGE[IMGCNT] (50 bytes each)
├─ N_s[16]     — sprite name (max 15 chars + null)
├─ FLAGS       — sprite flags (Marked, Loaded, Changed, Delete, etc.)
├─ ANIX, ANIY  — primary animation point (X, Y)
├─ W, H        — sprite bounding box size
├─ PALNUM      — palette index
├─ OSET        — ROM byte offset of sprite pixels
├─ DATA        — pointer to pixel data (runtime)
├─ LIB         — library handle index
├─ ANIX2, ANIY2, ANIZ2  — secondary animation point (X, Y, Z)
├─ FRM         — frame number (for seq/scr entries)
├─ PTTBLNUM    — point table index (-1 = none)
└─ OPALS       — alternate palette index (-1 = none)

PALETTE[PALCNT] (26 bytes each)
├─ N_s[10]    — palette name (max 9 chars + null)
├─ FLAGS      — palette flags (Marked, etc.)
├─ BITSPIX    — bits per pixel (always 8 for 256-color)
├─ NUMC       — number of colors used
├─ OSET       — ROM byte offset of palette data
├─ DATA       — pointer to color data (runtime, 3 bytes per color: 6-bit RGB)
├─ LIB        — library handle index
├─ COLIND     — CRAM start color (hardware specific)
├─ CMAP       — color map selection (0-F)
└─ spare

SEQSCR[SEQCNT] — Animation sequences and scripts
├─ name_s[16]  — sequence name
├─ flags       — sequence flags
├─ num         — frame count
├─ entry_t[16] — 16 frame entries (IMAGE pointers + timing)
├─ startx, starty  — animation offset
├─ dam[6]      — damage table
└─ spare

BLOB DATA
└─ Raw sprite pixel data and palette color data
```

**Load behavior:**
- File header validated: `TEMP == 0xABCD`
- Version checked: must be >= 0x0500
- IMAGE/PALETTE records loaded into linked lists (memory-mapped)
- Pixel data and palette data loaded into separate heap buffers
- Pre-2.x (VERSION < 0x0500) files are auto-converted to new format

**Save behavior:**
- All IMAGE/PALETTE/SEQSCR records written back to file
- Pixel/palette data appended as BLOB at end
- `TEMP` field updated with magic 0xABCD
- Damage tables zeroed out on save (not persisted in current version)

---

### 2. **TGA** — TrueVision Targa Bitmap

**Purpose:** Export/import individual sprites as 24-bit RGB bitmap  
**Structure:** Standard TGA file format (uncompressed)

**Load:** `ilst_loadtga` — load a TGA file as a new sprite into the current library
- Parsed as 24-bit RGB
- Converted to indexed 8-bit color using current palette
- Creates new IMAGE entry with TGA dimensions

**Save:** `ilst_savetga` — export selected sprite as TGA file
- Current sprite's pixel data
- Deplanarized back to native format
- Saved with current palette applied

**Use case:** Exchange sprites with external graphics tools (Photoshop, GIMP, etc.)

---

### 3. **LBM** — Interchange File Format (IFF/LBM)

**Purpose:** Load/save sprites in LBM (ILBM) bitmap format  
**Structure:** IFF chunk-based format

**Load:** `ilst_loadlbm` — load LBM file as new sprite
- Parse IFF header
- Extract palette chunk (CMAP)
- Extract bitmap data (BODY)
- Create new IMAGE entry

**Save:** `ilst_savelbm` — export selected sprite as LBM
- Write IFF container
- Write palette chunk (CMAP)
- Write bitmap data (BODY)
- Support for marked sprites: `ilst_savelbmmrkd`

**Use case:** Compatibility with Amiga/ILBM tools and older graphics systems

---

### 4. **VDA** — TrueVision VDA Format

**Purpose:** Variant of TGA format, less common  
**Status:** Referenced in original DOS tool, support unclear in SDL2 port

---

### 5. **MODEL** — 3D Geometry Files

**Purpose:** 3D model/polygon data for game objects  
**Structure:** Williams proprietary format

**Handlers:** `ittex.asm` (3D texture/model code)  
**Status:** Not fully reverse-engineered in SDL2 port

---

### 6. **USR1, USR2, USR3** — User-Defined Formats

**Purpose:** Custom/proprietary format slots  
**Status:** Not currently used, intended for expansion

**Environment variables:**
- `ITUSR1` — User format 1 directory
- `ITUSR2` — User format 2 directory
- `ITUSR3` — User format 3 directory

---

## Directory Environment Variables

All paths default to current working directory if not set:

| Variable | Purpose | Example |
|----------|---------|---------|
| `IMGDIR` | IMG library files | `C:\GRAPHICS\IMG` |
| `TGADIR` | TGA export/import | `C:\GRAPHICS\TGA` |
| `MODELS` | 3D model files | `C:\GRAPHICS\MODELS` |
| `ITUSR1` | User format 1 | (unused) |
| `ITUSR2` | User format 2 | (unused) |
| `ITUSR3` | User format 3 | (unused) |

---

## Keyboard Shortcuts for File Operations

| Key | Action | Handler |
|-----|--------|---------|
| **L** | Load IMG library | `main_loadi` |
| **S** | Save IMG library | `main_savei` |
| Ctrl+L | Load TGA file | `ilst_loadtga` |
| Ctrl+S | Save TGA file | `ilst_savetga` |
| Alt+L | Load LBM file | `ilst_loadlbm` |
| Alt+S | Save LBM file | `ilst_savelbm` |

---

## Internal Data Flow

```
File Load:
  *.IMG file
    ↓
  Parse LIB_HDR
    ↓
  Allocate IMAGE/PALETTE linked lists
    ↓
  Load IMAGE/PALETTE records
    ↓
  Load pixel/palette BLOB data into heap
    ↓
  Display in editor

File Save:
  Edited IMAGE/PALETTE structs
    ↓
  Update OSET offsets for BLOB data
    ↓
  Write LIB_HDR + IMAGE/PALETTE/SEQSCR records
    ↓
  Append pixel/palette BLOB
    ↓
  Update TEMP = 0xABCD
    ↓
  *.IMG file

Export to TGA/LBM:
  Selected IMAGE
    ↓
  Deplanarize pixel data
    ↓
  Apply current palette (if TGA)
    ↓
  Write TGA/LBM header + image data
    ↓
  *.TGA or *.LBM file
```

---

## SDL2 Port Status

**Currently Supported:**
- ✅ IMG library load/save (primary format)
- ✅ Auto-convert pre-2.x IMG files to new format
- ⚠️ TGA load/save (basic support, may need enhancement)
- ⚠️ LBM load/save (basic support, may need enhancement)

**Not Implemented:**
- ❌ VDA format
- ❌ MODEL files (3D geometry)
- ❌ USR1/USR2/USR3 custom formats
- ❌ Damage tables (zeroed on save)

**Recommended Next Steps:**
1. Verify TGA/LBM load/save work correctly in current port
2. Test round-trip: load IMG → export TGA → reimport TGA
3. Document any missing features or format discrepancies
4. Add file format filter buttons to ImGui file browser (match DOS UI)

---

## Technical Notes

- **Byte order:** Little-endian (x86 Intel)
- **Struct packing:** 2-byte alignment (`#pragma pack(2)`)
- **Color depth:** 8-bit indexed (256 colors per palette)
- **Sprite size limits:** Max 640×400 pixels (VGA resolution)
- **Counts:** Up to 2000 images, 2000 palettes per library
- **Animation:** Sequences support up to 16 frames each
- **Collision:** Point tables with 5 collision boxes + 1 centered box per sprite

---

## References

- `IT/wmpstruc.inc` — Binary struct definitions (authoritative)
- `IT/itimg.asm` — Main image loader/saver logic
- `IT/itos.asm` — Sequence/script handlers
- `IT/ittex.asm` — 3D model/texture handlers (less documented)
- `platform/shim_file.c` — SDL2 port file I/O shim layer
