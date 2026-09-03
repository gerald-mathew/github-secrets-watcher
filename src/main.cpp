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

#include "github.hpp"
#include "scanner.hpp"
#include "utils.hpp"
#include "thread_pool.hpp"
#include <git2.h>

// Helper function to get current timestamp for logging
std::string get_timestamp() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &time_t); // Windows
#else
    localtime_r(&time_t, &tm); // Linux/macOS
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm, "%H:%M:%S");
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
};

// Structure to hold the result of scanning a repository
struct RepoResult {
    std::string repo_name;
    std::string html_url;
    bool success;
    std::string error_message;
    std::vector<FoundFile> files;
    size_t index; // For progress tracking in output
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
            throw std::runtime_error("Failed to initialize libgit2: " + std::string(e->message));
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

// Function to create a temporary directory for cloning
std::string create_temp_dir(const std::string& repo_name) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    fs::path temp_base_path = std::getenv("TEMP") ? fs::path(std::getenv("TEMP")) : fs::path("/tmp");
    fs::path temp_dir_path = temp_base_path / ("github_secrets_watcher_" + repo_name + "_" + std::to_string(millis));

    // Create directory using filesystem
    try {
        fs::create_directories(temp_dir_path);
    } catch (const fs::filesystem_error& e) {
        throw std::runtime_error("Failed to create temporary directory: " + temp_dir_path.string() + " - " + e.what());
    }
    return temp_dir_path.string();
}

// Function to remove a directory and its contents
void remove_dir(const std::string& dir_path) {
    // Remove directory using filesystem
    try {
        fs::remove_all(dir_path);
        // Ignore errors for cleanup as before
    } catch (const fs::filesystem_error& e) {
        // Ignore errors for cleanup as before
        // Optionally log warning: std::cerr << "Warning: Failed to remove directory " << dir_path << ": " << e.what() << std::endl;
    }
}

// Print usage information
void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " scan -u <USERNAME> [-t <TOKEN>] [-d <DEPTH>] [-m <NUM>] [-p] [-n <NUM>] [-f <FMT>] [-o <FILE>] [-v]\n\n";
    std::cout << "Options:\n";
    std::cout << "  -u, --username <USERNAME>   GitHub username (required)\n";
    std::cout << "  -t, --token <TOKEN>         GitHub personal access token (optional, for private repos and higher rate limits)\n";
    std::cout << "  -d, --depth <NUM>           Commits to scan in history (default: 100)\n";
    std::cout << "  -m, --max-repos <NUM>       Maximum repositories to scan (default: all)\n";
    std::cout << "  -p, --include-private       Include private repositories (requires token)\n";
    std::cout << "  -n, --threads <NUM>         Number of threads to use for scanning (default: hardware concurrency)\n";
    std::cout << "  -f, --format <FMT>          Output format: text, json, or csv (default: text)\n";
    std::cout << "  -o, --output <FILE>         Output file path (default: stdout)\n";
    std::cout << "  -v, --verbose               Enable verbose output\n";
}

