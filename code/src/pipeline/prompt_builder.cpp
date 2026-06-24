/**
 * @file prompt_builder.cpp
 * @brief Implementation of the prompt builder extracted from main.cpp.
 */
#include "pipeline/prompt_builder.h"

#include "prompt/personas.h"

#include <algorithm>
#include <sstream>

namespace aipr::pipeline {

namespace {
constexpr std::size_t kMaxFilesInPrompt     = 30;
constexpr std::size_t kMaxPatchLinesPerFile = 200;
constexpr std::size_t kMaxBodyCharsInPrompt = 2000;
}  // namespace

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
    const aipr::github::PullRequest&                pr,
    const std::vector<aipr::analyzer::AstSummary>&  ast_summaries,
    const std::vector<aipr::rule::RuleHit>&         rule_hits) {

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

    // ---- Rule engine pre-scan (deterministic, zero-false-positive safety net) ----
    if (!rule_hits.empty()) {
        oss << "# 规则引擎预扫描（确定性检查，零误报兜底）\n\n";
        oss << "以下问题由规则引擎在 diff 上直接检出，无需 LLM 二次判断。"
               "请在审查中引用这些发现，不要再重复报告相同问题。\n\n";
        for (const auto& h : rule_hits) {
            oss << "- **`" << h.file << ":" << h.line << "`** ["
                << aipr::rule::severity_str(h.severity) << "] "
                << h.rule_id << " — " << h.message;
            if (!h.suggestion.empty()) {
                oss << " (建议: " << h.suggestion << ")";
            }
            oss << "\n";
        }
        oss << "\n";
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
            << " 个文件未展示，留待 chunk-merge 策略处理)_\n";
    }

    // ---- AST analysis section ----
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

}  // namespace aipr::pipeline
