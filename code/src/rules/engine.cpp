#include "rules/engine.h"
#include "analyzer/ast_analyzer.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace aipr::rule {

// ---------------------------------------------------------------------------
// Severity helpers
// ---------------------------------------------------------------------------
const char* severity_str(Severity s) noexcept {
    switch (s) {
        case Severity::Info:  return "info";
        case Severity::Warn:  return "warn";
        case Severity::Error: return "error";
    }
    return "info";
}

// ---------------------------------------------------------------------------
// Engine::run — primary interface (Issue #1)
// ---------------------------------------------------------------------------
std::vector<RuleHit> RuleEngine::run(const ParsedDiff& diff,
                                     const analyzer::AstSummary* ast) {
    std::vector<RuleHit> hits;

    for (const auto& file_diff : diff.files) {
        check_unpaired_alloc(file_diff, hits);
        check_virtual_modify(file_diff, hits);
        check_format_only(file_diff, hits);
        check_test_file(file_diff, hits);
        check_todo_fixme(file_diff, hits);
        check_raw_ptr_new(file_diff, hits);
        check_c_style_cast(file_diff, hits);
        check_memcpy_size(file_diff, hits);
    }

    // AST enrichment: when AST context is available, we can upgrade severity
    // or add supplementary findings. Currently a no-op placeholder — the
    // AST→rule integration is a follow-up per ADR-001.
    (void)ast;

    return hits;
}

// ---------------------------------------------------------------------------
// Engine::scan — legacy interface
// ---------------------------------------------------------------------------
std::vector<RuleHit> RuleEngine::scan(const std::vector<FileDiff>& diffs) {
    ParsedDiff pd;
    pd.files = diffs;
    return run(pd, nullptr);
}

// ---------------------------------------------------------------------------
// Rule implementations
// ---------------------------------------------------------------------------

void RuleEngine::check_unpaired_alloc(const FileDiff& diff,
                                      std::vector<RuleHit>& hits) {
    bool has_alloc = false;
    bool has_dealloc = false;
    int  alloc_line = 0;
    std::string alloc_kind;

    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (line.find("malloc(") != std::string::npos ||
            line.find("calloc(") != std::string::npos ||
            line.find("realloc(") != std::string::npos) {
            has_alloc = true;
            alloc_line = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
            alloc_kind = "malloc/calloc/realloc";
        }
        if (line.find("new ") != std::string::npos ||
            line.find("new[") != std::string::npos ||
            line.find("new(") != std::string::npos) {
            has_alloc = true;
            alloc_line = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
            alloc_kind = "new";
        }
        if (line.find("free(") != std::string::npos) has_dealloc = true;
        if (line.find("delete ") != std::string::npos ||
            line.find("delete[]") != std::string::npos) has_dealloc = true;
    }

    if (has_alloc && !has_dealloc) {
        hits.push_back({"unpaired_alloc", Severity::Error, diff.filename,
                        alloc_line,
                        "检测到 " + alloc_kind + " 但未发现对应的 free/delete，可能存在内存泄漏",
                        "确保在分配后添加对应的 free/delete，或使用智能指针管理生命周期"});
    }
}

void RuleEngine::check_virtual_modify(const FileDiff& diff,
                                      std::vector<RuleHit>& hits) {
    for (size_t i = 0; i < diff.removed_lines.size(); ++i) {
        const auto& line = diff.removed_lines[i];
        if (line.find("override") != std::string::npos ||
            line.find("virtual") != std::string::npos) {
            int ln = diff.removed_line_nums.empty() ? 0 : diff.removed_line_nums[i];
            hits.push_back({"virtual_modify", Severity::Error, diff.filename,
                            ln,
                            "修改了虚函数/override 方法，可能影响多态行为和派生类",
                            "审查所有派生类是否受影响，确认多态行为未被破坏"});
            break;
        }
    }
}

