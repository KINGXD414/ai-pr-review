cmake_minimum_required(VERSION 3.16)
project(ai-pr-review
    VERSION 0.2.0
    DESCRIPTION "AI-powered Pull Request review assistant (Day 2 — rule engine added)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(OpenSSL REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(fmt REQUIRED)
find_package(nlohmann_json REQUIRED)

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG        v0.14.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    cli11
    GIT_REPOSITORY https://github.com/CLIUtils/CLI11.git
    GIT_TAG        v2.4.1
    GIT_SHALLOW    TRUE
)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
    GIT_SHALLOW    TRUE
)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CLI11_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(httplib cli11 googletest)

add_library(ai_pr_review_core STATIC
    src/diff/parser.cpp
    src/github/api_client.cpp
    src/gateway/llm_gateway.cpp
    src/rule/engine.cpp
)

target_include_directories(ai_pr_review_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(ai_pr_review_core
    PUBLIC
        CPPHTTPLIB_OPENSSL_SUPPORT
)

target_link_libraries(ai_pr_review_core
    PUBLIC
        httplib::httplib
        nlohmann_json::nlohmann_json
        spdlog::spdlog
        fmt::fmt
        OpenSSL::SSL
        OpenSSL::Crypto
        SQLite::SQLite3
        Threads::Threads
)

add_executable(ai-pr-review src/main.cpp)
target_link_libraries(ai-pr-review
    PRIVATE
        ai_pr_review_core
        CLI11::CLI11
)

enable_testing()
include(GoogleTest)

add_executable(diff_parser_tests tests/test_diff_parser.cpp)
target_link_libraries(diff_parser_tests
    PRIVATE
        ai_pr_review_core
        GTest::gtest_main
)
gtest_discover_tests(diff_parser_tests)

add_executable(rule_engine_tests tests/test_rule_engine.cpp)
target_link_libraries(rule_engine_tests
    PRIVATE
        ai_pr_review_core
        GTest::gtest_main
)
gtest_discover_tests(rule_engine_tests)
