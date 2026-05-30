# AI PR Review — 智能代码审查助手

> 七牛云×XEngineer暑期实训营 · 题目三

## 项目简介

AI PR Review 是一个基于**规则引擎 + AST 分析 + LLM** 三层架构的 GitHub PR 智能审查工具。它自动拉取 PR 变更，逐层扫描风险模式，生成带风险等级和修复建议的结构化审查报告。

**设计哲学：** 规则引擎秒级筛掉确定性问题，AST 提供跨文件上下文，LLM 只在需要语义理解时介入——避免对每个 PR 都发起 API 调用，兼顾速度与深度。

## 架构设计

```
GitHub PR
    │
    ▼
┌──────────────┐
│  Diff Parser  │  解析 unified diff → ParsedDiff
└──────┬───────┘
       │
       ▼
┌──────────────┐
│  Rule Engine  │  ← 第一层：正则规则扫描（毫秒级，无需LLM）
│  (9条规则)    │     malloc/free配对、虚函数修改、裸指针new等
└──────┬───────┘
       │ RuleHit: rule_id / severity / file / line / message / suggestion
       ▼
┌──────────────┐
│ AST Analyzer  │  ← 第二层：libclang 语法树上下文分析
│  (libclang)   │     跨文件类型推导、继承关系、调用链追踪
└──────┬───────┘
       │ AstSummary 传入 Engine::run(diff, ast)
       ▼
┌──────────────┐
│  LLM Gateway  │  ← 第三层：DeepSeek 大模型深度分析
│  (DeepSeek)   │     语义理解、风格建议、复杂逻辑审查
└──────┬───────┘
       │
       ▼
  结构化审查报告 (RuleHit[])
```

### 上下文获取方式

| 层级 | 输入来源 | 获取方式 |
|------|----------|----------|
| Rule Engine | PR diff（unified format） | GitHub API 拉 diff，本地 ParsedDiff 解析 |
| AST Analyzer | 仓库源码（.h/.cpp） | --source-dir 指定本地路径，libclang 索引 |
| LLM Gateway | diff + RuleHit + AstSummary | 组装 prompt，HTTP SSE 调用 DeepSeek |

**关键设计：** `Engine::run(const ParsedDiff& diff, const AstSummary* ast = nullptr)`——不传 AST 参数时退化为纯 regex 模式，传了则启用跨文件检查。这保证了规则引擎可以独立于 AST 运行，降低部署门槛。

## 功能特性

### 规则引擎（Rule Engine）
- 9条静态扫描规则，无需 LLM 调用，毫秒级返回
- 每条命中输出 RuleHit：rule_id / severity(info|warn|error) / file / line / message / suggestion
- rule_id 采用 snake_case 命名（如 unpaired_alloc），可读性优先于排序性

| 规则 | rule_id | 检测内容 | severity |
|------|---------|----------|----------|
| 未配对分配 | `unpaired_alloc` | malloc/new 未配对 free/delete | error |
| 虚函数修改 | `virtual_modify` | 修改 override/virtual 虚函数 | error |
| 裸指针 new | `raw_ptr_new` | 裸指针 new（应使用 make_unique） | warn |
| C 风格转换 | `c_style_cast` | C 风格强制类型转换 | warn |
| memcpy 参数错误 | `memcpy_size` | memcpy sizeof 参数为指针而非数据 | error |
| 纯格式化改动 | `format_only` | 仅空格/缩进变化 | info |
| 测试文件变更 | `test_file` | 变更文件位于 test 目录 | info |
| TODO/FIXME | `todo_fixme` | 遗留 TODO/FIXME 注释 | warn |
| 迭代器失效/UB | `iterator_invalidation` | 容器遍历中修改导致 UB | error |

### AST 分析器（AST Analyzer）
- 基于 libclang 的 C++ 语法树分析
- `--source-dir` 参数指定源码目录，构建 AST 索引
- 跨文件类型推导与继承关系追踪
- 分析结果封装为 AstSummary，注入规则引擎和 LLM prompt

### LLM 网关（LLM Gateway）
- **模型选择：DeepSeek-V3**——中文代码审查场景下表现优秀，API 成本远低于 GPT-4 级模型，支持 SSE 流式输出，适合实时交互场景
- 支持流式输出（SSE），实时返回审查结果
- 可配置 prompt 模板，支持规则命中结果和 AST 上下文注入

### 评测框架（ai-pr-eval）
- 独立可执行程序，自动化评测审查质量
- 三维度加权评分：
  - 关键词命中率（40%）：AI 是否识别到标注的核心问题
  - 风险等级准确率（30%）：severity 是否与人工标注一致
  - 建议有效性（30%）：suggestion 是否具备可操作性
- 评测集：9个真实 PR 标注（3高4中2低），源自 nlohmann/json、spdlog、muduo
- dry-run 全过，正式跑平均 85%

## 构建与运行

### 依赖

