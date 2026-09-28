"""Lords Mobile Bot Command Center + Anyy Bot Host — port 7860, password gated."""
import base64
import json
import os
import re
import signal
import socket
import ssl
import struct
import subprocess
import sys
import threading
import time
from collections import deque
from pathlib import Path
from urllib.parse import parse_qs

import gradio as gr
import uvicorn
from fastapi import FastAPI, Request
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, RedirectResponse

PORT = int(os.environ.get("PORT") or "7860")
ROOT = Path(__file__).resolve().parent
PASSWORD = os.environ.get("DASH_PASSWORD") or "keshav"

# ============================================================ Anyy telegram bot
ANYY_DIR = ROOT / "Anyyking"
ANYY_LOG = deque(maxlen=4000)
ANYY_STATE = {"started": time.time(), "pid": None, "exit": None, "restarts": 0}


def _pump(stream):
    for line in stream:
        ANYY_LOG.append(line.rstrip("\n"))


def _run_anyy():
    while True:
        try:
            proc = subprocess.Popen(
                [sys.executable, "-u", str(ROOT / "dbg_boot.py")],
                cwd=str(ANYY_DIR),
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                bufsize=1,
                env=dict(os.environ),
            )
            ANYY_STATE["pid"] = proc.pid
            ANYY_STATE["exit"] = None
            ANYY_LOG.append(f"[host] anyy bot started pid={proc.pid}")
            _pump(proc.stdout)
            code = proc.wait()
            ANYY_STATE["exit"] = code
            ANYY_LOG.append(f"[host] anyy bot exited code={code}")
        except Exception as exc:
            ANYY_STATE["exit"] = f"{type(exc).__name__}: {exc}"
            ANYY_LOG.append(f"[host] spawn failed: {type(exc).__name__}: {exc}")
        ANYY_STATE["restarts"] += 1
        time.sleep(5)


def anyy_body():
    lines = [
        "Anyy Bot Host (@meanyybot)",
        f"uptime_s: {int(time.time() - ANYY_STATE['started'])}",
        f"pid: {ANYY_STATE['pid']}  exit: {ANYY_STATE['exit']}  restarts: {ANYY_STATE['restarts']}",
        "----- last log -----",
    ]
    lines.extend(list(ANYY_LOG)[-250:])
    return ("\n".join(lines) + "\n").encode("utf-8", "replace")


def probe_body():
    import ssl as _ssl
    import urllib.request as _ur

    tok = os.environ.get("BOT_TOKEN") or ""
    proxy = (os.environ.get("TELEGRAM_API_URL") or "").strip().rstrip("/")
    tests = [
        ("hf", "https://huggingface.co"),
        ("direct_getMe", f"https://api.telegram.org/bot{tok}/getMe"),
        ("proxy_getMe", f"{proxy}/telegram-api/bot{tok}/getMe" if proxy else ""),
        ("proxy2_getMe", "https://tgproxy-pages.pages.dev/telegram-api/bot" + tok),
        ("pages_getMe", f"https://tgproxy-pages.pages.dev/bot{tok}/getMe"),
        ("example", "https://example.com"),
        ("lmworker", "https://lmproxy2.anyyverify.workers.dev/"),
        ("lmworker_tcp", "https://lmproxy2.anyyverify.workers.dev/proxy?h=192.243.44.63&p=5999&k=lm7f3k9x"),
    ]
    out = []
    ctx = _ssl.create_default_context()
    for name, url in tests:
        if not url:
            continue
        t0 = time.time()
        try:
            req = _ur.Request(url, method="GET")
            with _ur.urlopen(req, timeout=12, context=ctx) as r:
                body = r.read(200)
            out.append(f"{name}: http={r.status} t={time.time()-t0:.2f}s len={len(body)}")
        except Exception as e:
            out.append(f"{name}: ERR {type(e).__name__}: {e} t={time.time()-t0:.2f}s")
    return ("\n".join(out) + "\n").encode()


# ============================================================ Lords Mobile bot
BOT_DIR = str(ROOT / "bot")
LOG = os.path.join(BOT_DIR, "bot.log")
CFG = os.path.join(BOT_DIR, "config.cfg")
CMD = os.path.join(BOT_DIR, "data", "webcmd.txt")

PROC = None
LOCK = threading.Lock()


def is_running():
    return PROC is not None and PROC.poll() is None


HOST_LOG = deque(maxlen=300)

AUTO = {"on": os.environ.get("LM_WATCHDOG", "0") == "1", "delay": 30}


