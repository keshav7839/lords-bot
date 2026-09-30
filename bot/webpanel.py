#!/usr/bin/env python3
"""
lordsM web panel - control the C bot from a browser.

Run `bot` in Termux, open the printed URL, and the bot is driven from the
page: every setting, the account credential, live logs, start/stop.

Why a web panel rather than the APK: this device refuses to install
sideloaded packages (HyperOS blocks it before the installer sees the file),
and a browser needs no install step at all. It also means the UI can be far
better than what a framework-only Android View hierarchy would give.

Python standard library only - no pip install, which matters on a device
with a small and slow package mirror.

Layout note: everything lives in this one file on purpose. Deploying a
single script means there is no import path to get wrong and no static
asset directory to lose.
"""

import base64
import json
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time
import queue
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

HERE = os.path.dirname(os.path.abspath(__file__))
BOT_BIN = os.environ.get("LORDSM_BIN", os.path.join(HERE, "client"))
BOT_SRC = os.environ.get("LORDSM_SRC", os.path.join(HERE, "src"))
def _first_existing(*cands):
    for c in cands:
        if c and os.path.exists(c):
            return c
    return cands[-1]


# The defaults come from whichever shipped config is present. Getting this
# wrong is not cosmetic: with no file the key list collapses to just the
# synthesised strncmp families and the panel shows 9 controls instead of 124,
# every one of the real settings silently missing.
DEFAULT_CFG = _first_existing(
    os.environ.get("LORDSM_DEFAULT_CFG"),
    os.path.join(HERE, "config_default.cfg"),
    os.path.join(HERE, "config_wave.cfg"),
    os.path.join(HERE, "config.cfg"),
    os.path.join(HERE, "..", "config_wave.cfg"),
)
STATE_DIR = os.path.join(HERE, "state")
CFG_PATH = os.path.join(STATE_DIR, "config.cfg")
ACCT_PATH = os.path.join(STATE_DIR, "account.json")
PORT = int(os.environ.get("LORDSM_PORT", "8777"))

# --------------------------------------------------------------------------
# Feature registry, derived from the bot itself rather than hand-copied.
#
# config.c decides what is a legal key (strcmp, or strncmp for families like
# train.target_*), and the shipped config gives defaults. Generating the
# list from those means the panel cannot drift from the engine: add a key to
# the bot and it shows up here; rename one and the old name stops being
# offered.
# --------------------------------------------------------------------------

# Keys that are pinned off in EVERY mode. Each was tried for real against
# this server and closed the session - see docs/DEEP_PROTOCOL.md.
ALWAYS_BLOCKED = {
    "wave.request_role_info",   # 1004 _MSG_LOGIN_REQUESTLOGIN: a re-auth attempt
    "wave.request_build_info",  # 2000 rejected
    "wave.load_equip_inventory",  # 1416 rejected
    "train.dismiss_above",      # 2405 rejected
    "train.instant_finish",     # 2407 FINISHTRAINING, spends gems
    "alliance.request_own_help",  # 2852 rejected
    "wave.heal_troops",         # payload unconfirmed, closes the session
    "protection.shield_always_on",
    "protection.shield_on_incoming_attack",
    "protection.shield_on_incoming_scout",
    "speedup.enabled",
    "wave.stage_sweep",
}

# risk: SAFE      read-only, or proven and observed working
#        CAUTION   proven opcode but state-changing, spends own resources
#        RISKY     never proven, or proven to close the session
RISK = {
    "train.enabled": "CAUTION", "train.rotate": "CAUTION",
    "train.target_infantry": "CAUTION", "train.target_ranged": "CAUTION",
    "train.target_cavalry": "CAUTION", "train.target_siege": "CAUTION",
    "train.max_batch": "CAUTION", "train.interval_s": "CAUTION",
    "train.dismiss_above": "RISKY", "train.dismiss_batch": "RISKY",
    "train.instant_finish": "RISKY",
    "wave.build_upgrade": "CAUTION",
    "wave.research_auto": "CAUTION",
    "wave.reward_mask": "CAUTION",
    "wave.arena_challenge": "CAUTION",
    "wave.arena_offense_hero1": "CAUTION", "wave.arena_offense_hero2": "CAUTION",
    "wave.arena_offense_hero3": "CAUTION", "wave.arena_offense_hero4": "CAUTION",
    "wave.arena_offense_hero5": "CAUTION",
    "wave.gather_auto": "CAUTION", "wave.gather_gem_load": "CAUTION",
    "wave.gather_load": "CAUTION",
    "wave.trap_build": "CAUTION", "wave.trap_repair": "CAUTION",
    "wave.trap_repair_batch": "CAUTION",
    "wave.hunt_auto": "CAUTION", "wave.hunt_min_level": "CAUTION",
    "wave.hunt_max_level": "CAUTION",
    "wave.heal_troops": "RISKY", "wave.heal_style": "RISKY",
    "wave.stage_sweep": "RISKY",
    "protection.enabled": "CAUTION",
    "protection.shield_always_on": "RISKY",
    "protection.shield_on_incoming_attack": "RISKY",
    "protection.shield_on_incoming_scout": "RISKY",
    "protection.shield_priority": "RISKY",
    "speedup.enabled": "RISKY", "speedup.daily_cap": "RISKY",
    "cargo_ship.auto_trade": "RISKY",
    "cargo_ship.spend_food": "RISKY", "cargo_ship.spend_rock": "RISKY",
    "cargo_ship.spend_wood": "RISKY", "cargo_ship.spend_ore": "RISKY",
    "cargo_ship.spend_gold": "RISKY",
    "bank.enabled": "RISKY",
    "bank.send_food": "RISKY", "bank.send_rock": "RISKY",
    "bank.send_wood": "RISKY", "bank.send_ore": "RISKY",
    "bank.send_gold": "RISKY",
    "rally.enabled": "CAUTION",
    "tunnel.enabled": "RISKY",
    "wave.labyrinth_spend": "RISKY",
    "wave.shelter_always": "RISKY",
}

