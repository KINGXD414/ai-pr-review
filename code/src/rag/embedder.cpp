/**
 * @file embedder.cpp
 * @brief HashingEmbedder implementation (offline, deterministic).
 */
#include "rag/embedder.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace aipr::rag {

namespace {

/// Decode a UTF-8 string into a sequence of Unicode code points.
std::vector<uint32_t> utf8_codepoints(const std::string& s) {
    std::vector<uint32_t> cps;
    cps.reserve(s.size());
    const unsigned char* p = reinterpret_cast<const unsigned char*>(s.data());
    const std::size_t n = s.size();
    std::size_t i = 0;
    while (i < n) {
        unsigned char c = p[i];
        uint32_t cp = 0;
        int len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c >> 5) == 0x06) { cp = c & 0x1F; len = 2; }
        else if ((c >> 4) == 0x0E) { cp = c & 0x0F; len = 3; }
        else if ((c >> 3) == 0x1E) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }  // invalid lead byte: skip
        if (i + len > n) break;  // truncated: stop
        for (int k = 1; k < len; ++k) {
            unsigned char cont = p[i + k];
            if ((cont >> 6) != 0x02) { cp = 0; len = 0; break; }
            cp = (cp << 6) | (cont & 0x3F);
        }
        if (len == 0) { ++i; continue; }
        // Skip characters that carry no lexical signal.
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r') { i += len; continue; }
        cps.push_back(cp);
        i += len;
    }
    return cps;
}

/// FNV-1a 64-bit hash, used to map an n-gram into a bucket index.
uint64_t fnv1a(const unsigned char* data, std::size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (std::size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace

std::vector<float> HashingEmbedder::embed(const std::string& text) const {
    std::vector<float> vec(dim_, 0.0f);

    // Lowercase the raw bytes first (ASCII casing), then derive code points.
    std::string lower = text;
    for (char& ch : lower) {
        if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
    }
    std::vector<uint32_t> cps = utf8_codepoints(lower);

    auto hash_gram = [&](uint32_t a, uint32_t b) {
        unsigned char buf[8];
        buf[0] = static_cast<unsigned char>((a >> 24) & 0xFF);
        buf[1] = static_cast<unsigned char>((a >> 16) & 0xFF);
        buf[2] = static_cast<unsigned char>((a >> 8) & 0xFF);
        buf[3] = static_cast<unsigned char>(a & 0xFF);
        buf[4] = static_cast<unsigned char>((b >> 24) & 0xFF);
        buf[5] = static_cast<unsigned char>((b >> 16) & 0xFF);
        buf[6] = static_cast<unsigned char>((b >> 8) & 0xFF);
        buf[7] = static_cast<unsigned char>(b & 0xFF);
        return fnv1a(buf, sizeof(buf)) % static_cast<uint64_t>(dim_);
    };

    // Unigrams (captures rare terms / CJK single characters).
    for (uint32_t cp : cps) {
        unsigned char buf[4];
        buf[0] = static_cast<unsigned char>((cp >> 24) & 0xFF);
        buf[1] = static_cast<unsigned char>((cp >> 16) & 0xFF);
        buf[2] = static_cast<unsigned char>((cp >> 8) & 0xFF);
        buf[3] = static_cast<unsigned char>(cp & 0xFF);
        uint64_t idx = fnv1a(buf, sizeof(buf)) % static_cast<uint64_t>(dim_);
        vec[idx] += 1.0f;
    }
    // Bigrams (captures adjacent-term semantics, e.g. "double free").
    for (std::size_t i = 1; i < cps.size(); ++i) {
        uint64_t idx = hash_gram(cps[i - 1], cps[i]);
        vec[idx] += 1.0f;
    }

    // L2 normalise so cosine == dot product and vectors are comparable.
    double norm = 0.0;
    for (float v : vec) norm += static_cast<double>(v) * v;
    norm = std::sqrt(norm);
    if (norm > 0.0) {
        for (float& v : vec) v = static_cast<float>(v / norm);
    }
    return vec;
}

}  // namespace aipr::rag
