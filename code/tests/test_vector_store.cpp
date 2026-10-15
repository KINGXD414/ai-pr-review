/**
 * @file test_vector_store.cpp
 * @brief Offline tests for the embedding + vector-store building blocks.
 *
 * Pure standard library + nlohmann/json — runs anywhere with g++ -std=c++17,
 * no network, no model, no libclang.  Exercises the JD's "Embedding / 向量数据库"
 * path deterministically.
 */
#include "rag/embedder.h"
#include "rag/vector_store.h"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

namespace {

int failures = 0;
#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::cerr << "FAIL: " << #cond << " @line " << __LINE__     \
                      << "\n";                                          \
            ++failures;                                                 \
        }                                                               \
    } while (0)

double l2_norm(const std::vector<float>& v) {
    double s = 0;
    for (float x : v) s += static_cast<double>(x) * x;
    return std::sqrt(s);
}

double cosine(const std::vector<float>& a, const std::vector<float>& b) {
    double dot = 0, na = 0, nb = 0;
    for (size_t i = 0; i < a.size(); ++i) {
        dot += a[i] * b[i];
        na += a[i] * a[i];
        nb += b[i] * b[i];
    }
    return dot / (std::sqrt(na) * std::sqrt(nb));
}

}  // namespace

int main() {
    using namespace aipr::rag;

    // ---- HashingEmbedder: dim, determinism, L2-normalisation ----
    HashingEmbedder emb(256);
    CHECK(emb.dim() == 256);
    auto v1 = emb.embed("double free in async callback");
    auto v2 = emb.embed("double free in async callback");
    CHECK(v1.size() == 256);
    CHECK(std::abs(l2_norm(v1) - 1.0) < 1e-5);
    CHECK(v1 == v2);  // deterministic

    // ---- Semantic similarity: related > unrelated ----
    auto a = emb.embed("memory leak malloc free smart pointer");
    auto b = emb.embed("memory leak new delete unique ptr");
    auto c = emb.embed("basketball game winner final score");
    double cos_ab = cosine(a, b);
    double cos_ac = cosine(a, c);
    CHECK(cos_ab > cos_ac);  // same topic should beat unrelated topic

    // ---- BruteForceStore: add + search ordering ----
    BruteForceStore store;
    CHECK(store.size() == 0);
    store.add("kb_leak", "memory leak", a);
    store.add("kb_sport", "basketball", c);
    CHECK(store.size() == 2);
    auto top = store.search(a, 1);
    CHECK(top.size() == 1);
    CHECK(top[0].id == "kb_leak");  // nearest neighbour of `a` is itself
    CHECK(top[0].score > 0.99);

    // ---- BruteForceStore: save / load round trip ----
    const char* idx = "test_vector_index.json";
    CHECK(store.save(idx));
    BruteForceStore reloaded;
    CHECK(reloaded.load(idx));
    CHECK(reloaded.size() == 2);
    auto top2 = reloaded.search(a, 2);
    CHECK(top2.size() == 2);
    CHECK(top2[0].id == "kb_leak");
    std::remove(idx);

    // ---- VectorStore interface seam (hnswlib) is satisfied by BruteForce ----
    VectorStorePtr ptr = std::make_shared<BruteForceStore>();
    ptr->add("x", "m", a);
    CHECK(ptr->search(a, 1)[0].id == "x");

    if (failures == 0) {
        std::cout << "ALL VECTOR STORE / EMBEDDER TESTS PASSED\n";
        return 0;
    }
    std::cerr << failures << " CHECK(s) failed\n";
    return 1;
}
