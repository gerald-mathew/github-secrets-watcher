#ifndef GITHUB_H
#define GITHUB_H

#include <string>
#include <vector>
#include <optional>

struct Repository {
    std::string name;
    std::string html_url;
    std::string default_branch;
    // private repo flag not needed for public only
};

namespace github {

    // Fetch public repositories for a given username.
    // If token is empty, uses unauthenticated request (rate limited).
    // Returns vector of Repository, or empty if error.
    std::vector<Repository> get_user_repos(const std::string& username, const std::optional<std::string>& token = std::nullopt);

}

#endif // GITHUB_H
