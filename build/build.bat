@echo off
REM Build script for github-secrets-watcher C++ version
REM To be run from the build directory: .\build\build.bat
REM Assumes source files are in the parent directory (src)

echo Building github-secrets-watcher...

REM Change to the directory where this script is located (build)
cd /d "%~dp0"

REM Define source directory (parent of build, then src)
set "SRC_DIR=..\src"

REM Compile with C++20
g++ -std=c++20 -Wall -Wextra -I"%SRC_DIR%" ^
    "%SRC_DIR%\main.cpp" "%SRC_DIR%\github.cpp" "%SRC_DIR%\scanner.cpp" "%SRC_DIR%\utils.cpp" ^
    -L/c/msys64/ucrt64/lib -lcurl ^
    -o github_secrets_watcher.exe

if errorlevel 1 (
    echo Build failed.
    exit /b 1
) else (
    echo Build successful! Executable: github_secrets_watcher.exe
    echo Location: %cd%\github_secrets_watcher.exe
)

REM Optional: run a quick test if an argument is provided
if "%~1" neq "" (
    if "%~1"=="test" (
        echo.
        echo Testing with username: Gerald-Mathew
        github_secrets_watcher.exe scan --username Gerald-Mathew --depth 5 --max-repos 2
    )
)
