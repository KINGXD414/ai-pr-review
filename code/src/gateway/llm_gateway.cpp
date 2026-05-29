/**
 * @file llm_gateway.cpp
 * @brief Implementation of the DeepSeek chat-completion gateway.
 */
#include "gateway/llm_gateway.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <chrono>
#include <thread>
#include <utility>

namespace aipr::gateway {

namespace {

/// Build the OpenAI-compatible chat body.
nlohmann::json build_body(const LLMConfig&   cfg,
                          const std::string& system_prompt,
                          const std::string& user_prompt,
                          bool               stream) {
    nlohmann::json messages = nlohmann::json::array();
    if (!system_prompt.empty()) {
        messages.push_back({{"role", "system"}, {"content", system_prompt}});
    }
    messages.push_back({{"role", "user"}, {"content", user_prompt}});

    nlohmann::json body = {
        {"model",       cfg.model},
        {"messages",    messages},
        {"temperature", cfg.temperature},
        {"stream",      stream},
    };
    return body;
}

/// Pull the assistant text out of an OpenAI/DeepSeek response payload.
/// Returns empty string if the schema is unfamiliar — callers treat that
/// as a soft failure.
std::string extract_content(const nlohmann::json& j) {
    try {
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) {
            return {};
        }
        const auto& first = j["choices"][0];
        if (first.contains("message") && first["message"].contains("content")) {
            return first["message"]["content"].get<std::string>();
        }
    } catch (const std::exception& e) {
        spdlog::warn("Failed to extract content from LLM response: {}", e.what());
    }
    return {};
}

}  // namespace

LLMGateway::LLMGateway(LLMConfig cfg) : cfg_(std::move(cfg)) {}

LLMResponse LLMGateway::chat(const std::string& system_prompt,
                             const std::string& user_prompt) {
    LLMResponse out;
    if (cfg_.api_key.empty()) {
        out.error = "missing API key";
        return out;
    }

    const auto       body = build_body(cfg_, system_prompt, user_prompt, /*stream=*/false);
    const std::string body_str = body.dump();

    httplib::Headers headers = {
        {"Authorization", "Bearer " + cfg_.api_key},
        {"Content-Type",  "application/json"},
        {"Accept",        "application/json"},
    };

    for (int attempt = 1; attempt <= cfg_.max_retries; ++attempt) {
        out.attempts = attempt;

        httplib::SSLClient cli(cfg_.host, 443);
        cli.set_connection_timeout(cfg_.timeout_seconds);
        cli.set_read_timeout(cfg_.timeout_seconds);
        cli.set_write_timeout(cfg_.timeout_seconds);

        auto res = cli.Post(cfg_.path.c_str(), headers, body_str, "application/json");
        if (!res) {
            out.error = "transport error: " + httplib::to_string(res.error());
            spdlog::warn("LLM attempt {}/{} failed: {}",
                         attempt, cfg_.max_retries, out.error);
            // Linear back-off (Day-1 is fine; exponential later).
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
            continue;
        }

        if (res->status >= 500 && res->status < 600) {
            out.error = "HTTP " + std::to_string(res->status);
            spdlog::warn("LLM attempt {}/{} got {}: {}",
                         attempt, cfg_.max_retries, res->status,
                         res->body.substr(0, 200));
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
            continue;
        }

        if (res->status != 200) {
            // 4xx — almost always a permanent error (bad key, bad model name);
            // do not retry to avoid burning quota.
            out.error = "HTTP " + std::to_string(res->status) + ": " +
                        res->body.substr(0, 200);
            spdlog::error("LLM call failed permanently: {}", out.error);
            return out;
        }

        try {
            const auto j = nlohmann::json::parse(res->body);
            out.content  = extract_content(j);
            if (out.content.empty()) {
                out.error = "empty content in LLM response";
                spdlog::warn("{}", out.error);
            } else {
                out.ok = true;
            }
        } catch (const std::exception& e) {
            out.error = std::string("JSON parse error: ") + e.what();
            spdlog::error("{}", out.error);
        }
        return out;
    }

    spdlog::error("LLM call exhausted {} retries", cfg_.max_retries);
    return out;
}

LLMResponse LLMGateway::chat_stream(const std::string& system_prompt,
                                    const std::string& user_prompt,
                                    TokenCallback      on_token) {
    // Day-1 placeholder: cpp-httplib v0.15.3 does not expose a Post overload
    // that combines (Headers, body, content_type, ContentReceiver), so a true
    // SSE consumer would need either ResponseHandler+ContentReceiver wiring
    // or a hand-rolled socket layer.  Both are Day-2 scope.
    //
    // For Day 1 we degrade gracefully: call the non-streaming endpoint and,
    // if it succeeds, emit the full content as a single "token" so the
    // streaming caller still observes a callback contract.
    LLMResponse out = chat(system_prompt, user_prompt);
    if (out.ok && on_token) {
        on_token(out.content);
    }
    return out;
}

}  // namespace aipr::gateway