def start_bot():
    global PROC
    with LOCK:
        print(f"[dbg] start_bot: running={is_running()} proc={PROC}", flush=True)
        HOST_LOG.append(f"{time.strftime('%H:%M:%S')} start_bot running={is_running()} proc={PROC}")
        if is_running():
            return True
        os.makedirs(os.path.dirname(CMD), exist_ok=True)
        fout = open(LOG, "a")
        try:
            PROC = subprocess.Popen(
                ["stdbuf", "-oL", "./client", "config.cfg"],
                cwd=BOT_DIR,
                stdout=fout,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            print(f"[dbg] start_bot: spawned pid={PROC.pid}", flush=True)
            HOST_LOG.append(f"{time.strftime('%H:%M:%S')} spawned pid={PROC.pid}")
        except Exception:
            import traceback as _tb
            _tb.print_exc()
            HOST_LOG.append(f"{time.strftime('%H:%M:%S')} spawn FAILED (see stdout traceback)")
            PROC = None
            fout.close()
            return False
        finally:
            if not fout.closed:
                fout.close()
        time.sleep(0.3)
        rc = PROC.poll()
        print(f"[dbg] start_bot: after 0.3s poll={rc}", flush=True)
        HOST_LOG.append(f"{time.strftime('%H:%M:%S')} poll rc={rc}")
        return rc is None


def stop_bot():
    global PROC
    AUTO["on"] = False
    with LOCK:
        if PROC is not None and PROC.poll() is None:
            try:
                os.killpg(os.getpgid(PROC.pid), signal.SIGTERM)
            except Exception:
                try:
                    PROC.terminate()
                except Exception:
                    pass
            for _ in range(20):
                if PROC.poll() is not None:
                    break
                time.sleep(0.1)
            if PROC.poll() is None:
                try:
                    os.killpg(os.getpgid(PROC.pid), signal.SIGKILL)
                except Exception:
                    pass
        PROC = None


def read_cfg():
    vals = {}
    try:
        with open(CFG) as f:
            for line in f:
                if "=" in line and not line.strip().startswith("#"):
                    k, _, v = line.partition("=")
                    vals[k.strip()] = v.strip()
    except Exception:
        pass
    return vals


def write_cfg_key(key, value):
    with open(CFG) as f:
        lines = f.readlines()
    out, done = [], False
    for line in lines:
        if line.partition("=")[0].strip() == key:
            out.append(f"{key} = {value}\n")
            done = True
        else:
            out.append(line)
    if not done:
        out.append(f"{key} = {value}\n")
    with open(CFG, "w") as f:
        f.writelines(out)


def tail_log(max_bytes=16000, lines=140):
    try:
        size = os.path.getsize(LOG)
        with open(LOG, "rb") as f:
            if size > 2_000_000:
                f.seek(size - 200_000)
                f.readline()
            else:
                f.seek(max(0, size - max_bytes))
            text = f.read().decode("utf-8", "replace")
    except Exception:
        return ""
    return "\n".join(text.splitlines()[-lines:])


def log_tail_list(n=20):
    text = tail_log(lines=n)
    return text.splitlines() if text else []


def parse_stats(text):
    def last(pat, default="—"):
        m = None
        for m in re.finditer(pat, text):
            pass
        return m.group(1) if m else default

    def num(v):
        try:
            return f"{int(v.replace(',', '')):,}"
        except Exception:
            return v

    return {
        "Character": last(r"Character:\s*(\S+)"),
        "VIP": last(r"VIP:\s*(\S+)"),
        "Might": num(last(r"Might:\s*(\S+)")),
        "Kills": num(last(r"Kills:\s*(\S+)")),
        "Gems": num(last(r"Gems:\s*(\S+)")),
        "Location": last(r"Location:\s*(\S+)"),
        "login_ok": "Game login successful" in text,
        "error": last(r"\[ERROR\]\s*(.+)") if "[ERROR]" in text else None,
    }


def card(label, value, color):
    return (
        f'<div style="background:{color};border-radius:14px;padding:14px 16px;'
        f'margin:6px;flex:1;min-width:140px;box-shadow:0 2px 10px rgba(0,0,0,.3)">'
        f'<div style="font-size:11px;opacity:.85;text-transform:uppercase;letter-spacing:1.2px">{label}</div>'
        f'<div style="font-size:21px;font-weight:700;margin-top:4px;color:#fff">{value}</div></div>'
    )


def refresh():
    text = tail_log()
    s = parse_stats(text)
    cfg = read_cfg()
    running = is_running()
    if running and s["login_ok"]:
        st_color, st_txt = "#16a34a", "ONLINE IN GAME"
    elif running:
        st_color, st_txt = "#d97706", "STARTING / CONNECTING"
    else:
        st_color, st_txt = "#dc2626", "OFFLINE"
    anyy_alive = ANYY_STATE.get("pid") and ANYY_STATE.get("exit") is None

    def yn(k):
        return "ON" if cfg.get(k) == "true" else "off"

    stats = (
        '<div style="display:flex;flex-wrap:wrap;gap:4px">'
        + card("Lords Bot", st_txt, st_color)
        + card("Character", s["Character"], "#4f46e5")
        + card("Might", s["Might"], "#7c3aed")
        + card("Gems", s["Gems"], "#0891b2")
        + card("Kills", s["Kills"], "#be123c")
        + card("VIP", s["VIP"], "#b45309")
        + card("Location", s["Location"], "#334155")
        + card("Anyy TG Bot", "RUNNING" if anyy_alive else "RESTARTING", "#0f766e")
        + "</div>"
        + '<div style="display:flex;flex-wrap:wrap;gap:4px;margin-top:6px">'
        + card("Shield 24/7", yn("protection.shield_always_on"), "#065f46" if yn("protection.shield_always_on") == "ON" else "#374151")
        + card("Auto Help", yn("alliance.auto_help"), "#1e3a8a" if yn("alliance.auto_help") == "ON" else "#374151")
        + card("Cargo Auto", yn("cargo_ship.auto_trade"), "#713f12" if yn("cargo_ship.auto_trade") == "ON" else "#374151")
        + card("Auto Train", yn("train.enabled"), "#14532d" if yn("train.enabled") == "ON" else "#374151")
        + card("Speedups β", yn("speedup.enabled"), "#4c1d95" if yn("speedup.enabled") == "ON" else "#374151")
        + "</div>"
        + (
            f'<div style="margin-top:8px;background:#7f1d1d;color:#fecaca;padding:10px 14px;border-radius:10px;font-family:monospace">⚠ LAST ERROR: {s["error"]}</div>'
            if s["error"]
            else ""
        )
    )
    status = f'<span style="color:{st_color};font-weight:700">● LORDS: {st_txt}</span>'
    return text or "(log empty — press Start)", stats, status


def act_start():
    AUTO["on"] = True
    ok = start_bot()
    gr.Info("Lords bot starting…" if ok else "Failed to start — check log")
    time.sleep(1)
    return refresh()


def act_stop():
    stop_bot()
    gr.Info("Lords bot stopped — you can play on your phone now")
    return refresh()


def act_restart():
    stop_bot()
    time.sleep(1)
    AUTO["on"] = True
    ok = start_bot()
    gr.Info("Lords bot restarting…" if ok else "Restart failed")
    time.sleep(1)
    return refresh()


def send_command(cmd):
    cmd = (cmd or "").strip()
    if not cmd:
        gr.Warning("Type a command first (e.g. $bank bal)")
        return refresh()
    if not cmd.startswith("$"):
        cmd = "$" + cmd
    os.makedirs(os.path.dirname(CMD), exist_ok=True)
    with open(CMD, "w") as f:
        f.write(cmd + "\n")
    gr.Info(f"Queued: {cmd}")
    time.sleep(1)
    return refresh()


def quick_cmd(cmd):
    os.makedirs(os.path.dirname(CMD), exist_ok=True)
    with open(CMD, "w") as f:
        f.write(cmd + "\n")
    gr.Info(f"Queued: {cmd}")
    time.sleep(1)
    return refresh()


def transfer_cmd(resource, amount):
    resource = (resource or "").strip().lower()
    amount = (amount or "").strip()
    if not amount:
        gr.Warning("Enter an amount, e.g. 5M")
        return refresh()
    cmd = f"${resource} {amount}"
    os.makedirs(os.path.dirname(CMD), exist_ok=True)
    with open(CMD, "w") as f:
        f.write(cmd + "\n")
    gr.Info(f"Queued: {cmd}")
    time.sleep(1)
    return refresh()


# ============================================================ CF TCP tunnel bridge
# The C client connects to 127.0.0.1:16000, sends "CONNECT <ip> <port>\n",
# and this bridge carries the bytes over a WebSocket to a Cloudflare Worker,
# which dials the real game server from Cloudflare's network.
CF_HOST = "lmproxy2.anyyverify.workers.dev"
CF_KEY = "lm7f3k9x"
TUNNEL_PORT = 16000
TUNNEL_LOG = deque(maxlen=300)


def _tlog(msg):
    line = f"[tunnel {time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    TUNNEL_LOG.append(line)


def _ws_handshake(host, path, timeout=15, attempts=3):
    last = None
    for attempt in range(attempts):
        try:
            ctx = ssl.create_default_context()
            infos = socket.getaddrinfo(host, 443, socket.AF_INET, socket.SOCK_STREAM)
            raw = socket.socket(infos[0][0], socket.SOCK_STREAM)
            raw.settimeout(timeout)
            raw.connect(infos[0][4])
            s = ctx.wrap_socket(raw, server_hostname=host)
            key = base64.b64encode(os.urandom(16)).decode()
            req = (
                f"GET {path} HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
                f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
                f"Sec-WebSocket-Version: 13\r\n\r\n"
            )
            s.sendall(req.encode())
            buf = b""
            while b"\r\n\r\n" not in buf:
                chunk = s.recv(4096)
                if not chunk:
                    raise ConnectionError("ws handshake: peer closed")
                buf += chunk
                if len(buf) > 65536:
                    raise ConnectionError("ws handshake: oversized")
            head, rest = buf.split(b"\r\n\r\n", 1)
            status = head.split(b"\r\n")[0]
            if b" 101" not in status:
                raise ConnectionError(f"ws handshake refused: {status!r}")
            s.settimeout(None)
            return s, rest
        except Exception as exc:
            last = exc
            time.sleep(1.5)
    raise ConnectionError(f"ws handshake failed after {attempts} tries: {last}")


def _ws_send(sock, op, payload):
    ln = len(payload)
    hdr = bytearray([0x80 | op])
    mask = os.urandom(4)
    if ln < 126:
        hdr.append(0x80 | ln)
    elif ln < 65536:
        hdr.append(0x80 | 126)
        hdr += struct.pack("!H", ln)
    else:
        hdr.append(0x80 | 127)
        hdr += struct.pack("!Q", ln)
    hdr += mask
    sock.sendall(bytes(hdr) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))


