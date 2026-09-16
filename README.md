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
- `--dry-run` to preview which repositories would be scanned (no cloning)
- `--yes` to skip the confirmation prompt (handy for scripts/CI)
- Warns when the GitHub API rate limit is running low
- Paginates the GitHub API using the recommended `Link` header (no manual page counting)
- Ships a unit test suite (Catch2) covering the scanner and helpers
- Fast history scan: walks the diff of each commit against its first parent instead of re-walking every tree, so the cost grows with the changed paths, not `depth × repository size`
- Persistent clone cache: full history is cloned once and then only new commits are fetched on later runs (default `~/.cache/github-secrets-watcher`), so repeat scans of the same repos are much cheaper
- Optional early-exit heuristic that stops a full-history walk after a run of commits with no new findings (opt-in)
- Skips likely-non-secret files by extension and name: test/spec scaffolding, build artifacts (lockfiles, minified bundles, type declarations, source maps), and media/fonts/docs
- Content-aware confidence: files whose contents look like real credentials (non-placeholder assignments to `key`/`token`/`secret`/`password`, AWS/GitHub/Stripe tokens, JWTs, private keys) are ranked above files flagged on name alone
- Reports GitHub API errors with the actual cause (rate limit vs. bad/insufficient token) instead of a generic HTTP code
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
# from the repository root
cmake -B build -DBUILD_TESTING=ON
cmake --build build --parallel

# run the unit tests
ctest --test-dir build --output-on-failure

# The executable will be build/github_secrets_watcher
```

> `libgit2` has no CMake config, so CMake locates it via `pkg-config`
> (same as libcurl). `BUILD_TESTING` is ON by default.

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
g++ -std=c++20 -Wall -Wextra -pthread -Isrc \
    src/main.cpp src/github.cpp src/scanner.cpp src/utils.cpp \
    $(pkg-config --cflags --libs libcurl libgit2) \
    -o github_secrets_watcher
```

> Both libs are needed for linking: `-lcurl` is pulled in by `pkg-config libcurl`
> and `libgit2` (with `-pthread`) by `pkg-config libgit2`. Forgetting `libgit2`
> produces undefined references to `git_*` symbols.

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

# Preview what would be scanned without actually scanning
./github_secrets_watcher scan -u YOUR_USERNAME --dry-run

# Skip the confirmation prompt (for scripts/CI)
./github_secrets_watcher scan -u YOUR_USERNAME -y

# Scan only a specific repository
./github_secrets_watcher scan -u YOUR_USERNAME --repo REPO_NAME
./github_secrets_watcher scan -u YOUR_USERNAME -r REPO_NAME

# Scan only specific repositories (comma-separated list)
./github_secrets_watcher scan -u YOUR_USERNAME --repos "repo1,repo2,repo3"
./github_secrets_watcher scan -u YOUR_USERNAME -R "repo1,repo2,repo3"

# Optional parameters (long and short forms available)
-d, --depth <NUM>      Commits to scan in history (default: 0 = all history; a positive value scans at most NUM commits)
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
--cache <DIR>               Cache directory for reusing full clones between runs
                            (default: ~/.cache/github-secrets-watcher, or %LOCALAPPDATA%\github-secrets-watcher on Windows)
--no-cache                  Always clone fresh into a temporary directory
--early-exit <N>            Stop a full-history walk after N consecutive commits with no new findings
                            (only applies to -d 0 scans)
