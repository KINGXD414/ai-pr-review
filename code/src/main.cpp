/**
 * @file main.cpp
 * @brief CLI entry point for the AI PR Review assistant (Day 1).
 *
 * Pipeline: parse args -> resolve credentials -> parse PR URL ->
 * GitHub API fetch -> (optional) AST analysis -> assemble prompt ->
 * DeepSeek chat -> write Markdown report.
 */

#include "analyzer/ast_analyzer.h"
#include "diff/parser.h"
#include "gateway/llm_gateway.h"
#include "github/api_client.h"

#include <CLI/CLI.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <chrono>
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

std::string build_system_prompt() {
    return R"(You are a senior C++ / Go / general-purpose code reviewer.
You read a GitHub Pull Request diff and produce a structured Markdown review.
Always respond in Simplified Chinese.

Your report MUST use exactly the following three top-level sections, in order:

## 一、变更总结
* 用 3-6 个 bullet 概括 PR 的整体意图、影响面、改动模块。
* 不要逐文件复述，要抽象出主线。

## 二、风险代码
* 列出你认为有风险的修改，每条用如下格式：
  - **`<文件>:<行号>`** [严重度: 高 / 中 / 低] — <一句话风险描述>
    - 触发条件 / 后果：…
    - 建议修复：…
* 若确实没有风险，写"未发现明显风险"，并说明你检查了哪些维度。

## 三、Review 建议
* 给作者 3-5 条可执行的建议（命名、注释、测试、可读性、性能等）。
* 每条用一行 bullet。

写作要求：
1. 严格基于 diff 给出的内容，禁止虚构未出现的函数名、变量名、行号。
2. 严重度判断要谨慎，宁可漏报也不要堆砌"高风险"。
3. 涉及并发 / 内存 / 错误处理 / API 兼容性 时优先标注。
4. 不要用 ```markdown 围栏包裹整体输出。)";
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
                          const aipr::gateway::LLMResponse& llm,
                          const std::string& model,
                          std::chrono::milliseconds elapsed) {
    std::ostringstream oss;
    oss << "# AI PR Review 报告\n\n"
        << "> 由 `ai-pr-review` 自动生成。仅供辅助参考，最终判断以 reviewer 为准。\n\n"
        << "| 项 | 值 |\n"
        << "| --- | --- |\n"
        << "| PR | [" << pr.locator.owner << "/" << pr.locator.repo
        << " #" << pr.locator.number << "](" << pr.html_url << ") |\n"
        << "| 标题 | " << pr.title << " |\n"
        << "| 作者 | " << pr.author << " |\n"
        << "| 文件数 | " << pr.files.size() << " |\n"
        << "| 模型 | " << model << " |\n"
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
    app.set_version_flag("--version", std::string("ai-pr-review 0.1.0"));

    std::string pr_url;
    std::string github_token;
    std::string llm_key;
    std::string output_path  = "review.md";
    std::string model        = "deepseek-chat";
    std::string llm_host     = "api.deepseek.com";
    std::string llm_path     = "/v1/chat/completions";
    std::string user_agent   = "ai-pr-review/0.1";
    std::string source_dir;
    int timeout_s = 60;
    int retries   = 3;
    bool verbose  = false;

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

    const std::string system_prompt = build_system_prompt();
    const std::string user_prompt   = build_user_prompt(*pr, ast_summaries);
    spdlog::debug("System prompt: {} chars", system_prompt.size());
    spdlog::debug("User prompt: {} chars", user_prompt.size());

    aipr::gateway::LLMConfig cfg;
    cfg.host            = llm_host;
    cfg.path            = llm_path;
    cfg.model           = model;
    cfg.api_key         = llm_key;
    cfg.timeout_seconds = timeout_s;
    cfg.max_retries     = retries;
    cfg.temperature     = 0.2;

    aipr::gateway::LLMGateway gw(cfg);
    spdlog::info("Calling {} on {}{} ...", model, llm_host, llm_path);

    const auto t0 = std::chrono::steady_clock::now();
    auto resp = gw.chat(system_prompt, user_prompt);
    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0);

    if (resp.ok) {
        spdlog::info("LLM ok in {} ms after {} attempt(s), {} chars",
                     elapsed.count(), resp.attempts, resp.content.size());
    } else {
        spdlog::error("LLM failed after {} attempt(s): {}",
                      resp.attempts, resp.error);
    }

    const std::string report = render_report(*pr, resp, model, elapsed);
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

    return resp.ok ? 0 : 5;
}
