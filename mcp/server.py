#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
AI PR Review — MCP server (stdio JSON-RPC, Python stdlib only).

Exposes the C++ reviewer as MCP tools so Claude Code / Cursor / any MCP
client can call it directly. This is the bridge that makes the project
satisfy the JD's "MCP 工具与 Skills 体系" requirement: the same engine
that runs as a CLI now also runs as an agent-callable tool.

Tools:
  - review_pr   : run a full AI review on a GitHub PR URL.
  - list_rules  : list the 8 built-in static rules (no network).
  - list_skills : list the review Skills (personas + meta-strategies, no network).
  - run_eval    : run the evaluation harness on the bundled dataset.

Wire-up (Claude Code / Cursor mcp.json):
  {
    "mcpServers": {
      "ai-pr-review": {
        "command": "python3",
        "args": ["/abs/path/to/mcp/server.py"],
        "env": { "AI_PR_REVIEW_BIN": "/abs/path/to/ai-pr-review",
                 "DEEPSEEK_API_KEY": "sk-xxx" }
      }
    }
  }

Design notes:
  * Uses only json + sys + subprocess + os — no pip dependencies, runs on
    any Python 3.8+.
  * Messages are line-delimited JSON-RPC 2.0 over stdin/stdout (the MCP
    stdio transport). Each request is processed synchronously; the binary is
    invoked via subprocess and its Markdown report is returned as the tool
    result.
