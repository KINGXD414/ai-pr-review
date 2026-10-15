/**
 * @file embedder.h
 * @brief Embedding abstraction for the RAG pipeline.
 *
 * JD asks for "Embedding / 向量数据库".  The clean way to satisfy that is an
 * interface, not a single hard-coded provider:
 *
 *   - HashingEmbedder : offline, deterministic, zero-dependency.  It hashes
 *     character n-grams into a fixed-dimensional, L2-normalised bag-of-features
 *     vector.  No model, no network, no API key — so the *entire* embedding +
 *     cosine-similarity path is runnable and demonstrable offline.  This is what
 *     we ship as the default so the project never hard-fails on a missing key.
 *
 *   - RemoteEmbedder (seam only, see note below) : drop-in for a real OpenAI-
 *     compatible embedding API (e.g. siliconflow bge-m3).  Implement the same
 *     interface and the rest of the pipeline is unchanged.
 *
 * Swapping providers is a one-line change at the call site (main.cpp), which is
 * exactly the point of the abstraction: the vector store and retriever do not
 * care where the floats come from.
 */
#pragma once

#include <memory>
#include <string>
#include <vector>

namespace aipr::rag {

/// A text -> dense vector encoder.
class Embedder {
public:
    virtual ~Embedder() = default;

    /// Dimensionality of the produced vectors (must be constant per instance).
    virtual int dim() const = 0;

    /// Encode `text` into a vector of length dim().
    virtual std::vector<float> embed(const std::string& text) const = 0;
};

using EmbedderPtr = std::shared_ptr<Embedder>;

/// Offline, deterministic lexical embedder.
///
/// Approach: normalise the text (lowercase, strip casing), walk it as a stream
/// of Unicode code points, form character bigrams (and unigrams for very short
/// inputs), hash each into a bucket of size `dim_`, accumulate counts, then L2
/// normalise.  Two texts sharing terminology land in overlapping buckets, so
/// cosine similarity tracks lexical/semantic relatedness well enough to drive
/// top-k retrieval — without any external model.
///
/// This is intentionally simple; its job is to make the embedding+ANN path
/// *tangible and testable*, not to win a MTEB leaderboard.  Replace with
/// RemoteEmbedder for production quality.
class HashingEmbedder : public Embedder {
public:
    explicit HashingEmbedder(int dim = 256) : dim_(dim > 0 ? dim : 256) {}

    int dim() const override { return dim_; }

    std::vector<float> embed(const std::string& text) const override;

private:
    int dim_;
};

// ---------------------------------------------------------------------------
// Seam for a production embedder (NOT implemented — interface only).
//
// class RemoteEmbedder : public Embedder {
//   // POST https://api.siliconflow.cn/v1/embeddings  (OpenAI-compatible)
//   // body: {"model":"BAAI/bge-m3","input":[text]}
//   // parse data[0].embedding -> std::vector<float>
//   // reuse cpp-httplib (already a dependency) + the LLM retry logic.
// };
// To enable: implement this class, then in main.cpp do
//   EmbedderPtr e = std::make_shared<RemoteEmbedder>(api_key);
// and nothing else changes.
// ---------------------------------------------------------------------------

}  // namespace aipr::rag
