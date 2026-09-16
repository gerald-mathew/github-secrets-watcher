#pragma once

#include <string>
#include <vector>
#include <optional>

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

    // RFC 4180 CSV field escaping (quotes the field when it contains a comma,
    // quote, newline, or carriage return, doubling any embedded double quotes)
    std::string csv_escape(const std::string& value);

    // Extract the "next" URL from a GitHub Link header value.
    // Returns std::nullopt when there is no rel="next" link or no header.
    std::optional<std::string> extract_next_link(const std::string& link_header);
}