#include "scanner.hpp"
#include "utils.hpp"
#include <stdexcept>
#include <regex>
#include <set>
#include <string_view>
#include <filesystem>
#include <git2.h>

namespace scanner {

    // Paths that never contain interesting files - defined at file scope so
    // the set is built once instead of per scan.
    static const std::set<std::string> EXCLUDED_DIRS = {
        "node_modules", ".git", "__pycache__", "dist", "build", "coverage",
        ".next", ".nuxt", "vendor", "bower_components", ".svelte-kit",
        ".cache", ".parcel", ".webpack", ".turbo", ".expo", "android", "ios"
    };

    static const std::regex env_pattern(R"(\.(env|env\.)|config|settings|secrets)", std::regex::icase);
    // Files that cannot be potential secrets even when their name contains
    // "env/config/settings/secrets": documentation, machine-generated build
    // artifacts, source maps, media, and fonts.
    static const std::set<std::string> SAFE_EXTENSIONS = {
        // documentation and templates
        ".example", ".template", ".sample", ".md", ".txt", ".rst", ".adoc",
        // machine-generated build artifacts
        ".lock", ".min.js", ".d.ts", ".map",
        // media and fonts
        ".png", ".jpg", ".jpeg", ".gif", ".webp", ".svg", ".ico", ".bmp",
        ".icns", ".woff", ".woff2", ".ttf", ".otf", ".eot", ".pdf",
    };

    // Test/spec scaffolding repeats the app's module names (config.test.js,
    // settings.spec.ts) but is never a deployed runtime secret.
    static bool is_test_file(const std::string& basename) {
        return basename.find(".test.") != std::string::npos ||
               basename.find(".spec.") != std::string::npos;
    }

    // The penv-style file class: a schema that describes the shape of the
    // environment (names and types) without any values. It must never be
    // reported as a leak - it is the *expected* companion to a secret-less
    // working tree.
    static bool is_schema_file(const std::string& basename) {
        return basename.find(".schema") != std::string::npos;
    }

    // Safe accessor for libgit2's last error (giterr_last may return nullptr)
    static std::string git_error_message() {
        const git_error* e = giterr_last();
        return e && e->message ? std::string(e->message) : std::string("unknown libgit2 error");
    }

    // True for .env/config/settings/secrets paths that are not inside an
    // excluded directory and do not end in a known safe extension.
    static bool looks_interesting(const std::string& relative_path) {
        std::filesystem::path path_obj(relative_path);
        for (const auto& part : path_obj) {
            if (EXCLUDED_DIRS.find(part.string()) != EXCLUDED_DIRS.end()) {
                return false;
            }
        }

        std::string basename = path_obj.filename().string();
        if (!std::regex_search(basename, env_pattern)) {
            return false;
        }
        if (is_test_file(basename)) {
            return false;
        }
        if (is_schema_file(basename)) {
            return false;
        }

        for (const std::string& ext : SAFE_EXTENSIONS) {
            if (basename.size() >= ext.size() &&
                basename.compare(basename.size() - ext.size(), ext.size(), ext) == 0) {
                return false;
            }
        }
        return true;
    }

    // Content-level confidence for a matched file. Name matches alone deserve a
    // review; a file whose bytes also look like real credentials is the
    // high-confidence class and should be acted on first.
    static bool oid_is_zero(const git_oid* oid) {
        for (size_t i = 0; i < sizeof(oid->id); ++i) {
            if (oid->id[i] != 0) return false;
        }
        return true;
    }