# Human labels for the ones people actually look for. Everything else falls
# back to a title-cased last component.
LABELS = {
    "server.addr": "Gateway address", "server.port": "Gateway port",
    "account.igg_id": "IGG ID", "account.device_uuid": "Device UUID",
    "account.access_key": "Access key", "admin.name": "Bot name",
    "command.prefix": "Command prefix", "command.input": "Read commands from",
    "command.output": "Send replies to", "data.path": "Data path",
    "client.version_major": "Client major", "client.version_minor": "Client minor",
    "client.version_patch": "Client patch", "client.language_code": "Language",
    "wave.build_upgrade": "Auto building upgrades", "wave.gather_gems_first": "Prefer gem lodes",
    "wave.gather_gem_load": "Gem load per march", "wave.gather_fit": "Fit troops to tile",
    "wave.gather_priority": "Tile priority", "wave.online_gift": "Claim free box",
    "wave.tycoon": "Kingdom Tycoon", "wave.action_min_gap_ms": "Min action gap (ms)",
    "wave.load_equip_inventory": "Request gear inventory",
    "wave.request_build_info": "Request building list",
    "train.rotate": "Rotation order", "train.dismiss_above": "Dismiss surplus",
    "train.instant_finish": "Instant finish (gems)",
    "wave.labyrinth": "Labyrinth", "wave.labyrinth_mode": "Labyrinth mode",
    "wave.shelter_always": "Shelter always", "wave.hunt_auto": "Auto hunt",
    "wave.heal_troops": "Heal troops", "wave.stage_sweep": "Stage sweep",
    "protection.shield_always_on": "Keep shield up",
}

CHOICES = {
    "command.input": ["GUILD", "CHAT", "MAIL"],
    "command.output": ["MAIL", "CHAT", "GUILD"],
    "wave.gather_priority": ["amount", "mixed", "distance", "lowest"],
    "wave.labyrinth_mode": ["0", "1", "2", "3"],
    "protection.shield_priority": [
        "SHIELD_4H, SHIELD_8H, SHIELD_12H, SHIELD_1D"],
}

SECRET_KEYS = {"account.access_key"}


def _read(path):
    try:
        with open(path, "r", errors="ignore") as f:
            return f.read()
    except OSError:
        return ""


