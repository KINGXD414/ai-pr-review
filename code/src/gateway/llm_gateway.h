/**
 * @file llm_gateway.h
 * @brief HTTPS gateway to the DeepSeek (OpenAI-compatible) chat endpoint.
 *
 * Day-1 scope:
 *   - Non-streaming `chat()` returns the full assistant message as a string.
 *   - Streaming `chat_stream()` parses SSE frames and dispatches each token
 *     to a user callback.  Day-1 only requires the non-streaming path to be
 *     production-quality; the streaming entry point lives behind the same
 *     SSL client so Day-2 work plugs in without an API churn.
 *
 * Both calls honour a configurable per-attempt timeout (default 30 s) and
 * retry up to N times (default 3) on transport errors or HTTP 5xx.
 *
 * Reference: design document §4.5.
 */
#pragma once

#include <functional>
#include <optional>
#include <string>

namespace aipr::gateway {

struct LLMConfig {
    std::string host           = "api.deepseek.com";  ///< Bare hostname.
    std::string path           = "/v1/chat/completions";
    std::string model          = "deepseek-chat";
    std::string api_key;                              ///< Mandatory.
    int         timeout_seconds = 30;
    int         max_retries     = 3;
    double      temperature     = 0.2;                ///< Lower => more deterministic.

    // Cache settings (P1-2: avoids duplicate API calls for identical inputs).
    std::string cache_dir;            ///< Empty => cache disabled.  Suggested: ~/.cache/ai-pr-review/
    int         cache_ttl_seconds = 3600;  ///< Entries older than this are ignored.
};

/// Result envelope for the non-streaming call.
struct LLMResponse {
    bool        ok{false};
    std::string content;     ///< Assistant message.  Empty when ok == false.
    std::string error;       ///< Human-readable error, e.g. "HTTP 401" or "timeout".
    int         attempts{0}; ///< Number of HTTPS requests we actually made.
};

class LLMGateway {
public:
    explicit LLMGateway(LLMConfig cfg);

    /**
     * Synchronous, non-streaming chat completion.
     *
     * @param system_prompt  System role text.  Pass an empty string to omit
     *                       the system message entirely.
     * @param user_prompt    User message body.
     */
    LLMResponse chat(const std::string& system_prompt,
                     const std::string& user_prompt);

    /// Streaming variant — Day-1 implementation builds the SSE request and
    /// forwards every `data: …` content delta to `on_token`.  Returns the
    /// concatenated assistant message on success.
    using TokenCallback = std::function<void(const std::string& token)>;
    LLMResponse chat_stream(const std::string& system_prompt,
                            const std::string& user_prompt,
                            TokenCallback      on_token);

private:
    LLMConfig cfg_;
};

}  // namespace aipr::gateway
