#include <iostream>
#include <string>
#include <vector>
#include <optional>
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <fstream>
#include <sstream>
#include <atomic>
#include <algorithm>
#include <map>

#include "github.hpp"
#include "scanner.hpp"
#include "utils.hpp"
#include "thread_pool.hpp"
#include "json.hpp"
#include <git2.h>

// Helper function to get current timestamp for logging (millisecond precision,
// so the verbose log can be used to see where a scan spends its time).
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                  now.time_since_epoch()) % 1000;
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time_t); // Windows
#else
    localtime_r(&time_t, &tm); // Linux/macOS
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S") << '.'
        << std::setfill('0') << std::setw(3) << ms.count();
    return oss.str();
}

// Monotonic clock used for the per-phase durations. Unlike the wall-clock
// timestamp above it never jumps, so it is safe to subtract.
using SteadyClock = std::chrono::steady_clock;

long long elapsed_us(SteadyClock::time_point start) {
    return std::chrono::duration_cast<std::chrono::microseconds>(
               SteadyClock::now() - start).count();
}

// Render a microsecond duration as a short human-readable string.
std::string format_duration(long long us) {
    std::ostringstream oss;
    if (us < 1000) {
        oss << us << " us";
    } else if (us < 1000000) {
        oss << std::fixed << std::setprecision(1) << (us / 1000.0) << " ms";
    } else {
        oss << std::fixed << std::setprecision(2) << (us / 1000000.0) << " s";
    }
    return oss.str();
}

// Output format options
enum class OutputFormat {
    Text,
    JSON,
    CSV
};

// Structure to hold a found file
struct FoundFile {
    std::string path;
    std::string commit_hash;
    bool likely_secret = false;
};

// Structure to hold the result of scanning a repository
struct RepoResult {
    std::string repo_name;
    std::string html_url;
    bool success;
    std::string error_message;
    std::vector<FoundFile> files;
    size_t index; // For progress tracking in output
    // Scanner statistics (empty/false when the repo failed to prepare)
    size_t commits_considered = 0;
    size_t commits_skipped = 0;
    bool stopped_early = false;
    bool truncated_by_depth = false;
    // Wall-time spent on the two phases of this repository. Precisely the
    // phases users care about: the network-bound "get a local copy" phase and
    // the CPU-bound "walk the history" phase.
    long long prepare_us = 0;
    long long scan_us = 0;
};

// Files copied out of ScanResult to build the per-repo report
struct RepoFindings {
    std::vector<FoundFile> files;
    size_t commits_considered = 0;
    size_t commits_skipped = 0;
    bool stopped_early = false;
    bool truncated_by_depth = false;
    long long prepare_us = 0;
    long long scan_us = 0;
};

// Global variables for output configuration
OutputFormat g_output_format = OutputFormat::Text;
std::ostream* g_output_stream = &std::cout;
std::mutex g_output_mutex;
std::vector<RepoResult> g_results;
std::mutex g_results_mutex;

// Mutex for console output (status messages, errors, etc.)
std::mutex g_console_mutex;

// Progress tracking
std::atomic<size_t> g_completed_count{0};
std::mutex g_progress_mutex; // For protecting progress output to avoid interleaving
size_t g_total_repos = 0;

namespace fs = std::filesystem;

// Color definitions
const char* const COLOR_RESET   = "\033[0m";
const char* const COLOR_RED     = "\033[31m";
const char* const COLOR_GREEN   = "\033[32m";
const char* const COLOR_YELLOW  = "\033[33m";
const char* const COLOR_BLUE    = "\033[34m";
const char* const COLOR_MAGENTA = "\033[35m";
const char* const COLOR_CYAN    = "\033[36m";
const char* const COLOR_WHITE   = "\033[37m";
const char* const COLOR_BOLD    = "\033[1m";

const char* const COLOR_WARN   = COLOR_YELLOW;
const char* const COLOR_INFO   = COLOR_BLUE;
const char* const COLOR_OK     = COLOR_GREEN;
const char* const COLOR_FAIL   = COLOR_RED;
const char* const COLOR_LINK   = COLOR_CYAN;

// RAII wrapper for libgit2 initialization
struct Git2Library {
    Git2Library() {
        int error = git_libgit2_init();
        if (error < 0) {
            const git_error* e = giterr_last();
            throw std::runtime_error("Failed to initialize libgit2: " + std::string(e && e->message ? e->message : "unknown error"));
        }
        initialized_ = true;
    }
    ~Git2Library() {
        if (initialized_) {
            git_libgit2_shutdown();
        }
    }
    bool initialized_ = false;
};

