# Demo 录制工具包

> 生成时间：2026-05-31 14:10
> 二进制版本：v0.2.0 Release（Release sha256 校验通过）
> 用途：录屏时展示预跑好的报告，避免现场翻车 / LLM 慢 / API 超时

---

## 环境准备（录前 5 分钟）

```bash
# VM 小机器上
cd ~/code/AI_PR_Review/code/build
export DEEPSEEK_API_KEY="sk-29ae1609ae8d4f24b6560b4760efc29b"
export GITHUB_TOKEN="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"
export QINIU_AK="LTfj34dqwNlloOydMlSUEIPU_RNLwklznsnHx1tu"
export QINIU_SK="6IQbNS-XvEfnPW-vcxnT3qx2dYDv5NoUj3EKrldd"
export QINIU_BUCKET="ai-pr-review-demo"
export QINIU_REGION="z0"
export QINIU_DOMAIN="tfvs8e8zx.hd-bkt.clouddn.com"

# 验证二进制
./ai-pr-review --version
```

## 章节 2：Self-Reviewing Workflow（1:30 ~ 4:30）

### 方案 A：现场跑（推荐，说服力最强）

```bash
# PR #26 — 自己的仓库，9 秒出结果
./ai-pr-review --pr https://github.com/KINGXD414/ai-pr-review/pull/26 -o /tmp/demo_pr26.md
cat /tmp/demo_pr26.md
```

### 方案 B：展示预跑结果（现场跑翻车时备用）

```bash
cat demo/recording_kit/chapter2_self_reviewing/01_baseline_pr26.md
```

**切镜顺序**：
1. 浏览器 PR #26 页面 → timeline 区域
2. 展开 sticky comment（AI 评论 3 处可改进）
3. 终端跑命令 / cat 预跑结果
4. 回浏览器 commit diff 页面（修复代码）
5. 回 PR #26 → 二跑 sticky comment 原地更新

## 章节 3：5×4 评测矩阵（4:30 ~ 7:00）

### 演示命令（按脚本顺序）

```bash
# 1. baseline（6s）
./ai-pr-review --pr https://github.com/chenshuo/muduo/pull/729 -o /tmp/demo_baseline.md

# 2. persona linus（9s）
./ai-pr-review --pr https://github.com/chenshuo/muduo/pull/729 --persona linus -o /tmp/demo_linus.md

# 3. self-critique（12s）
./ai-pr-review --pr https://github.com/chenshuo/muduo/pull/729 --self-critique -o /tmp/demo_critique.md

# 4. consensus 3（19s）
./ai-pr-review --pr https://github.com/chenshuo/muduo/pull/729 --consensus 3 -o /tmp/demo_consensus.md
```

### 预跑结果备份（现场跑翻车时 cat）

| 文件 | 对应模式 | 耗时 | 行数 |
|---|---|---|---|
| chapter3_evaluation/01_baseline.md | baseline | 6s | 39 |
| chapter3_evaluation/02_persona_linus.md | persona linus | 9s | 46 |
| chapter3_evaluation/03_self_critique.md | self-critique | 12s | 46 |
| chapter3_evaluation/04_consensus_3.md | consensus 3 | 19s | 63 |
| chapter3_evaluation/05_muduo505_baseline.md | muduo-505 baseline（反差） | 6s | 39 |
| chapter3_evaluation/06_muduo505_linus.md | muduo-505 linus（反差） | 8s | 40 |
| chapter3_evaluation/07_json5187_baseline.md | json-5187 baseline | 6s | 39 |

### 切镜重点

1. baseline → persona linus：对比语气差异（linus 严厉尖锐）
2. baseline → self-critique：展示二审反思补了什么
3. baseline → consensus：展示 `[共识 3/3]` 票数标注 + 置信度
4. 切 README 评测矩阵表：5×4 数据总览

## 章节 4：工程化诚实（7:00 ~ 9:30）

### KODO 上传演示

```bash
# 需要最新 main 编译的二进制（v0.2.0 Release 没有 --upload-kodo flag）
# 用 VM 上 git pull + make 编译最新版本后跑：
./ai-pr-review --pr https://github.com/nlohmann/json/pull/5187 --upload-kodo -o /tmp/demo_kodo.md

# 成功后终端会打印 KODO URL，浏览器验证可访问
```

### 其他工程亮点（直接切浏览器/文件）

- ADR 决策记录：`docs/adr/`
- GitHub Actions 双 workflow：`.github/workflows/`
- Release v0.2.0 sha256：`ai-pr-review-v0.2.0-linux-x64.tar.gz.sha256`
- KODO 零 SDK 代码：`code/src/storage/kodo_client.cpp`（228 行）

---

## 录制建议

1. **终端字体** 16pt+（Cascadia / SauceCode），确保录屏清晰
2. **先跑一遍预跑素材**确认 VM 网络通、API 可用
3. **KODO 上传必须提前验证**：这是最容易翻车的环节，建议录前 30 分钟跑一次
4. **LLM 等待期间**：录屏时可以切到预跑结果展示，后期剪辑加速等待
5. **环境变量不要出现在录屏**：export 命令前切镜头或提前配好
