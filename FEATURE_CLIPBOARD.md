# Cut / Copy / Paste & Expanded Properties Panel

**Status:** ✅ Implemented and committed (commit `7431bd6`)

---

## What's New

### 1. Complete Properties Panel (Right Sidebar)

The **Properties** section now displays **all** IMG struct fields available for the currently selected image:

| Field | Display | Notes |
|-------|---------|-------|
| **Name** | Frame name (max 15 chars) | e.g., `JCPOWERPUNCH1` |
| **Size** | W × H (width × height) | Sprite bounding box |
| **Pal** | Index + palette name | e.g., `5  SKIES` |
| **AX/AY** | Primary animation point | X, Y offset |
| **AX2/AY2/AZ2** | Secondary/tertiary anipt | All three coordinates |
| **Flags** | Hex + bit labels | e.g., `0x0005  Marked Changed` |
| **OPALS** | Alternate palette | `none` if 0xFFFF, else hex |
| **PTTBL** | Point table attached? | `present` or `none` |
| **DATA** | ROM pixel buffer address | Hex address (resolved OSET) |
| **Clip** | Clipboard image name | Shows if copy/cut active |
| **Undo** | Stack depth | e.g., `3/32` |

---

### 2. Cut / Copy / Paste

Three new Edit menu items + keyboard shortcuts:

#### **Copy** (Ctrl+C or Edit > Copy)
- Snapshots the selected image's metadata: name, all anipt coordinates, size, palette, flags, OPALS
- Strips the Marked bit so pasted images start unmarked
- **Does NOT copy pixel data** — clipboard is metadata only
- Clipboard status appears in Properties panel

#### **Paste** (Ctrl+V or Edit > Paste)
- Applies the copied metadata to the **currently selected image**
- Overwrites: name, AX/AY, AX2/AY2/AZ2, W/H, Pal, Flags (except Marked bit), OPALS
- **Preserves** the image's own pixel data pointer and point table (safe for asm)
- Automatically pushes an undo snapshot before paste
- Forces texture rebuild (so canvas reflects any size changes)
- Disabled until an image is copied

#### **Cut** (Ctrl+X or Edit > Cut)
- Copy + inject asm Ctrl+D (delete key) to remove image from list
- Asm handles list relinking and counter updates safely
- Clipboard retains the metadata for later paste

---

## Use Cases

### Example 1: Copy sprite properties across multiple images
1. Select image A (e.g., `JCPUNCH1` with specific anipt coords)
2. Press **Ctrl+C** — clipboard now holds `JCPUNCH1` metadata
3. Select image B — press **Ctrl+V** — B now has A's name, anipts, size, palette
4. Repeat for images C, D, ... with different sprite content but shared properties

### Example 2: Rename and reparent sprites
1. Select image A
2. **Ctrl+C** to copy its metadata
3. Select image B
4. **Ctrl+V** to paste — B gets A's exact metadata (name, coordinates, size)
5. Images still have their original pixel data — only properties changed

### Example 3: Migrate from one palette to another
1. Copy an image with palette 5
2. Paste it to another image and manually change **Pal** field
3. Both now reference a different palette with preserved sprite layout

---

## Technical Details

**Metadata-only clipboard design:**
- No deep copy of pixel buffers (avoids memory bloat, heap issues)
- Paste doesn't touch `data_p` (pixel pointer) or `pttbl_p` (point table pointer)
- Safe for asm-managed linked list — only metadata fields are transferred
- Undo system captures paste operations automatically

**Preserved Marked bit during paste:**
- If you copy an unmarked image and paste into a marked one, the Marked flag stays on the destination
- Ensures batch operations don't lose mark state

---

## Keyboard Shortcuts

| Shortcut | Action |
|----------|--------|
| **Ctrl+C** | Copy selected image metadata |
| **Ctrl+X** | Cut (copy + delete from list) |
| **Ctrl+V** | Paste metadata to current image |

Also available via **Edit** menu.

---

## Testing Checklist

- [x] Load an `.IMG` file
- [x] Select an image — verify all Properties fields display (name, size, pal+name, anipts, flags, opals, pttbl, data ptr)
- [x] **Ctrl+C** — clipboard name appears in Properties
- [x] Select a different image — **Ctrl+V** — metadata transfers, pixel canvas unchanged
- [x] Undo after paste — metadata reverts
- [x] Edit > Cut — copies metadata then injects asm delete key
- [x] Build succeeds with no errors (only pre-existing MSVC deprecation warnings)

---

## Compatibility Notes

- **Pixel data untouched** — paste is safe for hardware-managed sprite buffers
- **Point tables unchanged** — pttbl_p pointers not modified (preserves asm state)
- **Undo integration** — paste operations captured in undo stack automatically
- **No file format changes** — clipboard is in-memory only, not persisted to disk

---

## Related Files

- **Platform:** `platform/imgui_overlay.cpp` (clipboard struct, copy/paste helpers, Properties panel)
- **Keyboard:** Ctrl+C/X/V handled in main render loop (lines 387-394)
- **Menu:** Edit menu items (lines 424-430)
- **Display:** Properties panel (lines 673-704)

---

## Future Enhancements

- Clipboard could be extended to deep-copy pixel data (for full sprite cloning)
- Multi-image paste (apply one image's properties to all marked images)
- Clipboard persistence (save/load clipboard to a temp file across sessions)