// RAII wrapper so temporary clone dirs are always cleaned up, even on exceptions
class TempDir {
public:
    TempDir(const std::string& repo_name) {
        auto now = std::chrono::system_clock::now();
        auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        fs::path base = std::getenv("TEMP") ? fs::path(std::getenv("TEMP")) : fs::temp_directory_path();
        path_ = base / ("github_secrets_watcher_" + repo_name + "_" + std::to_string(millis));
        try {
            fs::create_directories(path_);
        } catch (const fs::filesystem_error& e) {
            throw std::runtime_error("Failed to create temporary directory: " + path_.string() + " - " + e.what());
        }
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path_, ec); // best-effort cleanup
    }
    const fs::path& path() const { return path_; }

private:
    fs::path path_;
};

// Credential material handed to libgit2's auth callback. Stores the token in
// the struct so it never ends up inside the clone/fetch URL.
struct AuthToken {
    std::string token;
    std::string password{"x-oauth-basic"};
};

int credential_cb(git_credential** out, const char* url, const char* username_from_url,
                  unsigned int allowed_types, void* payload) {
    (void)url;
    (void)username_from_url;
    (void)allowed_types;
    auto* auth = static_cast<AuthToken*>(payload);
    return git_credential_userpass_plaintext_new(out, auth->token.c_str(), auth->password.c_str());
}

// Wire the credential callback into a fetch options struct when a token is set.
// Each network call gets a fresh credential (libgit2 frees the one we return),
// which avoids double-frees when the server asks more than once.
void set_fetch_credentials(git_fetch_options& opts, const std::optional<std::string>& token, AuthToken& auth) {
    if (!token.has_value()) return;
    auth = AuthToken{token.value()};
    opts.callbacks.credentials = credential_cb;
    opts.callbacks.payload = &auth;
}

// Clone into `target` (bare). Retries once after clearing the directory when a
// partially-created clone from a previous run is in the way.
int clone_into(const std::string& clone_url, const std::string& target,
               const std::optional<std::string>& token, AuthToken& auth, int depth) {
    git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
    clone_opts.bare = 1; // no worktree checkout; we only read history
    if (depth > 0) {
        clone_opts.fetch_opts.depth = depth;
    }
    set_fetch_credentials(clone_opts.fetch_opts, token, auth);

    git_repository* repo = nullptr;
    int error = git_clone(&repo, clone_url.c_str(), target.c_str(), &clone_opts);
    if (repo) git_repository_free(repo);

    if (error < 0) {
        // Clear a stale partial clone and try once more (the cache may hold a
        // half-written repository from an interrupted earlier run).
        std::error_code ec;
        fs::remove_all(target, ec);
        clone_opts = GIT_CLONE_OPTIONS_INIT;
        clone_opts.bare = 1;
        if (depth > 0) clone_opts.fetch_opts.depth = depth;
        set_fetch_credentials(clone_opts.fetch_opts, token, auth);
        error = git_clone(&repo, clone_url.c_str(), target.c_str(), &clone_opts);
        if (repo) git_repository_free(repo);
    }
    return error;
}

// Fetch the latest history into an existing cached repository.
int fetch_cached(const std::string& cache_path, const std::optional<std::string>& token, AuthToken& auth) {
    git_repository* repo = nullptr;
    if (git_repository_open(&repo, cache_path.c_str()) < 0) {
        return -1;
    }
    git_remote* remote = nullptr;
    int error = git_remote_lookup(&remote, repo, "origin");
    if (error == 0) {
        git_fetch_options fopts = GIT_FETCH_OPTIONS_INIT;
        fopts.prune = GIT_FETCH_PRUNE;
        fopts.download_tags = GIT_REMOTE_DOWNLOAD_TAGS_AUTO;
        set_fetch_credentials(fopts, token, auth);
        error = git_remote_fetch(remote, nullptr, &fopts, nullptr);
        git_remote_free(remote);
    }
    git_repository_free(repo);
    return error;
}

// Default location for the clone cache.
fs::path default_cache_dir() {
    if (const char* local_app = std::getenv("LOCALAPPDATA")) {
        return fs::path(local_app) / "github-secrets-watcher";
    }
    const char* home = std::getenv("HOME");
    if (!home) home = std::getenv("USERPROFILE");
    if (home && *home) {
        return fs::path(home) / ".cache" / "github-secrets-watcher";
    }
    return fs::temp_directory_path() / "github-secrets-watcher-cache";
}

