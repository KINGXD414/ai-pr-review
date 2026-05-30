/**
 * @file test_rule_engine.cpp
 * @brief Unit tests for aipr::rule::RuleEngine.
 *
 * Updated for Issue #1 architecture:
 * - RuleHit fields: rule_id / severity / file / line / message / suggestion / confidence
 * - Engine::run(ParsedDiff, AstSummary*) interface
 * - Engine::scan() legacy interface still works
 */
#include "rules/engine.h"

#include <gtest/gtest.h>
#include <set>

using aipr::rule::FileDiff;
using aipr::rule::ParsedDiff;
using aipr::rule::RuleEngine;
using aipr::rule::RuleHit;
using aipr::rule::Severity;

// ---- Helper: build a ParsedDiff from a single FileDiff ----
static ParsedDiff make_pd(const FileDiff& fd) {
    ParsedDiff pd;
    pd.files.push_back(fd);
    return pd;
}

// ---- Severity mapping tests ----

TEST(RuleEngine, SeverityMappingInfo) {
    RuleHit h;
    h.severity = Severity::Info;
    EXPECT_STREQ(aipr::rule::severity_str(h.severity), "info");
}

TEST(RuleEngine, SeverityMappingWarn) {
    RuleHit h;
    h.severity = Severity::Warn;
    EXPECT_STREQ(aipr::rule::severity_str(h.severity), "warn");
}

TEST(RuleEngine, SeverityMappingError) {
    RuleHit h;
    h.severity = Severity::Error;
    EXPECT_STREQ(aipr::rule::severity_str(h.severity), "error");
}

// ---- Rule 1: unpaired_alloc ----

TEST(RuleEngine, DetectsMallocWithoutFree) {
    FileDiff diff;
    diff.filename = "src/main.cpp";
    diff.added_lines = {"void* buf = malloc(1024);"};
    diff.added_line_nums = {10};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "unpaired_alloc") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Error);
            EXPECT_FALSE(h.suggestion.empty());
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, DetectsNewWithoutDelete) {
    FileDiff diff;
    diff.filename = "src/handler.cpp";
    diff.added_lines = {"auto* obj = new Handler();"};
    diff.added_line_nums = {42};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "unpaired_alloc") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Error);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, NoHitWhenMallocAndFreePaired) {
    FileDiff diff;
    diff.filename = "src/buf.cpp";
    diff.added_lines = {
        "char* buf = (char*)malloc(1024);",
        "free(buf);"
    };
    diff.added_line_nums = {5, 6};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    for (const auto& h : hits) {
        EXPECT_NE(h.rule_id, "unpaired_alloc");
    }
}

// ---- Rule 2: virtual_modify ----

TEST(RuleEngine, DetectsOverrideModification) {
    FileDiff diff;
    diff.filename = "src/derived.cpp";
    diff.removed_lines = {"void run() override {"};
    diff.removed_line_nums = {20};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "virtual_modify") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Error);
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ---- Rule 3: format_only ----

TEST(RuleEngine, DetectsFormatOnlyChange) {
    FileDiff diff;
    diff.filename = "src/utils.cpp";
    diff.added_lines = {"    ", "\t"};
    diff.added_line_nums = {1, 2};
    diff.removed_lines = {""};
    diff.removed_line_nums = {1};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "format_only") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Info);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, NoFormatOnlyForCodeChange) {
    FileDiff diff;
    diff.filename = "src/utils.cpp";
    diff.added_lines = {"int x = 1;"};
    diff.added_line_nums = {5};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    for (const auto& h : hits) {
        EXPECT_NE(h.rule_id, "format_only");
    }
}

// ---- Rule 4: test_file ----

TEST(RuleEngine, DetectsTestFile) {
    FileDiff diff;
    diff.filename = "src/tests/main_test.cpp";
    diff.added_lines = {"EXPECT_EQ(1, 1);"};
    diff.added_line_nums = {10};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "test_file") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Info);
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ---- Rule 5: todo_fixme ----

TEST(RuleEngine, DetectsTODO) {
    FileDiff diff;
    diff.filename = "src/feature.cpp";
    diff.added_lines = {"// TODO: implement error handling"};
    diff.added_line_nums = {15};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "todo_fixme") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Warn);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, DetectsFIXME) {
    FileDiff diff;
    diff.filename = "src/bug.cpp";
    diff.added_lines = {"// FIXME: race condition here"};
    diff.added_line_nums = {30};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "todo_fixme") {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ---- Rule 6: raw_ptr_new ----

TEST(RuleEngine, DetectsRawPointerNew) {
    FileDiff diff;
    diff.filename = "src/service.cpp";
    diff.added_lines = {"auto* svc = new Service();"};
    diff.added_line_nums = {8};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "raw_ptr_new") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Warn);
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, NoHitForMakeUnique) {
    FileDiff diff;
    diff.filename = "src/service.cpp";
    diff.added_lines = {"auto svc = std::make_unique<Service>();"};
    diff.added_line_nums = {8};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    for (const auto& h : hits) {
        EXPECT_NE(h.rule_id, "raw_ptr_new");
    }
}

