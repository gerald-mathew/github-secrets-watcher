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

// Stage all workdir changes and create a commit pointing HEAD at it
void make_commit(git_repository* repo, const std::string& email, const std::string& message) {
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

    git_oid commit_oid;
    int err = git_commit_create(&commit_oid, repo, "HEAD", sig, sig, nullptr,
                                message.c_str(), tree, 0, nullptr);
    git_signature_free(sig);
    git_tree_free(tree);
    git_index_free(index);
    if (err < 0) {
        throw std::runtime_error("failed to create commit");
    }
}

} // anonymous namespace

TEST_CASE("scan_repo_history finds committed env files and skips the rest") {
    REQUIRE(git_libgit2_init() >= 1);

    fs::path dir = fs::temp_directory_path() /
                   ("gsw_test_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    git_repository* repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);

    // Files that should be flagged...
    std::ofstream(dir / ".env") << "API_KEY=secret\n";
    std::ofstream(dir / "settings.json") << "{}\n";

    // ...and files that must be skipped (excluded dirs / safe extensions)
    fs::create_directories(dir / "node_modules" / "pkg");
    std::ofstream(dir / "node_modules" / "pkg" / "secrets.env") << "x\n";
    fs::create_directories(dir / "build");
    std::ofstream(dir / "build" / "appsettings.env") << "x\n";
    std::ofstream(dir / "config.example") << "x\n";

    REQUIRE_NOTHROW(make_commit(repo, "test@example.com", "initial commit"));
    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 100);

    REQUIRE(found.count(".env") == 1);
    REQUIRE(found.count("settings.json") == 1);
    REQUIRE(found.count("node_modules/pkg/secrets.env") == 0);
    REQUIRE(found.count("build/appsettings.env") == 0);
    REQUIRE(found.count("config.example") == 0);

    // reported hashes must be valid git object ids
    REQUIRE(utils::is_valid_commit_hash(found["settings.json"]));

    fs::remove_all(dir);
    git_libgit2_shutdown();
}

TEST_CASE("scan_repo_history handles empty repositories gracefully") {
    REQUIRE(git_libgit2_init() >= 1);

    fs::path dir = fs::temp_directory_path() /
                   ("gsw_empty_" + std::to_string(std::chrono::system_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);

    git_repository* repo = nullptr;
    REQUIRE(git_repository_init(&repo, dir.string().c_str(), 0) == 0);
    git_repository_free(repo);

    auto found = scanner::scan_repo_history(dir.string(), 100);
    REQUIRE(found.empty());

    fs::remove_all(dir);
    git_libgit2_shutdown();
}