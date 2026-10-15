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
#include "agent/agent.h"
#include "diff/parser.h"
#include "gateway/llm_gateway.h"
#include "github/api_client.h"
#include "prompt/personas.h"
#include "prompt/consensus.h"
#include "prompt/self_critique.h"
#include "prompt/skills.h"
#include "rag/retriever.h"
#include "rag/embedder.h"
#include "rag/vector_store.h"
#include "rules/engine.h"
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
#include <iostream>
#include <memory>
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

/// Convert a GitHub PR into a ParsedDiff for the rule engine (P0).
/// Uses the unified `patch` payloads we already fetched — no re-networking.
aipr::rule::ParsedDiff parse_diff_from_pr(const aipr::github::PullRequest& pr) {
    aipr::rule::ParsedDiff out;
    out.files.reserve(pr.files.size());
    for (const auto& f : pr.files) {
        if (f.patch.empty()) continue;  // binary / pure rename: nothing to scan
        auto hunks = aipr::diff::parse_patch(f.patch);
        aipr::rule::FileDiff fd;
        fd.filename = f.filename;
        int new_ln = 0, old_ln = 0;
        // Re-derive per-line line numbers from the hunks for accurate reports.
        for (const auto& h : hunks) {
            new_ln = h.new_start;
            old_ln = h.old_start;
            for (const auto& l : h.lines) {
                if (l.type == aipr::diff::DiffLine::Type::Add) {
                    fd.added_lines.push_back(l.content);
                    fd.added_line_nums.push_back(new_ln);
                    ++new_ln;
                } else if (l.type == aipr::diff::DiffLine::Type::Del) {
                    fd.removed_lines.push_back(l.content);
                    fd.removed_line_nums.push_back(old_ln);
                    ++old_ln;
                } else {
                    ++new_ln; ++old_ln;  // context line advances both
                }
            }
        }
        out.files.push_back(std::move(fd));
    }
    return out;
}

/// Render rule-engine hits as a prompt section (P0 injection).
std::string build_rule_hits_prompt(const std::vector<aipr::rule::RuleHit>& hits) {
    if (hits.empty()) {
        return "\n# 规则引擎扫描结果\n\n(静态规则未命中任何确定性问题，请聚焦语义层审查。)\n";
    }
    std::ostringstream oss;
    oss << "\n# 规则引擎扫描结果（第一层，毫秒级，确定性规则）\n\n";
    oss << "以下命中供你复核——**不要盲目照搬**，请判断每条是否真实成立，"
        << "误报请直接忽略并说明理由：\n\n";
    for (const auto& h : hits) {
        // File-level rules carry no meaningful line number (line == 0); render
        // a location without a bogus ":0" suffix so reports stay readable.
        const std::string location = (h.line > 0)
            ? (h.file + ":" + std::to_string(h.line))
            : (h.file + " (file-level)");
        oss << "- [" << aipr::rule::severity_str(h.severity) << "] `"
            << h.rule_id << "` @ `" << location << "`\n"
            << "  - 问题: " << h.message << "\n";
        if (!h.suggestion.empty())
            oss << "  - 建议: " << h.suggestion << "\n";
    }
    oss << "\n";
    return oss.str();
}

aipr::rag::Retriever build_rag_corpus(const std::string& extra_dir) {
    aipr::rag::Retriever r;
    r.add_document("kb_unpaired_alloc",
        "内存管理：malloc/calloc/realloc 必须与 free 配对，new 必须与 delete 配对；"
        "跨函数分配释放时优先使用 std::unique_ptr / std::shared_ptr 管理所有权。");
    r.add_document("kb_raw_ptr_new",
        "优先使用 std::make_unique / std::make_shared 替代裸指针 new，避免异常路径下泄漏。");
    r.add_document("kb_c_style_cast",
        "C++ 中应改用 static_cast / const_cast / reinterpret_cast / dynamic_cast，"
        "避免 C 风格转换隐藏类型不安全。");
    r.add_document("kb_memcpy_size",
        "memcpy/memset 的 size 参数应使用 sizeof(数组) 而非 sizeof(指针)，"
        "否则会复制错误的字节数导致缓冲区溢出或截断。");
    r.add_document("kb_virtual_modify",
        "修改 virtual / override 方法会影响所有派生类与多态行为，需确认基类契约未被破坏。");
    r.add_document("kb_exception_safety",
        "异常安全：构造与赋值应保证强异常安全或基本保证，释放资源优先用 RAII；"
        "异步回调中注意对象生命周期，避免 double free 与 use-after-free。");
    r.add_document("kb_test_file",
        "测试文件变更风险较低，审查应聚焦断言正确性与覆盖率，而非风格。");

    if (!extra_dir.empty()) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(extra_dir, ec)) {
            if (entry.is_regular_file(ec)) {
                const auto ext = entry.path().extension().string();
                if (ext != ".md" && ext != ".txt" && ext != ".markdown") continue;
                std::ifstream in(entry.path(), std::ios::binary);
                std::stringstream ss; ss << in.rdbuf();
                const std::string text = ss.str();
                if (!text.empty()) r.add_document("corpus:" + entry.path().filename().string(), text);
            }
        }
    }
    r.build();
    return r;
}

