/**
 * @file eval/runner.cpp
 * @brief Implementation of the AI PR Review evaluation harness.
 */
#include "eval/runner.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace aipr::eval {

namespace fs = std::filesystem;
using nlohmann::json;

namespace {

std::string read_file_or_empty(const fs::path& p) {
    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) return {};
    std::ostringstream oss;
    oss << ifs.rdbuf();
    return oss.str();
}

/// Case-insensitive substring search. Empty needle => match.
bool contains_ci(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return std::tolower(static_cast<unsigned char>(a)) ==
                   std::tolower(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

/// Sanitize an id so it can be used as a filesystem path segment.
std::string sanitize_id(const std::string& id) {
    std::string out;
    out.reserve(id.size());
    for (char c : id) {
        if (std::isalnum(static_cast<unsigned char>(c)) ||
            c == '-' || c == '_') {
            out.push_back(c);
        } else {
            out.push_back('_');
        }
    }
    return out;
}

/// Single-quote a string for safe inclusion in a /bin/sh command line.
std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

}  // namespace

std::vector<CaseSpec> load_cases(const std::string& file) {
    std::ifstream ifs(file);
    if (!ifs) {
        throw std::runtime_error("Cannot open cases file: " + file);
    }
    json j;
    try {
        ifs >> j;
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("JSON parse error: ") + e.what());
    }

    if (!j.contains("cases") || !j.at("cases").is_array()) {
        throw std::runtime_error(
            "cases.json must contain top-level 'cases' array");
    }

    std::vector<CaseSpec> specs;
    for (const auto& jc : j.at("cases")) {
        CaseSpec c;
        c.id = jc.at("id").get<std::string>();
        c.name = jc.value("name", c.id);
        c.pr_url = jc.at("pr_url").get<std::string>();
        c.source_dir = jc.value("source_dir", std::string{});
        c.difficulty = jc.value("difficulty", std::string{"unknown"});
        if (jc.contains("expected_keywords")) {
            c.expected_keywords = jc.at("expected_keywords")
                                      .get<std::vector<std::string>>();
        }
        if (jc.contains("expected_severity_appears")) {
            c.expected_severity_appears =
                jc.at("expected_severity_appears")
                    .get<std::vector<std::string>>();
        }
        if (jc.contains("must_not_hallucinate")) {
            c.must_not_hallucinate =
                jc.at("must_not_hallucinate")
                    .get<std::vector<std::string>>();
        }
        specs.push_back(std::move(c));
    }
    return specs;
}

CaseResult run_one_case(const CaseSpec& spec, const RunOptions& opts) {
    CaseResult r;
    r.id = spec.id;

    fs::path out_dir(opts.out_dir);
    std::error_code ec;
    fs::create_directories(out_dir, ec);
    if (ec) {
        r.exec_error = "cannot create out_dir: " + ec.message();
        return r;
    }
    fs::path out_file = out_dir / (sanitize_id(spec.id) + ".md");

    std::ostringstream cmd;
    cmd << shell_quote(opts.binary)
        << " --pr " << shell_quote(spec.pr_url)
        << " --output " << shell_quote(out_file.string());
    if (!spec.source_dir.empty()) {
        cmd << " --source-dir " << shell_quote(spec.source_dir);
    }
    if (!opts.model.empty()) {
        cmd << " --model " << shell_quote(opts.model);
    }
    if (!opts.github_token.empty()) {
        cmd << " --github-token " << shell_quote(opts.github_token);
    }
    if (!opts.llm_key.empty()) {
        cmd << " --llm-key " << shell_quote(opts.llm_key);
    }
    // Silence binary output; the harness emits its own progress logs.
    cmd << " > /dev/null 2>&1";

    spdlog::info("[{}] running: {}", spec.id, spec.pr_url);

    auto t0 = std::chrono::steady_clock::now();
    int rc = opts.dry_run ? 0 : std::system(cmd.str().c_str());
    auto t1 = std::chrono::steady_clock::now();
    r.elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                       t1 - t0).count();

    if (rc != 0) {
        r.exec_ok = false;
        r.exec_error = "binary exited with rc=" + std::to_string(rc);
        spdlog::warn("[{}] {}", spec.id, r.exec_error);
        return r;
    }
    if (opts.dry_run) {
        r.exec_ok = true;
        r.review_excerpt = "(dry-run; binary was not invoked)";
        return r;
    }

    std::string review = read_file_or_empty(out_file);
    if (review.empty()) {
        r.exec_ok = false;
        r.exec_error = "review file empty: " + out_file.string();
        return r;
    }
    r.exec_ok = true;

    // ---- keyword score ----
    r.kw_total = spec.expected_keywords.size();
    for (const auto& kw : spec.expected_keywords) {
        if (contains_ci(review, kw)) ++r.kw_hit;
    }
    r.score_keyword = r.kw_total == 0
                          ? 1.0
                          : static_cast<double>(r.kw_hit) /
                                static_cast<double>(r.kw_total);

    // ---- severity score ----
    r.sev_total = spec.expected_severity_appears.size();
    for (const auto& sv : spec.expected_severity_appears) {
        if (contains_ci(review, sv)) ++r.sev_hit;
    }
    r.score_severity = r.sev_total == 0
                           ? 1.0
                           : static_cast<double>(r.sev_hit) /
                                 static_cast<double>(r.sev_total);

    // ---- no-hallucination score (1.0 == no banned token seen) ----
    r.hallu_total = spec.must_not_hallucinate.size();
    for (const auto& h : spec.must_not_hallucinate) {
        if (contains_ci(review, h)) ++r.hallu_hit;
    }
    r.score_no_hallu = r.hallu_total == 0
                           ? 1.0
                           : 1.0 - static_cast<double>(r.hallu_hit) /
                                       static_cast<double>(r.hallu_total);

    // ---- weighted total: 40 / 30 / 30 ----
    r.score_total = 0.4 * r.score_keyword +
                    0.3 * r.score_severity +
                    0.3 * r.score_no_hallu;

    constexpr std::size_t kExcerpt = 400;
    r.review_excerpt = review.substr(
        0, std::min<std::size_t>(review.size(), kExcerpt));
    return r;
}

