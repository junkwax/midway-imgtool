# Contributing to midway-imgtool

`midway-imgtool` is a modern SDL2 + Dear ImGui re-host of a 1992 Midway DOS
image/sprite tool. This guide gets a new contributor from a fresh clone to a
working build and a clean pull request.

## Prerequisites

| Platform | Toolchain | SDL2 |
|----------|-----------|------|
| Windows  | Visual Studio 2022 (Desktop C++ workload) + CMake (the one bundled with VS is fine) | Auto-fetched by `build.ps1` |
| Linux    | GCC/Clang + CMake ≥ 3.20 | `sudo apt-get install libsdl2-dev` (or `dnf install SDL2-devel`) |
| macOS    | Xcode command-line tools + CMake ≥ 3.20 | `brew install sdl2` |

The project builds two targets: `imgtool` (the GUI) and `imgtool-cli`
(headless, `IMGTOOL_CLI_ONLY`).

## Building

### Windows

```powershell
powershell -ExecutionPolicy Bypass -File build.ps1
```

`build.ps1` locates VS 2022, downloads the matching SDL2 devel package into a
shared cache, configures CMake, builds Release, and copies `SDL2.dll` next to
the produced `imgtool.exe`. Pass `-Arch x86` for a 32-bit build.

### Linux / macOS

```bash
./build.sh            # configure + build Release into ./build
./build.sh clean      # wipe ./build first
```

### Via CMake presets (any platform)

Presets live in `CMakePresets.json`:

```bash
cmake --preset linux        # or macos, windows-x64, windows-x86
cmake --build --preset linux
```

> **Windows note:** the `windows-x64` / `windows-x86` presets read `SDL2_DIR`
> from the environment (point it at the SDL2 devel `cmake` folder). `build.ps1`
> sets this up for you; the presets are mainly for IDEs and repeat builds.

## Running the tests

```bash
cd build
ctest --output-on-failure
```

`sprite_sheet_import_test` always runs. `mk2_roundtrip_test` only builds when
you point CMake at a real `MKSTK.ASM`:

```bash
cmake -B build -DMK2_MKSTK_PATH=/path/to/MKSTK.ASM
```

## Continuous integration

Every push to `SDL-main` / `sdl-experimental` / `refactor/**` and **every pull
request** triggers `.github/workflows/c-cpp.yml`, which builds Windows
(x64 + x86), Linux, and macOS. A push of a `v*` tag additionally publishes a
GitHub Release with the platform artifacts; release notes come from the
matching `CHANGELOG.md` section. Make sure CI is green before requesting review.

## Branch & commit conventions

- `SDL-main` is the mainline; `sdl-experimental` is the integration branch most
  PRs target. Confirm the intended base before opening a PR.
- Branch per change: `refactor/...`, `feat/...`, `fix/...`.
- Write focused commits with a conventional-ish subject
  (`refactor(overlay): ...`, `fix(palette): ...`). Keep each commit building.

## Codebase orientation

- `platform/` — the app: SDL2/ImGui UI, IMG/PAL I/O, DOS-compat shims.
  - `imgui_overlay.cpp` is the large UI module currently being split up — see
    [`refactoring_plan.md`](refactoring_plan.md) before adding to it.
  - `palette_math.{h,cpp}`, `img_io.cpp`, `load2_verify.cpp`, `lod_parser.cpp`,
    `mk2_hitbox.cpp`, `mk2_fatality.cpp` — focused modules.
  - `shim_*.c` emulate legacy DOS behaviors for the ported business logic.
- `imgui/` — vendored Dear ImGui + SDL2 backends.
- `test/` — CTest-driven unit tests.
- Platform-specific code is guarded with `#ifdef _WIN32`; keep new OS-specific
  calls behind the same guards so the Linux/macOS builds stay green.

## Working on the imgui_overlay split

The overlay deconstruction follows a strict, low-risk loop so a half-finished
attempt can never leave the tree broken. Per slice:

1. Pick one cohesive, low-coupling group of functions.
2. Move it into a new `.cpp`/`.h` pair; un-`static` the moved symbols and
   declare them in the new header.
3. `#include` the new header from `imgui_overlay.cpp`; add the `.cpp` to
   `CMakeLists.txt`.
4. **Build green, then commit** — one slice per commit.

The compiler is the source of truth — do not extract symbols with regex/text
scripts. See [`refactoring_plan.md`](refactoring_plan.md) for the slice
checklist and ordering.
