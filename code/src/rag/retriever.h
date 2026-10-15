/**
 * @file retriever.h
 * @brief Lightweight RAG retriever (BM25 + optional embedding cosine).
 *
 * Pure standard library — no external vector DB, no network. This is the
 * retrieval half of our RAG pipeline: it indexes a small corpus of coding
 * rules + historical review snippets and returns the top-k most relevant
 * chunks for a given query, which the LLM gateway then splices into the
 * review prompt.
 *
 * Why BM25 instead of a heavyweight vector store: our corpus is tiny
 * (O(hundreds) of chunks), lives entirely in-process, and must build in
 * milliseconds without a sidecar service. BM25 is deterministic, needs no
 * model, and degrades gracefully on CJK text (tokenised on punctuation +
 * whitespace). An optional embedding path is provided for the "real" RAG
 * story: callers may supply precomputed vectors and switch to cosine search.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>
#include <unordered_map>

#include "rag/embedder.h"
#include "rag/vector_store.h"

namespace aipr::rag {

struct RetrievedChunk {
    std::string id;
    double      score{0.0};
};

class Retriever {
public:
    /// Add one document to the corpus. `id` must be unique; `text` is the
    /// searchable body (rule description, historical review, etc.).
    void add_document(const std::string& id, const std::string& text);

    /// Finalise the index (document frequencies, average length). Must be
    /// called after all add_document() calls and before search().
    void build();

    /// Lexical (BM25) search. Returns up to `top_k` chunks, highest score
    /// first. Empty query returns an empty vector.
    std::vector<RetrievedChunk> search(const std::string& query, int top_k = 5) const;

    /// Optional: attach a precomputed embedding for a document id.
    void set_embedding(const std::string& id, const std::vector<float>& vec);

    /// Vector (cosine) search over embeddings. Returns up to `top_k` chunks.
    /// If no embeddings are registered this returns empty.
    std::vector<RetrievedChunk> search_vector(const std::vector<float>& qvec,
                                             int top_k = 5) const;

    // ----- Semantic (embedding) path -------------------------------------
    /// Attach an embedder + an external vector store.  After index_embeddings()
    /// the retriever can answer semantic queries via cosine similarity.
    void set_embedder(EmbedderPtr embedder) { embedder_ = std::move(embedder); }
    void set_store(VectorStorePtr store) { store_ = std::move(store); }

    /// Embed every indexed document into the attached store.  No-op (and safe)
    /// when no embedder is attached.  Call after build().
    void index_embeddings();

    /// True when an embedder + store are wired and populated.
    bool has_semantic() const { return store_ && store_->size() > 0; }

    /// Embed `query` and run cosine search over the store.  Returns empty when
    /// the semantic path is not available.
    std::vector<RetrievedChunk> search_semantic(const std::string& query,
                                               int top_k = 5) const;

    /// Unified entry point used by tools/pipeline: semantic when available,
    /// lexical BM25 otherwise.  Guarantees a non-empty strategy always works.
    std::vector<RetrievedChunk> search_best(const std::string& query,
                                           int top_k = 5) const;

    size_t size() const { return ids_.size(); }
    bool   built() const { return built_; }

private:
    static std::vector<std::string> tokenize(const std::string& s);
    static double                  cosine(const std::vector<float>& a,
                                         const std::vector<float>& b);

    std::vector<std::string>              ids_;
    std::vector<std::string>              texts_;
    std::vector<std::vector<std::string>> tokens_;
    std::unordered_map<std::string, double> df_;
    std::vector<double>                   doc_len_;
    double                                avg_len_{0.0};
    bool                                  built_{false};

    std::unordered_map<std::string, std::vector<float>> emb_;

    EmbedderPtr    embedder_;
    VectorStorePtr store_;
};

}  // namespace aipr::rag