std::string build_user_prompt(
    const aipr::github::PullRequest& pr,
    const std::vector<aipr::analyzer::AstSummary>& ast_summaries = {},
    const std::string& rule_hits_block = {}) {
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

    // --- Rule-engine section (P0: now wired into the main pipeline) ---
    if (!rule_hits_block.empty()) {
        oss << rule_hits_block;
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
    bool use_agent = false;        // P1: Function-Calling agentic review loop
    bool use_rag   = false;        // P3: retrieve relevant rules/history for context
    int  agent_max_iter = 6;
    std::string rag_corpus_dir;    // optional dir of .md/.txt to index for RAG
    bool use_embed = false;        // use the semantic (embedding + vector) path
    std::string vector_index_path; // optional on-disk vector index (--embed)
    bool list_skills = false;      // print the Skill registry and exit
    std::string kodo_prefix = "reviews";
    std::string kodo_viewer_base;  // not used — KODO links go directly into report footer

    app.add_option("--pr", pr_url,
        "GitHub PR URL, e.g. https://github.com/owner/repo/pull/123 "
        "(not required with --list-skills)");
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
    app.add_flag("--agent", use_agent,
        "Run the agentic review loop: the LLM decides which tools (rules/AST/RAG) to call");
    app.add_option("--agent-max-iter", agent_max_iter,
        "Max agent rounds (default 6)")->check(CLI::Range(1, 12));
    app.add_flag("--rag", use_rag,
        "Enable RAG: retrieve relevant coding rules / historical reviews and inject as context");
    app.add_option("--rag-corpus-dir", rag_corpus_dir,
        "Directory of .md/.txt files to index for RAG (optional; built-in rules used otherwise)");
    app.add_flag("--embed", use_embed,
        "Use the semantic path: embed corpus + query into vectors and retrieve by cosine "
        "similarity (offline HashingEmbedder by default; swap to a remote embedder in code). "
        "When omitted, RAG falls back to lexical BM25.");
    app.add_option("--index", vector_index_path,
        "Path to a persisted vector index (JSON). With --embed: load if exists, else build and "
        "save. Gives the RAG pipeline a real on-disk 向量数据库 artifact.");
    app.add_flag("--upload-kodo", upload_kodo,
        "Upload the rendered report to Qiniu KODO (needs env QINIU_AK/SK/BUCKET)");
    app.add_option("--kodo-prefix", kodo_prefix,
        "Key prefix used inside the KODO bucket (default: reviews)");
    app.add_option("--kodo-viewer", kodo_viewer_base,
        "Deprecated: KODO links are now embedded directly in the report footer");
    app.add_flag("-v,--verbose", verbose,
        "Enable debug logs");
    app.add_flag("--list-skills", list_skills,
        "Print the Skill registry (personas + meta-strategies) as Markdown and exit");

    CLI11_PARSE(app, argc, argv);

    spdlog::set_level(verbose ? spdlog::level::debug : spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S.%e] [%^%l%$] %v");

    // --list-skills is an offline, no-credential query (also used by the MCP
    // server's list_skills tool) — handle it before any credential checks.
    if (list_skills) {
        std::fputs(aipr::prompt::list_skills_markdown().c_str(), stdout);
        return 0;
    }
    if (pr_url.empty()) {
        spdlog::error("missing --pr (GitHub PR URL); use --list-skills to inspect available skills");
        return 2;
    }

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

    // --- P0: rule engine now runs on every PR (closes the README gap) ---
    const auto parsed_diff = parse_diff_from_pr(*pr);
    aipr::rule::RuleEngine rule_engine;
    const auto rule_hits = rule_engine.run(parsed_diff, /*ast=*/nullptr);
    if (!rule_hits.empty()) {
        spdlog::info("Rule engine: {} hit(s) on {} file(s)",
                     rule_hits.size(), parsed_diff.files.size());
    } else {
        spdlog::info("Rule engine: no hits");
    }
    const std::string rule_hits_block = build_rule_hits_prompt(rule_hits);
    // Observability: with -v you can see exactly what was injected into the
    // LLM prompt. This is the evidence that the rule engine is truly wired
    // into the pipeline (and not just a module with green unit tests).
    if (!rule_hits_block.empty()) {
        spdlog::debug("Rule hits injected into prompt ({} chars):\n{}",
                      rule_hits_block.size(), rule_hits_block);
    }

    // --- P3: optional RAG corpus (built-in rules + user corpus dir) ---
    std::unique_ptr<aipr::rag::Retriever> rag_index;
    if (use_rag) {
        rag_index = std::make_unique<aipr::rag::Retriever>(
            build_rag_corpus(rag_corpus_dir));
        rag_index->build();

        if (use_embed) {
            // Semantic path: wire an embedder + (optional) persisted vector store.
            auto embedder = std::make_shared<aipr::rag::HashingEmbedder>(256);
            rag_index->set_embedder(embedder);

            bool loaded = false;
            std::shared_ptr<aipr::rag::VectorStore> store;
            if (!vector_index_path.empty()) {
                auto s = std::make_shared<aipr::rag::BruteForceStore>();
                if (s->load(vector_index_path)) {
                    store = s;
                    loaded = true;
                    spdlog::info("RAG: loaded vector index from {} ({} items)",
                                 vector_index_path, s->size());
                } else {
                    spdlog::warn("RAG: cannot load {} -- building fresh index", vector_index_path);
                }
            }
            if (!loaded) {
                store = std::make_shared<aipr::rag::BruteForceStore>();
            }
            rag_index->set_store(store);
            rag_index->index_embeddings();
            if (!loaded && !vector_index_path.empty()) {
                auto* bs = dynamic_cast<aipr::rag::BruteForceStore*>(store.get());
                if (bs && bs->save(vector_index_path)) {
                    spdlog::info("RAG: saved vector index to {} ({} items)",
                                 vector_index_path, bs->size());
                }
            }
            spdlog::info("RAG: semantic (embedding+vector) path enabled, {} chunk(s)",
                         rag_index->size());
        } else {
            spdlog::info("RAG: lexical BM25 path, {} chunk(s)", rag_index->size());
        }
    }

    const std::string system_prompt = build_system_prompt(persona);
    const std::string base_user_prompt =
        build_user_prompt(*pr, ast_summaries, rule_hits_block);
    spdlog::debug("System prompt: {} chars", system_prompt.size());
    spdlog::debug("User prompt: {} chars", base_user_prompt.size());
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
    std::string user_prompt;                 // assigned below (non-agent path)
    std::vector<std::string> stage1_reports; // consensus runs collector
    int total_attempts = 0;

    // ----- P1: agentic review loop (Function Calling) -----
    // In agent mode the model itself decides which tools to call; we skip the
    // fixed consensus / self-critique post-stages (the agent already iterates).
    if (use_agent) {
        aipr::agent::AgentContext actx;
        actx.pr      = &pr.value();
        actx.diff    = &parsed_diff;
        actx.source_dir = source_dir;
        actx.retriever  = rag_index.get();
        actx.max_iterations = agent_max_iter;

        int agent_attempts = 0;
        spdlog::info("Agent mode: letting the model drive rule/AST/RAG tools ...");
        const std::string agent_report = aipr::agent::run_agent(
            system_prompt, base_user_prompt, gw, actx, agent_attempts);
        resp.ok      = !agent_report.empty();
        resp.content = agent_report;
        resp.attempts = agent_attempts;
    }

    // ----- Non-agent path: optional RAG injection into the prompt -----
    user_prompt = base_user_prompt;
    if (!use_agent && use_rag && rag_index) {
        // Use the PR title + first file names as a pseudo-query, then inject
        // the top-k relevant rule/history snippets ahead of the diff.
        std::string query = pr->title;
        auto ctx_hits = rag_index->search_best(query, 3);
        if (!ctx_hits.empty()) {
            std::ostringstream rag_block;
            rag_block << "\n# 相关编码规范 / 历史审查片段（RAG 检索 top-"
                      << ctx_hits.size() << "）\n\n";
            for (const auto& h : ctx_hits) rag_block << "- " << h.id << "\n";
            rag_block << "\n";
            user_prompt = rag_block.str() + base_user_prompt;
            spdlog::info("RAG: injected {} snippet(s) into prompt",
                         ctx_hits.size());
        }
    }

    // NOTE: in agent mode the agent loop has ALREADY produced the final report.
    // Running the fixed single-shot stage here would (a) silently overwrite the
    // agent's output with a plain chat result and (b) double the LLM cost.
    // So the whole stage-1 loop is skipped when the agent owns the review.
    if (!use_agent) {
        stage1_reports.reserve(consensus_n);
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
    }

    // --- Consensus stage: meta-review when we have N>=2 successful runs ---
    // (skipped in agent mode)
    if (!use_agent && consensus_n > 1 && !stage1_reports.empty()
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
    }  // (skipped entirely in agent mode)

    // --- Self-Critique stage: optional second-pass audit on the final report ---
    // (also skipped in agent mode — the agent already self-iterates)
    bool self_critique_used = false;
    if (!use_agent && self_critique && resp.ok) {
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

    // In agent mode the attempt count comes from the agent loop itself
    // (run_agent sums the model round-trips); don't clobber it with the
    // stage-1 counter, which stays 0 when the agent owns the review.
    if (!use_agent) resp.attempts = total_attempts;

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

                // Append KODO links to the report file footer
                if (!kr.url.empty()) {
                    std::ofstream afs(output_path, std::ios::app | std::ios::binary);
                    if (afs) {
                        afs << "\n---\n"
                            << "📦 **报告已上传至七牛云 KODO**\n\n"
                            << "- 📥 [直接下载](" << kr.url << ")\n"
                            << "- ☁️ [跳转 KODO 空间](https://portal.qiniu.com/kodo/bucket/resource-v2?bucketName=" << cfg.bucket << ")\n";
                    }
                }
            } else {
                spdlog::error("KODO upload failed: {}", kr.error);
            }
        }
    }

    return resp.ok ? 0 : 5;
}
