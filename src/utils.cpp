#include "utils.hpp"
#include <cctype>
#include <sstream>
#include <iomanip>

namespace utils {

    std::string url_encode(const std::string& value) {
        std::ostringstream escaped;
        escaped.fill('0');
        escaped << std::hex;

        for (char c : value) {
            // Keep alphanumeric and other accepted characters
            if (std::isalnum(static_cast<unsigned char>(c)) ||
                c == '-' || c == '_' || c == '.' || c == '~') {
                escaped << c;
            } else {
                escaped << '%' << std::setw(2) << static_cast<int>(static_cast<unsigned char>(c));
            }
        }
        return escaped.str();
    }

    std::string remove_trailing_slash(const std::string& str) {
        if (!str.empty() && (str.back() == '/' || str.back() == '\\')) {
            return str.substr(0, str.size() - 1);
        }
        return str;
    }

    bool is_valid_commit_hash(const std::string& hash) {
        if (hash.length() != 40) return false;
        for (char c : hash) {
            if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
        }
        return true;
    }

    std::vector<std::string> split(const std::string& s, char delimiter) {
        std::vector<std::string> tokens;
        std::string token;
        std::istringstream tokenStream(s);
        while (std::getline(tokenStream, token, delimiter)) {
            tokens.push_back(token);
        }
        return tokens;
    }

    std::string trim(const std::string& str) {
        size_t start = str.find_first_not_of(" \t\n\r");
        size_t end = str.find_last_not_of(" \t\n\r");
        if (start == std::string::npos) return "";
        return str.substr(start, end - start + 1);
    }

    std::string csv_escape(const std::string& value) {
        bool needs_quotes = value.find_first_of(",\"\n\r") != std::string::npos;
        if (!needs_quotes) return value;
        std::string escaped;
        escaped.reserve(value.size() + 2);
        escaped += '"';
        for (char c : value) {
            if (c == '"') escaped += '"';
            escaped += c;
        }
        escaped += '"';
        return escaped;
    }

    std::optional<std::string> extract_next_link(const std::string& link_header) {
        if (link_header.empty()) return std::nullopt;
        // GitHub sends: <https://...>; rel="next", <https://...>; rel="last"
        for (const auto& raw_part : utils::split(link_header, ',')) {
            const std::string part = utils::trim(raw_part);
            size_t angle_open = part.find('<');
            size_t angle_close = part.find('>');
            if (angle_open == std::string::npos || angle_close == std::string::npos ||
                angle_close <= angle_open) {
                continue;
            }
            std::string url = part.substr(angle_open + 1, angle_close - angle_open - 1);
            if (part.find("rel=\"next\"") != std::string::npos ||
                part.find("rel='next'") != std::string::npos ||
                part.find("rel=next") != std::string::npos) {
                return url;
            }
        }
        return std::nullopt;
    }
}