void write_report(const std::vector<CaseResult>& results,
                  const std::vector<CaseSpec>& specs,
                  const RunOptions& opts) {
    fs::path rp(opts.report_path);
    if (rp.has_parent_path()) {
        std::error_code ec;
        fs::create_directories(rp.parent_path(), ec);
    }
    std::ofstream ofs(rp, std::ios::binary | std::ios::trunc);
    if (!ofs) {
        throw std::runtime_error("Cannot open report: " + opts.report_path);
    }

    ofs << "# AI PR Review 评测报告\n\n"
        << "> 由 `ai-pr-eval` 自动生成。每条 case 以"
           "「关键词命中 40% + 严重度匹配 30% + 无幻觉 30%」"
           "加权计分。\n\n"
        << "| 项 | 值 |\n"
        << "| --- | --- |\n"
        << "| 用例数 | " << results.size() << " |\n"
        << "| 模型 | " << (opts.model.empty() ? "default" : opts.model)
        << " |\n"
        << "| 二进制 | `" << opts.binary << "` |\n"
        << "| 输出目录 | `" << opts.out_dir << "` |\n\n";

    ofs << "## 总览\n\n"
        << "| ID | 难度 | 关键词 | 严重度 | 无幻觉 | 综合 | 耗时(ms) | 状态 |\n"
        << "| --- | --- | --- | --- | --- | --- | --- | --- |\n";
    double sum = 0.0;
    std::size_t ok_cnt = 0;
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const auto& s = specs[i];
        ofs << "| `" << r.id << "` | " << s.difficulty
            << " | " << r.kw_hit << "/" << r.kw_total
            << " | " << r.sev_hit << "/" << r.sev_total
            << " | " << (r.hallu_total - r.hallu_hit) << "/" << r.hallu_total
            << " | "
            << (r.exec_ok
                    ? std::to_string(static_cast<int>(r.score_total * 100)) +
                          "%"
                    : "—")
            << " | " << r.elapsed_ms
            << " | " << (r.exec_ok ? "✅" : "❌ " + r.exec_error)
            << " |\n";
        if (r.exec_ok) {
            sum += r.score_total;
            ++ok_cnt;
        }
    }
    double overall = ok_cnt == 0 ? 0.0
                                 : sum / static_cast<double>(ok_cnt);
    ofs << "\n**整体平均分（仅计入执行成功用例）**: **"
        << static_cast<int>(overall * 100) << "%** ("
        << ok_cnt << "/" << results.size() << " ok)\n\n";

    ofs << "## 每条用例详情\n\n";
    for (std::size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        const auto& s = specs[i];
        ofs << "### `" << r.id << "` — " << s.name << "\n\n"
            << "- PR: " << s.pr_url << "\n"
            << "- 难度: " << s.difficulty << "\n"
            << "- 执行: " << (r.exec_ok ? "✅" : "❌ " + r.exec_error)
            << "\n"
            << "- 关键词命中: " << r.kw_hit << "/" << r.kw_total << " — `[";
        for (std::size_t k = 0; k < s.expected_keywords.size(); ++k) {
            ofs << (k ? ", " : "") << s.expected_keywords[k];
        }
        ofs << "]`\n"
            << "- 严重度匹配: " << r.sev_hit << "/" << r.sev_total << "\n"
            << "- 幻觉命中: " << r.hallu_hit << "/" << r.hallu_total
            << "（越少越好）\n"
            << "- 综合分: " << static_cast<int>(r.score_total * 100)
            << "%\n\n";
        if (r.exec_ok && !r.review_excerpt.empty()) {
            ofs << "**review 预览（前 400 字）**:\n\n```\n"
                << r.review_excerpt << "\n```\n\n";
        }
    }
}

}  // namespace aipr::eval
