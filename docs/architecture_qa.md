# AI PR Review — 架构问答与工程取舍

> 本文档把项目的架构决策、实现细节与已知限制整理成一组问答，
> 便于在代码评审、技术分享或项目交接时快速对照。**每条结论都对应代码中的具体位置或评测数据。**

---

## 0. 关键数字（实现速查）

| 项目 | 数字 |
|---|---|
| Agent 工具数 | **6 个**（`run_rule_engine` / `analyze_ast` / `rag_query` / `draft_comment` / `select_skill` / `list_skills`），其中 `rag_query` 需 `--rag` 才挂载 |
| Skill 数 | **7 个**（5 persona + `self_critique` + `consensus`） |
| 内置静态规则 | **8 条**；规则引擎在主流程中**始终运行**，命中结果注入 prompt |
| 测试 | 7 个 CMake 测试目标，本地 `ctest` **7/7 全过** |
| MCP 工具 | 4 个（`review_pr` / `list_rules` / `list_skills` / `run_eval`） |

代码位置：`code/src/agent/agent.cpp` 的 `build_tools()`、`code/src/prompt/skills.cpp`、`code/src/rules/engine.cpp`、`mcp/server.py`。

**v0.3.0 评测分数（5 cases 加权平均，5 种配置各跑一次 = 25 次调用全成功）**：baseline **89%** / +RAG **84%**（−5）/ +Agent 88% / +Agent+RAG+Embed 87% / +persona-linus **92%**（最高）。v0.3.0 baseline 比 v0.2.0 的 82% 高 **+7 分**——这是「规则引擎接通主流程 + Agent 结果覆盖修复」的实证收益（规则引擎在 v0.2.0 里未接入主流程，见下文取舍记录）。

---

## 1. 动机与整体架构

**Q：为什么做这个工具，而不是用现成的 CodeRabbit / Copilot Review？**
> 三点差异定位：
> 1. **可控**：规则引擎的结果由项目自身确定，不依赖模型输出——这是压幻觉的地基。
> 2. **可嵌入**：它是 C++ 二进制 + CI workflow + MCP server，能塞进任意流水线，而非只能在浏览器里点。
> 3. **可量化**：评测框架用 20 次真实 LLM 调用跑出 5×4 矩阵，每种配置的好坏有数字支撑，而非"我觉得更好"。

**Q：为什么用 C++ 写？AI 应用不是都用 Python？**
> 项目目标是**编译成单个二进制、跑在 CI 里、无运行时依赖**，同时要用 libclang 做真实 AST 分析——libclang 的 C API 用 C++ 调用最自然。Python 生态（LangChain 之类）在此场景帮不上忙，反而引入版本地狱。
> 真正需要胶水的地方（MCP server）用纯 Python 标准库，因为重点是协议而非性能。**该用 C++ 的地方用 C++，该用 Python 的地方用 Python**。

**Q：三层架构里，为什么必须先把规则引擎的结果喂给 LLM？**
> 核心逻辑：**LLM 擅长判断，不擅长精确枚举**。让它自己在 diff 里数"哪个 malloc 没配 free"，会漏、会编。规则引擎是确定性枚举，命中即命中，行号精确。把 RuleHit 列表喂进去，等于**给模型一份带行号的事实清单**，它只需做归因和建议。
> 对应的幻觉治理：评测中专设 `must_not_hallucinate` 维度（占 30% 权重），专门惩罚"编造不存在的标识符"。

---

## 2. Function Calling / Agent

**Q：Agent 和普通 prompt 调用到底区别在哪？**
> 普通调用是**编排方编排**模型：fetch → 拼 prompt → 一次调用，工具选择与顺序全部写死。
> Agent 是**模型编排**工具：把 6 个工具的 JSON Schema 发给模型，它每轮返回 `tool_calls`，本地执行并把结果以 `role: tool` 消息回灌，再让它继续。模型会自己决定"这个 diff 有 malloc 我先跑规则引擎""涉及虚函数我查 AST""该文件在 test 目录，风险低"。
> 一句话：**workflow 是画好的流程图，Agent 是给定能力和上下文后由它自己走。**

