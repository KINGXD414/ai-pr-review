# AI PR Review — 智能代码审查助手

> 七牛云×XEngineer暑期实训营 · 题目三

## 🎬 Demo 视频

> [📺 B 站演示视频（10 分钟）](https://www.bilibili.com/video/TODO_REPLACE_BEFORE_DEADLINE)

实战剧本：muduo 真实 PR → AI 30 秒定位风险 → 修复 commit → AI Approve。

> ⚠️ 视频链接将在 2026-05-31 23:00 前更新为最终录制版本。

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
│  (8条规则)    │     malloc/free配对、虚函数修改、裸指针new等
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
| LLM Gateway | diff + RuleHit + AstSummary | 组装 prompt，HTTP 调用 DeepSeek |

**关键设计：** `Engine::run(const ParsedDiff& diff, const AstSummary* ast = nullptr)`——不传 AST 参数时退化为纯 regex 模式，传了则启用跨文件检查。这保证了规则引擎可以独立于 AST 运行，降低部署门槛。

## 功能特性

### 规则引擎（Rule Engine）
- 8条静态扫描规则，无需 LLM 调用，毫秒级返回
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

### AST 分析器（AST Analyzer）
- 基于 libclang 的 C++ 语法树分析
- `--source-dir` 参数指定源码目录，构建 AST 索引
- 跨文件类型推导与继承关系追踪
- 分析结果封装为 AstSummary，注入规则引擎和 LLM prompt

### LLM 网关（LLM Gateway）
- **模型选择：DeepSeek-V3**——中文代码审查场景下表现优秀，API 成本远低于 GPT-4 级模型，适合实时交互场景
- 可配置 prompt 模板，支持规则命中结果和 AST 上下文注入

### 评测框架（ai-pr-eval）

独立可执行程序 `ai-pr-eval`，基于 JSON-driven 测试集自动化评测审查质量。设计见 [ADR-002](docs/adr/002-evaluation-framework.md)。

**三维加权评分**：

| 维度 | 权重 | 评测方法 |
|------|------|----------|
| 关键词命中率 | 40% | AI 输出是否包含 `expected_keywords` 中的关键词 |
| 风险等级准确率 | 30% | severity 是否与 `expected_severity_appears` 一致 |
| 无幻觉惩罚 | 30% | AI 是否输出 `must_not_hallucinate` 中标注的"虚构标识符" |

**评分公式**：`score = keyword_hit × 0.4 + severity_match × 0.3 + (1 - hallucination_rate) × 0.3`

**评测集**：5 条真实 merged PR（2 高 2 中 1 低），来自 nlohmann/json、spdlog、muduo 三个开源项目，位于 `code/eval/cases.json`；候选 PR 的人工标注详见 `eval/dataset_v2.md`。

**v0.2.0 实测**：完整 5 cases × 4 configs 评测矩阵已跑完，20 次 LLM 调用全部成功。4 配置综合分：baseline **82%** / persona-linus **87%** / self-critique **80%** / consensus-3 **82%**。高难度 case 上 persona-linus 平均提升 +22.5%（spdlog-3594 +30%），self-critique 在低难度被拖累 -2%，consensus-3 零增益但 3 倍开销。完整数据 + 诚实分析见 `code/eval/comparison_report.md`，v0.1.0 baseline 历史报告见 `code/eval/baseline_report.md`。

## 差异化特性 v0.2.0（三招）

为应对现有产品（Codacy / SonarCloud / Copilot）的同质化输出，v0.2.0 引入三招 prompt engineering 创新，聚焦"审查质量本身"而非平台集成。设计动机详见 [ADR-001](docs/adr/001-differentiation-strategy.md)。

### 1. 人格化 Reviewer（`--persona`）

提供 5 种 reviewer 人设，适配不同审查场景：

| Persona | 风格 | 适用场景 |
|---------|------|----------|
| `linus` | 严厉直接、对事不对人 | 资深团队需要高效审查 |
| `mentor` | 耐心解释原因 | 新手代码审查 |
| `devil` | 对抗式，专挑 edge case | 关键路径上线前 |
| `zen` | 鼓励式，先肯定再建议 | 实习生 / 首次贡献 |
| `default`（不指定） | 均衡资深模式 | 通用场景 |

**所有 persona 都内置"不要为了显得严厉而编造风险"的边界约束**，禁止人身攻击。

```bash
./ai-pr-review --repo owner/repo --pr 123 --persona linus
```

### 2. Self-Critique 反思链（`--self-critique`）

两阶段自我修正：一审输出初版意见 → 二审反思过滤误报、补充遗漏、增强建议具体性。直接针对 LLM 单次输出的幻觉与遗漏问题，是评测体系中"无幻觉"维度提升最大的功能。

```bash
./ai-pr-review --repo owner/repo --pr 123 --self-critique
```

### 3. Consensus 多次投票（`--consensus N`）

运行 N 次独立审查（temperature=0.7 提升多样性），meta-review 阶段（temperature=0.2）按投票合并：
- N>1 时只保留 `ceil(N/2)` 票以上的条目，单票视为噪声/幻觉
- 报告附带 `[共识 X/N]` 票数后缀和单票丢弃原因
- N 范围 `[1, 5]`，默认 1（保持 v0.1.0 行为）

```bash
./ai-pr-review --repo owner/repo --pr 123 --consensus 3
```

> 三招互斥使用。v0.2.0 实测：persona-linus 综合 87%（最佳），baseline 82%，self-critique 80%，consensus-3 82%（零增益但 3x 开销）。完整 5×4 矩阵见 `code/eval/comparison_report.md`。

## 构建与运行

### 依赖

**系统依赖（apt，需手动安装）**：

| 依赖 | 推荐版本 | 用途 |
|------|----------|------|
| C++17 编译器 | GCC 9+ / Clang 10+ | 基础编译 |
| CMake | 3.16+ | 构建系统 |
| libclang-dev | 12~18（任一） | AST 分析（推荐 libclang-18-dev） |
| OpenSSL | 1.1+ | HTTPS（GitHub API / DeepSeek） |
| spdlog | 1.5+ | 日志 |
| fmt | 6.0+ | 格式化 |
| nlohmann-json | 3.x | JSON 解析 |
| sqlite3 | 3.x | 评测结果落地 |

**Ubuntu 20.04 一键安装**：

```bash
sudo apt update && sudo apt install -y \
  build-essential cmake \
  libclang-18-dev libssl-dev libsqlite3-dev \
  libspdlog-dev libfmt-dev nlohmann-json3-dev
```

**第三方依赖（CMake FetchContent 自动拉取，无需手动安装）**：

| 依赖 | 版本 | 用途 |
|------|------|------|
| cpp-httplib | v0.14.3 | HTTP 客户端（GitHub API / DeepSeek） |
| CLI11 | v2.4.1 | 命令行参数解析 |
| Google Test | v1.14.0 | 单元测试框架 |

**API Key 配置**：

```bash
export GITHUB_TOKEN=ghp_xxx        # GitHub Personal Access Token
export DEEPSEEK_API_KEY=sk-xxx      # DeepSeek API Key
```

### 构建
```bash
cd code/build
cmake .. -DCMAKE_BUILD_TYPE=Debug
cmake --build . -j$(nproc)
```

### 运行测试
```bash
cd code/build

# 规则引擎测试
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

# v0.2.0 差异化三招（详见 docs/adr/001-differentiation-strategy.md）
# Linus 人设审查（严厉直接）
./ai-pr-review --repo owner/repo --pr 123 --persona linus

# Self-Critique 二审自检（提升无幻觉）
./ai-pr-review --repo owner/repo --pr 123 --self-critique

# Consensus 多次投票（N=3，提升置信度）
./ai-pr-review --repo owner/repo --pr 123 --consensus 3

# 输出保存到 ./review_<repo>_<pr>.md
```

### 运行评测
```bash
cd code/build

# 全量评测（baseline，无 flag）
./ai-pr-eval --cases ../eval/cases.json --binary ./ai-pr-review \
             --github-token $GITHUB_TOKEN --llm-key $DEEPSEEK_API_KEY \
             --out-dir ../eval/runs/baseline

# 三招对比评测（任一）
./ai-pr-eval --cases ../eval/cases.json --binary ./ai-pr-review \
             --persona linus       # 或 --self-critique / --consensus 3
             --out-dir ../eval/runs/persona-linus
```

输出：每条 case 的三维度得分 + 总评分 + JSON 详情。

## 项目结构

```
ai-pr-review/
├── code/
│   ├── CMakeLists.txt          # 主构建配置（FetchContent + apt 双管依赖）
│   ├── src/
│   │   ├── main.cpp            # 主 CLI 入口（Pipeline 编排，含三招 flag）
│   │   ├── eval_main.cpp       # 评测程序 ai-pr-eval 入口
│   │   ├── rules/              # 规则引擎模块（June）
│   │   │   ├── engine.h        # RuleHit 结构体 + Engine 接口
│   │   │   ├── engine.cpp      # 8 条规则实现
│   │   │   └── README.md       # 规则引擎设计文档
│   │   ├── analyzer/           # AST 分析模块（Turkey）
│   │   │   ├── ast_analyzer.h
│   │   │   └── ast_analyzer.cpp
│   │   ├── diff/               # Diff 解析模块
│   │   ├── github/             # GitHub API 客户端
│   │   ├── gateway/            # LLM 网关（DeepSeek）
│   │   ├── prompt/             # v0.2.0 三招 prompt 模板
│   │   │   ├── personas.h      # 5 种 reviewer 人设
│   │   │   ├── self_critique.h # 二审反思 prompt
│   │   │   └── consensus.h     # 多次投票 meta-review prompt
│   │   └── eval/               # 评测程序核心代码（runner.cpp / runner.h）
│   ├── eval/                   # 评测数据与实测产出
│   │   ├── cases.json          # 评测集（5 条标注 PR，dataset_v2 子集）
│   │   ├── baseline_report.md  # v0.1.0 baseline 实测报告（综合 85%）
│   │   ├── comparison_report.md # v0.2.0 三招对比矩阵（5×4，已完成）
│   │   └── runs/               # 各次评测原始输出（runs/<时间戳>/）
│   └── tests/                  # GTest 单元测试
│       ├── test_rule_engine.cpp
│       ├── test_ast_analyzer.cpp
│       └── ...
├── docs/
│   ├── DAILY.md                # 每日进度日志
│   ├── CHANGELOG.md            # 版本变更记录（Keep a Changelog）
│   ├── adr/                    # 架构决策记录
│   │   ├── 001-differentiation-strategy.md  # 三招方向选择
│   │   └── 002-evaluation-framework.md      # 评测体系设计
│   └── kodo-prestudy.md        # 七牛云 KODO 集成预研（未来扩展）
├── demo/                       # 真实开源 PR demo 输出
│   ├── muduo_pr705_review.md
│   └── muduo_pr729_review.md
├── eval/
│   └── dataset_v2.md           # 评测集人工标注说明（9 条候选 PR）
└── README.md                   # 本文件
```

## 评测集

基于 nlohmann/json、spdlog、muduo 三个开源项目的**真实 merged PR**标注，覆盖 5 个评测维度：

| 维度 | 说明 |
|------|------|
| 误报抑制 | 低风险 PR 不输出无意义建议 |
| 多问题识别 | 识别同一 PR 中的多个独立问题 |
| 不盲从作者 | 区分"真 bug"和"想象的 bug" |
| 异常安全 | 异常路径内存泄漏检测 |
| 过度优化识别 | 判断优化收益与风险 |

**难度分布：** 🔴 高 ×2 / 🟡 中 ×2 / 🟢 低 ×1

## 原创代码说明

本项目在 72 小时内完成，以下为各成员原创贡献：

### love-June（规则引擎 + 评测集 + 项目文档）
- `code/src/rules/`：RuleHit 结构体、Engine::run 接口、8 条规则的完整实现
- `code/tests/test_rule_engine.cpp`：GTest 单元测试用例
- `eval/cases.json` + `eval/dataset_v2.md`：5 条真实 PR 评测集
- `docs/adr/001-differentiation-strategy.md`：差异化方向 ADR
- `docs/adr/002-evaluation-framework.md`：评测体系 ADR
- `docs/DAILY.md`：每日进度日志
- `docs/CHANGELOG.md`：版本变更记录
- `docs/kodo-prestudy.md`：七牛云 KODO 集成预研
- `README.md`：本文件

### KINGXD414（AST 分析 + 主 Pipeline + 评测框架 + v0.2.0 三招）
- `code/src/analyzer/`：基于 libclang 的 AST 分析器（含 9 项单元测试）
- `code/src/diff/` + `code/src/github/` + `code/src/gateway/`：Diff 解析、GitHub API 客户端、DeepSeek LLM 网关
- `code/src/main.cpp`：CLI Pipeline 编排，支持 `--source-dir` / `--persona` / `--self-critique` / `--consensus`
- `code/src/prompt/`：v0.2.0 三招 prompt 模板（personas / self-critique / consensus）
- `code/src/eval/` + `eval_main.cpp`：评测程序 `ai-pr-eval` 三维度评分实现
- `code/CMakeLists.txt`：FetchContent + apt 双管依赖构建配置
- `demo/muduo_pr705_review.md` + `demo/muduo_pr729_review.md`：真实 PR demo 输出

## 未来扩展

- **增量 AST 缓存：** 对大型仓库，缓存上次 AST 索引，仅重新解析变更文件
- **多模型支持：** 支持 OpenAI / Claude 等模型切换
- **GitHub App 集成：** 从 CLI 工具升级为 GitHub App，PR 提交自动触发审查
- **VSCode Extension：** 开发者提交 PR 后侧边栏实时展示审查结果
- **规则自定义：** 支持用户通过配置文件定义项目特定规则
- **规则引擎主集成：** 当前规则引擎为独立模块（`code/src/rules/`），未来集成至主 Pipeline 第一层，正则秒级筛掉确定性问题
- **真 SSE 流式输出：** 当前 DeepSeek 调用为同步阻塞，未来切换至 SSE 流式输出，提升大型 PR 审查的交互体验
- **更多规则：** 迭代器失效检测（`iterator_invalidation`）、线程安全规则（共享变量无锁保护）、资源泄漏规则（RAII 覆盖检测）
- **扩展评测集：** cases.json 从 5 条扩至 10-15 条，覆盖更多语言特性和 bug 模式

## 团队

| 成员 | GitHub | 职责 |
|------|--------|------|
| love-June | [@love-June](https://github.com/love-June) | 规则引擎 + 评测集 + 项目文档 |
| KINGXD414 | [@KINGXD414](https://github.com/KINGXD414) | AST 分析器 + 主 Pipeline + CMake + 评测框架 + v0.2.0 三招 |


## ☁️ KODO 对象存储集成（v0.3）

`ai-pr-review` 可以在生成 review 报告后，**自动把 Markdown 报告上传到七牛云 KODO**，得到一个稳定的公网 URL —— 评委 / 队友点链接即看，不必 clone 仓库或本地构建。

> 实现走的是「最小依赖」路线：直接复用项目已有的 cpp-httplib + OpenSSL HMAC-SHA1 + nlohmann/json 手写上传 token，**没有引入任何 Qiniu SDK**。代码集中在 `code/src/storage/kodo_client.{h,cpp}`，约 230 行。

### 配置

通过环境变量提供凭证（**绝不写入仓库**）：

| 变量名 | 说明 |
|---|---|
| `QINIU_AK` | AccessKey |
| `QINIU_SK` | SecretKey |
| `QINIU_BUCKET` | bucket 名 |
| `QINIU_REGION` | 区域 id，默认 `z0`（华东-浙江） |
| `QINIU_DOMAIN` | 公网外链域名（可选；不填则不返回 URL） |

### 使用

```bash
export QINIU_AK=...
export QINIU_SK=...
export QINIU_BUCKET=ai-pr-review-demo
export QINIU_REGION=z0
export QINIU_DOMAIN=tfvs8e8zx.hd-bkt.clouddn.com

ai-pr-review --pr https://github.com/owner/repo/pull/123 \
             --output review.md \
             --upload-kodo
```

输出会多一段 `=== KODO UPLOAD ===` 块，包含 bucket / key / hash / URL。

### 安全设计

- **scope 锁到单个 key**：上传 token 的 `PutPolicy.scope` 写成 `bucket:key` 而不是 `bucket`，即使 token 在 demo 中被截屏泄露，攻击者也只能覆盖**这一个 key**，不能往 bucket 里塞任意垃圾。
- **deadline 默认 1h**：token 过期即作废。
- **凭证只读环境变量**：CI 走 GitHub repo secret，本地走 shell env，二进制和 git 历史里都不留痕。

## License

MIT

---

## 🚀 部署 / Cross-Platform Usage

> 项目原生跨平台（C++17 + CMake + FetchContent 拉取头文件依赖），实测在 Ubuntu 22.04 上 CI 通过。下面给出三种"装到哪都能跑"的姿势，按上手成本排序。

---

### 方式 A：Docker（推荐 — 任意平台一行命令）

适用于 Linux / macOS / Windows（WSL2 或 Docker Desktop）。

**从 GitHub Container Registry 拉镜像**（`v0.2.0` 已自动构建并发布，仓库公开后可直接拉取）：

```bash
docker pull ghcr.io/kingxd414/ai-pr-review:latest

docker run --rm \
  -e DEEPSEEK_API_KEY=$DEEPSEEK_API_KEY \
  -e GITHUB_TOKEN=$GITHUB_TOKEN \
  -v "$PWD":/work \
  ghcr.io/kingxd414/ai-pr-review:latest \
  --pr https://github.com/chenshuo/muduo/pull/729 \
  --persona linus --consensus 3 --self-critique \
  --output /work/review.md
```

跑完后 `review.md` 直接落到当前目录。镜像大小约 600MB（libclang runtime 占大头）。

**或本地自构建**：

```bash
docker build -t ai-pr-review:dev .
docker run --rm ai-pr-review:dev --help
```

---

### 方式 B：GitHub Release 下载预编译二进制（Linux x86_64）

```bash
# 下载并解压
curl -L https://github.com/KINGXD414/ai-pr-review/releases/latest/download/ai-pr-review-v0.2.0-linux-x64.tar.gz \
  | tar xz
cd ai-pr-review-v0.2.0-linux-x64

# 直接跑（需 Ubuntu 22.04 同等的 libssl3 / libcurl4 / libclang1-14 runtime）
./ai-pr-review --version
```

适合不想编译、只想试跑的 reviewer。

---

### 方式 C：源码编译（三平台通用）

#### Linux（Ubuntu 22.04 / Debian 12）

```bash
sudo apt-get install -y \
  build-essential cmake pkg-config \
  libcurl4-openssl-dev libssl-dev libsqlite3-dev \
  libspdlog-dev libfmt-dev nlohmann-json3-dev \
  libclang-14-dev clang-14

cmake -B build -S code -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/ai-pr-review --help
```

#### macOS（Apple Silicon / Intel）

```bash
brew install cmake llvm spdlog fmt nlohmann-json

# llvm 在 brew 上不在 PATH，需要显式告诉 CMake 在哪
cmake -B build -S code -DCMAKE_BUILD_TYPE=Release \
  -DLIBCLANG_INCLUDE_DIR=$(brew --prefix llvm)/include \
  -DLIBCLANG_LIBRARY=$(brew --prefix llvm)/lib/libclang.dylib
cmake --build build -j$(sysctl -n hw.ncpu)
```

#### Windows（vcpkg + MSVC，理论可行，未在 CI 验证）

```powershell
vcpkg install curl[ssl] sqlite3 spdlog fmt nlohmann-json
# libclang 需要单独装 LLVM Windows 二进制：https://releases.llvm.org/

cmake -B build -S code -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT\scripts\buildsystems\vcpkg.cmake
cmake --build build --config Release
```

> ⚠️ 本项目 72h 开发周期内只在 Ubuntu 22.04 完整 CI 验证。macOS / Windows 编译路径基于 CMake 跨平台特性设计，欢迎 issue / PR 报告平台问题。

---

### 凭证准备

跑之前需要两个环境变量：

| 变量 | 用途 | 获取方式 |
|---|---|---|
| `DEEPSEEK_API_KEY` | LLM 调用 | https://platform.deepseek.com/ |
| `GITHUB_TOKEN` | 拉 PR diff（公开仓库可省，私有仓库必填） | GitHub Settings → Developer settings → PAT |

```bash
export DEEPSEEK_API_KEY="sk-xxxxxxxx"
export GITHUB_TOKEN="ghp_xxxxxxxx"
```
