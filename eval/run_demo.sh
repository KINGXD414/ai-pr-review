#!/usr/bin/env bash
# =============================================================================
# run_demo.sh — AI PR Review 三招差异化 Demo 脚本
#
# 用法:
#   ./run_demo.sh [--pr <PR_URL>] [--binary <PATH>]
#
# 依次运行三个 Demo:
#   1. Linus Persona  (--persona linus)
#   2. Self-Critique  (--self-critique)
#   3. Consensus N=3  (--consensus 3)
#
# 环境变量 (必需):
#   DEEPSEEK_API_KEY   DeepSeek API Key
#   GITHUB_TOKEN       GitHub PAT (可选, 不设则匿名 60req/h)
#
# 输出:
#   eval/demo-output/  目录下 3 份 Markdown 报告 + 用时统计
# =============================================================================
set -euo pipefail

# ---- 默认参数 ----
PR_URL="https://github.com/chenshuo/muduo/pull/505"
BINARY="${BINARY:-./build/ai-pr-review}"
OUTPUT_DIR="eval/demo-output"

# ---- 解析参数 ----
while [[ $# -gt 0 ]]; do
    case "$1" in
        --pr)     PR_URL="$2"; shift 2 ;;
        --binary) BINARY="$2"; shift 2 ;;
        -h|--help)
            echo "Usage: $0 [--pr <PR_URL>] [--binary <PATH>]"
            exit 0 ;;
        *) echo "Unknown option: $1"; exit 1 ;;
    esac
done

# ---- 前置检查 ----
if [[ -z "${DEEPSEEK_API_KEY:-}" ]]; then
    echo "ERROR: DEEPSEEK_API_KEY not set" >&2
    exit 1
fi
if [[ ! -x "$BINARY" ]]; then
    echo "ERROR: Binary not found or not executable: $BINARY" >&2
    echo "Hint: cd code/build && cmake .. && make -j\$(nproc)" >&2
    exit 1
fi

mkdir -p "$OUTPUT_DIR"

echo "========================================="
echo "  AI PR Review — 差异化 Demo"
echo "========================================="
echo "  PR:       $PR_URL"
echo "  Binary:   $BINARY"
echo "  Output:   $OUTPUT_DIR/"
echo "========================================="
echo ""

COMMON_ARGS=(--pr "$PR_URL" --model deepseek-chat)
if [[ -n "${GITHUB_TOKEN:-}" ]]; then
    COMMON_ARGS+=(--github-token "$GITHUB_TOKEN")
fi

# ---- Demo 1: Linus Persona ----
echo ">>> Demo 1: Linus Persona (--persona linus)"
echo "    让 Reviewer 以 Linus Torvalds 风格审查代码"
echo "    特点：直率犀利，但对事不对人"
echo ""

START=$(date +%s%N)
$BINARY "${COMMON_ARGS[@]}" \
    --persona linus \
    -o "$OUTPUT_DIR/demo1-linus-persona.md" \
    -v
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
echo "    Demo 1 done: ${ELAPSED_MS}ms"
echo "    Report: $OUTPUT_DIR/demo1-linus-persona.md"
echo ""

# ---- Demo 2: Self-Critique ----
echo ">>> Demo 2: Self-Critique (--self-critique)"
echo "    一审 → 二审反思链，校准幻觉/严重度/建议"
echo "    特点：两轮审查，输出更可靠"
echo ""

START=$(date +%s%N)
$BINARY "${COMMON_ARGS[@]}" \
    --self-critique \
    -o "$OUTPUT_DIR/demo2-self-critique.md" \
    -v
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
echo "    Demo 2 done: ${ELAPSED_MS}ms"
echo "    Report: $OUTPUT_DIR/demo2-self-critique.md"
echo ""

# ---- Demo 3: Consensus N=3 ----
echo ">>> Demo 3: Consensus N=3 (--consensus 3)"
echo "    3 次独立 review + meta-review 合并"
echo "    特点：多票取共识，过滤过度建议"
echo ""

START=$(date +%s%N)
$BINARY "${COMMON_ARGS[@]}" \
    --consensus 3 \
    -o "$OUTPUT_DIR/demo3-consensus-3.md" \
    -v
END=$(date +%s%N)
ELAPSED_MS=$(( (END - START) / 1000000 ))
echo "    Demo 3 done: ${ELAPSED_MS}ms"
echo "    Report: $OUTPUT_DIR/demo3-consensus-3.md"
echo ""

# ---- 汇总 ----
echo "========================================="
echo "  Demo 完成! 报告目录: $OUTPUT_DIR/"
echo "========================================="
echo ""
echo "  1. demo1-linus-persona.md   Linus 人格化审查"
echo "  2. demo2-self-critique.md   自我反思链审查"
echo "  3. demo3-consensus-3.md     3 次投票共识审查"
echo ""
echo "  对比三份报告可以看到："
echo "  - Linus 模式：语气犀利，重点关注代码质量"
echo "  - Self-Critique：二审校准后幻觉更少"
echo "  - Consensus：多票合并后建议更稳健"
