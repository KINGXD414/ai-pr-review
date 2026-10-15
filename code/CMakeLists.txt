cmake_minimum_required(VERSION 3.16)

project(ai-pr-review
        VERSION 0.2.0
        DESCRIPTION "AI-powered Pull Request review assistant (Day 2 — AST analyzer + rule engine)"
        LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Global compile options
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Strict warnings — verify the validation requirement (-Wall -Wextra, zero warning).
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ---------------------------------------------------------------------------
# System dependencies (installed through apt)
# ---------------------------------------------------------------------------
find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(nlohmann_json REQUIRED)

# ---------------------------------------------------------------------------
# libclang — used by analyzer module (Day 2). We pin to whichever version
# the user has installed (any of 12..18 works). No CMake config from LLVM
# is required: we only use the stable C API in <clang-c/Index.h>.
# ---------------------------------------------------------------------------
find_path(LIBCLANG_INCLUDE_DIR
          NAMES clang-c/Index.h
          PATHS /usr/lib/llvm-18/include
                /usr/lib/llvm-17/include
                /usr/lib/llvm-16/include
                /usr/lib/llvm-15/include
                /usr/lib/llvm-14/include
                /usr/lib/llvm-13/include
                /usr/lib/llvm-12/include
                /usr/include
          DOC "Path to clang-c/Index.h"
          REQUIRED)

find_library(LIBCLANG_LIBRARY
             NAMES clang-18 clang-17 clang-16 clang-15 clang-14 clang-13 clang-12 clang
             PATHS /usr/lib/llvm-18/lib
                   /usr/lib/llvm-17/lib
                   /usr/lib/llvm-16/lib
                   /usr/lib/llvm-15/lib
                   /usr/lib/llvm-14/lib
                   /usr/lib/llvm-13/lib
                   /usr/lib/llvm-12/lib
                   /usr/lib/x86_64-linux-gnu
             DOC "Path to libclang.so"
             REQUIRED)

message(STATUS "libclang include: ${LIBCLANG_INCLUDE_DIR}")
message(STATUS "libclang library: ${LIBCLANG_LIBRARY}")

# ---------------------------------------------------------------------------
# Third-party libraries fetched at configure time (no vcpkg, per task spec)
# ---------------------------------------------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# ---------------------------------------------------------------------------
# Local dependency override (third_party/) — build fully OFFLINE.
# Drop cpp-httplib / CLI11 / googletest sources into code/third_party/<dir>/
# and FetchContent uses them as-is, skipping all network access. Useful behind
# the GFW where GitHub / mirrors are unreachable or DNS-polluted.
# NOTE: FETCHCONTENT_SOURCE_DIR_<NAME> is case-sensitive — the suffix must be
# the UPPERCASE form of the FetchContent_Declare() name below.
# ---------------------------------------------------------------------------
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib/CMakeLists.txt")
    message(STATUS "Using local dependency: third_party/cpp-httplib (offline)")
    set(FETCHCONTENT_SOURCE_DIR_HTTPPLIB
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/cpp-httplib")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11/CMakeLists.txt")
    message(STATUS "Using local dependency: third_party/CLI11 (offline)")
    set(FETCHCONTENT_SOURCE_DIR_CLI11
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/CLI11")
endif()
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest/CMakeLists.txt")
    message(STATUS "Using local dependency: third_party/googletest (offline)")
    set(FETCHCONTENT_SOURCE_DIR_GOOGLETEST
        "${CMAKE_CURRENT_SOURCE_DIR}/third_party/googletest")
endif()

# ---------------------------------------------------------------------------
# GitHub mirror switch (for users behind GFW)
# ---------------------------------------------------------------------------
# Override at configure time:
#   cmake -B build -DGITHUB_MIRROR=https://kkgithub.com
# Not needed when third_party/ local overrides above are in place.
set(GITHUB_MIRROR "https://github.com"
    CACHE STRING "Mirror used for FetchContent GitHub clones (e.g. https://kkgithub.com)")
message(STATUS "Using GitHub mirror: ${GITHUB_MIRROR}")

# cpp-httplib v0.14.3 — header-only HTTP client/server with SSL & SSE support.
# Pinned to v0.14.3 because v0.15+ requires OpenSSL >= 3.0.0 while Ubuntu
# 20.04 ships OpenSSL 1.1.1f. v0.14.3 supports both OpenSSL 1.1.1 and 3.x.
FetchContent_Declare(
    httplib
    GIT_REPOSITORY ${GITHUB_MIRROR}/yhirose/cpp-httplib.git
    GIT_TAG        v0.14.3
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    cli11
    GIT_REPOSITORY ${GITHUB_MIRROR}/CLIUtils/CLI11.git
    GIT_TAG        v2.4.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY ${GITHUB_MIRROR}/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

set(BUILD_GMOCK   OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(httplib cli11 googletest)

# ---------------------------------------------------------------------------
# Core library
# ---------------------------------------------------------------------------
add_library(ai_pr_review_core STATIC
    src/diff/parser.cpp
    src/github/api_client.cpp
    src/gateway/llm_gateway.cpp
    src/analyzer/ast_analyzer.cpp
    src/rules/engine.cpp
    src/rag/retriever.cpp
    src/rag/embedder.cpp
    src/rag/vector_store.cpp
    src/prompt/skills.cpp
    src/agent/agent.cpp
    src/eval/runner.cpp
    src/storage/kodo_client.cpp
)

target_include_directories(ai_pr_review_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${LIBCLANG_INCLUDE_DIR}
)

target_compile_definitions(ai_pr_review_core PUBLIC
    CPPHTTPLIB_OPENSSL_SUPPORT
)

target_link_libraries(ai_pr_review_core PUBLIC
    httplib::httplib
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    fmt::fmt
    OpenSSL::SSL
    OpenSSL::Crypto
    SQLite::SQLite3
    Threads::Threads
    ${LIBCLANG_LIBRARY}
)

# ---------------------------------------------------------------------------
# CLI binary
# ---------------------------------------------------------------------------
add_executable(ai-pr-review src/main.cpp)
target_link_libraries(ai-pr-review PRIVATE
    ai_pr_review_core
    CLI11::CLI11
)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
enable_testing()
include(GoogleTest)

add_executable(diff_parser_tests tests/test_diff_parser.cpp)
target_link_libraries(diff_parser_tests PRIVATE
    ai_pr_review_core
    GTest::gtest_main
)
add_test(NAME diff_parser_tests COMMAND diff_parser_tests)

add_executable(ast_analyzer_tests tests/test_ast_analyzer.cpp)
target_compile_definitions(ast_analyzer_tests PRIVATE
    AIPR_TESTS_FIXTURES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures"
)
target_link_libraries(ast_analyzer_tests PRIVATE
    ai_pr_review_core
    GTest::gtest_main
)
add_test(NAME ast_analyzer_tests COMMAND ast_analyzer_tests)

add_executable(rule_engine_tests tests/test_rule_engine.cpp)
target_link_libraries(rule_engine_tests PRIVATE
    ai_pr_review_core
    GTest::gtest_main
)
add_test(NAME rule_engine_tests COMMAND rule_engine_tests)

# RAG retriever test — pure standard library, also buildable standalone with
# `g++ -std=c++17 -Isrc tests/test_rag.cpp src/rag/retriever.cpp -o test_rag`.
add_executable(rag_retriever_tests tests/test_rag.cpp
    src/rag/retriever.cpp
    src/rag/embedder.cpp
    src/rag/vector_store.cpp)
target_include_directories(rag_retriever_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rag_retriever_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME rag_retriever_tests COMMAND rag_retriever_tests)

# Agent loop (Function-Calling) test — drives run_agent() with a MockGateway
# (no network, no libclang call at runtime). Links the real core (incl. the
# real AST analyzer) so the static symbols resolve. NOTE: this test has its
# own main(), so it must NOT link GTest::gtest_main.
add_executable(agent_loop_tests tests/test_agent_mock.cpp)
target_link_libraries(agent_loop_tests PRIVATE ai_pr_review_core)
add_test(NAME agent_loop_tests COMMAND agent_loop_tests)

# Embedder + VectorStore (offline RAG building blocks) — pure standard library.
add_executable(rag_embedder_tests tests/test_vector_store.cpp
    src/rag/embedder.cpp
    src/rag/vector_store.cpp)
target_include_directories(rag_embedder_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(rag_embedder_tests PRIVATE nlohmann_json::nlohmann_json)
add_test(NAME rag_embedder_tests COMMAND rag_embedder_tests)

# Skill registry — wraps persona / self_critique / consensus as Skills.
# (prompt/*.h are header-only, so only skills.cpp is compiled here.)
add_executable(skills_tests tests/test_skills.cpp
    src/prompt/skills.cpp)
target_include_directories(skills_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
add_test(NAME skills_tests COMMAND skills_tests)

# ---------------------------------------------------------------------------
# Evaluation harness binary (Day 3)
# ---------------------------------------------------------------------------
add_executable(ai-pr-eval src/eval_main.cpp)
target_link_libraries(ai-pr-eval PRIVATE
    ai_pr_review_core
    CLI11::CLI11
)
