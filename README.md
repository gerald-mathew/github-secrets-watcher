# GitHub Secrets Watcher - C++ Version

A command-line tool that scans GitHub repositories (public and private with token) for accidentally committed environment files (like `.env`, config files with secrets, etc.) in the Git history and reports findings (read-only, no modifications).

## Features

- Scans public and private repositories (with token) for a given GitHub username
- Checks Git history for environment/configuration files across multiple branches (configurable depth)
- Excludes common directories (like `node_modules`, `.git`, `dist`, etc.) to reduce noise
- Provides direct links to the exact commit where each file appears (with URL-encoding for safety)
- Validates commit hash format (40 hex characters) to prevent broken links
- Read-only operations only - does not modify any repositories
- Optional GitHub token for higher API rate limits and access to private repositories
- Multi-threaded repository scanning for improved performance (configurable thread count)
- Configurable output formats: human-readable text, machine-readable JSON, or CSV
- Colored terminal output for better readability (ANSI colors, Windows VT processing enabled)
- Memory-efficient stream-based processing
- Modern C++20 standard

## Requirements

- C++20 compiler (g++, clang++, or MSVC)
- libcurl development library
- libgit2 development library (version 1.9+)
- nlohmann/json (single header included in the repository)

### On Ubuntu/Debian

```bash
sudo apt-get install build-essential libcurl4-openssl-dev libgit2-dev
```

*Note: nlohmann/json is provided as a single header (json.hpp) in the src/ directory.*

### On CentOS/RHEL

```bash
sudo yum groupinstall "Development Tools"
sudo yum install libcurl-devel libgit2-devel
```

*Note: nlohmann/json is provided as a single header (json.hpp) in the src/ directory.*

### On macOS (using Homebrew)

```bash
brew install curl libgit2
```

*Note: nlohmann/json is provided as a single header (json.hpp) in the src/ directory.*

### On Windows (using MSYS2)

```bash
pacman -S mingw-w64-x86_64-toolchain mingw-w64-x86_64-libcurl mingw-w64-x86_64-libgit2
```

*Note: nlohmann/json is provided as a single header (json.hpp) in the src/ directory.*

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

### Using the provided build script (Windows)

```cmd
build.bat
# The executable will be github_secrets_watcher.exe in the current directory
```

### Manual compilation

```bash
g++ -std=c++20 -Wall -Wextra -Isrc \
    src/main.cpp src/github.cpp src/scanner.cpp src/utils.cpp \
    $(pkg-config --cflags --libs libcurl) \
    -o github_secrets_watcher
```

## Usage

**From the build directory:**

```bash
# Basic scan (no token needed for public repos)
./github_secrets_watcher scan -u YOUR_USERNAME

# With token for higher API rate limits and access to private repos
./github_secrets_watcher scan -u YOUR_USERNAME -t YOUR_PERSONAL_ACCESS_TOKEN

# Scan private repositories (requires token)
./github_secrets_watcher scan -u YOUR_USERNAME -t YOUR_PERSONAL_ACCESS_TOKEN -p

# Specify number of threads for parallel scanning (default: hardware concurrency)
./github_secrets_watcher scan -u YOUR_USERNAME -n 4

# Specify output format (text, json, csv)
./github_secrets_watcher scan -u YOUR_USERNAME -f json -o results.json

# Enable verbose output (shows detailed progress for each repository with timestamps)
./github_secrets_watcher scan -u YOUR_USERNAME -v

# Scan only a specific repository
./github_secrets_watcher scan -u YOUR_USERNAME --repo REPO_NAME
./github_secrets_watcher scan -u YOUR_USERNAME -r REPO_NAME

# Scan only specific repositories (comma-separated list)
./github_secrets_watcher scan -u YOUR_USERNAME --repos "repo1,repo2,repo3"
./github_secrets_watcher scan -u YOUR_USERNAME -R "repo1,repo2,repo3"

# Optional parameters (long and short forms available)
-d, --depth <NUM>      Commits to scan in history (default: 100)
-m, --max-repos <NUM>  Maximum repositories to scan (default: all)
-p, --include-private  Include private repositories (requires token)
-n, --threads <NUM>    Number of threads to use for scanning (default: hardware concurrency)
-f, --format <FMT>     Output format: text, json, or csv (default: text)
-o, --output <FILE>    Output file path (default: stdout)
-v, --verbose          Show detailed progress for each repository during scanning (includes timestamps)
-u, --username <USERNAME>   GitHub username (required)
-t, --token <TOKEN>         GitHub personal access token (optional, for private repos and higher rate limits)
-r, --repo <REPO>           Scan only the specified repository
-R, --repos <REPO1,REPO2,...>  Scan only the specified repositories (comma-separated list)
```

**Progress Indicator:**
When running without `--verbose`, the tool shows a real-time progress indicator:
```
[14:30:22] Progress: 5/20 repositories (25%)
```
When using `--verbose`, detailed output with timestamps is shown for each repository:
```
[14:30:22] Scanning: repository-name
[14:30:22] [INFO] Cloning repository (depth=100)...
[14:30:23] [INFO] Scanning history...
[14:30:25] [WARN] Found 3 potential environment/configuration files:
```

