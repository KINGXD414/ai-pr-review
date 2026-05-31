# ADR-003: RuleHit 输出格式与 Engine 接口设计

## Context

规则引擎（Rule Engine）是 AI PR Review 三层架构的第一层，负责毫秒级扫描 PR diff 中的确定性风险模式。Issue #1 讨论了规则引擎的模块边界、输出格式和接口设计，需要在以下选项中做决策：

### 输出格式

1. 简单字符串 — 每条命中返回一行文本，灵活但无法程序化处理
2. 结构化 RuleHit — 每条命中包含固定字段（rule_id / severity / file / line / message / suggestion），可被下游 LLM Gateway 和评测框架程序化消费
3. JSON 字典 — 灵活但缺乏类型约束，C++ 端需要额外的 schema 校验

### Engine 接口

1. `scan()` 直接扫描 diff — 简单，但无法接入 AST 上下文
2. `run(diff, ast = nullptr)` — 可选退化设计：不传 AST 时退化为纯 regex 模式，传了则启用跨文件检查
3. `run(diff) + set_ast(ast)` — 分步调用，灵活但调用时序容易出错

### rule_id 命名

1. 编号式（R001, R002...）— 有序，但加规则需维护编号表，可读性差
2. snake_case（unpaired_alloc, virtual_modify...）— 自解释，无需维护编号，未来扩展零成本

## Decision

### 1. 结构化 RuleHit 输出

```cpp
enum class Severity { Info, Warn, Error };

struct RuleHit {
    std::string rule_id;     // snake_case, e.g. "unpaired_alloc"
    Severity severity;       // Info / Warn / Error
    std::string file;        // 变更文件路径
    int line;                // 命中行号
    std::string message;     // 风险描述
    std::string suggestion;  // 修复建议
    double confidence = 0.8; // 置信度（当前常量，未来接入 AST 后动态计算）
};
```

选择理由：
- 下游可消费：LLM Gateway 可将 RuleHit[] 注入 prompt，评测框架可按 severity 维度统计
- 类型安全：enum class Severity 避免 string 比较的拼写错误
- 可扩展：confidence 字段为 AST enrichment 预留，当前为常量 0.8

### 2. Engine::run(diff, ast = nullptr) 可选退化接口

```cpp
class Engine {
public:
    static std::vector<RuleHit> run(const ParsedDiff& diff,
                                     const AstSummary* ast = nullptr);
private:
    static std::vector<RuleHit> scan(const std::string& diff_text);
};
```

选择理由：
- 降低部署门槛：不依赖 libclang 即可运行纯 regex 模式，AST 为可选增强
- 向后兼容：scan() 保留为 legacy 接口，内部委托 run()，已调用的代码无需修改
- 清晰语义：调用方通过是否传 ast 参数明确选择模式

### 3. rule_id 采用 snake_case

选择理由：
- 可读性优先：`unpaired_alloc` 比 `R001` 自解释
- 零维护成本：新增规则不需要维护编号表
- 搜索友好：grep / IDE 搜索 rule_id 即可定位

## Consequences

### 正面
- RuleHit 结构化输出支持评测框架的三维加权评分（关键词命中 / 严重度匹配 / 无幻觉惩罚）
- Engine::run 的可选退化设计保证规则引擎独立于 AST 运行，降低 CI/CD 部署门槛
- snake_case rule_id 自解释，降低团队沟通成本

### 负面
- confidence 当前为常量 0.8，尚未接入 AST 信息做动态计算（Day-3 补强项）
- AST enrichment 当前为 placeholder，等 libclang TU 接通后补真实数据
- scan() 保留为 legacy 接口，存在两套调用路径的维护负担

### 缓解措施
- confidence 动态计算列为 Day-3 补强项，优先级低于核心功能
- scan() 标记为 [[deprecated]]，所有新代码统一走 run()
- AST enrichment 完成后，Engine::run 的 ast != nullptr 路径将提供跨文件检查能力

## 相关 Issue

Refs #1
