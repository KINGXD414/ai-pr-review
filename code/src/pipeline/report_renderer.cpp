/**
 * @file report_renderer.cpp
 * @brief Implementation of the Markdown report renderer.
 */
#include "pipeline/report_renderer.h"

#include "common/utils.h"
#include "prompt/personas.h"

#include <sstream>

namespace aipr::pipeline {

std::string render_report(const aipr::github::PullRequest& pr,
                          int                               consensus_n,
                          const aipr::gateway::LLMResponse& llm,
                          const std::string&                model,
                          std::chrono::milliseconds         elapsed,
                          const std::string&                persona,
                          bool                              self_critique_used) {
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
        << "| 生成时间 | " << aipr::common::now_iso() << " |\n\n"
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

}  // namespace aipr::pipeline
