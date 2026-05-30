# Rule Engine - Static Rule Scanner

## Overview

Rule Engine is the first check layer of the AI PR Review assistant. It scans PR code changes using regex patterns to quickly identify common risk patterns. No LLM calls needed, results in seconds, complementing the AI analysis layer.

## Supported Rules

| Rule Name | Detection | Risk Level |
|-----------|-----------|------------|
| unpaired_alloc | malloc/new without matching free/delete | High |
| virtual_modify | Modifying override/virtual functions | High |
| raw_ptr_new | Raw pointer new (should use make_unique) | Mid |
| c_style_cast | C-style cast (Type*)expr | Mid |
| memcpy_size | memcpy sizeof parameter is pointer not data | High |
| format_only | Formatting-only changes (spaces/indentation) | Low |
| test_file | Test file changes | Info |
| todo_fixme | Leftover TODO/FIXME comments | Mid |

## Output Format

```cpp
struct RuleHit {
    std::string rule_name;
    std::string file_path;
    int         line_number;
    std::string message;
};
```

`RuleEngine::analyze(const std::vector<FileDiff>& diffs)` returns `std::vector<RuleHit>`. One line may hit multiple rules.

## Build and Run

```bash
cd code/build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
./rule_engine_tests
```

## Test Coverage

- 16 Google Test cases covering all 8 rules (positive and negative)
- Multi-rule hit and empty input boundary tests
- Test file: code/tests/test_rule_engine.cpp

## Dependencies

- C++17, CMake 3.16+
- Depends on ai_pr_review_core static library
