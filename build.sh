#!/usr/bin/env bash
# build.sh — Build midway-imgtool on Linux/macOS.
#
# The Windows build uses build.ps1 (VS 2022 + auto-fetched SDL2). On
# Linux/macOS, SDL2 comes from the system package manager:
#   Debian/Ubuntu : sudo apt-get install libsdl2-dev
#   Fedora        : sudo dnf install SDL2-devel
#   macOS         : brew install sdl2
#
# Usage:
#   ./build.sh           # configure + build (Release) into ./build
#   ./build.sh clean     # remove the build directory first
set -euo pipefail

SOURCE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SOURCE_DIR/build"

if [[ "${1:-}" == "clean" ]]; then
    echo "Removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

# Prefer a named preset when this CMake supports them; fall back to plain
# flags for older CMake (< 3.20) that lacks preset support.
PRESET=""
case "$(uname -s)" in
    Darwin) PRESET="macos" ;;
    Linux)  PRESET="linux" ;;
esac

CMAKE_PREFIX_ARG=()
if [[ "$(uname -s)" == "Darwin" ]] && command -v brew >/dev/null 2>&1; then
    CMAKE_PREFIX_ARG=(-DCMAKE_PREFIX_PATH="$(brew --prefix)")
    export HOMEBREW_PREFIX="$(brew --prefix)"
fi

if [[ -n "$PRESET" ]] && cmake --list-presets >/dev/null 2>&1; then
    echo "[1/2] Configuring (preset: $PRESET)"
    cmake --preset "$PRESET"
    echo "[2/2] Building"
    cmake --build --preset "$PRESET" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
else
    echo "[1/2] Configuring (no preset; CMake $(cmake --version | head -1))"
    cmake -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release "${CMAKE_PREFIX_ARG[@]}" "$SOURCE_DIR"
    echo "[2/2] Building"
    cmake --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)"
fi

echo ""
echo "*** Build succeeded ***"
echo "EXE: $BUILD_DIR/imgtool"
echo "CLI: $BUILD_DIR/imgtool-cli"