class _FrameBuf:
    def __init__(self, initial=b""):
        self.buf = initial

    def _exact(self, n, sock):
        while len(self.buf) < n:
            chunk = sock.recv(65536)
            if not chunk:
                raise ConnectionError("ws eof")
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def recv(self, sock):
        hdr = self._exact(2, sock)
        fin = hdr[0] & 0x80
        op = hdr[0] & 0x0F
        masked = hdr[1] & 0x80
        ln = hdr[1] & 0x7F
        if ln == 126:
            ln = struct.unpack("!H", self._exact(2, sock))[0]
        elif ln == 127:
            ln = struct.unpack("!Q", self._exact(8, sock))[0]
        mask = self._exact(4, sock) if masked else None
        payload = self._exact(ln, sock) if ln else b""
        if mask:
            payload = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        return fin, op, payload


def _bridge_handle(conn):
    peer = "?"
    ws = None
    try:
        try:
            conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        except Exception:
            pass
        data = b""
        while b"\n" not in data:
            chunk = conn.recv(4096)
            if not chunk:
                return
            data += chunk
            if b"\n" not in data and len(data) > 4096:
                raise ValueError(f"bad first bytes {data[:120]!r}")
        line, extra = data.split(b"\n", 1)
        parts = line.split()
        if len(parts) != 3 or parts[0] != b"CONNECT":
            raise ValueError(f"bad handshake {line[:60]!r}")
        host = parts[1].decode("ascii", "replace")
        port = int(parts[2])
        peer = f"{host}:{port}"
        t0 = time.time()
        ws, initial = _ws_handshake(CF_HOST, f"/proxy?h={host}&p={port}&k={CF_KEY}")
        fb = _FrameBuf(initial)
        _tlog(f"open {peer} via {CF_HOST} ({time.time() - t0:.2f}s)")
        if extra:
            _ws_send(ws, 0x2, extra)

        stop = threading.Event()

        def tcp2ws():
            try:
                while True:
                    chunk = conn.recv(65536)
                    if not chunk:
                        break
                    _ws_send(ws, 0x2, chunk)
            except Exception:
                pass
            finally:
                stop.set()

        threading.Thread(target=tcp2ws, daemon=True).start()

        frag = bytearray()
        frag_op = None
        while True:
            fin, op, payload = fb.recv(ws)
            if op in (0x1, 0x2) and not fin:
                frag = bytearray(payload)
                frag_op = op
                continue
            if op == 0x0:
                frag += payload
                if not fin:
                    continue
                payload = bytes(frag)
                op = frag_op or 0x2
                frag = bytearray()
                frag_op = None
            if op == 0x9:
                _ws_send(ws, 0xA, payload)
                continue
            if op == 0x8:
                code = struct.unpack("!H", payload[:2])[0] if len(payload) >= 2 else -1
                _tlog(f"close {peer} ws-code={code}")
                break
            if op == 0x1:
                if payload == b"READY":
                    continue
                _tlog(f"text {peer}: {payload[:80]!r}")
                continue
            if op == 0x2:
                conn.sendall(payload)
    except Exception as exc:
        _tlog(f"error {peer}: {type(exc).__name__}: {exc}")
    finally:
        try:
            conn.close()
        except Exception:
            pass
        if ws is not None:
            try:
                ws.close()
            except Exception:
                pass


def _bridge_listener():
    try:
        ls = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        ls.bind(("127.0.0.1", TUNNEL_PORT))
        ls.listen(32)
        _tlog(f"bridge listening on 127.0.0.1:{TUNNEL_PORT} -> {CF_HOST}")
        while True:
            c, _addr = ls.accept()
            threading.Thread(target=_bridge_handle, args=(c,), daemon=True).start()
    except Exception:
        import traceback

        traceback.print_exc()


