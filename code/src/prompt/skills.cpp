/**
 * @file skills.cpp
 * @brief Skill registry implementation.
 */
#include "prompt/skills.h"

#include "prompt/consensus.h"
#include "prompt/personas.h"
#include "prompt/self_critique.h"

#include <algorithm>

namespace aipr::prompt {

namespace {
const std::vector<Skill>& registry() {
    static const std::vector<Skill> kSkills = {
        {"default", "Default 模式 — 均衡资深审查者",
         "客观、专业，关注并发/内存/错误处理/API 兼容性。",
         kDefaultPrompt + std::string(kCommonStructure)},
        {"linus", "Linus 模式 — 直击痛点，不留情面",
         "直接、不留情面，但所有批评必须基于具体行号与事实。",
         kLinusPrompt + std::string(kCommonStructure)},
        {"mentor", "Mentor 模式 — 讲清楚为什么，再说怎么改",
         "面向初中级开发者，每个问题先解释 root cause 再给修复。",
         kMentorPrompt + std::string(kCommonStructure)},
        {"devil", "Devil 模式 — 专挑 edge case 与隐蔽 bug",
         "聚焦空指针/并发/错误处理/整数溢出等隐蔽风险。",
         kDevilPrompt + std::string(kCommonStructure)},
        {"zen", "Zen 模式 — 先肯定，再建议",
         "非暴力沟通，先看到光再指出影。",
         kZenPrompt + std::string(kCommonStructure)},
        {"self_critique", "Self-Critique — 二审幻觉过滤",
         "在初稿之上跑第二遍审核，删除幻觉、校准严重度、强化建议。",
         aipr::prompt::critique_system_prompt()},
        {"consensus", "Consensus — 多数投票合成",
         "多份独立审查按多数票合成高置信报告，每条保留项带 [共识 X/N]。",
         aipr::prompt::consensus_system_prompt(3)},
    };
    return kSkills;
}
}  // namespace

std::vector<Skill> all_skills() { return registry(); }

const Skill* find_skill(const std::string& id) {
    const auto& r = registry();
    auto it = std::find_if(r.begin(), r.end(),
                           [&](const Skill& s) { return s.id == id; });
    return it == r.end() ? nullptr : &(*it);
}

bool is_persona_skill(const std::string& id) {
    return id == "default" || id == "linus" || id == "mentor" ||
           id == "devil" || id == "zen";
}

std::string list_skills_markdown() {
    std::string out = "# 可用 Skills（审查策略）\n\n";
    for (const auto& s : registry()) {
        out += "- `" + s.id + "` — " + s.name + "\n  " + s.description + "\n";
    }
    return out;
}

}  // namespace aipr::prompt