def build_spec():
    """Keys the engine accepts, with defaults and a type guess."""
    src = _read(os.path.join(BOT_SRC, "config.c"))
    exact = set(re.findall(r'strcmp\(key,\s*"([^"]+)"\)', src))
    prefix = set(re.findall(r'strncmp\(key,\s*"([^"]+)"', src))

    defaults, order = {}, []
    for raw in _read(DEFAULT_CFG).splitlines():
        line = raw.split("#")[0].split(";")[0].strip()
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        k, v = k.strip(), v.strip()
        if not re.fullmatch(r"[A-Za-z0-9_.]+", k):
            continue
        if k not in defaults:
            order.append(k)
        defaults[k] = v

    # Materialise the strncmp families, but only with the suffixes the
    # engine actually looks for. Blindly pairing every prefix with every
    # suffix produced junk like train.target_1, which is not a key the
    # parser recognises - it would render as a dead control in the UI.
    SUFFIXES = {
        "train.target_": ["infantry", "ranged", "cavalry", "siege"],
        "wave.arena_offense_hero": ["1", "2", "3", "4", "5"],
    }
    for pfx in sorted(prefix):
        for base in SUFFIXES.get(pfx, []):
            k = pfx + base
            if k not in defaults:
                defaults[k] = "0"
                order.append(k)

    spec = []
    for k in order:
        v = defaults[k]
        if k in CHOICES:
            kind = "choice"
        elif v in ("true", "false"):
            kind = "bool"
        elif re.fullmatch(r"-?\d+", v):
            kind = "int"
        elif k in ("server.addr", "account.igg_id", "account.device_uuid"):
            kind = "text"
        else:
            kind = "text"
        spec.append({
            "key": k,
            "default": v,
            "type": kind,
            "group": k.split(".")[0] if "." in k else "core",
            "label": LABELS.get(k, k.split(".")[-1].replace("_", " ").capitalize()),
            "risk": RISK.get(k, "SAFE"),
            "blocked": k in ALWAYS_BLOCKED,
            "choices": CHOICES.get(k, []),
            "secret": k in SECRET_KEYS,
        })
    spec.sort(key=lambda s: (s["group"], s["key"]))
    return spec


SPEC = None


def spec():
    global SPEC
    if SPEC is None:
        SPEC = build_spec()
    return SPEC


# --------------------------------------------------------------------------
# State
# --------------------------------------------------------------------------

LOCK = threading.RLock()
PROC = None
LOGS = []                      # ring buffer
SSE_Q = queue.Queue()           # fan-out to browser tabs
VALUES = {}                    # current config values
ACCOUNT = {"label": "Main", "ultra_safe": False}
MAX_LOG = 3000


def clamp(values):
    """Apply the safety rules. Runs on every save, server-side."""
    out = dict(values)
    for s in spec():
        out.setdefault(s["key"], s["default"])
        if s["blocked"] and s["type"] == "bool":
            out[s["key"]] = "false"
        if ACCOUNT.get("ultra_safe") and s["risk"] == "RISKY" and s["type"] == "bool":
            out[s["key"]] = "false"
    for k in ALWAYS_BLOCKED:
        if k in out:
            out[k] = "false"
    return out


def load_state():
    global VALUES, ACCOUNT
    os.makedirs(STATE_DIR, exist_ok=True)
    if os.path.exists(ACCT_PATH):
        try:
            with open(ACCT_PATH) as f:
                ACCOUNT.update(json.load(f))
        except Exception:
            pass
    src = CFG_PATH if os.path.exists(CFG_PATH) else DEFAULT_CFG
    vals = {}
    for raw in _read(src).splitlines():
        line = raw.split("#")[0].split(";")[0].strip()
        if "=" not in line:
            continue
        k, v = line.split("=", 1)
        vals[k.strip()] = v.strip()
    VALUES = clamp(vals)


def save_state():
    os.makedirs(STATE_DIR, exist_ok=True)
    tmp = CFG_PATH + ".tmp"
    with open(tmp, "w") as f:
        f.write("# Written by the lordsM web panel.\n")
        f.write("# Flat key = value: LoadConfig is sscanf-based and has no sections.\n")
        for s in spec():
            v = VALUES.get(s["key"], s["default"])
            if s["secret"]:
                v = v  # written as-is; needed by the engine
            f.write(f"{s['key']} = {v}\n")
    os.replace(tmp, CFG_PATH)
    with open(ACCT_PATH, "w") as f:
        json.dump(ACCOUNT, f)


def emit(line):
    with LOCK:
        LOGS.append(line)
        if len(LOGS) > MAX_LOG:
            del LOGS[: len(LOGS) - MAX_LOG]
    SSE_Q.put(line)


def running():
    return PROC is not None and PROC.poll() is None


def start_bot():
    global PROC
    with LOCK:
        if running():
            return False, "already running"
        if not os.path.exists(BOT_BIN):
            return False, f"bot binary not found at {BOT_BIN}"
        if not VALUES.get("account.access_key"):
            return False, "no credential - import a PCAPdroid capture first"
        save_state()
        emit("[panel] starting engine: " + BOT_BIN + " " + CFG_PATH)
        PROC = subprocess.Popen(
            [BOT_BIN, CFG_PATH],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            bufsize=1, universal_newlines=True, cwd=HERE,
            preexec_fn=os.setsid,
        )

        def pump():
            try:
                for line in PROC.stdout:
                    emit(line.rstrip("\n"))
            except Exception as e:
                emit(f"[panel] log pump ended: {e}")
            emit(f"[panel] engine exited rc={PROC.poll()}")
        threading.Thread(target=pump, daemon=True).start()
        return True, "started"


