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

> 数据来源：`code/eval/baseline_report.md`，ai-pr-eval 于 2026-05-30 18:36 真实运行产出。

| Case ID | 难度 | 关键词 | 严重度 | 无幻觉 | 综合 | 耗时 |
|---------|------|--------|--------|--------|------|------|
| muduo-729 | low | 4/4 | 0/1 | 3/3 | 70% | 11s |
| muduo-505 | high | 3/3 | 2/2 | 2/2 | 100% | 8s |
| spdlog-3594 | high | — | — | — | — | — |
| json-5187 | medium | — | — | — | — | — |
| spdlog-3541 | medium | — | — | — | — | — |

**已跑通 2 条：整体平均 85%（2/2 ok）**

### 分析

- **muduo-729（low）**：关键词命中完美，无幻觉满分，但严重度标签未出现"低"。可能原因：模型输出倾向于给出更严重的评估，对低风险变更的降级表达不够。
- **muduo-505（high）**：三项满分。高并发/原子性话题 LLM 表现优秀。

### 局限

- Baseline 只跑了 2 条 case（muduo-729, muduo-505），新增 3 条（spdlog-3594, json-5187, spdlog-3541）尚未跑通。
- 85% 基于 2/5 样本，统计代表性不足。Day-3 补齐 5 条 case 后数据会更稳健。
- 评测 harness 是黑盒调用 `ai-pr-review` 二进制，需要 DeepSeek API key 才能跑完。

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
| --self-critique | 持平 | 显著提升（二审校准） | 显著提升（幻觉检测任务） | 85% → 95%+ |
| --consensus N=3 | 持平 | 提升（多票取共识） | 提升（单票丢弃过滤过度建议） | 85% → 90%+ |

> 以上预期基于功能机制分析，实际数据待评测矩阵跑完后填入。

## 4. v0.2.0 评测矩阵

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
./ai-pr-eval --cases ../eval/cases.json \
             --binary ./ai-pr-review \
             [--persona linus | --self-critique | --consensus 3] \
             --github-token $GITHUB_TOKEN \
             --llm-key $DEEPSEEK_KEY \
             --out-dir ../eval/runs/<config-name>
```

## 5. 结论与下一步

- v0.1.0 baseline 85%（2/2 cases 已跑通），low 难度严重度表达有改善空间。
- v0.2.0 三招中 self-critique 对无幻觉维度提升最直接，预期综合分可达 95%+。
- Day-3 需补齐 5 cases × 4 configs 完整矩阵后更新本报告。

---

*本报告随评测结果持续更新。*
