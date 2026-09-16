#pragma once

#include <string>
#include <vector>
#include <optional>

struct Repository {
    std::string name;
    std::string html_url;
    std::string default_branch;
};

namespace github {

struct UserRepos {
    std::vector<Repository> repos;
    // GitHub rate-limit remaining for the API (from X-RateLimit-Remaining).
    // -1 when unknown (unauthenticated/undocumented responses).
    long rate_limit_remaining = -1;
};

    // Fetch a user's repositories via the GitHub REST API.
    // Uses Link-header pagination (the recommended mechanism).
    // Throws on network errors or a non-2xx/non-404 HTTP response.
    UserRepos get_user_repos(const std::string& username, const std::optional<std::string>& token = std::nullopt, bool include_private = false);
}