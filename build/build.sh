#!/bin/bash
# Build script for github-secrets-watcher C++ version.
# Cross-platform (Linux, macOS, WSL, MSYS2, Termux). To be run from the
# build directory: ./build.sh
#
# Strategy: locate each tool with `command -v` (the Unix twin of Windows'
# `where`), prefer CMake, and let pkg-config answer where the libraries are -
# it also lists the transitive link dependencies (ssl, crypto, z, ws2_32, ...)
# that bare -l flags omit. A small SEARCH_DIRS fallback covers setups where
# the tools are installed outside PATH (the twin of build.bat's guessing).

set -e
cd "$(dirname "$0")"
SRC_DIR="../src"
PROJ_ROOT="$(cd .. && pwd)"

# Well-known directories used only when a tool is NOT on PATH (the Unix
# counterpart of build.bat's SEARCH_DIRS fallback for `where`).
SEARCH_DIRS=(
    "/usr/bin" "/usr/local/bin" "/opt/local/bin" "/opt/homebrew/bin"
    "${HOMEBREW_PREFIX%/}/bin" "${MSYSTEM_PREFIX%/}/bin" "$PREFIX/bin"
    "/mingw64/bin" "/mingw32/bin" "/ucrt64/bin"
    "/c/msys64/mingw64/bin" "/c/msys64/ucrt64/bin" "/c/tools/msys64/mingw64/bin"
    "/data/data/com.termux/files/usr/bin"
)

# Locate the first of the given tools. Tries `command -v` (the Unix twin of
# Windows' `where`); if that fails it guesses across SEARCH_DIRS.
find_tool() {
    local tool dir
    for tool in "$@"; do
        if command -v "$tool" >/dev/null 2>&1; then
            echo "$tool"
            return 0
        fi
    done
    for dir in "${SEARCH_DIRS[@]}"; do
        for tool in "$@"; do
            if [ -x "$dir/$tool" ]; then
                echo "$dir/$tool"
                return 0
            fi
        done
    done
    return 1
}

echo "Building github-secrets-watcher..."

CMAKE=$(find_tool cmake)

# Preferred path: CMake. It drives pkg-config for libcurl/libgit2, finds
# Catch2 through find_package, and builds the executable *and* the unit
# tests in one shot.
if [ -n "$CMAKE" ]; then
    echo "[INFO] Found CMake: configuring, building and testing with it."
    "$CMAKE" -S "$PROJ_ROOT" -B "$PROJ_ROOT/build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    "$CMAKE" --build "$PROJ_ROOT/build" --parallel
    if CTEST=$(find_tool ctest); then
        "$CTEST" --test-dir "$PROJ_ROOT/build" --output-on-failure
    else
        echo "[WARN] ctest not found; built successfully but tests not run."
    fi
    echo "Build successful! Executable: $PROJ_ROOT/build/github_secrets_watcher"
    exit 0
fi

PKGCONFIG=$(find_tool pkg-config pkgconf)
CXX=$(find_tool g++ clang++)

if [ -z "$PKGCONFIG" ]; then
    echo "[ERROR] Neither CMake nor pkg-config/pkgconf was found on PATH." >&2
    echo "        Install a CMake toolchain or pkg-config plus the compiler." >&2
    exit 1
fi
if [ -z "$CXX" ]; then
    echo "[ERROR] No C++ compiler found on PATH (g++ or clang++)." >&2
    exit 1
fi

# Library flags come from pkg-config, not from a hardcoded directory list.
if ! "$PKGCONFIG" --exists libcurl libgit2; then
    echo "[ERROR] libcurl/libgit2 developer files not found via pkg-config." >&2
    echo "        Install them, e.g." >&2
    echo "        Ubuntu:   apt-get install libcurl4-openssl-dev libgit2-dev" >&2
    echo "        macOS:    brew install curl libgit2" >&2
    echo "        MSYS2:    pacman -S mingw-w64-x86_64-curl mingw-w64-x86_64-libgit2" >&2
    exit 1
fi
CFLAGS=$("$PKGCONFIG" --cflags libcurl libgit2)
LIBS=$("$PKGCONFIG" --libs libcurl libgit2)

echo "[INFO] Compiler: $CXX  via: $PKGCONFIG"
# shellcheck disable=SC2086 # intentional word-splitting for compiler flags
"$CXX" -std=c++20 -Wall -Wextra -I"$SRC_DIR" $CFLAGS \
    "$SRC_DIR/main.cpp" "$SRC_DIR/github.cpp" "$SRC_DIR/scanner.cpp" "$SRC_DIR/utils.cpp" \
    $LIBS -o github_secrets_watcher

echo "Build successful! Executable: github_secrets_watcher"
echo "Note: the unit tests need CMake (find_package Catch2); this manual"
echo "      build produced the CLI only. Prefer the CMake path above."