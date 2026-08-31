#ifndef UTILS_H
#define UTILS_H

#include <string>
#include <vector>

namespace utils {

    // URL encode a string (percent-encoding)
    std::string url_encode(const std::string& value);

    // Remove trailing slash from a string if present
    std::string remove_trailing_slash(const std::string& str);

    // Validate that a string is a 40-character lowercase hex string (git commit hash)
    bool is_valid_commit_hash(const std::string& hash);

    // Split a string by delimiter
    std::vector<std::string> split(const std::string& s, char delimiter);

    // Trim whitespace from both ends
    std::string trim(const std::string& str);

}

#endif // UTILS_H
