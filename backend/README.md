# AI PR Review · Online Demo Backend

为前端演示页 `docs/index.html` 提供 HTTP API。

**0 依赖**：纯 Python 3 标准库（无需 pip install）。

## 设计

- 凭证（DeepSeek API key / GitHub token）通过环境变量注入，不暴露给前端
- 白名单 6 个 repo，其他 PR 一律拒绝（防 API 被滥用）
- IP rate limit：每 IP 每小时 10 次（可配置）
- 单次执行超时：60s
- 结果缓存：同 PR+mode 半小时内复用

## 部署（在小 VM `xd@xd-virtual-machine` 上）

### 1. 准备二进制和依赖

```bash
# 二进制（v0.2.0）
mkdir -p ~/demo && cd ~/demo
TOKEN="<your_github_pat>"
curl -sL -H "Authorization: token $TOKEN" -H "Accept: application/octet-stream" \
  "https://api.github.com/repos/KINGXD414/ai-pr-review/releases/assets/434181552" \
  -o v020.tar.gz
tar -xzf v020.tar.gz

# 动态库依赖
sudo apt-get install -y libspdlog1 libfmt8 libclang1-14
```

### 2. 启动后端

```bash
cd ~/code/AI_PR_Review/backend

export DEEPSEEK_API_KEY="sk-..."
export GITHUB_TOKEN="ghp_..."

# 可选自定义
# export RATE_LIMIT_PER_HOUR=10
# export EXEC_TIMEOUT_SEC=60
# export PORT=8000

python3 app.py
```

输出 `[boot] listening on 0.0.0.0:8000` 即可。

### 3. ngrok 公网暴露（另开一个终端）

```bash
# 装 ngrok（一次性）
curl -sSL https://ngrok-agent.s3.amazonaws.com/ngrok.asc | sudo tee /etc/apt/trusted.gpg.d/ngrok.asc >/dev/null
echo "deb https://ngrok-agent.s3.amazonaws.com buster main" | sudo tee /etc/apt/sources.list.d/ngrok.list
sudo apt-get update && sudo apt-get install -y ngrok

# 注册 ngrok.com 拿 authtoken，配置
ngrok config add-authtoken <your_ngrok_token>

# 启动隧道
ngrok http 8000
```

会输出类似：
```
Forwarding  https://abc-123.ngrok-free.app -> http://localhost:8000
```

### 4. 配置前端

把 ngrok URL 写入 `docs/config.js`：

```js
window.BACKEND_URL = "https://abc-123.ngrok-free.app";
```

push 后 GitHub Pages 自动更新。

### 5. 测试

```bash
curl https://abc-123.ngrok-free.app/health
```

应返回 `{"status": "ok", "bin_exists": true, ...}`。

## 关闭演示窗口

直接 `Ctrl+C` 关掉 `python3 app.py` 和 `ngrok` 即可。前端会显示「后端离线」，但 8 份预生成报告（快速演示）仍可访问。

## API

### GET /health
返回服务状态。

### POST /api/review
请求体：
```json
{ "url": "https://github.com/chenshuo/muduo/pull/729", "mode": "linus" }
```

`mode` 可选：`baseline` / `linus` / `critique` / `consensus`

返回：
```json
{
  "success": true,
  "cached": false,
  "report_md": "# AI PR Review 报告\n...",
  "runtime_sec": 8.3,
  "repo": "chenshuo/muduo",
  "pr": 729,
  "mode": "linus"
}
```

错误码：
- 400 URL/mode 不合法
- 403 repo 不在白名单
- 429 超过 rate limit
- 504 执行超时
- 500 二进制错误
