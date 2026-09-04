#!/bin/bash
# Build script for github-secrets-watcher C++ version
# To be run from the build directory: ./build.sh
# Assumes source files are in the parent directory (src)
# Searches for libcurl and libgit2 libraries in common locations
# Tries compilers in order: g++, clang++, cl (MSVC)
# Also supports Termux and Linux environments

echo "Building github-secrets-watcher..."

# Change to the directory where this script is located (build)
cd "$(dirname "$0")"

# Define source directory (parent of build, then src)
SRC_DIR="../src"

# Determine the environment and set SEARCH_DIRS accordingly
if [ -d "/c" ] || [ -n "$MSYSTEM" ]; then
    # We are in Windows/MSYS2
    SEARCH_DIRS=(
        "/mingw64"
        "/usr/local"
        "/c"
        "/c/msys64"
        "/c/msys64/ucrt64"
        "/c/msys64/mingw64"
        "/c/Program Files"
        "/c/Program Files (x86)"
        "/c/Program Files (x86)/Microsoft Visual Studio/2019/Community/VC/Tools/MSVC"
        "/c/Program Files (x86)/Microsoft Visual Studio/2022/Community/VC/Tools/MSVC"
    )
    # Library names for Windows
    CURL_NAMES=("libcurl.dll.a" "libcurl.lib")
    GIT2_NAMES=("libgit2.dll.a" "libgit2.lib")
else
    # We are in a Unix-like environment (Termux, Linux, etc.)
    SEARCH_DIRS=(
        "$PREFIX/lib"
        "/data/data/com.termux/files/usr/lib"
        "/usr/lib"
        "/usr/local/lib"
        "/lib"
        "/usr/lib/x86_64-linux-gnu"
        "/usr/lib/i386-linux-gnu"
        "/usr/lib/arm-linux-gnueabihf"
        "/usr/lib/aarch64-linux-gnu"
    )
    # Library names for Unix-like systems
    CURL_NAMES=("libcurl.so" "libcurl.so.4" "libcurl.a")
    GIT2_NAMES=("libgit2.so" "libgit2.so.0" "libgit2.a")
fi

# Function to find a library (supports multiple possible names)
find_library() {
    local libbase="$1"
    shift
    local possible_names=("$@")
    for dir in "${SEARCH_DIRS[@]}"; do
        for name in "${possible_names[@]}"; do
            # Check in lib subdirectory
            if [ -f "$dir/lib/$name" ]; then
                echo "$dir"
                return 0
            fi
            # Check directly in directory
            if [ -f "$dir/$name" ]; then
                echo "$dir"
                return 0
            fi
        done
    done
    return 1
}

# Try to find the libraries
CURL_DIR=$(find_library "libcurl" "${CURL_NAMES[@]}")
GIT2_DIR=$(find_library "libgit2" "${GIT2_NAMES[@]}")

if [ -z "$CURL_DIR" ]; then
    echo "Error: libcurl library not found"
    exit 1
fi

if [ -z "$GIT2_DIR" ]; then
    echo "Error: libgit2 library not found"
    exit 1
fi

# Determine the prefix for curl and git2 (assuming standard layout: prefix/{lib,include})
if [ -d "/c" ] || [ -n "$MSYSTEM" ]; then
    # Windows/MSYS2
    if [[ "$CURL_DIR" == */lib ]]; then
        CURL_PREFIX="${CURL_DIR%/lib}"
    else
        CURL_PREFIX="$CURL_DIR"
    fi
    LIBURL_LIBDIR="$CURL_PREFIX/lib"
    LIBURL_INCLUDE="$CURL_PREFIX/include"

    if [[ "$GIT2_DIR" == */lib ]]; then
        GIT2_PREFIX="${GIT2_DIR%/lib}"
    else
        GIT2_PREFIX="$GIT2_DIR"
    fi
    LIBGIT2_LIBDIR="$GIT2_PREFIX/lib"
    LIBGIT2_INCLUDE="$GIT2_PREFIX/include"
else
    # Unix-like (Termux/Linux)
    # For these systems, the lib and include directories are typically directly under the prefix
    LIBURL_LIBDIR="$CURL_DIR"
    LIBURL_INCLUDE="$(dirname "$(dirname "$CURL_DIR")")/include"
    LIBGIT2_LIBDIR="$GIT2_DIR"
    LIBGIT2_INCLUDE="$(dirname "$(dirname "$GIT2_DIR")")/include"
fi

# Determine what we actually found for curl to set the link flag
found_curl_name=""
for name in "${CURL_NAMES[@]}"; do
    if [ -n "$LIBURL_LIBDIR" ] && [ -f "$LIBURL_LIBDIR/$name" ]; then
        found_curl_name="$name"
        break
    elif [ -f "$CURL_DIR/$name" ]; then
        found_curl_name="$name"
        break
    fi
done

found_git2_name=""
for name in "${GIT2_NAMES[@]}"; do
    if [ -n "$LIBGIT2_LIBDIR" ] && [ -f "$LIBGIT2_LIBDIR/$name" ]; then
        found_git2_name="$name"
        break
    elif [ -f "$GIT2_DIR/$name" ]; then
        found_git2_name="$name"
        break
    fi
done

