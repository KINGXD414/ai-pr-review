/**
 * @file prompt_builder.h
 * @brief Builds the system and user prompts sent to the LLM gateway.
 *
 * Extracted from main.cpp (P1-4 refactoring).  Accepts PR metadata,
 * optional AST summaries, and optional rule-engine hits, producing
 * a complete user prompt that the orchestrator hands to the LLM.
 */
#pragma once

#include "analyzer/ast_analyzer.h"
#include "github/api_client.h"
#include "rules/engine.h"

#include <cstddef>
#include <string>
#include <vector>

namespace aipr::pipeline {

/// Truncate a unified-diff patch blob to at most `max_lines` lines.
std::string truncate_patch(const std::string& patch, std::size_t max_lines);

/// Return the full system prompt for the given persona.
std::string build_system_prompt(const std::string& persona);

/**
 * Build the user prompt (PR metadata + diff + optional AST + optional rule hits).
 *
 * @param pr            Fetched PR with all file patches.
 * @param ast_summaries AST summaries from libclang (empty = skip section).
 * @param rule_hits     Rule-engine findings (empty = skip section).
 */
std::string build_user_prompt(
    const aipr::github::PullRequest&                pr,
    const std::vector<aipr::analyzer::AstSummary>&  ast_summaries = {},
    const std::vector<aipr::rule::RuleHit>&         rule_hits     = {});

}  // namespace aipr::pipeline
