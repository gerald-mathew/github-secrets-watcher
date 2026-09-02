#!/bin/bash
# Build script for github-secrets-watcher C++ version
# To be run from the build directory: ./build.sh
# Assumes source files are in the parent directory (src)

echo "Building github-secrets-watcher..."

# Change to the directory where this script is located (build)
cd "$(dirname "$0")"

# Define source directory (parent of build, then src)
SRC_DIR="../src"

# Libcurl include and library paths for MSYS2 UCRT64
LIBURL_INCLUDE="/c/msys64/ucrt64/include"
LIBURL_LIBDIR="/c/msys64/ucrt64/lib"

# Check if libcurl library exists (import library for dynamic linking)
if [ ! -f "$LIBURL_LIBDIR/libcurl.dll.a" ]; then
    echo "Error: libcurl library not found at $LIBURL_LIBDIR/libcurl.dll.a"
    exit 1
fi

# Compile with C++20
g++ -std=c++20 -Wall -Wextra -I"$SRC_DIR" -I"$LIBURL_INCLUDE" \
    "$SRC_DIR/main.cpp" "$SRC_DIR/github.cpp" "$SRC_DIR/scanner.cpp" "$SRC_DIR/utils.cpp" \
    -L"$LIBURL_LIBDIR" -lcurl \
    -o github_secrets_watcher

if [ $? -eq 0 ]; then
    echo "Build successful! Executable: github_secrets_watcher"
else
    echo "Build failed."
    exit 1
fi