def stop_bot():
    global PROC
    with LOCK:
        if not running():
            return False, "not running"
        try:
            os.killpg(os.getpgid(PROC.pid), signal.SIGTERM)
        except Exception:
            pass
        emit("[panel] stop requested")
        return True, "stopping"


# --------------------------------------------------------------------------
# PCAP import
# --------------------------------------------------------------------------

TOKEN = re.compile(r"eyJ[A-Za-z0-9_-]{8,}\.[A-Za-z0-9_-]{20,}\.[A-Za-z0-9_-]{20,}")


def import_capture(text):
    """Pull the access key out of a PCAPdroid text export.

    Matches the token itself rather than reproducing byte offsets: PCAPdroid
    renders unprintable bytes inconsistently, which is exactly what made an
    offset-based decoder brittle.
    """
    m = TOKEN.search(text or "")
    if not m:
        return {"ok": False,
                "error": "No access key found. Export the game's login "
                         "exchange from PCAPdroid as Text and paste it here."}
    token = m.group(0)
    header = token.split(".")[0]
    try:
        raw = base64.urlsafe_b64decode(header + "=" * (-len(header) % 4))
        obj = json.loads(raw.decode("utf-8", "replace"))
    except Exception as e:
        return {"ok": False, "error": f"key header unreadable: {e}"}
    if not obj.get("v"):
        return {"ok": False, "error": "no version field; not a Lords key"}
    return {"ok": True, "access_key": token, "akid": obj.get("akid", ""),
            "kmd": obj.get("kmd", ""), "gid": obj.get("g", ""),
            "v": obj.get("v")}


# --------------------------------------------------------------------------
# HTTP
# --------------------------------------------------------------------------

