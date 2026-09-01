#pragma once

#include <string>
#include <map>

namespace scanner {
    std::map<std::string, std::string> scan_repo_history(const std::string& repo_path, int depth);
}
