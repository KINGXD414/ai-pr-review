/**
 * @file eval_main.cpp
 * @brief CLI entry point for the AI PR Review evaluation harness
 *        (`ai-pr-eval`). Day-3 deliverable.
 *
 * Reads eval/cases.json, runs ai-pr-review once per case, scores each
 * produced review via aipr::eval::run_one_case and writes a final
 * Markdown report.
 */
#include "common/utils.h"
#include "eval/runner.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::string today_stamp() {
    auto iso = aipr::common::now_iso();  // "YYYY-MM-DD HH:MM:SS"
    // Transform to "YYYY-MM-DD_HHMM"
    std::string stamp = iso;
    if (stamp.size() >= 16) {
        stamp[10] = '_';
        stamp.erase(13, 1);  // remove second ':'
        stamp.resize(16);
    }
    return stamp;
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"AI PR Review evaluation runner"};
    app.set_version_flag("--version", std::string("ai-pr-eval 0.1.0"));

    std::string cases_file = "eval/cases.json";
    std::string binary = "./build/ai-pr-review";
    std::string out_dir;
    std::string report_path;
    std::string model = "deepseek-chat";
    std::string github_token;
    std::string llm_key;
    bool verbose = false;
    bool dry_run = false;

    app.add_option("--cases", cases_file, "Path to cases JSON")
        ->capture_default_str();
    app.add_option("--binary", binary, "Path to ai-pr-review executable")
        ->capture_default_str();
    app.add_option("--out-dir", out_dir,
                   "Per-case review output dir "
                   "(default: eval/runs/<stamp>/)");
    app.add_option("--report", report_path,
                   "Final report path "
                   "(default: eval/run_report_<stamp>.md)");
    app.add_option("--model", model, "LLM model")->capture_default_str();
    app.add_option("--github-token", github_token,
                   "GitHub PAT (fallback env GITHUB_TOKEN)");
    app.add_option("--llm-key", llm_key,
                   "LLM key (fallback env DEEPSEEK_API_KEY)");
    app.add_flag("--dry-run", dry_run,
                 "Do not invoke the binary; verify pipeline only");
    app.add_flag("-v,--verbose", verbose, "Enable debug logs");
    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    if (github_token.empty()) github_token = aipr::common::env_or_empty("GITHUB_TOKEN");
    if (llm_key.empty()) llm_key = aipr::common::env_or_empty("DEEPSEEK_API_KEY");
    if (llm_key.empty() && !dry_run) {
        spdlog::error(
            "LLM key missing: pass --llm-key or set DEEPSEEK_API_KEY "
            "(or use --dry-run for pipeline check)");
        return 2;
    }

    const std::string stamp = today_stamp();
    if (out_dir.empty()) out_dir = "eval/runs/" + stamp;
    if (report_path.empty())
        report_path = "eval/run_report_" + stamp + ".md";

    aipr::eval::RunOptions opts;
    opts.cases_file = cases_file;
    opts.binary = binary;
    opts.out_dir = out_dir;
    opts.report_path = report_path;
    opts.github_token = github_token;
    opts.llm_key = llm_key;
    opts.model = model;
    opts.dry_run = dry_run;

    std::vector<aipr::eval::CaseSpec> specs;
    try {
        specs = aipr::eval::load_cases(cases_file);
    } catch (const std::exception& e) {
        spdlog::error("load_cases failed: {}", e.what());
        return 2;
    }
    spdlog::info("Loaded {} cases from {}", specs.size(), cases_file);
    if (specs.empty()) {
        spdlog::warn("No cases to run -- exiting cleanly.");
        return 0;
    }

    std::vector<aipr::eval::CaseResult> results;
    results.reserve(specs.size());
    for (const auto& s : specs) {
        results.push_back(aipr::eval::run_one_case(s, opts));
    }

    try {
        aipr::eval::write_report(results, specs, opts);
    } catch (const std::exception& e) {
        spdlog::error("write_report failed: {}", e.what());
        return 3;
    }
    spdlog::info("Report written: {}", report_path);

    std::size_t ok = 0;
    double sum = 0.0;
    for (const auto& r : results) {
        if (r.exec_ok) {
            ++ok;
            sum += r.score_total;
        }
    }
    std::cout << "\n=== EVAL SUMMARY ===\n"
              << "cases ok: " << ok << "/" << results.size() << "\n"
              << "average score: "
              << (ok == 0 ? 0 : static_cast<int>(sum / ok * 100)) << "%\n"
              << "report: " << report_path << "\n";

    return (ok == results.size()) ? 0 : 5;
}
