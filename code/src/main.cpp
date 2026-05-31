/**
 * @file main.cpp
 * @brief CLI entry point for the AI PR Review assistant.
 *
 * Pipeline: parse args -> resolve credentials -> parse PR URL ->
 * GitHub API fetch -> (optional) AST analysis -> assemble prompt ->
 * DeepSeek chat -> write Markdown report.
 *
 * Day-2: added --persona flag for multi-persona reviewing.
 * Day-2: added --consensus N flag for multi-vote consensus.
 * Day-2: added --self-critique flag for two-pass hallucination filter.
 */

#include "analyzer/ast_analyzer.h"
#include "diff/parser.h"
#include "gateway/llm_gateway.h"
#include "github/api_client.h"
#include "prompt/personas.h"
#include "prompt/consensus.h"
#include "prompt/self_critique.h"
#include "storage/kodo_client.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kMaxFilesInPrompt     = 30;
constexpr std::size_t kMaxPatchLinesPerFile = 200;
constexpr std::size_t kMaxBodyCharsInPrompt = 2000;

std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

std::string truncate_patch(const std::string& patch, std::size_t max_lines) {
    if (patch.empty()) return patch;
    std::istringstream iss(patch);
    std::ostringstream oss;
    std::string line;
    std::size_t count = 0;
    while (std::getline(iss, line)) {
        if (count >= max_lines) {
            oss << "... (patch truncated, " << max_lines << " lines kept)\n";
            break;
        }
        oss << line << '\n';
        ++count;
    }
    return oss.str();
}

std::string build_system_prompt(const std::string& persona) {
    return aipr::prompt::system_prompt_for(persona);
}

std::string build_user_prompt(
    const aipr::github::PullRequest& pr,
    const std::vector<aipr::analyzer::AstSummary>& ast_summaries = {}) {

    std::ostringstream oss;
    oss << "# PR 元信息\n"
        << "- 仓库：" << pr.locator.owner << "/" << pr.locator.repo << "\n"
        << "- 编号：#" << pr.locator.number << "\n"
        << "- 标题：" << pr.title << "\n"
        << "- 作者：" << pr.author << "\n"
        << "- base / head：" << pr.base_ref << " <- " << pr.head_ref << "\n"
        << "- 链接：" << pr.html_url << "\n\n";

    if (!pr.body.empty()) {
        std::string body = pr.body;
        if (body.size() > kMaxBodyCharsInPrompt) {
            body = body.substr(0, kMaxBodyCharsInPrompt) + "\n...(描述已截断)";
        }
        oss << "# PR 描述\n" << body << "\n\n";
    }

    oss << "# 变更文件（共 " << pr.files.size() << " 个，最多展示 "
        << kMaxFilesInPrompt << " 个，每个 patch 最多 "
        << kMaxPatchLinesPerFile << " 行）\n\n";

    const std::size_t limit = std::min(pr.files.size(), kMaxFilesInPrompt);
    for (std::size_t i = 0; i < limit; ++i) {
        const auto& f = pr.files[i];
        oss << "## " << (i + 1) << ". `" << f.filename << "` ["
            << f.status << ", +" << f.additions << "/-" << f.deletions << "]\n";
        if (f.patch.empty()) {
            oss << "_(二进制文件或纯重命名，无 patch)_\n\n";
            continue;
        }
        oss << "```diff\n"
            << truncate_patch(f.patch, kMaxPatchLinesPerFile)
            << "```\n\n";
    }

    if (pr.files.size() > kMaxFilesInPrompt) {
        oss << "_(另有 " << (pr.files.size() - kMaxFilesInPrompt)
            << " 个文件未展示，留待 Day 2 的 chunk-merge 策略处理)_\n";
    }

    // --- AST analysis section (optional, appears when --source-dir is used) ---
    if (!ast_summaries.empty()) {
        std::size_t shown = 0;
        for (const auto& s : ast_summaries) if (s.ok) ++shown;
        if (shown > 0) {
            oss << "\n# AST 分析（基于 libclang，含 " << shown << " 个文件）\n\n";
            for (const auto& s : ast_summaries) {
                if (!s.ok) continue;
                oss << s.to_prompt() << "\n";
            }
        }
    }

    return oss.str();
}

std::string now_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return ts;
}

