/**
 * @file parser.cpp
 * @brief Implementation of the unified-diff parser declared in parser.h.
 */
#include "diff/parser.h"

#include <cctype>
#include <sstream>
#include <string>

namespace aipr::diff {

namespace {

/// Parse the "@@ -a,b +c,d @@ trailing" header line.  Returns false on any
/// shape we don't recognise — the caller will skip the hunk.
bool parse_hunk_header(const std::string& line, DiffHunk& out) {
    // Minimum sensible header: "@@ -1 +1 @@" — 11 chars.
    if (line.size() < 11 || line.rfind("@@", 0) != 0) {
        return false;
    }

    // Find the closing "@@" so we can isolate the trailer.
    const auto closing = line.find("@@", 2);
    if (closing == std::string::npos) {
        return false;
    }

    // Body is the text between the two `@@` markers.
    std::string body = line.substr(2, closing - 2);
    // Trim leading and trailing spaces.
    auto ltrim = body.find_first_not_of(' ');
    auto rtrim = body.find_last_not_of(' ');
    if (ltrim == std::string::npos) {
        return false;
    }
    body = body.substr(ltrim, rtrim - ltrim + 1);

    // Body should look like "-a,b +c,d" (counts are optional, default 1).
    std::istringstream iss(body);
    std::string old_part;
    std::string new_part;
    iss >> old_part >> new_part;
    if (old_part.empty() || new_part.empty() ||
        old_part[0] != '-' || new_part[0] != '+') {
        return false;
    }

    auto parse_pair = [](const std::string& token, int& start, int& count) {
        // Token looks like "-12,7" or just "-12".
        const std::string num = token.substr(1);
        const auto        comma = num.find(',');
        try {
            if (comma == std::string::npos) {
                start = std::stoi(num);
                count = 1;
            } else {
                start = std::stoi(num.substr(0, comma));
                count = std::stoi(num.substr(comma + 1));
            }
        } catch (const std::exception&) {
            return false;
        }
        return true;
    };

    if (!parse_pair(old_part, out.old_start, out.old_count)) return false;
    if (!parse_pair(new_part, out.new_start, out.new_count)) return false;

    // Trailing context (function name etc.) lives after the closing `@@`.
    if (closing + 2 < line.size()) {
        std::string trailer = line.substr(closing + 2);
        if (!trailer.empty() && trailer.front() == ' ') trailer.erase(0, 1);
        out.header = std::move(trailer);
    }
    return true;
}

}  // namespace

std::vector<DiffHunk> parse_patch(const std::string& patch) {
    std::vector<DiffHunk> hunks;
    if (patch.empty()) {
        return hunks;
    }

    std::istringstream iss(patch);
    std::string        line;
    DiffHunk*          current = nullptr;
    bool               skip_current = false;  // header malformed — eat lines until next @@

    while (std::getline(iss, line)) {
        // Normalise potential trailing \r from CRLF input.
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }

        if (line.rfind("@@", 0) == 0) {
            DiffHunk hunk;
            if (!parse_hunk_header(line, hunk)) {
                // Skip lines until the next valid hunk header.
                skip_current = true;
                current      = nullptr;
                continue;
            }
            hunks.push_back(std::move(hunk));
            current      = &hunks.back();
            skip_current = false;
            continue;
        }

        if (skip_current || current == nullptr) {
            // Either we're before the first hunk (e.g. ignoring `diff --git`
            // preambles that GitHub sometimes strips, sometimes not) or the
            // previous hunk header was malformed.
            continue;
        }

        // GitHub's patch field never includes the leading `diff --git`
        // header, but it does include `\ No newline at end of file`
        // sentinels — silently drop them.
        if (line.rfind("\\ ", 0) == 0) {
            continue;
        }

        if (line.empty()) {
            // A genuinely empty line in a diff is a context line whose
            // body is empty.  Some producers emit it without the leading
            // space — accept it as a context line.
            current->lines.push_back({DiffLine::Type::Context, ""});
            continue;
        }

        switch (line[0]) {
            case '+':
                current->lines.push_back({DiffLine::Type::Add, line.substr(1)});
                break;
            case '-':
                current->lines.push_back({DiffLine::Type::Del, line.substr(1)});
                break;
            case ' ':
                current->lines.push_back({DiffLine::Type::Context, line.substr(1)});
                break;
            default:
                // Unknown leading char — treat as context to keep parsing
                // forgiving.  Real-world diffs shouldn't reach here.
                current->lines.push_back({DiffLine::Type::Context, line});
                break;
        }
    }
    return hunks;
}

}  // namespace aipr::diff
