/**
 * @file ast_analyzer.cpp
 * @brief Implementation of the libclang AST extractor.
 *
 * Uses the stable C API exclusively:
 *   - clang_createIndex / clang_parseTranslationUnit
 *   - clang_visitChildren with a CXCursorVisitor
 *   - clang_getCursorSpelling / clang_getCursorKind / etc.
 *
 * Tested against LLVM 14 and 18.  The C API is ABI-stable so a single
 * .so suffices across that range.
 */
#include "analyzer/ast_analyzer.h"

#include <clang-c/Index.h>

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <unordered_set>

namespace aipr::analyzer {

// ---------------------------------------------------------------------------
// Internal helpers — wrapping the ugly CXString lifetime
// ---------------------------------------------------------------------------
namespace {

std::string cx_to_string(CXString s) {
    const char* c = clang_getCString(s);
    std::string r = c ? c : "";
    clang_disposeString(s);
    return r;
}

/// Walk up to find the nearest class/struct cursor and return its
/// qualified name.  Returns "" for free functions.
std::string parent_class_name(CXCursor cursor) {
    CXCursor parent = clang_getCursorSemanticParent(cursor);
    while (clang_isDeclaration(clang_getCursorKind(parent))) {
        auto kind = clang_getCursorKind(parent);
        if (kind == CXCursor_ClassDecl || kind == CXCursor_StructDecl) {
            return cx_to_string(clang_getCursorSpelling(parent));
        }
        parent = clang_getCursorSemanticParent(parent);
    }
    return "";
}

CXSourceLocation cursor_location(CXCursor c) {
    return clang_getCursorLocation(c);
}

std::pair<std::string, int> file_and_line(CXCursor c) {
    CXSourceLocation loc = cursor_location(c);
    CXFile file = nullptr;
    unsigned line = 0, col = 0, off = 0;
    clang_getFileLocation(loc, &file, &line, &col, &off);
    std::string fname = file ? cx_to_string(clang_getFileName(file)) : "";
    return {fname, static_cast<int>(line)};
}

// ---------------------------------------------------------------------------
// CXCursor visitor — fills vectors in the closure's AstSummary
// ---------------------------------------------------------------------------

struct VisitorData {
    AstSummary*        summary;
    std::string        target_file;  // only collect from this file
    CXTranslationUnit  tu;
};

CXChildVisitResult visit_cursor(CXCursor cursor, CXCursor /*parent*/,
                                CXClientData client_data) {
    auto* vd = static_cast<VisitorData*>(client_data);

    CXCursorKind kind = clang_getCursorKind(cursor);

    // --- Skip everything from included headers ---
    auto [file, line] = file_and_line(cursor);
    if (!file.empty() && file != vd->target_file) {
        // Recurse into children only for the main file's top-level nodes.
        return CXChildVisit_Continue;
    }

    switch (kind) {
    // ---- Function / method declarations ----
    case CXCursor_FunctionDecl:
    case CXCursor_CXXMethod:
    case CXCursor_Constructor:
    case CXCursor_Destructor: {
        FunctionInfo fi;
        fi.name     = cx_to_string(clang_getCursorSpelling(cursor));
        fi.file     = file;
        fi.line     = line;

        // Return type
        CXType result_type = clang_getCursorResultType(cursor);
        fi.return_type = cx_to_string(clang_getTypeSpelling(result_type));

        // Parameters
        int num_args = clang_Cursor_getNumArguments(cursor);
        for (int i = 0; i < num_args; ++i) {
            CXCursor arg = clang_Cursor_getArgument(cursor, i);
            CXType  arg_type = clang_getArgType(result_type, i);
            // If getArgType fails, fall back to the cursor's type
            if (arg_type.kind == CXType_Invalid) {
                arg_type = clang_getCursorType(arg);
            }
            std::string type_spelling = cx_to_string(clang_getTypeSpelling(arg_type));
            std::string arg_name      = cx_to_string(clang_getCursorSpelling(arg));
            if (arg_name.empty()) arg_name = "arg" + std::to_string(i);
            fi.params.push_back(type_spelling + " " + arg_name);
        }

        // Method-specific flags
        fi.is_method  = (kind == CXCursor_CXXMethod ||
                         kind == CXCursor_Constructor ||
                         kind == CXCursor_Destructor);
        fi.is_static  = clang_CXXMethod_isStatic(cursor) != 0;
        fi.is_virtual = clang_CXXMethod_isVirtual(cursor) != 0;

        // Parent class
        if (fi.is_method) {
            fi.parent_class = parent_class_name(cursor);
            fi.qualified = fi.parent_class + "::" + fi.name;
        } else {
            fi.qualified = fi.name;
        }

        vd->summary->functions.push_back(std::move(fi));
        break;
    }

    // ---- Class / struct declarations ----
    case CXCursor_ClassDecl:
    case CXCursor_StructDecl: {
        ClassInfo ci;
        ci.name  = cx_to_string(clang_getCursorSpelling(cursor));
        ci.kind  = (kind == CXCursor_ClassDecl) ? "class" : "struct";
        ci.file  = file;
        ci.line  = line;
        ci.qualified = cx_to_string(clang_getCursorUSR(cursor));

        // Count direct members & bases
        clang_visitChildren(cursor, [](CXCursor child, CXCursor /*p*/,
                                        CXClientData data) -> CXChildVisitResult {
            auto* info = static_cast<ClassInfo*>(data);
            CXCursorKind ck = clang_getCursorKind(child);
            if (ck == CXCursor_CXXBaseSpecifier) {
                info->bases.push_back(
                    cx_to_string(clang_getTypeSpelling(clang_getCursorType(child))));
            } else if (ck == CXCursor_CXXMethod ||
                       ck == CXCursor_Constructor ||
                       ck == CXCursor_Destructor) {
                ++info->method_count;
            } else if (ck == CXCursor_FieldDecl) {
                ++info->field_count;
            }
            return CXChildVisit_Continue;
        }, &ci);

        vd->summary->classes.push_back(std::move(ci));
        break;
    }

    // ---- #include directives ----
    case CXCursor_InclusionDirective: {
        CXFile inc_file = clang_getIncludedFile(cursor);
        if (inc_file) {
            std::string inc_name = cx_to_string(clang_getFileName(inc_file));
            vd->summary->includes.push_back(std::move(inc_name));
        }
        break;
    }

    default:
        break;
    }

    return CXChildVisit_Recurse;
}

}  // anonymous namespace

// ---------------------------------------------------------------------------
// AstAnalyzer public API
// ---------------------------------------------------------------------------

AstAnalyzer::AstAnalyzer()
    : index_(clang_createIndex(/*excludeDeclsFromPCH=*/0,
                               /*displayDiagnostics=*/0)) {}

AstAnalyzer::~AstAnalyzer() {
    if (index_) {
        clang_disposeIndex(static_cast<CXIndex>(index_));
    }
}

AstSummary AstAnalyzer::analyze_file(const std::string& path,
                                     const std::vector<std::string>& extra_args) {
    AstSummary result;
    result.source_file = path;

    // Build command-line args for libclang.
    // We default to C++17 with common system include paths.
    std::vector<const char*> cargs;
    cargs.push_back("-x");
    cargs.push_back("c++");
    cargs.push_back("-std=c++17");

    // System include paths come from extra_args (CMake fills them via
    // llvm-config / find_package(Clang) at build time).

    // Insert user-provided extra args.
    std::vector<std::string> owned_args = extra_args;
    for (auto& a : owned_args) {
        cargs.push_back(a.c_str());
    }

    // Parse the file.
    CXTranslationUnit tu = nullptr;
    CXErrorCode ec = clang_parseTranslationUnit2(
        static_cast<CXIndex>(index_),
        path.c_str(),
        cargs.data(), static_cast<int>(cargs.size()),
        nullptr, 0,  // unsaved files
        CXTranslationUnit_DetailedPreprocessingRecord,
        &tu);

    if (ec != CXError_Success || !tu) {
        result.ok = false;
        result.error = "libclang parse failed, error code: " + std::to_string(ec);
        spdlog::warn("ASTAnalyzer: {}", result.error);
        return result;
    }

    // Collect diagnostics (first 10).
    unsigned num_diag = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < num_diag && i < 10; ++i) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        result.diagnostics.push_back(
            cx_to_string(clang_formatDiagnostic(diag,
                CXDiagnostic_DisplaySourceLocation |
                CXDiagnostic_DisplayColumn |
                CXDiagnostic_DisplayOption)));
        clang_disposeDiagnostic(diag);
    }

    // Normalize the target file path for comparison inside visitor.
    // libclang may canonicalize; we read back from the TU's file handle.
    CXFile main_file = clang_getFile(tu, path.c_str());
    std::string canonical = main_file
        ? cx_to_string(clang_getFileName(main_file))
        : path;

    // Visit the AST.
    VisitorData vd{&result, canonical, tu};
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visit_cursor, &vd);

    clang_disposeTranslationUnit(tu);

    result.ok = true;
    spdlog::debug("ASTAnalyzer: {} → {} funcs, {} classes, {} includes",
                  path, result.functions.size(),
                  result.classes.size(), result.includes.size());
    return result;
}

