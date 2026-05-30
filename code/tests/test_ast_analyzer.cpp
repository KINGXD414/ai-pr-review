// GoogleTest suite for the libclang AST extractor.
#include "analyzer/ast_analyzer.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>

namespace {

// The fixture lives under tests/fixtures/.  CMake's gtest_discover_tests
// runs the test binary from CMAKE_CURRENT_BINARY_DIR which is code/build/
// so we resolve the fixture path relative to CMAKE_SOURCE_DIR via the
// AIPR_TESTS_FIXTURES_DIR define injected from CMakeLists.txt.
const std::string kFixtureDir = AIPR_TESTS_FIXTURES_DIR;

aipr::analyzer::AstSummary parse_sample() {
    aipr::analyzer::AstAnalyzer az;
    return az.analyze_file(kFixtureDir + "/sample.hpp", {"-std=c++17"});
}

}  // namespace

TEST(AstAnalyzer, ParsesSampleHeaderSuccessfully) {
    auto s = parse_sample();
    EXPECT_TRUE(s.ok) << s.error;
    EXPECT_FALSE(s.source_file.empty());
}

TEST(AstAnalyzer, FindsStructAndClasses) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);

    // demo::Point (struct), demo::Shape (class), demo::Circle (class)
    EXPECT_GE(s.classes.size(), 3u);

    auto has = [&](const std::string& name, const std::string& kind) {
        return std::any_of(s.classes.begin(), s.classes.end(),
            [&](const auto& c) { return c.name == name && c.kind == kind; });
    };
    EXPECT_TRUE(has("Point",  "struct"));
    EXPECT_TRUE(has("Shape",  "class"));
    EXPECT_TRUE(has("Circle", "class"));
}

TEST(AstAnalyzer, CapturesInheritance) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);
    auto it = std::find_if(s.classes.begin(), s.classes.end(),
        [](const auto& c) { return c.name == "Circle"; });
    ASSERT_NE(it, s.classes.end());
    ASSERT_FALSE(it->bases.empty());
    EXPECT_NE(it->bases.front().find("Shape"), std::string::npos);
}

TEST(AstAnalyzer, FindsFreeFunctions) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);

    auto find_free = [&](const std::string& name) {
        return std::any_of(s.functions.begin(), s.functions.end(),
            [&](const auto& f) { return f.name == name && !f.is_method; });
    };
    EXPECT_TRUE(find_free("add"));
    EXPECT_TRUE(find_free("greet"));
}

TEST(AstAnalyzer, FlagsVirtualAndStaticMethods) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);

    bool found_virtual = false;
    bool found_static  = false;
    for (const auto& f : s.functions) {
        if (f.name == "area" && f.is_virtual) found_virtual = true;
        if (f.name == "total_count" && f.is_static) found_static = true;
    }
    EXPECT_TRUE(found_virtual);
    EXPECT_TRUE(found_static);
}

TEST(AstAnalyzer, CapturesParameters) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);
    // demo::greet(const std::string& name, int times)
    auto it = std::find_if(s.functions.begin(), s.functions.end(),
        [](const auto& f) { return f.name == "greet"; });
    ASSERT_NE(it, s.functions.end());
    ASSERT_EQ(it->params.size(), 2u);
    EXPECT_NE(it->params[0].find("string"), std::string::npos);
    EXPECT_EQ(it->return_type.find("string"),
              it->return_type.find("string"));  // exists
    EXPECT_NE(it->return_type.find("string"), std::string::npos);
}

TEST(AstAnalyzer, CollectsIncludes) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);
    bool found = false;
    for (const auto& inc : s.includes) {
        if (inc.find("string") != std::string::npos) found = true;
    }
    EXPECT_TRUE(found);
}

TEST(AstAnalyzer, ToPromptIsNonEmptyAndContainsMarkers) {
    auto s = parse_sample();
    ASSERT_TRUE(s.ok);
    const std::string md = s.to_prompt();
    EXPECT_FALSE(md.empty());
    EXPECT_NE(md.find("AST 上下文"), std::string::npos);
    EXPECT_NE(md.find("函数签名"), std::string::npos);
    EXPECT_NE(md.find("类型定义"), std::string::npos);
}

TEST(AstAnalyzer, FailsGracefullyOnMissingFile) {
    aipr::analyzer::AstAnalyzer az;
    auto s = az.analyze_file("/this/file/does/not/exist.cpp");
    // libclang returns success for non-existent files (creates empty TU)
    // OR returns failure depending on version; either way we should never
    // crash and should never spuriously claim we found functions.
    EXPECT_TRUE(s.functions.empty());
    EXPECT_TRUE(s.classes.empty());
}