| 依赖 | 版本 | 用途 |
|------|------|------|
| C++17 编译器 | GCC 9+ / Clang 10+ | 基础编译 |
| CMake | 3.16+ | 构建系统 |
| libclang | 系统包 | AST 分析模块 |
| cpp-httplib | v0.14.3 | HTTP 客户端（GitHub API / DeepSeek） |
| CLI11 | v2.4.1 | 命令行参数解析 |
| Google Test | v1.14.0 | 单元测试框架 |

> 所有第三方依赖通过 CMake FetchContent 自动下载，无需手动安装。

### 构建
```bash
cd code/build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

### 运行测试
```bash
cd code/build

# 规则引擎测试（16个用例）
./rule_engine_tests

# AST分析器测试
./ast_analyzer_tests

# Diff解析器测试
./diff_parser_tests

# 或运行全部测试
ctest --output-on-failure
```

### 运行主程序
```bash
# 设置 GitHub Token 和 DeepSeek API Key
export GITHUB_TOKEN=your_github_token
export DEEPSEEK_API_KEY=your_api_key

# 基础模式（仅规则引擎）
./ai-pr-review --repo owner/repo --pr 123

# 启用 AST 分析
./ai-pr-review --repo owner/repo --pr 123 --source-dir /path/to/repo
```

### 运行评测
```bash
cd code/build
./ai-pr-eval
```

## 项目结构

```
ai-pr-review/
├── code/
│   ├── CMakeLists.txt          # 主构建配置（FetchContent 管理依赖）
│   ├── src/
│   │   ├── rules/              # 规则引擎模块（June）
│   │   │   ├── engine.h        # RuleHit 结构体 + Engine 接口
│   │   │   ├── engine.cpp      # 9条规则实现
│   │   │   └── README.md       # 规则引擎设计文档
│   │   ├── analyzer/           # AST 分析模块（Turkey）
│   │   │   ├── ast_analyzer.h
│   │   │   └── ast_analyzer.cpp
│   │   ├── diff/               # Diff 解析模块
│   │   │   └── parser.cpp
│   │   ├── github/             # GitHub API 客户端
│   │   │   └── api_client.cpp
│   │   ├── gateway/            # LLM 网关
│   │   │   └── llm_gateway.cpp
│   │   └── main.cpp            # 主入口（Pipeline 编排）
│   └── tests/                  # 测试
│       ├── test_rule_engine.cpp
│       ├── test_ast_analyzer.cpp
│       ├── fixtures/           # 测试固件
│       └── ...
├── eval/
│   └── dataset_v2.md           # 9个PR标注评测集
└── README.md                   # 本文件
```

## 评测集

基于 nlohmann/json、spdlog、muduo 三个开源项目的**真实 PR**标注（非合成 case），覆盖 5 个评测维度：

| 维度 | 说明 |
|------|------|
| 误报抑制 | 低风险 PR 不输出无意义建议 |
| 多问题识别 | 识别同一 PR 中的多个独立问题 |
| 不盲从作者 | 区分"真 bug"和"想象的 bug" |
| 异常安全 | 异常路径内存泄漏检测 |
| 过度优化识别 | 判断优化收益与风险 |

**难度分布：** 🔴 高 ×3 / 🟡 中 ×4 / 🟢 低 ×2

## 原创代码说明

本项目在 72 小时内完成，以下为各成员原创贡献：

### love-June（规则引擎 + 评测集）
- `code/src/rules/engine.h`：RuleHit 结构体设计、Engine 接口定义
- `code/src/rules/engine.cpp`：9条规则的完整实现（正则匹配 + 语义过滤）
- `code/tests/test_rule_engine.cpp`：16个单元测试用例，全部通过
- `eval/dataset_v2.md`：9个真实 PR 的标注评测集，含难度分级和预期识别项

### KINGXD414（AST 分析 + 主 Pipeline + 评测框架）
- `code/src/analyzer/`：基于 libclang 的 AST 分析器
- `code/src/main.cpp`：三层 Pipeline 编排，支持 --source-dir 参数
- `code/CMakeLists.txt`：完整构建配置，FetchContent 依赖管理
- 评测 driver（ai-pr-eval）：三维度自动评分

## 未来扩展

- **增量 AST 缓存：** 对大型仓库，缓存上次 AST 索引，仅重新解析变更文件
- **更多规则：** 线程安全规则（共享变量无锁保护）、资源泄漏规则（RAII 覆盖检测）
- **多模型支持：** 支持 OpenAI / Claude 等模型切换
- **GitHub App 集成：** 从 CLI 工具升级为 GitHub App，PR 提交自动触发审查
- **VSCode Extension：** 开发者提交 PR 后侧边栏实时展示审查结果，支持点击 RuleHit 跳转到对应代码行
- **规则自定义：** 支持用户通过配置文件定义项目特定规则

## 团队

| 成员 | GitHub | 职责 |
|------|--------|------|
| love-June | [@love-June](https://github.com/love-June) | 规则引擎 + 评测集 + README |
| KINGXD414 | [@KINGXD414](https://github.com/KINGXD414) | AST 分析器 + 主 Pipeline + CMake + 评测框架 |

## License

MIT