// ---------------------------------------------------------------------------
// AstSummary::to_prompt — compact Markdown for the LLM
// ---------------------------------------------------------------------------

std::string AstSummary::to_prompt() const {
    if (!ok) return "";

    std::ostringstream oss;

    oss << "### AST 上下文：`" << source_file << "`\n";

    if (!includes.empty()) {
        oss << "**includes**：";
        for (size_t i = 0; i < std::min(includes.size(), size_t(20)); ++i) {
            // Only show the basename to keep it short
            std::string base = includes[i];
            auto slash = base.rfind('/');
            if (slash != std::string::npos) base = base.substr(slash + 1);
            oss << "`" << base << "`";
            if (i + 1 < includes.size() && i + 1 < 20) oss << ", ";
        }
        if (includes.size() > 20) oss << ", …";
        oss << "\n";
    }

    if (!classes.empty()) {
        oss << "**类型定义**：\n";
        for (const auto& c : classes) {
            oss << "- `" << c.kind << " " << c.name << "`";
            if (!c.bases.empty()) {
                oss << " : " ;
                for (size_t i = 0; i < c.bases.size(); ++i) {
                    oss << c.bases[i];
                    if (i + 1 < c.bases.size()) oss << ", ";
                }
            }
            oss << " (" << c.method_count << " methods, "
                << c.field_count << " fields, line " << c.line << ")\n";
        }
    }

    if (!functions.empty()) {
        oss << "**函数签名**：\n";
        for (const auto& f : functions) {
            oss << "- ";
            if (f.is_method) {
                oss << f.parent_class << "::";
            }
            oss << f.name << "(";
            for (size_t i = 0; i < f.params.size(); ++i) {
                oss << f.params[i];
                if (i + 1 < f.params.size()) oss << ", ";
            }
            oss << ")";
            if (!f.return_type.empty() && f.return_type != "void") {
                oss << " → " << f.return_type;
            }
            oss << "  [line " << f.line << "]";
            if (f.is_virtual) oss << " virtual";
            if (f.is_static)  oss << " static";
            oss << "\n";
        }
    }

    return oss.str();
}

}  // namespace aipr::analyzer