// Print usage information
void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " scan -u <USERNAME> [-t <TOKEN>] [-d <DEPTH>] [-m <NUM>] [-p] [-n <NUM>] [-f <FMT>] [-o <FILE>] [-v] [-r <REPO>] [-R <REPO1,REPO2,...>] [--cache <DIR>] [--no-cache] [--early-exit <N>] [--dry-run] [-y]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -u, --username <USERNAME>   GitHub username (required)\n";
    std::cout << "  -t, --token <TOKEN>         GitHub personal access token (optional, for private repos and higher rate limits)\n";
    std::cout << "  -d, --depth <NUM>           Commits to scan in history (default: 0 = all history; a positive value scans at most NUM commits)\n";
    std::cout << "  -m, --max-repos <NUM>       Maximum repositories to scan (default: all)\n";
    std::cout << "  -p, --include-private       Include private repositories (requires token)\n";
    std::cout << "  -n, --threads <NUM>         Number of threads to use for scanning (default: hardware concurrency)\n";
    std::cout << "  -f, --format <FMT>          Output format: text, json, or csv (default: text)\n";
    std::cout << "  -o, --output <FILE>         Output file path (default: stdout)\n";
    std::cout << "  -v, --verbose               Enable verbose output\n";
    std::cout << "  -r, --repo <REPO>           Scan only the specified repository\n";
    std::cout << "  -R, --repos <REPO1,REPO2,...>\n";
    std::cout << "                              Scan only the specified repositories (comma-separated list)\n";
    std::cout << "      --cache <DIR>           Cache directory for reusing full clones between runs\n";
    std::cout << "                              (default: ~/.cache/github-secrets-watcher)\n";
    std::cout << "      --no-cache              Always clone fresh into a temporary directory\n";
    std::cout << "      --early-exit <N>        Stop a full-history walk after N consecutive commits with no new\n";
    std::cout << "                              findings (default: off). Only applies to -d 0 scans.\n";
    std::cout << "      --no-early-exit         Disable the early-exit heuristic explicitly\n";
    std::cout << "      --dry-run               List the repositories that would be scanned without scanning\n";
    std::cout << "  -y, --yes                   Skip the confirmation prompt\n";
}

