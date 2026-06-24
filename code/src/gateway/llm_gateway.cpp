/**
 * @file llm_gateway.cpp
 * @brief Implementation of the DeepSeek chat-completion gateway.
 */
#include "gateway/llm_gateway.h"

#include <httplib.h>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <openssl/sha.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string_view>
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

// ---- P1-2: Response cache ----

/// SHA256 hex digest of an arbitrary string.
std::string sha256_hex(const std::string& s) {
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(s.data()), s.size(), hash);
    std::ostringstream oss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0')
            << static_cast<int>(hash[i]);
    }
    return oss.str();
}

/// Produce a deterministic cache key from the input parameters.
std::string cache_key(const LLMConfig& cfg,
                      const std::string& sys,
                      const std::string& usr) {
    // Everything that affects the LLM output is part of the key.
    std::string material = cfg.model + "|" +
                           std::to_string(cfg.temperature) + "|" +
                           sys + "|" + usr;
    return sha256_hex(material);
}

/// File path for a given cache key.
std::string cache_path(const LLMConfig& cfg, const std::string& key) {
    // Two-level directory to avoid a single directory with thousands of files.
    std::string dir = cfg.cache_dir + "/" + key.substr(0, 2) + "/" + key.substr(2, 2);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir + "/" + key + ".json";
}

/// Try to load a cached response. Returns nullopt on miss / expiry / parse failure.
std::optional<LLMResponse> cache_load(const LLMConfig& cfg, const std::string& key) {
    if (cfg.cache_dir.empty()) return std::nullopt;

    auto p = cache_path(cfg, key);
    std::ifstream ifs(p);
    if (!ifs) return std::nullopt;

    try {
        auto j = nlohmann::json::parse(ifs);
        std::int64_t created = j.value("created_at", std::int64_t{0});
        auto now = static_cast<std::int64_t>(std::time(nullptr));
        if (now - created > cfg.cache_ttl_seconds) {
            spdlog::debug("Cache: expired entry {}", key.substr(0, 12));
            return std::nullopt;
        }
        LLMResponse out;
        out.ok      = true;
        out.content = j.value("content", std::string{});
        out.attempts = 0;  // cached, not a real API call
        spdlog::info("Cache: hit {} (age {}s)", key.substr(0, 12), now - created);
        return out;
    } catch (const std::exception& e) {
        spdlog::debug("Cache: parse error {}: {}", key.substr(0, 12), e.what());
    }
    return std::nullopt;
}

/// Persist a successful response to the cache.
void cache_store(const LLMConfig& cfg, const std::string& key, const LLMResponse& resp) {
    if (cfg.cache_dir.empty() || !resp.ok) return;

    auto p = cache_path(cfg, key);
    try {
        nlohmann::json j;
        j["key"]        = key;
        j["content"]    = resp.content;
        j["created_at"] = static_cast<std::int64_t>(std::time(nullptr));
        std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
        if (ofs) ofs << j.dump();
        spdlog::debug("Cache: stored {}", key.substr(0, 12));
    } catch (const std::exception& e) {
        spdlog::debug("Cache: write error {}: {}", key.substr(0, 12), e.what());
    }
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

    // --- P1-2: Check cache before making any API call ---
    const std::string ck = cache_key(cfg_, system_prompt, user_prompt);
    if (auto cached = cache_load(cfg_, ck)) {
        return *cached;
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
                // P1-2: Persist to cache so repeated runs skip the API call.
                cache_store(cfg_, ck, out);
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
    LLMResponse out;
    if (cfg_.api_key.empty()) {
        out.error = "missing API key";
        return out;
    }
    if (!on_token) {
        // No callback → fall back to non-streaming (simpler & faster).
        return chat(system_prompt, user_prompt);
    }

    // Build body with stream=true for SSE.
    const auto       body = build_body(cfg_, system_prompt, user_prompt, /*stream=*/true);
    const std::string body_str = body.dump();

    httplib::Headers headers = {
        {"Authorization", "Bearer " + cfg_.api_key},
        {"Content-Type",  "application/json"},
        {"Accept",        "text/event-stream"},
    };

    std::string full_content;
    std::string sse_buffer;

    // P1-3: use httplib's content receiver to process SSE chunks as they arrive.
    for (int attempt = 1; attempt <= cfg_.max_retries; ++attempt) {
        out.attempts = attempt;
        full_content.clear();
        sse_buffer.clear();

        httplib::SSLClient cli(cfg_.host, 443);
        cli.set_connection_timeout(cfg_.timeout_seconds);
        cli.set_read_timeout(cfg_.timeout_seconds);
        cli.set_write_timeout(cfg_.timeout_seconds);

        // Progressive content receiver — httplib calls this as data arrives
        // from the socket, before the full response body is complete.
        cli.set_content_receiver([&](const char* data, size_t len) -> bool {
            sse_buffer.append(data, len);

            // SSE events are delimited by double newlines.
            size_t pos;
            while ((pos = sse_buffer.find("\n\n")) != std::string::npos) {
                std::string event = sse_buffer.substr(0, pos);
                sse_buffer.erase(0, pos + 2);

                // Each event can have multiple "data:" lines; we process each.
                std::istringstream eiss(event);
                std::string eline;
                while (std::getline(eiss, eline)) {
                    // Strip trailing \r if present.
                    if (!eline.empty() && eline.back() == '\r') eline.pop_back();

                    constexpr std::string_view kPrefix = "data: ";
                    if (eline.rfind(kPrefix, 0) != 0) continue;

                    std::string json_str = eline.substr(kPrefix.size());
                    if (json_str == "[DONE]") continue;

                    try {
                        auto j = nlohmann::json::parse(json_str);
                        if (j.contains("choices") && j["choices"].is_array() &&
                            !j["choices"].empty()) {
                            const auto& choice = j["choices"][0];
                            if (choice.contains("delta") &&
                                choice["delta"].contains("content")) {
                                std::string delta =
                                    choice["delta"]["content"].get<std::string>();
                                full_content += delta;
                                on_token(delta);
                            }
                        }
                    } catch (const std::exception&) {
                        // Some providers inject non-JSON keepalive comments;
                        // silently skip them.
                    }
                }
            }
            return true;  // continue receiving
        });

        auto res = cli.Post(cfg_.path.c_str(), headers, body_str, "application/json");

        // Clear content receiver so next attempt starts fresh.
        cli.set_content_receiver(nullptr);

        if (!res) {
            out.error = "transport error: " + httplib::to_string(res.error());
            spdlog::warn("LLM stream attempt {}/{} failed: {}",
                         attempt, cfg_.max_retries, out.error);
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
            continue;
        }

        if (res->status >= 500 && res->status < 600) {
            out.error = "HTTP " + std::to_string(res->status);
            spdlog::warn("LLM stream attempt {}/{} got {}",
                         attempt, cfg_.max_retries, res->status);
            std::this_thread::sleep_for(std::chrono::seconds(attempt));
            continue;
        }

        if (res->status != 200) {
            out.error = "HTTP " + std::to_string(res->status) + ": " +
                        res->body.substr(0, 200);
            spdlog::error("LLM stream call failed permanently: {}", out.error);
            return out;
        }

        if (full_content.empty()) {
            out.error = "empty content in LLM stream response";
            spdlog::warn("{}", out.error);
        } else {
            out.ok = true;
            out.content = std::move(full_content);
        }
        return out;
    }

    spdlog::error("LLM stream exhausted {} retries", cfg_.max_retries);
    return out;
}

}  // namespace aipr::gateway
