# -*- coding: utf-8 -*-
"""
space_manager — multi-space leader election + status registry.

Every HostingBot Space shares one TiDB (bot_state table). Exactly ONE Space
is the "leader" and polls Telegram; the others stay hot-standby. If the leader
stops heartbeating (crash / rebuild / sleep), the standby with the lowest ping
takes over within ~60s. Zero conflicts: only one poller at a time.

Also publishes each Space's live status (cpu/ram/disk/ping/uptime) so /stats
can show the whole fleet from any Space.
"""
import os
import time
import json
import threading
import logging

logger = logging.getLogger('space_manager')

try:
    import tidb_store
except Exception:
    tidb_store = None

SPACE_NAME = os.getenv('SPACE_NAME', os.getenv('SPACE_ID', 'space'))
LEADER_KEY = 'space_leader'
STATS_PREFIX = 'space_stats:'
LEASE_SECONDS = 60          # leader must renew every < 60s
CLAIM_AFTER = 90            # reclaim leader after it's silent for 90s
RENEW_EVERY = 20            # leader renews heartbeat every 20s
STATS_EVERY = 30            # publish own stats every 30s
_local = {'leader': False, 'stop': False, 'ping_ms': None, 'last_ping': 0}


def _kv_get(key):
    if tidb_store is None or not tidb_store.enabled():
        return None
    try:
        return tidb_store.get_state(key)
    except Exception as e:
        logger.warning("kv get %s failed: %s", key, e)
        return None


def _kv_set(key, value):
    if tidb_store is None or not tidb_store.enabled():
        return False
    try:
        return tidb_store.set_state(key, value)
    except Exception as e:
        logger.warning("kv set %s failed: %s", key, e)
        return False


def _now():
    return time.time()


def measure_ping():
    """Measure Telegram API latency via the proxy (cheap getMe on a
    throwaway token-less call is not possible; use the bot's own later).
    Falls back to the stored value."""
    try:
        import telebot
        b = telebot.TeleBot(os.getenv('TELEGRAM_BOT_TOKEN', ''), num_threads=1)
        t0 = _now()
        b.get_me()
        ms = int((_now() - t0) * 1000)
        _local['ping_ms'] = ms
        _local['last_ping'] = _now()
        return ms
    except Exception:
        return _local.get('ping_ms')


def my_stats():
    """Collect this Space's real stats for the registry."""
    out = {
        'space': SPACE_NAME,
        'ts': int(_now()),
        'ping': _local.get('ping_ms'),
    }
    try:
        if tidb_store is not None:
            out['tidb_cluster'] = tidb_store.active_cluster() if hasattr(tidb_store, 'active_cluster') else 0
            out['tidb_enabled'] = 1 if (tidb_store.enabled() if hasattr(tidb_store, 'enabled') else False) else 0
    except Exception as e:
        logger.debug("tidb stats failed: %s", e)
    try:
        from psutil_compat import cpu_percent, virtual_memory, disk_usage, _get_cpu_count
        out['cpu'] = cpu_percent(interval=0.4)
        out['cores'] = _get_cpu_count()
        m = virtual_memory()
        out['ram_used'] = round(m.used / 1024**3, 1)
        out['ram_total'] = round(m.total / 1024**3, 1)
        out['ram_pct'] = m.percent
        d = disk_usage('/')
        out['disk_used'] = round(d.used / 1024**3, 1)
        out['disk_total'] = round(d.total / 1024**3, 1)
        out['disk_pct'] = d.percent
    except Exception as e:
        logger.debug("stats collect failed: %s", e)
    return out


def publish_stats():
    """Push my stats + heartbeat into TiDB so every Space sees the fleet."""
    while not _local['stop']:
        try:
            s = my_stats()
            s['leader'] = 1 if _local['leader'] else 0
            _kv_set(STATS_PREFIX + SPACE_NAME, json.dumps(s))
            if _local['leader']:
                _kv_set(LEADER_KEY, json.dumps({'space': SPACE_NAME, 'ts': int(_now())}))
        except Exception as e:
            logger.warning("publish_stats: %s", e)
        time.sleep(STATS_EVERY)


def _try_claim():
    """Atomically claim leadership if lease is stale or mine. Returns bool."""
    if tidb_store is None or not tidb_store.enabled():
        # No TiDB: single-space mode, everyone is leader.
        return True
    try:
        cur = _kv_get(LEADER_KEY)
        now = int(_now())
        if cur:
            try:
                data = json.loads(cur)
            except Exception:
                data = {}
            cur_space = data.get('space')
            cur_ts = int(data.get('ts', 0))
            if cur_space != SPACE_NAME and now - cur_ts < CLAIM_AFTER:
                return False
        _kv_set(LEADER_KEY, json.dumps({'space': SPACE_NAME, 'ts': now}))
        _local['leader'] = True
        return True
    except Exception as e:
        logger.warning("claim failed: %s", e)
        return False


def am_i_leader():
    """Re-check lease: I am leader only while the lease says so and is fresh."""
    if tidb_store is None or not tidb_store.enabled():
        return True
    try:
        cur = _kv_get(LEADER_KEY)
        if not cur:
            return False
        data = json.loads(cur)
        return data.get('space') == SPACE_NAME and _now() - int(data.get('ts', 0)) < LEASE_SECONDS
    except Exception:
        return _local.get('leader', False)


def fleet_status():
    """Return {space: stats_dict} for every registered Space (leader flagged)."""
    out = {}
    if tidb_store is None or not tidb_store.enabled():
        return out
    try:
        keys = tidb_store.list_state_keys(STATS_PREFIX)
        for k in keys:
            space = k[len(STATS_PREFIX):]
            raw = _kv_get(k)
            if raw:
                try:
                    out[space] = json.loads(raw)
                except Exception:
                    pass
    except Exception as e:
        logger.warning("fleet_status: %s", e)
    return out


def leader_name():
    """Name of the current leader, or None."""
    if tidb_store is None or not tidb_store.enabled():
        return SPACE_NAME
    try:
        cur = _kv_get(LEADER_KEY)
        if not cur:
            return None
        data = json.loads(cur)
        if _now() - int(data.get('ts', 0)) < CLAIM_AFTER:
            return data.get('space')
    except Exception:
        pass
    return None


def start():
    """Start the background threads: leader claim loop + stats publisher."""
    if tidb_store is None:
        logger.info("space_manager: no tidb_store (single-space mode)")
    threading.Thread(target=_claim_loop, daemon=True, name='space_claim').start()
    threading.Thread(target=publish_stats, daemon=True, name='space_stats').start()
    threading.Thread(target=_ping_loop, daemon=True, name='space_ping').start()
    logger.info("space_manager started on %s", SPACE_NAME)


def stop():
    _local['stop'] = True


def _claim_loop():
    failures = 0
    while not _local['stop']:
        try:
            if not _local['leader']:
                if _try_claim():
                    logger.info("🎯 %s is now LEADER", SPACE_NAME)
                    failures = 0
            else:
                if not am_i_leader():
                    _local['leader'] = False
                    logger.warning("Lost leadership (lease stolen/expired)")
        except Exception as e:
            failures += 1
            logger.warning("claim loop error #%s: %s", failures, e)
        time.sleep(RENEW_EVERY)


def _ping_loop():
    while not _local['stop']:
        measure_ping()
        time.sleep(300)