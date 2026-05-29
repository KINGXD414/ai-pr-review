/**
 * @file parser.h
 * @brief Lightweight unified-diff parser for GitHub PR `patch` payloads.
 *
 * GitHub's REST API returns the diff of each file inside a PR through the
 * `patch` field of `/repos/{owner}/{repo}/pulls/{n}/files`.  The payload is
 * a *unified* diff fragment such as:
 *
 *     @@ -10,7 +10,8 @@ void foo()
 *      int a = 0;
 *     -int b = 1;
 *     +int b = 2;
 *     +int c = 3;
 *
 * We do NOT use libgit2 here — that library wants a full clone to do its job.
 * For PR review we only ever see the post-truncation diff so a 200-LOC
 * single-purpose parser is plenty.  The implementation lives in parser.cpp.
 *
 * Reference: design document §4.1.
 */
#pragma once

#include <string>
#include <vector>

namespace aipr::diff {

/// One line of a hunk.  We keep the leading character stripped so consumers
/// can render the line however they want.
struct DiffLine {
    enum class Type {
        Add,      ///< Line was added (`+`).
        Del,      ///< Line was removed (`-`).
        Context,  ///< Context line (` `).
    };

    Type        type{Type::Context};
    std::string content;  ///< Line body without the leading +/-/space.
};

/// A single `@@` hunk inside a diff.
///
/// We deliberately do not carry the surrounding file paths here — those come
/// from the enclosing `PRFile` record and would be redundant per-hunk.
struct DiffHunk {
    int                   old_start{0};   ///< 1-based old-file start line.
    int                   old_count{0};   ///< Number of lines in the old chunk.
    int                   new_start{0};   ///< 1-based new-file start line.
    int                   new_count{0};   ///< Number of lines in the new chunk.
    std::string           header;         ///< Trailing text after the second `@@`.
    std::vector<DiffLine> lines;
};

/**
 * Parse a GitHub `patch` blob into hunks.
 *
 * @param patch  The raw `patch` string returned by GitHub.  Empty input
 *               returns an empty vector — that is *not* an error (binary
 *               files and renames-without-content-changes legitimately
 *               have an empty patch).
 * @return The list of hunks in source order.  Malformed `@@` headers cause
 *         that single hunk to be skipped while still parsing the rest of
 *         the patch — we never throw.
 */
std::vector<DiffHunk> parse_patch(const std::string& patch);

}  // namespace aipr::diff