class H(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    server_version = "lordsM"

    def log_message(self, *_a):
        pass

    def _send(self, code, body, ctype="application/json"):
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _json(self, obj, code=200):
        self._send(code, json.dumps(obj))

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(n) if n else b"{}"
        try:
            return json.loads(raw.decode())
        except Exception:
            return {}

    def do_GET(self):
        path = self.path.split("?")[0]
        if path in ("/", "/index.html"):
            return self._send(200, PAGE, "text/html; charset=utf-8")
        if path == "/api/spec":
            return self._json({"spec": spec(),
                               "blocked": sorted(ALWAYS_BLOCKED)})
        if path == "/api/state":
            with LOCK:
                return self._json({
                    "running": running(),
                    "ultra_safe": bool(ACCOUNT.get("ultra_safe")),
                    "values": dict(VALUES),
                    "account": {k: v for k, v in ACCOUNT.items()},
                    "has_credential": bool(VALUES.get("account.access_key")),
                    "enabled": enabled_count(),
                    "total": len(spec()),
                })
        if path == "/api/logs":
            with LOCK:
                return self._json({"lines": list(LOGS)})
        if path == "/api/logs/stream":
            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.send_header("Cache-Control", "no-cache")
            self.send_header("Connection", "keep-alive")
            self.end_headers()
            try:
                with LOCK:
                    backlog = list(LOGS)
                for l in backlog:
                    self.wfile.write(f"data: {json.dumps(l)}\n\n".encode())
                self.wfile.flush()
                while True:
                    try:
                        line = SSE_Q.get(timeout=15)
                        self.wfile.write(f"data: {json.dumps(line)}\n\n".encode())
                        self.wfile.flush()
                    except queue.Empty:
                        self.wfile.write(b": ping\n\n")
                        self.wfile.flush()
            except (BrokenPipeError, ConnectionResetError):
                pass
            return
        self._send(404, "not found", "text/plain")

    def do_POST(self):
        path = self.path.split("?")[0]
        b = self._body()
        if path == "/api/save":
            with LOCK:
                for k, v in (b.get("values") or {}).items():
                    VALUES[k] = str(v)
                VALUES = clamp(VALUES)
                if "ultra_safe" in b:
                    ACCOUNT["ultra_safe"] = bool(b["ultra_safe"])
                VALUES = clamp(VALUES)
                save_state()
                emit("[panel] settings saved")
            return self._json({"ok": True, "values": dict(VALUES)})
        if path == "/api/start":
            ok, msg = start_bot()
            return self._json({"ok": ok, "msg": msg})
        if path == "/api/stop":
            ok, msg = stop_bot()
            return self._json({"ok": ok, "msg": msg})
        if path == "/api/import":
            res = import_capture(b.get("text", ""))
            if res.get("ok"):
                with LOCK:
                    VALUES["account.access_key"] = res["access_key"]
                    VALUES["account.igg_id"] = res["akid"]
                    VALUES["account.device_uuid"] = res["kmd"]
                    if res["gid"]:
                        VALUES["admin.name"] = ACCOUNT.get("label") or "lord"
                    VALUES = clamp(VALUES)
                    save_state()
                res.pop("access_key", None)
                emit("[panel] credential imported from capture")
            return self._json(res)
        if path == "/api/label":
            with LOCK:
                ACCOUNT["label"] = b.get("label", ACCOUNT.get("label", "Main"))
                save_state()
            return self._json({"ok": True})
        if path == "/api/clear_logs":
            with LOCK:
                LOGS.clear()
            return self._json({"ok": True})
        self._send(404, "not found", "text/plain")


def enabled_count():
    v = clamp(VALUES)
    return sum(1 for s in spec()
               if s["type"] == "bool" and not s["blocked"]
               and v.get(s["key"]) == "true" and s["risk"] != "RISKY")


def lan_ip():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except Exception:
        return None
    finally:
        s.close()


def banner():
    ip = lan_ip()
    print("", flush=True)
    print("  \033[1;33m lordsM \033[0m web panel is up")
    print("")
    print("    \033[1;36mhttp://127.0.0.1:%d/\033[0m   <- open this on the phone" % PORT)
    if ip:
        print("    \033[1;36mhttp://%s:%d/\033[0m   <- other device on the same wifi"
              % (ip, PORT))
    print("")
    print("  Ctrl-C to stop.")
    print("")
    sys.stdout.flush()


PAGE = r"""<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>lordsM</title>
<style>
:root{
  --bg:#0f1115; --panel:#171a21; --panel2:#1e222b; --line:#2a2f3a;
  --fg:#e7eaf0; --dim:#98a0b0; --gold:#d9a441; --cyan:#4fb3c4;
  --red:#e2604f; --green:#5bc07a;
}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font:15px/1.45 system-ui,-apple-system,Roboto,sans-serif;
  padding-bottom:env(safe-area-inset-bottom)}
header{position:sticky;top:0;z-index:5;background:var(--panel);
  border-bottom:1px solid var(--line);padding:calc(env(safe-area-inset-top) + 10px) 14px 10px}
h1{margin:0;font-size:17px;color:var(--gold);letter-spacing:.3px}
#sub{font-size:12px;color:var(--dim)}
.pill{display:inline-block;padding:2px 8px;border-radius:99px;font-size:11px;
  border:1px solid var(--line);margin-right:6px}
.pill.run{background:rgba(91,192,122,.15);color:var(--green);border-color:transparent}
.pill.stop{background:rgba(226,96,79,.15);color:var(--red);border-color:transparent}
nav{display:flex;gap:4px;padding:8px 10px;background:var(--panel);
  position:sticky;top:64px;z-index:4;border-bottom:1px solid var(--line)}
nav button{flex:1;padding:9px 4px;border:0;border-radius:9px;background:transparent;
  color:var(--dim);font-size:13px;font-weight:600;cursor:pointer}
nav button.on{background:var(--panel2);color:var(--gold)}
main{padding:12px;max-width:900px;margin:0 auto}
.card{background:var(--panel);border:1px solid var(--line);border-radius:13px;
  padding:13px;margin-bottom:11px}
.card h2{margin:0 0 9px;font-size:13px;color:var(--gold);letter-spacing:.4px;
  text-transform:uppercase}
.row{display:flex;align-items:center;gap:10px;padding:9px 0}
.row+.row{border-top:1px solid var(--line)}
.lbl{flex:1;min-width:0}
.lbl b{display:block;font-weight:600;font-size:14px}
.lbl code{font-size:11px;color:var(--dim);word-break:break-all}
.badge{font-size:9px;padding:1px 5px;border-radius:4px;margin-left:6px;
  vertical-align:1px;font-weight:700}
.SAFE{background:rgba(79,179,196,.16);color:var(--cyan)}
.CAUTION{background:rgba(217,164,65,.16);color:var(--gold)}
.RISKY{background:rgba(226,96,79,.18);color:var(--red)}
button.act{padding:9px 14px;border:0;border-radius:9px;background:var(--gold);
  color:#1a1305;font-weight:700;cursor:pointer}
button.sec{background:var(--panel2);color:var(--fg);border:1px solid var(--line)}
button:disabled{opacity:.4;cursor:not-allowed}
input[type=text],input[type=number],input[type=password],select,textarea{
  background:var(--panel2);border:1px solid var(--line);color:var(--fg);
  padding:8px 9px;border-radius:8px;font:inherit;font-size:13px;max-width:190px}
textarea{width:100%;max-width:none;min-height:110px;font-family:ui-monospace,monospace;
  font-size:11px}
.sw{appearance:none;width:44px;height:25px;border-radius:99px;background:#39404e;
  position:relative;cursor:pointer;flex:0 0 auto;transition:.15s}
.sw:after{content:"";position:absolute;top:3px;left:3px;width:19px;height:19px;
  border-radius:50%;background:#fff;transition:.15s}
.sw:checked{background:var(--green)}
.sw:checked:after{left:22px}
.sw:disabled{opacity:.35}
.note{font-size:11px;color:var(--dim)}
.warn{font-size:11px;color:var(--red)}
#logs{background:#0a0c10;border:1px solid var(--line);border-radius:11px;padding:9px;
  height:56vh;overflow:auto;font:11px/1.5 ui-monospace,monospace;white-space:pre-wrap}
#logs .g{color:#5bc07a}#logs .r{color:#e2604f}#logs .y{color:#d9a441}
#logs .c{color:#4fb3c4}#logs .d{color:#98a0b0}
.tools{display:flex;gap:8px;margin-bottom:9px;flex-wrap:wrap}
.grid2{display:grid;grid-template-columns:1fr 1fr;gap:9px}
.stat{background:var(--panel2);border-radius:10px;padding:11px;text-align:center}
.stat b{display:block;font-size:20px;color:var(--gold)}
.stat span{font-size:11px;color:var(--dim)}
input.search{width:100%;max-width:none;margin-bottom:9px}
.hidden{display:none}
.toast{position:fixed;left:50%;transform:translateX(-50%);bottom:18px;
  background:var(--panel2);border:1px solid var(--line);padding:9px 15px;
  border-radius:9px;font-size:13px;z-index:9;box-shadow:0 6px 22px rgba(0,0,0,.5)}
</style></head><body>

<header>
  <h1>lordsM</h1>
  <div id="sub">loading</div>
</header>

<nav>
  <button data-t="dash" class="on">Dash</button>
  <button data-t="feat">Features</button>
  <button data-t="acc">Account</button>
  <button data-t="log">Logs</button>
</nav>

<main>
  <section id="dash">
    <div class="card">
      <h2>Engine</h2>
      <div style="margin-bottom:11px">
        <span class="pill stop" id="pill">stopped</span>
        <span class="pill" id="pillcfg">config saved</span>
      </div>
      <div class="tools">
        <button class="act" id="bstart">Start</button>
        <button class="sec" id="bstop">Stop</button>
      </div>
      <div class="grid2">
        <div class="stat"><b id="sOn">0</b><span>features on</span></div>
        <div class="stat"><b id="sTot">0</b><span>total controls</span></div>
        <div class="stat"><b id="sMode">Normal</b><span>mode</span></div>
        <div class="stat"><b id="sBlk">0</b><span>pinned off</span></div>
      </div>
    </div>
    <div class="card">
      <h2>Ultra Safe Mode</h2>
      <div class="row">
        <div class="lbl"><b>Pin every risky key off</b>
          <span class="note">Forces all RISKY features off, plus the
          <span id="nblk">0</span> keys proven to close this session.
          Applied server-side on every save, so the browser cannot bypass it.</span>
        </div>
        <input type="checkbox" class="sw" id="usafe">
      </div>
    </div>
  </section>

  <section id="feat" class="hidden">
    <input class="search" id="q" type="text" placeholder="Search settings">
    <select id="grp" style="width:100%;max-width:none;margin-bottom:9px"></select>
    <div id="frows"></div>
  </section>

  <section id="acc" class="hidden">
    <div class="card">
      <h2>Credential</h2>
      <div class="row"><div class="lbl"><b>Label</b></div>
        <input type="text" id="aLabel"></div>
      <div class="row"><div class="lbl"><b>IGG ID</b><code id="kIgg"></code></div>
        <input type="text" id="aIgg"></div>
      <div class="row"><div class="lbl"><b>Device UUID</b></div>
        <input type="text" id="aUuid"></div>
      <div class="row"><div class="lbl"><b>Access key</b>
        <span class="note" id="kState"></span></div>
        <input type="password" id="aKey"></div>
      <div class="row"><div class="lbl"><b>Gateway</b></div>
        <span><input type="text" id="aAddr" style="width:120px">
        <input type="text" id="aPort" style="width:74px"></span></div>
    </div>
    <div class="card">
      <h2>Import PCAPdroid capture</h2>
      <textarea id="cap" placeholder="Paste the PCAPdroid text export here, or drop the .txt file onto this box."></textarea>
      <div class="tools" style="margin-top:9px">
        <button class="act" id="bImp">Import</button>
        <span class="note" id="impMsg"></span>
      </div>
    </div>
  </section>

  <section id="log" class="hidden">
    <div class="tools">
      <button class="sec" id="bClear">Clear</button>
      <button class="sec" id="bAuto" style="color:var(--cyan)">auto-scroll on</button>
    </div>
    <div id="logs"></div>
  </section>
</main>

<div id="toast" class="toast hidden"></div>

<script>
let SPEC=[],VALS={},BLKED=[],running=false,autoScroll=true;

const $=s=>document.querySelector(s), $$=s=>[...document.querySelectorAll(s)];
function toast(m){const t=$('#toast');t.textContent=m;t.classList.remove('hidden');
  clearTimeout(t._h);t._h=setTimeout(()=>t.classList.add('hidden'),2600);}
const api=async(p,b)=>(await fetch(p,{method:b?'POST':'GET',
  headers:{'Content-Type':'application/json'},
  body:b?JSON.stringify(b):undefined})).json();

function cls(line){
  if(/ERROR|Disconnected|Recv error|failed|Failed/.test(line))return 'r';
  if(/WARN|rejected|timeout/.test(line))return 'y';
  if(/^\[panel\]/.test(line))return 'c';
  if(/\[(BUILD|HERO|GEAR|TALENT|TREASURE|SPIRE|SCAN|TRAIN|GATHER)\]/.test(line))return 'g';
  if(/PKT|INFO /i.test(line))return 'd';
  return '';
}
function pushLog(l){
  const box=$('#logs'), d=document.createElement('div');
  d.className=cls(l); d.textContent=l;
  box.appendChild(d);
  while(box.childNodes.length>2500)box.removeChild(box.firstChild);
  if(autoScroll)box.scrollTop=box.scrollHeight;
}

async function refresh(){
  const s=await api('/api/state');
  running=s.running; VALS=s.values;
  $('#pill').textContent=running?'running':'stopped';
  $('#pill').className='pill '+(running?'run':'stop');
  $('#pillcfg').textContent=s.has_credential?'credential set':'no credential';
  $('#sub').innerHTML='<span class="pill '+(running?'run':'stop')+'">'+
    (running?'engine running':'engine stopped')+'</span>'+
    (s.account&&s.account.label?s.account.label:'');
  $('#sOn').textContent=s.enabled; $('#sTot').textContent=s.total;
  $('#sMode').textContent=s.ultra_safe?'Ultra Safe':'Normal';
  $('#sBlk').textContent=BLKED.length; $('#nblk').textContent=BLKED.length;
  $('#usafe').checked=!!s.ultra_safe;
  $('#aLabel').value=(s.account&&s.account.label)||'Main';
  $('#aIgg').value=VALS['account.igg_id']||'';
  $('#aUuid').value=VALS['account.device_uuid']||'';
  $('#aKey').value=VALS['account.access_key']||'';
  $('#aAddr').value=VALS['server.addr']||'';
  $('#aPort').value=VALS['server.port']||'';
  $('#kIgg').textContent=VALS['account.igg_id']||'not set';
  $('#kState').textContent=VALS['account.access_key']
    ? ('set, '+VALS['account.access_key'].length+' chars'):'not set';
  $('#bstart').disabled=running; $('#bstop').disabled=!running;
  renderRows();
}

function blockedFor(s){
  return s.blocked || ($('#usafe').checked && s.risk==='RISKY');
}

function renderRows(){
  const q=$('#q').value.toLowerCase(), g=$('#grp').value;
  const rows=SPEC.filter(s=>(g==='all'||s.group===g) &&
    (!q||s.label.toLowerCase().includes(q)||s.key.toLowerCase().includes(q)));
  const host=$('#frows'); host.innerHTML='';
  if(!rows.length){host.innerHTML='<div class="card note">nothing matches</div>';return;}
  for(const s of rows){
    const blk=blockedFor(s);
    const d=document.createElement('div'); d.className='card';
    d.innerHTML='<div class="row"><div class="lbl"><b>'+esc(s.label)+
      '<span class="badge '+s.risk+'">'+s.risk+'</span></b><code>'+s.key+'</code></div></div>';
    const ctl=document.createElement('div');
    ctl.className='row';
    if(s.type==='bool'){
      const i=document.createElement('input');
      i.type='checkbox';i.className='sw';
      i.checked=(VALS[s.key]??s.default)==='true'; i.disabled=blk;
      i.onchange=()=>save({[s.key]:String(i.checked)});
      ctl.appendChild(i);
    }else if(s.type==='choice'){
      const sel=document.createElement('select');
      for(const c of s.choices){const o=document.createElement('option');
        o.textContent=c;o.value=c;if(c===(VALS[s.key]??s.default))o.selected=true;sel.appendChild(o);}
      sel.disabled=blk; sel.onchange=()=>save({[s.key]:sel.value});
      ctl.appendChild(sel);
    }else{
      const i=document.createElement('input');
      i.type=(s.type==='int')?'number':'text';
      if(s.secret)i.type='password';
      i.value=VALS[s.key]??s.default; i.disabled=blk;
      i.onchange=()=>save({[s.key]:i.value});
      ctl.appendChild(i);
    }
    if(blk){
      const w=document.createElement('span');
      w.className='warn';
      w.textContent=s.blocked?'pinned off: closes the session':'blocked by Ultra Safe';
      ctl.appendChild(w);
    }
    d.appendChild(ctl);
    host.appendChild(d);
  }
}

const esc=s=>String(s).replace(/[&<>]/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;'}[c]));

let saveTimer=null, pending={};
function save(patch){
  Object.assign(pending,patch);
  clearTimeout(saveTimer);
  saveTimer=setTimeout(async()=>{
    const p=pending; pending={};
    Object.assign(VALS,p);
    const r=await api('/api/save',{values:p,ultra_safe:$('#usafe').checked});
    if(r.values){VALS=r.values;}
    refresh();
  },260);
}

document.addEventListener('click',e=>{
  const t=e.target.closest('nav button'); if(!t)return;
  $$('nav button').forEach(b=>b.classList.toggle('on',b===t));
  ['dash','feat','acc','log'].forEach(x=>
    $('#'+x).classList.toggle('hidden',x!==t.dataset.t));
  if(t.dataset.t==='log')$('#logs').scrollTop=$('#logs').scrollHeight;
});
$('#q').oninput=renderRows;
$('#grp').onchange=renderRows;
$('#usafe').onchange=()=>save({});
$('#bstart').onclick=async()=>{const r=await api('/api/start',{});toast(r.msg);refresh();};
$('#bstop').onclick=async()=>{const r=await api('/api/stop',{});toast(r.msg);refresh();};
$('#bClear').onclick=async()=>{await api('/api/clear_logs',{});$('#logs').innerHTML='';};
$('#bAuto').onclick=()=>{autoScroll=!autoScroll;
  $('#bAuto').textContent='auto-scroll '+(autoScroll?'on':'off');};
$('#bImp').onclick=async()=>{
  const r=await api('/api/import',{text:$('#cap').value});
  $('#impMsg').textContent=r.ok?('imported v'+r.v+' akid '+r.akid):r.error;
  if(r.ok)refresh();
};
['aLabel','aIgg','aUuid','aKey','aAddr','aPort'].forEach(id=>{
  $('#'+id).onchange=()=>{
    const m={aLabel:['account.access_key',null],aIgg:['account.igg_id'],
      aUuid:['account.device_uuid'],aKey:['account.access_key'],
      aAddr:['server.addr'],aPort:['server.port']};
    const k=m[id][0]; if(!k)return;
    const p={}; p[k]=$('#'+id).value; save(p);
  };
});
$('#aLabel').onchange=async()=>{await api('/api/label',{label:$('#aLabel').value});refresh();};
document.addEventListener('dragover',e=>e.preventDefault());
document.addEventListener('drop',async e=>{
  e.preventDefault();
  const f=e.dataTransfer.files[0]; if(!f)return;
  if($('#tab-acc')===null && !$('#acc').classList.contains('hidden')){}
  $('#cap').value=await f.text();
  toast('loaded '+f.name+' - press Import');
});

(async function init(){
  const s=await api('/api/spec');
  SPEC=s.spec; BLKED=s.blocked;
  const gs=[...new Set(SPEC.map(x=>x.group))].sort();
  $('#grp').innerHTML='<option value="all">all groups</option>'+
    gs.map(g=>'<option>'+esc(g)+'</option>').join('');
  const lg=await api('/api/logs');
  lg.lines.forEach(pushLog);
  await refresh();
  setInterval(refresh,3000);
  const es=new EventSource('/api/logs/stream');
  es.onmessage=e=>pushLog(JSON.parse(e.data));
})();
</script></body></html>
"""


def main():
    load_state()
    if not os.path.exists(BOT_BIN):
        print(f"  ! bot binary not found: {BOT_BIN}")
        print(f"    build it:  cd {HERE}/src && gcc -O2 -std=gnu11 "
              f"-Iinclude *.c -o ../client")
    srv = ThreadingHTTPServer(("0.0.0.0", PORT), H)
    srv.daemon_threads = True
    t = threading.Thread(target=srv.serve_forever, daemon=True)
    t.start()
    banner()
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        print("\n  stopping engine and panel...")
        stop_bot()
        time.sleep(1)
        if running():
            try:
                os.killpg(os.getpgid(PROC.pid), signal.SIGKILL)
            except Exception:
                pass
        srv.shutdown()


if __name__ == "__main__":
    main()
