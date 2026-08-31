# GitHub Secrets Watcher - C++ Version

A command-line tool that scans public GitHub repositories for accidentally committed environment files (like `.env`, config files with secrets, etc.) in the Git history and reports findings (read-only, no modifications).

## Features

- Scans public repositories for a given GitHub username
- Checks Git history for files matching environment/configuration patterns
- Excludes common directories (like `node_modules`, `.git`, `dist`, etc.) to reduce noise
- Provides direct links to the exact commit where each file appears
- Validates commit hashes and URL-encodes paths to prevent 404 errors
- Read-only operations only - does not modify any repositories
- Optional GitHub token for higher API rate limits

## Requirements

- C++17 compiler (g++, clang++, or MSVC)
- libcurl development library
- jsoncpp development library

### On Ubuntu/Debian
```bash
sudo apt-get install build-essential libcurl4-openssl-dev libjsoncpp-dev
```

### On CentOS/RHEL
```bash
sudo yum groupinstall "Development Tools"
sudo yum install libcurl-devel jsoncpp-devel
```

### On macOS (using Homebrew)
```bash
brew install curl jsoncpp
```

### On Windows (using MSYS2)
```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-libcurl mingw-w64-x86_64-jsoncpp
```

## Building

### Using CMake (recommended)
```bash
mkdir build && cd build
cmake ..
make
# The executable will be in the current directory
```

### Using the provided build script (Linux/macOS/WSL/MSYS2)
```bash
chmod +x build.sh
./build.sh
# The executable will be github_secrets_watcher in the current directory
```

### Manual compilation
```bash
g++ -std=c++17 -Wall -Wextra \
    main.cpp github.cpp scanner.cpp utils.cpp \
    $(pkg-config --cflags --libs libcurl jsoncpp) \
    -o github_secrets_watcher
```

## Usage

```bash
# Basic scan (no token needed for public repos)
./github_secrets_watcher scan --username YOUR_USERNAME

# With token for higher API rate limits
./github_secrets_watcher scan --username YOUR_USERNAME --token YOUR_PERSONAL_ACCESS_TOKEN

# Optional parameters
--depth <NUM>      Commits to scan in history (default: 100)
--max-repos <NUM>  Maximum repositories to scan (default: all)
```

## Example Output

```
[1/5] Scanning: my-web-app
  📥 Cloning (depth=100)...
  🔎 Scanning history...
  ⚠️  Found 3 potential environment/configuration files:
     - .env
       🔗 https://github.com/username/my-web-app/blob/a1b2c3d4e5f6789012345678901234567890abcd/.env
     - config/production.json
       🔗 https://github.com/username/my-web-app/blob/f6e5d4c3b2a1098765432109876543210fedcba9/config/production.json
     - settings.local.yml
       🔗 https://github.com/username/my-web-app/blob/1a2b3c4d5e6f789012345678901234567890abcdef/settings.local.yml
```

Note: The links point to the exact commit where each file was found, allowing immediate location in the repository history.

## Safety Notes

- Only scan repositories you own or have explicit permission to scan
- Tool performs read-only operations only (no modifications)
- Intended for defensive security awareness and learning
- Review what the tool does before running it on any repositories

## License

This tool is provided as-is for educational purposes.