void RuleEngine::check_format_only(const FileDiff& diff,
                                   std::vector<RuleHit>& hits) {
    if (diff.added_lines.empty() && diff.removed_lines.empty()) return;

    auto is_whitespace_only = [](const std::string& s) -> bool {
        return std::all_of(s.begin(), s.end(), [](char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        });
    };

    bool all_whitespace = true;
    for (const auto& line : diff.added_lines) {
        if (!is_whitespace_only(line)) { all_whitespace = false; break; }
    }
    if (all_whitespace) {
        for (const auto& line : diff.removed_lines) {
            if (!is_whitespace_only(line)) { all_whitespace = false; break; }
        }
    }

    if (all_whitespace && !diff.added_lines.empty()) {
        hits.push_back({"format_only", Severity::Info, diff.filename,
                        diff.added_line_nums.empty() ? 0 : diff.added_line_nums[0],
                        "纯格式化变更（仅空格/缩进），无逻辑影响",
                        ""});
    }
}

void RuleEngine::check_test_file(const FileDiff& diff,
                                 std::vector<RuleHit>& hits) {
    const auto& name = diff.filename;
    bool is_test = name.find("test") != std::string::npos ||
                   name.find("Test") != std::string::npos ||
                   name.find("_test.") != std::string::npos ||
                   name.find("_unittest.") != std::string::npos ||
                   name.find("/tests/") != std::string::npos;

    if (is_test) {
        hits.push_back({"test_file", Severity::Info, diff.filename, 0,
                        "修改了测试文件，风险较低",
                        ""});
    }
}

void RuleEngine::check_todo_fixme(const FileDiff& diff,
                                  std::vector<RuleHit>& hits) {
    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (line.find("TODO") != std::string::npos ||
            line.find("FIXME") != std::string::npos ||
            line.find("HACK") != std::string::npos) {
            int ln = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
            hits.push_back({"todo_fixme", Severity::Warn, diff.filename,
                            ln,
                            "新增 TODO/FIXME/HACK 标记，表明存在待处理问题",
                            "处理标记后再合入主分支"});
        }
    }
}

void RuleEngine::check_raw_ptr_new(const FileDiff& diff,
                                   std::vector<RuleHit>& hits) {
    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (line.find("= new ") != std::string::npos ||
            line.find("=new ") != std::string::npos) {
            if (line.find("unique_ptr") == std::string::npos &&
                line.find("shared_ptr") == std::string::npos &&
                line.find("make_unique") == std::string::npos &&
                line.find("make_shared") == std::string::npos) {
                int ln = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
                hits.push_back({"raw_ptr_new", Severity::Warn, diff.filename,
                                ln,
                                "使用裸指针 new，建议改用智能指针",
                                "改用 std::unique_ptr 或 std::make_unique"});
            }
        }
    }
}

void RuleEngine::check_c_style_cast(const FileDiff& diff,
                                    std::vector<RuleHit>& hits) {
    static const std::regex re(R"(\(\s*[A-Za-z_][A-Za-z0-9_:]*\s*\**\s*\))");
    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (std::regex_search(line, re)) {
            if (line.find("return") == std::string::npos &&
                line.find("delete") == std::string::npos &&
                line.find("sizeof(") == std::string::npos &&
                line.find("static_cast") == std::string::npos &&
                line.find("reinterpret_cast") == std::string::npos &&
                line.find("const_cast") == std::string::npos &&
                line.find("dynamic_cast") == std::string::npos) {
                int ln = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
                hits.push_back({"c_style_cast", Severity::Info, diff.filename,
                                ln,
                                "使用 C 风格强制转换，建议改用 C++ cast",
                                "改用 static_cast / const_cast / reinterpret_cast"});
            }
        }
    }
}

void RuleEngine::check_memcpy_size(const FileDiff& diff,
                                   std::vector<RuleHit>& hits) {
    static const std::regex mem_func_re(R"((memcpy|memset|memmove)\s*\()");
    static const std::regex sizeof_var_re(R"(sizeof\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\))");
    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (std::regex_search(line, mem_func_re) &&
            std::regex_search(line, sizeof_var_re)) {
            static const std::regex sizeof_type_re(R"(sizeof\s*\(\s*(int|char|long|short|float|double|void|unsigned|uint\d+_t|int\d+_t|size_t|ssize_t))");
            if (!std::regex_search(line, sizeof_type_re)) {
                int ln = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
                hits.push_back({"memcpy_size", Severity::Error, diff.filename,
                                ln,
                                "memcpy/memset 的 sizeof 参数可能是指针而非数组",
                                "使用 sizeof(数组) 替代 sizeof(指针)，或显式传入缓冲区大小"});
            }
        }
    }
}

}  // namespace aipr::rule
