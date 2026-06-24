/**
 * @file utils.h
 * @brief Shared utility functions used across the project.
 *
 * Extracted from main.cpp and eval_main.cpp to eliminate duplication.
 * All functions are header-only inline for simplicity — this module is
 * small enough that a separate .cpp adds more ceremony than value.
 */
#pragma once

#include <ctime>
#include <cstdlib>
#include <string>

namespace aipr::common {

/// Read an environment variable, returning "" when unset.
inline std::string env_or_empty(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

/// Return the current local time as an ISO-ish string "YYYY-MM-DD HH:MM:SS".
inline std::string now_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm_buf{};
    localtime_r(&now, &tm_buf);
    char ts[32];
    std::strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_buf);
    return ts;
}

/// Return a compact UTC timestamp "YYYYMMDD-HHMMSS" suitable for filenames.
inline std::string utc_stamp() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char ts[32] = {0};
    std::strftime(ts, sizeof(ts), "%Y%m%d-%H%M%S", &tm);
    return ts;
}

}  // namespace aipr::common