std::string render_report(const aipr::github::PullRequest& pr,
                          int consensus_n,
                          const aipr::gateway::LLMResponse& llm,
                          const std::string& model,
                          std::chrono::milliseconds elapsed,
                          const std::string& persona,
                          bool self_critique_used) {
    std::ostringstream oss;
    oss << "# AI PR Review 报告\n\n"
        << "> 由 `ai-pr-review` 自动生成。仅供辅助参考，最终判断以 reviewer 为准。\n"
        << "> " << aipr::prompt::persona_badge(persona) << "\n";
    if (consensus_n > 1) {
        oss << "> 🗳️ Consensus 模式 — " << consensus_n
            << " 次独立审查后按多数票合成，保留阈值 ⌈N/2⌉="
            << ((consensus_n + 1) / 2) << "\n";
    }
    if (self_critique_used) {
        oss << "> 🪞 Self-Critique 模式 — 二审已对一审进行幻觉过滤、严重度校准、建议强化\n";
    }
    oss << "\n"
        << "| 项 | 值 |\n"
        << "| --- | --- |\n"
        << "| PR | [" << pr.locator.owner << "/" << pr.locator.repo
        << " #" << pr.locator.number << "](" << pr.html_url << ") |\n"
        << "| 标题 | " << pr.title << " |\n"
        << "| 作者 | " << pr.author << " |\n"
        << "| Persona | " << aipr::prompt::persona_badge(persona) << " |\n"
        << "| 文件数 | " << pr.files.size() << " |\n"
        << "| 模型 | " << model << " |\n"
        << "| Consensus | " << (consensus_n > 1
                                ? ("✅ N=" + std::to_string(consensus_n)
                                   + "（阈值 " + std::to_string((consensus_n + 1) / 2) + "）")
                                : std::string("未启用")) << " |\n"
        << "| Self-Critique | " << (self_critique_used ? "✅ 启用（二阶段）" : "未启用") << " |\n"
        << "| LLM 调用次数 | " << llm.attempts << " |\n"
        << "| 用时 | " << elapsed.count() << " ms |\n"
        << "| 生成时间 | " << now_iso() << " |\n\n"
        << "---\n\n";

    if (llm.ok) {
        oss << llm.content << "\n";
    } else {
        oss << "## ⚠️ 评审失败\n\n"
            << "调用 LLM 失败：`" << llm.error << "`（重试 "
            << llm.attempts << " 次）。\n";
    }
    return oss.str();
}

} // namespace