"""

import json
import os
import subprocess
import sys

# --- Configuration (env-overridable) -------------------------------------
BIN = os.environ.get(
    "AI_PR_REVIEW_BIN",
    os.path.expanduser("~/demo/ai-pr-review-v0.2.0-linux-x64/ai-pr-review"),
)
EVAL_BIN = os.environ.get(
    "AI_PR_EVAL_BIN",
    os.path.expanduser("~/demo/ai-pr-review-v0.2.0-linux-x64/ai-pr-eval"),
)

RULES = [
    ("unpaired_alloc", "error", "malloc/new 未配对 free/delete，可能内存泄漏"),
    ("virtual_modify", "error", "修改 virtual/override 方法，影响派生类多态"),
    ("raw_ptr_new", "warn", "裸指针 new，建议改用智能指针"),
    ("c_style_cast", "warn", "C 风格强制转换，建议改用 C++ cast"),
    ("memcpy_size", "error", "memcpy/memset 的 sizeof 参数为指针而非数组"),
    ("format_only", "info", "纯格式化变更（空格/缩进）"),
    ("test_file", "info", "变更文件位于 test 目录，风险较低"),
    ("todo_fixme", "warn", "遗留 TODO/FIXME 注释"),
]


def _send(obj):
    sys.stdout.write(json.dumps(obj, ensure_ascii=False) + "\n")
    sys.stdout.flush()


def _tool_result(text):
    return {"content": [{"type": "text", "text": text}], "isError": False}


def _err(text):
    return {"content": [{"type": "text", "text": text}], "isError": True}


def handle_tools_list():
    tools = [
        {
            "name": "review_pr",
            "description": (
                "对指定 GitHub PR 运行 AI 代码审查（规则引擎 + AST + LLM），"
                "返回结构化 Markdown 审查报告。支持可选 agent / rag / persona 模式。"
            ),
            "inputSchema": {
                "type": "object",
                "properties": {
                    "pr_url": {"type": "string",
                               "description": "GitHub PR URL，如 https://github.com/owner/repo/pull/123"},
                    "persona": {"type": "string",
                                "enum": ["default", "linus", "mentor", "devil", "zen"],
                                "default": "default"},
                    "agent": {"type": "boolean", "default": False,
                              "description": "启用 Function-Calling Agent 循环"},
                    "rag": {"type": "boolean", "default": False,
                            "description": "启用 RAG 检索相关规范注入上下文"},
                    "embed": {"type": "boolean", "default": False,
                              "description": "RAG 走语义路径（embedding + 向量余弦检索）而非 BM25"},
                    "index": {"type": "string",
                              "description": "向量索引文件路径（JSON）。配合 embed 使用：存在则加载，否则构建并落盘"},
                    "rag_corpus_dir": {"type": "string",
                                       "description": "额外 RAG 语料目录（.md/.txt），与内置规范合并"},
                    "source_dir": {"type": "string",
                                   "description": "本地源码根目录（启用 AST 分析）"},
                },
                "required": ["pr_url"],
            },
        },
        {
            "name": "list_rules",
            "description": "列出内置的 8 条静态审查规则（无需网络）。",
            "inputSchema": {"type": "object", "properties": {}},
        },
        {
            "name": "list_skills",
            "description": "列出可用的审查 Skills（5 种 persona + self_critique / consensus，无需网络）。",
            "inputSchema": {"type": "object", "properties": {}},
        },
        {
            "name": "run_eval",
            "description": "运行评测框架，对内置数据集输出三维评分（关键词/严重度/无幻觉）。",
            "inputSchema": {
                "type": "object",
                "properties": {
                    "config": {"type": "string",
                               "enum": ["baseline", "linus", "critique", "consensus"],
                               "default": "baseline"}
                },
            },
        },
    ]
    return {"tools": tools}


def handle_tool_call(name, args):
    if name == "list_rules":
        lines = ["# 内置静态审查规则", ""]
        for rid, sev, desc in RULES:
            lines.append(f"- [{sev}] `{rid}`: {desc}")
        return _tool_result("\n".join(lines))

    if name == "list_skills":
        try:
            proc = subprocess.run([BIN, "--list-skills"],
                                  capture_output=True, text=True, timeout=30)
        except FileNotFoundError:
            return _err(f"找不到二进制：{BIN}（请设置环境变量 AI_PR_REVIEW_BIN）")
        return _tool_result(proc.stdout.strip() or proc.stderr.strip())

    if name == "review_pr":
        pr_url = args.get("pr_url")
        if not pr_url:
            return _err("缺少参数 pr_url")
        cmd = [BIN, "--pr", pr_url, "-o", "-"]
        if args.get("persona"):
            cmd += ["--persona", args["persona"]]
        if args.get("agent"):
            cmd.append("--agent")
        if args.get("rag"):
            cmd.append("--rag")
        if args.get("embed"):
            cmd.append("--embed")
        if args.get("index"):
            cmd += ["--index", args["index"]]
        if args.get("rag_corpus_dir"):
            cmd += ["--rag-corpus-dir", args["rag_corpus_dir"]]
        if args.get("source_dir"):
            cmd += ["--source-dir", args["source_dir"]]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180)
        except FileNotFoundError:
            return _err(f"找不到二进制：{BIN}（请设置环境变量 AI_PR_REVIEW_BIN）")
        except subprocess.TimeoutExpired:
            return _err("审查超时（>180s）")
        if proc.returncode != 0 and not proc.stdout.strip():
            return _err(f"审查失败（exit {proc.returncode}）：{proc.stderr[:500]}")
        return _tool_result(proc.stdout.strip() or proc.stderr.strip())

    if name == "run_eval":
        cfg = args.get("config", "baseline")
        cases = os.path.join(os.path.dirname(BIN), "..", "code", "eval", "cases.json")
        cmd = [EVAL_BIN, "--cases", cases, "--out-dir", f"/tmp/aipr-eval-{cfg}"]
        if cfg == "linus":
            cmd += ["--persona", "linus"]
        elif cfg == "critique":
            cmd.append("--self-critique")
        elif cfg == "consensus":
            cmd += ["--consensus", "3"]
        try:
            proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300)
        except FileNotFoundError:
            return _err(f"找不到评测二进制：{EVAL_BIN}（请设置 AI_PR_EVAL_BIN）")
        return _tool_result(proc.stdout.strip() or proc.stderr.strip())

    return _err(f"未知工具：{name}")


def main():
    for raw in sys.stdin:
        raw = raw.strip()
        if not raw:
            continue
        try:
            req = json.loads(raw)
        except json.JSONDecodeError:
            continue

        method = req.get("method")
        req_id = req.get("id")
        params = req.get("params", {})

        if method == "initialize":
            _send({
                "jsonrpc": "2.0", "id": req_id,
                "result": {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "ai-pr-review", "version": "0.3.0"},
                },
            })
        elif method == "notifications/initialized":
            pass
        elif method == "tools/list":
            _send({"jsonrpc": "2.0", "id": req_id, "result": handle_tools_list()})
        elif method == "tools/call":
            name = params.get("name", "")
            args = params.get("arguments", {}) or {}
            result = handle_tool_call(name, args)
            _send({"jsonrpc": "2.0", "id": req_id, "result": result})
        elif method in ("ping",):
            _send({"jsonrpc": "2.0", "id": req_id, "result": {}})
        else:
            _send({"jsonrpc": "2.0", "id": req_id,
                   "error": {"code": -32601, "message": f"unknown method: {method}"}})


if __name__ == "__main__":
    main()
