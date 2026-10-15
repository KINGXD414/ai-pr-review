/**
 * @file vector_store.h
 * @brief Vector index abstraction for the RAG pipeline.
 *
 * JD asks for "向量数据库".  We model it as an interface so the storage backend
 * is a swappable detail:
 *
 *   - BruteForceStore : in-memory exact cosine search.  For our corpus size
 *     (O(hundreds) of rule/history chunks) this is optimal — no index overhead,
 *     O(n) scan is sub-millisecond, and it is trivially correct.  Supports
 *     save()/load() to a JSON file, so the "vector database" is a real on-disk
 *     artifact the project can build and reload.
 *
 *   - (Seam) HnswStore : implement VectorStore over hnswlib for million-scale
 *     corpora.  The retriever and main.cpp call only the interface, so swapping
 *     BruteForceStore -> HnswStore is a one-line change.  This is how we answer
 *     "what is HNSW / efSearch" in an interview without paying the integration
 *     cost up front.
 */
#pragma once

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

namespace aipr::rag {

/// A single indexed item: id + free-form metadata + dense vector.
struct VectorItem {
    std::string              id;
    std::string              meta;
    std::vector<float>       vec;
};

/// Cosine-sorted search result.
struct ScoredId {
    std::string id;
    double      score;  // cosine similarity in [-1, 1]
};

/// Abstract vector index.
class VectorStore {
public:
    virtual ~VectorStore() = default;

    /// Insert one item.  If `id` already exists it is overwritten.
    virtual void add(const std::string& id,
                     const std::string& meta,
                     const std::vector<float>& vec) = 0;

    /// Return up to `k` items most similar to `q` by cosine similarity,
    /// highest score first.
    virtual std::vector<ScoredId> search(const std::vector<float>& q,
                                        int k) const = 0;

    virtual std::size_t size() const = 0;

    /// Persist the index to `path`.  Default: unsupported.
    virtual bool save(const std::string& path) const {
        (void)path;
        return false;
    }
    /// Load the index from `path`.  Default: unsupported.
    virtual bool load(const std::string& path) {
        (void)path;
        return false;
    }
};

using VectorStorePtr = std::shared_ptr<VectorStore>;

/// In-memory exact cosine index with JSON serialisation.
class BruteForceStore : public VectorStore {
public:
    void add(const std::string& id,
             const std::string& meta,
             const std::vector<float>& vec) override;

    std::vector<ScoredId> search(const std::vector<float>& q,
                                 int k) const override;

    std::size_t size() const override { return items_.size(); }

    bool save(const std::string& path) const override;
    bool load(const std::string& path) override;

private:
    std::vector<VectorItem> items_;
};

}  // namespace aipr::rag
