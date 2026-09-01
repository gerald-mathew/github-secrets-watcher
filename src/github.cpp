#include "github.hpp"
#include "utils.hpp"
#include <curl/curl.h>
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
    // Callback for libcurl to write data into a std::string
    size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        std::string* buffer = static_cast<std::string*>(userdata);
        size_t total = size * nmemb;
        buffer->append(ptr, total);
        return total;
    }

    std::string http_get(const std::string& url, const std::optional<std::string>& token) {
        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }
        std::string response;

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "github-secrets-watcher/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

        if (token.has_value()) {
            std::string auth = "token " + token.value();
            struct curl_slist* headers = nullptr;
            headers = curl_slist_append(headers, auth.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        // Optional: set timeout
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::string error(curl_easy_strerror(res));
            curl_easy_cleanup(curl);
            throw std::runtime_error("CURL request failed: " + error);
        }

        long http_code = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
        curl_easy_cleanup(curl);

        if (token.has_value()) {
            // Clean up headers list
        }

        if (http_code != 200) {
            throw std::runtime_error("HTTP request failed with code " + std::to_string(http_code));
        }
        return response;
    }

} // anonymous namespace

namespace github {
    std::vector<Repository> get_user_repos(const std::string& username, const std::optional<std::string>& token, bool include_private) {
        std::vector<Repository> repos;
        std::string type = include_private && token.has_value() ? "all" : "public";
        std::string url = "https://api.github.com/users/" + utils::url_encode(username) + "/repos?type=" + type + "&sort=updated&per_page=100";

        int page = 1;
        while (true) {
            std::string page_url = url + "&page=" + std::to_string(page);
            try {
                std::string response = http_get(page_url, token);
                nlohmann::json data = nlohmann::json::parse(response);
                if (!data.is_array()) {
                    throw std::runtime_error("Expected JSON array");
                }
                if (data.empty()) {
                    break; // no more repos
                }
                for (const auto& item : data) {
                    Repository repo;
                    repo.name = item.value("name", "");
                    repo.html_url = item.value("html_url", "");
                    repo.default_branch = item.value("default_branch", "main");
                    if (!repo.name.empty()) {
                        repos.push_back(repo);
                    }
                }
                // GitHub API returns empty array when no more items
                if (data.empty()) {
                    break;
                }
                page++;
            } catch (const std::exception& e) {
                std::cerr << "Error fetching repos page " << page << ": " << e.what() << std::endl;
                break;
            }
        }
        return repos;
    }
} // namespace github
