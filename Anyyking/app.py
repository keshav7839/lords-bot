"""Anyy Bot HF Host — Gradio UI + bot subprocess + self-keepalive."""
import atexit
import json
import os
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

import httpx

import gradio as gr
import spaces

BASE = Path(__file__).resolve().parent
LOG_FILE = BASE / "bot_runtime.log"
TAIL_LINES = 300
_proc = {"p": None}
_lock = threading.Lock()


def _launch_bot():
    with _lock:
        p = _proc["p"]
        if p is not None and p.poll() is None:
            return p.pid
        logf = open(LOG_FILE, "ab", buffering=0)
        p = subprocess.Popen(
            [sys.executable, "-u", "anyybest.py"],
            cwd=str(BASE),
            stdout=logf,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
        )
        _proc["p"] = p
        return p.pid


def _stop_bot():
    with _lock:
        p = _proc["p"]
        if p is None or p.poll() is not None:
            return "not running"
        p.terminate()
        try:
            p.wait(timeout=8)
        except Exception:
            p.kill()
        return "stopped"


def _status():
    with _lock:
        p = _proc["p"]
        if p is None:
            return "not started"
        if p.poll() is None:
            return f"running (pid={p.pid})"
        return f"dead (rc={p.returncode})"


def _log_tail():
    try:
        with open(LOG_FILE, "r", encoding="utf-8", errors="replace") as f:
            return "\n".join(f.read().splitlines()[-TAIL_LINES:])
    except FileNotFoundError:
        return "(no log yet)"


def _keepalive_loop():
    space_host = os.environ.get("SPACE_HOST", "").strip()
    if not space_host:
        return
    url = f"https://{space_host}"
    while True:
        time.sleep(300)
        try:
            httpx.get(url, timeout=10, follow_redirects=True)
        except Exception:
            pass


def _watchdog():
    backoff = 0
    while True:
        time.sleep(15)
        with _lock:
            p = _proc["p"]
        if p is not None and p.poll() is not None:
            backoff = min(backoff + 15, 180)
            time.sleep(backoff)
            pid = _launch_bot()
        elif p is None:
            backoff = min(backoff + 15, 180)
            time.sleep(backoff)
            pid = _launch_bot()


BOT_PID = _launch_bot()
print(f"[app.py] Bot subprocess started pid={BOT_PID}", flush=True)

keepalive = threading.Thread(target=_keepalive_loop, daemon=True)
keepalive.start()
watchdog = threading.Thread(target=_watchdog, daemon=True)
watchdog.start()


def _stop_and_exit():
    _stop_bot()


atexit.register(_stop_and_exit)


class _HealthHandler(BaseHTTPRequestHandler):
    def do_GET(self):
        if self.path.startswith("/health"):
            body = json.dumps({"status": _status(), "log": _log_tail()}, ensure_ascii=False).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        else:
            self.send_response(404)
            self.end_headers()

    def log_message(self, *a):
        pass


def _start_health():
    srv = ThreadingHTTPServer(("0.0.0.0", 8000), _HealthHandler)
    srv.serve_forever()


threading.Thread(target=_start_health, daemon=True).start()
print("[app.py] health server on :8000", flush=True)


def refresh_ui():
    return _status(), _log_tail()


@spaces.GPU(duration=1)
def _zerogpu_dummy():
    return "unused"


with gr.Blocks(title="Anyy Bot Host", css="#logbox{height:420px;overflow-y:auto}") as demo:
    gr.Markdown("## 🤖 Anyy Bot — Hugging Face Host\nTelegram long-polling + wake keepalive.  Bot **auto-starts** on space boot.")
    with gr.Row():
        start_btn = gr.Button("▶ Start bot", variant="primary")
        stop_btn = gr.Button("■ Stop bot")
        status_btn = gr.Button("↻ Refresh")
    status_box = gr.Textbox(label="Bot status", value=_status(), interactive=False)
    log_area = gr.Textbox(label="Runtime log (last 300 lines)", value=_log_tail(), lines=22, interactive=False, elem_id="logbox")

    def _start_click():
        pid = _launch_bot()
        return f"started pid={pid}" if pid else "failed", _log_tail()

    start_btn.click(fn=_start_click, outputs=[status_box, log_area], api_name="start")
    stop_btn.click(fn=lambda: (_stop_bot(), _log_tail()), outputs=[status_box, log_area], api_name="stop")
    status_btn.click(fn=refresh_ui, outputs=[status_box, log_area], api_name="health")
    demo.load(fn=refresh_ui, outputs=[status_box, log_area])
    timer = gr.Timer(5)
    timer.tick(fn=refresh_ui, outputs=[status_box, log_area])

if __name__ == "__main__":
    demo.queue().launch(server_name="0.0.0.0", server_port=7860, show_error=True)