--no-early-exit             Disable the early-exit heuristic explicitly
--dry-run                   List the repositories that would be scanned without scanning
-y, --yes                   Skip the confirmation prompt
```

### How scanning works

For each repository the tool builds a bare clone and then walks the commit history
from the newest reachable tip (across all branches) back towards the root:

1. **Tip seeding** — files present in the newest commit are immediately credited to
   that commit, so a file deleted yesterday is attributed correctly.
2. **Diff walk** — every commit is compared to its first parent (an empty tree at the
   root). Added/modified files are credited to the commit that introduced them;
   **deleted** files are credited to the parent commit, so the reported links always
   point at a commit where the file actually existed.
3. **Content probe** — each reported file's bytes at that commit are checked against
   secret-shaped patterns. Files that match (a non-placeholder value assigned to a
   `key`/`token`/`secret`/`password`, or a known credential format such as AWS,
   GitHub PAT, Stripe `sk_live_`, JWT, or a private key) are marked
   `likely_secret`; everything else is a name-only flag waiting for a manual review.
4. **Early exit (opt-in)** — with `--early-exit N` and a full-history scan, walking
   stops after `N` consecutive commits that change no file of interest.
5. **Depth limit** — with `-d N`, only `N` commits are examined, using a shallow
   fetch on the clone so bounded scans stay small.

Files are only reported when their name suggests they could hold secrets (`.env`,
`.env.*`, or names containing `config`, `settings`, or `secrets`) and the path is not
inside an excluded directory. Files unlikely to be secrets are skipped even when the
name matches: test/spec files (`config.test.js`, `settings.spec.ts`), schema files
(`.env.schema`, `appsettings.schema.json` — the value-less env *shape*, not a leak),
build artifacts (`*.lock`, `*.min.js`, `*.d.ts`, `*.map`), media/fonts (`.png`, `.svg`,
`.woff2`, ...), and documentation/templates (`.md`, `.txt`, `.rst`, `.example`,
`.sample`, ...). Note that `.js`/`.json` configuration files (e.g. `vite.config.js`,
`settings.json`) are reported by design — they often contain API keys — so they are
not in the skip-list.

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
[INFO] History depth: all commits
[INFO] Clone cache: %USERPROFILE%/.cache/github-secrets-watcher (reused between runs)
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
[INFO] History depth: all commits
[INFO] Clone cache: %USERPROFILE%/.cache/github-secrets-watcher (reused between runs)
[INFO] Maximum repos to scan: 5

[INFO] Found 7 public repositories to scan.

[1/7] Scanning: github-secrets-watcher
[INFO] Cloning repository into cache (full history)...
[INFO] Scanning history...
[OK] No potential environment files found in history.

[2/7] Scanning: FlightReservation-CLI
[INFO] Cloning repository into cache (full history)...
[INFO] Scanning history...
[OK] No potential environment files found in history.

[3/7] Scanning: CareConnect-Clinic-Appointment-System
[INFO] Cloning repository into cache (full history)...
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
        "commit_hash": "5d580e41d2fd7b6c2b45e29b3250927dc0f3a4a0",
        "likely_secret": false
      },
      {
        "path": "client(First)/assets/js/config.js",
        "commit_hash": "e537568aa7264bd1d27c6c5ba311e6440580873f",
        "likely_secret": true
      }
    ],
    "scan": {
      "commits_considered": 29,
      "commits_skipped": 0,
      "stopped_early": false,
      "truncated_by_depth": false
    }
  }
]
```

`scan` reports per-repository statistics: how many commits were examined
(`commits_considered`, minus any `commits_skipped` that changed nothing of
interest), whether the early-exit heuristic stopped the walk (`stopped_early`),
and whether history was cut short by `--depth` (`truncated_by_depth`, also set
for bounded bare shallow clones).

### CSV Format Example

```csv
repo_name,success,error_message,file_path,commit_hash,likely_secret
CareConnect-Clinic-Appointment-System,true,,tmp-browser-check/playwright.config.js,5d580e41d2fd7b6c2b45e29b3250927dc0f3a4a0,false
CareConnect-Clinic-Appointment-System,true,,client(First)/assets/js/config.js,e537568aa7264bd1d27c6c5ba311e6440580873f,true
```

## Performance & Trade-offs

Benchmarked on this machine (Ubuntu, cmake 4.2.3, g++ 15.2.0, libgit2 1.9.1),
scanning 3 small repositories (CareConnect-Clinic-Appointment-System, StudySync,
FlightReservation-CLI) with 4 threads:

| Scenario | Wall time | Peak RSS |
| --- | --- | --- |
| Old build, cold, `-d 100` (fresh shallow clone per repo) | 1.27 s | ~42.5 MB |
| New build, cold, full history (`--no-cache`, fresh bare clone per repo) | 1.50 s | ~34.8 MB |
| New build, warm, full history (incremental fetch via cache) | 0.39 s | ~35.4 MB |

Repeat scans are about **3× faster** and the savings grow with repository size:
a cached run only downloads the commits pushed since the last scan instead of the
whole repository. The cold scan now does strictly more work than the old one — it
examines *all* history instead of only 100 commits — yet stays in the same
ballpark and uses less memory, because the new walker diffs each commit against
its first parent (cost = changed paths) instead of re-walking every tree (cost =
tree size) for each commit.

