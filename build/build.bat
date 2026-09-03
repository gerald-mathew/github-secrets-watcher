@echo off
REM Build script for github-secrets-watcher C++ version
REM To be run from the build directory: .\build.bat
REM Assumes source files are in the parent directory (src)

echo Building github-secrets-watcher...

REM Change to the directory where this script is located (build)
cd /d "%~dp0"

REM Define source directory (parent of build, then src)
set "SRC_DIR=..\src"

REM Define library paths for MSYS2 UCRT64
set "LIBURL_LIBDIR=C:/msys64/ucrt64/lib"
set "LIBGIT2_LIBDIR=C:/msys64/ucrt64/lib"
set "LIBURL_INCLUDE=C:/msys64/ucrt64/include"
set "LIBGIT2_INCLUDE=C:/msys64/ucrt64/include"

REM Check if libcurl library exists
if not exist "%LIBURL_LIBDIR%/libcurl.dll.a" (
    echo Error: libcurl library not found at %LIBURL_LIBDIR%/libcurl.dll.a
    exit /b 1
)

REM Check if libgit2 library exists
if not exist "%LIBGIT2_LIBDIR%/libgit2.dll.a" (
    echo Error: libgit2 library not found at %LIBGIT2_LIBDIR%/libgit2.dll.a
    exit /b 1
)

REM Compile with C++20
g++ -std=c++20 -Wall -Wextra -I"%SRC_DIR%" -I"%LIBURL_INCLUDE%" -I"%LIBGIT2_INCLUDE%" ^
    "%SRC_DIR%\main.cpp" "%SRC_DIR%\github.cpp" "%SRC_DIR%\scanner.cpp" "%SRC_DIR%\utils.cpp" ^
    -L"%LIBURL_LIBDIR%" -L"%LIBGIT2_LIBDIR%" -lcurl -lgit2 ^
    -o github_secrets_watcher

if errorlevel 1 (
    echo Build failed.
    exit /b 1
) else (
    echo Build successful! Executable: github_secrets_watcher
)