# ============================================================ Feature board
# (key, label, description, badge, category)
# badge: "live" works today | "beta" new/experimental | pending = separate list
FEATURES = [
    ("protection.enabled", "Protection master", "Gates all shield + recall reactions", "live", "🛡️ Protection"),
    ("protection.shield_always_on", "Shield 24/7", "Renews shield automatically when <5 min left", "live", "🛡️ Protection"),
    ("protection.shield_on_incoming_attack", "Shield on attack", "Raise shield when an army targets you", "live", "🛡️ Protection"),
    ("protection.shield_on_incoming_scout", "Shield on scout", "Raise shield when a scout approaches", "live", "🛡️ Protection"),
    ("protection.recall_on_incoming_attack", "Recall on attack", "Recall marches when attacked", "live", "🛡️ Protection"),
    ("protection.recall_on_incoming_scout", "Recall on scout", "Recall marches when scouted", "live", "🛡️ Protection"),
    ("protection.recall_on_incoming_conflict", "Recall on conflict", "Recall gathering/camp marches before conflict", "live", "🛡️ Protection"),
    ("alliance.auto_help", "Auto guild help", "Send helps to alliance automatically", "live", "🤝 Alliance"),
    ("alliance.auto_open_gifts", "Auto guild gifts", "Open guild gift boxes automatically", "live", "🤝 Alliance"),
    ("rally.enabled", "Darknest rally join", "Join Darknest rallies (partial — join logic still stubbed)", "beta", "🤝 Alliance"),
    ("cargo_ship.auto_trade", "Cargo auto-trade", "Trade on the Black Market automatically", "live", "🚢 Cargo Ship"),
    ("cargo_ship.spend_food", "Cargo: spend food", "Allow trading away food", "live", "🚢 Cargo Ship"),
    ("cargo_ship.spend_rock", "Cargo: spend stone", "Allow trading away stone", "live", "🚢 Cargo Ship"),
    ("cargo_ship.spend_wood", "Cargo: spend wood", "Allow trading away wood", "live", "🚢 Cargo Ship"),
    ("cargo_ship.spend_ore", "Cargo: spend ore", "Allow trading away ore", "live", "🚢 Cargo Ship"),
    ("cargo_ship.spend_gold", "Cargo: spend gold", "Allow trading away gold", "live", "🚢 Cargo Ship"),
    ("cargo_ship.use_bag_rss", "Cargo: use bag items", "Open resource items from bag for trades", "live", "🚢 Cargo Ship"),
    ("bank.enabled", "Bank system", "Master switch for the resource bank", "live", "🏦 Banking"),
    ("bank.send_food", "Bank: allow food", "Fulfil $food requests", "live", "🏦 Banking"),
    ("bank.send_rock", "Bank: allow stone", "Fulfil $stone requests", "live", "🏦 Banking"),
    ("bank.send_wood", "Bank: allow wood", "Fulfil $wood requests", "live", "🏦 Banking"),
    ("bank.send_ore", "Bank: allow ore", "Fulfil $ore requests", "live", "🏦 Banking"),
    ("bank.send_gold", "Bank: allow gold", "Fulfil $gold requests", "live", "🏦 Banking"),
    ("bank.use_bag_rss", "Bank: use bag items", "Open bag resource items for delivery", "live", "🏦 Banking"),
    ("bank.use_bag_food", "Bank bag: food", "Use food items from bag", "live", "🏦 Banking"),
    ("bank.use_bag_rock", "Bank bag: stone", "Use stone items from bag", "live", "🏦 Banking"),
    ("bank.use_bag_wood", "Bank bag: wood", "Use wood items from bag", "live", "🏦 Banking"),
    ("bank.use_bag_ore", "Bank bag: ore", "Use ore items from bag", "live", "🏦 Banking"),
    ("bank.use_bag_gold", "Bank bag: gold", "Use gold items from bag", "live", "🏦 Banking"),
    ("train.enabled", "Auto troop training", "Trains troops on an interval (config below)", "live", "⚔️ Automation"),
    ("speedup.enabled", "Auto speed-ups β", "Uses 30-min speedups on active build/research — daily cap", "beta", "⚔️ Automation"),
    # ---- Wave B ----
    ("wave.build_upgrade", "Auto building upgrade β", "Upgrades buildings (priority list) whenever the build queue is idle", "beta", "🏗️ Construction"),
    ("wave.research_auto", "Auto research ⚠", "Starts research when idle — 3202 ACKs (3203) but the start does NOT take effect yet; keep OFF until the payload is cracked", "unverified", "🔬 Research"),
    ("wave.shelter_on_attack", "Shelter on attack", "Hides troops when an army targets you — payload validated live (RESP 5603)", "live", "🛡️ Protection"),
    ("wave.shelter_on_scout", "Shelter on scout", "Hides troops when a scout approaches — payload validated live (RESP 5603)", "live", "🛡️ Protection"),
    ("wave.quest_claim", "Quest auto-claim", "Requests turf missions + claims daily mission rewards (RESP 11873 validated); classic quest finish/complete payloads still unresolved", "live", "📜 Quests & Rewards"),
    ("wave.vip_collect", "VIP chest collect", "Hourly VIP chest collection — payload validated live (RESP 3125)", "live", "📜 Quests & Rewards"),
    ("wave.arena_challenge", "Colosseum auto-challenge", "Fetches board then challenges target 0 every 10 min — validated live (RESP 5209, u8 index payload)", "live", "🏟️ Arena"),
    ("wave.reward_claim", "Reward auto-claim", "Claims daily/quest/event prizes hourly (mask in Config) — treasure daily gift validated live (RESP 3136)", "live", "📜 Quests & Rewards"),
    # ---- Wave C ----
    ("wave.gather_auto", "Auto map scanning β", "Requests map data around home zone (gather learning mode)", "beta", "🗺️ World Map"),
    ("wave.hunt_auto", "Monster hunt scan β", "Includes monster scan targets in map requests (learning mode)", "beta", "🗺️ World Map"),
    # ---- Wave D ----
    ("wave.trap_build", "Auto trap build β", "Wall trap automation (trap info parser learning mode)", "beta", "🐾 Pets & Traps"),
    ("wave.pet_train", "Auto pet training β", "Pet training automation (pet list parser learning mode)", "beta", "🐾 Pets & Traps"),
    # ---- debug ----
    ("wave.log_packets", "Packet logging (debug)", "Log every received packet type — payload discovery for new features", "live", "🔬 Research"),
]

PENDING = [
    ("Research start payload", "Wave B — 3202 answered (3203 garbage struct) but start never takes effect (next-login tech=0) — shape unresolved"),
    ("Quest finish/complete", "Wave B — 3117/3119 silent for every payload tried; MISSIONINFO/FLAG/DAILY structures partially decoded"),
    ("Gather & monster marches", "Wave C — TROOPMARCH / SENDMONSTER send once map tiles are parsed"),
    ("Labyrinth & Kingdom Tycoon", "Wave D — event opcodes located (PUZZLE_*) — payload capture still needed"),
    ("Guild Fest / Guild Showdown", "Wave D — no stable opcodes in packet map — deeper protocol study needed"),
    ("Treasure Trove deposit", "Wave D — gem deposit automation — payload capture needed"),
    ("Trap build & repair", "Wave D — TRAPCONSTRUCT wires up once trap info layout is parsed"),
    ("Hero stages / Expedition combat", "Wave D — prize claiming wired — stage gameplay not implemented"),
]

FEATURE_LABELS = {k: lbl for k, lbl, *_ in FEATURES}
CATEGORIES = []
for _f in FEATURES:
    if _f[4] not in CATEGORIES:
        CATEGORIES.append(_f[4])