**Q：怎么保证它不会死循环？**
> 三重保险：
> 1. **有界循环**：`--agent-max-iter`，默认 6，硬上限 12，超了取最后一份 `draft_comment` 内容兜底，绝不无限自旋。
> 2. **终止条件明确**：模型返回不带 `tool_calls` 的纯文本即收敛。
> 3. **temperature 固定**，同一份输入行为可复现。
> 另外 `agent_loop_tests` 用假网关脚本化 5 轮响应（规则引擎 → list_skills → select_skill → rag_query → 终稿），断言 reports 非空、轮数正确。**不依赖网络、不依赖真实模型**，CI 里永远能跑。

**Q：工具调用失败了怎么办？**
> 工具返回字符串，含错误语义（如"`--source-dir` 未配置，AST 分析不可用"）。这些错误信息**原样回灌给模型**，让它自己决定降级策略（改用规则引擎结果，或直接基于 diff 判断）。这比在代码里写 try-catch 兜底更灵活：**把失败变成模型可见的信息，而非异常**。

**Q：`tool_calls` 的具体协议？**
> OpenAI 兼容的 `tools` 协议：请求体带 `tools` 数组（每项含 `name`/`description`/`parameters` JSON Schema）；响应里 `choices[0].message.tool_calls[]` 每项有 `id`/`function.name`/`function.arguments`。执行后按 `tool_call_id` 一一对应回灌 `role: tool` 消息——**这个 id 必须对齐，否则模型认不出哪个结果对应哪个调用**，是实现里最容易错的一步。

---

## 3. RAG / Embedding / 向量库

**Q：为什么 RAG 默认走 BM25 而不是向量检索？**
> 1. **零依赖、断网可演示**——CI、内网、离线都能跑。向量路径要 embedding 模型或远端 API，引入密钥、网络、延迟三个故障点。
> 2. **确定性**——BM25 输出稳定可复现，便于测试；远端 embedding 每次可能漂移。
> 3. 本场景（几百到几千条规范条文）**BM25 召回质量并不比小向量模型差**，而成本低几个数量级。
> `Retriever::search_best()` 是统一入口，检测到 embedding 就走向量余弦，否则自动回退 BM25。**默认走便宜稳的路，能力留在开关后面。**

**Q：embedding 是什么模型？**
> 默认 `HashingEmbedder`：字符 n-gram 哈希 + L2 归一化，捕捉字面重叠而非语义相似度（见已知限制 §6）。它是离线、确定性、零依赖的，用于打通并验证整条 VectorStore/检索流水线。语义能力通过 `--embed` 开启，且 `Embedder` 是抽象接口，实现 `RemoteEmbedder`（如 bge-m3 / text-embedding-3）即可替换，调用方零改动。

**Q：向量库为什么自己写暴力余弦，不用 hnswlib / Milvus？**
> 语料量级千级 chunk，暴力余弦 O(n) 毫秒级完成，**正确性优先**。`VectorStore` 是接口，`BruteForceStore` 只是其一实现；数据规模上来换 `HnswStore` 即可，调用方零改动。工程态度：**先用能验证正确性的朴素实现打通端到端，把抽象边界立住，再按瓶颈替换实现**。

**Q：向量索引为什么要落盘 JSON？**
> 把 RAG 生命周期做成**可复现闭环**：`--embed --index xxx.json` 首次构建保存，之后直接加载跳过重新嵌入。该文件是真实产物（id + vector + text），可提交仓库、分发同事、CI 复用。**"向量数据库"在此不是空话，是有实体文件的概念。**

**Q：语料从哪来？**
> 两部分：内置 8 条规则的知识描述 + `--rag-corpus-dir` 挂接团队自己的 `.md/.txt`，使"团队约定"类软规范也能进检索。

---

