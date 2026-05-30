# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added

- AST analyzer: libclang-based C/C++ context extractor for PR review (#2)
- Rule engine: 8 static analysis rules (unpaired_alloc, virtual_modify, format_only, test_file, todo_fixme, raw_ptr_new, c_style_cast, memcpy_size) (#3)
- Evaluation dataset with 8 PR annotations across difficulty levels (#4)
- AST context wired into LLM prompt via `--source-dir` flag (#7)
- JSON-driven evaluation harness `ai-pr-eval` with three-axis scoring (#8)
- Eval cases expanded to 5 (2 high, 2 medium, 1 low) with verified PR URLs (#9, #11)
- Multi-persona reviewer: `--persona linus|mentor|devil|zen` (#13)
- Two-pass self-critique review: `--self-critique` (#14)
- Multi-vote consensus review: `--consensus N` (#15)
- RuleHit struct with rule_id / severity / file / line / message / suggestion (#18)
- Engine::run(ParsedDiff, AstSummary*) interface (#18)
- Severity enum class (Info / Warn / Error) with severity_str() helper (#18)

### Changed

- Renamed `code/src/rule/` to `code/src/rules/` per Issue #1 convention (#6)
- ADR-001 v2: removed internal CLI parameter names, aligned persona descriptions with implementation (#12)

### Fixed

- Eval dataset: replaced 3 broken (404) PR URLs with verified merged PRs (#11)
- Eval dataset: removed `source_dir` field for portability (#11)

### Security

- DeepSeek API key passed via environment variable, never logged (#7)
- GitHub token handled through CLI flag with no persistent storage (#8)

[Unreleased]: https://github.com/KINGXD414/ai-pr-review/compare/main...HEAD
