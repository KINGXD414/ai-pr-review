/**
 * @file test_diff_parser.cpp
 * @brief Unit tests for aipr::diff::parse_patch.
 *
 * Coverage targets:
 *   - empty input
 *   - single hunk with counts omitted
 *   - single hunk with explicit counts + trailing function name
 *   - multi-hunk patch
 *   - malformed header is skipped while the next valid hunk still parses
 *   - "\ No newline at end of file" sentinel is dropped
 *   - empty context line (no leading space) is preserved
 *   - CRLF input is normalised
 *   - parse_pr_url accepts the canonical and tolerated shapes
 */
#include "diff/parser.h"
#include "github/api_client.h"

#include <gtest/gtest.h>

using aipr::diff::DiffLine;
using aipr::diff::parse_patch;

TEST(DiffParser, EmptyPatchReturnsEmpty) {
    EXPECT_TRUE(parse_patch("").empty());
}

TEST(DiffParser, SingleHunkOmittedCounts) {
    // "@@ -1 +1 @@" — counts default to 1 each.
    const std::string patch =
        "@@ -1 +1 @@\n"
        "-int x = 0;\n"
        "+int x = 1;\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 1u);
    EXPECT_EQ(hunks[0].old_start, 1);
    EXPECT_EQ(hunks[0].old_count, 1);
    EXPECT_EQ(hunks[0].new_start, 1);
    EXPECT_EQ(hunks[0].new_count, 1);
    ASSERT_EQ(hunks[0].lines.size(), 2u);
    EXPECT_EQ(hunks[0].lines[0].type, DiffLine::Type::Del);
    EXPECT_EQ(hunks[0].lines[0].content, "int x = 0;");
    EXPECT_EQ(hunks[0].lines[1].type, DiffLine::Type::Add);
    EXPECT_EQ(hunks[0].lines[1].content, "int x = 1;");
}

TEST(DiffParser, SingleHunkExplicitCountsAndTrailer) {
    const std::string patch =
        "@@ -10,3 +10,4 @@ void foo()\n"
        " ctx0\n"
        "-old1\n"
        "+new1\n"
        "+new2\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 1u);
    EXPECT_EQ(hunks[0].old_start, 10);
    EXPECT_EQ(hunks[0].old_count, 3);
    EXPECT_EQ(hunks[0].new_start, 10);
    EXPECT_EQ(hunks[0].new_count, 4);
    EXPECT_EQ(hunks[0].header, "void foo()");
    ASSERT_EQ(hunks[0].lines.size(), 4u);
    EXPECT_EQ(hunks[0].lines[0].type, DiffLine::Type::Context);
    EXPECT_EQ(hunks[0].lines[0].content, "ctx0");
    EXPECT_EQ(hunks[0].lines[2].type, DiffLine::Type::Add);
    EXPECT_EQ(hunks[0].lines[2].content, "new1");
}

TEST(DiffParser, MultiHunkPatch) {
    const std::string patch =
        "@@ -1,2 +1,3 @@\n"
        " a\n"
        "+b\n"
        "@@ -100,1 +101,1 @@ class Foo\n"
        "-x\n"
        "+y\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 2u);
    EXPECT_EQ(hunks[0].new_count, 3);
    EXPECT_EQ(hunks[1].old_start, 100);
    EXPECT_EQ(hunks[1].new_start, 101);
    EXPECT_EQ(hunks[1].header, "class Foo");
    ASSERT_EQ(hunks[1].lines.size(), 2u);
    EXPECT_EQ(hunks[1].lines[0].content, "x");
    EXPECT_EQ(hunks[1].lines[1].content, "y");
}

TEST(DiffParser, MalformedHeaderIsSkippedAndLaterHunkParses) {
    const std::string patch =
        "@@ this is not a valid header @@\n"
        "-bogus\n"
        "@@ -1 +1 @@\n"
        "+ok\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 1u);
    ASSERT_EQ(hunks[0].lines.size(), 1u);
    EXPECT_EQ(hunks[0].lines[0].type, DiffLine::Type::Add);
    EXPECT_EQ(hunks[0].lines[0].content, "ok");
}

TEST(DiffParser, NoNewlineSentinelDropped) {
    const std::string patch =
        "@@ -1 +1 @@\n"
        "-old\n"
        "+new\n"
        "\\ No newline at end of file\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 1u);
    EXPECT_EQ(hunks[0].lines.size(), 2u);
}

TEST(DiffParser, EmptyContextLinePreserved) {
    const std::string patch =
        "@@ -1,3 +1,3 @@\n"
        " a\n"
        "\n"          // bare empty line — should become an empty context line
        " c\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 1u);
    ASSERT_EQ(hunks[0].lines.size(), 3u);
    EXPECT_EQ(hunks[0].lines[1].type, DiffLine::Type::Context);
    EXPECT_EQ(hunks[0].lines[1].content, "");
}

TEST(DiffParser, CRLFInputNormalised) {
    const std::string patch =
        "@@ -1 +1 @@\r\n"
        "-old\r\n"
        "+new\r\n";
    auto hunks = parse_patch(patch);
    ASSERT_EQ(hunks.size(), 1u);
    ASSERT_EQ(hunks[0].lines.size(), 2u);
    EXPECT_EQ(hunks[0].lines[0].content, "old");
    EXPECT_EQ(hunks[0].lines[1].content, "new");
}

// ---------- parse_pr_url ----------
using aipr::github::parse_pr_url;

TEST(ParsePrUrl, Canonical) {
    auto loc = parse_pr_url("https://github.com/chenshuo/muduo/pull/729");
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->owner, "chenshuo");
    EXPECT_EQ(loc->repo, "muduo");
    EXPECT_EQ(loc->number, 729);
}

TEST(ParsePrUrl, TrailingSlash) {
    auto loc = parse_pr_url("https://github.com/octocat/Hello-World/pull/1/");
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->owner, "octocat");
    EXPECT_EQ(loc->repo, "Hello-World");
    EXPECT_EQ(loc->number, 1);
}

TEST(ParsePrUrl, FilesSubpath) {
    auto loc = parse_pr_url("https://github.com/a/b/pull/42/files");
    ASSERT_TRUE(loc.has_value());
    EXPECT_EQ(loc->number, 42);
}

TEST(ParsePrUrl, RejectsNonPrUrl) {
    EXPECT_FALSE(parse_pr_url("https://github.com/a/b/issues/1").has_value());
    EXPECT_FALSE(parse_pr_url("https://example.com/a/b/pull/1").has_value());
    EXPECT_FALSE(parse_pr_url("not a url").has_value());
    EXPECT_FALSE(parse_pr_url("").has_value());
}
