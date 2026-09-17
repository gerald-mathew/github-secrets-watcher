@echo off
setlocal EnableExtensions EnableDelayedExpansion

title github-secrets-watcher build
echo Building github-secrets-watcher...
cd /d "%~dp0"
set "SRC_DIR=..\src"

REM Define the fallback search directories. Used only when `where` is NOT
REM available, or when a tool cannot be found on PATH.
set "SEARCH_DIRS=C:\msys64 C:\tools\msys64 C:\Program Files\CMake\bin C:\Program Files\CMake C:\Program Files\Git\mingw64 C:\DevKit\mingw64 C:\DevKit\mingw32 C:\libs C:\dev-libs C:\"

REM Where.exe is present on normal Windows; if it isn't, we go straight to
REM the directory search in :find_tool (at the bottom of this file).
set "HAVE_WHERE="
where where >nul 2>&1 && set "HAVE_WHERE=1"

REM --------------------------------------------------------------
REM 1. Locate the tools (mirrors find_tool in build.sh).
REM --------------------------------------------------------------
call :find_tool cmake
set "CMAKE=!TOOL_FOUND!"

set "PKGCONFIG="
call :find_tool pkg-config
if defined TOOL_FOUND set "PKGCONFIG=!TOOL_FOUND!"
if not defined PKGCONFIG (
    call :find_tool pkgconf
    if defined TOOL_FOUND set "PKGCONFIG=!TOOL_FOUND!"
)

set "CXX="
call :find_tool g++
if defined TOOL_FOUND set "CXX=!TOOL_FOUND!"
if not defined CXX (
    call :find_tool clang++
    if defined TOOL_FOUND set "CXX=!TOOL_FOUND!"
)

REM --------------------------------------------------------------
REM 2. Preferred path: CMake (mirror of build.sh).
REM --------------------------------------------------------------
if defined CMAKE (
    echo [INFO] Found CMake: configuring, building and testing with it.
    echo [INFO] Configure...
    "!CMAKE!" -S ".." -B "build" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
    if errorlevel 1 (
        echo [ERROR] CMake configure failed.
        exit /b 1
    )
    echo [INFO] Build...
    "!CMAKE!" --build build --parallel
    if errorlevel 1 (
        echo [ERROR] CMake build failed.
        exit /b 1
    )
    set "CTEST="
    call :find_tool ctest
    if defined TOOL_FOUND set "CTEST=!TOOL_FOUND!"
    if defined CTEST (
        "!CTEST!" --test-dir build --output-on-failure
    ) else (
        echo [WARN] ctest not found; built successfully but tests not run.
    )
    echo Build successful! Executable: build\github_secrets_watcher.exe
    exit /b 0
)

REM --------------------------------------------------------------
REM 3. Fallback: direct compilation via pkg-config (mirror of
REM    build.sh). pkg-config lists the transitive link dependencies
REM    that bare -l flags omit.
REM --------------------------------------------------------------
if not defined PKGCONFIG (
    echo [ERROR] Neither CMake nor pkg-config/pkgconf was found on PATH.
    echo         Install a CMake toolchain or pkg-config plus the compiler.
    exit /b 1
)
if not defined CXX (
    echo [ERROR] No C++ compiler found on PATH ^(g++ or clang++^).
    exit /b 1
)

"!PKGCONFIG!" --exists libcurl libgit2 2>nul
if errorlevel 1 (
    echo [ERROR] libcurl/libgit2 developer files not found via pkg-config.
    echo         Install them, e.g. on MSYS2:
    echo         pacman -S mingw-w64-x86_64-curl mingw-w64-x86_64-libgit2
    exit /b 1
)

set "CFLAGS_FILE=%TEMP%\gsw_cflags.txt"
set "LIBS_FILE=%TEMP%\gsw_libs.txt"
"!PKGCONFIG!" --cflags libcurl libgit2 > "!CFLAGS_FILE!"
"!PKGCONFIG!" --libs   libcurl libgit2 > "!LIBS_FILE!"
set "CFLAGS="
set "LIBS="
set /p CFLAGS=<"!CFLAGS_FILE!"
set /p LIBS=<"!LIBS_FILE!"
del "!CFLAGS_FILE!" "!LIBS_FILE!" 2>nul

echo [INFO] Compiler: !CXX!  via: !PKGCONFIG!
"!CXX!" -std=c++20 -Wall -Wextra -I"!SRC_DIR!" !CFLAGS! ^
    "!SRC_DIR!\main.cpp" "!SRC_DIR!\github.cpp" "!SRC_DIR!\scanner.cpp" "!SRC_DIR!\utils.cpp" ^
    !LIBS! -o github_secrets_watcher.exe
if errorlevel 1 (
    echo [ERROR] Build failed.
    exit /b 1
)

echo Build successful! Executable: github_secrets_watcher.exe
echo Note: the unit tests need CMake ^(find_package Catch2^); this manual
echo       build produced the CLI only. Prefer the CMake path above.
exit /b 0

REM ==============================================================
REM :find_tool <name>  ->  sets TOOL_FOUND to the tool (or a path).
REM Tries `where` first; if that fails (or where is missing) it falls
REM back to guessing several well-known directories.
REM ==============================================================
:find_tool
set "TOOL_FOUND="
if defined HAVE_WHERE (
    where %~1 >nul 2>&1 && set "TOOL_FOUND=%~1"
)
if not defined TOOL_FOUND (
    for %%D in (%SEARCH_DIRS%) do (
        if not defined TOOL_FOUND if exist "%%~D\%~1.exe"      set "TOOL_FOUND=%%~D\%~1.exe"
        if not defined TOOL_FOUND if exist "%%~D\bin\%~1.exe"  set "TOOL_FOUND=%%~D\bin\%~1.exe"
        if not defined TOOL_FOUND if exist "%%~D\mingw64\bin\%~1.exe" set "TOOL_FOUND=%%~D\mingw64\bin\%~1.exe"
        if not defined TOOL_FOUND if exist "%%~D\ucrt64\bin\%~1.exe"  set "TOOL_FOUND=%%~D\ucrt64\bin\%~1.exe"
        if not defined TOOL_FOUND if exist "%%~D\mingw32\bin\%~1.exe" set "TOOL_FOUND=%%~D\mingw32\bin\%~1.exe"
    )
)
exit /b 0