// Function to process a single repository (cloning and scanning)
void process_repository(const Repository& repo, int depth, const std::string& username, const std::optional<std::string>& token, size_t index, bool verbose) {
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

    // Create a temporary directory for cloning (read-only)
    std::string temp_dir = create_temp_dir(repo.name);

    bool success = true;
    std::string error_message;
    std::vector<FoundFile> files;

    try {
        // Determine clone URL: use token if provided for authentication
        std::string clone_url;
        if (token.has_value()) {
            // Use token for authentication (works for both public and private repos)
            clone_url = "https://" + token.value() + "@github.com/" + username + "/" + repo.name + ".git";
        } else {
            // No token, use public URL
            clone_url = "https://github.com/" + username + "/" + repo.name + ".git";
        }
        if (verbose) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << "Cloning repository (depth=" << depth << ")..." << std::endl;
        }

        // Clone repository using libgit2
        git_clone_options clone_opts = GIT_CLONE_OPTIONS_INIT;
        clone_opts.checkout_branch = nullptr; // Use default branch

        // Set depth if specified (shallow clone)
        if (depth > 0) {
            clone_opts.fetch_opts.depth = depth;
            // localclone is not a member in this version, using default behavior
        }

        // Set up credentials if token is provided
        git_cred* cred = nullptr;
        if (token.has_value()) {
            git_cred_userpass_plaintext_new(&cred, token.value().c_str(), "x-oauth-basic");
            clone_opts.fetch_opts.callbacks.credentials = [](git_cred** out, const char* url, const char* username_from_url, unsigned int allowed_types, void* payload) -> int {
                (void)url;
                (void)username_from_url;
                (void)allowed_types;
                git_cred* cred = *static_cast<git_cred**>(payload);
                *out = cred;
                return 0;
            };
            clone_opts.fetch_opts.callbacks.payload = &cred;
        }

        // Perform the clone
        git_repository* cloned_repo = nullptr;
        int error = git_clone(&cloned_repo, clone_url.c_str(), temp_dir.c_str(), &clone_opts);

        // Cleanup credentials
        if (cred) {
            git_cred_free(cred);
        }

        if (error < 0) {
            const git_error* e = giterr_last();
            if (verbose) {
                std::lock_guard<std::mutex> lock(g_console_mutex);
                std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "[" << get_timestamp() << "] Git clone failed: " << std::string(e->message) << std::endl;
            }
            throw std::runtime_error("Git clone failed: " + std::string(e->message));
        }

        // Cleanup cloned repository handle (we don't need to keep it open)
        if (cloned_repo) {
            git_repository_free(cloned_repo);
        }

        // Scan history for env-like files
        if (verbose) {
            std::lock_guard<std::mutex> lock(g_console_mutex);
            std::cerr << COLOR_INFO << "[INFO] " << COLOR_RESET << "[" << get_timestamp() << "] Scanning history..." << std::endl;
        }
        std::map<std::string, std::string> file_to_commit = scanner::scan_repo_history(temp_dir, depth);

        if (!file_to_commit.empty()) {
            if (verbose) {
                std::lock_guard<std::mutex> lock(g_console_mutex);
                std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "[" << get_timestamp() << "] Found " << file_to_commit.size() << " potential environment/configuration files:" << std::endl;
            }
            for (const auto& [file_path, commit_hash] : file_to_commit) {
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
                    std::cerr << "     - " << file_path << std::endl;
                    std::cerr << "       " << COLOR_LINK << "[LINK] " << COLOR_RESET << file_url << std::endl;
                }

                // Add to our local files vector
                files.push_back({file_path, commit_hash});
            }
        } else {
            if (verbose) {
                std::lock_guard<std::mutex> lock(g_console_mutex);
                std::cerr << COLOR_OK << "[OK] " << COLOR_RESET << "No potential environment files found in history." << std::endl;
            }
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

    // Clean up temporary directory
    remove_dir(temp_dir);

    // Add the result to the global results
    {
        std::lock_guard<std::mutex> lock(g_results_mutex);
        g_results.push_back({repo.name, repo.html_url, success, error_message, files, index});
    }
}

