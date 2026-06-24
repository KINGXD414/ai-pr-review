/**
 * @file kodo_client.cpp
 * @brief Minimal Qiniu KODO object-storage client implementation.
 *
 * Only implements PUT upload (multipart/form-data) with hand-rolled upload
 * token signing. No external SDK needed.
 *
 * Prerequisites (already available in the project):
 *   - OpenSSL HMAC-SHA1 for signing.
 *   - cpp-httplib v0.14.3 for the HTTP POST.
 *   - nlohmann/json for PutPolicy serialisation.
 *
 * Reference: https://developer.qiniu.com/kodo/manual/1208/upload-token
 */

#include "storage/kodo_client.h"

#include <httplib.h>
#include <nlohmann/json.hpp>

#include <openssl/hmac.h>

#include <spdlog/spdlog.h>

#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace aipr::storage {

// ---------------------------------------------------------------------------
// Internal helpers (file-local)
// ---------------------------------------------------------------------------

namespace {

/// URL-safe base64 alphabet (Qiniu spec: '+' -> '-', '/' -> '_'), with
/// standard '=' padding kept (matches Qiniu's official Python/Go SDKs which
/// call urlsafe_b64encode without strip).
constexpr const char* kB64Url =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/// URL-safe base64 encoder. Pads remainder with '=' so the output is always
/// a multiple of 4 chars, matching Qiniu's reference implementation.
std::string base64_urlsafe(const unsigned char* data, std::size_t len) {
    std::string out;
    out.reserve(((len + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const unsigned int v = (static_cast<unsigned int>(data[i])     << 16) |
                               (static_cast<unsigned int>(data[i + 1]) <<  8) |
                                static_cast<unsigned int>(data[i + 2]);
        out += kB64Url[(v >> 18) & 0x3F];
        out += kB64Url[(v >> 12) & 0x3F];
        out += kB64Url[(v >>  6) & 0x3F];
        out += kB64Url[ v        & 0x3F];
    }

    const std::size_t rem = len - i;
    if (rem == 1) {
        const unsigned int v = static_cast<unsigned int>(data[i]) << 16;
        out += kB64Url[(v >> 18) & 0x3F];
        out += kB64Url[(v >> 12) & 0x3F];
        out += '=';
        out += '=';
    } else if (rem == 2) {
        const unsigned int v = (static_cast<unsigned int>(data[i])     << 16) |
                               (static_cast<unsigned int>(data[i + 1]) <<  8);
        out += kB64Url[(v >> 18) & 0x3F];
        out += kB64Url[(v >> 12) & 0x3F];
        out += kB64Url[(v >>  6) & 0x3F];
        out += '=';
    }
    return out;
}

std::string base64_urlsafe(const std::string& s) {
    return base64_urlsafe(reinterpret_cast<const unsigned char*>(s.data()),
                          s.size());
}

/// HMAC-SHA1 -> raw 20-byte digest.
std::vector<unsigned char> hmac_sha1(const std::string& key,
                                     const std::string& msg) {
    std::vector<unsigned char> digest(EVP_MAX_MD_SIZE);
    unsigned int len = 0;
    HMAC(EVP_sha1(),
         key.data(), static_cast<int>(key.size()),
         reinterpret_cast<const unsigned char*>(msg.data()),
         static_cast<int>(msg.size()),
         digest.data(), &len);
    digest.resize(len);  // SHA-1 -> 20 bytes
    return digest;
}

/// Read an env var, return "" if unset.
std::string env_or(const char* name, const char* fallback = "") {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string(fallback);
}

/// Map region id -> upload hostname.
/// Ref: https://developer.qiniu.com/kodo/manual/1671/region-endpoint
std::string region_to_upload_host(const std::string& region) {
    // Pattern: "up-<region>.qiniup.com" for all standard zones (z0/z1/z2,
    // na0, as0, cn-east-2, ...).
    if (region.empty()) return "https://up-z0.qiniup.com";
    return "https://up-" + region + ".qiniup.com";
}

}  // namespace

// ---------------------------------------------------------------------------
// KodoClient
// ---------------------------------------------------------------------------

KodoClient::KodoClient(KodoConfig cfg) : cfg_(std::move(cfg)) {}

KodoConfig KodoClient::from_env() {
    KodoConfig cfg;
    cfg.ak     = env_or("QINIU_AK");
    cfg.sk     = env_or("QINIU_SK");
    cfg.bucket = env_or("QINIU_BUCKET");
    cfg.region = env_or("QINIU_REGION", "z0");
    cfg.domain = env_or("QINIU_DOMAIN");
    return cfg;
}

bool KodoClient::env_ready() {
    return !env_or("QINIU_AK").empty()
        && !env_or("QINIU_SK").empty()
        && !env_or("QINIU_BUCKET").empty();
}

std::string KodoClient::make_upload_token(const std::string& key) const {
    // 1) Build PutPolicy JSON. Pinning scope to "<bucket>:<key>" prevents
    //    the token from being abused to overwrite OTHER objects in the
    //    same bucket -- even if it leaks during the demo, blast radius
    //    stays at this single key.
    nlohmann::json policy;
    policy["scope"]      = cfg_.bucket + ":" + key;
    policy["deadline"]   = static_cast<std::int64_t>(std::time(nullptr))
                         + cfg_.deadline_sec;
    policy["insertOnly"] = 0;  // allow overwrite so re-uploads of the same key work

    const std::string encoded = base64_urlsafe(policy.dump());

    // 2) Sign: HMAC-SHA1(SK, encoded) -> URL-safe base64.
    const auto sig_bytes = hmac_sha1(cfg_.sk, encoded);
    const std::string sign = base64_urlsafe(sig_bytes.data(), sig_bytes.size());

    // 3) Token = AK ":" sign ":" encoded.
    return cfg_.ak + ":" + sign + ":" + encoded;
}

UploadResult KodoClient::upload(const std::string& key,
                                const std::string& body,
                                const std::string& content_type) const {
    UploadResult result;

    if (cfg_.ak.empty() || cfg_.sk.empty() || cfg_.bucket.empty()) {
        result.error = "KODO: ak/sk/bucket not configured";
        return result;
    }

    const std::string token = make_upload_token(key);
    const std::string host  = region_to_upload_host(cfg_.region);

    spdlog::info("KODO: uploading key='{}' to {} ({} bytes)",
                 key, host, body.size());

    httplib::Client cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(30);
    cli.set_write_timeout(30);

    // 'file' MUST be the last item per Qiniu spec; cpp-httplib preserves
    // vector order, so this is safe.
    httplib::MultipartFormDataItems items = {
        {"token", token, "", ""},
        {"key",   key,   "", ""},
        {"file",  body,  key, content_type},
    };

    auto res = cli.Post("/", items);

    if (!res) {
        result.error = "KODO: HTTP transport error (code="
                     + std::to_string(static_cast<int>(res.error())) + ")";
        spdlog::error("{}", result.error);
        return result;
    }

    if (res->status != 200) {
        result.error = "KODO: HTTP " + std::to_string(res->status) + " -- "
                     + res->body;
        spdlog::error("{}", result.error);
        return result;
    }

    // Parse response JSON: {"key":"...","hash":"..."}
    try {
        auto j = nlohmann::json::parse(res->body);
        result.key  = j.value("key",  key);
        result.hash = j.value("hash", std::string{});
    } catch (const std::exception& e) {
        result.error = std::string("KODO: JSON parse error: ") + e.what()
                     + " | body=" + res->body;
        spdlog::error("{}", result.error);
        return result;
    }

    result.url = public_url(result.key);
    result.ok  = true;

    spdlog::info("KODO: upload OK -- key='{}' hash='{}' url='{}'",
                 result.key, result.hash, result.url);
    return result;
}

std::string KodoClient::public_url(const std::string& key) const {
    if (cfg_.domain.empty()) return {};
    return "https://" + cfg_.domain + "/" + key;
}

}  // namespace aipr::storage
