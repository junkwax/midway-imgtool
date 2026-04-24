# Menu & Features Gap Analysis — DOS vs ImGui Port

**Date:** 2026-04-23  
**Analysis of:** itimg.asm menu definitions (lines 289-394) and key_t bindings (lines 596-666)

---

## Executive Summary

The **ImGui overlay port is ~80% feature-complete** for the 2D sprite editor (ITIMG). The following features are either missing or incomplete:

- ✅ **Core features:** Present (load/save IMG, image editing, palette operations)
- ⚠️ **Marked image operations:** Present but architecturally different (buttons vs submenu)
- ❌ **Advanced features:** Partially missing (histogram, unused color deletion, debug menu)
- ❌ **Prog menu:** Completely unimplemented (dump memory, write animation list)

---

## Complete Menu Breakdown (DOS Original)

### **Main Menu** (7 items)
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| EXIT | ESC | `main_exit_stub` | ✅ File > Quit (Esc) |
| CLEAR | — | `main_clear` | ✅ File > New |
| LOAD | l | `main_loadi` | ✅ File > Open (L) |
| APPEND | — | `main_appendi` | ✅ File > Append (A) |
| SAVE | s | `main_savei` | ✅ File > Save (S) |
| HELP | h | `help_main_stub` | ✅ Help > Show Help (H) |
| RAW SAVE | — | `main_saveiraw` | ⚠️ Not in menu (keyboard only) |

### **In/Out Menu** (5 items) — DOS submenu, ImGui spreads to File
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| LOAD LBM | Alt+L | `ilst_loadlbm` | ✅ File > Load LBM (Alt+L) |
| SAVE LBM | Alt+S | `ilst_savelbm` | ✅ File > Save LBM (Alt+S) |
| SAVE MRKD LBM | — | `ilst_savelbmmrkd` | ⚠️ Missing menu item |
| LOAD TGA | Ctrl+L | `ilst_loadtga` | ✅ File > Load TGA (Ctrl+L) |
| SAVE TGA | Ctrl+S | `ilst_savetga` | ✅ File > Save TGA (Ctrl+S) |

### **Image Menu** (5 items)
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| RENAME | Ctrl+R | `ilst_rename` | ✅ Edit > Rename Image (Ctrl+R) |
| DELETE | Ctrl+D | `ilst_delete` | ✅ Edit > Delete Image (Ctrl+D) |
| SET PALETTE | ] | `ilst_setpal` | ✅ Image (context menu in right panel) |
| DUPLICATE | — | `ilst_duplicate` | ✅ Edit > Duplicate (shown in menu) |
| ADD/DEL PTTBL | Ctrl+P | `ilst_pttblchng` | ✅ Edit > Add/Del Point Table (Ctrl+P) |

