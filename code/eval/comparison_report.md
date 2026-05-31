# AI PR Review 评测对比报告：v0.1.0 vs v0.2.0

> 作者：Turkey + June（数据：Turkey 实测；体系：June 设计）
> 最后更新：2026-05-31 09:43（v0.2.0 全矩阵跑完）
> Refs: Issue #1, PR #13/#14/#15/#24

## 1. 评测体系

三维加权计分（沿用 ADR-002）：

| 维度 | 权重 | 说明 |
|------|------|------|
| 关键词命中 (keyword hit-rate) | 40% | 期望关键词在 review 报告中出现的比例 |
| 严重度匹配 (severity match) | 30% | review 中是否出现期望的严重度标签 |
| 无幻觉 (no-hallucination) | 30% | review 中未出现禁止的幻觉关键词的比例 |

- 评测集：`code/eval/cases.json`（5 条 PR：2 高 / 2 中 / 1 低）
- harness：`ai-pr-eval` 二进制，每个 case 独立 sandbox 运行
- 调用模型：`deepseek-chat`
- 真实运行环境：Ubuntu 20.04 / xd-virtual-machine / 2026-05-31

## 2. v0.1.0 Baseline（历史）

| Case ID | 难度 | 综合 |
|---------|------|------|
| muduo-729 | low | 70% |
| muduo-505 | high | 100% |

- 早期 baseline 只跑了 2 case 即取样数据，得平均 85%
- Day-3 全 5 case + 4 config 重跑见下节

## 3. v0.2.0 全矩阵真实数据

### 3.1 4 个 config 平均

| Config | Flag | 综合平均 | 单次耗时 | vs baseline |
|--------|------|---------|---------|-------------|
| baseline | （无） | **82%** | 7.6s | — |
| persona-linus | `--persona linus` | **87%** 🏆 | 8.6s | **+5%** |
| self-critique | `--self-critique` | **80%** | 12.2s | -2% |
| consensus-3 | `--consensus 3` | **82%** | 23.4s | 持平（3x 耗时） |

### 3.2 按 case 拆解（这才是真相）

| Case | 难度 | baseline | persona | critique | consensus |
|------|------|---------|---------|----------|-----------|
| `json-5187` | medium | 100% | 100% | 100% | 100% |
| `spdlog-3541` | medium | 100% | 100% | 100% | 100% |
| `muduo-729` | **low** | 90% | 70% ↓ | 70% ↓ | 90% |
| `muduo-505` | **high** | 58% | **73%** ↑ | **71%** ↑ | 58% |
| `spdlog-3594` | **high** | 62% | **92%** ↑↑ | 62% | 62% |

## 4. 真实结论（覆盖之前的预期）

### 反直觉发现

1. **medium 难度撞顶**——baseline 就 100%，三招没有空间提升。说明评测集 medium case 的关键词覆盖面偏宽。

2. **persona-linus 是 high 难度的杀手**
   - spdlog-3594: +30%（62% → 92%）
   - muduo-505: +15%（58% → 73%）
   - 机制：Linus 人设输出更严苛、关键词命中率显著提升、严重度更敢标"高"
   - 副作用：low case 把"低"严重度标错（太凶）→ -20%

3. **self-critique 在 high 难度有效，但被 low case 拖**
   - muduo-505 (high): +13%（58% → 71%）
   - muduo-729 (low): -20%（90% → 70%）— 二审反思加重了语气，丢"低"标签
   - **平均 -2% 是低 case 拖累的假象**

4. **consensus-3 这版不值这个钱** ⚠️
   - 5 case 全部跟 baseline 一字不差
   - 耗时 3x（23.4s vs 7.6s）
   - 根因：meta-review 合并 prompt 过于保守，没产生有效投票
   - 改进路径：调 meta-review prompt 让它更 aggressive，或换 consensus N=5 + 高 temperature

### 工程结论

- v0.2.0 的真实卖点是 **persona** 而非 self-critique
- 三招应该按 case 难度自适应启用，而不是一锅端
- consensus 还需要二次迭代才能落地（保留为 future work）

## 5. 评测方法学反思

### 评测集偏差

- medium 难度 case 关键词覆盖面太宽（撞顶），无法区分招式优劣
- Day-4+ 需要增加 medium 难度的细粒度 case（如：关键词从 5 个收紧到 2-3 个）

### 严重度评分粒度

- 当前严重度只看"标签是否出现"，未考虑"标对位置"
- persona-linus 会把所有问题都标"高/中"，丢失"低"标签 → 严重度命中下降
- 改进：严重度按 PR 整体风险评分，而非逐项标签匹配

### 单次 LLM 调用偏差

- 同一 case 多次跑可能出现 ±5% 波动（DeepSeek 非确定性输出）
- 当前为 1 次跑结果，没做多次平均
- 改进：每个 case 跑 3 次取平均，但工时是 3x

## 6. Demo 视频引用素材

**核心故事**："我们以为三招都会涨，结果数据告诉我们事实复杂得多。"

**关键数据点**：
- 🔥 spdlog-3594 high 难度：persona-linus +30%
- ⚠️ muduo-729 low 难度：persona/critique 都掉了 20%（人设太凶丢低标）
- ❌ consensus-3 完全没涨，但贵 3 倍

**诚实分析卖点**：
- 不交付"招招涨"的虚假产品
- 提供 4 config 矩阵让用户按场景选
- 高难度复杂 PR → persona-linus
- 低难度简单 PR → baseline 就够
- 想要确定性 → self-critique（high case 有提升）
- consensus 当前 v0.2.0 不推荐

## 7. 下一步

- v0.3.0 计划：consensus meta-review prompt 重写（让它真的投票）
- v0.3.0 计划：persona 自适应（按 case 难度自动选人设）
- v0.4.0 计划：评测集扩到 15 case（5 高 / 5 中 / 5 低 / 平衡分布）
- v0.4.0 计划：每 case 跑 3 次取平均（降低 LLM 随机性影响）

---

*本报告基于 5 cases × 4 configs = 20 次真实 LLM 调用产出，原始数据见 `code/eval/runs/v020_<config>/`。*
