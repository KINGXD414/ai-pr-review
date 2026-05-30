# DAILY.md — Day 2 (5/30)

> 七牛云 x XEngineer 暑期实训营 · AI PR Review 助手

## PR Workflow 时间线

### #2 feat(analyzer): add libclang-based AST context extractor
- **Author:** KINGXD414 (Turkey)
- **State:** Merged
- **关键内容:** 基于 libclang 的 AST 分析器，提取 C++ 语法树上下文（类型推导、继承关系、调用链追踪），为规则引擎和 LLM 提供跨文件分析能力
- **Commits:** ast_analyzer.h / ast_analyzer.cpp / test_ast_analyzer.cpp / CMakeLists 更新

### #3 docs(rules): add rule engine module README
- **Author:** love-June (June)
- **State:** Merged
- **关键内容:** 规则引擎模块文档 v1，描述 8 条静态扫描规则的设计思路和 RuleHit 输出格式
- **教训:** 第一次提 PR，学会了 review → approve → squash merge 流程

### #4 feat(Day2): add evaluation dataset with 8 PR annotations
- **Author:** love-June (June)
- **State:** Merged
- **关键内容:** 评测集 v1，8 个 PR 标注（2 高 4 中 2 低），来源 nlohmann/json、spdlog、muduo 真实 PR
- **操作:** wget 下载到 eval/dataset_v2.md，git add + commit + push + 创建 PR

### #6 refactor(Day2): rename code/src/rule to code/src/rules per issue #1
- **Author:** love-June (June)
- **State:** Merged
- **关键内容:** 按 Issue #1 约定，将 code/src/rule/ 重命名为 code/src/rules/（复数），同步修改 CMakeLists.txt 和 include 路径
- **操作:** git mv + sed 批量替换，16/16 单元测试全绿

### #7 feat(prompt): wire AST analyzer into LLM prompt via --source-dir
- **Author:** KINGXD414 (Turkey)
- **State:** Merged
- **关键内容:** AST 分析器接入 LLM prompt，新增 --source-dir CLI 参数，June approved
- **June Review:** 在 Conversation 评论后在 Files changed → Review changes → Approve

### #8 feat(eval): add JSON-driven evaluation harness ai-pr-eval
- **Author:** KINGXD414 (Turkey)
- **State:** Merged
- **关键内容:** 新增 ai-pr-eval 可执行程序，三维度评分（关键词命中率 40% + 风险等级准确率 30% + 无幻觉惩罚 30%），cases.json 含 2 条校准 case，dry-run 全过，正式跑 19 秒平均 85%
- **教训:** June approve + merge 太快，Turkey 还没回评论就合了。以后等 30 分钟再合

### #9 feat(Day2): add #9 high-difficulty eval case, remove legacy dataset.md
- **Author:** love-June (June)
- **State:** Merged
- **关键内容:** 评测集补第 9 个高难度 case（迭代器失效/UB），删掉旧版 dataset.md，难度分布从 2 高 4 中 2 低 → 3 高 4 中 2 低

### #10 docs(Day2): README final draft
- **Author:** love-June (June)
- **State:** Open — Turkey 22:00 后 push commit 校准 4 处 gap
- **关键内容:** README 终稿，包含三层架构图、9 条规则详情、RuleHit 结构体、DeepSeek 模型选择理由、ai-pr-eval 评分框架、完整依赖版本表、原创代码说明、未来扩展
- **Turkey 校准项:** 评测第三维度术语修改、区分 cases.json 与 dataset_v2.md、架构图 Rule Engine 位置调整、AST 注入描述修正
- **注意:** docs/readme-final 分支不要动，不 force-push 不 rebase

### #11 feat(Day2): expand eval cases from 2 to 5
- **Author:** love-June (June)
- **State:** Merged
- **v1 问题:** 3 个新增 case 的 PR URL 全部 404（spdlog-2856 / json-3911 / spdlog-2912 是凭标题猜的编号，不存在），且包含不可移植的 source_dir 字段
- **v2 修复:** 替换为 Turkey 提供的候选清单中真实 merged PR（spdlog#3594 / json#5187 / spdlog#3541），去掉 source_dir，基于真实 diff 重写 expected_keywords 和 must_not_hallucinate，难度 2 高 2 中 1 低
- **教训:** 所有外部引用必须实拉验证，禁止凭标题猜测

### #12 docs(Day2): add ADR-001 differentiation strategy
- **Author:** love-June (June)
- **State:** Merged
- **v1 问题:** ADR 写了内部 CLI 参数名（--persona 等）锁死实现路径；persona 描述与实际实现不符（zen 写成"禅意"实际是"鼓励式"）；Context 段措辞弱势
- **v2 修复:** CLI 参数名改为能力描述（多视角审查/两阶段自我修正/多采样共识过滤）；persona 对齐实际 prompt；Context 改为事实陈述；加 Alternatives Considered 段
- **教训:** ADR 是决策记录不是实现规格，不应暴露内部参数名

### #13 feat(persona): add multi-persona reviewer (--persona flag)
- **Author:** KINGXD414 (Turkey)
- **State:** Open — June 已评论，等回复
- **关键内容:** 5 种 reviewer 人格（default/linus/mentor/devil/zen），每种独立 system prompt + 统一三段式报告结构，报告头部和元信息表嵌入 persona badge
- **June Review:** 确认 linus prompt 有无"对事不对人"边界约束（与 ADR-001 承诺对齐）

### #14 feat(self-critique): add two-pass review via --self-critique flag
- **Author:** KINGXD414 (Turkey)
- **State:** Open — June 已评论，等回复
- **关键内容:** 二审 4 项任务（幻觉检测/严重度校准/建议强化/语气保持），二审失败防御性回退，修正说明三栏可追溯
- **June Review:** 大 diff 一审报告可能超 token 限制，有无处理

## Takeaways

1. **所有外部引用必须实拉验证** — PR #11 v1 三个 404 URL 的教训：不要凭标题猜测编号，用 API 确认存在且 merged 再写
2. **PR merge 留 30 分钟缓冲** — PR #8 太快合并，Turkey 还没回评论；以后等对方充分 review 再合
3. **ADR 是决策记录不是规格书** — 不暴露内部参数名，persona 描述对齐实际实现，措辞避免自爆型描述
