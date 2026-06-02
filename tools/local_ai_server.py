#!/usr/bin/env python3
"""Small OpenAI-compatible DeepSeek proxy with optional web search.

Run from the repository root:

    python tools/local_ai_server.py

Environment variables:
    DEEPSEEK_API_KEY       DeepSeek API key. If unset, this script tries to
                           read DEEPSEEK_API_KEY from main/api_config_private.h.
    LOCAL_AI_HOST          Bind host. Defaults to 0.0.0.0.
    LOCAL_AI_PORT          Bind port. Defaults to 8080.
    LOCAL_AI_SEARCH        auto, always, or off. Defaults to auto.
    LOCAL_AI_MAX_RESULTS   Maximum search results to pass to DeepSeek. Default 3.
"""

from __future__ import annotations

import html
import json
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[1]
PRIVATE_CONFIG = REPO_ROOT / "main" / "api_config_private.h"

DEEPSEEK_URL = os.getenv("DEEPSEEK_URL", "https://api.deepseek.com/chat/completions")
DEEPSEEK_MODEL = os.getenv("DEEPSEEK_MODEL", "deepseek-v4-pro")
HOST = os.getenv("LOCAL_AI_HOST", "0.0.0.0")
PORT = int(os.getenv("LOCAL_AI_PORT", "8080"))
SEARCH_MODE = os.getenv("LOCAL_AI_SEARCH", "auto").strip().lower()
MAX_RESULTS = int(os.getenv("LOCAL_AI_MAX_RESULTS", "3"))
HTTP_TIMEOUT = float(os.getenv("LOCAL_AI_TIMEOUT", "30"))

SEARCH_TRIGGERS = (
    "search",
    "web",
    "internet",
    "latest",
    "today",
    "now",
    "news",
    "weather",
    "price",
    "stock",
    "查",
    "搜索",
    "网上",
    "联网",
    "最新",
    "今天",
    "现在",
    "新闻",
    "天气",
    "价格",
    "股价",
)


def load_api_key() -> str:
    key = os.getenv("DEEPSEEK_API_KEY", "").strip()
    if key:
        return key

    if PRIVATE_CONFIG.exists():
        text = PRIVATE_CONFIG.read_text(encoding="utf-8", errors="ignore")
        match = re.search(r'#define\s+DEEPSEEK_API_KEY\s+"([^"]+)"', text)
        if match:
            key = match.group(1).strip()
            if key and key != "PLEASE_SET_YOUR_API_KEY":
                return key

    return ""


def local_ip_hint() -> str:
    try:
        with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
            sock.connect(("8.8.8.8", 80))
            return sock.getsockname()[0]
    except OSError:
        return "127.0.0.1"


def strip_tags(value: str) -> str:
    value = re.sub(r"<[^>]+>", " ", value)
    value = html.unescape(value)
    return re.sub(r"\s+", " ", value).strip()


def ddg_redirect_to_url(href: str) -> str:
    href = html.unescape(href)
    parsed = urllib.parse.urlparse(href)
    query = urllib.parse.parse_qs(parsed.query)
    if "uddg" in query and query["uddg"]:
        return query["uddg"][0]
    return href


def search_web(query: str, max_results: int) -> list[dict[str, str]]:
    form = urllib.parse.urlencode({"q": query}).encode("utf-8")
    request = urllib.request.Request(
        "https://lite.duckduckgo.com/lite/",
        data=form,
        method="POST",
        headers={
            "User-Agent": "Mozilla/5.0 (compatible; local-ai-server/1.0)",
            "Accept": "text/html",
            "Content-Type": "application/x-www-form-urlencoded",
        },
    )

    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
        body = response.read(256 * 1024).decode("utf-8", errors="ignore")

    results: list[dict[str, str]] = []
    link_matches = list(
        re.finditer(
            r"<a[^>]+href=['\"]([^'\"]+)['\"][^>]+class=['\"]result-link['\"][^>]*>(.*?)</a>",
            body,
            flags=re.IGNORECASE | re.DOTALL,
        )
    )

    for index, link_match in enumerate(link_matches):
        start = link_match.end()
        end = link_matches[index + 1].start() if index + 1 < len(link_matches) else len(body)
        block = body[start:end]
        link_match = re.search(
            r"<a[^>]+href=['\"]([^'\"]+)['\"][^>]+class=['\"]result-link['\"][^>]*>(.*?)</a>",
            link_match.group(0),
            flags=re.IGNORECASE | re.DOTALL,
        )
        if not link_match:
            continue

        snippet_match = re.search(
            r"<td[^>]+class=['\"]result-snippet['\"][^>]*>(.*?)</td>",
            block,
            flags=re.IGNORECASE | re.DOTALL,
        )
        snippet_html = snippet_match.group(1) if snippet_match else ""

        title = strip_tags(link_match.group(2))
        link = ddg_redirect_to_url(link_match.group(1))
        snippet = strip_tags(snippet_html)
        if title and link:
            results.append({"title": title, "url": link, "snippet": snippet})
        if len(results) >= max_results:
            break

    return results


