/**
 * @file consensus.h
 * @brief Multi-vote consensus prompts for the --consensus N flag.
 *
 * Stage 1 is run N times (independent LLM calls, each yields a markdown review;
 * temperature is raised so the runs actually differ).
 * The consensus stage then feeds all N reports back to the LLM as a
 * meta-reviewer and asks it to keep only items endorsed by ceil(N/2)
 * of the N runs, producing a higher-confidence consolidated report
 * where every kept item is annotated with its vote tally.
 */
#pragma once
#include <string>
#include <vector>

namespace aipr::prompt {

inline std::string consensus_system_prompt(int n) {
    int threshold = (n + 1) / 2; // ceil(N/2)
    std::string s = "You are a META-REVIEWER. You will receive N independent ";
    s += "AI-generated review reports of the SAME Pull Request and must ";
    s += "consolidate them into ONE high-confidence final report, keeping only ";
    s += "items endorsed by at least ceil(N/2) = " + std::to_string(threshold);
    s += " out of N = " + std::to_string(n) + " runs.\n\n";
    s += R"(Tasks (in priority order):

1. For each candidate risk / suggestion / summary point that appears across
   the N reports, count how many reports endorse it. Treat SEMANTIC
   equivalents as the same vote — e.g. "race condition on shared_ptr" and
   "thread-unsafe access to the shared pointer" count as the same vote.

2. KEEP an item iff its vote count >= ceil(N/2). Suffix every kept item
   in sections 二 and 三 with its tally, e.g. "[共识 3/3]" or "[共识 2/3]".

3. DISCARD items endorsed by fewer reports — these are likely random
   hallucinations or one-off speculation. They MUST NOT appear in the main
   sections (一/二/三). Record what was discarded in section 四.

4. If reports disagree on severity (e.g. one says 高, another says 中),
   take the MEDIAN; tie goes to the LOWER severity (conservative).

5. Preserve the overall structure and any persona voice present in the
   majority of inputs. Do not water down or rewrite voice unnecessarily.

Output requirements (Simplified Chinese):
- Output the FULL consolidated report using EXACTLY the same three top-level
  sections in order:
  ## 一、变更总结
  ## 二、风险代码
  ## 三、Review 建议
- Each item in 二/三 MUST end with a tally like "[共识 X/N]".
- Then append a fourth section explaining the voting:
  ## 四、Consensus 共识说明
  - **总运行次数**: N
  - **保留阈值**: ceil(N/2)
  - **保留项**: <列出保留的关键项 + 票数>
  - **丢弃项**: <列出丢弃的项 + 票数 + 简要原因>，无则写 "无"
- Do NOT wrap the output in ```markdown fences.)";
    return s;
}

inline std::string build_consensus_user_prompt(
    const std::vector<std::string>& reports,
    const std::string& original_user_prompt) {
    std::string out;
    std::size_t cap = original_user_prompt.size() + 1024;
    for (const auto& r : reports) cap += r.size() + 256;
    out.reserve(cap);

    out += "# 原始上下文（每次审查看到的相同输入）\n\n";
    out += original_user_prompt;
    out += "\n\n---\n\n";
    for (std::size_t i = 0; i < reports.size(); ++i) {
        out += "# 第 " + std::to_string(i + 1) + " 次独立审查产出\n\n";
        out += reports[i];
        out += "\n\n---\n\n";
    }
    out += "# 你的任务\n\n";
    out += "按 system prompt 要求对上面 " + std::to_string(reports.size());
    out += " 次独立审查做投票合成，输出共识版本（含第四段 Consensus 共识说明，每条保留项必须带 [共识 X/N] 票数标记）。\n";
    return out;
}

} // namespace aipr::prompt