// Prepare a local copy of a repository (cached bare clone, or a fresh bare
// clone in a temp dir) and scan its history for env/config files.
void process_repository(const Repository& repo, int depth, size_t early_exit,
                        const std::string& username, const std::optional<std::string>& token,
                        bool use_cache, const fs::path& cache_root, size_t index, bool verbose) {
    // Output status message to console (with mutex)
    if (verbose) {
        std::lock_guard<std::mutex> lock(g_console_mutex);
        std::cerr << "[" << get_timestamp() << "] Scanning: " << repo.name << std::endl;
    } else {
        // Show progress indicator for non-verbose mode
        {
            std::lock_guard<std::mutex> lock(g_progress_mutex);
            size_t completed = ++g_completed_count;
            std::cerr << "\r[" << get_timestamp() << "] Progress: " << completed << "/" << g_total_repos
                      << " repositories (" << static_cast<int>((completed * 100.0) / g_total_repos) << "%)   ";
            std::cerr.flush();
        }
    }

    bool success = true;
    std::string error_message;
    RepoFindings findings;

    std::unique_ptr<TempDir> temp; // only used for non-cached clones
    AuthToken auth;
    const bool caching = use_cache && (depth <= 0);

    const SteadyClock::time_point phase_start = SteadyClock::now();

    try {
        // Keep the token out of the clone/fetch URL so errors never leak it:
        // libgit2 calls our credentials callback when the server asks for auth.
        const std::string clone_url = "https://github.com/" + username + "/" + repo.name + ".git";

        std::string scan_path;
        if (caching) {
            // Persistent cache: clone once, then fetch only new commits.
            const fs::path target = cache_root / username / repo.name;
            std::error_code ec;
            fs::create_directories(target.parent_path(), ec);

            bool prepared = false;
            if (fs::exists(target)) {
                if (verbose) {
                    std::lock_guard<std::mutex> lock(g_console_mutex);
                    std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << "Fetching latest changes into cache..." << std::endl;
                }
                int fetch_error = fetch_cached(target.string(), token, auth);
                if (fetch_error < 0) {
                    if (verbose) {
                        std::lock_guard<std::mutex> lock(g_console_mutex);
                        std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "Cache refresh failed, re-cloning..." << std::endl;
                    }
                    std::error_code fec;
                    fs::remove_all(target, fec);
                } else {
                    prepared = true;
                }
            }

            if (!prepared) {
                if (verbose) {
                    std::lock_guard<std::mutex> lock(g_console_mutex);
                    std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << "Cloning repository into cache (full history)..." << std::endl;
                }
                int clone_error = clone_into(clone_url, target.string(), token, auth, 0);
                if (clone_error < 0) {
                    const git_error* e = giterr_last();
                    std::string msg = e && e->message ? std::string(e->message) : "unknown libgit2 error";
                    if (verbose) {
                        std::lock_guard<std::mutex> lock(g_console_mutex);
                        std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "[" << get_timestamp() << "] Git clone failed: " << msg << std::endl;
                    }
                    throw std::runtime_error("Git clone failed: " + msg);
                }
                prepared = true;
            }
            scan_path = target.string();
        } else {
            // Bounded depth: a fresh shallow bare clone each run.
            temp = std::make_unique<TempDir>(repo.name);
            scan_path = temp->path().string();
            if (verbose) {
                std::lock_guard<std::mutex> lock(g_console_mutex);
                std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET
                          << "Cloning repository (depth=" << depth << ")..." << std::endl;
            }
            int clone_error = clone_into(clone_url, scan_path, token, auth, depth);
            if (clone_error < 0) {
                const git_error* e = giterr_last();
                std::string msg = e && e->message ? std::string(e->message) : "unknown libgit2 error";
                if (verbose) {
                    std::lock_guard<std::mutex> lock(g_console_mutex);
                    std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "[" << get_timestamp() << "] Git clone failed: " << msg << std::endl;
                }
                throw std::runtime_error("Git clone failed: " + msg);
            }
        }

        // Scan history for env-like files
        findings.prepare_us = elapsed_us(phase_start);
        if (verbose) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << "[" << get_timestamp()
                      << "] Ready to scan (" << (caching ? "cache" : "fresh clone")
                      << ") in " << format_duration(findings.prepare_us) << std::endl;
        }
        const SteadyClock::time_point scan_start = SteadyClock::now();
        if (verbose) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << "[" << get_timestamp() << "] Scanning history..." << std::endl;
        }
        scanner::ScanResult scan = scanner::scan_repo_history(scan_path, depth, early_exit);
        findings.scan_us = elapsed_us(scan_start);

        findings.commits_considered = scan.commits_considered;
        findings.commits_skipped = scan.commits_skipped;
        findings.stopped_early = scan.stopped_early;
        // A bounded scan uses a shallow fetch, so older commits beyond --depth
        // were never downloaded even if they exist.
        findings.truncated_by_depth = scan.truncated_by_depth || (depth > 0);

        if (!scan.file_to_commit.empty()) {
            size_t likely_count = scan.likely_secret.size();
            if (verbose) {
                std::lock_guard<std::mutex> lock(g_console_mutex);
                std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "[" << get_timestamp() << "] Found " << scan.file_to_commit.size()
                          << " potential environment/configuration files"
                          << (likely_count > 0 ? " (" + std::to_string(likely_count) + " likely secret" + (likely_count != 1 ? "s" : "") + ")" : "")
                          << " (walked " << scan.commits_considered
                          << " commits, skipped " << scan.commits_skipped << " unchanged):" << std::endl;
            }
            for (const auto& [file_path, commit_hash] : scan.file_to_commit) {
                // Validate commit hash format (40 hex characters)
                if (!utils::is_valid_commit_hash(commit_hash)) {
                    if (verbose) {
                        std::lock_guard<std::mutex> lock(g_console_mutex);
                        std::cerr << "     - " << file_path << std::endl;
                        std::cerr << "       " << COLOR_WARN << "[WARN] " << COLOR_RESET << "[" << get_timestamp() << "] Invalid commit hash: " << commit_hash << std::endl;
                    }
                    continue;
                }

                // Construct a link to the file at the specific commit (URL-encode the file path)
                std::string repo_url = utils::remove_trailing_slash(repo.html_url);
                std::string encoded_file_path = utils::url_encode(file_path);
                std::string file_url = repo_url + "/blob/" + commit_hash + "/" + encoded_file_path;

                if (verbose) {
                    std::lock_guard<std::mutex> lock(g_console_mutex);
                    std::cerr << "     - " << file_path;
                    if (scan.likely_secret.count(file_path)) {
                        std::cerr << " (likely secret)";
                    }
                    std::cerr << std::endl;
                    std::cerr << "       " << COLOR_LINK << "[LINK] " << COLOR_RESET << file_url << std::endl;
                }

                // Add to our local files vector
                findings.files.push_back({file_path, commit_hash,
                                          scan.likely_secret.count(file_path) > 0});
            }
        } else {
            if (verbose) {
                std::lock_guard<std::mutex> lock(g_console_mutex);
                std::cerr << COLOR_OK << "[OK] " << COLOR_RESET << "No potential environment files found in history." << std::endl;
            }
        }

        if (verbose && scan.stopped_early) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET
                      << "History walk stopped early (" << scan.commits_considered
                      << " commits examined; --early-exit heuristic)." << std::endl;
        }
        if (verbose && scan.truncated_by_depth) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET
                      << "History walk truncated by --depth (" << scan.commits_considered
                      << " commits examined)." << std::endl;
        }

    } catch (const std::exception& e) {
        success = false;
        error_message = e.what();
        if (verbose) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Error processing " << repo.name << ": " << e.what() << std::endl;
        }
    } catch (...) {
        success = false;
        error_message = "Unknown error";
        if (verbose) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Unknown error processing " << repo.name << std::endl;
        }
    }

    // Add the result to the global results
    {
        std::lock_guard<std::mutex> lock(g_results_mutex);
        g_results.push_back({repo.name, repo.html_url, success, error_message, findings.files, index,
                             findings.commits_considered, findings.commits_skipped,
                             findings.stopped_early, findings.truncated_by_depth,
                             findings.prepare_us, findings.scan_us});
    }
}