def extract_last_user_message(messages: list[dict[str, Any]]) -> str:
    for message in reversed(messages):
        if message.get("role") == "user":
            content = message.get("content", "")
            return content if isinstance(content, str) else json.dumps(content, ensure_ascii=False)
    return ""


def should_search(user_text: str) -> bool:
    if SEARCH_MODE == "off":
        return False
    if SEARCH_MODE == "always":
        return True
    lowered = user_text.lower()
    return any(trigger in lowered for trigger in SEARCH_TRIGGERS)


def add_search_context(payload: dict[str, Any], results: list[dict[str, str]]) -> None:
    if not results:
        return

    lines = []
    for index, result in enumerate(results, start=1):
        lines.append(
            f"[{index}] {result['title']}\n"
            f"URL: {result['url']}\n"
            f"Snippet: {result.get('snippet', '')}"
        )

    context = (
        "Use these recent web search results when they are relevant. "
        "Answer in the user's language. Keep the answer short for a small robot display.\n\n"
        + "\n\n".join(lines)
    )
    messages = payload.setdefault("messages", [])
    messages.insert(0, {"role": "system", "content": context})


def call_deepseek(payload: dict[str, Any], api_key: str) -> bytes:
    payload = dict(payload)
    payload["model"] = payload.get("model") or DEEPSEEK_MODEL
    payload["stream"] = False

    body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    request = urllib.request.Request(
        DEEPSEEK_URL,
        data=body,
        method="POST",
        headers={
            "Authorization": f"Bearer {api_key}",
            "Content-Type": "application/json",
            "Accept": "application/json",
        },
    )
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
        return response.read()


class Handler(BaseHTTPRequestHandler):
    server_version = "LocalAIServer/1.0"

    def log_message(self, fmt: str, *args: Any) -> None:
        sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))

    def send_json(self, status: int, value: dict[str, Any]) -> None:
        body = json.dumps(value, ensure_ascii=False).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path == "/health":
            self.send_json(200, {"ok": True, "search": SEARCH_MODE, "model": DEEPSEEK_MODEL})
            return
        self.send_json(404, {"error": {"message": "not found"}})

    def do_POST(self) -> None:
        if urllib.parse.urlparse(self.path).path != "/chat/completions":
            self.send_json(404, {"error": {"message": "not found"}})
            return

        api_key = load_api_key()
        if not api_key:
            self.send_json(500, {"error": {"message": "DEEPSEEK_API_KEY is not configured"}})
            return

        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            messages = payload.get("messages", [])
            user_text = extract_last_user_message(messages if isinstance(messages, list) else [])

            results: list[dict[str, str]] = []
            if user_text and should_search(user_text):
                try:
                    results = search_web(user_text, MAX_RESULTS)
                    add_search_context(payload, results)
                    print(f"search: {user_text!r}, results={len(results)}", flush=True)
                except Exception as exc:  # noqa: BLE001
                    print(f"search failed: {exc}", flush=True)

            upstream_body = call_deepseek(payload, api_key)
            self.send_response(200)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(upstream_body)))
            self.end_headers()
            self.wfile.write(upstream_body)
        except urllib.error.HTTPError as exc:
            message = exc.read().decode("utf-8", errors="replace")
            self.send_json(exc.code, {"error": {"message": message}})
        except Exception as exc:  # noqa: BLE001
            self.send_json(500, {"error": {"message": str(exc)}})


def main() -> int:
    if not load_api_key():
        print("DEEPSEEK_API_KEY is not configured.", file=sys.stderr)
        print(f"Set the env var or add it to {PRIVATE_CONFIG}.", file=sys.stderr)
        return 1

    server = ThreadingHTTPServer((HOST, PORT), Handler)
    ip = local_ip_hint()
    print(f"Local AI server listening on http://{HOST}:{PORT}")
    print(f"ESP32 URL: http://{ip}:{PORT}/chat/completions")
    print(f"Search mode: {SEARCH_MODE}, max results: {MAX_RESULTS}")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nStopping server.")
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
