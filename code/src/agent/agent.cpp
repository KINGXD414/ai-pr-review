/**
 * @file agent.cpp
 * @brief Tool implementations + agentic loop for the PR reviewer.
 */
#include "agent/agent.h"

#include <filesystem>
#include <sstream>

#include "prompt/skills.h"
#include <spdlog/spdlog.h>

namespace aipr::agent {

namespace {

std::string rule_hits_to_text(const std::vector<aipr::rule::RuleHit>& hits) {
    if (hits.empty()) return "(规则引擎未产生任何命中)";
    std::ostringstream oss;
    for (const auto& h : hits) {
        oss << "- [" << aipr::rule::severity_str(h.severity) << "] "
            << h.rule_id << " @ " << h.file << ":" << h.line << "\n"
            << "  问题: " << h.message << "\n";
        if (!h.suggestion.empty()) oss << "  建议: " << h.suggestion << "\n";
    }
    return oss.str();
}

nlohmann::json string_schema(const std::string& desc,
                              bool required = true) {
    nlohmann::json s = {{"type", "string"}, {"description", desc}};
    if (required) s["description"] = desc + " (必填)";
    return s;
}

}  // namespace

std::vector<aipr::gateway::ToolSpec> build_tools(bool with_rag) {
    std::vector<aipr::gateway::ToolSpec> tools;

    // run_rule_engine — pure static analysis, no arguments.
    {
        nlohmann::json params = nlohmann::json::object();
        params["type"] = "object";
        params["properties"] = nlohmann::json::object();
        params["required"] = nlohmann::json::array();
        tools.push_back({"run_rule_engine",
            "对已解析的 PR diff 运行 8 条静态规则，返回所有命中"
            "（含规则 id、文件、行号、严重度、建议）。"
            "在你需要对确定性问题（内存泄漏、裸指针、C 风格转换、memcpy 尺寸等）给出证据时使用。",
            params});
    }

    // analyze_ast — needs a source file name.
    {
        nlohmann::json props = nlohmann::json::object();
        props["file"] = string_schema("要分析的源文件名，如 muduo/net/TcpConnection.cc");
        nlohmann::json params = nlohmann::json::object();
        params["type"] = "object";
        params["properties"] = props;
        params["required"] = nlohmann::json::array({"file"});
        tools.push_back({"analyze_ast",
            "对指定源文件运行 libclang AST 分析，返回其中的类/函数签名、"
            "继承关系与包含文件。当 diff 涉及虚函数、接口改动或需要理解"
            "跨文件类型关系时使用。",
            params});
    }

    // rag_query — only offered when the RAG corpus is enabled.
    if (with_rag) {
        nlohmann::json props = nlohmann::json::object();
        props["query"] = string_schema("检索查询，如 '异步回调中 double free 风险'");
        nlohmann::json params = nlohmann::json::object();
        params["type"] = "object";
        params["properties"] = props;
        params["required"] = nlohmann::json::array({"query"});
        tools.push_back({"rag_query",
            "在编码规范 / 历史审查报告语料中检索与查询最相关的片段，"
            "用于对齐团队的编码惯例或借鉴过往类似问题的审查意见。",
            params});
    }

    // draft_comment — final report body.
    {
        nlohmann::json props = nlohmann::json::object();
        props["body"] = string_schema("最终审查报告的完整 Markdown 正文");
        nlohmann::json params = nlohmann::json::object();
        params["type"] = "object";
        params["properties"] = props;
        params["required"] = nlohmann::json::array({"body"});
        tools.push_back({"draft_comment",
            "将最终审查意见写入一条待发布评论（结构化 Markdown）。"
            "调用它表示审查已完成，模型随后应输出最终报告。",
            params});
    }

    // select_skill — JD "Skill": pick a composable review strategy.
    {
        nlohmann::json props = nlohmann::json::object();
        props["id"] = string_schema("Skill id，如 default / linus / devil / self_critique / consensus");
        nlohmann::json params = nlohmann::json::object();
        params["type"] = "object";
        params["properties"] = props;
        params["required"] = nlohmann::json::array({"id"});
        tools.push_back({"select_skill",
            "选择一个审查 Skill（策略/人设）。调用后你会得到该策略的"
            "system prompt 片段，请据此调整后续审查的语气与焦点。",
            params});
    }

    // list_skills — enumerate available skills.
    {
        nlohmann::json params = nlohmann::json::object();
        params["type"] = "object";
        params["properties"] = nlohmann::json::object();
        params["required"] = nlohmann::json::array();
        tools.push_back({"list_skills",
            "列出全部可用 Skill（审查策略）及其用途，供你按需选择。",
            params});
    }

    return tools;
}

std::string execute_tool(const std::string& name,
                         const nlohmann::json& args,
                         const AgentContext&   ctx) {
    if (name == "run_rule_engine") {
        if (!ctx.diff) return "错误：尚未解析 PR diff。";
        aipr::rule::RuleEngine engine;
        auto hits = engine.run(*ctx.diff, /*ast=*/nullptr);
        return rule_hits_to_text(hits);
    }

    if (name == "analyze_ast") {
        std::string file = args.value("file", std::string{});
        if (file.empty()) return "错误：缺少参数 file。";
        if (ctx.source_dir.empty()) {
            return "错误：未提供 --source-dir，无法定位本地源码进行 AST 分析。";
        }
        std::filesystem::path p = std::filesystem::path(ctx.source_dir) / file;
        if (!std::filesystem::exists(p)) {
            return "错误：本地未找到文件 " + p.string();
        }
        aipr::analyzer::AstAnalyzer analyzer;
        auto summary = analyzer.analyze_file(p.string());
        return summary.ok ? summary.to_prompt() : ("AST 解析失败: " + summary.error);
    }

    if (name == "rag_query") {
        if (!ctx.retriever || ctx.retriever->size() == 0) {
            return "错误：RAG 语料未加载（未启用 --rag）。";
        }
        std::string query = args.value("query", std::string{});
        if (query.empty()) return "错误：缺少参数 query。";
        auto hits = ctx.retriever->search_best(query, 3);
        if (hits.empty()) return "(未检索到相关片段)";
        std::ostringstream oss;
        oss << "检索到的相关片段 (top " << hits.size() << "):\n";
        for (const auto& h : hits) {
            oss << "- [" << h.id << ", score=" << h.score << "]\n";
        }
        return oss.str();
    }

    if (name == "list_skills") {
        return aipr::prompt::list_skills_markdown();
    }

    if (name == "select_skill") {
        std::string id = args.value("id", std::string{});
        if (id.empty()) return "错误：缺少参数 id。";
        const aipr::prompt::Skill* s = aipr::prompt::find_skill(id);
        if (!s) {
            return "未知 Skill: " + id + "\n" + aipr::prompt::list_skills_markdown();
        }
        std::ostringstream oss;
        oss << "已选择 Skill: " << s->name << "\n"
            << "说明: " << s->description << "\n\n"
            << "请基于以下 system prompt 片段调整你的审查语气与焦点：\n\n"
            << s->system_prompt << "\n";
        return oss.str();
    }

    if (name == "draft_comment") {
        std::string body = args.value("body", std::string{});
        if (body.empty()) return "错误：缺少参数 body。";
        // Persist the draft so run_agent can return it as the final report.
        // We echo it back; main writes it to disk exactly as a normal review.
        return "评论已草拟（将作为最终报告）：\n\n" + body;
    }

    return "未知工具: " + name;
}

std::string run_agent(const std::string&         system_prompt,
                     const std::string&         base_user_prompt,
                     aipr::gateway::LLMGateway& gw,
                     const AgentContext&        ctx,
                     int&                       out_attempts) {
    out_attempts = 0;
    const bool with_rag = (ctx.retriever && ctx.retriever->size() > 0);
    auto tools = build_tools(with_rag);

    // Conversation history passed to the model.
    nlohmann::json messages = nlohmann::json::array();
    messages.push_back({{"role", "user"}, {"content", base_user_prompt}});

    std::string final_text;
    bool draft_seen = false;

    for (int iter = 1; iter <= ctx.max_iterations; ++iter) {
        auto res = gw.chat_with_tools(system_prompt, messages, tools);
        out_attempts += res.attempts;

        if (!res.ok) {
            spdlog::error("Agent round {}/{} failed: {}", iter, ctx.max_iterations, res.error);
            return "Agent 审查失败：" + res.error;
        }

        if (!res.has_tool_calls) {
            // Model produced a final answer (no more tool calls).
            final_text = res.content;
            break;
        }

        // Append the assistant message (with its tool_calls) to history.
        nlohmann::json assistant = {{"role", "assistant"}};
        nlohmann::json tcs = nlohmann::json::array();
        for (const auto& c : res.calls) {
            tcs.push_back({
                {"id", c.id},
                {"type", "function"},
                {"function", {{"name", c.name}, {"arguments", c.raw_args}}},
            });
        }
        assistant["tool_calls"] = tcs;
        messages.push_back(assistant);

        // Execute each tool call and feed results back.
        for (const auto& call : res.calls) {
            spdlog::info("  agent calls tool: {} (iter {})", call.name, iter);
            std::string result = execute_tool(call.name, call.arguments, ctx);
            if (call.name == "draft_comment") draft_seen = true;
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", call.id},
                {"name", call.name},
                {"content", result},
            });
        }
    }

    if (final_text.empty()) {
        // Loop exhausted without a clean final answer.  If a draft_comment was
        // produced, fall back to it so we still emit a usable report.
        if (draft_seen) {
            for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
                if ((*it).value("role", "") == "tool") {
                    const std::string& c = (*it)["content"].get<std::string>();
                    const std::string k = "评论已草拟（将作为最终报告）：\n\n";
                    if (c.rfind(k, 0) == 0) return c.substr(k.size());
                }
            }
        }
        return "Agent 在限定步数内未给出最终结论，请增大 --agent-max-iter 或检查工具结果。";
    }
    return final_text;
}

}  // namespace aipr::agent
