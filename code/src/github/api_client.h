/**
 * @file api_client.h
 * @brief Minimal GitHub REST API client for PR review use cases.
 *
 * We only need two endpoints to drive Day-1 review:
 *   - `GET /repos/{owner}/{repo}/pulls/{n}`        — PR metadata
 *   - `GET /repos/{owner}/{repo}/pulls/{n}/files`  — per-file diff (`patch`)
 *
 * Auth is via a personal access token passed as `Authorization: Bearer …`.
 * The token is loaded from CLI flags or environment by main(); this header
 * intentionally has no notion of where it came from to keep credentials out
 * of any compiled-in path.
 *
 * Reference: design document §3 and §4.
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace aipr::github {

/// Result of parsing a `https://github.com/{owner}/{repo}/pull/{n}` URL.
struct PRLocator {
    std::string owner;
    std::string repo;
    int         number{0};
};

/// One file as returned from `/pulls/{n}/files`.  We only keep what the
/// reviewer pipeline needs — additions/deletions for the summary, status
/// to differentiate "modified" vs "added" vs "renamed", patch for the diff
/// parser.
struct PRFile {
    std::string filename;
    std::string status;        ///< "added", "removed", "modified", "renamed", …
    std::string patch;         ///< Empty for binary files / pure renames.
    int         additions{0};
    int         deletions{0};
    int         changes{0};
};

/// Top-level PR view used downstream.
struct PullRequest {
    PRLocator           locator;
    std::string         title;
    std::string         body;          ///< Body may be empty.
    std::string         html_url;
    std::string         diff_url;
    std::string         author;
    std::string         base_ref;
    std::string         head_ref;
    std::vector<PRFile> files;
};

/// Parse a GitHub PR URL.  Accepts plain `https://github.com/o/r/pull/123`
/// and tolerates a trailing slash or extra path segments
/// (`.../pull/123/files`).  Returns nullopt on any other shape.
std::optional<PRLocator> parse_pr_url(const std::string& url);

/// Thin synchronous client.  Each call is a fresh HTTPS request — Day 1
/// review tools fire a handful of requests so we don't bother with a
/// connection pool yet (design doc §4.5 has that as a Day 3 nice-to-have).
class APIClient {
public:
    /**
     * @param token  GitHub Personal Access Token.  May be empty for
     *               public-repo read-only access — GitHub will throttle
     *               us at 60 req/h instead of the authenticated 5000/h.
     * @param user_agent  Sent as the User-Agent header.  GitHub *requires*
     *                    a non-empty UA on every request.
     */
    APIClient(std::string token, std::string user_agent = "ai-pr-review/0.1");

    /// Fetch PR metadata + file list.  Returns nullopt on any HTTP or JSON
    /// failure (the implementation logs the cause to spdlog).
    std::optional<PullRequest> fetch_pull_request(const PRLocator& loc);

private:
    std::string token_;
    std::string user_agent_;

    /// Helper that performs a GET against api.github.com and returns the
    /// raw body on HTTP 200.  Any other status returns nullopt.
    std::optional<std::string> get(const std::string& path);
};

}  // namespace aipr::github
