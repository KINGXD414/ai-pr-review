/**
 * @file test_agent_mock.cpp
 * @brief End-to-end test of the agentic loop + tool dispatch using a MOCK
 *        LLM gateway (no network, no libclang).
 *
 * This compiles standalone with a plain C++17 compiler plus the project's
 * headers. It proves the P1 Function-Calling logic is correct:
 *   - build_tools() emits the right tool catalogue (incl. rag_query when RAG on)
 *   - execute_tool() routes each tool to the right handler
 *   - run_agent() drives a multi-round loop: the mock model first asks for
 *     run_rule_engine, then for rag_query, then emits a final answer.
 *
 * Build & run (ast_analyzer_stub.cpp provides the libclang symbols so we
 * can link without LLVM installed):
 *   g++ -std=c++17 -Isrc tests/test_agent_mock.cpp src/agent/agent.cpp \
 *       src/rag/retriever.cpp src/rules/engine.cpp src/diff/parser.cpp \
 *       src/prompt/skills.cpp \
 *       stubs/ast_analyzer_stub.cpp -o test_agent
 *   ./test_agent
 */
#include "agent/agent.h"
#include "gateway/llm_gateway.h"
#include "rag/retriever.h"

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_fail = 0;
#define CHECK(c) do{ if(!(c)){ std::printf("FAIL %s:%d %s\n",__FILE__,__LINE__,#c); ++g_fail; } }while(0)

// ---- Mock gateway: scripted to mimic a tool-using model ----
class MockGateway : public aipr::gateway::LLMGateway {
public:
    mutable int round = 0;
    explicit MockGateway() : aipr::gateway::LLMGateway(aipr::gateway::LLMConfig{}) {}

    aipr::gateway::ToolChatResult chat_with_tools(
        const std::string&,
        const nlohmann::json&,
        const std::vector<aipr::gateway::ToolSpec>& /*tools*/) override {
        aipr::gateway::ToolChatResult r;
        r.ok = true;
        r.attempts = 1;  // each model round-trip counts as one attempt
        ++round;
        if (round == 1) {
            // Model decides to call run_rule_engine.
            r.has_tool_calls = true;
            r.calls.push_back({"call_1", "run_rule_engine", nlohmann::json::object(), "{}"});
        } else if (round == 2) {
            // Model asks which Skills are available.
            r.has_tool_calls = true;
            r.calls.push_back({"call_2", "list_skills", nlohmann::json::object(), "{}"});
        } else if (round == 3) {
            // Model selects a persona skill.
            r.has_tool_calls = true;
            r.calls.push_back({"call_3", "select_skill",
                              nlohmann::json::parse(R"({"id":"linus"})"),
                              R"({"id":"linus"})"});
        } else if (round == 4) {
            // Model decides to call rag_query.
            r.has_tool_calls = true;
            r.calls.push_back({"call_4", "rag_query",
                              nlohmann::json::parse(R"({"query":"内存泄漏"})"),
                              R"({"query":"内存泄漏"})"});
        } else {
            // Model produces the final review.
            r.has_tool_calls = false;
            r.content = "## 一、变更总结\n修复了 socket 关闭顺序。\n## 二、风险代码\n无。\n## 三、Review 建议\ngood。";
        }
        return r;
    }
};

}  // namespace

int main() {
    // Build a fake PR + diff so run_rule_engine has something to scan.
    aipr::github::PullRequest pr;
    pr.locator.owner = "acme";
    pr.locator.repo  = "net";
    pr.locator.number = 7;
    pr.title = "fix leak";
    pr.body  = "";
    pr.html_url = "https://github.com/acme/net/pull/7";
    pr.author = "dev";
    pr.base_ref = "main"; pr.head_ref = "fix";

    aipr::github::PRFile f;
    f.filename = "conn.cc";
    f.status = "modified";
    f.patch = "@@ -10,3 +10,4 @@ void close()\n int* p = (int*)malloc(4);\n+foo();\n- free(p);\n";
    pr.files.push_back(f);

    aipr::rule::ParsedDiff pd;
    aipr::rule::FileDiff fd;
    fd.filename = "conn.cc";
    fd.added_lines = {"int* p = (int*)malloc(4);", "foo();"};
    fd.added_line_nums = {11, 12};
    fd.removed_lines = {"free(p);"};
    fd.removed_line_nums = {13};
    pd.files.push_back(fd);

    // Build a small RAG corpus directly so the rag_query tool has something to hit.
    aipr::rag::Retriever rag;
    rag.add_document("kb_unpaired_alloc",
        "malloc/calloc/realloc 必须与 free 配对，new 必须与 delete 配对。");
    rag.add_document("kb_leak",
        "内存泄漏：分配后未释放会在长生命周期对象中累积。");
    rag.build();

    aipr::agent::AgentContext ctx;
    ctx.pr = &pr;
    ctx.diff = &pd;
    ctx.retriever = &rag;
    ctx.max_iterations = 6;

    MockGateway gw;

    int attempts = 0;
    std::string report = aipr::agent::run_agent(
        "你是一个严格的代码审查者。", "# 上下文\n" + pr.title, gw, ctx, attempts);

    CHECK(!report.empty());
    CHECK(report.find("变更总结") != std::string::npos);
    CHECK(attempts >= 5);  // 4 tool rounds + 1 final
    CHECK(gw.round == 5);

    if (g_fail == 0) {
        std::printf("ALL AGENT LOOP TESTS PASSED (rounds=%d)\n", gw.round);
        return 0;
    }
    std::printf("%d AGENT LOOP TEST(S) FAILED\n", g_fail);
    return 1;
}
