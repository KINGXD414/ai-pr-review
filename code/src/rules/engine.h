#pragma once

#include <string>
#include <vector>

namespace aipr::rule {

struct RuleHit {
    std::string file;
    int         line{0};
    std::string level;      // "high" / "medium" / "low"
    std::string rule_name;
    std::string reason;
};

struct FileDiff {
    std::string              filename;
    std::vector<std::string> added_lines;
    std::vector<int>         added_line_nums;
    std::vector<std::string> removed_lines;
    std::vector<int>         removed_line_nums;
    std::vector<std::string> context_lines;
};

class RuleEngine {
public:
    std::vector<RuleHit> scan(const std::vector<FileDiff>& diffs);

private:
    void check_unpaired_alloc(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_virtual_modify(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_format_only(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_test_file(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_todo_fixme(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_raw_ptr_new(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_c_style_cast(const FileDiff& diff, std::vector<RuleHit>& hits);
    void check_memcpy_size(const FileDiff& diff, std::vector<RuleHit>& hits);
};

}  // namespace aipr::rule