# ============================================================ Dashboard UI
with gr.Blocks(title="Lords Mobile Bot — Command Center", theme=gr.themes.Soft(primary_hue="purple")) as demo:
    gr.HTML(
        """
        <style>
          .lm-hero{text-align:center;padding:14px 10px 4px;
            background:linear-gradient(135deg,#1e1b4b 0%,#312e81 50%,#4c1d95 100%);
            border-radius:16px;margin-bottom:10px}
          .lm-hero h1{margin:0;font-size:26px;color:#fbbf24;letter-spacing:1px;
            text-shadow:0 2px 8px rgba(0,0,0,.6)}
          .lm-hero p{margin:6px 0 0;color:#c7d2fe;font-size:13px}
          .lm-pend{background:#18181b;border:1px solid #3f3f46;border-radius:14px;
            padding:14px 16px}
          .lm-pend h3{margin:0 0 10px;color:#a1a1aa;font-size:13px;
            text-transform:uppercase;letter-spacing:1.5px}
          .lm-card{display:flex;align-items:center;justify-content:space-between;
            gap:10px;background:#27272a;border:1px solid #3f3f46;border-radius:12px;
            padding:10px 14px;margin:6px 0}
          .lm-card b{color:#e4e4e7;font-size:14px}
          .lm-card span{color:#71717a;font-size:12px;display:block;margin-top:2px}
          .lm-sw{width:44px;height:24px;border-radius:12px;background:#52525b;
            position:relative;flex:none;opacity:.7}
          .lm-sw:after{content:"";position:absolute;top:3px;left:3px;width:18px;
            height:18px;border-radius:50%;background:#a1a1aa}
          .lm-badge{font-size:10px;padding:2px 8px;border-radius:99px;
            background:#3730a3;color:#c7d2fe;margin-left:8px}
        </style>
        <div class="lm-hero">
          <h1>🏰 LORDS MOBILE — BOT COMMAND CENTER</h1>
          <p>packet bot • cloud hosted • live control · Anyy TG bot running alongside</p>
        </div>
        """
    )
    status_html = gr.HTML('<span style="color:#dc2626;font-weight:700">● CHECKING…</span>')
    stats_html = gr.HTML("")

    with gr.Tabs():
        # ---------------- Overview ----------------
        with gr.Tab("🏠 Overview", id="overview"):
            with gr.Row():
                btn_start = gr.Button("▶ START LORDS BOT", variant="primary")
                btn_stop = gr.Button("■ STOP (play on phone)", variant="stop")
                btn_restart = gr.Button("⟳ RESTART (apply config)", variant="secondary")
            gr.HTML(
                """
                <div style="background:#1e1b4b;color:#c7d2fe;border-radius:10px;padding:10px 14px;margin:6px 0;font-size:13px">
                ⚠ RULE: while the Lords bot is ONLINE, close the game on your phone — one session per account.
                Opening the game on your phone kicks the bot offline automatically; press START to bring it back.
                </div>
                """
            )
            btn_conn = gr.Button("🌐 TEST CONNECTIVITY (HF → game / telegram)")
            conn_out = gr.HTML("")
            log_box = gr.Textbox(
                label="📜 Live Lords bot log (auto-refresh 2s)",
                lines=16,
                max_lines=24,
                show_copy_button=True,
                interactive=False,
            )

        # ---------------- Control ----------------
        with gr.Tab("🎮 Control", id="control"):
            gr.Markdown("### 💰 Resource transfer builder")
            with gr.Row():
                res_pick = gr.Radio(
                    choices=["food", "stone", "wood", "ore", "gold"],
                    value="food",
                    label="Resource",
                )
                amt_box = gr.Textbox(label="Amount (k / m / b)", value="1M")
                btn_send_res = gr.Button("📤 SEND TRANSFER", variant="primary")
            gr.Markdown("### ⚡ Quick commands")
            with gr.Row():
                btn_bank = gr.Button("$bank bal")
                btn_help = gr.Button("$help (mail usage)")
            with gr.Row():
                su_name = gr.Textbox(label="Grant admin (in-game name)", placeholder="PlayerName")
                btn_su = gr.Button("👑 MAKE ADMIN")
            with gr.Row():
                cmd_box = gr.Textbox(label="Any raw command", placeholder="food 5M   /   bank bal   /   waves   /   upgrade")
                cmd_send = gr.Button("📤 SEND", variant="primary")
            gr.Markdown("### 🔬 Raw packet probe (payload discovery)")
            with gr.Row():
                probe_op = gr.Textbox(label="Opcode", placeholder="3202 or 0x0C82")
                probe_hex = gr.Textbox(label="Payload hex (optional)", placeholder="06 00")
                probe_send = gr.Button("🔬 SEND PROBE", variant="primary")
            gr.HTML(
                """
                <div style="font-family:monospace;font-size:12px;background:#0f172a;color:#fcd34d;padding:10px 14px;border-radius:10px;line-height:1.8">
                Sends <b>size+opcode+seq+payload</b> to the game server. Success answers <b>opcode+1</b> —
                turn on <b>Packet logging (debug)</b> in All Features and watch Logs.
                Example: <code>$probe 3135</code> (treasure daily gift, empty payload).<br>
                In-game: <b>$probe &lt;opcode&gt; &lt;hex&gt;</b> · <b>$waves</b> status ·
                <b>$upgrade</b> / <b>$research</b> / <b>$claim</b> manual triggers (admin only).
                </div>
                """
            )
            gr.HTML(
                """
                <div style="font-family:monospace;font-size:13px;background:#0f172a;color:#a5f3fc;padding:12px 16px;border-radius:10px;line-height:1.9">
                <b>$food / $stone / $wood / $ore / $gold &lt;amount&gt;</b> — resource transfer (k/m/b suffixes)<br>
                <b>$bank bal</b> — mail you bank + bag balances · <b>$su &lt;name&gt;</b> — grant admin<br>
                Replies arrive as <b>in-game MAIL</b> to admin.name (spyxlight).
                </div>
                """
            )

        # ---------------- Feature board ----------------
        with gr.Tab("🎮 All Features", id="features"):
            feat_status = gr.Markdown("")
            feature_boxes = []
            for cat in CATEGORIES:
                with gr.Accordion(cat, open=True):
                    row_items = []
                    for (key, label, desc, badge, category) in FEATURES:
                        if category != cat:
                            continue
                        cb = gr.Checkbox(
                            label=f"{label}  [{badge}]",
                            info=f"{key} — {desc}",
                            value=False,
                        )
                        row_items.append((key, cb))
                    feature_boxes.extend(row_items)

            pend_html = ['<div class="lm-pend"><h3>⏳ Coming next — packet captures unlock these</h3>']
            for name, desc in PENDING:
                pend_html.append(
                    f'<div class="lm-card" title="{desc}">'
                    f'<div><b>{name}</b><span>{desc}</span></div>'
                    f'<div class="lm-sw"></div></div>'
                )
            pend_html.append("</div>")
            gr.HTML("".join(pend_html))
            gr.Markdown(
                "Live switches write `config.cfg` **immediately** — press "
                "**⟳ Restart** on Overview to apply while the bot is running."
            )

        # ---------------- Config ----------------
        with gr.Tab("⚙️ Config", id="config"):
            cfg_fields = [
                ("admin.name", "Admin in-game name (mail replies)"),
                ("command.prefix", "Chat command prefix"),
                ("command.input", "Command channel IN (WORLD/GUILD/MAIL)"),
                ("command.output", "Command channel OUT (WORLD/GUILD/MAIL)"),
                ("protection.shield_priority", "Shield priority order"),
                ("train.kind", "Train: kind (0 inf / 1 ranged / 2 cav / 3 siege)"),
                ("train.tier", "Train: tier (0=T1 … 4=T5)"),
                ("train.amount", "Train: amount per request"),
                ("train.interval_s", "Train: interval seconds (min 30)"),
                ("speedup.daily_cap", "Speedups: daily cap"),
                ("cargo_ship.reserve_food", "Cargo reserve: food"),
                ("cargo_ship.reserve_rock", "Cargo reserve: stone"),
                ("cargo_ship.reserve_wood", "Cargo reserve: wood"),
                ("cargo_ship.reserve_ore", "Cargo reserve: ore"),
                ("cargo_ship.reserve_gold", "Cargo reserve: gold"),
                ("bank.reserve_food", "Bank reserve: food"),
                ("bank.reserve_rock", "Bank reserve: stone"),
                ("bank.reserve_wood", "Bank reserve: wood"),
                ("bank.reserve_ore", "Bank reserve: ore"),
                ("bank.reserve_gold", "Bank reserve: gold"),
                ("bank.max_delivery_distance", "Bank: max delivery distance (tiles)"),
                ("wave.build_priority", "Wave: build upgrade priority (build ids, comma list)"),
                ("wave.build_max_level", "Wave: max building level (1-25)"),
                ("wave.research_priority", "Wave: research priority (tech ids, comma list)"),
                ("wave.hunt_max_level", "Wave: max monster level to hunt"),
                ("wave.scan_interval_s", "Wave: map scan interval seconds (min 60)"),
                ("wave.reward_mask", "Wave: reward claim mask (1=daily gift 2=daily 4=achievement 8=expedition 16=battlepass 32=stage 64=month)"),
            ]
            cfg_boxes = []
            for key, label in cfg_fields:
                cfg_boxes.append(gr.Textbox(label=label, info=key))
            save_status = gr.Markdown("")
            btn_save = gr.Button("💾 SAVE CONFIG", variant="primary")
            gr.HTML(
                '<div style="background:#111827;border:1px solid #374151;border-radius:10px;'
                'padding:10px 14px;color:#9ca3af;font-size:12px;font-family:monospace">'
                "account.igg_id / device_uuid / access_key / server.* are hidden here on purpose "
                "(credentials — edit the file directly on the Space if needed).</div>"
            )

        # ---------------- Logs ----------------
        with gr.Tab("📜 Logs", id="logs"):
            with gr.Tabs():
                with gr.Tab("Lords bot"):
                    lords_log2 = gr.Textbox(
                        label="bot.log", lines=24, show_copy_button=True, interactive=False
                    )
                with gr.Tab("Anyy Telegram bot"):
                    anyy_box = gr.Textbox(
                        label="anyy log", lines=24, show_copy_button=True, interactive=False
                    )

        # ---------------- Help ----------------
        with gr.Tab("📖 Help", id="help"):
            gr.HTML(
                """
                <div style="display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:10px">
                <div style="background:#052e16;border:1px solid #22c55e;border-radius:12px;padding:14px">
                  <b style="color:#4ade80">✅ LIVE TODAY</b>
                  <ul style="margin:8px 0 0;padding-left:18px;color:#bbf7d0;line-height:1.7">
                    <li>Auto Shield (always / on attack / on scout)</li>
                    <li>Troop recall (attack / scout / conflict)</li>
                    <li>Guild auto-help + auto gifts</li>
                    <li>Cargo ship auto-trade</li>
                    <li>Resource bank ($food…$gold, $bank bal, $su)</li>
                    <li>Auto troop training · auto speed-ups β</li>
                    <li>Web start/stop/restart + config + feature board</li>
                    <li>Live logs, stats, mail replies</li>
                  </ul>
                </div>
                <div style="background:#1e1b4b;border:1px solid #818cf8;border-radius:12px;padding:14px">
                  <b style="color:#a5b4fc">🚧 NEXT WAVES</b>
                  <ul style="margin:8px 0 0;padding-left:18px;color:#c7d2fe;line-height:1.7">
                    <li>Wave B: build, research, shelter, quests, arena</li>
                    <li>Wave C: map gathering, monster hunting</li>
                    <li>Wave D: labyrinth, guild fest, pets, treasure</li>
                  </ul>
                  <p style="color:#818cf8;font-size:12px;margin-top:8px">
                  Each wave needs one PCAPdroid recording of you doing the actions.</p>
                </div>
                <div style="background:#27272a;border:1px solid #52525b;border-radius:12px;padding:14px">
                  <b style="color:#a1a1aa">📜 COMMANDS</b>
                  <ul style="margin:8px 0 0;padding-left:18px;color:#a1a1aa;line-height:1.7;font-family:monospace">
                    <li>$food 5M · $stone 1M · $wood 2M</li>
                    <li>$ore 500k · $gold 100k</li>
                    <li>$bank bal · $su PlayerName</li>
                  </ul>
                </div>
                <div style="background:#451a03;border:1px solid #f59e0b;border-radius:12px;padding:14px">
                  <b style="color:#fbbf24">⚠ RULES</b>
                  <ul style="margin:8px 0 0;padding-left:18px;color:#fde68a;line-height:1.7">
                    <li>Close the phone game while bot is online</li>
                    <li>New automations start OFF — enable consciously</li>
                    <li>One toggle flip = one config write; Restart to apply</li>
                  </ul>
                </div>
                </div>
                """
            )

    # ---------------- timers & wiring ----------------
    timer = gr.Timer(2.0)
    timer.tick(fn=refresh, outputs=[log_box, stats_html, status_html], show_progress="hidden")

    def refresh_all():
        a = refresh()
        b = tail_log(lines=200) or "(log empty)"
        c = anyy_body().decode("utf-8", "replace")
        return a[0], a[1], a[2], b, c

    timer.tick(fn=refresh_all, outputs=[log_box, stats_html, status_html, lords_log2, anyy_box], show_progress="hidden")

    btn_start.click(fn=act_start, outputs=[log_box, stats_html, status_html])
    btn_stop.click(fn=act_stop, outputs=[log_box, stats_html, status_html])
    btn_restart.click(fn=act_restart, outputs=[log_box, stats_html, status_html])
    cmd_send.click(fn=send_command, inputs=[cmd_box], outputs=[log_box, stats_html, status_html])
    cmd_box.submit(fn=send_command, inputs=[cmd_box], outputs=[log_box, stats_html, status_html])
    btn_send_res.click(fn=transfer_cmd, inputs=[res_pick, amt_box], outputs=[log_box, stats_html, status_html])
    btn_bank.click(fn=lambda: quick_cmd("$bank bal"), outputs=[log_box, stats_html, status_html])
    btn_help.click(fn=lambda: quick_cmd("$help"), outputs=[log_box, stats_html, status_html])
    btn_su.click(fn=lambda n: quick_cmd(f"$su {n}".strip()), inputs=[su_name], outputs=[log_box, stats_html, status_html])
    probe_send.click(
        fn=lambda o, h: quick_cmd(f"$probe {o} {h}".strip()),
        inputs=[probe_op, probe_hex],
        outputs=[log_box, stats_html, status_html],
    )

    btn_conn.click(fn=lambda: {"value": connectivity_html()}, outputs=[conn_out])

    # Feature board: immediate write on user interaction (.input = user only)
    for key, cb in feature_boxes:
        cb.input(
            fn=lambda v, k=key: flip_feature(k, v),
            inputs=[cb],
            outputs=[feat_status],
        )

    # Config tab
    cfg_inputs = cfg_boxes

    def load_cfg_state():
        v = read_cfg()
        return [v.get(k, "") for k, _ in cfg_fields]

    def save_cfg(*vals):
        n = 0
        for (key, _), val in zip(cfg_fields, vals):
            write_cfg_key(key, str(val).strip())
            n += 1
        gr.Info("Config saved" + (" — restart bot to apply" if is_running() else ""))
        return f"✅ Saved {n} settings at {time.strftime('%H:%M:%S')}"

    btn_save.click(fn=save_cfg, inputs=cfg_inputs, outputs=[save_status])

    def load_features():
        v = read_cfg()
        return [v.get(k, "false") == "true" for k, _cb in feature_boxes]

    demo.load(fn=load_features, outputs=[cb for _k, cb in feature_boxes], show_progress="hidden")
    demo.load(fn=load_cfg_state, outputs=cfg_inputs, show_progress="hidden")
    demo.load(
        fn=refresh_all,
        outputs=[log_box, stats_html, status_html, lords_log2, anyy_box],
        show_progress="hidden",
    )