## 4. Skills / MCP

**Q：Skills 和 prompt 模板有什么区别？不就是换个说法包装吗？**
> 设计约束是：**不重复任何一行 prompt 逻辑**。5 个 persona 和 2 个元策略都是已跑过评测、有分数的资产（linus **92%**（v0.3.0）/ self-critique 80% / consensus 82%（v0.2.0）），Skill 注册表只加三样：**id、自然语言描述、索引**。
> 由此产生新能力：**模型可自己发现并选择策略**——Agent 里 `list_skills` 让其知道有哪些选项，`select_skill` 让其按 diff 特征选（如见 test 目录用 zen 模式，见内存操作用 devil 模式）。**从"人配置策略"变成"模型选择策略"**，是 Skill 的价值所在。

**Q：MCP 是什么？为什么需要它？**
> MCP 是模型调用外部能力的**标准协议**，解决 N 客户端 × M 工具的集成爆炸。实现 `mcp/server.py`：**纯 Python 标准库**，stdio JSON-RPC 2.0，约 200 行。它只做一件——**协议翻译**：把 JSON-RPC 请求翻译成对同一个 C++ 二进制的命令行调用。
> 关键设计：**server 不复制任何业务逻辑**，所有审查能力来自同一 `ai-pr-review` 二进制。CLI、CI、MCP 三条路径行为完全一致，无"CLI 与 MCP 结果不同"的分叉。

**Q：怎么接到 Claude Code / Cursor？**
> 在客户端 mcp.json 配一个 stdio server：`python3 mcp/server.py`，环境变量指到二进制路径与 API key。之后模型在对话里可直接调 `review_pr`——**"代码审查"成为其能力集中的一个原生动作**。

---

## 5. C++ 工程实现

**Q：AST 分析怎么做的？**
> 用 libclang 的 **C API**（`clang_parseTranslationUnit` + `clang_visitChildren`），非手撕语法。提取三类信息：类及其基类（继承/多态影响）、函数签名（API 变更）、包含文件。
> 踩坑：libclang 需编译期参数才能正确解析项目（`-std=c++17`、include 路径），故接受外部传入；Windows/macOS 上 libclang 不在默认 PATH，CMake 写了多版本探测（12~18）+ 用户环境变量覆盖。

**Q：HTTP / HTTPS 怎么做的？为什么 httplib 锁 v0.14.3？**
> cpp-httplib（header-only）+ OpenSSL。锁 v0.14.3 因 **v0.15+ 要求 OpenSSL ≥ 3.0，而 Ubuntu 20.04 是 1.1.1f**——为兼容性区间最大化，选同时支持两者的最后版本。此取舍在 README 有注释。

**Q：LLM 调用的重试怎么设计？**
> 传输层重试（超时、连接失败）与**业务层重试**分开：前者有限次重试 + 退避；后者针对"LLM 返回格式不对"。每条路径记录 `attempts`，最终报告可见本次审查花了多少次调用——**可观测性对排查成本很重要**。

---

## 6. 已知限制与演进路径

> 下列均为项目当前边界，已在设计中明确取舍并规划演进。

**① HashingEmbedder 非真正语义嵌入**
> 它是字符 n-gram 哈希 + L2 归一化，捕捉字面重叠，非语义相似度。用途是提供离线、确定性、零依赖的完整端到端路径，并验证 VectorStore/检索流水线正确性。演进：`Embedder` 抽象接口实现 `RemoteEmbedder`（bge-m3 / text-embedding-3）替换注入，调用方零改动。

**② BM25 对中文分词支持弱**
> 当前分词按 ASCII 字母数字切，**CJK 被当分隔符**，中文语料词法召回差。语义路径（`HashingEmbedder` 按字符 n-gram）对中文有效，故中文场景建议 `--rag --embed`。完整方案：换 jieba 等中文分词器或直接上中文 embedding 模型。

