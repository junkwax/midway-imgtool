# Building midway-imgtool

This document covers building `imgtool` from source on both **Linux** (GCC + JWasm)
and **Windows** (MSVC + MASM).

The project is a 32-bit x86 application.  It combines original DOS-era MASM-syntax
assembly with a thin SDL2 C shim layer.

> **Note:** The `build.bat` and `build.ps1` scripts are provided for convenience but
> are no longer actively maintained. For current builds, use CMake directly as shown
> below. See [CLEANUP.md](CLEANUP.md) for details on obsolete files.

---

## Linux

### Requirements

| Tool | Version | Install |
|---|---|---|
| GCC (multilib) | any recent | `sudo apt install gcc gcc-multilib g++-multilib` |
| CMake | ≥ 3.20 | `sudo apt install cmake` |
| SDL2 (i386) | 2.x | `sudo apt install libsdl2-dev:i386` |
| JWasm | 2.12 (patched, see below) | build from source |

Enable the i386 architecture if you have not already:

```bash
sudo dpkg --add-architecture i386
sudo apt update
```

### JWasm — build the patched version

The standard `jwasm` package from `apt` has a bug in flat-model builds: it emits
`ASSUME CS:ERROR` when entering `.data` sections, which causes label resolution
errors (`A2183 / A2108`) in the assembly sources.

You must build JWasm from the patched source tree.

**Clone and patch:**

```bash
git clone https://github.com/JWasm/JWasm.git
cd JWasm
```

Edit `simsegm.c`.  Find the `SIM_DATA` / `SIM_DATA_UN` / `SIM_CONST` case block
(around line 261) and change:

```c
// BEFORE
AddLineQueueX( "%r %r:ERROR", T_ASSUME, T_CS );
```

```c
// AFTER — skip ASSUME CS:ERROR in flat model (fixes A2183 in .data sections)
if ( ModuleInfo.model != MODEL_FLAT )
    AddLineQueueX( "%r %r:ERROR", T_ASSUME, T_CS );
```

The full diff:

```diff
--- a/simsegm.c
+++ b/simsegm.c
@@ -261,7 +261,10 @@ ret_code SimplifiedSegDir( int i, struct asm_tok tokenarray[] )
     case SIM_DATA_UN: /* .data? */
     case SIM_CONST:   /* .const */
         SetSimSeg( type, name );
-        AddLineQueueX( "%r %r:ERROR", T_ASSUME, T_CS );
+        /* MASM/flat-model: don't set CS:ERROR in data sections for FLAT,
+         * otherwise named labels (`:` suffix) in .data fail A2183 */
+        if ( ModuleInfo.model != MODEL_FLAT )
+            AddLineQueueX( "%r %r:ERROR", T_ASSUME, T_CS );
         if ( name || (!init) )
             AddToDgroup( type, name );
         break;
```

**Build JWasm:**

```bash
make -f GccUnix.mak
```

The binary is output to `GccUnixR/jwasm`.

**Install:**

```bash
sudo cp GccUnixR/jwasm /usr/local/bin/jwasm
```

CMake will find it automatically via `find_program(... NAMES jwasm uasm ...)`.

### Build imgtool

```bash
cd /path/to/midway-imgtool
mkdir build-linux && cd build-linux
cmake ..
cmake --build .
```

The binary is `build-linux/imgtool`.  Run it from the directory that contains
your `.img` files, or navigate to them using the built-in file browser.

---

## Windows

### Requirements

| Tool | Notes |
|---|---|
| Visual Studio 2022 | Community edition is fine. Install the **Desktop development with C++** workload, which includes MASM. |
| PowerShell | Included with Windows. |
| Internet access | The build script downloads SDL2 automatically. |

No MASM patch is required — MASM handles flat-model `.data` sections differently
from JWasm and the assembly sources were written to be compatible with both.

### Build with CMake (Recommended)

```cmd
mkdir build-win32
cd build-win32
cmake .. -G "Visual Studio 17 2022" -A Win32
cmake --build . --config Release
```

Output: `build-win32\Release\imgtool.exe`

### Build with convenience scripts (Legacy)

Alternatively, you can use the included PowerShell or batch scripts to automate
the above steps, including SDL2 download:

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

Or the classic batch script:

```cmd
build.bat
```

Both scripts:

1. Automatically locate the VS 2022 x86 build environment via `vswhere.exe`
2. Download the SDL2 VC developer package from GitHub (cached in `%LOCALAPPDATA%\imgtool-build\deps`)
3. Configure and build with CMake targeting Win32

Output: `%LOCALAPPDATA%\imgtool-build\build\Release\imgtool.exe`

`SDL2.dll` and `it.hlp` are copied next to the EXE automatically.

### Running

From the output directory:

```cmd
imgtool.exe
```

Or copy `imgtool.exe`, `SDL2.dll`, and `it.hlp` to any folder of your choice
and run from there. There is no installer and no registry use.

---

## Notes

- The build is always **32-bit** (x86).  On Linux, `-m32` is passed to GCC;
  on Windows, CMake targets `Win32`.
- The assembly sources use MASM syntax and are assembled by MASM (Windows) or
  the patched JWasm (Linux).
- `SDL2.dll` must be in the same directory as `imgtool.exe` on Windows.
- `it.hlp` must be in the same directory as the executable (it's what the
  `h` key loads).
- On Linux the binary links against the i386 SDL2 shared library
  (`/usr/lib/i386-linux-gnu/libSDL2.so`).
