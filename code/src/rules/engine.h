/**
 * @file engine.h
 * @brief Rule-based static analysis engine for PR review.
 *
 * Day-2 update: RuleHit struct aligned with Issue #1 architecture
 * (rule_id / severity / file / line / message / suggestion).
 * New Engine::run() interface accepts ParsedDiff + optional AstSummary.
 */
#pragma once

#include <string>
#include <vector>

// Forward declaration — avoids pulling AST headers into the rules module.
namespace aipr::analyzer { struct AstSummary; }

namespace aipr::rule {

// ---------------------------------------------------------------------------
// Severity levels (aligned with ADR-001 v2)
// ---------------------------------------------------------------------------
enum class Severity {
    Info,    ///< Low-risk or informational finding.
    Warn,    ///< Medium-risk; worth reviewing.
    Error    ///< High-risk; likely bug or security issue.
};

/// Convert Severity to string literal ("info" / "warn" / "error").
const char* severity_str(Severity s) noexcept;

// ---------------------------------------------------------------------------
// RuleHit — a single rule finding
// ---------------------------------------------------------------------------
struct RuleHit {
    std::string rule_id;     ///< Snake_case identifier, e.g. "unpaired_alloc".
    Severity    severity{Severity::Info};
    std::string file;        ///< File path from the diff.
    int         line{0};     ///< 1-based line number (0 = unknown).
    std::string message;     ///< Human-readable description of the finding.
    std::string suggestion;  ///< Suggested fix or action (may be empty).
};

// ---------------------------------------------------------------------------
// FileDiff — per-file diff data consumed by the rule engine
// ---------------------------------------------------------------------------
struct FileDiff {
    std::string              filename;
    std::vector<std::string> added_lines;
    std::vector<int>         added_line_nums;
    std::vector<std::string> removed_lines;
    std::vector<int>         removed_line_nums;
    std::vector<std::string> context_lines;
};

// ---------------------------------------------------------------------------
// ParsedDiff — the full PR diff as seen by the main pipeline
// ---------------------------------------------------------------------------
struct ParsedDiff {
    std::vector<FileDiff> files;
};

// ---------------------------------------------------------------------------
// RuleEngine
// ---------------------------------------------------------------------------
class RuleEngine {
public:
    /**
     * Primary interface (Issue #1 architecture).
     * Runs all rules on the parsed diff, optionally enriched by AST context.
     *
     * @param diff  The full PR diff (one FileDiff per touched file).
     * @param ast   Optional AST summary for deeper analysis. When nullptr,
     *              rules operate on diff-only heuristics.
     * @return Ordered list of findings.
     */
    std::vector<RuleHit> run(const ParsedDiff& diff,
                             const analyzer::AstSummary* ast = nullptr);

    /**
     * Legacy interface — kept for backward compat and unit tests.
     * Delegates to run() internally.
     */
    std::vector<RuleHit> scan(const std::vector<FileDiff>& diffs);

private:
    void check_unpaired_alloc(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_virtual_modify(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_format_only(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_test_file(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_todo_fixme(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_raw_ptr_new(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_c_style_cast(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_memcpy_size(const FileDiff& diff, std::vector<RuleHit>& hits);
};

}  // namespace aipr::rule