// Helper function to output the results in the chosen format
void output_results(bool verbose) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    switch (g_output_format) {
        case OutputFormat::JSON: {
            // nlohmann::json handles all string escaping correctly
            nlohmann::json out = nlohmann::json::array();
            for (const auto& r : g_results) {
                nlohmann::json entry;
                entry["repo_name"] = r.repo_name;
                entry["html_url"] = r.html_url;
                entry["success"] = r.success;
                if (!r.error_message.empty()) {
                    entry["error_message"] = r.error_message;
                }
                entry["scan"] = {
                    {"commits_considered", r.commits_considered},
                    {"commits_skipped", r.commits_skipped},
                    {"stopped_early", r.stopped_early},
                    {"truncated_by_depth", r.truncated_by_depth},
                };
                nlohmann::json files = nlohmann::json::array();
                for (const auto& f : r.files) {
                    files.push_back({{"path", f.path},
                                     {"commit_hash", f.commit_hash},
                                     {"likely_secret", f.likely_secret}});
                }
                entry["files"] = files;
                out.push_back(entry);
            }
            *g_output_stream << out.dump(2) << "\n";
            break;
        }
        case OutputFormat::CSV: {
            // RFC 4180: fields containing , " or newlines are quoted and
            // embedded quotes are doubled
            auto field = [](const std::string& s) { return utils::csv_escape(s); };
            *g_output_stream << "repo_name,success,error_message,file_path,commit_hash,likely_secret\n";
            for (const auto& r : g_results) {
                if (r.files.empty()) {
                    *g_output_stream << field(r.repo_name) << "," << (r.success ? "true" : "false") << ","
                                     << field(r.error_message) << ",,,\n";
                } else {
                    for (const auto& f : r.files) {
                        *g_output_stream << field(r.repo_name) << "," << (r.success ? "true" : "false") << ","
                                         << field(r.error_message) << "," << field(f.path) << ","
                                         << field(f.commit_hash) << "," << (f.likely_secret ? "true" : "false") << "\n";
                    }
                }
            }
            break;
        }
        case OutputFormat::Text: {
            for (const auto& r : g_results) {
                if (verbose) {
                    *g_output_stream << "[" << (r.index + 1) << "/" << g_results.size() << "] Repository: " << r.repo_name << "\n";
                } else {
                    *g_output_stream << "Repository: " << r.repo_name << "\n";
                }
                if (r.success) {
                    *g_output_stream << "  Status: Success\n";
                } else {
                    *g_output_stream << "  Status: Failed\n";
                    if (!r.error_message.empty()) {
                        *g_output_stream << "  Error: " << r.error_message << "\n";
                    }
                }
                if (r.files.empty()) {
                    *g_output_stream << "  No potential environment files found.\n";
                } else {
                    size_t likely = 0;
                    for (const auto& f : r.files) likely += f.likely_secret ? 1 : 0;
                    *g_output_stream << "  Found " << r.files.size() << " potential environment/configuration files"
                                     << (likely > 0 ? " (" + std::to_string(likely) + " likely secret" + (likely != 1 ? "s" : "") + ")" : "")
                                     << ":\n";
                    for (const auto& f : r.files) {
                        *g_output_stream << "    - " << f.path
                                         << (f.likely_secret ? " (likely secret)" : "") << "\n";
                        std::string repo_url = utils::remove_trailing_slash(r.html_url);
                        std::string encoded_file_path = utils::url_encode(f.path);
                        std::string file_url = repo_url + "/blob/" + f.commit_hash + "/" + encoded_file_path;
                        *g_output_stream << "      [LINK] " << file_url << "\n";
                    }
                }
                if (verbose) {
                    *g_output_stream << "  Timing: clone/fetch " << format_duration(r.prepare_us)
                                     << ", history scan " << format_duration(r.scan_us) << "\n";
                }
                *g_output_stream << "\n";
            }
            break;
        }
    }
}

