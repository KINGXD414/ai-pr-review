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
#include <vector>

#include <nlohmann/json.hpp>

namespace aipr::gateway {

struct LLMConfig {
    std::string host           = "api.deepseek.com";  ///< Bare hostname.
    std::string path           = "/v1/chat/completions";
    std::string model          = "deepseek-chat";
    std::string api_key;                              ///< Mandatory.
    int         timeout_seconds = 30;
    int         max_retries     = 3;
    double      temperature     = 0.2;                ///< Lower => more deterministic.
};

/// Result envelope for the non-streaming call.
struct LLMResponse {
    bool        ok{false};
    std::string content;     ///< Assistant message.  Empty when ok == false.
    std::string error;       ///< Human-readable error, e.g. "HTTP 401" or "timeout".
    int         attempts{0}; ///< Number of HTTPS requests we actually made.
};

// ---------------------------------------------------------------------------
// Function-Calling / Agent support (OpenAI-compatible `tools` protocol)
// ---------------------------------------------------------------------------

/// A single tool the model may decide to call.
struct ToolSpec {
    std::string            name;         ///< e.g. "run_rule_engine"
    std::string            description;  ///< What the tool does, for the model.
    nlohmann::json         parameters;   ///< JSON-Schema object for arguments.
};

/// One tool invocation requested by the model.
struct ToolCall {
    std::string        id;       ///< Call id used to correlate the result.
    std::string        name;     ///< Tool name.
    nlohmann::json     arguments;///< Parsed arguments object.
    std::string        raw_args; ///< Raw JSON string as sent by the model.
};

/// Outcome of a chat round that may carry tool calls instead of final text.
struct ToolChatResult {
    bool                  ok{false};
    bool                  has_tool_calls{false};
    std::string           content;   ///< Final assistant text (when no tool calls).
    std::vector<ToolCall> calls;     ///< Tool invocations requested this round.
    std::string           error;     ///< Human-readable error.
    int                   attempts{0};
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

    /**
     * Agent round: send a full message history (array of {role, content}
     * plus optional {role:"tool", tool_call_id, content} entries) together
     * with a set of `tools`.  If the model responds with tool calls, those
     * are returned in `calls`; the caller executes them, appends the results
     * as tool messages, and calls again.  If the model returns plain text,
     * `has_tool_calls` is false and `content` holds the final answer.
     */
    virtual ToolChatResult chat_with_tools(const std::string&             system_prompt,
                                   const nlohmann::json&          messages,
                                   const std::vector<ToolSpec>&   tools);

private:
    LLMConfig cfg_;

    /// Shared POST + retry/parse core.  `extract` turns the response JSON
    /// into either a final content string or a list of tool calls.
    ToolChatResult post_json_(const nlohmann::json& body);
};

}  // namespace aipr::gateway