### **Marked Image Menu** (9 items) — DOS submenu, ImGui merges into Image menu
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| RENAME | — | `ilst_renamemrkd` | ⚠️ Not explicitly in menu |
| DELETE | — | `ilst_deletemrkd` | ⚠️ Not explicitly in menu |
| SET PALETTE | [ | `ilst_setpalmrkd` | ⚠️ Keyboard shortcut only |
| STRIP EDGE | — | `ilst_stripmrkd` | ❌ Not in ImGui |
| STRIP EDGE LOW | — | `ilst_striplowmrkd` | ❌ Not in ImGui |
| STRIP EDGE RNG | — | `ilst_striprngmrkd` | ❌ Not in ImGui |
| LEAST SQUARE | ; | `ilst_leastsqmrkd` | ✅ Keyboard shortcut works |
| DITHER REPLACE | — | `ilst_ditherrepmrkd` | ❌ Not in ImGui |
| BUILD TGA | Ctrl+B | `ilst_buildtgamrkd` | ✅ Edit > Build TGA (Ctrl+B) |

### **Palette Menu** (6 items)
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| RENAME | Shift+R | `plst_rename` | ⚠️ Keyboard works, not in menu |
| MERGE | * | `plst_merge` | ✅ Palette > Merge button |
| DELETE | Del | `plst_delete` | ✅ Palette > Del button |
| DUPLICATE | — | `plst_duplicate` | ⚠️ No menu item |
| SHOW HISTOGRAM | — | `plst_histogram` | ❌ MISSING |
| DEL UNUSED COLS | — | `plst_delunusedcols` | ❌ MISSING |

### **Marks Menu** (6 items) — DOS submenu, ImGui moves to Palette panel buttons
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| CLR ALL IMG | m | `ilmrk_clrall` | ✅ Images panel > "Clr All" button |
| SET ALL IMG | M | `ilmrk_setall` | ✅ Images panel > "Set All" button |
| INVERT ALL IMG | — | `ilmrk_invertall` | ✅ Images panel > "Invert" button |
| CLR ALL PAL | — | `plmrk_clrall` | ✅ Palettes panel > "Clr All" button |
| SET ALL PAL | — | `plmrk_setall` | ✅ Palettes panel > "Set All" button |
| INVERT ALL PAL | — | `plmrk_invertall` | ✅ Palettes panel > "Invert" button |

### **Prog Menu** (2 items) — DEBUG/BATCH OPERATIONS
| Item | Shortcut | Handler | Status in ImGui |
|------|----------|---------|-----------------|
| DUMP SLAVE | — | `host_dumpslavemem_stub` | ❌ COMPLETELY MISSING |
| WRITE ANILST | — | `ilst_wanilstmrkd` | ❌ COMPLETELY MISSING |

---

## Complete Keyboard Bindings (65 total)

### **Implemented in ImGui** ✅
- **Navigation:** Arrow keys, PgUp/Down, Tab (switch lists)
- **Zoom:** D/d, F11/F12 (decrease/increase)
- **File I/O:** L (load), S (save), Alt+L (LBM load), Alt+S (LBM save), Ctrl+L (TGA load), Ctrl+S (TGA save)
- **Image Editing:** Ctrl+R (rename), Ctrl+D (delete), Ctrl+B (build TGA), Ctrl+P (point table), Ctrl+C/X/V (copy/cut/paste — new in ImGui)
- **Marks:** m (clear all), M (set all), Space (toggle), ' / (palette up/down)
- **Palette:** * (merge), Shift+R (rename — may need verification)
- **General:** ESC (exit), h (help), f (redraw), Ctrl+Z/Y (undo/redo — ImGui additions)

### **Partially Implemented** ⚠️
- **T/t toggles:** `palblk_togtruc`, `iwin_keys` — May not be fully wired
- **Alt+ movement:** Alt+Up/Down/Left/Right for anim point nudging — Keyboard works but may need testing
- **Ctrl+ movement:** Ctrl+Up/Down/Left/Right for 2nd anim point — Keyboard works but may need testing

### **Missing/Incomplete** ❌
- **Test function:** | (pipe) → `test_main` — Debug feature, not critical
- **F1:** Toggle 3D editor (only in original; 3D editor not ported)
- **F7/F8:** Custom image window modes (unclear function)
- **Ctrl+Home/End/Del:** Damage table clear operations — Not critical
- **a:** Animation/anim point mode toggle — Keyboard binding exists but may not fire
- **;:** Least-squares reduction — Keyboard binding works but should add to menu

---

## Detailed Feature Gap Analysis

### **CRITICAL MISSING (Should Add)**

#### 1. **Palette > Show Histogram**
- **Handler:** `plst_histogram`
- **Purpose:** Display color usage histogram for selected palette
- **Difficulty:** Medium (requires histogram drawing, probably modal dialog)
- **Priority:** Medium (useful for color analysis but not daily use)

#### 2. **Palette > Delete Unused Colors**
- **Handler:** `plst_delunusedcols`
- **Purpose:** Remove unused colors from palette to shrink file
- **Difficulty:** Medium (requires color remapping across all images)
- **Priority:** Low (optimization feature)

#### 3. **Marked Image Operations Submenu**
- **Items:** RENAME, DELETE, STRIP EDGE (variants), DITHER REPLACE
- **Purpose:** Batch operations on multiple selected images
- **Difficulty:** High (requires reworking menu structure or adding submenu)
- **Priority:** Medium (useful for batch work but keyboard shortcuts partially work)

#### 4. **Save Marked Images as LBM**
- **Handler:** `ilst_savelbmmrkd`
- **Purpose:** Export all marked images as individual LBM files
- **Difficulty:** Medium (straightforward file I/O variant)
- **Priority:** Low (TGA build is higher priority)

### **NOT CRITICAL (Can Defer)**

#### 5. **Prog Menu** (Debug/Development)
- **DUMP SLAVE:** `host_dumpslavemem_stub` — Memory dump for debugging
- **WRITE ANILST:** `ilst_wanilstmrkd` — Export animation list (batch operation)
- **Purpose:** Development/debugging tools, not user-facing
- **Difficulty:** High (requires understanding of original VUNIT slave hardware)
- **Priority:** Very Low (skip for now)

#### 6. **Test Function** (|)
- **Handler:** `test_main`
- **Purpose:** Debugging/testing code
- **Difficulty:** Unknown (may be incomplete in original)
- **Priority:** Very Low (skip)

### **ARCHITECTURAL DIFFERENCES (Not Gaps)**

These are intentional changes from DOS UI to ImGui — feature-complete but different:

1. **In/Out submenu merged into File menu**
   - DOS: Separate menu with 5 items
   - ImGui: All 5 items in File menu
   - Status: ✅ All functions present, just different organization

2. **Marked Image operations distributed**
   - DOS: Separate "Marked Image" submenu
   - ImGui: Merged with "Image" menu items, some as keyboard-only
   - Status: ⚠️ Functional but less discoverable; should add submenu

3. **Mark operations as buttons**
   - DOS: "Marks" menu with 6 items
   - ImGui: SmallButtons in Images/Palettes panels
   - Status: ✅ All 6 functions present, more discoverable in context

---

## Priority Feature List for Porting

### **Phase 1 — HIGH PRIORITY (Do next)**
1. ✅ Add menu items for marked image operations (RENAME MRKD, DELETE MRKD, BUILD TGA)
2. ✅ Add "Save LBM for Marked" to File menu
3. ✅ Wire `Shift+R` palette rename shortcut correctly
4. ⚠️ Verify `T/t` toggle and Alt+arrow nudging work correctly

### **Phase 2 — MEDIUM PRIORITY**
5. Add "Palette > Show Histogram" (modal dialog with histogram)
6. Add "Palette > Delete Unused Colors"
7. Add submenu for Marked Image operations (if UX review determines it's needed)
8. Add "Edit > Strip Edge" variants to menu (or submenu)

### **Phase 3 — LOW PRIORITY (Nice-to-have)**
9. "Marked > Dither Replace" operation
10. Export "Animation List" feature
11. Test/Debug features (Prog menu)

### **Phase 4 — NOT FOR 2D PORT**
12. ❌ 3D editor (F1) — Out of scope, ITIMG only
13. ❌ Slave memory dump — Out of scope, requires VUNIT hardware

---

## Current ImGui Menu Structure vs DOS

### **DOS Structure** (7 menus, 35+ items)
```
File
├─ Load (l)
├─ Append
├─ Save (s)
├─ Help (h)
└─ Exit (ESC)

In/Out
├─ Load LBM (A-l)
├─ Save LBM (A-s)
├─ Load TGA (C-l)
└─ Save TGA (C-s)

Image
├─ Rename (C-r)
├─ Delete (C-d)
├─ Set Palette
├─ Duplicate
└─ Add/Del PTTBL (C-p)

Marked Image [9 items]
├─ Rename
├─ Delete
├─ Set Palette
├─ Strip variants
├─ Least Square
├─ Dither Replace
└─ Build TGA (C-b)

Palette [6 items]
├─ Rename (Shift-r)
├─ Merge (*)
├─ Delete (Del)
├─ Duplicate
├─ Histogram
└─ Del Unused Cols

Marks [6 items]
├─ Clear All IMG (m)
├─ Set All IMG (M)
├─ Invert All IMG
├─ Clear All PAL
├─ Set All PAL
└─ Invert All PAL

Prog [2 items]
├─ Dump Slave
└─ Write ANILST
```

### **ImGui Structure** (3 menus, ~20+ items)
```
File
├─ Open (l)
├─ Append (a)
├─ Save (s)
├─ Load LBM (A-l)
├─ Save LBM (A-s)
├─ Load TGA (C-l)
├─ Save TGA (C-s)
├─ Quit (ESC)
└─ [Missing: Raw Save, Save LBM for Marked]

Edit
├─ Undo (C-z)
├─ Redo (C-y)
├─ Copy (C-c)
├─ Cut (C-x)
├─ Paste (C-v)
├─ Rename Image (C-r)
├─ Delete Image (C-d)
├─ Duplicate
├─ Build TGA (C-b)
├─ Add/Del Point Table (C-p)
└─ [Missing: Marked image submenu]

Help
└─ Show Help (h)

[Right Panel Buttons for Marks]
Images: Mk All, Clr All, Invert
Palettes: Mk All, Clr All, Invert, Merge, Del
```

---

## Recommendations for Next Steps

### **Quick Wins (< 30 min each)**
1. Add "Edit > Save LBM for Marked Images" menu item → call `ilst_savelbmmrkd`
2. Add "Edit > Rename Marked Images" → call `ilst_renamemrkd`
3. Verify `Shift+R` palette rename triggers correctly
4. Test Alt+arrow nudging (should already work via keyboard handler)

### **Medium Effort (1-2 hours)**
1. Add submenu "Edit > Marked Images >" with RENAME, DELETE, BUILD TGA, LEAST SQUARE
2. Add "Palette > Show Histogram" with histogram display dialog
3. Add "Palette > Delete Unused Colors"

### **Optional Polish**
1. Add "Edit > Strip Edge" submenu with variants
2. Implement "Dither Replace" feature
3. Wire test function (|) to a debug console

---

## Files to Reference

- `/IT/itimg.asm` lines 289-394 — Menu definitions (MENUI, menu_mi arrays)
- `/IT/itimg.asm` lines 596-666 — Key bindings (key_t table)
- `/platform/imgui_overlay.cpp` — Current menu implementation
- `/IT/wmpstruc.inc` — Data structure definitions
