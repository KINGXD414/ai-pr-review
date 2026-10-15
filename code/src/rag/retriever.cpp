/**
 * @file retriever.cpp
 * @brief BM25 + cosine retrieval implementation for the RAG pipeline.
 */
#include "rag/retriever.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace aipr::rag {

namespace {

bool is_term_char(char c) {
    // ASCII alphanumerics pass; everything else (incl. CJK byte sequences,
    // punctuation, whitespace) is a delimiter.
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

}  // namespace

std::vector<std::string> Retriever::tokenize(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (is_term_char(c)) {
            cur.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        } else {
            if (!cur.empty()) {
                out.push_back(cur);
                cur.clear();
            }
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

void Retriever::add_document(const std::string& id, const std::string& text) {
    ids_.push_back(id);
    texts_.push_back(text);
    tokens_.push_back(tokenize(text));
    built_ = false;
}

void Retriever::build() {
    doc_len_.assign(tokens_.size(), 0.0);
    df_.clear();
    double total = 0.0;
    for (size_t i = 0; i < tokens_.size(); ++i) {
        std::map<std::string, int> tf;
        for (const auto& t : tokens_[i]) {
            tf[t]++;
        }
        for (const auto& kv : tf) {
            df_[kv.first] += 1.0;
        }
        doc_len_[i] = static_cast<double>(tokens_[i].size());
        total += doc_len_[i];
    }
    avg_len_ = tokens_.empty() ? 0.0 : total / static_cast<double>(tokens_.size());
    built_ = true;
}

std::vector<RetrievedChunk> Retriever::search(const std::string& query,
                                             int top_k) const {
    std::vector<RetrievedChunk> out;
    if (!built_ || tokens_.empty()) return out;

    const auto qterms = tokenize(query);
    if (qterms.empty()) return out;

    const double k1 = 1.5, b = 0.75;
    const double N = static_cast<double>(tokens_.size());

    for (size_t i = 0; i < tokens_.size(); ++i) {
        std::map<std::string, int> tf;
        for (const auto& t : tokens_[i]) tf[t]++;

        double score = 0.0;
        for (const auto& qt : qterms) {
            auto dit = df_.find(qt);
            if (dit == df_.end()) continue;  // term absent from whole corpus
            double idf = std::log(1.0 + (N - dit->second + 0.5) / (dit->second + 0.5));
            double f = static_cast<double>(tf[qt]);
            double denom = f + k1 * (1.0 - b + b * doc_len_[i] / avg_len_);
            score += idf * (f * (k1 + 1.0)) / denom;
        }
        if (score > 0.0) out.push_back({ids_[i], score});
    }

    std::sort(out.begin(), out.end(),
              [](const RetrievedChunk& a, const RetrievedChunk& b) {
                  return a.score > b.score;
              });
    if (top_k > 0 && static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

void Retriever::set_embedding(const std::string& id,
                             const std::vector<float>& vec) {
    emb_[id] = vec;
}

double Retriever::cosine(const std::vector<float>& a,
                        const std::vector<float>& b) {
    if (a.empty() || b.empty() || a.size() != b.size()) return 0.0;
    double dot = 0.0, na = 0.0, nb = 0.0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    if (na == 0.0 || nb == 0.0) return 0.0;
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

std::vector<RetrievedChunk> Retriever::search_vector(const std::vector<float>& qvec,
                                                   int top_k) const {
    std::vector<RetrievedChunk> out;
    if (emb_.empty() || qvec.empty()) return out;
    for (const auto& kv : emb_) {
        double s = cosine(qvec, kv.second);
        if (s > 0.0) out.push_back({kv.first, s});
    }
    std::sort(out.begin(), out.end(),
              [](const RetrievedChunk& a, const RetrievedChunk& b) {
                  return a.score > b.score;
              });
    if (top_k > 0 && static_cast<int>(out.size()) > top_k) out.resize(top_k);
    return out;
}

void Retriever::index_embeddings() {
    if (!embedder_) return;  // no embedder wired: nothing to do
    for (size_t i = 0; i < ids_.size(); ++i) {
        std::vector<float> vec = embedder_->embed(texts_[i]);
        if (store_) {
            store_->add(ids_[i], texts_[i], vec);
        } else {
            emb_[ids_[i]] = vec;  // fall back to in-object store
        }
    }
}

std::vector<RetrievedChunk> Retriever::search_semantic(const std::string& query,
                                                      int top_k) const {
    std::vector<RetrievedChunk> out;
    if (!embedder_ || query.empty()) return out;
    std::vector<float> qvec = embedder_->embed(query);
    if (store_) {
        for (const auto& s : store_->search(qvec, top_k)) {
            out.push_back({s.id, s.score});
        }
    } else if (!emb_.empty()) {
        out = search_vector(qvec, top_k);
    }
    return out;
}

std::vector<RetrievedChunk> Retriever::search_best(const std::string& query,
                                                  int top_k) const {
    if (has_semantic()) return search_semantic(query, top_k);
    return search(query, top_k);  // lexical BM25 fallback
}

}  // namespace aipr::rag
