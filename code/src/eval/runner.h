/**
 * @file eval/runner.h
 * @brief Public interface of the AI PR Review evaluation harness.
 *
 * The harness loads a JSON-driven case list, invokes the existing
 * `ai-pr-review` binary once per case (black-box), and scores each
 * produced review using three weighted axes:
 *   - keyword hit-rate          (40%)
 *   - severity-label appearance (30%)
 *   - no-hallucination penalty  (30%)
 *
 * Designed for Day-3 evaluation; intentionally avoids in-process LLM
 * calls so the evaluator and the system under test stay decoupled.
 */
#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace aipr::eval {

/// Specification of a single evaluation case, loaded from cases.json.
struct CaseSpec {
    std::string id;
    std::string name;
    std::string pr_url;
    std::string source_dir;  ///< optional; empty disables AST context.
    std::string difficulty;  ///< low / mid / high (reporting only).
    std::vector<std::string> expected_keywords;
    std::vector<std::string> expected_severity_appears;
    std::vector<std::string> must_not_hallucinate;
};

/// Result of running and scoring a single case.
struct CaseResult {
    std::string id;
    bool exec_ok{false};
    std::string exec_error;
    long elapsed_ms{0};

    std::size_t kw_hit{0};
    std::size_t kw_total{0};
    std::size_t sev_hit{0};
    std::size_t sev_total{0};
    std::size_t hallu_hit{0};   ///< count of hallucination keywords seen.
    std::size_t hallu_total{0};

    double score_keyword{0.0};   ///< 0.0 - 1.0
    double score_severity{0.0};  ///< 0.0 - 1.0
    double score_no_hallu{0.0};  ///< 0.0 - 1.0
    double score_total{0.0};     ///< weighted 0.0 - 1.0

    std::string review_excerpt;  ///< first ~400 chars of generated review.
};

/// Top-level runner configuration.
struct RunOptions {
    std::string cases_file;     ///< path to cases.json
    std::string binary;         ///< path to ai-pr-review executable
    std::string out_dir;        ///< per-case output directory
    std::string report_path;    ///< final Markdown report path
    std::string github_token;   ///< forwarded to ai-pr-review
    std::string llm_key;        ///< forwarded to ai-pr-review
    std::string model;          ///< LLM model name
    bool dry_run{false};        ///< skip actual binary invocation
    /// Extra CLI arguments forwarded verbatim to `ai-pr-review`
    /// (e.g. {"--persona", "linus"} / {"--agent"} / {"--rag", "--embed"}).
    /// Lets the harness run a real config matrix instead of baseline only.
    std::vector<std::string> extra_args;
};

/// Parse the JSON file and return a list of cases.
/// Throws std::runtime_error on I/O or schema failure.
std::vector<CaseSpec> load_cases(const std::string& file);

/// Run one case end-to-end and score the resulting review.
/// Never throws on review-scoring failures; sets `exec_ok = false` instead.
CaseResult run_one_case(const CaseSpec& spec, const RunOptions& opts);

/// Emit the final Markdown report. Throws std::runtime_error on I/O failure.
void write_report(const std::vector<CaseResult>& results,
                  const std::vector<CaseSpec>& specs,
                  const RunOptions& opts);

}  // namespace aipr::eval