int main(int argc, char** argv) {
    CLI::App app{"AI-powered Pull Request review assistant"};
    app.set_version_flag("--version", std::string("ai-pr-review 0.2.0"));

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
    app.add_flag("-v,--verbose", verbose,
        "Enable debug logs");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    if (github_token.empty()) github_token = env_or_empty("GITHUB_TOKEN");
    if (llm_key.empty())      llm_key      = env_or_empty("DEEPSEEK_API_KEY");
    if (llm_key.empty()) {
        spdlog::error("LLM key missing: pass --llm-key or set DEEPSEEK_API_KEY");
        return 2;
    }
    if (github_token.empty()) {
        spdlog::warn("GitHub token empty -- using anonymous access (60 req/h).");
    }

    // Validate persona early
    if (persona != "default" && persona != "linus" && persona != "mentor"
        && persona != "devil" && persona != "zen") {
        spdlog::error("Unknown persona '{}'. Choose: default | linus | mentor | devil | zen", persona);
        return 2;
    }

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

    const std::string system_prompt = build_system_prompt(persona);
    const std::string user_prompt   = build_user_prompt(*pr, ast_summaries);
    spdlog::debug("System prompt: {} chars", system_prompt.size());
    spdlog::debug("User prompt: {} chars", user_prompt.size());
    spdlog::info("Persona: {}", persona);

    aipr::gateway::LLMConfig cfg;
    cfg.host            = llm_host;
    cfg.path            = llm_path;
    cfg.model           = model;
    cfg.api_key         = llm_key;
    cfg.timeout_seconds = timeout_s;
    cfg.max_retries     = retries;
    cfg.temperature     = 0.2;

    // --- Stage 1: run N independent reviews (N=1 keeps default behaviour) ---
    // Raise temperature when N>1 so the N runs actually differ enough for voting.
    if (consensus_n > 1) cfg.temperature = 0.7;

    aipr::gateway::LLMGateway gw(cfg);
    spdlog::info("Calling {} on {}{} (persona={}, consensus N={}) ...",
                 model, llm_host, llm_path, persona, consensus_n);

    const auto t0 = std::chrono::steady_clock::now();
    aipr::gateway::LLMResponse resp;
    std::vector<std::string> stage1_reports;
    stage1_reports.reserve(consensus_n);
    int total_attempts = 0;
    for (int i = 1; i <= consensus_n; ++i) {
        spdlog::info("  consensus run {}/{} ...", i, consensus_n);
        auto r = gw.chat(system_prompt, user_prompt);
        total_attempts += r.attempts;
        if (!r.ok) {
            spdlog::error("LLM run {}/{} failed after {} attempt(s): {}",
                          i, consensus_n, r.attempts, r.error);
            resp = std::move(r);
            stage1_reports.clear();
            break;
        }
        spdlog::info("  run {}/{} ok, {} chars", i, consensus_n, r.content.size());
        stage1_reports.push_back(r.content);
        resp = std::move(r);
    }

    // --- Consensus stage: meta-review when we have N>=2 successful runs ---
    if (consensus_n > 1 && !stage1_reports.empty()
            && (int)stage1_reports.size() == consensus_n) {
        spdlog::info("Consensus: consolidating {} runs by majority vote ...",
                     consensus_n);
        aipr::gateway::LLMConfig cfg2 = cfg;
        cfg2.temperature = 0.2; // meta-review should be deterministic
        aipr::gateway::LLMGateway gw2(cfg2);
        const std::string consensus_sys  =
            aipr::prompt::consensus_system_prompt(consensus_n);
        const std::string consensus_user =
            aipr::prompt::build_consensus_user_prompt(stage1_reports, user_prompt);
        auto resp2 = gw2.chat(consensus_sys, consensus_user);
        total_attempts += resp2.attempts;
        if (resp2.ok) {
            spdlog::info("Consensus stage ok after {} attempt(s), {} chars",
                         resp2.attempts, resp2.content.size());
            resp = std::move(resp2);
        } else {
            spdlog::warn("Consensus stage failed: {} -- keeping last run as fallback",
                         resp2.error);
        }
    }

    // --- Self-Critique stage: optional second-pass audit on the final report ---
    bool self_critique_used = false;
    if (self_critique && resp.ok) {
        spdlog::info("Self-Critique enabled: running second-pass audit ...");
        aipr::gateway::LLMConfig cfg3 = cfg;
        cfg3.temperature = 0.2; // critique should be deterministic
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
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    if (resp.ok) {
        spdlog::info("Final review ready in {} ms, total {} attempt(s), {} chars",
                     elapsed.count(), resp.attempts, resp.content.size());
    } else {
        spdlog::error("LLM failed after {} attempt(s): {}",
                      resp.attempts, resp.error);
    }

    const std::string report = render_report(*pr, consensus_n, resp, model, elapsed, persona, self_critique_used);

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
            const auto cfg = aipr::storage::KodoClient::from_env();
            aipr::storage::KodoClient kc(cfg);

            // key = <prefix>/<owner>-<repo>-PR<n>-<UTC timestamp>.md
            const std::time_t now_t = std::time(nullptr);
            std::tm tm{};
            gmtime_r(&now_t, &tm);
            char ts[32] = {0};
            std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);

            std::ostringstream key_oss;
            key_oss << kodo_prefix << "/" << pr->locator.owner << "-"
                    << pr->locator.repo << "-PR" << pr->locator.number
                    << "-" << ts << ".md";
            const std::string key = key_oss.str();

            spdlog::info("KODO: uploading report to bucket='{}' key='{}'",
                         cfg.bucket, key);
            const auto kr = kc.upload(key, report);
            if (kr.ok) {
                std::cout << "\n=== KODO UPLOAD ===\n"
                          << "Bucket : " << cfg.bucket << "\n"
                          << "Key    : " << kr.key    << "\n"
                          << "Hash   : " << kr.hash   << "\n";
                if (!kr.url.empty()) {
                    std::cout << "URL    : " << kr.url << "\n";
                }
                std::cout << "=== END KODO UPLOAD ===\n";
            } else {
                spdlog::error("KODO upload failed: {}", kr.error);
            }
        }
    }

    return resp.ok ? 0 : 5;
}