    static bool looks_like_secret_text(const char* data, size_t size) {
        if (data == nullptr || size == 0) return false;
        const size_t kProbeLimit = 64 * 1024; // a needle is near the top
        const std::string text(data, std::min(size, kProbeLimit));
        if (text.find('\0') != std::string::npos) return false; // not text

        // key/token/secret/password assigned a value that is not a placeholder
        static const std::regex assignment(
            R"(\b(api[_-]?key|access[_-]?key|secret|password|passwd|passphrase|token|auth|credential)s?\b\s*[=:]\s*["']?([^\s"',;}]+))",
            std::regex::icase);
        static const std::regex placeholder(
            R"(^(<[^>]*>|your[_-].*|change[_-]?me.*|example.*|xxx+|\*+|\.\.\.+|todo.*|fill[_-]?in.*|rep[a-z]*lac[a-z]*.*|none|null|false|true|0|1|n/a|na|empty|test.*)$)",
            std::regex::icase);
        // tokens whose format identifies them regardless of the surrounding name
        static const std::regex known_formats(
            R"((AKIA[0-9A-Z]{16}|ghp_[A-Za-z0-9]{20,}|gho_[A-Za-z0-9]{20,}|github_pat_[A-Za-z0-9_]{20,}|sk_live_[0-9A-Za-z]{16,}|xox[baprs]-[A-Za-z0-9-]{10,}|eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{9,}\.[A-Za-z0-9_-]{9,}|-----BEGIN ([A-Z0-9 ':]*)?PRIVATE KEY-----))",
            std::regex::icase);

        std::smatch m;
        std::string::const_iterator it = text.cbegin();
        while (it != text.cend() && std::regex_search(it, text.cend(), m, assignment)) {
            const std::string value = m[2].str();
            if (value.size() >= 3 && !std::regex_match(value, placeholder)) {
                return true;
            }
            it = m[0].second;
        }
        return std::regex_search(text, known_formats);
    }

    // Best-effort probe: resolve a blob and run the content heuristic. The
    // object may be missing in shallow clones (a deleted file's blob lives in
    // the parent tree), in which case nothing is concluded.
    static bool blob_looks_secret(git_repository* repo, const git_oid* oid) {
        if (oid == nullptr || oid_is_zero(oid)) return false;
        git_blob* blob = nullptr;
        if (git_blob_lookup(&blob, repo, oid) < 0) return false;
        const char* data = static_cast<const char*>(git_blob_rawcontent(blob));
        const bool secret = looks_like_secret_text(data, git_blob_rawsize(blob));
        git_blob_free(blob);
        return secret;
    }

    struct SeedContext {
        ScanResult* result;
        const std::string* tip_hash;
        git_repository* repo;
    };

    struct DeltaContext {
        ScanResult* result;
        const std::string* current_hash;
        const std::string* deleted_hash;
        git_repository* repo;
    };

    // Tree-walk callback used once to seed the result from the newest tip: a
    // file that still exists there is at its most recent known commit.
    static int seed_tree_cb(const char* root, const git_tree_entry* entry, void* payload) {
        auto* ctx = static_cast<SeedContext*>(payload);

        std::string file_path = (root && *root) ? (std::string(root) + "/" + git_tree_entry_name(entry))
                                                : std::string(git_tree_entry_name(entry));
        std::filesystem::path path_obj(file_path);
        std::string normalized_path = path_obj.generic_string();
        // git_tree_walk reports the top-level tree with an empty root; ensure
        // root-level files never get a leading "/".
        if (!normalized_path.empty() && normalized_path.front() == '/') {
            normalized_path.erase(normalized_path.begin());
        }

        if (looks_interesting(normalized_path)) {
            ctx->result->file_to_commit[normalized_path] = *ctx->tip_hash;
            if (git_tree_entry_filemode(entry) != GIT_FILEMODE_TREE &&
                blob_looks_secret(ctx->repo, git_tree_entry_id(entry))) {
                ctx->result->likely_secret[normalized_path] = true;
            }
        }
        return 0;
    }

    // Diff iteration callback. After the tip is seeded, new findings can only
    // come from files that were removed again (their newest existing commit is
    // the parent of the deleting commit). ADD/MODIFY are kept as a fallback for
    // files that only live on non-tip branches.
    static int delta_cb(const git_diff_delta* delta, float progress, void* payload) {
        (void)progress;
        auto* ctx = static_cast<DeltaContext*>(payload);

        const std::string_view path = delta->status == GIT_DELTA_DELETED
            ? std::string_view(delta->old_file.path)
            : std::string_view(delta->new_file.path);
        if (path.empty() || !looks_interesting(std::string(path))) {
            return 0;
        }

        // Keep the first (newest) record only.
        if (ctx->result->file_to_commit.find(std::string(path)) != ctx->result->file_to_commit.end()) {
            return 0;
        }

        if (delta->status == GIT_DELTA_DELETED) {
            ctx->result->file_to_commit[std::string(path)] = *ctx->deleted_hash;
            if (blob_looks_secret(ctx->repo, &delta->old_file.id)) {
                ctx->result->likely_secret[std::string(path)] = true;
            }
        } else {
            ctx->result->file_to_commit[std::string(path)] = *ctx->current_hash;
            if (blob_looks_secret(ctx->repo, &delta->new_file.id)) {
                ctx->result->likely_secret[std::string(path)] = true;
            }
        }
        return 0;
    }

    // Walk a repository's history and collect matched environment/config files
    // with the most recent commit in which each is known to exist. See
    // scanner.hpp for the semantics of `depth` and `early_exit`.
    ScanResult scan_repo_history(const std::string& repo_path, int depth, size_t early_exit) {
        ScanResult result;
        const bool early_exit_applicable = (depth <= 0 && early_exit > 0);

        git_repository* repo = nullptr;
        git_revwalk* walk = nullptr;
        git_tree* empty_tree = nullptr;

        if (git_repository_open(&repo, repo_path.c_str()) < 0) {
            throw std::runtime_error("Failed to open repository: " + git_error_message());
        }

        // A reusable empty tree serves as the "previous" tree for root commits
        // and shallow-boundary commits whose parent objects are missing.
        {
            git_oid empty_oid;
            git_treebuilder* builder = nullptr;
            if (git_treebuilder_new(&builder, repo, nullptr) < 0 ||
                git_treebuilder_write(&empty_oid, builder) < 0 ||
                git_tree_lookup(&empty_tree, repo, &empty_oid) < 0) {
                if (builder) git_treebuilder_free(builder);
                git_tree_free(empty_tree);
                git_repository_free(repo);
                throw std::runtime_error("Failed to prepare empty tree: " + git_error_message());
            }
            git_treebuilder_free(builder);
        }

        if (git_revwalk_new(&walk, repo) < 0) {
            git_tree_free(empty_tree);
            git_repository_free(repo);
            throw std::runtime_error("Failed to create revision walker: " + git_error_message());
        }

        // Walk every branch children-before-parents so a commit is always
        // processed before its ancestors (deterministic attribution even when
        // commit timestamps collide). Cached bare clones only keep
        // refs/remotes/*; fresh clones have refs/heads/*.
        git_revwalk_sorting(walk, GIT_SORT_TOPOLOGICAL);
        git_revwalk_push_glob(walk, "refs/heads/*");
        git_revwalk_push_glob(walk, "refs/remotes/origin/*");

        char oid_buf[65]; // SHA-1 (40) and SHA-256 (64) commit hashes
        bool seeded = false;
        size_t stale = 0;
        const size_t budget = depth > 0 ? static_cast<size_t>(depth) : 0;

        while (true) {
            git_oid oid;
            int err = git_revwalk_next(&oid, walk);
            if (err == GIT_ITEROVER) break;
            if (err < 0) {
                git_revwalk_free(walk);
                git_repository_free(repo);
                git_tree_free(empty_tree);
                throw std::runtime_error("Error walking revisions: " + git_error_message());
            }

            if (budget > 0 && result.commits_considered >= budget) {
                result.truncated_by_depth = true;
                break;
            }

            git_commit* commit = nullptr;
            if (git_commit_lookup(&commit, repo, &oid) < 0) {
                continue; // missing object; skip this commit
            }
            ++result.commits_considered;

            git_oid_tostr(oid_buf, sizeof(oid_buf), &oid);
            std::string current_hash(oid_buf);

            git_tree* tree = nullptr;
            if (git_commit_tree(&tree, commit) < 0 || tree == nullptr) {
                git_commit_free(commit);
                continue;
            }

            if (!seeded) {
                // First commit processed (a tip): files present here still
                // exist, so this is their most recent known commit. The tip is
                // also often the commit that deleted an older file, so its diff
                // is still examined below.
                SeedContext ctx{&result, &current_hash, repo};
                git_tree_walk(tree, GIT_TREEWALK_PRE, seed_tree_cb, &ctx);
                seeded = true;
            }

            // Diff this commit against its first parent. Comparing the two
            // trees instead of re-walking the full tree makes the cost
            // proportional to the changed paths, not depth * repository size.
            git_tree* parent_tree = nullptr;
            std::string deleted_hash;
            if (git_commit_parentcount(commit) > 0) {
                const git_oid* parent_oid = git_commit_parent_id(commit, 0);
                if (parent_oid) {
                    git_oid_tostr(oid_buf, sizeof(oid_buf), parent_oid);
                    deleted_hash.assign(oid_buf);
                }
                git_commit* parent = nullptr;
                if (git_commit_parent(&parent, commit, 0) == 0 && parent != nullptr) {
                    git_commit_tree(&parent_tree, parent); // best effort
                    git_commit_free(parent);
                } else if (!deleted_hash.empty()) {
                    // The parent object is unavailable: a shallow fetch cut the
                    // history here, so older commits were never obtained.
                    result.truncated_by_depth = true;
                }
            }
            if (parent_tree == nullptr) {
                parent_tree = empty_tree;
            }

            // Identical tree to its parent: nothing changed here, skip the diff.
            if (git_oid_equal(git_tree_id(tree), git_tree_id(parent_tree))) {
                ++result.commits_skipped;
                if (parent_tree != empty_tree) git_tree_free(parent_tree);
                git_tree_free(tree);
                git_commit_free(commit);
                if (early_exit_applicable && ++stale >= early_exit) {
                    result.stopped_early = true;
                    break;
                }
                continue;
            }

            const size_t before = result.file_to_commit.size();
            git_diff* diff = nullptr;
            git_diff_options diff_opts = GIT_DIFF_OPTIONS_INIT;
            if (git_diff_tree_to_tree(&diff, repo, parent_tree, tree, &diff_opts) == 0 && diff != nullptr) {
                DeltaContext ctx{&result, &current_hash, &deleted_hash, repo};
                git_diff_foreach(diff, delta_cb, nullptr, nullptr, nullptr, &ctx);
                git_diff_free(diff);
            }

            if (parent_tree != empty_tree) git_tree_free(parent_tree);
            git_tree_free(tree);
            git_commit_free(commit);

            // Early exit: stop once enough consecutive commits changed nothing.
            if (result.file_to_commit.size() == before) {
                if (early_exit_applicable && ++stale >= early_exit) {
                    result.stopped_early = true;
                    break;
                }
            } else {
                stale = 0;
            }
        }

        git_revwalk_free(walk);
        git_repository_free(repo);
        git_tree_free(empty_tree);
        return result;
    }
} // namespace scanner