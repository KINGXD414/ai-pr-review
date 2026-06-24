/**
 * @file main.cpp
 * @brief CLI entry point for the AI PR Review assistant.
 *
 * Pipeline:
 *   parse args -> resolve credentials -> parse PR URL ->
 *   GitHub API fetch -> rule engine pre-scan -> optional AST analysis ->
 *   assemble prompt -> run LLM pipeline (persona / consensus / self-critique) ->
 *   optional KODO upload -> write Markdown report.
 *
 * v0.2.1 (P0+P1 refactoring):
 *   - Rule engine hits injected into LLM prompt (three-layer synergy).
 *   - Consensus runs are now parallelized via std::async.
 *   - Prompt builder & report renderer extracted to pipeline/ module.
 */
#include "common/utils.h"
#include "analyzer/ast_analyzer.h"
#include "diff/parser.h"
#include "gateway/llm_gateway.h"
#include "github/api_client.h"
#include "pipeline/prompt_builder.h"
#include "pipeline/report_renderer.h"
#include "prompt/personas.h"
#include "prompt/consensus.h"
#include "prompt/self_critique.h"
#include "rules/engine.h"
#include "storage/kodo_client.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

/// Convert GitHub PRFile patches + diff parser -> rule engine hits.
std::vector<aipr::rule::RuleHit> run_rule_engine(
    const std::vector<aipr::github::PRFile>& files) {
    aipr::rule::RuleEngine engine;
    aipr::rule::ParsedDiff pd;

    for (const auto& f : files) {
        if (f.patch.empty()) continue;

        auto hunks = aipr::diff::parse_patch(f.patch);
        if (hunks.empty()) continue;

        aipr::rule::FileDiff fd;
        fd.filename = f.filename;

        for (const auto& hunk : hunks) {
            int new_line = hunk.new_start;
            int old_line = hunk.old_start;
            for (const auto& dl : hunk.lines) {
                switch (dl.type) {
                case aipr::diff::DiffLine::Type::Add:
                    fd.added_lines.push_back(dl.content);
                    fd.added_line_nums.push_back(new_line++);
                    break;
                case aipr::diff::DiffLine::Type::Del:
                    fd.removed_lines.push_back(dl.content);
                    fd.removed_line_nums.push_back(old_line++);
                    break;
                case aipr::diff::DiffLine::Type::Context:
                    ++new_line;
                    ++old_line;
                    break;
                }
            }
        }
        pd.files.push_back(std::move(fd));
    }

    return engine.run(pd, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
    CLI::App app{"AI-powered Pull Request review assistant"};
    app.set_version_flag("--version", std::string("ai-pr-review 0.2.1"));

    std::string pr_url;
    std::string github_token;
    std::string llm_key;
    std::string output_path  = "review.md";
    std::string model        = "deepseek-chat";
    std::string llm_host     = "api.deepseek.com";
    std::string llm_path     = "/v1/chat/completions";
    std::string user_agent   = "ai-pr-review/0.2";
    std::string source_dir;
    std::string persona      = "default";
    int timeout_s   = 60;
    int retries     = 3;
    bool verbose    = false;
    int consensus_n = 1;
    bool self_critique = false;
    bool upload_kodo = false;
    std::string kodo_prefix = "reviews";
    std::string kodo_viewer_base;  // deprecated
    std::string cache_dir;  // P1-2: directory for LLM response cache

    app.add_option("--pr", pr_url,
        "GitHub PR URL, e.g. https://github.com/owner/repo/pull/123")
        ->required();
    app.add_option("--github-token", github_token,
        "GitHub PAT (fallback: env GITHUB_TOKEN; empty = anonymous)");
    app.add_option("--llm-key", llm_key,
        "DeepSeek API key (fallback: env DEEPSEEK_API_KEY)");
    app.add_option("--output,-o", output_path,
        "Path of the Markdown report (default: review.md)");
    app.add_option("--model", model,
        "LLM model (default: deepseek-chat)");
    app.add_option("--llm-host", llm_host,
        "LLM host (default: api.deepseek.com)");
    app.add_option("--llm-path", llm_path,
        "LLM endpoint path (default: /v1/chat/completions)");
    app.add_option("--user-agent", user_agent,
        "User-Agent for GitHub requests (GitHub requires this)");
    app.add_option("--timeout", timeout_s,
        "Per-attempt timeout in seconds");
    app.add_option("--retries", retries,
        "Max retries on LLM transport errors");
    app.add_option("--source-dir", source_dir,
        "Local repo root for libclang AST analysis (optional, enables AST context)");
    app.add_option("--persona", persona,
        "Reviewer persona: default | linus | mentor | devil | zen");
    app.add_option("--consensus", consensus_n,
        "Run N independent reviews (N in [1,5]) and consolidate by majority vote (default 1)")
        ->check(CLI::Range(1, 5));
    app.add_flag("--self-critique", self_critique,
        "Enable two-pass review: generate -> self-audit -> refined report");
    app.add_flag("--upload-kodo", upload_kodo,
        "Upload the rendered report to Qiniu KODO (needs env QINIU_AK/SK/BUCKET)");
    app.add_option("--kodo-prefix", kodo_prefix,
        "Key prefix used inside the KODO bucket (default: reviews)");
    app.add_option("--kodo-viewer", kodo_viewer_base,
        "Deprecated: KODO links are now embedded directly in the report footer");
    app.add_flag("-v,--verbose", verbose, "Enable debug logs");
    app.add_option("--cache-dir", cache_dir,
        "Directory for LLM response cache (empty = disabled, e.g. ~/.cache/ai-pr-review/)");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    // --- Resolve credentials ---
    if (github_token.empty()) github_token = aipr::common::env_or_empty("GITHUB_TOKEN");
    if (llm_key.empty())      llm_key      = aipr::common::env_or_empty("DEEPSEEK_API_KEY");
    if (llm_key.empty()) {
        spdlog::error("LLM key missing: pass --llm-key or set DEEPSEEK_API_KEY");
        return 2;
    }
    if (github_token.empty()) {
        spdlog::warn("GitHub token empty -- using anonymous access (60 req/h).");
    }

    // Validate persona
    if (persona != "default" && persona != "linus" && persona != "mentor"
        && persona != "devil" && persona != "zen") {
        spdlog::error("Unknown persona '{}'. Choose: default | linus | mentor | devil | zen", persona);
        return 2;
    }

    // --- Parse PR URL & fetch from GitHub ---
    auto loc = aipr::github::parse_pr_url(pr_url);
    if (!loc) {
        spdlog::error("Invalid PR URL: {}", pr_url);
        return 2;
    }
    spdlog::info("PR target: {}/{} #{}", loc->owner, loc->repo, loc->number);

    aipr::github::APIClient gh(github_token, user_agent);
    spdlog::info("Fetching PR metadata + files from GitHub ...");
    auto pr = gh.fetch_pull_request(*loc);
    if (!pr) {
        spdlog::error("Failed to fetch PR -- see previous logs.");
        return 3;
    }
    spdlog::info("PR fetched: \"{}\" by {} | {} files",
                 pr->title, pr->author, pr->files.size());

    if (pr->files.empty()) {
        spdlog::warn("PR has zero changed files -- nothing to review.");
        return 0;
    }

    // ===== P0-1: Rule engine pre-scan (three-layer synergy) =====
    auto rule_hits = run_rule_engine(pr->files);
    spdlog::info("Rule engine: {} hit(s) across {} file(s)",
                 rule_hits.size(), pr->files.size());
    for (const auto& h : rule_hits) {
        spdlog::debug("  [{}] {}:{} — {}", aipr::rule::severity_str(h.severity),
                      h.file, h.line, h.message);
    }

    // --- Optional AST analysis via --source-dir ---
    std::vector<aipr::analyzer::AstSummary> ast_summaries;
    if (!source_dir.empty()) {
        spdlog::info("AST: scanning '{}' for PR source files", source_dir);
        aipr::analyzer::AstAnalyzer analyzer;
        namespace fs = std::filesystem;
        for (const auto& f : pr->files) {
            auto ext = fs::path(f.filename).extension().string();
            if (ext != ".cpp" && ext != ".cc"  && ext != ".cxx" &&
                ext != ".h"   && ext != ".hpp" && ext != ".hxx")
                continue;
            fs::path p = fs::path(source_dir) / f.filename;
            if (!fs::exists(p)) {
                spdlog::warn("AST: not found, skip: {}", p.string());
                continue;
            }
            auto summary = analyzer.analyze_file(p.string());
            if (!summary.ok) {
                spdlog::warn("AST: parse fail {}: {}", p.string(), summary.error);
            }
            ast_summaries.push_back(std::move(summary));
        }
        spdlog::info("AST: analyzed {} file(s) total", ast_summaries.size());
    }

    // --- Build prompts (rule hits now injected into user prompt) ---
    const std::string system_prompt = aipr::pipeline::build_system_prompt(persona);
    const std::string user_prompt   = aipr::pipeline::build_user_prompt(*pr, ast_summaries, rule_hits);
    spdlog::debug("System prompt: {} chars", system_prompt.size());
    spdlog::debug("User prompt: {} chars", user_prompt.size());
    spdlog::info("Persona: {} | Consensus N: {} | Self-Critique: {}",
                 persona, consensus_n, self_critique ? "on" : "off");

    // --- LLM config ---
    aipr::gateway::LLMConfig cfg;
    cfg.host            = llm_host;
    cfg.path            = llm_path;
    cfg.model           = model;
    cfg.api_key         = llm_key;
    cfg.timeout_seconds = timeout_s;
    cfg.max_retries     = retries;
    cfg.temperature     = 0.2;
    cfg.cache_dir       = cache_dir;

    aipr::gateway::LLMGateway gw(cfg);
    spdlog::info("Calling {} on {}{} ...", model, llm_host, llm_path);

    const auto t0 = std::chrono::steady_clock::now();
    aipr::gateway::LLMResponse resp;
    std::vector<std::string> stage1_reports;
    int total_attempts = 0;

    // ===== P1-1: Consensus — parallelized via std::async =====
    if (consensus_n > 1) {
        cfg.temperature = 0.7;  // higher temp so runs differ
        aipr::gateway::LLMGateway gw_parallel(cfg);

        stage1_reports.resize(consensus_n);
        std::vector<std::future<aipr::gateway::LLMResponse>> futures;
        futures.reserve(consensus_n);

        for (int i = 0; i < consensus_n; ++i) {
            futures.push_back(std::async(std::launch::async, [&gw_parallel, &system_prompt, &user_prompt, i, consensus_n]() {
                spdlog::info("  consensus run {}/{} (parallel) ...", i + 1, consensus_n);
                return gw_parallel.chat(system_prompt, user_prompt);
            }));
        }

        bool all_ok = true;
        for (int i = 0; i < consensus_n; ++i) {
            auto r = futures[i].get();
            total_attempts += r.attempts;
            if (!r.ok) {
                spdlog::error("LLM run {}/{} failed after {} attempt(s): {}",
                              i + 1, consensus_n, r.attempts, r.error);
                resp = std::move(r);
                all_ok = false;
                break;
            }
            spdlog::info("  run {}/{} ok, {} chars", i + 1, consensus_n, r.content.size());
            stage1_reports[i] = r.content;
            resp = std::move(r);
        }

        // --- Consensus meta-review ---
        if (all_ok && (int)stage1_reports.size() == consensus_n) {
            spdlog::info("Consensus: consolidating {} runs by majority vote ...", consensus_n);
            aipr::gateway::LLMConfig cfg2 = cfg;
            cfg2.temperature = 0.2;
            aipr::gateway::LLMGateway gw2(cfg2);
            const std::string consensus_sys  = aipr::prompt::consensus_system_prompt(consensus_n);
            const std::string consensus_user = aipr::prompt::build_consensus_user_prompt(stage1_reports, user_prompt);
            auto resp2 = gw2.chat(consensus_sys, consensus_user);
            total_attempts += resp2.attempts;
            if (resp2.ok) {
                spdlog::info("Consensus stage ok after {} attempt(s), {} chars",
                             resp2.attempts, resp2.content.size());
                resp = std::move(resp2);
            } else {
                spdlog::warn("Consensus stage failed: {} -- keeping last run as fallback", resp2.error);
            }
        }
    } else {
        // N=1: single review (original behaviour)
        resp = gw.chat(system_prompt, user_prompt);
        total_attempts = resp.attempts;
        if (resp.ok) {
            spdlog::info("LLM ok after {} attempt(s), {} chars",
                         resp.attempts, resp.content.size());
        } else {
            spdlog::error("LLM failed after {} attempt(s): {}", resp.attempts, resp.error);
        }
    }

    // --- Self-Critique stage ---
    bool self_critique_used = false;
    if (self_critique && resp.ok) {
        spdlog::info("Self-Critique enabled: running second-pass audit ...");
        aipr::gateway::LLMConfig cfg3 = cfg;
        cfg3.temperature = 0.2;
        aipr::gateway::LLMGateway gw3(cfg3);
        const std::string critique_sys  = aipr::prompt::critique_system_prompt();
        const std::string critique_user =
            aipr::prompt::build_critique_user_prompt(user_prompt, resp.content);
        auto resp3 = gw3.chat(critique_sys, critique_user);
        total_attempts += resp3.attempts;
        if (resp3.ok) {
            spdlog::info("Self-Critique stage ok after {} attempt(s), {} chars",
                         resp3.attempts, resp3.content.size());
            resp = std::move(resp3);
            self_critique_used = true;
        } else {
            spdlog::warn("Self-Critique stage failed: {} -- keeping previous result as final",
                         resp3.error);
        }
    }

    resp.attempts = total_attempts;

    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    if (resp.ok) {
        spdlog::info("Final review ready in {} ms, total {} attempt(s), {} chars",
                     elapsed.count(), resp.attempts, resp.content.size());
    } else {
        spdlog::error("LLM failed after {} attempt(s): {}", resp.attempts, resp.error);
    }

    // --- Render & write report ---
    const std::string report = aipr::pipeline::render_report(
        *pr, consensus_n, resp, model, elapsed, persona, self_critique_used);

    try {
        std::filesystem::path out(output_path);
        if (out.has_parent_path()) {
            std::filesystem::create_directories(out.parent_path());
        }
        std::ofstream ofs(out, std::ios::binary | std::ios::trunc);
        if (!ofs) {
            spdlog::error("Cannot open output file: {}", output_path);
            return 4;
        }
        ofs << report;
    } catch (const std::exception& e) {
        spdlog::error("Failed to write report: {}", e.what());
        return 4;
    }

    spdlog::info("Report written to {} ({} bytes)", output_path, report.size());
    std::cout << "\n=== REPORT PREVIEW (first 1200 chars) ===\n"
              << report.substr(0, std::min<std::size_t>(report.size(), 1200))
              << "\n=== END PREVIEW ===\n";

    // ----- Optional: upload to Qiniu KODO -----
    if (upload_kodo) {
        if (!aipr::storage::KodoClient::env_ready()) {
            spdlog::error("--upload-kodo set, but QINIU_AK / QINIU_SK / QINIU_BUCKET "
                          "not all present in env -- skipping upload");
        } else {
            const auto kcfg = aipr::storage::KodoClient::from_env();
            aipr::storage::KodoClient kc(kcfg);

            const std::string ts = aipr::common::utc_stamp();
            std::ostringstream key_oss;
            key_oss << kodo_prefix << "/" << pr->locator.owner << "-"
                    << pr->locator.repo << "-PR" << pr->locator.number
                    << "-" << ts << ".md";
            const std::string key = key_oss.str();

            spdlog::info("KODO: uploading report to bucket='{}' key='{}'",
                         kcfg.bucket, key);
            const auto kr = kc.upload(key, report);
            if (kr.ok) {
                std::cout << "\n=== KODO UPLOAD ===\n"
                          << "Bucket : " << kcfg.bucket << "\n"
                          << "Key    : " << kr.key    << "\n"
                          << "Hash   : " << kr.hash   << "\n";
                if (!kr.url.empty()) {
                    std::cout << "URL    : " << kr.url << "\n";
                }
                std::cout << "=== END KODO UPLOAD ===\n";

                if (!kr.url.empty()) {
                    std::ofstream afs(output_path, std::ios::app | std::ios::binary);
                    if (afs) {
                        afs << "\n---\n"
                            << "📦 **报告已上传至七牛云 KODO**\n\n"
                            << "- 📥 [直接下载](" << kr.url << ")\n"
                            << "- ☁️ [跳转 KODO 空间](https://portal.qiniu.com/kodo/bucket/resource-v2?bucketName=" << kcfg.bucket << ")\n";
                    }
                }
            } else {
                spdlog::error("KODO upload failed: {}", kr.error);
            }
        }
    }

    return resp.ok ? 0 : 5;
}
