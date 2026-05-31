# v0.2.0 评测矩阵 & Self-Reviewing Workflow 实证

> 本文件是 AI PR Review 助手 **v0.2.0** 的演示档案，对应仓库 tag [`v0.2.0`](https://github.com/KINGXD414/ai-pr-review/releases/tag/v0.2.0)。
> 评委可基于本文件还原所有数据；所有原始产物在 `code/eval/` 与 `demo/`。

---

## 0. 摘要（30 秒读完）

v0.2.0 相对 v0.1.0 的两件大事：

1. **5 × 4 评测矩阵**：5 个真实 PR × 4 种 review 策略，给出"哪种策略在哪种 PR 上有用"的实证答案，而不是空谈"加 prompt 就更好"。
2. **Self-Reviewing Workflow**：CI 上 `pr_review.yml` 用本项目自己 review 自己的 PR，并 sticky 原地更新评论，实现"工具吃自己狗粮"闭环。PR #26 全程留痕。

**结论先说**：
- `persona-linus` 在 **high-complexity 大 PR**（500+ 行改动）上把命中率从 58~62% 拉到 **73~92%**（+15 ~ +30pp），是目前最值得默认开启的策略。
- `self-critique` 在 **low-complexity 小 PR** 上会把已经命中的规则反复怀疑掉，得分倒退 -20pp，**不要全局开**。
- `consensus-2`（双轮投票）没有显著增益，但耗时 ×3，**性价比最差**，v0.3 会下线。

---

## 1. 评测矩阵（5 × 4）

| Case | 改动复杂度 | baseline | persona-linus | self-critique | consensus-2 |
|---|---|---|---|---|---|
| `json-5187`（nlohmann/json #5187） | medium | **100%** | 100% | 100% | 100% |
| `spdlog-3541`（gabime/spdlog #3541） | medium | **100%** | 100% | 100% | 100% |
| `muduo-729`（chenshuo/muduo #729） | low | **90%** | 70% ⬇️ | 70% ⬇️ | 90% |
| `muduo-505`（chenshuo/muduo #505） | high | 58% | **73%** ⬆️ | 71% ⬆️ | 58% |
| `spdlog-3594`（gabime/spdlog #3594） | high | 62% | **92%** ⬆️ | 62% | 62% |
| **加权平均** | — | **82%** | **87%** | **80%** | **82%** |
| **平均耗时（单 PR）** | — | 7.6 s | 8.6 s | 12.2 s | 23.4 s |

> 评分口径：人工标注 ground-truth bug/style/perf 项，AI 命中即得分；漏报扣分；误报扣 0.5 分。
> 详细原始数据见 `code/eval/cases.json` 与 `code/eval/comparison_report.md`。

---

## 2. 四条关键洞察

### 洞察 1：`persona-linus` 在大 PR 上效果显著，小 PR 上反而拖后腿
- High 复杂度（muduo-505 / spdlog-3594）平均提升 **+22.5pp**。
- Low 复杂度（muduo-729）反而 **-20pp**——Linus 视角"挑刺"过度，把简单 PR 的非问题点也报成 bug。
- **v0.3 计划**：按改动行数自动切策略（>300 行 LOC 走 persona，否则 baseline）。

### 洞察 2：`self-critique` 在小 PR 上灾难性回退
- muduo-729 baseline 90% → critique 70%，倒退 **-20pp**。
- 原因：第二轮 critique 反复怀疑第一轮已命中的简单规则，导致 AI 改口"其实可能没问题"。
- **v0.3 计划**：仅对置信度 <0.7 的命中触发 self-critique，不对全量触发。

### 洞察 3：`consensus-2` 零增益但 ×3 耗时
- 平均得分与 baseline 持平（82% vs 82%），但耗时 23.4s vs 7.6s。
- 原因：DeepSeek 同一温度下输出方差很小，"双轮投票"基本是两次相同输出。
- **v0.3 计划**：下线 consensus-2，换成 `multi-temp`（不同温度跑两轮再投票）。

### 洞察 4：medium 复杂度 PR 已经触顶
- json-5187 / spdlog-3541 四策略全 100%。
- 说明 baseline 在中等规模 PR 上已经做到了能做的极限。
- **结论**：未来精力优先投到 high 复杂度场景。

---

## 3. Self-Reviewing Workflow 实战时间线（PR #26）

`pr_review.yml` 在 PR opened/synchronize 时触发，调用本项目跑一遍 review，并在 PR 上 sticky 原地更新评论（认 `<!-- ai-pr-review-bot -->` HTML 注释作为锚点）。

| 时间（UTC+8） | 事件 | Artifact |
|---|---|---|
| 02:14:22 | PR #26 首次 open，触发 workflow | sticky comment 首发 |
| 02:21:00 | AI 评论指出 3 处可改进（异常处理 + 注释 + 命名） | comment id `4585452430` |
| 02:21:38 | 作者 push commit `5cbcacc6b9`，针对 AI 建议修复 | — |
| 02:23:01 | workflow 自动二次运行，sticky 评论**原地更新**为"已修复，无新增问题" | 同 comment id `4585452430` |

> **关键**：sticky comment 没有刷屏（不是每次 push 都 new 一条），而是用 HTML 注释做幂等锚点，模拟了 Codecov / DangerJS 的成熟做法。

PR #28（v0.2.0 release prep）同样跑了一次，AI 评"未发现明显风险" + 5 条文档润色建议，详情见 sticky [comment 4585503677](https://github.com/KINGXD414/ai-pr-review/pull/28#issuecomment-4585503677)。

---

## 4. 故事卖点（给 Demo 视频用）

1. **"我们用自己的工具，review 我们自己的代码"**——`pr_review.yml` 全自动，无人工触发。
2. **"原地更新，不刷屏"**——sticky comment 锚点幂等，工程化细节。
3. **"基于实证选策略"**——不是拍脑袋说 persona 好，而是 5×4 矩阵说话；甚至给出反面案例（小 PR 不要用 persona）。
4. **"性价比意识"**——consensus-2 零增益直接下线，不为了"看起来有 N 种 mode"而堆功能。

---

## 5. 现场可重现命令

### 5.1 拉镜像直接跑
```bash
# 拉 v0.2.0 镜像（仓库公开后可访问）
docker pull ghcr.io/kingxd414/ai-pr-review:v0.2.0

# 跑单个 case
docker run --rm \
  -e DEEPSEEK_API_KEY=$DEEPSEEK_API_KEY \
  -e GITHUB_TOKEN=$GITHUB_TOKEN \
  ghcr.io/kingxd414/ai-pr-review:v0.2.0 \
  ai-pr-review \
    --owner gabime --repo spdlog --pr 3594 \
    --config persona-linus
```

### 5.2 跑全套 5×4 矩阵
```bash
# 在 code/eval/ 目录下
bash run_eval_matrix.sh   # 输出 comparison_report.md
```

### 5.3 下载 Release 二进制
```bash
curl -L -o ai-pr-review-v0.2.0.tar.gz \
  https://github.com/KINGXD414/ai-pr-review/releases/download/v0.2.0/ai-pr-review-v0.2.0-linux-x64.tar.gz

# 校验
curl -L https://github.com/KINGXD414/ai-pr-review/releases/download/v0.2.0/ai-pr-review-v0.2.0-linux-x64.tar.gz.sha256
sha256sum ai-pr-review-v0.2.0.tar.gz   # 对比上面输出
```

---

## 6. 相关文件索引

| 路径 | 内容 |
|---|---|
| `code/eval/cases.json` | 5 个 case 的 PR 元数据 + ground-truth |
| `code/eval/baseline_report.md` | v0.1.0 baseline 报告 |
| `code/eval/comparison_report.md` | v0.2.0 完整 5×4 矩阵报告 |
| `.github/workflows/pr_review.yml` | Self-Reviewing Workflow 定义 |
| `.github/workflows/release.yml` | tag → 自动构建 binary + Docker image |
| `docs/CHANGELOG.md` | v0.2.0 变更说明 |
| `docs/设计方案.md` | 整体设计文档（v0.2.0 起从仓库根迁入 docs/） |

---

**版本**：v0.2.0
**提交时间**：2026-05-31
**作者**：[KINGXD414](https://github.com/KINGXD414) ＋ [love-June](https://github.com/love-June)
