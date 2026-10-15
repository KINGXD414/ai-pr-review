/**
 * @file skills.h
 * @brief Thin "Skills" registry over the existing prompt strategies.
 *
 * JD asks for "Skill" (agent skills / composable capabilities).  Rather than a
 * heavy refactor, we model each review strategy — the 5 personas plus the two
 * meta-strategies (self-critique, consensus) — as a named *Skill* with an id, a
 * human description, and the prompt fragment that realises it.  This makes the
 * "Skills" concept first-class and agent-callable (see agent.cpp's
 * select_skill / list_skills tools) without duplicating any prompt logic.
 */
#pragma once

#include <string>
#include <vector>

namespace aipr::prompt {

/// A composable review capability the agent (or user) can select.
struct Skill {
    std::string id;            ///< e.g. "linus", "devil", "self_critique"
    std::string name;          ///< human label
    std::string description;   ///< what choosing this skill changes
    std::string system_prompt; ///< prompt fragment realised when selected
};

/// All built-in skills (personas + meta-strategies).
std::vector<Skill> all_skills();

/// Find a skill by id, or nullptr if unknown.
const Skill* find_skill(const std::string& id);

/// Whether `id` is one of the 5 base personas (acceptable as --persona).
bool is_persona_skill(const std::string& id);

/// Markdown bullet list of skills, for the agent's list_skills tool.
std::string list_skills_markdown();

}  // namespace aipr::prompt