**③ 规则引擎是逐行 / 正则匹配，非数据流分析**
> 发现不了跨函数所有权转移（如 A 函数 malloc、B 函数 free），不做别名分析。定位上不与其竞品（clang-tidy）对标——其角色是给 LLM 提供低成本、高确定性的证据，兼做软治理（test 文件降噪、TODO 提示）。边界清楚本身即设计一部分。

**④ 评测样本量小（5 cases）**
> 绝对分数统计意义有限。真实价值在两点：**相对对比**（同批 case 上不同配置排序稳定）与**回归护栏**（改代码后跑同一组 case 防退化）。论文级结论需扩到几十上百条。

**⑤ 无流式输出，单次调用同步阻塞**
> 已知短板，列于"未来扩展"。未做原因非没考虑到，而是优先级——CI / MCP 批处理场景对延迟不如交互式 IDE 敏感。

**⑥ consensus / RAG 为实测负面结论**
> consensus 零增益却 3 倍开销、RAG 现实现下 −5%，均为 v0.3.0 评测矩阵**主动暴露的负面数据**。RAG 已列入"先修检索质量门禁（相关性阈值 + 去重 + 命中才注入），再默认开启"的 TODO；Agent 定性为能力储备。对每项功能做量化验证，不靠"听起来更厉害"下结论。

---

## 7. 踩坑与工程经验

**经验 1：README 记载三层架构，代码里规则引擎未接入主流程（最高价值修复）**
> 项目文档写规则引擎 → AST → LLM 三层。加 RAG 前需确认 RuleHit 怎么进 prompt，遂读 `main.cpp`，发现其从未 include `rules/engine.h`，规则引擎是未接入的孤岛。未照文档假设接着写，而是**先对着实现验证文档说法**，再将 `RuleEngine().run()` 接进主流程、把 RuleHit 渲染成独立区块注入 prompt（P0 修复）。现规则引擎在主流程必然执行，幻觉治理有了确定性来源。
> 由此形成习惯：**对着实现验证文档，而非假设文档正确**。规则引擎有独立单测、单测绿不代表被集成——**测试通过 ≠ 功能生效**，也是后续补集成级测试的动因。

**经验 2：受限开发环境（libclang / OpenSSL / 网络均不可用）**
> 未硬等环境，而是写 stub 头（nlohmann/json、spdlog、CLI、httplib、ast_analyzer）+ 本地编译器做**离线语法与逻辑验证**，先把 RAG、Agent 循环、MCP 协议正确性验证完。后在真实环境（WSL + libclang-14）一次性编译通过，7/7 测试全绿。受限环境不阻塞自己，但清楚 stub 验证的边界——必在真实工具链复跑。

**经验 3：stub 假实现让测试"假过"**
> 最初 JSON stub 的 `parse()` 永远返回空对象，使向量库 save/load 往返测试看似通过。发现后升级为**小而真实的 JSON 实现**（递归下降 parser + dump）。教训：**测试环境若与真实环境行为不一致，绿灯无意义**。

**经验 4：新功能不是堆上去就涨分（v0.3.0 评测矩阵）**
> v0.3.0 加了 RAG、Agent、Embedding 三件套，用 `ai-pr-eval` 把 5 种配置各跑 5 条真实 PR 量化：+RAG 把 89% 拉到 84%（−5%，检索注入抬高幻觉率）；+Agent 基本持平（88%，−1%），因 5 条用例单次推理已够；最高反而是最便宜的 persona-linus（92%）；+Agent+RAG+Embed 组合（87%）竟低于纯 baseline（89%）——未各自调优就叠加只会互相干扰。
> 结论：对每个功能做量化验证，**feature discipline 也是工程能力**。

---

## 8. 下一步演进

1. **`RemoteEmbedder` + `HnswStore`**：填充抽象接口，验证"换实现不动调用方"，同时获得真语义能力
2. **SSE 流式输出**：当前 DeepSeek 调用同步阻塞，流式可显著改善交互体验
3. **增量 AST 缓存**：缓存上次 AST 索引，仅重解析变更文件（大仓库关键优化）
4. **扩展评测集到 10-15 条**，并加入"修复是否被采纳"的真实反馈闭环
5. **GitHub App 化**：从 CLI 工具升级为 PR 自动触发

