# AI PR Review 评测报告

> 由 `ai-pr-eval` 自动生成。每条 case 以「关键词命中 40% + 严重度匹配 30% + 无幻觉 30%」加权计分。

| 项 | 值 |
| --- | --- |
| 用例数 | 2 |
| 模型 | deepseek-chat |
| 二进制 | `./build/ai-pr-review` |
| 输出目录 | `eval/runs/2026-05-30_1836` |

## 总览

| ID | 难度 | 关键词 | 严重度 | 无幻觉 | 综合 | 耗时(ms) | 状态 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `muduo-729` | low | 4/4 | 0/1 | 3/3 | 70% | 11085 | ✅ |
| `muduo-505` | high | 3/3 | 2/2 | 2/2 | 100% | 7968 | ✅ |

**整体平均分（仅计入执行成功用例）**: **85%** (2/2 ok)

## 每条用例详情

### `muduo-729` — [FIX] compile: missing include of noncopyable.h

- PR: https://github.com/chenshuo/muduo/pull/729
- 难度: low
- 执行: ✅
- 关键词命中: 4/4 — `[noncopyable, Hiredis, 头文件, include]`
- 严重度匹配: 0/1
- 幻觉命中: 0/3（越少越好）
- 综合分: 70%

**review 预览（前 400 字）**:

```
# AI PR Review 报告

> 由 `ai-pr-review` 自动生成。仅供辅助参考，最终判断以 reviewer 为准。

| 项 | 值 |
| --- | --- |
| PR | [chenshuo/muduo #729](https://github.com/chenshuo/muduo/pull/729) |
| 标题 | fix missing header of noncopyable.h and coredump issue |
| 作者 | Horadrim-TalRasha |
| 文件数 | 3 |
| 模型 | deepseek-chat |
| LLM 调用次数 | 1 |
| 用时 | 7
```

### `muduo-505` — corner case: happens-before in Atomic / Mutex

- PR: https://github.com/chenshuo/muduo/pull/505
- 难度: high
- 执行: ✅
- 关键词命中: 3/3 — `[并发, 原子, 内存]`
- 严重度匹配: 2/2
- 幻觉命中: 0/2（越少越好）
- 综合分: 100%

**review 预览（前 400 字）**:

```
# AI PR Review 报告

> 由 `ai-pr-review` 自动生成。仅供辅助参考，最终判断以 reviewer 为准。

| 项 | 值 |
| --- | --- |
| PR | [chenshuo/muduo #505](https://github.com/chenshuo/muduo/pull/505) |
| 标题 | fix corner case : happens before issue |
| 作者 | ikenchina |
| 文件数 | 2 |
| 模型 | deepseek-chat |
| LLM 调用次数 | 1 |
| 用时 | 4961 ms |
| 生成时间 
```

