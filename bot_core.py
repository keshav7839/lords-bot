import os, sys, threading, logging, time, urllib.request, ssl

os.chdir(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, 'src')

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)

from dotenv import load_dotenv
load_dotenv()

HF_SPACE_URL = os.environ.get('SPACE_URL', 'https://hohjhjj-k.hf.space')
SPACE_NAME = os.environ.get('SPACE_NAME', 'main')

# Persist bot data under the Space's data dir so projects/uploads and the
# SQLite fallback survive restarts (ephemeral /tmp is wiped every reboot).
DATA_DIR = os.environ.get('HOSTING_DATA_DIR', '/data/hosting_data')
os.environ['HOSTING_DATA_DIR'] = DATA_DIR
try:
    os.makedirs(DATA_DIR, exist_ok=True)
    for sub in ('projects', 'logs', 'tmp'):
        os.makedirs(os.path.join(DATA_DIR, sub), exist_ok=True)
except Exception as exc:
    logger.warning("Could not create data dir %s: %s", DATA_DIR, exc)

# Hardcoded proxy fallback — bot works even if TG_API_PROXY secret is missing.
os.environ['TG_API_PROXY'] = (os.environ.get('TG_API_PROXY') or 'https://tgproxy-pages.pages.dev')

import teleproxy_patch
teleproxy_patch.apply()

import main as bot_module
import space_manager
import tidb_store


def _ping(url, timeout=10):
    try:
        ctx = ssl._create_unverified_context()
        req = urllib.request.Request(url)
        req.add_header('User-Agent', 'Mozilla/5.0 (keep-alive)')
        urllib.request.urlopen(req, timeout=timeout, context=ctx).read(64)
        return True
    except:
        return False


def keep_alive():
    """External keep-alive: pings this Space URL so HF counts traffic."""
    i = 0
    while True:
        try:
            for path in ['/', '/health']:
                _ping(f'{HF_SPACE_URL}{path}?_k={i}')
                time.sleep(1)
            try:
                bot_module.bot.get_me()
            except:
                pass
            i = (i + 1) % 100000
        except:
            pass
        time.sleep(45)


def _bootstrap():
    bot_module.init_db()
    bot_module.load_data()
    tidb_store.init_sync()
    if tidb_store.enabled():
        try:
            tidb_store.backup_sqlite(bot_module.DB_PATH)
        except Exception:
            pass
    bot_module.clear_old_data()
    bot_module.tidb_restore_all()
    bot_module.keep_alive()


def start_bot():
    """Only the elected leader polls Telegram; others hot-standby."""
    _bootstrap()
    while True:
        try:
            if space_manager.am_i_leader():
                if not space_manager._local['leader']:
                    space_manager._local['leader'] = True
                    logger.info("🎯 %s promoted to LEADER — polling Telegram", SPACE_NAME)
                try:
                    bot_module.bot.infinity_polling(timeout=30, long_polling_timeout=30)
                except Exception as e:
                    logger.error(f"Polling error: {e}", exc_info=True)
                    time.sleep(10)
            else:
                space_manager._local['leader'] = False
                time.sleep(15)
        except Exception as e:
            logger.error(f"Bot loop error: {e}", exc_info=True)
            time.sleep(15)


def _bot_supervisor():
    """Remote self-healing: if the bot thread dies (crash, exception, poller
    exit), restart it automatically with a short backoff so the panel never
    stays dead until the next manual redeploy."""
    failures = 0
    while True:
        t = threading.Thread(target=start_bot, daemon=True)
        t.start()
        t.join()
        failures += 1
        delay = min(10 * failures, 120)
        logger.error("Bot thread exited (failure #%s); restarting in %ss", failures, delay)
        time.sleep(delay)
        if failures >= 12:
            failures = 0


def watchdog():
    """Extra safety net: verify the supervisor thread itself is alive."""
    while True:
        time.sleep(60)
        alive = any(t.is_alive() and t.name == 'bot_supervisor' for t in threading.enumerate())
        if not alive:
            logger.error("Supervisor thread died - restarting")
            threading.Thread(target=_bot_supervisor, daemon=True, name='bot_supervisor').start()


def start_all():
    """Start every background system (idempotent)."""
    space_manager.start()
    tidb_store.start()
    threading.Thread(target=keep_alive, daemon=True).start()
    threading.Thread(target=_bot_supervisor, daemon=True, name='bot_supervisor').start()
    threading.Thread(target=watchdog, daemon=True, name='watchdog').start()
    logger.info("All background systems started on %s (%s)", SPACE_NAME, HF_SPACE_URL)


def fleet_health():
    """JSON overview of the whole fleet for the /health endpoint."""
    try:
        return {
            'space': SPACE_NAME,
            'bot': 'running',
            'leader': space_manager.am_i_leader(),
            'current_leader': space_manager.leader_name(),
            'fleet': sorted(space_manager.fleet_status().keys()),
            'tidb_cluster': tidb_store.active_cluster(),
        }
    except Exception as e:
        return {'space': SPACE_NAME, 'bot': 'running', 'error': str(e)}


def debug_state():
    """Diagnostics: why is TiDB on/off, what's installed, env presence."""
    import tidb_store as ts
    out = {
        'space': SPACE_NAME,
        'have_pymysql': ts.HAVE_PYMYSQL,
        'configured_clusters': ts.configured_clusters(),
        'enabled': ts.enabled(),
        'active_cluster': ts.active_cluster(),
        'tidb_env': {k: bool(os.environ.get(k)) for k in (
            'TIDB_HOST', 'TIDB_USER', 'TIDB_PASSWORD',
            'TIDB2_HOST', 'TIDB2_USER', 'TIDB2_PASSWORD',
            'TIDB3_HOST', 'TIDB3_USER', 'TIDB3_PASSWORD',
            'TIDB4_HOST', 'TIDB4_USER', 'TIDB4_PASSWORD',
            'TIDB5_HOST', 'TIDB5_USER', 'TIDB5_PASSWORD',
            'TELEGRAM_BOT_TOKEN', 'TG_API_PROXY')},
    }
    try:
        import psutil
        out['real_psutil'] = True
    except Exception:
        out['real_psutil'] = False
    # Live probe: try each cluster now and report the first error
    probe = []
    for c in ts.configured_clusters():
        try:
            conn, cl = ts._connect_any() if c == 1 else (ts._connect(c), c)
            conn.close()
            probe.append({'cluster': c, 'ok': True})
            break
        except Exception as e:
            probe.append({'cluster': c, 'ok': False, 'error': str(e)[:200]})
    out['probe'] = probe
    return out
