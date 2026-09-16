#include <catch2/catch_test_macros.hpp>
#include <git2.h>
#include <fstream>
#include <chrono>
#include <stdexcept>
#include <filesystem>

#include "scanner.hpp"
#include "utils.hpp"

namespace fs = std::filesystem;

namespace {

// Stage all workdir changes and create a commit pointing HEAD at it;
// returns the new commit's hash.
std::string make_commit(git_repository* repo, const std::string& email, const std::string& message) {
    git_index* index = nullptr;
    if (git_repository_index(&index, repo) < 0) {
        throw std::runtime_error("failed to open index");
    }
    git_strarray pathspec = {nullptr, 0};
    if (git_index_add_all(index, &pathspec, GIT_INDEX_ADD_DEFAULT, nullptr, nullptr) < 0) {
        git_index_free(index);
        throw std::runtime_error("failed to stage files");
    }
    git_index_write(index);

    git_oid tree_oid;
    if (git_index_write_tree(&tree_oid, index) < 0) {
        git_index_free(index);
        throw std::runtime_error("failed to write tree");
    }
    git_tree* tree = nullptr;
    if (git_tree_lookup(&tree, repo, &tree_oid) < 0) {
        git_index_free(index);
        throw std::runtime_error("failed to lookup tree");
    }

    git_signature* sig = nullptr;
    if (git_signature_now(&sig, "Scanner Test", email.c_str()) < 0) {
        git_tree_free(tree);
        git_index_free(index);
        throw std::runtime_error("failed to create signature");
    }

    // Re-parent onto the current HEAD so multi-commit histories stay linear.
    git_commit* parent_obj = nullptr;
    const git_commit* parents[1] = {nullptr};
    unsigned int parent_count = 0;
    git_oid head_oid;
    if (git_reference_name_to_id(&head_oid, repo, "HEAD") == 0 &&
        git_commit_lookup(&parent_obj, repo, &head_oid) == 0) {
        parents[0] = parent_obj;
        parent_count = 1;
    }

    git_oid commit_oid;
    int err = git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                                message.c_str(), tree, parent_count, parents);
    if (parent_obj) git_commit_free(parent_obj);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(index);
    if (err < 0) {
        const git_error* e = git_error_last();
        throw std::runtime_error(std::string("failed to create commit: ") +
                                 (e && e->message ? e->message : "unknown libgit2 error"));
    }

    char buf[65]; // SHA-1 and SHA-256 commit hashes
    git_oid_tostr(buf, sizeof(buf), &commit_oid);
    return std::string(buf);
}

fs::path make_temp_repo(git_repository** out) {
    fs::path dir = fs::temp_directory_path() /
                   ("gsw_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    REQUIRE(git_repository_init(out, dir.string().c_str(), 0) == 0);
    return dir;
}

void write_file(const fs::path& base, const std::string& name, const std::string& contents) {
    fs::path full = base / name;
    fs::create_directories(full.parent_path());
    std::ofstream(full) << contents;
}

void remove_file(const fs::path& base, const std::string& name) {
    REQUIRE(fs::remove(base / name));
}

} // anonymous namespace

