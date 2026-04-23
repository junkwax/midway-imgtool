# midway-imgtool Obsolete Files & Cleanup

**Status:** SDL2/ImGui port complete (Phase 6). The following files are no longer needed for the current Windows/Linux build pipeline.

---

## Files Recommended for Removal

### 1. `img_convert.py` (standalone pre-2.x converter)
- **Status:** Superseded by runtime conversion in `platform/shim_file.c`
- **Why:** The SDL port handles pre-2.x IMG auto-conversion automatically on file open, displaying a MessageBox confirmation
- **References:** See README.md line 94–95 and QUICKSTART.md
- **Action:** Safe to delete — no build or documentation references remain

### 2. `WATCOM/` directory (1.8 MB archive)
- **Status:** Obsolete DOS toolchain
- **Why:** The original DOS build (on `main` branch) used Watcom C/C++. The SDL2 port uses GCC (Linux) and MSVC (Windows)
- **Contents:** `WATCOM.7z` — archived Watcom compiler binaries
- **Action:** Safe to delete — no CMakeLists.txt references, deprecated for 6+ months

### 3. `build.bat` & `build.ps1` (Windows build scripts)
- **Status:** Legacy scripts that predate `CMakeLists.txt`
- **Why:** Modern builds use CMake on both Windows and Linux; these scripts are convenience wrappers that are no longer maintained
- **Impact:** Users should follow BUILD.md (cmake + cmake --build) instead
- **Action:** Consider keeping for reference, but mark as deprecated in BUILD.md or archive them

---

## Already Fixed

✅ **`main.png` deletion** — Removed in commit `fccdaef`; README updated to use `imgtool.png`

---

## Documentation Updates Completed

✅ **README.md** — Fixed image link from `main.png` → `imgtool.png`

---

## Recommended Next Steps

1. **Delete obsolete files:**
   ```bash
   rm img_convert.py
   rm -rf WATCOM/
   ```

2. **Update BUILD.md** — Add a note that `build.bat`/`build.ps1` are deprecated; recommend direct CMake usage

3. **Commit cleanup:**
   ```bash
   git add -u
   git commit -m "Remove obsolete DOS-era toolchain and pre-2.x converter

   - Delete img_convert.py (superseded by runtime conversion in shim_file.c)
   - Delete WATCOM/ directory (DOS toolchain no longer needed)
   - These were not referenced in any build pipeline or documentation
   
   Co-Authored-By: Claude Sonnet 4.6 <noreply@anthropic.com>"
   ```

---

## Build Pipeline Summary (Current)

| Platform | Toolchain | Entry Point |
|----------|-----------|------------|
| **Windows** | MSVC + MASM + CMake | `build.ps1` or direct `cmake` |
| **Linux** | GCC (multilib) + JWasm (patched) + CMake | `cmake .. && cmake --build .` |

Both pipelines are fully documented in BUILD.md.
