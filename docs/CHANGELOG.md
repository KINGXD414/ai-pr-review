# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### Added
- Qiniu KODO object-storage integration (`code/src/storage/kodo_client.{h,cpp}`).
  - New CLI flag `--upload-kodo` uploads the rendered Markdown report after generation.
  - Hand-rolled upload-token signing (PutPolicy + HMAC-SHA1 + URL-safe base64); no Qiniu SDK dependency.
  - `PutPolicy.scope` is pinned to `bucket:key` so a leaked demo token can only overwrite the same key.
  - Credentials read from env `QINIU_AK` / `QINIU_SK` / `QINIU_BUCKET` / `QINIU_REGION` / `QINIU_DOMAIN`.
- Online markdown viewer (`docs/viewer.html`) hosted via GitHub Pages.
  - Pure client-side rendering with marked.js + highlight.js + github-markdown-css from CDN.
  - Works around Qiniu default-domain forced `Content-Disposition: attachment` by fetching the raw md cross-origin (KODO sets `Access-Control-Allow-Origin: *`) and rendering in-browser.
  - CLI now prints an extra `View   : <viewer>?url=<encoded>` line so reviewers can open the rendered report with one click.
  - New CLI flag `--kodo-viewer` lets users override the viewer base URL (default: `https://kingxd414.github.io/ai-pr-review/viewer.html`); empty string suppresses the `View` line.

### Changed
- README: added a new "☁️ KODO 对象存储集成 (v0.3)" section documenting the workflow and the safety design.
- README: documented the GitHub Pages viewer enablement flow (Settings -> Pages -> /docs branch).


See [Issue #27](https://github.com/KINGXD414/ai-pr-review/issues/27) for v0.3 backlog.

## [0.2.0] - 2026-05-31

First tagged release. Covers the full v0.1 + v0.2 development cycle: AST-aware
rule engine, JSON-driven evaluation harness, four review modes
(baseline / persona / self-critique / consensus), containerised deployment, and
a self-reviewing GitHub Actions workflow.

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
- Dockerfile + .dockerignore for containerised deployment (#25)
- GitHub Actions `release.yml`: build & publish linux-x64 binary + Docker image to ghcr.io on tag push (#25)
- GitHub Actions `pr_review.yml`: auto-comment AI review on every PR, sticky comment is updated in place (no comment spam)
- v0.2.0 evaluation matrix: 5 cases × 4 configs = 20 LLM calls, full real data backfilled into `code/eval/comparison_report.md` (#26)
- MIT LICENSE file with dual copyright (KINGXD414 + love-June)

### Changed

- Renamed `code/src/rule/` to `code/src/rules/` per Issue #1 convention (#6)
- ADR-001 v2: removed internal CLI parameter names, aligned persona descriptions with implementation (#12)
- README expanded with full deployment section (Docker + binary release) (#25)
- comparison_report v0.2.0: high-difficulty gain reported as +22.5% arithmetic mean across two cases (not a uniform +30%), to avoid over-generalising a single-case win (#26)

### Fixed

- Eval dataset: replaced 3 broken (404) PR URLs with verified merged PRs (#11)
- Eval dataset: removed `source_dir` field for portability (#11)

### Security

- DeepSeek API key passed via environment variable, never logged (#7)
- GitHub token handled through CLI flag with no persistent storage (#8)
- `pr_review.yml` reads `DEEPSEEK_API_KEY` from repository secrets, never committed to the repo

[Unreleased]: https://github.com/KINGXD414/ai-pr-review/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/KINGXD414/ai-pr-review/releases/tag/v0.2.0
