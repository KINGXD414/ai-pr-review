#!/usr/bin/env bash
# KODO 上传演示一键脚本
# 用法：
#   1) 编辑下方 export 段，把 <FILL> 替换为 SECRET.md 里的真值
#   2) chmod +x run_kodo_demo.sh && ./run_kodo_demo.sh
#
# 预期输出：终端最后出现 "=== KODO UPLOAD ===" 块，含 url
# 把 url 复制到浏览器 → 看到 markdown 报告 = Demo 视频里最炸裂镜头

set -euo pipefail

# ============================================================
# 0) 凭证（从 SECRET.md 拷过来替换 <FILL>）
# ============================================================
# SECURITY: never commit real credentials. Read from env / a local SECRET.md
# that is gitignored, or export them in your shell before running this script.
export DEEPSEEK_API_KEY='${DEEPSEEK_API_KEY:?set DEEPSEEK_API_KEY first}'
export GITHUB_TOKEN='${GITHUB_TOKEN:?set GITHUB_TOKEN first}'
export QINIU_AK='${QINIU_AK:?set QINIU_AK first}'
export QINIU_SK='${QINIU_SK:?set QINIU_SK first}'
export QINIU_BUCKET='ai-pr-review-demo'
export QINIU_REGION='z0'
export QINIU_DOMAIN='tfvs8e8zx.hd-bkt.clouddn.com'

# ============================================================
# 1) 拉最新 main（含 KODO 集成 PR #30）
# ============================================================
cd ~/code/AI_PR_Review
git pull origin main
echo "[OK] main 已更新到："
git log -1 --oneline

# ============================================================
# 2) 编译（增量，首次会自动 fetch cpp-httplib）
# ============================================================
cd code/build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --target ai-pr-review -j"$(nproc)"

# ============================================================
# 3) 跑 KODO 上传（拿 PR #30 自己做演示对象）
# ============================================================
./ai-pr-review \
  --pr https://github.com/KINGXD414/ai-pr-review/pull/30 \
  --persona linus \
  --output /tmp/demo-review.md \
  --upload-kodo \
  --kodo-prefix demo

# ============================================================
# 4) 提示
# ============================================================
echo ""
echo "[DONE] 把上面 === KODO UPLOAD === 块里的 url 复制到浏览器打开"
echo "[DONE] 本地报告也在 /tmp/demo-review.md，可对照"
