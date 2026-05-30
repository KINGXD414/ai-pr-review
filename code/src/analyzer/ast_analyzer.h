/**
 * @file ast_analyzer.h
 * @brief libclang-based C/C++ AST extractor for PR review context.
 *
 * Day-2 goal: when the LLM reviews a PR diff, hand it more than just the
 * raw `+`/`-` lines — also tell it which functions / classes / includes
 * actually live in the touched files, so it can reason about side-effects.
 *
 * This module wraps the libclang C API (`<clang-c/Index.h>`) so the rest
 * of the project never has to touch CXCursor / CXTranslationUnit directly.
 * We pick the C API on purpose: it's ABI-stable across LLVM 14..18, ships
 * as a single `.so`, and avoids dragging LLVM C++ template guts into our
 * build.
 *
 * Reference: design document §4.4 (originally rule-engine, repurposed
 * to "AST context" per Day-2 plan).
 */
#pragma once

#include <string>
#include <vector>

namespace aipr::analyzer {

/// Pretty-printed signature of a function or method we found in the AST.
struct FunctionInfo {
    std::string name;          ///< Bare name, e.g. "fetch_pull_request".
    std::string qualified;     ///< Fully qualified, e.g. "aipr::github::APIClient::fetch_pull_request".
    std::string return_type;   ///< Spelled return type, e.g. "std::optional<PullRequest>".
    std::vector<std::string> params;  ///< Each entry is "<type> <name>".
    std::string file;          ///< Absolute path of the defining source file.
    int         line{0};       ///< 1-based line in the source file.
    bool        is_method{false};
    bool        is_static{false};
    bool        is_virtual{false};
    std::string parent_class;  ///< Empty when not a method.
};

/// Lightweight class/struct descriptor.  We deliberately do NOT carry
/// every method — methods live in the flat FunctionInfo vector so the
/// LLM can reason about them without nested traversal.
struct ClassInfo {
    std::string name;          ///< Bare name, e.g. "APIClient".
    std::string qualified;     ///< Fully qualified, e.g. "aipr::github::APIClient".
    std::string kind;          ///< "class" / "struct".
    std::string file;
    int         line{0};
    std::vector<std::string> bases;  ///< Spelled base-class names.
    int         method_count{0};
    int         field_count{0};
};

/// Aggregated AST summary for one translation unit.
struct AstSummary {
    std::string source_file;
    std::vector<std::string>  includes;   ///< Spelled paths from `#include` lines.
    std::vector<ClassInfo>    classes;
    std::vector<FunctionInfo> functions;

    bool        ok{false};
    std::string error;          ///< Non-empty when ok == false.
    std::vector<std::string> diagnostics;  ///< Compiler warnings/errors (first N).

    /// Render a compact Markdown block suitable for splicing into the
    /// LLM prompt.  Empty when ok == false.
    std::string to_prompt() const;
};

/**
 * Stateless façade around libclang.  Owns a single CXIndex (cheap to keep
 * alive for the lifetime of the process) and parses one file per call.
 *
 * Thread-safety: NOT thread-safe.  Wrap in a mutex if you need to call
 * `analyze_file` from multiple threads.
 */
class AstAnalyzer {
public:
    AstAnalyzer();
    ~AstAnalyzer();

    AstAnalyzer(const AstAnalyzer&)            = delete;
    AstAnalyzer& operator=(const AstAnalyzer&) = delete;

    /**
     * Parse one .cpp / .h / .hpp file and return its AST summary.
     *
     * @param path        File to parse.  Must exist on disk.
     * @param extra_args  Extra command-line flags to forward to libclang,
     *                    e.g. {"-std=c++17", "-I/some/include"}.  We add
     *                    sensible defaults on top.
     */
    AstSummary analyze_file(const std::string& path,
                            const std::vector<std::string>& extra_args = {});

private:
    void* index_;  ///< Opaque CXIndex handle.
};

}  // namespace aipr::analyzer
