/**
 * @file kodo_client.h
 * @brief Minimal Qiniu KODO object-storage client.
 *
 * v0.3 feature: optionally upload the rendered review report to Qiniu KODO
 * so that reviewers / graders can read the report via a stable public URL
 * (instead of having to clone the repo and rebuild).
 *
 * We only need ONE operation — PUT a small Markdown blob — so we implement
 * the upload token by hand (PutPolicy + HMAC-SHA1 + URL-safe base64) per
 * https://developer.qiniu.com/kodo/manual/1208/upload-token, and we do the
 * multipart POST with the project's existing cpp-httplib dependency.
 *
 * Design notes:
 *   - All credentials are loaded from environment variables; nothing is
 *     baked into the binary, nothing is logged. See `from_env()`.
 *   - The client is stateless — instantiate it where you need it.
 *   - We do NOT introduce a new SDK (no Qiniu official C SDK, no AWS SDK
 *     for C++). Both would have added another FetchContent block and a
 *     non-trivial CMake glue layer for ~ 100 lines of business logic.
 */
#pragma once

#include <cstdint>
#include <string>

namespace aipr::storage {

/// Runtime configuration. Build via `from_env()` in normal use.
struct KodoConfig {
    std::string ak;        ///< AccessKey   (env: QINIU_AK)
    std::string sk;        ///< SecretKey   (env: QINIU_SK)
    std::string bucket;    ///< Bucket name (env: QINIU_BUCKET)
    std::string region;    ///< Region id, e.g. "z0" (env: QINIU_REGION, default "z0")
    std::string domain;    ///< Public domain bound to the bucket (env: QINIU_DOMAIN)
    int         deadline_sec{3600};  ///< Upload-token TTL, default 1h.
};

/// Result of an upload call. Inspect `ok` first; on failure read `error`.
struct UploadResult {
    bool        ok{false};
    std::string key;       ///< Object key actually stored (echoed by KODO).
    std::string hash;      ///< ETag-equivalent returned by KODO.
    std::string url;       ///< Public URL (only meaningful for public buckets with a bound domain).
    std::string error;     ///< Empty on success.
};

class KodoClient {
public:
    explicit KodoClient(KodoConfig cfg);

    /// Build a Qiniu upload token for ONE specific key. Implementation:
    ///   PutPolicy = {"scope":"<bucket>:<key>","deadline":<unix_ts>,"insertOnly":0}
    ///   encoded   = url_safe_base64(json(PutPolicy))
    ///   sign      = url_safe_base64(HmacSha1(SK, encoded))
    ///   token     = AK + ":" + sign + ":" + encoded
    std::string make_upload_token(const std::string& key) const;

    /// Synchronous upload. Returns immediately on transport / HTTP / parse
    /// failure with `ok=false`; never throws.
    UploadResult upload(const std::string& key,
                        const std::string& body,
                        const std::string& content_type = "text/markdown; charset=utf-8") const;

    /// Build the public URL for a key, assuming a public bucket bound to
    /// `cfg.domain`. Returns empty string if domain is not configured.
    std::string public_url(const std::string& key) const;

    /// Load config from QINIU_AK/SK/BUCKET/REGION/DOMAIN.
    static KodoConfig from_env();

    /// True iff AK + SK + BUCKET are all present in the environment.
    static bool env_ready();

private:
    KodoConfig cfg_;
};

}  // namespace aipr::storage