// ---- Rule 7: c_style_cast ----

TEST(RuleEngine, DetectsCStyleCast) {
    FileDiff diff;
    diff.filename = "src/cast.cpp";
    diff.added_lines = {"int* p = (int*)ptr;"};
    diff.added_line_nums = {12};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "c_style_cast") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Info);
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ---- Rule 8: memcpy_size ----

TEST(RuleEngine, DetectsMemcpyWithPointerSizeof) {
    FileDiff diff;
    diff.filename = "src/mem.cpp";
    diff.added_lines = {"memcpy(dst, src, sizeof(ptr));"};
    diff.added_line_nums = {25};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_id == "memcpy_size") {
            found = true;
            EXPECT_EQ(h.severity, Severity::Error);
            break;
        }
    }
    EXPECT_TRUE(found);
}

// ---- Multi-rule combination test ----

TEST(RuleEngine, MultipleRulesCanFireOnSameDiff) {
    FileDiff diff;
    diff.filename = "src/tests/handler_test.cpp";
    diff.added_lines = {
        "// TODO: add more tests",
        "auto* obj = new Handler();"
    };
    diff.added_line_nums = {5, 6};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    EXPECT_GE(hits.size(), 3u);

    std::set<std::string> rules;
    for (const auto& h : hits) {
        rules.insert(h.rule_id);
    }
    EXPECT_TRUE(rules.count("test_file"));
    EXPECT_TRUE(rules.count("todo_fixme"));
    EXPECT_TRUE(rules.count("raw_ptr_new") || rules.count("unpaired_alloc"));
}

// ---- Engine::run vs scan equivalence ----

TEST(RuleEngine, RunAndScanProduceSameResults) {
    FileDiff diff;
    diff.filename = "src/mixed.cpp";
    diff.added_lines = {"auto* p = new int(42);"};
    diff.added_line_nums = {10};

    RuleEngine engine;
    auto run_hits  = engine.run(make_pd(diff));
    auto scan_hits = engine.scan({diff});

    ASSERT_EQ(run_hits.size(), scan_hits.size());
    for (size_t i = 0; i < run_hits.size(); ++i) {
        EXPECT_EQ(run_hits[i].rule_id, scan_hits[i].rule_id);
        EXPECT_EQ(run_hits[i].severity, scan_hits[i].severity);
        EXPECT_EQ(run_hits[i].file, scan_hits[i].file);
        EXPECT_EQ(run_hits[i].line, scan_hits[i].line);
    }
}

// ---- Engine::run with nullptr AST (no crash) ----

TEST(RuleEngine, RunWithNullAstDoesNotCrash) {
    FileDiff diff;
    diff.filename = "src/main.cpp";
    diff.added_lines = {"void* buf = malloc(1024);"};
    diff.added_line_nums = {5};

    RuleEngine engine;
    ParsedDiff pd;
    pd.files.push_back(diff);

    EXPECT_NO_THROW(engine.run(pd, nullptr));
}

// ---- Empty input tests ----

TEST(RuleEngine, EmptyInputReturnsNoHits) {
    RuleEngine engine;
    ParsedDiff pd;
    auto hits = engine.run(pd);
    EXPECT_TRUE(hits.empty());
}

TEST(RuleEngine, EmptyDiffReturnsNoHits) {
    FileDiff diff;
    diff.filename = "src/empty.cpp";

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));
    EXPECT_TRUE(hits.empty());
}

// ---- RuleHit field completeness test ----

TEST(RuleEngine, RuleHitFieldsArePopulated) {
    FileDiff diff;
    diff.filename = "src/leak.cpp";
    diff.added_lines = {"void* buf = malloc(1024);"};
    diff.added_line_nums = {7};

    RuleEngine engine;
    auto hits = engine.run(make_pd(diff));

    ASSERT_FALSE(hits.empty());
    const auto& h = hits[0];
    EXPECT_FALSE(h.rule_id.empty());
    EXPECT_FALSE(h.file.empty());
    EXPECT_GT(h.line, 0);
    EXPECT_FALSE(h.message.empty());
    // suggestion may be empty for some rules, but unpaired_alloc has one
    if (h.rule_id == "unpaired_alloc") {
        EXPECT_FALSE(h.suggestion.empty());
    }
    // confidence defaults to 0.8 per ADR-003 Decision 1
    EXPECT_GT(h.confidence, 0.0);
    EXPECT_LE(h.confidence, 1.0);
}
