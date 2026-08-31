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

#include "github.h"
#include "scanner.h"
#include "utils.h"

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

// Print usage information
void print_usage(const std::string& prog_name) {
    std::cout << "Usage: " << prog_name << " scan --username <USERNAME> [--token <TOKEN>] [--depth <DEPTH>] [--max-repos <NUM>]\n\n";
    std::cout << "Examples:\n";
    std::cout << "  " << prog_name << " scan --username octocat\n";
    std::cout << "  " << prog_name << " scan --username octocat --token ghp_abcdefghijklmnopqrstuvwxyz0123456789 --depth 50\n";
}

// Function to create a temporary directory for cloning
std::string create_temp_dir(const std::string& repo_name) {
    auto now = std::chrono::system_clock::now();
    auto duration = now.time_since_epoch();
    auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
    std::string temp_base = std::getenv("TEMP") ? std::getenv("TEMP") : "/tmp";
    std::string temp_dir = temp_base + "/github_secrets_watcher_" + repo_name + "_" + std::to_string(millis);

    // Create directory
    #ifdef _WIN32
        std::string mkdir_cmd = "mkdir \"" + temp_dir + "\"";
    #else
        std::string mkdir_cmd = "mkdir -p \"" + temp_dir + "\"";
    #endif
    int result = std::system(mkdir_cmd.c_str());
    if (result != 0) {
        throw std::runtime_error("Failed to create temporary directory: " + temp_dir);
    }
    return temp_dir;
}

// Function to remove a directory and its contents
void remove_dir(const std::string& dir_path) {
    #ifdef _WIN32
        std::string rm_cmd = "rmdir /s /q \"" + dir_path + "\"";
    #else
        std::string rm_cmd = "rm -rf \"" + dir_path + "\"";
    #endif
    std::system(rm_cmd.c_str()); // Ignore errors for cleanup
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

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--username") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --username requires a value\n";
                return 1;
            }
            username = argv[++i];
        } else if (arg == "--token") {
            if (i + 1 >= argc) {
                std::cerr << "Error: --token requires a value\n";
                return 1;
            }
            token = argv[++i];
        } else if (arg == "--depth") {
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
        } else if (arg == "--max-repos") {
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

    try {
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scanning public repositories for user: " << username << "\n";
        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "History depth: " << depth << " commits\n";
        if (max_repos.has_value()) {
            std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Maximum repos to scan: " << max_repos.value() << "\n";
        }
        std::cout << "\n";

        // Get list of repositories
        std::vector<Repository> repos = github::get_user_repos(username, token);
        if (repos.empty()) {
            std::cout << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "No repositories found or error occurred.\n";
            return 0;
        }

        if (max_repos.has_value()) {
            if (static_cast<int>(repos.size()) > max_repos.value()) {
                repos.resize(max_repos.value());
            }
        }

        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Found " << repos.size() << " public repositories to scan.\n\n";

        // Process each repository
        for (size_t i = 0; i < repos.size(); i++) {
            const Repository& repo = repos[i];
            std::cout << "[" << (i + 1) << "/" << repos.size() << "] Scanning: " << repo.name << "\n";

            // Create a temporary directory for cloning (read-only)
            std::string temp_dir = create_temp_dir(repo.name);

            try {
                // Clone repository (public, no auth needed for cloning)
                std::string clone_url = "https://github.com/" + username + "/" + repo.name + ".git";
                std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Cloning repository (depth=" << depth << ")...\n";

                #ifdef _WIN32
                    std::string clone_cmd = "cd \"" + temp_dir + "\" && git clone --quiet --depth " + std::to_string(depth) + " \"" + clone_url + "\" .";
                #else
                    std::string clone_cmd = "cd \"" + temp_dir + "\" && git clone --quiet --depth " + std::to_string(depth) + " " + clone_url + " .";
                #endif
                int clone_result = std::system(clone_cmd.c_str());
                if (clone_result != 0) {
                    throw std::runtime_error("Git clone failed with exit code " + std::to_string(clone_result));
                }

                // Scan history for env-like files
                std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scanning history...\n";
                std::map<std::string, std::string> file_to_commit = scanner::scan_repo_history(temp_dir, depth);

                if (!file_to_commit.empty()) {
                    std::cout << COLOR_WARN << "[WARN] " << COLOR_RESET << "Found " << file_to_commit.size() << " potential environment/configuration files:\n";
                    for (const auto& [file_path, commit_hash] : file_to_commit) {
                        // Validate commit hash format (40 hex characters)
                        if (!utils::is_valid_commit_hash(commit_hash)) {
                            std::cout << "     - " << file_path << "\n";
                            std::cout << "       " << COLOR_WARN << "[WARN] " << COLOR_RESET << "Invalid commit hash: " << commit_hash << "\n";
                            continue;
                        }

                        // Construct a link to the file at the specific commit (URL-encode the file path)
                        std::string repo_url = utils::remove_trailing_slash(repo.html_url);
                        std::string encoded_file_path = utils::url_encode(file_path);
                        std::string file_url = repo_url + "/blob/" + commit_hash + "/" + encoded_file_path;

                        std::cout << "     - " << file_path << "\n";
                        std::cout << "       " << COLOR_LINK << "[LINK] " << COLOR_RESET << file_url << "\n";
                    }
                } else {
                    std::cout << COLOR_OK << "[OK] " << COLOR_RESET << "No potential environment files found in history.\n";
                }

            } catch (const std::exception& e) {
                std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Error processing " << repo.name << ": " << e.what() << "\n";
            } catch (...) {
                std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Unknown error processing " << repo.name << "\n";
            }

            // Clean up temporary directory
            remove_dir(temp_dir);

            std::cout << "\n"; // Blank line between repos
        }

        std::cout << COLOR_INFO << "[INFO] " << COLOR_RESET << "Scan complete!\n";

    } catch (const std::exception& e) {
        std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Fatal error: " << e.what() << "\n";
        return 1;
    } catch (...) {
        std::cerr << COLOR_FAIL << "[FAIL] " << COLOR_RESET << "Unknown fatal error\n";
        return 1;
    }

    return 0;
}