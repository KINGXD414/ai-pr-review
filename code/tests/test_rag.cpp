/**
 * @file test_rag.cpp
 * @brief Standalone unit test for the BM25 + cosine retriever.
 *
 * Compiles with a plain C++17 compiler and NO external dependencies — it
 * only needs retriever.h/.cpp. This lets us verify the RAG retrieval core
 * (the part we are adding) on any machine, including the WorkBuddy sandbox
 * where libclang / OpenSSL are absent.
 *
 * Build & run:
 *   g++ -std=c++17 -Isrc tests/test_rag.cpp src/rag/retriever.cpp -o test_rag
 *   ./test_rag
 */
#include "rag/retriever.h"
#include "rag/embedder.h"
#include "rag/vector_store.h"

#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

namespace {

int g_failures = 0;
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
            ++g_failures;                                                      \
        }                                                                      \
    } while (0)

}  // namespace

int main() {
    using aipr::rag::Retriever;
    using aipr::rag::RetrievedChunk;

    Retriever r;
    r.add_document("rule_unpaired_alloc",
                   "malloc new free delete memory leak unpaired_alloc");
    r.add_document("rule_raw_ptr_new", "raw pointer new make_unique shared_ptr");
    r.add_document("rule_c_style_cast", "c style cast static_cast reinterpret_cast");
    r.add_document("rule_memcpy_size",
                   "memcpy memset sizeof pointer array buffer overflow");
    r.add_document("hist_spdl3594",
                   "exception safety async callback double free after reconnect");
    r.build();

    CHECK(r.size() == 5);
    CHECK(r.built());

    // BM25: a memory-leak query should rank the malloc rule first.
    auto hits = r.search("unpaired malloc free memory leak", 3);
    CHECK(!hits.empty());
    CHECK(hits[0].id == "rule_unpaired_alloc");
    CHECK(hits[0].score > 0.0);

    // BM25: a cast query should surface the c_style_cast rule.
    auto hits2 = r.search("use static_cast instead of c style cast", 2);
    CHECK(hits2[0].id == "rule_c_style_cast");

    // BM25: top_k truncation respected.
    auto hits3 = r.search("memory leak free", 1);
    CHECK(hits3.size() == 1);

    // No tooling crash on empty query.
    auto hits4 = r.search("", 5);
    CHECK(hits4.empty());

    // --- Embedding / cosine path ---
    Retriever r2;
    r2.add_document("a", "irrelevant placeholder text");
    r2.add_document("b", "irrelevant placeholder text");
    r2.set_embedding("a", {1.0f, 0.0f, 0.0f});
    r2.set_embedding("b", {0.0f, 1.0f, 0.0f});
    auto vhits = r2.search_vector({1.0f, 0.0f, 0.0f}, 2);
    CHECK(!vhits.empty());
    CHECK(vhits[0].id == "a");
    CHECK(std::fabs(vhits[0].score - 1.0) < 1e-6);

    // Orthogonal query yields low score.
    auto vhits2 = r2.search_vector({0.0f, 1.0f, 0.0f}, 1);
    CHECK(vhits2[0].id == "b");

    // --- End-to-end semantic path: embed corpus + query via cosine ---
    Retriever r3;
    r3.add_document("kb_leak",
                    "memory leak malloc free smart pointer raii unpaired alloc");
    r3.add_document("kb_cast",
                    "c style cast static_cast reinterpret_cast safer conversion");
    r3.add_document("kb_sport", "basketball game winner final score quarter");
    r3.build();

    auto embedder = std::make_shared<aipr::rag::HashingEmbedder>(256);
    auto store = std::make_shared<aipr::rag::BruteForceStore>();
    r3.set_embedder(embedder);
    r3.set_store(store);
    r3.index_embeddings();
    CHECK(r3.has_semantic());

    // search_best picks the semantic path (store populated) over BM25.
    auto sem = r3.search_best("memory leak free smart pointer", 2);
    CHECK(!sem.empty());
    CHECK(sem[0].id == "kb_leak");

    // An unrelated query routes to the right bucket too.
    auto sem2 = r3.search_best("basketball final score", 1);
    CHECK(sem2[0].id == "kb_sport");

    if (g_failures == 0) {
        std::printf("ALL RAG RETRIEVER TESTS PASSED\n");
        return 0;
    }
    std::printf("%d RAG RETRIEVER TEST(S) FAILED\n", g_failures);
    return 1;
}
