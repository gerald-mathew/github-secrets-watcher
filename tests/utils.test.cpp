#include <catch2/catch_test_macros.hpp>
#include <string>
#include "utils.hpp"

TEST_CASE("url_encode keeps safe characters") {
    REQUIRE(utils::url_encode("abc-._~") == "abc-._~");
}

TEST_CASE("url_encode percent-encodes everything else") {
    REQUIRE(utils::url_encode("/") == "%2f");
    REQUIRE(utils::url_encode(" ") == "%20");
    REQUIRE(utils::url_encode("a/b c") == "a%2fb%20c");
}

TEST_CASE("remove_trailing_slash") {
    REQUIRE(utils::remove_trailing_slash("https://github.com/a/b") == "https://github.com/a/b");
    REQUIRE(utils::remove_trailing_slash("https://github.com/a/b/") == "https://github.com/a/b");
    REQUIRE(utils::remove_trailing_slash("") == "");
}

TEST_CASE("is_valid_commit_hash") {
    REQUIRE(utils::is_valid_commit_hash("0123456789abcdef0123456789abcdef01234567"));
    REQUIRE_FALSE(utils::is_valid_commit_hash("short"));
    REQUIRE_FALSE(utils::is_valid_commit_hash("G123456789abcdef0123456789abcdef01234567"));
    REQUIRE_FALSE(utils::is_valid_commit_hash(""));
}

TEST_CASE("split") {
    auto parts = utils::split("a,b,c", ',');
    REQUIRE(parts.size() == 3);
    REQUIRE(parts[0] == "a");
    REQUIRE(parts[2] == "c");
}

TEST_CASE("trim") {
    REQUIRE(utils::trim("  hello  ") == "hello");
    REQUIRE(utils::trim("\t\r\n hello \n") == "hello");
    REQUIRE(utils::trim("   ") == "");
}

TEST_CASE("csv_escape leaves plain fields alone") {
    REQUIRE(utils::csv_escape("plain") == "plain");
    REQUIRE(utils::csv_escape("") == "");
}

TEST_CASE("csv_escape quotes special fields (RFC 4180)") {
    REQUIRE(utils::csv_escape("a,b") == "\"a,b\"");
    REQUIRE(utils::csv_escape("say \"hi\"") == "\"say \"\"hi\"\"\"");
    REQUIRE(utils::csv_escape("line\nbreak") == "\"line\nbreak\"");
}

TEST_CASE("extract_next_link from a GitHub Link header") {
    std::string header = "<https://api.github.com/user/repos?page=2>; rel=\"next\", "
                         "<https://api.github.com/user/repos?page=4>; rel=\"last\"";
    auto next = utils::extract_next_link(header);
    REQUIRE(next.has_value());
    REQUIRE(next.value() == "https://api.github.com/user/repos?page=2");
}

TEST_CASE("extract_next_link with no next page") {
    REQUIRE_FALSE(utils::extract_next_link("<https://api.github.com/user/repos?page=1>; rel=\"first\""));
    REQUIRE_FALSE(utils::extract_next_link(""));
}