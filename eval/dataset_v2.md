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

## #2 muduo PR #705 — [FIX] compile: missing include（低风险样本）

- **URL**: https://github.com/chenshuo/muduo/pull/705
- **状态**: Merged
- **难度**: 🟢 低
- **变更**: 加一个 `#include <cstring>`，解决 gcc 13.2.1 编译失败

**人工标注**：
- 真实问题：纯合规性修复，零风险
- 唯一可挑的点：可能用 `<cstring>` 更 C++ 风格（但 muduo 风格混用，不算 bug）

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
- 真实问题 2：coredump 修复——根因是 hiredis 上下文在异步回调中被销毁后仍被引用（use-after-free）
- 两个修复互相独立，不应混在一个 PR 里

**AI 预期输出**：
- ✅ 摘要里识别出"两个独立修复"（不是一个）
- ✅ 对 coredump 修复部分给中等风险（修了 bug 但可能引入新 bug）
- ⭐ 加分项：建议补充测试覆盖 coredump 场景

**为什么值钱**：测「多问题识别能力」+「分别建议」。

---

## #4 nlohmann/json PR #3901 — 内存泄漏：adl_serializer::to_json 异常路径（高难度内存样本）⭐

- **URL**: https://github.com/nlohmann/json/pull/3901
- **状态**: Merged
- **难度**: 🔴 高
- **变更**: 在 `adl_serializer::to_json` 中抛出异常时，`basic_json` 构造函数已分配的内存不会被释放。修复方案：将 `m_value` 等成员变量提取到独立的 `data` 子对象中，利用子对象析构自动释放

**人工标注**：
- 真实问题：异常安全（exception safety）缺失导致内存泄漏
- 根因：构造函数中 `new` 分配内存后，若后续 `adl_serializer::to_json` 抛异常，已分配内存无人释放
- 修复复杂度：涉及重构 `basic_json` 内部数据结构，非简单 patch
- Review 中讨论了 copy/move 构造函数是否应 delete（子对象应不可拷贝）

**AI 预期输出**：
- ✅ 识别这是内存管理 / 异常安全问题，标 🔴 高风险
- ✅ 摘要提到「memory leak / exception safety / adl_serializer」
- ✅ 指出修复方案涉及数据结构重构，建议重点关注 move 语义和析构顺序
- ❌ 失败模式：只看到"加了成员变量"而忽略异常安全的本质问题

**为什么值钱**：异常路径的内存泄漏是最难发现的 bug 类型之一。考验 AI 的「异常安全意识」，而非仅看正常路径。

---

## #5 spdlog PR #3049 — async_logger::flush() 同步化（后被回退的中等样本）

- **URL**: https://github.com/gabime/spdlog/pull/3049
- **状态**: Merged → **后来被 Revert**（v1.15 中回退，换用 condition_variable 方案 #3235）
- **难度**: 🟡 中
- **变更**: 将 `async_logger::flush()` 从异步投递改为同步等待——通过 `std::promise/future` 阻塞调用线程直到 flush 完成

**人工标注**：
- 真实问题：原 `flush()` 是异步投递消息后立即返回，调用者无法确认 flush 是否完成（对日志可靠性有影响）
- 修复风险：使用 promise/future 在队列满时，promise 被丢弃会导致 broken_promise 异常（维护者 gabime 指出）
- **关键事实**：这个 PR 合入后又被回退了，说明修复方案本身有缺陷——这是最有价值的评测维度
- 根因：flush 语义在异步日志中天然复杂，简单同步化不够

**AI 预期输出**：
- ✅ 识别这是并发/异步语义变更，标 🟡 中风险
- ✅ 摘要提到「async flush / promise/future / blocking」
- ⭐ 加分项：指出队列满时 promise 被丢弃的风险（gabime 的质疑点）
- ❌ 失败模式：标为低风险"小改进"，忽略异步语义变更的副作用

**为什么值钱**：一个被合并又被回退的 PR！测 AI 能否识别「看似合理但有隐患」的修复。大多数 AI 会标为"正确改进"，但实际这个方案被社区推翻了。

---

## #6 muduo PR #700 — std::move 消除 send(Buffer*) 的拷贝（优化类争议样本）

- **URL**: https://github.com/chenshuo/muduo/pull/700
- **状态**: Closed（未合并，提交者自行关闭）
- **难度**: 🟡 中
- **变更**: 在 `TcpConnection::send(Buffer* buf)` 中，将 `retrieveAllAsString()` 改为 `std::move` 版本，避免内存拷贝

