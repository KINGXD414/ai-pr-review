/**
 * @file report_renderer.h
 * @brief Renders the final Markdown review report.
 *
 * Extracted from main.cpp (P1-4 refactoring).  Takes the fetched PR,
 * LLM response, pipeline configuration, and produces a complete
 * Markdown string ready to write to disk.
 */
#pragma once

#include "github/api_client.h"
#include "gateway/llm_gateway.h"

#include <chrono>
#include <string>

namespace aipr::pipeline {

/**
 * Produce the full Markdown report.
 *
 * @param pr                 Fetched PR metadata.
 * @param consensus_n        Number of consensus runs (1 = disabled).
 * @param llm                Final LLM response (after optional consensus / critique stages).
 * @param model              Model name string for the report header.
 * @param elapsed            Wall-clock duration of the entire review.
 * @param persona            Persona key used.
 * @param self_critique_used Whether the self-critique stage ran.
 */
std::string render_report(const aipr::github::PullRequest& pr,
                          int                               consensus_n,
                          const aipr::gateway::LLMResponse& llm,
                          const std::string&                model,
                          std::chrono::milliseconds         elapsed,
                          const std::string&                persona,
                          bool                              self_critique_used);

}  // namespace aipr::pipeline