def flip_feature(key, value):
    write_cfg_key(key, "true" if value else "false")
    label = FEATURE_LABELS.get(key, key)
    state = "✅ ENABLED" if value else "❌ disabled"
    extra = " — ⟳ Restart to apply (bot running)" if is_running() else ""
    return f"{state}: **{label}** (`{key}`){extra} · {time.strftime('%H:%M:%S')}"


def tcp_probe(host, port, timeout=5.0):
    t0 = time.time()
    try:
        infos = socket.getaddrinfo(host, port, proto=socket.IPPROTO_TCP)
        addr = infos[0][4]
        s = socket.socket(infos[0][0], socket.SOCK_STREAM)
        s.settimeout(timeout)
        s.connect(addr)
        s.close()
        return {"ok": True, "ms": int((time.time() - t0) * 1000)}
    except Exception as e:
        return {"ok": False, "ms": int((time.time() - t0) * 1000), "err": f"{type(e).__name__}: {e}"}


def connectivity_data():
    cfg = read_cfg()
    game_host = cfg.get("server.addr", "192.243.44.63")
    game_port = int(cfg.get("server.port", "5999") or 5999)
    return {
        "game": {"host": game_host, "port": game_port, **tcp_probe(game_host, game_port)},
        "telegram": {"host": "api.telegram.org", "port": 443, **tcp_probe("api.telegram.org", 443)},
        "tgproxy": {"host": "tgproxy-pages.pages.dev", "port": 443, **tcp_probe("tgproxy-pages.pages.dev", 443)},
        "huggingface": {"host": "huggingface.co", "port": 443, **tcp_probe("huggingface.co", 443)},
    }


