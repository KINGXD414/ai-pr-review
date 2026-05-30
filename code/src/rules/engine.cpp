#include "rules/engine.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>
#include <vector>

namespace aipr::rule {

std::vector<RuleHit> RuleEngine::scan(const std::vector<FileDiff>& diffs) {
    std::vector<RuleHit> hits;
    for (const auto& diff : diffs) {
        check_unpaired_alloc(diff, hits);
        check_virtual_modify(diff, hits);
        check_format_only(diff, hits);
        check_test_file(diff, hits);
        check_todo_fixme(diff, hits);
        check_raw_ptr_new(diff, hits);
        check_c_style_cast(diff, hits);
        check_memcpy_size(diff, hits);
    }
    return hits;
}

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
        hits.push_back({diff.filename, alloc_line, "high",
                        "unpaired_alloc",
                        "检测到 " + alloc_kind + " 但未发现对应的 free/delete，可能存在内存泄漏"});
    }
}

void RuleEngine::check_virtual_modify(const FileDiff& diff,
                                      std::vector<RuleHit>& hits) {
    for (size_t i = 0; i < diff.removed_lines.size(); ++i) {
        const auto& line = diff.removed_lines[i];
        if (line.find("override") != std::string::npos ||
            line.find("virtual") != std::string::npos) {
            int ln = diff.removed_line_nums.empty() ? 0 : diff.removed_line_nums[i];
            hits.push_back({diff.filename, ln, "high",
                            "virtual_modify",
                            "修改了虚函数/override 方法，可能影响多态行为和派生类"});
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
        hits.push_back({diff.filename, diff.added_line_nums.empty() ? 0 : diff.added_line_nums[0],
                        "low", "format_only",
                        "纯格式化变更（仅空格/缩进），无逻辑影响"});
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
        hits.push_back({diff.filename, 0, "low", "test_file",
                        "修改了测试文件，风险较低"});
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
            hits.push_back({diff.filename, ln, "medium", "todo_fixme",
                            "新增 TODO/FIXME/HACK 标记，表明存在待处理问题"});
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
                hits.push_back({diff.filename, ln, "medium", "raw_ptr_new",
                                "使用裸指针 new，建议改用 std::unique_ptr 或 std::make_unique"});
            }
        }
    }
}

void RuleEngine::check_c_style_cast(const FileDiff& diff,
                                    std::vector<RuleHit>& hits) {
    // 匹配 (Type) 或 (Type*) 模式的 C 风格转换
    static const std::regex re(R"(\(\s*[A-Za-z_][A-Za-z0-9_:]*\s*\**\s*\))");
    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (std::regex_search(line, re)) {
            // 排除常见合法用法
            if (line.find("return") == std::string::npos &&
                line.find("delete") == std::string::npos &&
                line.find("sizeof(") == std::string::npos &&
                line.find("static_cast") == std::string::npos &&
                line.find("reinterpret_cast") == std::string::npos &&
                line.find("const_cast") == std::string::npos &&
                line.find("dynamic_cast") == std::string::npos) {
                int ln = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
                hits.push_back({diff.filename, ln, "low", "c_style_cast",
                                "使用 C 风格强制转换，建议改用 static_cast/const_cast 等"});
            }
        }
    }
}

void RuleEngine::check_memcpy_size(const FileDiff& diff,
                                   std::vector<RuleHit>& hits) {
    // 检测 memcpy/memset/memmove 中使用 sizeof(变量) 的情况
    // sizeof(指针变量) 只返回指针大小而非缓冲区大小，是常见 bug
    static const std::regex mem_func_re(R"((memcpy|memset|memmove)\s*\()");
    static const std::regex sizeof_var_re(R"(sizeof\s*\(\s*[A-Za-z_][A-Za-z0-9_]*\s*\))");
    for (size_t i = 0; i < diff.added_lines.size(); ++i) {
        const auto& line = diff.added_lines[i];
        if (std::regex_search(line, mem_func_re) &&
            std::regex_search(line, sizeof_var_re)) {
            // 排除 sizeof(数组) 的合法用法：sizeof(buf) 中 buf 名字含 "size" / "len" / "buf" / "array"
            // 或者 sizeof 是带类型的如 sizeof(int)
            static const std::regex sizeof_type_re(R"(sizeof\s*\(\s*(int|char|long|short|float|double|void|unsigned|uint\d+_t|int\d+_t|size_t|ssize_t))");
            if (!std::regex_search(line, sizeof_type_re)) {
                int ln = diff.added_line_nums.empty() ? 0 : diff.added_line_nums[i];
                hits.push_back({diff.filename, ln, "high", "memcpy_size",
                                "memcpy/memset 的 sizeof 参数可能是指针而非数组，sizeof(指针)只返回指针大小"});
            }
        }
    }
}

}  // namespace aipr::rule