#ifdef _WIN32
#include <windows.h>
void enable_vt_processing() {
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut == INVALID_HANDLE_VALUE) return;
    DWORD dwMode = 0;
    if (!GetConsoleMode(hOut, &dwMode)) return;
    dwMode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(hOut, dwMode);
}
#endif

int main(int argc, char* argv[]) {
    #ifdef _WIN32
    enable_vt_processing();
    #endif

    // Initialize libgit2
    Git2Library git2lib;

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string command = argv[1];
    if (command != "scan") {
        std::cerr << "Unknown command: " << command << "\n";
        print_usage(argv[0]);
        return 1;
    }

    // Parse arguments
    std::string username;
    std::optional<std::string> token = std::nullopt;
    int depth = 0; // 0 = entire reachable history
    std::optional<int> max_repos = std::nullopt;
    bool include_private = false;
    bool verbose = false;
    bool dry_run = false;
    bool assume_yes = false;
    int thread_count = static_cast<int>(std::thread::hardware_concurrency());
    if (thread_count <= 0) thread_count = 4; // fallback
    OutputFormat format = OutputFormat::Text;
    std::string output_file;
    size_t early_exit = 0; // 0 = disabled; >0 = stop after N quiet commits (-d 0 only)
    bool use_cache = true;
    fs::path cache_dir = default_cache_dir();
    // Repo selection options
    std::optional<std::string> selected_repo;
    std::optional<std::vector<std::string>> selected_repos_list;

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--username" || arg == "-u") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --username requires a value\n";
                return 1;
            }
            username = argv[++i];
        } else if (arg == "--token" || arg == "-t") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --token requires a value\n";
                return 1;
            }
            token = argv[++i];
        } else if (arg == "--depth" || arg == "-d") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --depth requires a value\n";
                return 1;
            }
            try {
                depth = std::stoi(argv[++i]);
                if (depth < 0) {
                    std::cerr << "Error: --depth must not be negative (0 = all history)\n";
                    return 1;
                }
            } catch (const std::exception&) {
                std::cerr << "Error: --depth must be a number\n";
                return 1;
            }
        } else if (arg == "--cache") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --cache requires a value\n";
                return 1;
            }
            cache_dir = fs::path(argv[++i]);
        } else if (arg == "--no-cache") {
            use_cache = false;
        } else if (arg == "--early-exit") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --early-exit requires a value\n";
                return 1;
            }
            try {
                early_exit = static_cast<size_t>(std::stoul(argv[++i]));
                if (early_exit == 0) {
                    std::cerr << "Error: --early-exit must be at least 1 (use --no-early-exit to disable)\n";
                    return 1;
                }
            } catch (const std::exception&) {
                std::cerr << "Error: --early-exit must be a number\n";
                return 1;
            }
        } else if (arg == "--no-early-exit") {
            early_exit = 0;
        } else if (arg == "--max-repos" || arg == "-m") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --max-repos requires a value\n";
                return 1;
            }
            try {
                int val = std::stoi(argv[++i]);
                if (val <= 0) {
                    std::cerr << "Error: --max-repos must be positive\n";
                    return 1;
                }
                max_repos = val;
            } catch (const std::exception&) {
                std::cerr << "Error: --max-repos must be a number\n";
                return 1;
            }
        } else if (arg == "--include-private" || arg == "-p") {
            include_private = true;
        } else if (arg == "--threads" || arg == "-n") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --threads requires a value\n";
                return 1;
            }
            try {
                int val = std::stoi(argv[++i]);
                if (val <= 0) {
                    std::cerr << "Error: --threads must be positive\n";
                    return 1;
                }
                thread_count = val;
            } catch (const std::exception&) {
                std::cerr << "Error: --threads must be a number\n";
                return 1;
            }
        } else if (arg == "--format" || arg == "-f") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --format requires a value\n";
                return 1;
            }
            std::string fmt = argv[++i];
            if (fmt == "json") {
                format = OutputFormat::JSON;
            } else if (fmt == "csv") {
                format = OutputFormat::CSV;
            } else if (fmt == "text") {
                format = OutputFormat::Text;
            } else {
                std::cerr << "Error: --format must be text, json, or csv\n";
                return 1;
            }
        } else if (arg == "--output" || arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --output requires a value\n";
                return 1;
            }
            output_file = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--repo" || arg == "-r") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --repo requires a value\n";
                return 1;
            }
            selected_repo = argv[++i];
        } else if (arg == "--repos" || arg == "-R") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --repos requires a value\n";
                return 1;
            }
            std::string repos_str = argv[++i];
            std::vector<std::string> repos_list;
            std::stringstream ss(repos_str);
            std::string repo;
            while (std::getline(ss, repo, ',')) {
                // Trim whitespace
                repo.erase(0, repo.find_first_not_of(" \t\n\r\f\v"));
                repo.erase(repo.find_last_not_of(" \t\n\r\f\v") + 1);
                if (!repo.empty()) {
                    repos_list.push_back(repo);
                }
            }
            if (repos_list.empty()) {
                std::cerr << "Error: --repos must contain at least one repository name\n";
                return 1;
            }
            selected_repos_list = repos_list;
        } else if (arg == "--dry-run") {
            dry_run = true;
        } else if (arg == "--yes" || arg == "-y") {
            assume_yes = true;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            print_usage(argv[0]);
            return 1;
        }
    }

    if (username.empty()) {
        std::cerr << "Error: --username is required.\n";
        return 1;
    }

    // Warn if trying to access private repos without token
    if (include_private && !token.has_value()) {
        std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "--include-private has no effect without --token; "
                  << "GitHub never lists private repositories for anonymous requests. "
                  << "Supply a personal access token to scan them.\n";
        std::cerr << "         Falling back to public repositories only.\n";
        include_private = false;
    }

    // Warning about responsible use
    std::cout << COLOR_WARN << "[WARN] " << COLOR_RESET << "WARNING: This tool scans public repository history for educational purposes.\n";
    std::cout << "         Only scan repositories you own or have permission to scan.\n";
    std::cout << "         This tool performs read-only operations and does not modify any repositories.\n\n";

    // Confirmation prompt is skipped with --dry-run and --yes
    if (!dry_run && !assume_yes) {
        std::cout << "Do you want to continue? (y/N): ";
        std::string response;
        std::getline(std::cin, response);
        std::transform(response.begin(), response.end(), response.begin(), ::tolower);
        if (response != "y" && response != "yes") {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scan cancelled.\n";
            return 0;
        }
    }

    // Set global output format and stream
    g_output_format = format;
    std::ofstream* ofs = nullptr;
    if (!output_file.empty()) {
        ofs = new std::ofstream(output_file);
        if (!ofs->is_open()) {
            std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Failed to open output file: " << output_file << "\n";
            return 1;
        }
        g_output_stream = ofs;
    }

    try {
        const SteadyClock::time_point t0 = SteadyClock::now();
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scanning " << (include_private ? "public and private" : "public")
                  << " repositories for user: " << username << "\n";
        if (depth <= 0) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "History depth: all commits\n";
        } else {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "History depth: " << depth << " commits\n";
        }
        if (use_cache && depth <= 0) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Clone cache: " << cache_dir.string() << " (reused between runs)\n";
        } else {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Clone cache: disabled (fresh temporary clones)\n";
        }
        if (early_exit > 0) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Early exit: stop after " << early_exit << " commits with no new findings\n";
        }
        if (max_repos.has_value()) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Maximum repos to scan: " << max_repos.value() << "\n";
        }
        std::cout << "\n";

        // Get list of repositories
        github::UserRepos user_repos = github::get_user_repos(username, token, include_private);
        const long long api_us = elapsed_us(t0);
        if (user_repos.repos.empty()) {
            std::cout << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "No repositories found or error occurred.\n";
            return 0;
        }

        // Warn when the API rate limit is about to run out
        if (user_repos.rate_limit_remaining >= 0 && user_repos.rate_limit_remaining <= 10) {
            std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "GitHub API rate limit is low: "
                      << user_repos.rate_limit_remaining << " requests remaining. Use --token to raise it.\n";
        }

        std::vector<Repository> repos = user_repos.repos;

        if (max_repos.has_value()) {
            if (static_cast<int>(repos.size()) > max_repos.value()) {
                repos.resize(max_repos.value());
            }
        }

        std::string repo_type = include_private ? "public and private" : "public";
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Found " << repos.size() << " " << repo_type << " repositories.\n\n";

        // Filter repositories based on --repo or --repos options
        std::vector<Repository> filtered_repos;
        if (selected_repo.has_value()) {
            // Scan only the specified repository
            auto it = std::find_if(repos.begin(), repos.end(), [&](const Repository& r) {
                return r.name == selected_repo.value();
            });
            if (it != repos.end()) {
                filtered_repos.push_back(*it);
                std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scanning specified repository: " << selected_repo.value() << "\n\n";
            } else {
                std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Repository not found: " << selected_repo.value() << "\n";
                return 0;
            }
        } else if (selected_repos_list.has_value()) {
            // Scan only the specified repositories
            for (const auto& repo_name : selected_repos_list.value()) {
                auto it = std::find_if(repos.begin(), repos.end(), [&](const Repository& r) {
                    return r.name == repo_name;
                });
                if (it != repos.end()) {
                    filtered_repos.push_back(*it);
                } else {
                    std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "Repository not found (skipping): " << repo_name << "\n";
                }
            }
            if (filtered_repos.empty()) {
                std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "No valid repositories found from the specified list.\n";
                return 0;
            }
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scanning " << filtered_repos.size() << " specified repositories.\n\n";
        } else {
            // No specific repos selected, use all repos (after max_repos filtering)
            filtered_repos = repos;
        }

        // Apply max_repos limit if specified (in case it wasn't applied earlier)
        if (max_repos.has_value() && static_cast<int>(filtered_repos.size()) > max_repos.value()) {
            filtered_repos.resize(max_repos.value());
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Limited to " << max_repos.value() << " repositories.\n\n";
        }

        // Dry run just lists what would be scanned, no cloning happens
        if (dry_run) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Dry run: " << filtered_repos.size() << " repositories would be scanned:\n";
            for (const auto& repo : filtered_repos) {
                std::cout << "  - " << repo.name << " (" << repo.html_url << ")\n";
            }
            delete ofs;
            return 0;
        }

        // Process each repository using thread pool
        {
            // Reset progress tracking
            g_completed_count = 0;
            g_total_repos = filtered_repos.size();

            ThreadPool pool(thread_count);

            size_t index = 0;
            for (const auto& repo : filtered_repos) {
                pool.enqueue(process_repository, repo, depth, early_exit, username, token, use_cache, cache_dir, index, verbose);
                index++;
            }
            // pool goes out of scope here, waiting for all threads
        }

        // After all threads are done, move to next line for progress indicator
        if (!verbose) {
            std::cerr << std::endl;
        }

        // Verbose timing summary: where did the time actually go?
        if (verbose) {
            long long prepare_total = 0, scan_total = 0;
            long long max_prepare = 0, max_scan = 0;
            const RepoResult* slow_prepare = nullptr;
            const RepoResult* slow_scan = nullptr;
            for (const auto& r : g_results) {
                prepare_total += r.prepare_us;
                scan_total += r.scan_us;
                if (r.prepare_us > max_prepare) { max_prepare = r.prepare_us; slow_prepare = &r; }
                if (r.scan_us > max_scan) { max_scan = r.scan_us; slow_scan = &r; }
            }
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET
                      << "Timing: GitHub API " << format_duration(api_us)
                      << " | clone/fetch total " << format_duration(prepare_total)
                      << " | history scan total " << format_duration(scan_total)
                      << " | wall " << format_duration(elapsed_us(t0)) << "\n";
            if (slow_prepare) {
                std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET
                          << "Timing: slowest clone/fetch " << slow_prepare->repo_name
                          << " (" << format_duration(max_prepare) << "), slowest history scan "
                          << (slow_scan ? slow_scan->repo_name : "?") << " ("
                          << format_duration(max_scan) << ")\n";
            }
        }

        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scan complete!\n";

        // Output results
        output_results(verbose);

        // Clean up if we opened an output file
        if (!output_file.empty()) {
            delete static_cast<std::ofstream*>(g_output_stream);
        }

    } catch (const std::exception& e) {
        std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Fatal error: " << e.what() << "\n";
        if (!output_file.empty()) {
            delete static_cast<std::ofstream*>(g_output_stream);
        }
        return 1;
    } catch (...) {
        std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Unknown fatal error\n";
        if (!output_file.empty()) {
            delete static_cast<std::ofstream*>(g_output_stream);
        }
        return 1;
    }
    return 0;
}