def connectivity_html():
    data = connectivity_data()
    rows = []
    for key, d in data.items():
        ok = d.get("ok")
        color = "#16a34a" if ok else "#dc2626"
        mark = "✅" if ok else "❌"
        info = f"{d['ms']} ms" if ok else d.get("err", "blocked")
        rows.append(
            f'<div class="lm-card"><div><b>{mark} {key}</b>'
            f'<span>{d["host"]}:{d["port"]} — {info}</span></div>'
            f'<div style="color:{color};font-weight:700">{"OK" if ok else "BLOCKED"}</div></div>'
        )
    verdict = (
        '<p style="color:#4ade80;font-weight:700">All outbound links OK — HF is not blocking the bot.</p>'
        if all(d.get("ok") for d in data.values())
        else '<p style="color:#fbbf24;font-weight:700">Some links blocked — the bot may lose features until HF allows them.</p>'
    )
    return '<div class="lm-pend"><h3>🌐 Outbound connectivity from the HF container</h3>' + "".join(rows) + verdict + "</div>"


# ============================================================ FastAPI app + auth
LOGIN_FORM = """<!doctype html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Bot Host — Login</title><style>
body{margin:0;height:100vh;display:flex;align-items:center;justify-content:center;
background:linear-gradient(135deg,#1e1b4b,#4c1d95);font-family:system-ui,sans-serif}
.card{background:#111827;border:1px solid #374151;border-radius:18px;padding:36px 32px;width:320px;text-align:center;box-shadow:0 18px 50px rgba(0,0,0,.5)}
h1{color:#fbbf24;font-size:24px;margin:0 0 6px}p{color:#9ca3af;font-size:13px;margin:0 0 18px}
input{width:100%;box-sizing:border-box;padding:13px 14px;border-radius:10px;border:1px solid #4b5563;background:#1f2937;color:#f9fafb;font-size:15px;margin-bottom:14px}
button{width:100%;padding:13px;border:0;border-radius:10px;background:#7c3aed;color:#fff;font-size:15px;font-weight:700;cursor:pointer}
button:hover{background:#6d28d9}.err{color:#f87171;font-size:13px;margin-bottom:10px}
</style></head><body><form class="card" method="post" action="/login">
<h1>🏰 Bot Host</h1><p>Lords Mobile + Anyy Telegram bot</p>
{{ERR}}<input type="password" name="password" placeholder="Password" autofocus>
<button type="submit">Enter Dashboard</button></form></body></html>"""

PUBLIC = {"/login", "/probe", "/head", "/stack", "/healthz"}

app = FastAPI(title="Bot Host")


@app.middleware("http")
async def auth_gate(request: Request, call_next):
    path = request.url.path
    if path in PUBLIC or request.cookies.get("lmk") == "yes":
        return await call_next(request)
    if path == "/favicon.ico":
        return await call_next(request)
    if path == "/" or path == "":
        return RedirectResponse("/login", status_code=302)
    return PlainTextResponse("login required", status_code=401)


@app.get("/login", response_class=HTMLResponse)
async def login_get():
    return HTMLResponse(LOGIN_FORM.replace("{{ERR}}", ""))


