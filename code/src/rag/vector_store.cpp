/**
 * @file vector_store.cpp
 * @brief BruteForceStore implementation (exact cosine + JSON persistence).
 */
#include "rag/vector_store.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>

namespace aipr::rag {

namespace {
double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        dot += static_cast<double>(a[i]) * b[i];
        na  += static_cast<double>(a[i]) * a[i];
        nb  += static_cast<double>(b[i]) * b[i];
    }
    double denom = std::sqrt(na) * std::sqrt(nb);
    return denom > 0.0 ? (dot / denom) : 0.0;
}
}  // namespace

void BruteForceStore::add(const std::string& id,
                          const std::string& meta,
                          const std::vector<float>& vec) {
    // Overwrite on duplicate id.
    for (auto& it : items_) {
        if (it.id == id) {
            it.meta = meta;
            it.vec  = vec;
            return;
        }
    }
    items_.push_back({id, meta, vec});
}

std::vector<ScoredId> BruteForceStore::search(const std::vector<float>& q,
                                              int k) const {
    std::vector<ScoredId> out;
    out.reserve(items_.size());
    for (const auto& it : items_) {
        out.push_back({it.id, cosine(q, it.vec)});
    }
    std::sort(out.begin(), out.end(),
              [](const ScoredId& a, const ScoredId& b) {
                  return a.score > b.score;
              });
    if (k > 0 && static_cast<int>(out.size()) > k) out.resize(k);
    return out;
}

bool BruteForceStore::save(const std::string& path) const {
    nlohmann::json j = nlohmann::json::array();
    for (const auto& it : items_) {
        nlohmann::json item;
        item["id"]   = it.id;
        item["meta"] = it.meta;
        item["vec"]  = it.vec;
        j.push_back(item);
    }
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) return false;
    ofs << j.dump(2);
    return static_cast<bool>(ofs);
}

bool BruteForceStore::load(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) return false;
    std::stringstream ss;
    ss << ifs.rdbuf();
    try {
        nlohmann::json j = nlohmann::json::parse(ss.str());
        if (!j.is_array()) return false;
        items_.clear();
        for (const auto& item : j) {
            std::vector<float> vec =
                item.value<std::vector<float>>("vec", {});
            items_.push_back({item.value("id", ""), item.value("meta", ""), vec});
        }
        return true;
    } catch (...) {
        return false;
    }
}

}  // namespace aipr::rag