TEST_CASE("scan_repo_history finds committed env files and skips the rest") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    // Files that should be flagged...
    write_file(dir, ".env", "API_KEY=secret\n");
    write_file(dir, "settings.json", "{}\n");

    // ...and files that must be skipped (excluded dirs / safe extensions)
    write_file(dir, "node_modules/pkg/secrets.env", "x\n");
    write_file(dir, "build/appsettings.env", "x\n");
    write_file(dir, "config.example", "x\n");
    write_file(dir, "config.test.js", "// unit tests\n");
    write_file(dir, "settings.spec.ts", "// unit tests\n");

    make_commit(repo, "test@example.com", "initial commit");
    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 100, 0);
    const auto& map = found.file_to_commit;

    REQUIRE(map.count(".env") == 1);
    REQUIRE(map.count("settings.json") == 1);
    REQUIRE(map.count("node_modules/pkg/secrets.env") == 0);
    REQUIRE(map.count("build/appsettings.env") == 0);
    REQUIRE(map.count("config.example") == 0);
    REQUIRE(map.count("config.test.js") == 0);
    REQUIRE(map.count("settings.spec.ts") == 0);

    // reported hashes must be valid git object ids
    REQUIRE(utils::is_valid_commit_hash(map.at("settings.json")));

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("scan_repo_history ignores build, media, and doc files that cannot be secrets") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    write_file(dir, "app.config.min.js", "// bundled\n");
    write_file(dir, "config.d.ts", "x\n");
    write_file(dir, "settings.lock", "x\n");
    write_file(dir, "config.png", "x\n");
    write_file(dir, "settings.svg", "x\n");
    write_file(dir, "config.pdf", "x\n");
    write_file(dir, "secrets.md", "x\n");

    make_commit(repo, "test@example.com", "initial commit");
    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 0, 0);
    const auto& map = found.file_to_commit;

    REQUIRE(map.empty());

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("scan_repo_history handles empty repositories gracefully") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);
    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 0, 0);
    REQUIRE(found.file_to_commit.empty());

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("deleted env files point at the newest commit that contained them") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    write_file(dir, ".env", "SECRET=old\n");
    write_file(dir, "app.config.yml", "x\n");
    std::string c1 = make_commit(repo, "test@example.com", "add env files");

    remove_file(dir, ".env");
    write_file(dir, "docs/readme.txt", "hello\n");
    std::string c2 = make_commit(repo, "test@example.com", "drop the env file");

    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 0, 0);
    const auto& map = found.file_to_commit;

    // .env was deleted in c2, so its newest existing commit is its parent (c1).
    REQUIRE(map.at(".env") == c1);
    // app.config.yml still exists at the tip (c2).
    REQUIRE(map.at("app.config.yml") == c2);

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("a file still present at the tip is attributed to the tip commit") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    write_file(dir, ".env", "SECRET=old\n");
    std::string c1 = make_commit(repo, "test@example.com", "first");
    write_file(dir, "server.env", "PORT=8080\n");
    std::string c2 = make_commit(repo, "test@example.com", "second");
    write_file(dir, ".env", "SECRET=new\n"); // modified, still present
    std::string c3 = make_commit(repo, "test@example.com", "third");

    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 0, 0);
    const auto& map = found.file_to_commit;

    REQUIRE(map.at(".env") == c3);
    REQUIRE(map.at("server.env") == c3);

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("early_exit stops after the configured number of quiet commits") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    write_file(dir, "docs/readme.txt", "a\n");
    make_commit(repo, "test@example.com", "quiet 1");
    write_file(dir, "docs/readme.txt", "b\n");
    make_commit(repo, "test@example.com", "quiet 2");
    write_file(dir, "docs/readme.txt", "c\n");
    make_commit(repo, "test@example.com", "quiet 3");
    write_file(dir, ".env", "SECRET=old\n");
    make_commit(repo, "test@example.com", "add env");
    remove_file(dir, ".env");
    write_file(dir, "docs/readme.txt", "d\n");
    make_commit(repo, "test@example.com", "drop env");

    git_repository_free(repo);

    // The deletion is discovered right after the tip (seed). Two consecutive
    // later commits change nothing interesting, so early_exit=2 stops the walk
    // before reaching the oldest quiet commits.
    auto found = scanner::scan_repo_history(dir.string(), 0, 2);
    REQUIRE(found.stopped_early);
    REQUIRE(found.commits_considered < 5); // did not exhaust all commits
    REQUIRE(found.file_to_commit.count(".env") == 1);

    // early_exit=0 never stops early.
    auto full = scanner::scan_repo_history(dir.string(), 0, 0);
    REQUIRE_FALSE(full.stopped_early);
    REQUIRE(full.commits_considered == 5);

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("depth truncation is reported and the walk is bounded") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    write_file(dir, ".env.old", "x\n");
    make_commit(repo, "test@example.com", "c1");
    remove_file(dir, ".env.old");
    make_commit(repo, "test@example.com", "c2");
    write_file(dir, ".env", "y\n");
    make_commit(repo, "test@example.com", "c3");

    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 1, 0);
    REQUIRE(found.truncated_by_depth);
    REQUIRE(found.commits_considered == 1);
    // Only the tip tree was seen, so only the .env still present is found.
    REQUIRE(found.file_to_commit.count(".env") == 1);
    REQUIRE(found.file_to_commit.count(".env.old") == 0);

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("commits with an identical tree are skipped, not diffed") {
    REQUIRE(git_libgit2_init() >= 1);

    git_repository* repo = nullptr;
    fs::path dir = make_temp_repo(&repo);

    write_file(dir, ".env", "x\n");
    make_commit(repo, "test@example.com", "c1");
    make_commit(repo, "test@example.com", "c2 (no changes)"); // identical tree
    make_commit(repo, "test@example.com", "c3 (no changes)"); // identical tree

    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 0, 0);
    REQUIRE(found.commits_skipped >= 1);

    fs::remove_all(dir);
    git_libgit2_shutdown();
}