# Set link flags based on what we found
if [ -d "/c" ] || [ -n "$MSYSTEM" ]; then
    # Windows/MSYS2
    if [[ "$found_curl_name" == *.dll.a ]]; then
        CURL_LINK_FLAG="-lcurl"
    elif [[ "$found_curl_name" == *.lib ]]; then
        CURL_LINK_FLAG="libcurl.lib"
    else   # .a or .lib (static)
        CURL_LINK_FLAG="-lcurl"
    fi

    if [[ "$found_git2_name" == *.dll.a ]]; then
        GIT2_LINK_FLAG="-lgit2"
    elif [[ "$found_git2_name" == *.lib ]]; then
        GIT2_LINK_FLAG="libgit2.lib"
    else   # .a or .lib (static)
        GIT2_LINK_FLAG="-lgit2"
    fi
else
    # Unix-like (Termux/Linux)
    if [[ "$found_curl_name" == *.so* ]]; then
        CURL_LINK_FLAG="-lcurl"
    else   # .a (static)
        CURL_LINK_FLAG="-lcurl"
    fi

    if [[ "$found_git2_name" == *.so* ]]; then
        GIT2_LINK_FLAG="-lgit2"
    else   # .a (static)
        GIT2_LINK_FLAG="-lgit2"
    fi
fi

LINKER_LIBS="$CURL_LINK_FLAG $GIT2_LINK_FLAG"

# Define compilers to try in order
COMPILERS=("g++" "clang++" "cl")

# Try each compiler until one works
compiler=""
for c in "${COMPILERS[@]}"; do
    if command -v $c &> /dev/null; then
        compiler=$c
        break
    fi
done

if [ -z "$compiler" ]; then
    echo "Error: No suitable compiler found (g++, clang++, or cl)"
    exit 1
fi

echo "Using compiler: $compiler"

# Set compiler flags based on compiler and library style
if [ "$compiler" = "cl" ]; then
    # MSVC compiler flags
    INCLUDE_FLAG="/I"
    LIBPATH_FLAG="/LIBPATH:"
    OUTPUT_FLAG="/Fe:"
    # Warning flags for MSVC (approximately equivalent to -Wall -Wextra)
    WARNING_FLAGS="/W3"
    # C++20 standard flag
    STD_FLAG="/std:c++20"
    # Define source files with proper paths
    SRC_FILES="$SRC_DIR\\main.cpp $SRC_DIR\\github.cpp $SRC_DIR\\scanner.cpp $SRC_DIR\\utils.cpp"
    # Convert paths to Windows style for MSVC
    SRC_FILES_WIN=$(echo "$SRC_FILES" | sed 's|/|\\|g')
    LIBURL_INCLUDE_WIN=$(echo "$LIBURL_INCLUDE" | sed 's|/|\\|g')
    LIBGIT2_INCLUDE_WIN=$(echo "$LIBGIT2_INCLUDE" | sed 's|/|\\|g')
    LIBURL_LIBDIR_WIN=$(echo "$LIBURL_LIBDIR" | sed 's|/|\\|g')
    LIBGIT2_LIBDIR_WIN=$(echo "$LIBGIT2_LIBDIR" | sed 's|/|\\|g')

    # Build the command
    COMPILE_CMD="$compiler $STD_FLAG $WARNING_FLAGS "
    COMPILE_CMD+="$INCLUDE_FLAG\"$LIBURL_INCLUDE_WIN\" $INCLUDE_FLAG\"$LIBGIT2_INCLUDE_WIN\" "
    COMPILE_CMD+="$INCLUDE_FLAG\"$SRC_DIR\" "
    COMPILE_CMD+="$LIBPATH_FLAG\"$LIBURL_LIBDIR_WIN\" $LIBPATH_FLAG\"$LIBGIT2_LIBDIR_WIN\" "
    COMPILE_CMD+="$SRC_FILES_WIN $LINKER_LIBS $OUTPUT_FLAGgithub_secrets_watcher.exe"
else
    # GCC/Clang compiler flags
    INCLUDE_FLAG="-I"
    LIBPATH_FLAG="-L"
    OUTPUT_FLAG="-o"
    WARNING_FLAGS="-Wall -Wextra"
    STD_FLAG="-std=c++20"

    # Build the command
    COMPILE_CMD="$compiler $STD_FLAG $WARNING_FLAGS "
    COMPILE_CMD+="$INCLUDE_FLAG\"$SRC_DIR\" $INCLUDE_FLAG\"$LIBURL_INCLUDE\" $INCLUDE_FLAG\"$LIBGIT2_INCLUDE\" "
    COMPILE_CMD+="$LIBPATH_FLAG\"$LIBURL_LIBDIR\" $LIBPATH_FLAG\"$LIBGIT2_LIBDIR\" "
    COMPILE_CMD+="$SRC_DIR/main.cpp $SRC_DIR/github.cpp $SRC_DIR/scanner.cpp $SRC_DIR/utils.cpp "
    COMPILE_CMD+="$LINKER_LIBS $OUTPUT_FLAG github_secrets_watcher"
fi

# Execute the compilation
eval "$COMPILE_CMD"

if [ $? -eq 0 ]; then
    echo "Build successful! Executable: github_secrets_watcher"
else
    echo "Build failed."
    exit 1
fi