Deliberate trade-offs, in the order they affect you:

- **`--early-exit` is opt-in (off by default).** A security scan should be
  exhaustive, so the tool never sacrifices coverage unless you say so. `--early-exit N`
  stops a full-history walk after `N` consecutive commits that change nothing of
  interest; a file deleted long ago *after* an otherwise-quiet stretch could be
  missed, but for busy repos this skips the ancient, uninteresting tail. It only
  applies to `-d 0` scans.
- **`-d N` (bounded scans) always clone fresh.** libgit2 cannot reliably deepen a
  shallow clone afterwards, so a bounded scan uses a fresh shallow bare clone in a
  temporary directory; the cache is only used for `-d 0`. There is no risk of stale
  results for bounded scans, just less reuse.
- **The clone cache costs disk space.** Each full-history repo is stored once as a
  bare clone under the cache directory (`~/.cache/github-secrets-watcher` by
  default). Point `--cache` elsewhere or delete the directory to start over; the
  cache is only ever read/written on your machine and never uploaded.
- **Files are skipped by "cannot be a secret" heuristics.** Test/spec files
  (`config.test.js`, `settings.spec.ts`), build artifacts (`*.lock`, `*.min.js`,
  `*.d.ts`, `*.map`) and media/fonts/docs are ignored even when their names match.
  This keeps noise low; a secret stored in, say, `config.png` would not be
  reported. Configuration files themselves (`.env`, `config.js`, `settings.json`,
  `*.yaml`, ...) are still reported by design.
- **Merge commits are walked along the first parent only** (like `git log`). Rarely
  matters: a secret merged through a side branch is almost always also reachable
  through the first-parent line the tool follows.
- **Deleted files are credited to the parent commit**, so the reported link always
  points at a commit in which the file actually existed (the old walker pointed at
  the deleting commit, producing links to files that were already gone).

## Safety Notes

- Only scan repositories you own or have explicit permission to scan
- Tool performs read-only operations only (no modifications)
- Intended for defensive security awareness and learning
- Review what the tool does before running it on any repositories

## Troubleshooting

**`undefined reference to git_*` at link time** — you are missing `-lgit2` (and usually `-pthread`). Use the `pkg-config libgit2` flags as shown in the manual compile command.

**`Fail: GitHub user not found` but the user exists** — GitHub renamed the user, or the username is case-sensitive/typo'd. Check the exact name on the profile URL.

**`GitHub API returned HTTP 403 - rate limit exceeded`** — you ran out of API rate limit. The error includes a reset time when GitHub provides one. Supply a `--token` to raise your limit.

**`GitHub API returned HTTP 403 - Bad credentials`** (or `Resource not accessible by personal access token`) — your token is missing, expired, or lacks the `repo` scope. Regenerate it with the full `repo` scope to scan private repositories. A missing `--token` also prevents listing private repos even when `--include-private` is set; the tool prints a warning in that case.

**`Git clone failed: authentication failed` when using `--include-private`** — the token is missing, expired, or lacks `repo` scope. Regenerate it with repo access. Your token is never embedded in the clone URL, so it cannot leak through error output.

**`CMake Error: Could not find a package configuration file for "Catch2"`** — install Catch2 (`catch2` on Debian/Ubuntu, `catch2` on Homebrew) or configure with `-DBUILD_TESTING=OFF` to skip the tests.

## Project Structure

```
.
├── CMakeLists.txt          # build definition (pkg-config for libcurl/libgit2)
├── build/
│   ├── build.sh            # Unix/macOS/MSYS2 build script
│   └── build.bat           # Windows (MSVC) build script
├── src/
│   ├── main.cpp            # CLI entry point, output formats
│   ├── github.cpp/hpp      # GitHub REST API client (Link-header pagination)
│   ├── scanner.cpp/hpp     # git history scanner (libgit2)
│   ├── thread_pool.hpp     # parallel worker pool
│   ├── utils.cpp/hpp       # URL/CSV helpers, link parsing
│   └── json.hpp            # vendored nlohmann/json (single header)
└── tests/
    ├── utils.test.cpp      # helpers + CSV/Link-header parsing
    └── scanner.test.cpp    # integration-style tests against a real git repo
```

## License

MIT
