/**
 * @file agent.h
 * @brief Agentic review loop built on LLM Function Calling.
 *
 * JD alignment: the role asks for "业务 Agent ... Function Calling / Agent
 * / Skill".  The plain `--persona` path is a fixed pipeline (fetch → prompt
 * → one LLM call).  This module lifts that into a real agent: the model is
 * given a set of *tools* (rule engine, AST analyzer, RAG query, comment
 * draft) and decides, per round, which to call and in what order, looping
 * until it produces a final answer.  That is the difference between a
 * workflow and an Agent — and it is what this role is really testing.
 *
 * All tools operate on already-fetched PR data; none of them re-fetch
 * GitHub.  The loop is bounded (max iterations) and deterministic on
 * temperature, so it never spins forever.
 */
#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "analyzer/ast_analyzer.h"
#include "gateway/llm_gateway.h"
#include "github/api_client.h"
#include "rag/retriever.h"
#include "rules/engine.h"

namespace aipr::agent {

/// Everything the agent's tools need to act on the current PR.
struct AgentContext {
    const aipr::github::PullRequest* pr{nullptr};
    const aipr::rule::ParsedDiff*    diff{nullptr};     ///< Parsed PR diff (P0).
    std::string                      source_dir;        ///< For AST, may be empty.
    aipr::rag::Retriever*            retriever{nullptr}; ///< Optional RAG corpus.
    int                             max_iterations{6};
};

/// Build the tool catalogue exposed to the model.
std::vector<aipr::gateway::ToolSpec> build_tools(bool with_rag);

/// Execute one tool call.  Returns a human/machine readable result string
/// that is fed back to the model as a `tool` message.
std::string execute_tool(const std::string& name,
                         const nlohmann::json& args,
                         const AgentContext&   ctx);

/**
 * Run the agentic review loop.
 *
 * @param system_prompt  Reviewer system prompt (persona-aware).
 * @param base_user_prompt  The PR context block (diff + AST) as assembled by
 *                          main.cpp.
 * @param gw            Configured LLM gateway (must support chat_with_tools).
 * @param ctx           PR data + optional RAG retriever.
 * @param out_attempts  Total LLM round-trips made (for the report footer).
 * @return Final review Markdown, or an error string if the loop failed.
 */
std::string run_agent(const std::string&                 system_prompt,
                     const std::string&                 base_user_prompt,
                     aipr::gateway::LLMGateway&         gw,
                     const AgentContext&                ctx,
                     int&                               out_attempts);

}  // namespace aipr::agent
