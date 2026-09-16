#include "github.hpp"
#include "utils.hpp"
#include <curl/curl.h>
#include "json.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <cstdlib>

namespace {

    // RAII guard that initializes libcurl once (thread-safe via function-local
    // static in C++11+) and cleans up at process exit.
    struct CurlGlobal {
        CurlGlobal() {
            if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
                throw std::runtime_error("Failed to initialize libcurl");
            }
        }
        ~CurlGlobal() {
            curl_global_cleanup();
        }
    };

    // Callback for libcurl to write data into a std::string
    size_t write_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        std::string* buffer = static_cast<std::string*>(userdata);
        size_t total = size * nmemb;
        buffer->append(ptr, total);
        return total;
    }

    struct header_collector {
        std::string rate_limit_remaining;
        std::string rate_limit_reset;
        std::string link;
    };

    // Callback for libcurl to collect response headers
    size_t header_callback(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* headers = static_cast<header_collector*>(userdata);
        size_t total = size * nmemb;
        std::string line(ptr, total);
        size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = utils::trim(line.substr(0, colon));
            std::string value = utils::trim(line.substr(colon + 1));
            if (key == "X-RateLimit-Remaining") headers->rate_limit_remaining = value;
            else if (key == "X-RateLimit-Reset") headers->rate_limit_reset = value;
            else if (key == "Link") headers->link += value;
        }
        return total;
    }

    struct HttpResponse {
        std::string body;
        long status_code = 0;
        header_collector headers;
    };

    // Perform a GET request; returns status + body + selected headers.
    // Does not validate the status code - the caller decides what to do with it.
    HttpResponse http_get(const std::string& url, const std::optional<std::string>& token) {
        CurlGlobal guard; // ensures curl is globally initialized once

        CURL* curl = curl_easy_init();
        if (!curl) {
            throw std::runtime_error("Failed to initialize CURL");
        }

        HttpResponse result;
        struct curl_slist* headers = nullptr;
        if (token.has_value()) {
            std::string auth = "Authorization: token " + token.value();
            headers = curl_slist_append(headers, auth.c_str());
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &result.body);
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_callback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &result.headers);
        curl_easy_setopt(curl, CURLOPT_USERAGENT, "github-secrets-watcher/1.0");
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

        CURLcode res = curl_easy_perform(curl);
        if (res != CURLE_OK) {
            std::string error(curl_easy_strerror(res));
            curl_slist_free_all(headers); // free header list even on error
            curl_easy_cleanup(curl);
            throw std::runtime_error("CURL request failed: " + error);
        }

        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.status_code);
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);

        return result;
    }

} // anonymous namespace

namespace github {
    UserRepos get_user_repos(const std::string& username, const std::optional<std::string>& token, bool include_private) {
        UserRepos result;
        std::string type = include_private && token.has_value() ? "all" : "public";
        std::string first_url = "https://api.github.com/users/" + utils::url_encode(username) +
                                "/repos?type=" + type + "&sort=updated&per_page=100";

        std::string url = first_url;
        while (!url.empty()) {
            HttpResponse response = http_get(url, token);

            // Remember rate-limit from the most recent response.
            if (!response.headers.rate_limit_remaining.empty()) {
                try {
                    result.rate_limit_remaining = std::stol(response.headers.rate_limit_remaining);
                } catch (...) {
                    result.rate_limit_remaining = -1;
                }
            }

            if (response.status_code == 404) {
                throw std::runtime_error("GitHub user not found: " + username);
            }
            if (response.status_code != 200) {
                // Report what the API actually told us. GitHub answers a 403
                // both when the rate limit ran out and when the token is
                // missing/expired/lacks scope, so reuse the response body's
                // `message` and the rate-limit headers to pick the right cause.
                std::string api_message;
                try {
                    nlohmann::json err = nlohmann::json::parse(response.body);
                    api_message = err.value("message", "");
                } catch (...) {
                    api_message.clear();
                }

                const bool rate_limited =
                    response.headers.rate_limit_remaining == "0" ||
                    api_message.find("rate limit") != std::string::npos;
                const bool auth_issue =
                    api_message.find("Bad credentials") != std::string::npos ||
                    api_message.find("Requires authentication") != std::string::npos ||
                    api_message.find("personal access token") != std::string::npos ||
                    api_message.find("Repository access blocked") != std::string::npos ||
                    api_message.find("access denied") != std::string::npos;

                std::ostringstream msg;
                msg << "GitHub API returned HTTP " << response.status_code;
                if (!api_message.empty()) {
                    msg << " - " << api_message;
                }
                if (rate_limited && !response.headers.rate_limit_reset.empty()) {
                    msg << " (rate limit reset at " << response.headers.rate_limit_reset << ")";
                }
                if (auth_issue) {
                    msg << ". Provide a valid --token with the repo scope to authenticate"
                        << " when scanning private/unlisted repositories.";
                }
                throw std::runtime_error(msg.str());
            }

            nlohmann::json data = nlohmann::json::parse(response.body);
            if (data.is_array() && !data.empty()) {
                for (const auto& item : data) {
                    Repository repo;
                    repo.name = item.value("name", "");
                    repo.html_url = item.value("html_url", "");
                    repo.default_branch = item.value("default_branch", "main");
                    if (!repo.name.empty()) {
                        result.repos.push_back(repo);
                    }
                }
            }

            // Follow the Link header 'rel="next"' URL when present (GitHub's
            // recommended pagination mechanism); otherwise stop.
            url = utils::extract_next_link(response.headers.link).value_or("");
        }
        return result;
    }
} // namespace github