**人工标注**：
- 表面看：消除一次 string 拷贝，性能优化，没毛病
- 实际问题：`retrieveAllAsString()` 后 Buffer 被 retrieveAll 清空，move 语义上正确，但：
  1. 修改了 `send` 的隐式语义（原来 buf 在 send 后仍可用，move 后 buf 被掏空）
  2. muduo 的 Buffer::retrieveAll 本身就会清空，所以 move 与否不影响 buf 的后续状态
  3. **真正的争议**：这算不算过早优化？string SSO 机制下短字符串本就不走堆分配
- 未合并原因：可能维护者认为改动收益不够明确

**AI 预期输出**：
- ✅ 识别这是一个性能优化 PR
- ✅ 指出 move 语义改变了函数的隐式约定
- ⭐ 加分项：讨论 SSO（Small String Optimization）下短字符串 move 反而可能更慢
- ❌ 失败模式：无条件赞扬"消除了拷贝"，忽略语义变更风险

**为什么值钱**：测「过度优化识别」能力。很多 AI 会把性能优化一律标为好，但实际需要判断收益与风险。

---

## #7 muduo PR #711 — build.sh 脚本修复（纯构建脚本低风险样本）

- **URL**: https://github.com/chenshuo/muduo/pull/711
- **状态**: Open（Issue 形式，非 PR）
- **难度**: 🟢 低
- **变更**: `build.sh` 中 `ln -sf $BUILD_DIR/$BUILD_TYPE-cpp11/compile_commands.json` 命令缺少目标路径参数

**人工标注**：
- 真实问题：shell 脚本 symlink 命令语法错误，导致 compile_commands.json 链接创建失败
- 影响：仅影响开发体验（LSP 无法找到编译数据库），不影响运行时代码
- 零风险：修改构建脚本，不涉及任何 C++ 逻辑

**AI 预期输出**：
- ✅ 识别为构建脚本修复，标 🟢 低风险
- ✅ 摘要简短：修复 build.sh 中 ln 命令缺少目标路径
- ❌ 失败模式：夸大风险、把构建脚本问题当安全漏洞

**为什么值钱**：又一个误报抑制样本。AI 必须能区分「构建脚本问题」和「代码逻辑问题」，不能一视同仁。

---

## #8 nlohmann/json PR #3927 — 自定义分配器缺少 rebind 类型定义（中等多文件修复样本）

- **URL**: https://github.com/nlohmann/json/pull/3927
- **状态**: Merged
- **难度**: 🟡 中
- **变更**: 修复自定义分配器（custom allocator）场景下，`basic_json` 容器缺少 `rebind` 类型定义导致编译失败

**人工标注**：
- 真实问题：C++ 标准要求分配器感知容器提供 `rebind<U>` 结构，nlohmann/json 缺了这个
- 影响范围：仅影响使用自定义分配器的用户（小众但合法使用场景）
- 修复涉及多个文件（json.hpp + json_fwd.hpp + 测试）

**AI 预期输出**：
- ✅ 识别为 C++ 模板/分配器相关修复
- ✅ 摘要提到「allocator / rebind / custom allocator」
- ✅ 标 🟡 中风险（模板修改可能影响其他特化路径）
- ⭐ 加分项：指出这是 C++ 标准合规性问题，不影响默认用户

**为什么值钱**：测「小众场景识别」能力。大多数用户不会用自定义分配器，但合规性修复仍然重要。AI 不能因为"不影响大多数用户"就标为低风险。

---

## 评测指标

| 指标 | 算法 | 目标 |
|------|------|------|
| 风险等级准确率 | AI 给的等级 vs 人工标注（🔴🟡🟢）一致比例 | ≥ 70% |
| 关键问题召回率 | 人工标注的真实问题，AI 命中几条 | ≥ 60% |
| 误报率 | AI 标的问题里，人工认为是"无意义/没价值"的比例 | ≤ 30% |
| 平均报告生成时间 | 8 个 PR 的端到端耗时平均 | ≤ 30s |

## 难度分布

| 难度 | 数量 | PR 编号 |
|------|------|---------|
| 🔴 高 | 2 | #1, #4 |
| 🟡 中 | 4 | #3, #5, #6, #8 |
| 🟢 低 | 2 | #2, #7 |

## 评测维度覆盖

| 维度 | 覆盖 PR |
|------|---------|
| 误报抑制（克制不乱说） | #2, #7 |
| 多问题识别 | #3 |
| 不盲从作者描述 | #1, #5 |
| 异常安全/内存问题 | #4 |
| 过度优化识别 | #6 |
| 小众场景合规性 | #8 |
