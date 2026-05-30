/**
 * @file test_rule_engine.cpp
 * @brief Unit tests for aipr::rule::RuleEngine.
 */
#include "rule/engine.h"

#include <gtest/gtest.h>
#include <set>

using aipr::rule::FileDiff;
using aipr::rule::RuleEngine;
using aipr::rule::RuleHit;

// ---- 规则 1：unpaired_alloc ----

TEST(RuleEngine, DetectsMallocWithoutFree) {
    FileDiff diff;
    diff.filename = "src/main.cpp";
    diff.added_lines = {"void* buf = malloc(1024);"};
    diff.added_line_nums = {10};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "unpaired_alloc") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, DetectsNewWithoutDelete) {
    FileDiff diff;
    diff.filename = "src/handler.cpp";
    diff.added_lines = {"auto* obj = new Handler();"};
    diff.added_line_nums = {42};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "unpaired_alloc") { found = true; break; }
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
    auto hits = engine.scan({diff});

    for (const auto& h : hits) {
        EXPECT_NE(h.rule_name, "unpaired_alloc");
    }
}

// ---- 规则 2：virtual_modify ----

TEST(RuleEngine, DetectsOverrideModification) {
    FileDiff diff;
    diff.filename = "src/derived.cpp";
    diff.removed_lines = {"void run() override {"};
    diff.removed_line_nums = {20};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "virtual_modify") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ---- 规则 3：format_only ----

TEST(RuleEngine, DetectsFormatOnlyChange) {
    FileDiff diff;
    diff.filename = "src/utils.cpp";
    diff.added_lines = {"    ", "\t"};
    diff.added_line_nums = {1, 2};
    diff.removed_lines = {""};
    diff.removed_line_nums = {1};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "format_only") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, NoFormatOnlyForCodeChange) {
    FileDiff diff;
    diff.filename = "src/utils.cpp";
    diff.added_lines = {"int x = 1;"};
    diff.added_line_nums = {5};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    for (const auto& h : hits) {
        EXPECT_NE(h.rule_name, "format_only");
    }
}

// ---- 规则 4：test_file ----

TEST(RuleEngine, DetectsTestFile) {
    FileDiff diff;
    diff.filename = "src/tests/main_test.cpp";
    diff.added_lines = {"EXPECT_EQ(1, 1);"};
    diff.added_line_nums = {10};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "test_file") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ---- 规则 5：todo_fixme ----

TEST(RuleEngine, DetectsTODO) {
    FileDiff diff;
    diff.filename = "src/feature.cpp";
    diff.added_lines = {"// TODO: implement error handling"};
    diff.added_line_nums = {15};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "todo_fixme") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, DetectsFIXME) {
    FileDiff diff;
    diff.filename = "src/bug.cpp";
    diff.added_lines = {"// FIXME: race condition here"};
    diff.added_line_nums = {30};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "todo_fixme") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ---- 规则 6：raw_ptr_new ----

TEST(RuleEngine, DetectsRawPointerNew) {
    FileDiff diff;
    diff.filename = "src/service.cpp";
    diff.added_lines = {"auto* svc = new Service();"};
    diff.added_line_nums = {8};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "raw_ptr_new") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

TEST(RuleEngine, NoHitForMakeUnique) {
    FileDiff diff;
    diff.filename = "src/service.cpp";
    diff.added_lines = {"auto svc = std::make_unique<Service>();"};
    diff.added_line_nums = {8};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    for (const auto& h : hits) {
        EXPECT_NE(h.rule_name, "raw_ptr_new");
    }
}

// ---- 规则 7：c_style_cast ----

TEST(RuleEngine, DetectsCStyleCast) {
    FileDiff diff;
    diff.filename = "src/cast.cpp";
    diff.added_lines = {"int* p = (int*)ptr;"};
    diff.added_line_nums = {12};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "c_style_cast") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ---- 规则 8：memcpy_size ----

TEST(RuleEngine, DetectsMemcpyWithPointerSizeof) {
    FileDiff diff;
    diff.filename = "src/mem.cpp";
    diff.added_lines = {"memcpy(dst, src, sizeof(ptr));"};
    diff.added_line_nums = {25};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    bool found = false;
    for (const auto& h : hits) {
        if (h.rule_name == "memcpy_size") { found = true; break; }
    }
    EXPECT_TRUE(found);
}

// ---- 多规则组合测试 ----

TEST(RuleEngine, MultipleRulesCanFireOnSameDiff) {
    FileDiff diff;
    diff.filename = "src/tests/handler_test.cpp";
    diff.added_lines = {
        "// TODO: add more tests",
        "auto* obj = new Handler();"
    };
    diff.added_line_nums = {5, 6};

    RuleEngine engine;
    auto hits = engine.scan({diff});

    EXPECT_GE(hits.size(), 3u);

    std::set<std::string> rules;
    for (const auto& h : hits) {
        rules.insert(h.rule_name);
    }
    EXPECT_TRUE(rules.count("test_file"));
    EXPECT_TRUE(rules.count("todo_fixme"));
    EXPECT_TRUE(rules.count("raw_ptr_new") || rules.count("unpaired_alloc"));
}

// ---- 空输入测试 ----

TEST(RuleEngine, EmptyInputReturnsNoHits) {
    RuleEngine engine;
    auto hits = engine.scan({});
    EXPECT_TRUE(hits.empty());
}

TEST(RuleEngine, EmptyDiffReturnsNoHits) {
    FileDiff diff;
    diff.filename = "src/empty.cpp";

    RuleEngine engine;
    auto hits = engine.scan({diff});
    EXPECT_TRUE(hits.empty());
}
