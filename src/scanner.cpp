#include "scanner.hpp"
#include "utils.hpp"
#include <cstdio>
#include <array>
#include <memory>
#include <stdexcept>
#include <regex>
#include <set>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <git2.h>

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

        git_repository* repo = nullptr;
        git_revwalk* walk = nullptr;
        git_oid oid;
        char oid_str[GIT_OID_HEXSZ + 1];

        // Open the repository
        int error = git_repository_open(&repo, repo_path.c_str());
        if (error < 0) {
            const git_error* e = giterr_last();
            throw std::runtime_error("Failed to open repository: " + std::string(e->message));
        }

        // Create a revision walker
        error = git_revwalk_new(&walk, repo);
        if (error < 0) {
            git_repository_free(repo);
            const git_error* e = giterr_last();
            throw std::runtime_error("Failed to create revision walker: " + std::string(e->message));
        }

        // Push HEAD to the walker (we want all commits reachable from HEAD)
        error = git_revwalk_push_head(walk);
        if (error < 0) {
            git_revwalk_free(walk);
            git_repository_free(repo);
            const git_error* e = giterr_last();
            throw std::runtime_error("Failed to push HEAD to revision walker: " + std::string(e->message));
        }

        // Walk through commits
        while ((error = git_revwalk_next(&oid, walk)) == 0) {
            // Convert OID to string
            git_oid_tostr(oid_str, sizeof(oid_str), &oid);
            std::string commit_hash(oid_str);

            // Lookup the commit
            git_commit* commit = nullptr;
            error = git_commit_lookup(&commit, repo, &oid);
            if (error < 0) {
                // Skip this commit if we can't lookup
                continue;
            }

            // Get the commit's tree
            git_tree* tree = nullptr;
            error = git_commit_tree(&tree, commit);
            if (error < 0) {
                git_commit_free(commit);
                continue;
            }

            // Get the commit OID for the payload
            git_oid commit_oid;
            git_oid_cpy(&commit_oid, git_commit_id(commit));

            // Tree entry callback to collect file paths
            auto tree_cb = [](const char* root, const git_tree_entry* entry, void* payload) -> int {
                auto* data = static_cast<std::pair<std::map<std::string, std::string>*, const git_oid*>*>(payload);
                std::map<std::string, std::string>& file_to_commit = *(data->first);
                const git_oid* commit_oid = data->second;

                // Get file path
                std::string file_path = std::string(root) + "/" + git_tree_entry_name(entry);

                // Use filesystem for path normalization
                std::filesystem::path path_obj(file_path);
                std::string normalized_path = path_obj.generic_string();

                // Skip if any part of the path is in an excluded directory
                bool skip = false;
                for (const auto& part : path_obj) {
                    const std::set<std::string> EXCLUDED_DIRS = {
                        "node_modules", ".git", "__pycache__", "dist", "build", "coverage",
                        ".next", ".nuxt", "vendor", "bower_components", ".svelte-kit",
                        ".cache", ".parcel", ".webpack", ".turbo", ".expo", "android", "ios"
                    };
                    if (EXCLUDED_DIRS.find(part.string()) != EXCLUDED_DIRS.end()) {
                        skip = true;
                        break;
                    }
                }
                if (skip) return 0;

                std::string basename = path_obj.filename().string();

                // Patterns that suggest environment/configuration files
                std::regex env_pattern(R"(\.(env|env\.)|config|settings|secrets)", std::regex::icase);
                // Extensions that are likely safe (templates, documentation, etc.)
                const std::set<std::string> SAFE_EXTENSIONS = {".example", ".template", ".md", ".txt", ".gitignore", ".sample"};

                // Check if it matches our env pattern
                if (!std::regex_search(basename, env_pattern)) {
                    return 0;
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
                    return 0;
                }

                // Record the first (most recent) commit we see for this file
                char commit_oid_str[GIT_OID_HEXSZ + 1];
                git_oid_tostr(commit_oid_str, sizeof(commit_oid_str), commit_oid);
                std::string commit_hash_str(commit_oid_str);

                if (file_to_commit.find(normalized_path) == file_to_commit.end()) {
                    file_to_commit[normalized_path] = commit_hash_str;
                }
                return 0;
            };

            // Payload for the callback
            std::pair<std::map<std::string, std::string>*, const git_oid*> payload(&file_to_commit, &commit_oid);

            // Walk the tree recursively
            git_tree_walk(tree, GIT_TREEWALK_PRE, tree_cb, &payload);

            // Cleanup
            git_tree_free(tree);
            git_commit_free(commit);
        }

        // Cleanup
        git_revwalk_free(walk);
        git_repository_free(repo);

        if (error < 0 && error != GIT_ITEROVER) {
            const git_error* e = giterr_last();
            throw std::runtime_error("Error walking revisions: " + std::string(e->message));
        }

        return file_to_commit;
    }
} // namespace scanner
