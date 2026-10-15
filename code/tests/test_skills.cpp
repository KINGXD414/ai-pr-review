/**
 * @file test_skills.cpp
 * @brief Offline tests for the Skill registry (JD "Skill" alignment).
 *
 * Verifies that personas + meta-strategies are exposed as a unified, queryable
 * registry — the foundation for the agent's select_skill / list_skills tools.
 */
#include "prompt/skills.h"

#include <iostream>
#include <string>

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::cerr << "FAIL: " << #cond << " @line " << __LINE__     \
                      << "\n";                                          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

}  // namespace

int main() {
    using namespace aipr::prompt;

    auto skills = all_skills();
    // 5 personas + self_critique + consensus = 7
    CHECK(skills.size() >= 7);

    const Skill* linus = find_skill("linus");
    CHECK(linus != nullptr);
    CHECK(!linus->name.empty());
    CHECK(!linus->system_prompt.empty());
    CHECK(linus->system_prompt.find("Linus") != std::string::npos);

    CHECK(find_skill("does_not_exist") == nullptr);
    CHECK(is_persona_skill("zen"));
    CHECK(!is_persona_skill("self_critique"));

    std::string md = list_skills_markdown();
    CHECK(md.find("linus") != std::string::npos);
    CHECK(md.find("consensus") != std::string::npos);

    if (failures == 0) {
        std::cout << "ALL SKILLS REGISTRY TESTS PASSED\n";
        return 0;
    }
    std::cerr << failures << " CHECK(s) failed\n";
    return 1;
}
