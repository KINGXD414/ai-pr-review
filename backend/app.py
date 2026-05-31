"""
AI PR Review · Online Demo Backend (Python stdlib only, no pip needed)
======================================================================
- 前端: docs/index.html (GitHub Pages)
- 后端: 此文件，跑在 VM 上 + ngrok 暴露公网
- 凭证: 通过环境变量 DEEPSEEK_API_KEY / GITHUB_TOKEN 注入，不暴露给前端
- 安全: 白名单 repo + IP rate limit + 超时 + 缓存
"""
import os
import re
import json
import time
import subprocess
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse
from collections import defaultdict, deque

# ── 配置 ──
BIN_PATH = os.environ.get(
    "AI_PR_REVIEW_BIN",
    os.environ.get(
        "BIN_PATH",
        os.path.expanduser("~/demo/ai-pr-review-v0.2.0-linux-x64/ai-pr-review"),
    ),
)
WHITELIST_REPOS = {
    "chenshuo/muduo",
    "nlohmann/json",
    "gabime/spdlog",
    "fmtlib/fmt",
    "abseil/abseil-cpp",
    "KINGXD414/ai-pr-review",
}
RATE_LIMIT_PER_HOUR = int(os.environ.get("RATE_LIMIT_PER_HOUR", "10"))
EXEC_TIMEOUT_SEC    = int(os.environ.get("EXEC_TIMEOUT_SEC",    "60"))
CACHE_TTL_SEC       = int(os.environ.get("CACHE_TTL_SEC",       "1800"))
PORT                = int(os.environ.get("PORT",                "8000"))

MODE_FLAGS = {
    "baseline":  [],
    "linus":     ["--persona", "linus"],
    "critique":  ["--self-critique"],
    "consensus": ["--consensus", "3"],
}

# ── State ──
_rate_lock = threading.Lock()
_ip_buckets = defaultdict(deque)
_cache_lock = threading.Lock()
_cache = {}

def check_rate_limit(ip):
    now = time.time()
    with _rate_lock:
        bucket = _ip_buckets[ip]
        while bucket and bucket[0] < now - 3600:
            bucket.popleft()
        if len(bucket) >= RATE_LIMIT_PER_HOUR:
            return False, int(3600 - (now - bucket[0]))
        bucket.append(now)
        return True, 0

def cache_get(key):
    with _cache_lock:
        if key in _cache:
            ts, val = _cache[key]
            if time.time() - ts < CACHE_TTL_SEC:
                return val
            del _cache[key]
    return None

def cache_set(key, val):
    with _cache_lock:
        _cache[key] = (time.time(), val)

# ── Handler ──
class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        print(f"[{time.strftime('%H:%M:%S')}] {self.address_string()} {fmt % args}")

    def _send_json(self, status, payload):
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Access-Control-Allow-Origin", "*")
        self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
        self.send_header("Access-Control-Allow-Headers", "Content-Type")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_OPTIONS(self):
        self._send_json(204, {})

    def do_GET(self):
        path = urlparse(self.path).path
        if path == "/health":
            self._send_json(200, {
                "status": "ok",
                "bin_exists": os.path.exists(BIN_PATH),
                "bin_path": BIN_PATH,
                "whitelist": sorted(WHITELIST_REPOS),
                "rate_limit_per_hour": RATE_LIMIT_PER_HOUR,
                "exec_timeout_sec": EXEC_TIMEOUT_SEC,
            })
        else:
            self._send_json(404, {"success": False, "error": "not found"})

    def do_POST(self):
        path = urlparse(self.path).path
        if path != "/api/review":
            self._send_json(404, {"success": False, "error": "not found"})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            data = json.loads(self.rfile.read(length).decode("utf-8")) if length else {}
        except Exception as e:
            self._send_json(400, {"success": False, "error": f"invalid JSON: {e}"})
            return

        url = (data.get("url") or "").strip()
        mode = data.get("mode", "baseline")
        ip = self.headers.get("X-Forwarded-For", self.client_address[0]).split(",")[0].strip()

        if mode not in MODE_FLAGS:
            self._send_json(400, {"success": False, "error": f"unsupported mode: {mode}"})
            return

        m = re.match(r"https?://github\.com/([^/]+/[^/]+)/pull/(\d+)", url)
        if not m:
            self._send_json(400, {"success": False, "error": "invalid PR URL"})
            return
        repo, pr = m.group(1), int(m.group(2))

        if repo not in WHITELIST_REPOS:
            self._send_json(403, {
                "success": False,
                "error": f"repo '{repo}' 不在白名单内。\n仅支持: {', '.join(sorted(WHITELIST_REPOS))}\n完整 CLI 见 https://github.com/KINGXD414/ai-pr-review"
            })
            return

        ok, wait = check_rate_limit(ip)
        if not ok:
            self._send_json(429, {"success": False, "error": f"已达每小时上限（{RATE_LIMIT_PER_HOUR} 次），请 {wait} 秒后再试"})
            return

        cache_key = f"{repo}|{pr}|{mode}"
        cached = cache_get(cache_key)
        if cached:
            self._send_json(200, {"success": True, "cached": True, **cached})
            return

        if not os.path.exists(BIN_PATH):
            self._send_json(500, {"success": False, "error": f"binary not found at {BIN_PATH}"})
            return

        out_path = f"/tmp/review_{int(time.time()*1000)}_{os.getpid()}.md"
        cmd = [BIN_PATH, "--pr", url, "-o", out_path] + MODE_FLAGS[mode]

        start = time.time()
        try:
            r = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=EXEC_TIMEOUT_SEC,
                env={**os.environ},
            )
        except subprocess.TimeoutExpired:
            self._send_json(504, {"success": False, "error": f"执行超时（> {EXEC_TIMEOUT_SEC}s）"})
            return
        runtime = time.time() - start

        if r.returncode != 0:
            self._send_json(500, {
                "success": False,
                "error": f"binary exited {r.returncode}\nstderr (last 500): {r.stderr[-500:]}"
            })
            return
        if not os.path.exists(out_path):
            self._send_json(500, {"success": False, "error": "binary did not produce output file"})
            return

        with open(out_path, "r") as f:
            report_md = f.read()
        try:
            os.remove(out_path)
        except Exception:
            pass

        result = {
            "report_md": report_md,
            "runtime_sec": runtime,
            "repo": repo,
            "pr": pr,
            "mode": mode,
        }
        cache_set(cache_key, result)
        self._send_json(200, {"success": True, "cached": False, **result})


if __name__ == "__main__":
    print(f"[boot] BIN_PATH = {BIN_PATH}")
    print(f"[boot] bin exists: {os.path.exists(BIN_PATH)}")
    print(f"[boot] whitelist: {sorted(WHITELIST_REPOS)}")
    print(f"[boot] rate limit: {RATE_LIMIT_PER_HOUR}/hour/IP")
    print(f"[boot] exec timeout: {EXEC_TIMEOUT_SEC}s")
    print(f"[boot] cache TTL: {CACHE_TTL_SEC}s")
    print(f"[boot] listening on 0.0.0.0:{PORT}")
    httpd = ThreadingHTTPServer(("0.0.0.0", PORT), Handler)
    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print("\n[bye]")
        httpd.server_close()
