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

    // Constants for file scanning - defined at file scope to avoid recreation
    static const std::set<std::string> EXCLUDED_DIRS = {
        "node_modules", ".git", "__pycache__", "dist", "build", "coverage",
        ".next", ".nuxt", "vendor", "bower_components", ".svelte-kit",
        ".cache", ".parcel", ".webpack", ".turbo", ".expo", "android", "ios"
    };

    static const std::regex env_pattern(R"(\.(env|env\.)|config|settings|secrets)", std::regex::icase);
    static const std::set<std::string> SAFE_EXTENSIONS = {".example", ".template", ".md", ".txt", ".gitignore", ".sample"};

    // Safe accessor for libgit2's last error (giterr_last may return nullptr)
    static std::string git_error_message() {
        const git_error* e = giterr_last();
        return e && e->message ? std::string(e->message) : std::string("unknown libgit2 error");
    }

    std::map<std::string, std::string> scan_repo_history(const std::string& repo_path, int depth) {
        (void)depth; // suppress unused warning - depth is used in the clone step by the caller
        std::map<std::string, std::string> file_to_commit; // file_path -> commit_hash

        git_repository* repo = nullptr;
        git_revwalk* walk = nullptr;
        git_oid oid;
        char oid_str[GIT_OID_HEXSZ + 1];

        // Open the repository
        int error = git_repository_open(&repo, repo_path.c_str());
        if (error < 0) {
            throw std::runtime_error("Failed to open repository: " + git_error_message());
        }

        // Create a revision walker
        error = git_revwalk_new(&walk, repo);
        if (error < 0) {
            git_repository_free(repo);
            throw std::runtime_error("Failed to create revision walker: " + git_error_message());
        }

        // Push HEAD to the walker (we want all commits reachable from HEAD)
        error = git_revwalk_push_head(walk);
        if (error < 0) {
            git_revwalk_free(walk);
            git_repository_free(repo);
            // Empty/unborn repository (no commits yet) - nothing to scan
            return {};
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
                std::string file_path = (root && *root) ? (std::string(root) + "/" + git_tree_entry_name(entry))
                                                        : std::string(git_tree_entry_name(entry));

                // Use filesystem for path normalization
                std::filesystem::path path_obj(file_path);
                std::string normalized_path = path_obj.generic_string();

                // git_tree_walk reports the top-level tree with an empty root;
                // ensure root-level files never get a leading "/" (that would
                // produce a broken blob link later).
                if (!normalized_path.empty() && normalized_path.front() == '/') {
                    normalized_path.erase(normalized_path.begin());
                }

                // Directories to exclude (to avoid scanning node_modules, etc.)
                bool skip = false;
                for (const auto& part : path_obj) {
                    if (EXCLUDED_DIRS.find(part.string()) != EXCLUDED_DIRS.end()) {
                        skip = true;
                        break;
                    }
                }
                if (skip) return 0;

                std::string basename = path_obj.filename().string();

                // Patterns and safe extensions are file-scope statics - the
                // regex is compiled exactly once instead of per tree entry.
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
            throw std::runtime_error("Error walking revisions: " + git_error_message());
        }

        return file_to_commit;
    }
} // namespace scanner