// Helper function to output the results in the chosen format
void output_results(bool verbose) {
    std::lock_guard<std::mutex> lock(g_output_mutex);
    switch (g_output_format) {
        case OutputFormat::JSON: {
            *g_output_stream << "[\n";
            for (size_t i = 0; i < g_results.size(); ++i) {
                const auto& r = g_results[i];
                *g_output_stream << "  {\n";
                *g_output_stream << "    \"repo_name\": \"" << r.repo_name << "\",\n";
                *g_output_stream << "    \"html_url\": \"" << r.html_url << "\",\n";
                *g_output_stream << "    \"success\": " << (r.success ? "true" : "false") << ",\n";
                if (!r.error_message.empty()) {
                    *g_output_stream << "    \"error_message\": \"" << r.error_message << "\",\n";
                }
                *g_output_stream << "    \"files\": [\n";
                for (size_t j = 0; j < r.files.size(); ++j) {
                    const auto& f = r.files[j];
                    *g_output_stream << "      {\n";
                    *g_output_stream << "        \"path\": \"" << f.path << "\",\n";
                    *g_output_stream << "        \"commit_hash\": \"" << f.commit_hash << "\"\n";
                    *g_output_stream << "      }";
                    if (j != r.files.size() - 1) {
                        *g_output_stream << ",";
                    }
                    *g_output_stream << "\n";
                }
                *g_output_stream << "    ]\n";
                *g_output_stream << "  }";
                if (i != g_results.size() - 1) {
                    *g_output_stream << ",";
                }
                *g_output_stream << "\n";
            }
            *g_output_stream << "]\n";
            break;
        }
        case OutputFormat::CSV: {
            *g_output_stream << "repo_name,success,error_message,file_path,commit_hash\n";
            for (const auto& r : g_results) {
                if (r.files.empty()) {
                    *g_output_stream << r.repo_name << "," << (r.success ? "true" : "false") << ",";
                    if (!r.error_message.empty()) {
                        *g_output_stream << "\"" << r.error_message << "\"";
                    }
                    *g_output_stream << ",,\n";
                } else {
                    for (size_t j = 0; j < r.files.size(); ++j) {
                        const auto& f = r.files[j];
                        *g_output_stream << r.repo_name << "," << (r.success ? "true" : "false") << ",";
                        if (!r.error_message.empty()) {
                            *g_output_stream << "\"" << r.error_message << "\"";
                        }
                        *g_output_stream << "," << f.path << "," << f.commit_hash << "\n";
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
                    *g_output_stream << "  Found " << r.files.size() << " potential environment/configuration files:\n";
                    for (const auto& f : r.files) {
                        *g_output_stream << "    - " << f.path << "\n";
                        std::string repo_url = utils::remove_trailing_slash(r.html_url);
                        std::string encoded_file_path = utils::url_encode(f.path);
                        std::string file_url = repo_url + "/blob/" + f.commit_hash + "/" + encoded_file_path;
                        *g_output_stream << "      [LINK] " << file_url << "\n";
                    }
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
    int depth = 100;
    std::optional<int> max_repos = std::nullopt;
    bool include_private = false;
    bool verbose = false;
    int thread_count = static_cast<int>(std::thread::hardware_concurrency());
    if (thread_count <= 0) thread_count = 4; // fallback
    OutputFormat format = OutputFormat::Text;
    std::string output_file;

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
                if (depth <= 0) {
                    std::cerr << "Error: --depth must be positive\n";
                    return 1;
                }
            } catch (const std::exception&) {
                std::cerr << "Error: --depth must be a number\n";
                return 1;
            }
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
        std::cerr << COLOR_WARN << "[WARN] " << COLOR_RESET << "Warning: --include-private requires a GitHub token for authentication\n";
        std::cerr << "         Falling back to public repositories only.\n";
        include_private = false;
    }

    // Warning about responsible use
    std::cout << COLOR_WARN << "[WARN] " << COLOR_RESET << "WARNING: This tool scans public repository history for educational purposes.\n";
    std::cout << "         Only scan repositories you own or have permission to scan.\n";
    std::cout << "         This tool performs read-only operations and does not modify any repositories.\n\n";

    std::cout << "Do you want to continue? (y/N): ";
    std::string response;
    std::getline(std::cin, response);
    std::transform(response.begin(), response.end(), response.begin(), ::tolower);
    if (response != "y" && response != "yes") {
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scan cancelled.\n";
        return 0;
    }

    // Set global output format and stream
    g_output_format = format;
    if (!output_file.empty()) {
        std::ofstream* ofs = new std::ofstream(output_file);
        if (!ofs->is_open()) {
            std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Failed to open output file: " << output_file << "\n";
            return 1;
        }
        g_output_stream = ofs;
    }

    try {
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scanning " << (include_private ? "public and private" : "public") << " repositories for user: " << username << "\n";
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "History depth: " << depth << " commits\n";
        if (max_repos.has_value()) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Maximum repos to scan: " << max_repos.value() << "\n";
        }
        std::cout << "\n";

        // Get list of repositories
        std::vector<Repository> repos = github::get_user_repos(username, token, include_private);
        if (repos.empty()) {
            std::cout << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "No repositories found or error occurred.\n";
            return 0;
        }

        if (max_repos.has_value()) {
            if (static_cast<int>(repos.size()) > max_repos.value()) {
                repos.resize(max_repos.value());
            }
        }

        std::string repo_type = include_private ? "public and private" : "public";
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Found " << repos.size() << " " << repo_type << " repositories to scan.\n\n";

        // Process each repository using thread pool
        {
            // Reset progress tracking
            g_completed_count = 0;
            g_total_repos = repos.size();

            ThreadPool pool(thread_count);

            size_t index = 0;
            for (const auto& repo : repos) {
                pool.enqueue(process_repository, repo, depth, username, token, index, verbose);
                index++;
            }
            // pool goes out of scope here, waiting for all threads
        }

        // After all threads are done, move to next line for progress indicator
        if (!verbose) {
            std::cerr << std::endl;
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
