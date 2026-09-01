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
    std::vector<Repository> get_user_repos(const std::string& username, const std::optional<std::string>& token = std::nullopt, bool include_private = false);
}
