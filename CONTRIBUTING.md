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
  - `imgui_overlay.cpp` is now a thin overlay coordinator. Prefer adding new UI
    code to the owning `ui_*` module instead of growing the coordinator again.
  - `palette_math.{h,cpp}`, `img_io.cpp`, `load2_verify.cpp`, `lod_parser.cpp`,
    `mk2_hitbox.cpp`, `mk2_fatality.cpp` — focused modules.
  - `shim_*.c` emulate legacy DOS behaviors for the ported business logic.
- `imgui/` — vendored Dear ImGui + SDL2 backends.
- `test/` — CTest-driven unit tests.
- Platform-specific code is guarded with `#ifdef _WIN32`; keep new OS-specific
  calls behind the same guards so the Linux/macOS builds stay green.

## Working on UI modules

Keep UI changes close to the module that owns the behavior:

- `ui_canvas.*` — canvas drawing, selection, paste/free-transform, World View,
  and sprite interaction.
- `ui_modals.*` — dialogs, import/export flows, and confirmation prompts.
- `ui_palette.*` — palette panels, palette operations, color picking, and HSL
  controls.
- `ui_main.*` — main window layout, menu bar, image list, and high-level edit
  actions.
- `ui_tools.*`, `ui_timeline.*`, `ui_undo.*`, and `ui_autochop.*` own their
  named subsystems.

When moving or adding UI code, build and run CTest before committing. The
compiler is the source of truth; avoid one-off regex/text scripts for source
rewrites.