@app.post("/login")
async def login_post(request: Request):
    body = parse_qs((await request.body()).decode("utf-8", "replace"))
    if body.get("password", [""])[0] == PASSWORD:
        resp = RedirectResponse("/", status_code=302)
        resp.set_cookie("lmk", "yes", max_age=60 * 60 * 24 * 30, samesite="lax")
        return resp
    return HTMLResponse(LOGIN_FORM.replace("{{ERR}}", '<div class="err">Wrong password</div>'), status_code=401)


@app.get("/healthz")
async def healthz():
    return PlainTextResponse(anyy_body().decode("utf-8", "replace"))


@app.get("/status")
async def status_endpoint():
    payload = {
        "lords": {
            "running": is_running(),
            "pid": PROC.pid if (PROC and PROC.poll() is None) else None,
        },
        "anyy": {
            "pid": ANYY_STATE.get("pid"),
            "exit": ANYY_STATE.get("exit"),
            "restarts": ANYY_STATE.get("restarts"),
            "uptime_s": int(time.time() - ANYY_STATE["started"]),
        },
        "connectivity": connectivity_data(),
        "tunnel": {
            "enabled": read_cfg().get("tunnel.enabled"),
            "worker": CF_HOST,
            "listen": f"127.0.0.1:{TUNNEL_PORT}",
            "log": list(TUNNEL_LOG)[-12:],
        },
        "log_tail": log_tail_list(20),
        "host": list(HOST_LOG)[-40:],
        "stats": parse_stats(tail_log(lines=200)),
        "time": time.strftime("%Y-%m-%d %H:%M:%S"),
    }
    return JSONResponse(payload)


@app.get("/selftest")
async def selftest():
    out = {}
    cl = os.path.join(BOT_DIR, "client")
    out["client_exists"] = os.path.exists(cl)
    try:
        st = os.stat(cl)
        out["client_stat"] = {"size": st.st_size, "mtime": time.ctime(st.st_mtime)}
    except Exception as e:
        out["client_stat"] = str(e)
    import shutil as _sh
    try:
        du = _sh.disk_usage("/")
        out["disk_mb"] = {"total": du.total // 2**20, "used": du.used // 2**20, "free": du.free // 2**20}
    except Exception as e:
        out["disk"] = str(e)
    try:
        st = os.stat(LOG)
        out["log_stat"] = {"size": st.st_size, "mtime": time.ctime(st.st_mtime)}
        with open(LOG, "rb") as f:
            raw = f.read()
        out["log_full"] = raw.decode("utf-8", "replace")[:4000]
        with open(LOG, "a") as f:
            f.write(f"\n[selftest] probe {time.strftime('%H:%M:%S')}\n")
        out["append_ok"] = True
    except Exception as e:
        out["log_err"] = str(e)
    for label, args in (("version", ["--version"]), ("noargs", [])):
        try:
            r = subprocess.run(["stdbuf", "-oL", "./client"] + args, cwd=BOT_DIR, capture_output=True, timeout=10)
            out[f"run_{label}"] = {
                "rc": r.returncode,
                "stdout": r.stdout.decode("utf-8", "replace")[:300],
                "stderr": r.stderr.decode("utf-8", "replace")[:300],
            }
        except Exception as e:
            out[f"run_{label}"] = str(e)
    try:
        txt = open(CFG).read()
        out["cfg"] = {
            "len": len(txt),
            "access_key_count": txt.count("account.access_key"),
            "igg_line": next((l for l in txt.splitlines() if l.startswith("account.igg_id")), None),
            "tunnel_lines": [l for l in txt.splitlines() if l.startswith("tunnel.")],
        }
    except Exception as e:
        out["cfg"] = str(e)
    procs = []
    for p in Path("/proc").iterdir():
        if p.name.isdigit():
            try:
                c = (p / "cmdline").read_bytes().decode("replace").replace("\x00", " ").strip()
                if c:
                    procs.append(f"{p.name} {c[:110]}")
            except Exception:
                pass
    out["procs"] = procs
    out["time"] = time.strftime("%H:%M:%S")
    return JSONResponse(out)


@app.get("/probe")
async def probe():
    return PlainTextResponse(probe_body().decode("utf-8", "replace"))


@app.get("/head")
async def head():
    return PlainTextResponse("\n".join(list(ANYY_LOG)[:150]) + "\n")


@app.get("/stack")
async def stack():
    if ANYY_STATE["pid"]:
        try:
            ANYY_LOG.append("[host] SIGUSR1 -> child stack dump")
            os.kill(ANYY_STATE["pid"], signal.SIGUSR1)
            time.sleep(1.0)
        except Exception as exc:
            ANYY_LOG.append(f"[host] signal failed: {exc}")
    return PlainTextResponse(anyy_body().decode("utf-8", "replace"))


gr.mount_gradio_app(app, demo, path="/")


def _auto_start_lords():
    print("[host] lords watchdog armed", flush=True)
    HOST_LOG.append(f"{time.strftime('%H:%M:%S')} watchdog armed auto={AUTO['on']} watchdog_env={os.environ.get('LM_WATCHDOG')!r}")
    time.sleep(6)
    first = True
    while True:
        try:
            if AUTO["on"] and not is_running():
                if not first:
                    time.sleep(AUTO["delay"])
                first = False
                if AUTO["on"] and not is_running():
                    want_tunnel = os.environ.get("LM_TUNNEL", "0") == "1"
                    write_cfg_key("tunnel.enabled", "true" if want_tunnel else "false")
                    for env_k, cfg_k in (
                        ("LM_IGG_ID", "account.igg_id"),
                        ("LM_DEVICE_UUID", "account.device_uuid"),
                        ("LM_ACCESS_KEY", "account.access_key"),
                    ):
                        v = os.environ.get(env_k)
                        if v:
                            write_cfg_key(cfg_k, v)
                    print(f"[tunnel] config tunnel.enabled -> {want_tunnel}", flush=True)
                    ok = start_bot()
                    print(f"[host] watchdog start result={ok}", flush=True)
                    HOST_LOG.append(f"{time.strftime('%H:%M:%S')} watchdog start result={ok}")
            time.sleep(5)
        except Exception:
            import traceback as _tb
            _tb.print_exc()
            HOST_LOG.append(f"{time.strftime('%H:%M:%S')} watchdog loop error (stdout traceback)")
            time.sleep(5)


threading.Thread(target=_bridge_listener, daemon=True).start()
if os.environ.get("ANYY", "1") == "1":
    threading.Thread(target=_run_anyy, daemon=True).start()
else:
    ANYY_LOG.append("[host] anyy bot disabled by ANYY=0")
threading.Thread(target=_auto_start_lords, daemon=True).start()

if __name__ == "__main__":
    print(f"[host] bot host on :{PORT} (dashboard password set)", flush=True)
    uvicorn.run(app, host="0.0.0.0", port=PORT, log_level="warning")
