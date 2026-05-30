# AI PR Review 评测对比报告：v0.1.0 vs v0.2.0

> 作者：love-June | 日期：2026-05-30 | Refs #1

## 1. 评测体系

三维加权计分（对齐 ADR-002）：

| 维度 | 权重 | 说明 |
|------|------|------|
| 关键词命中 (keyword hit-rate) | 40% | 期望关键词在 review 报告中出现的比例 |
| 严重度匹配 (severity match) | 30% | review 中是否出现期望的严重度标签 |
| 无幻觉 (no-hallucination) | 30% | review 中未出现禁止的幻觉关键词的比例 |

评测集：`eval/cases.json`，5 条 case（2 高 2 中 1 低）。

## 2. v0.1.0 Baseline 结果

> **状态：待跑** — baseline 评测尚未执行，以下为占位表，将在实际运行后替换。

| Case ID | 难度 | 关键词 | 严重度 | 无幻觉 | 综合 | 耗时 |
|---------|------|--------|--------|--------|------|------|
| muduo-729 | low | — | — | — | — | — |
| muduo-505 | high | — | — | — | — | — |
| spdlog-3594 | high | — | — | — | — | — |
| json-5187 | medium | — | — | — | — | — |
| spdlog-3541 | medium | — | — | — | — | — |

> 5 条 case 的 baseline 数据需通过 `ai-pr-eval` 实际运行后填入。运行日志和 JSON 输出将存放于 `eval/runs/baseline/`。

## 3. v0.2.0 差异化功能

v0.2.0 新增三招（互斥，不可同时启用）：

| 功能 | CLI Flag | 核心思路 |
|------|----------|----------|
| 人格化 Reviewer | `--persona linus|mentor|devil|zen` | 不同人设影响输出语气和关注重点 |
| Self-Critique | `--self-critique` | 一审 → 二审反思链，校准幻觉/严重度/建议 |
| Consensus | `--consensus N` | N 次独立 review + meta-review 合并，提高置信度 |

### 预期影响

| 功能 | 关键词命中 | 严重度匹配 | 无幻觉 | 综合预期 |
|------|-----------|-----------|--------|----------|
| --persona | 持平~略升 | 语气人设可能改变严重度表达 | linus 可能越界风险 | 取决于人设 |
| --self-critique | 持平 | 显著提升（二审校准） | 显著提升（幻觉检测任务） | 预期提升显著 |
| --consensus N=3 | 持平 | 提升（多票取共识） | 提升（单票丢弃过滤过度建议） | 预期提升 |

> 以上为基于功能机制的预期分析，实际数据待评测矩阵跑完后填入。

## 4. 评测矩阵

需要跑以下矩阵（共 5 cases × 4 configs = 20 次 LLM 调用组）：

| Config | Flag | Case 数 |
|--------|------|---------|
| baseline v0.2.0 | （无 flag） | 5 |
| persona-linus | `--persona linus` | 5 |
| self-critique | `--self-critique` | 5 |
| consensus-3 | `--consensus 3` | 5 |

**执行命令（每条）：**

```bash
cd code/build
./ai-pr-eval --cases ../../eval/cases.json \
             --binary ./ai-pr-review \
             [--persona linus | --self-critique | --consensus 3] \
             --github-token $GITHUB_TOKEN \
             --llm-key $DEEPSEEK_KEY \
             --out-dir ../../eval/runs/<config-name>
```

## 5. 下一步

- [ ] 运行 baseline（5 cases × 1 config）
- [ ] 运行三招评测（5 cases × 3 configs）
- [ ] 填入实际数据，替换占位符
- [ ] 对比分析 v0.1.0 vs v0.2.0 各维度得分

---

*本报告随评测结果持续更新。数据均为实际运行产出，不做估算。*
