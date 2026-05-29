# 评测 PR 数据集

> 每个 PR 包含：URL / 状态 / 人工标注的真实问题 / AI 预期能识别到什么 / 难度等级

---

## #1 muduo PR #505 — corner case: happens before（高难度争议样本）⭐

- **URL**: https://github.com/chenshuo/muduo/pull/505
- **状态**: Closed（未合并，作者认为是 bug，维护者反驳）
- **难度**: 🔴 高（最值钱的样本）
- **变更**: 修改 EventLoop::loop()，作者声称当 callingPendingFunctors_ 为 false 时其他线程 queueInLoop 不会触发 wakeup

**人工标注**：
- 表面看：作者描述的「starve / 唤醒丢失」是真实并发风险
- 实际：muduo 内部已有 wakeup 兜底机制（维护者反驳点）
- **正确结论**：这个 PR 是「看似有理但实际不必要」

**AI 预期输出**：
- ✅ 识别这是并发相关变更，标 🔴 高风险
- ✅ 摘要里提到「happens-before / wakeup / queueInLoop」
- ⭐ 加分项：能指出 muduo 的 wakeup 兜底机制（需要 libclang 跨文件 AST 分析才做得到）
- ❌ 失败模式：盲目认同作者，标"建议合并"

**为什么值钱**：考验 AI 的「不盲从作者描述」能力，区分「真 bug」和「想象的 bug」。

---

## #2 muduo PR #705 — [FIX] compile: missing <time.h>（低风险样本）

- **URL**: https://github.com/chenshuo/muduo/pull/705
- **状态**: Merged
- **难度**: 🟢 低
- **变更**: 加一个 `#include <time.h>`，解决 gcc 13.2.1 编译失败

**人工标注**：
- 真实问题：纯合规性修复，零风险
- 唯一可挑的点：可能用 `<ctime>` 更 C++ 风格（但 muduo 风格混用，不算 bug）

**AI 预期输出**：
- ✅ 识别为低风险变更，标 🟢
- ✅ 摘要简短（不到 2 行）
- ❌ 失败模式：硬找问题、标高风险、给一堆无关建议（这正是访谈学弟说的"为存在感而点评"）

**为什么值钱**：测「误报抑制」能力。AI 容易对每个 PR 都说一堆，这种 PR 必须**克制不输出无意义建议**。

---

## #3 muduo PR #729 — fix missing header & coredump（中等多问题样本）

- **URL**: https://github.com/chenshuo/muduo/pull/729
- **状态**: Merged
- **难度**: 🟡 中
- **变更**: 同时修两个问题——noncopyable.h 缺 include + hiredis 模块 coredump

**人工标注**：
- 真实问题 1：header 缺失（简单）
- 真实问题 2：coredump 修复（需要看具体改了什么内存/初始化逻辑）
- **TODO（你补充）**：点进 Files Changed 看实际改动，记一句话「coredump 的根因是 XX」

**AI 预期输出**：
- ✅ 摘要里识别出"两个独立修复"（不是一个）
- ✅ 对 coredump 修复部分给中等风险（修了 bug 但可能引入新 bug）
- ⭐ 加分项：建议补充测试覆盖 coredump 场景

**为什么值钱**：测「多问题识别能力」+「分别建议」。

---

## ⏳ 待补充（目标再凑 5 个，达到 8 个）

### 必补（你自己的 ChatServer，最有说服力）

**做法**：今晚在 ChatServer 仓库新建分支 `ai-review-test`，故意写 3 个有问题的 commit + 各自开 PR：

- [ ] **PR #4（待造）**：线程不加锁访问 user_map
  - 标注：明显的 data race
  - AI 预期：🔴 高风险，指出 race condition
- [ ] **PR #5（待造）**：消息处理 new 了 Message 但异常路径没 delete
  - 标注：内存泄漏
  - AI 预期：🔴 高风险，建议用 unique_ptr
- [ ] **PR #6（待造）**：vector 越界 / iterator 失效场景
  - 标注：UB，调试一周也未必能复现
  - AI 预期：🟡 中风险（取决于上下文）

### 选补（再来 2 个外部）

- [ ] **nlohmann/json 的 PR**：去 https://github.com/nlohmann/json/pulls?q=is%3Aclosed+sort%3Acomments-desc 挑评论最多的 1-2 个
- [ ] **spdlog 的 PR**：去 https://github.com/gabime/spdlog/pulls?q=is%3Aclosed 同样挑

---

## 评测指标（评测脚本明天写）

| 指标 | 算法 | 目标 |
|------|------|------|
| 风险等级准确率 | AI 给的等级 vs 人工标注（🔴🟡🟢）一致比例 | ≥ 70% |
| 关键问题召回率 | 人工标注的真实问题，AI 命中几条 | ≥ 60% |
| 误报率 | AI 标的问题里，人工认为是"无意义/没价值"的比例 | ≤ 30% |
| 平均报告生成时间 | 8 个 PR 的端到端耗时平均 | ≤ 30s |
