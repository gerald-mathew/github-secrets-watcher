#include "scanner.h"
#include "utils.h"
#include <cstdio>
#include <array>
#include <memory>
#include <stdexcept>
#include <regex>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <direct.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace scanner {

    std::map<std::string, std::string> scan_repo_history(const std::string& repo_path, int depth) {
        (void)depth; // suppress unused warning - depth is used in the clone step by the caller
        std::map<std::string, std::string> file_to_commit; // file_path -> commit_hash

        // Directories to exclude (to avoid scanning node_modules, etc.)
        const std::set<std::string> EXCLUDED_DIRS = {
            "node_modules", ".git", "__pycache__", "dist", "build", "coverage",
            ".next", ".nuxt", "vendor", "bower_components", ".svelte-kit",
            ".cache", ".parcel", ".webpack", ".turbo", ".expo", "android", "ios"
        };

        // Patterns that suggest environment/configuration files
        std::regex env_pattern(R"(\.(env|env\.)|config|settings|secrets)", std::regex::icase);
        // Extensions that are likely safe (templates, documentation, etc.)
        const std::set<std::string> SAFE_EXTENSIONS = {".example", ".template", ".md", ".txt", ".gitignore", ".sample"};
        // Hash validation regex (40 hex characters)
        std::regex hash_re(R"([0-9a-f]{40})");

        // Execute git log command to get commit hashes and file names
        // Format: %H (hash) then newline-separated filenames, blank line between commits
        std::string cmd = "git log --all --pretty=format:%H --name-only";

        // Save current directory
        char* cwd = nullptr;
#ifdef _WIN32
        cwd = _getcwd(nullptr, 0);
        if (!cwd) {
            throw std::runtime_error("Failed to get current directory");
        }
#else
        cwd = getcwd(nullptr, 0);
        if (!cwd) {
            throw std::runtime_error("Failed to get current directory");
        }
#endif
        std::string old_cwd(cwd);
        free(cwd);

        // Change to repo path
#ifdef _WIN32
        if (_chdir(repo_path.c_str()) != 0) {
#else
        if (chdir(repo_path.c_str()) != 0) {
#endif
#ifdef _WIN32
            _chdir(old_cwd.c_str());
#else
            chdir(old_cwd.c_str());
#endif
            throw std::runtime_error("Failed to change directory to " + repo_path);
        }

        FILE* pipe = popen(cmd.c_str(), "r");
        if (!pipe) {
#ifdef _WIN32
            _chdir(old_cwd.c_str());
#else
            chdir(old_cwd.c_str());
#endif
            throw std::runtime_error("Failed to run git log command");
        }

        std::array<char, 128> buffer;
        std::string result;
        while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
            result += buffer.data();
        }
        int exitCode = pclose(pipe);
#ifdef _WIN32
        _chdir(old_cwd.c_str());
#else
        chdir(old_cwd.c_str());
#endif

        if (exitCode != 0) {
            throw std::runtime_error("Git log command failed with exit code " + std::to_string(exitCode));
        }

        // Parse the output: lines are either commit hash (40 hex) or file paths
        std::vector<std::string> lines = utils::split(result, '\n');
        std::string current_hash;

        for (const std::string& line : lines) {
            std::string trimmed = utils::trim(line);
            if (trimmed.empty()) {
                // Empty line separates commits; we keep current_hash as is (it will be overwritten by next hash)
                continue;
            }

            // Check if line is a commit hash
            if (std::regex_match(trimmed, hash_re)) {
                current_hash = trimmed;
                continue;
            }

            // Otherwise, it's a filename (could be relative path)
            if (current_hash.empty()) {
                // Should not happen, but skip
                continue;
            }

            std::string file_path = trimmed;
            // Normalize path separators to forward slash for consistency
            std::replace(file_path.begin(), file_path.end(), '\\', '/');

            // Skip if any part of the path is in an excluded directory
            std::vector<std::string> parts = utils::split(file_path, '/');
            bool skip = false;
            for (const std::string& part : parts) {
                if (EXCLUDED_DIRS.find(part) != EXCLUDED_DIRS.end()) {
                    skip = true;
                    break;
                }
            }
            if (skip) {
                continue;
            }

            std::string basename = utils::split(file_path, '/').back();

            // Check if it matches our env pattern
            if (!std::regex_search(basename, env_pattern)) {
                continue;
            }

            // Check if it ends with a safe extension
            bool is_safe = false;
            for (const std::string& ext : SAFE_EXTENSIONS) {
                if (basename.size() >= ext.size() &&
                    basename.compare(basename.size() - ext.size(), ext.size(), ext) == 0) {
                    is_safe = true;
                    break;
                }
            }
            if (is_safe) {
                continue;
            }

            // Record the first (most recent) commit we see for this file
            if (file_to_commit.find(file_path) == file_to_commit.end()) {
                file_to_commit[file_path] = current_hash;
            }
        }

        return file_to_commit;
    }

} // namespace scanner