---

## 9. 快速上手命令

```bash
cd /mnt/d/c++perject/AI_PR_Review/code/build

# ① 零依赖：列出可用 Skill（无需任何 Key）
./ai-pr-review --list-skills

# ② RAG 能力：BM25 与语义路径均可用（完整演示需 Key）
export GITHUB_TOKEN=ghp_xxx DEEPSEEK_API_KEY=sk-xxx
./ai-pr-review --pr https://github.com/chenshuo/muduo/pull/705 \
    --agent --rag --embed --index /tmp/muduo.index.json -o /tmp/review_705.md
cat /tmp/review_705.md
ls -lh /tmp/muduo.index.json          # 真实落盘的向量索引

# ③ 工程化：测试全绿
ctest --output-on-failure

# ④ MCP：让任意 MCP 客户端直接拥有此能力
cd ../.. && python3 mcp/server.py    # stdio 模式，由客户端驱动
```

---

## 10. 核心设计原则

1. "**workflow 是画好的流程图，Agent 是给定能力、它自己走。**"
2. "**默认走便宜稳妥的路，把能力留在开关后面。**"
3. "**测试通过 ≠ 功能生效**——本项目最贵的教训。"
4. "**负面结论也保留：consensus 零增益却 3 倍开销，RAG 现实现下反而 −5%。**"
5. "**先写能验证正确性的朴素实现，把抽象边界立住，再按瓶颈替换实现。**"
6. "**新功能不是堆上去就涨分——用评测矩阵证伪'功能越多越好'，把能力留在开关后面、用数据决定默认开不开。**"

---

## 11. PR 流转全流程（对应 main.cpp / agent.cpp / pr_review.yml）

**一句话总览**：一个 PR 进来，系统走【触发 → 抓取 → 上下文构建 → 大模型推理 → 交付】五条主线，最终以一条幂等 comment 贴回 PR。核心不是"调一次大模型"，而是把链路做成**可观测、可重试、可幂等的流水线**。

1. **触发**：两条入口——CI 自动（PR 打开/更新触发 GitHub Action `pr_review.yml`）和人工（工程师在 Cursor 里点 `review_pr` MCP 工具）。两条路都落到同一二进制 `ai-pr-review`。
2. **抓取**：解析 PR URL、校验 token，调 GitHub API 拉 metadata + 改动文件 + patch diff，带 **3 次重试 + 退避** 避免限流 5xx（`api_client.cpp` 的 `kMaxRetries`）。
3. **上下文构建**：并行三件事——规则引擎（**始终跑**，8 条硬规则命中注入 prompt）、AST 分析（可选 `--source-dir`，libclang）、RAG 检索（可选 `--rag`/`--embed`）。
4. **推理**：persona + diff + 规则命中组装进 prompt 交 DeepSeek。`--agent` 开关：开 → Function Calling 多轮循环（模型自决调哪些工具）；关 → 单次 LLM，或 Consensus 多投 / Self-Critique 二审。
5. **交付**：渲染 Markdown，落盘，可选传 KODO；CI 模式下用 sticky comment（固定 HTML 锚点）**幂等回贴**——同一 PR 永远只占一条评论，不刷屏。

**三个设计亮点**：
- **规则引擎必须接入主流程**：曾踩坑——规则算出却未注入 prompt 等于白做。这是 P0 修复，也是 v0.3.0 评测 82%→89% 根因。
- **Agent 模式跳过 Consensus / Self-Critique 后处理**：Agent 循环已多轮自我迭代，再跑固定后处理会用普通 chat 结果覆盖 agent 产出且多花两份钱（`main.cpp` 有注释）。
- **sticky comment 幂等回贴**是高并发 / PR 反复更新时的必要工程设计。
