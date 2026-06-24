/**
 * @file api_client.cpp
 * @brief Implementation of the lightweight GitHub REST client.
 */
#include "github/api_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <regex>
#include <thread>
#include <utility>

namespace aipr::github {

namespace {
constexpr const char* kHost = "api.github.com";
}  // namespace

std::optional<PRLocator> parse_pr_url(const std::string& url) {
    // Pattern: github.com/<owner>/<repo>/pull/<number>(/anything)?
    // Owner/repo allow alphanumerics, dot, underscore, hyphen.  Number must
    // be a positive integer.  https:// or http:// or no scheme all accepted.
    static const std::regex re(
        R"(^(?:https?://)?(?:www\.)?github\.com/([A-Za-z0-9._-]+)/([A-Za-z0-9._-]+)/pull/(\d+)(?:[/?#].*)?$)");
    std::smatch m;
    if (!std::regex_match(url, m, re)) {
        return std::nullopt;
    }
    PRLocator loc;
    loc.owner  = m[1].str();
    loc.repo   = m[2].str();
    try {
        loc.number = std::stoi(m[3].str());
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (loc.number <= 0) return std::nullopt;
    return loc;
}

APIClient::APIClient(std::string token, std::string user_agent)
    : token_(std::move(token)), user_agent_(std::move(user_agent)) {}

std::optional<std::string> APIClient::get(const std::string& path) {
    httplib::Headers headers = {
        {"User-Agent",           user_agent_},
        {"Accept",               "application/vnd.github+json"},
        {"X-GitHub-Api-Version", "2022-11-28"},
    };
    if (!token_.empty()) {
        headers.emplace("Authorization", "Bearer " + token_);
    }

    constexpr int kMaxRetries = 3;
    for (int attempt = 1; attempt <= kMaxRetries; ++attempt) {
        httplib::SSLClient cli(kHost, 443);
        cli.set_connection_timeout(10);
        cli.set_read_timeout(30);
        cli.set_follow_location(true);

        auto res = cli.Get(path.c_str(), headers);

        if (!res) {
            auto err = httplib::to_string(res.error());
            spdlog::warn("GitHub GET {} attempt {}/{} failed: transport error ({})",
                         path, attempt, kMaxRetries, err);
            if (attempt < kMaxRetries) {
                std::this_thread::sleep_for(std::chrono::seconds(attempt));
            }
            continue;
        }

        // 5xx: server error — retryable
        if (res->status >= 500 && res->status < 600) {
            spdlog::warn("GitHub GET {} attempt {}/{} got HTTP {} — retrying",
                         path, attempt, kMaxRetries, res->status);
            if (attempt < kMaxRetries) {
                std::this_thread::sleep_for(std::chrono::seconds(attempt * 2));
            }
            continue;
        }

        // 403 with rate-limit header: retry after specified delay
        if (res->status == 403) {
            auto it = res->headers.find("Retry-After");
            if (it != res->headers.end()) {
                spdlog::warn("GitHub rate-limited on {}. Retry-After: {}s",
                             path, it->second);
                // Don't retry on rate-limit — we'd just burn more quota.
            }
        }

        if (res->status != 200) {
            // 4xx (except 403 w/ Retry-After) — permanent error, do not retry.
            spdlog::error("GitHub GET {} returned HTTP {}: {}",
                          path, res->status, res->body.substr(0, 200));
            return std::nullopt;
        }

        return res->body;
    }

    spdlog::error("GitHub GET {} exhausted {} retries", path, kMaxRetries);
    return std::nullopt;
}

std::optional<PullRequest> APIClient::fetch_pull_request(const PRLocator& loc) {
    using nlohmann::json;

    // ---- 1. PR metadata ---------------------------------------------------
    const std::string meta_path =
        "/repos/" + loc.owner + "/" + loc.repo + "/pulls/" +
        std::to_string(loc.number);
    auto meta_body = get(meta_path);
    if (!meta_body) {
        return std::nullopt;
    }

    PullRequest pr;
    pr.locator = loc;
    try {
        const auto j = json::parse(*meta_body);
        pr.title    = j.value("title", "");
        pr.body     = j.value("body", "");
        pr.html_url = j.value("html_url", "");
        pr.diff_url = j.value("diff_url", "");
        if (j.contains("user") && j["user"].is_object()) {
            pr.author = j["user"].value("login", "");
        }
        if (j.contains("base") && j["base"].is_object()) {
            pr.base_ref = j["base"].value("ref", "");
        }
        if (j.contains("head") && j["head"].is_object()) {
            pr.head_ref = j["head"].value("ref", "");
        }
    } catch (const json::exception& e) {
        spdlog::error("GitHub PR metadata JSON parse error: {}", e.what());
        return std::nullopt;
    }

    // ---- 2. PR files (with patches) --------------------------------------
    // GitHub paginates this endpoint at 30 items/page (max 100).  For Day 1
    // we cap at one page of 100 — large PRs will be addressed on Day 3 when
    // the chunking/budgeting work lands.
    const std::string files_path = meta_path + "/files?per_page=100";
    auto files_body = get(files_path);
    if (!files_body) {
        // We can still return a partial PullRequest with metadata-only;
        // downstream layers will treat an empty file list as a soft error.
        spdlog::warn("Returning PR metadata without file list (file fetch failed)");
        return pr;
    }
    try {
        const auto j = json::parse(*files_body);
        if (!j.is_array()) {
            spdlog::error("GitHub /files response was not a JSON array");
            return pr;
        }
        pr.files.reserve(j.size());
        for (const auto& item : j) {
            PRFile f;
            f.filename  = item.value("filename", "");
            f.status    = item.value("status", "");
            f.patch     = item.value("patch", "");
            f.additions = item.value("additions", 0);
            f.deletions = item.value("deletions", 0);
            f.changes   = item.value("changes", 0);
            pr.files.push_back(std::move(f));
        }
    } catch (const json::exception& e) {
        spdlog::error("GitHub PR files JSON parse error: {}", e.what());
        return pr;  // best-effort: hand back what we have
    }

    spdlog::info("Fetched PR #{} ({} files)", loc.number, pr.files.size());
    return pr;
}

}  // namespace aipr::github
