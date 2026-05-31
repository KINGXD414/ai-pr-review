# ADR-002: 评测体系设计 — 三维加权评分 + 真实 PR 标注

## Context

AI PR Review 工具需要一套可量化、可复现的评测体系来验证审查质量。评测的目标不是"跑通 demo"，而是回答：AI 的审查结果和人工标注的 ground truth 之间有多少差距？

可选方案：
1. 主观评估 — 人读 AI 输出判断好坏，不可复现、无量化指标
2. 单维度评分 — 只看关键词是否命中，忽略严重度和幻觉问题
3. 三维加权评分 + 真实 PR 标注 — 关键词命中 + 严重度校准 + 无幻觉惩罚，基于真实开源 PR 的 merged commit 标注

## Decision

选择三维加权评分 + 真实 PR 标注方案。

### 三维加权评分模型

| 维度 | 权重 | 评测方法 |
|------|------|----------|
| 关键词命中率 | 40% | AI 输出是否包含 expected_keywords 中的关键词 |
| 风险等级准确率 | 30% | AI 输出的 severity 是否与 expected_severity_appears 一致 |
| 无幻觉惩罚 | 30% | AI 输出是否出现 must_not_hallucinate 中列出的幻觉内容 |

评分公式：`score = keyword_hit_rate * 0.4 + severity_accuracy * 0.3 + (1 - hallucination_rate) * 0.3`

### cases.json Schema

```json
{
  "_comment": "评分规则说明",
  "cases": [
    {
      "id": "唯一标识（格式：repo-number）",
      "name": "PR 标题",
      "pr_url": "真实存在的 GitHub PR URL（必须 API 验证为 merged）",
      "difficulty": "low | medium | high",
      "expected_keywords": ["AI 输出应包含的关键词（基于真实 diff）"],
      "expected_severity_appears": ["高", "中", "低"],
      "must_not_hallucinate": ["diff 中不存在但容易被 AI 联想到的标识符"]
    }
  ]
}
```

关键字段说明：
- `expected_keywords`：基于 PR 的 Files changed 页面实际 diff 内容提炼，禁止凭 PR 标题猜测
- `must_not_hallucinate`：同一仓库中真实存在但本 PR diff 中未涉及的函数/类名，AI 容易"联想"到这些标识符
- 无 `source_dir` 字段：评测 case 不绑定本地路径，提高可移植性

### ai-pr-eval CLI

独立可执行程序，自动化评测流程：
1. 读取 cases.json
2. 逐条调用 ai-pr-review 对 PR 进行审查
3. 解析 AI 输出，与 expected 字段比对
4. 输出三维度得分和总评分

### 评测集来源

所有 case 来自 nlohmann/json、spdlog、muduo 三个开源项目的真实 merged PR，覆盖并发、内存、编译、API 兼容等维度。

当前规模：5 条（2 高 2 中 1 低）

### Alternatives Considered

- 合成 case（手写 diff）— 可控但脱离真实场景，评委可能质疑
- Python 评测脚本 — Turkey 选择 C++ 实现保持工具栈一致性
- 更多维度（如建议可操作性）— 难以自动化量化，留作人工评估补充

## Consequences

### 正面
- 可量化可复现 — 三维度评分公式明确，任何人跑 ai-pr-eval 都能得到相同结果
- 真实 PR 标注 — 基于开源项目真实问题，评测结果可信度高
- 无幻觉维度 — 直接针对 LLM 最大失败模式
- 可扩展 — 新增 case 只需在 cases.json 中添加条目，无需改代码

### 负面
- 评测集规模有限 — 5 条 case 覆盖面有限，统计意义不足
- 关键词匹配粗糙 — 无法判断 AI 是真正理解了问题还是碰巧命中关键词
- 无 source_dir — 评测时不启用 AST 分析，低估了完整流程的审查能力

### 缓解措施
- 后续可扩充至 10-15 条 case，提升统计意义
- 结合人工评估补充建议质量判断
- 在 README 中说明纯 diff 评测与 AST 增强评测的区别

## 相关

Refs #1
