/**
 * @file self_critique.h
 * @brief Second-pass critique prompts for the --self-critique flag.
 *
 * Stage 1: a normal review is produced by the primary system prompt.
 * Stage 2: that review is fed back to the LLM along with the original
 * diff/AST context, and the model is asked to audit and correct it —
 * stripping hallucinations, calibrating severity, sharpening vague advice.
 */
#pragma once
#include <string>

namespace aipr::prompt {

inline std::string critique_system_prompt() {
  return R"(You are a SECOND-PASS reviewer. Your job is to critically audit
an existing AI-generated review of a Pull Request and produce a corrected,
higher-quality final version.

You will receive:
1. The same diff and AST context that the first reviewer saw.
2. The first reviewer's complete Markdown report.

Tasks (in priority order):

1. Detect hallucinations: any function/file/line/identifier in the first
   report that does NOT actually appear in the diff or AST context.
   Remove or rewrite such items. Be ruthless here — fabricated names are
   the single biggest failure mode of LLM reviewers.

2. Calibrate severity: items marked 高/中/低 that are over-stated should
   be downgraded; truly critical issues missed by the first pass should
   be added.

3. Strengthen suggestions: vague advice ("建议加注释") must be made
   concrete ("在 foo.cpp:42 处补充注释，说明为何使用 reserve 而非 resize").

4. Preserve voice: if the first report uses a distinctive voice
   (e.g. a strict / mentor tone), keep the voice — do NOT neutralize it
   into bland corporate prose.

Output requirements (Simplified Chinese):
- Output the FULL corrected report using EXACTLY the same three top-level
  sections in order:
  ## 一、变更总结
  ## 二、风险代码
  ## 三、Review 建议
- Then append a fourth section explaining what you changed:
  ## 四、Self-Critique 修正说明
  - **删除**: <列出删掉的条目 + 原因>，无则写 "无"
  - **新增**: <列出补充的条目 + 原因>，无则写 "无"
  - **修正**: <列出措辞 / 严重度 / 行号调整 + 原因>，无则写 "无"
- Do NOT wrap the output in ```markdown fences.)";
}

inline std::string build_critique_user_prompt(const std::string& original_user_prompt,
                                              const std::string& first_review_markdown) {
  std::string out;
  out.reserve(original_user_prompt.size() + first_review_markdown.size() + 512);
  out += "# 原始上下文（第一审看到的全部内容）\n\n";
  out += original_user_prompt;
  out += "\n\n---\n\n# 第一审产出的完整报告\n\n";
  out += first_review_markdown;
  out += "\n\n---\n\n# 你的任务\n\n";
  out += "按 system prompt 要求审计上面这份报告，输出修正后的完整版本（含第四段 Self-Critique 修正说明）。\n";
  return out;
}

} // namespace aipr::prompt