**Note:** If you are not in the build directory, adjust the path to the executable accordingly.

## Example Output

### Text Format (Normal - without --verbose)

``` text
[WARN] WARNING: This tool scans public repository history for educational purposes.
         Only scan repositories you own or have permission to scan.
         This tool performs read-only operations and does not modify any repositories.

Do you want to continue? (y/N): y
[INFO] Scanning public repositories for user: Gerald-Mathew
[INFO] History depth: 100 commits
[INFO] Maximum repos to scan: 5

[INFO] Found 7 public repositories to scan.

[INFO] Scan complete!
Repository: github-secrets-watcher
  Status: Success
  No potential environment files found.

Repository: FlightReservation-CLI
  Status: Success
  No potential environment files found.

Repository: CareConnect-Clinic-Appointment-System
  Status: Success
  Found 5 potential environment/configuration files:
    - .tmp-browser-check/playwright.config.js
      [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/5d580e41d2fd7b6c2b45e29b3250927dc0f3a4a0/.tmp-browser-check%2fplaywright.config.js
    - client(First)/assets/js/config.js
      [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/e537568aa7264bd1d27c6c5ba311e6440580873f/client%28First%29%2fassets%2fjs%2fconfig.js
    - client/assets/js/config.js
      [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/c7e0fba59fd2141970193909f4e58e2888fca267/client%2fassets%2fjs%2fconfig.js
    - client/eslint.config.js
      [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/c7e0fba59fd2141970193909f4e58e2888fca267/client%2feslint.config.js
    - client/vite.config.js
      [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/c7e0fba59fd2141970193909f4e58e2888fca267/client%2fvite.config.js
```

### Text Format (Verbose - with --verbose)

``` text
[WARN] WARNING: This tool scans public repository history for educational purposes.
         Only scan repositories you own or have permission to scan.
         This tool performs read-only operations and does not modify any repositories.

Do you want to continue? (y/N): y
[INFO] Scanning public repositories for user: Gerald-Mathew
[INFO] History depth: 100 commits
[INFO] Maximum repos to scan: 5

[INFO] Found 7 public repositories to scan.

[1/7] Scanning: github-secrets-watcher
[INFO] Cloning repository (depth=100)...
[INFO] Scanning history...
[OK] No potential environment files found in history.

[2/7] Scanning: FlightReservation-CLI
[INFO] Cloning repository (depth=100)...
[INFO] Scanning history...
[OK] No potential environment files found in history.

[3/7] Scanning: CareConnect-Clinic-Appointment-System
[INFO] Cloning repository (depth=100)...
[INFO] Scanning history...
[WARN] Found 5 potential environment/configuration files:
     - .tmp-browser-check/playwright.config.js
       [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/5d580e41d2fd7b6c2b45e29b3250927dc0f3a4a0/.tmp-browser-check%2fplaywright.config.js
     - client(First)/assets/js/config.js
       [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/e537568aa7264bd1d27c6c5ba311e6440580873f/client%28First%29%2fassets%2fjs%2fconfig.js
     - client/assets/js/config.js
       [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/c7e0fba59fd2141970193909f4e58e2888fca267/client%2fassets%2fjs%2fconfig.js
     - client/eslint.config.js
       [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/c7e0fba59fd2141970193909f4e58e2888fca267/client%2feslint.config.js
     - client/vite.config.js
       [LINK] https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System/blob/c7e0fba59fd2141970193909f4e58e2888fca267/client%2fvite.config.js

[INFO] Scan complete!
```

*Note: In a terminal that supports ANSI colors, the [WARN], [INFO], [OK], [FAIL], and [LINK] prefixes will appear in yellow, blue, green, red, and cyan respectively.*

### JSON Format Example

```json
[
  {
    "repo_name": "CareConnect-Clinic-Appointment-System",
    "html_url": "https://github.com/Gerald-Mathew/CareConnect-Clinic-Appointment-System",
    "success": true,
    "files": [
      {
        "path": ".tmp-browser-check/playwright.config.js",
        "commit_hash": "5d580e41d2fd7b6c2b45e29b3250927dc0f3a4a0"
      },
      {
        "path": "client(First)/assets/js/config.js",
        "commit_hash": "e537568aa7264bd1d27c6c5ba311e6440580873f"
      }
    ]
  }
]
```

### CSV Format Example

```csv
repo_name,success,error_message,file_path,commit_hash
CareConnect-Clinic-Appointment-System,true,,"tmp-browser-check/playwright.config.js",5d580e41d2fd7b6c2b45e29b3250927dc0f3a4a0
CareConnect-Clinic-Appointment-System,true,,"client(First)/assets/js/config.js",e537568aa7264bd1d27c6c5ba311e6440580873f
```

## Safety Notes

- Only scan repositories you own or have explicit permission to scan
- Tool performs read-only operations only (no modifications)
- Intended for defensive security awareness and learning
- Review what the tool does before running it on any repositories

## License

MIT
