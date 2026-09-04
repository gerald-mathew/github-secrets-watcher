@echo off
setlocal EnableExtensions EnableDelayedExpansion

echo Building github-secrets-watcher...
cd /d "%~dp0"
set "SRC_DIR=..\src"

set "SEARCH_DIRS="C:\msys64\mingw64" "C:\msys64\usr\local" "C:\" "C:\msys64" "C:\msys64\ucrt64" "C:\msys64\mingw64" "C:\Program Files" "C:\Program Files (x86)" "C:\Program Files (x86)\Microsoft Visual Studio\2019\Community\VC\Tools\MSVC" "C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Tools\MSVC""

set "CURL_DIR="
set "CURL_NAME="
for %%D in (%SEARCH_DIRS%) do (
    for %%N in (libcurl.dll.a libcurl.lib) do (
        if not defined CURL_DIR if exist "%%~D\lib\%%N" (
            set "CURL_DIR=%%~D\lib"
            set "CURL_NAME=%%N"
        )
        if not defined CURL_DIR if exist "%%~D\%%N" (
            set "CURL_DIR=%%~D"
            set "CURL_NAME=%%N"
        )
    )
)

set "GIT2_DIR="
set "GIT2_NAME="
for %%D in (%SEARCH_DIRS%) do (
    for %%N in (libgit2.dll.a libgit2.lib) do (
        if not defined GIT2_DIR if exist "%%~D\lib\%%N" (
            set "GIT2_DIR=%%~D\lib"
            set "GIT2_NAME=%%N"
        )
        if not defined GIT2_DIR if exist "%%~D\%%N" (
            set "GIT2_DIR=%%~D"
            set "GIT2_NAME=%%N"
        )
    )
)

if not defined CURL_DIR (
    echo Error: libcurl library not found
    exit /b 1
)
if not defined GIT2_DIR (
    echo Error: libgit2 library not found
    exit /b 1
)

set "CURL_PREFIX=!CURL_DIR!"
if "!CURL_PREFIX:~-4!"=="\lib" set "CURL_PREFIX=!CURL_PREFIX:~0,-4!"
set "LIBURL_LIBDIR=!CURL_PREFIX!\lib"
set "LIBURL_INCLUDE=!CURL_PREFIX!\include"

set "GIT2_PREFIX=!GIT2_DIR!"
if "!GIT2_PREFIX:~-4!"=="\lib" set "GIT2_PREFIX=!GIT2_PREFIX:~0,-4!"
set "LIBGIT2_LIBDIR=!GIT2_PREFIX!\lib"
set "LIBGIT2_INCLUDE=!GIT2_PREFIX!\include"

if "!CURL_NAME:~-6!"==".dll.a" (set "CURL_LINK_FLAG=-lcurl") else (set "CURL_LINK_FLAG=libcurl.lib")
if "!GIT2_NAME:~-6!"==".dll.a" (set "GIT2_LINK_FLAG=-lgit2") else (set "GIT2_LINK_FLAG=libgit2.lib")
set "LINKER_LIBS=!CURL_LINK_FLAG! !GIT2_LINK_FLAG!"

set "compiler="
for %%C in (g++ clang++ cl) do (
    if not defined compiler (
        where %%C >nul 2>&1
        if not errorlevel 1 set "compiler=%%C"
    )
)
if not defined compiler (
    echo Error: No suitable compiler found ^(g++, clang++, or cl^)
    exit /b 1
)
echo Using compiler: !compiler!

if "!compiler!"=="cl" (
    set "COMPILE_CMD=!compiler! /std:c++20 /W3 /I"!LIBURL_INCLUDE!" /I"!LIBGIT2_INCLUDE!" /I"!SRC_DIR!" /LIBPATH:"!LIBURL_LIBDIR!" /LIBPATH:"!LIBGIT2_LIBDIR!" !SRC_DIR!\main.cpp !SRC_DIR!\github.cpp !SRC_DIR!\scanner.cpp !SRC_DIR!\utils.cpp !LINKER_LIBS! /Fe:github_secrets_watcher.exe"
) else (
    set "COMPILE_CMD=!compiler! -std=c++20 -Wall -Wextra -I"!SRC_DIR!" -I"!LIBURL_INCLUDE!" -I"!LIBGIT2_INCLUDE!" -L"!LIBURL_LIBDIR!" -L"!LIBGIT2_LIBDIR!" !SRC_DIR!/main.cpp !SRC_DIR!/github.cpp !SRC_DIR!/scanner.cpp !SRC_DIR!/utils.cpp !LINKER_LIBS! -o github_secrets_watcher"
)

!COMPILE_CMD!
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)
setlocal DisableDelayedExpansion
echo Build successful! Executable: github_secrets_watcher
exit /b 0