# -*- coding: utf-8 -*-
"""
tidb_store — TiDB cloud backup/restore for HostingBot.

Persists uploaded user files + sqlite DB into TiDB (MySQL protocol) so the bot
can fully self-heal: if the Space storage is wiped, startup pulls everything
back from TiDB automatically.
"""
import os
import io
import sys
import time
import base64
import sqlite3
import threading
import logging

logger = logging.getLogger('tidb_store')

try:
    import pymysql
    HAVE_PYMYSQL = True
except Exception:
    HAVE_PYMYSQL = False

# ── TiDB clusters — credentials MUST come from env (secrets) ──
# Up to 5 clusters: TIDB_*, TIDB2_*, TIDB3_*, TIDB4_*, TIDB5_*
# Rotates automatically: tries each in order until one connects.
_CLUSTER_ENV = (
    ('TIDB_HOST', 'TIDB_PORT', 'TIDB_USER', 'TIDB_PASSWORD', 'TIDB_DB'),
    ('TIDB2_HOST', 'TIDB2_PORT', 'TIDB2_USER', 'TIDB2_PASSWORD', 'TIDB2_DB'),
    ('TIDB3_HOST', 'TIDB3_PORT', 'TIDB3_USER', 'TIDB3_PASSWORD', 'TIDB3_DB'),
    ('TIDB4_HOST', 'TIDB4_PORT', 'TIDB4_USER', 'TIDB4_PASSWORD', 'TIDB4_DB'),
    ('TIDB5_HOST', 'TIDB5_PORT', 'TIDB5_USER', 'TIDB5_PASSWORD', 'TIDB5_DB'),
)

DB_PATH = os.getenv('DB_PATH', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'data', 'bot.db'))
_state = {'enabled': False, 'thread': None, 'active_cluster': 0}

_lock = threading.Lock()

_cfg_cache = None


def _cfg(cluster):
    if not (1 <= cluster <= 5):
        return ('', 4000, '', '', '')
    keys = _CLUSTER_ENV[cluster - 1]
    return (os.getenv(keys[0], ''), int(os.getenv(keys[1], '4000')),
            os.getenv(keys[2], ''), os.getenv(keys[3], ''), os.getenv(keys[4], ''))


def configured_clusters():
    """Return the list of cluster numbers that have creds set."""
    return [c for c in range(1, 6) if _cfg(c)[0] and _cfg(c)[2] and _cfg(c)[3]]


def _connect(cluster=1):
    if not HAVE_PYMYSQL:
        return None
    host, port, user, pwd, db = _cfg(cluster)
    if not (host and user and pwd):
        return None
    try:
        import ssl as _pyssl
        ssl_ctx = _pyssl.create_default_context()
    except Exception:
        ssl_ctx = None
    kwargs = dict(
        host=host, port=port,
        user=user, password=pwd,
        database=db, charset='utf8mb4',
        connect_timeout=10, read_timeout=25, write_timeout=25,
        autocommit=True,
    )
    if ssl_ctx is not None:
        kwargs['ssl'] = ssl_ctx
    return pymysql.connect(**kwargs)


def _connect_any():
    """Try every configured cluster in order (1→5). Returns (conn, cluster)."""
    err = None
    for cluster in configured_clusters():
        try:
            conn = _connect(cluster)
            if conn is not None:
                return conn, cluster
        except Exception as e:
            err = e
            logger.warning("TiDB cluster %d unreachable: %s", cluster, e)
    if err:
        raise err
    return None, 0


def check_connection():
    """Validate TiDB connectivity; sets _state['enabled'] and active cluster."""
    if not configured_clusters():
        logger.info("TiDB credentials not configured - store disabled")
        _state['enabled'] = False
        return False
    try:
        conn, cluster = _connect_any()
        if conn is None:
            logger.error("pymysql not installed - TiDB store disabled")
            return False
        with conn.cursor() as cur:
            cur.execute(
                "CREATE TABLE IF NOT EXISTS bot_files ("
                " uid BIGINT, name VARCHAR(255), ftype VARCHAR(32),"
                " data MEDIUMBLOB, ts DATETIME,"
                " PRIMARY KEY (uid, name))")
            cur.execute(
                "CREATE TABLE IF NOT EXISTS bot_state ("
                " k VARCHAR(255) PRIMARY KEY, v MEDIUMTEXT, ts DATETIME)")
            cur.execute(
                "CREATE TABLE IF NOT EXISTS bot_sqlite ("
                " id TINYINT PRIMARY KEY, data MEDIUMBLOB, ts DATETIME)")
        conn.close()
        _state['enabled'] = True
        _state['active_cluster'] = cluster
        host = _cfg(cluster)[0]
        logger.info("TiDB connected (cluster %d): %s/%s (db=%s)", cluster, host, _cfg(cluster)[1], _cfg(cluster)[4])
        return True
    except Exception as e:
        _state['enabled'] = False
        _state['active_cluster'] = 0
        logger.error("TiDB connect failed: %s", e)
        return False


def enabled():
    return _state['enabled']


def active_cluster():
    """Return the cluster number currently in use (1 or 2), or 0."""
    return _state.get('active_cluster') or 0


# ─────────────── file backup ───────────────

def backup_file(uid, name, path, ftype='executable'):
    if not _state['enabled']:
        return False
    try:
        with open(path, 'rb') as f:
            payload = base64.b64encode(f.read()).decode('ascii')
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute(
                'INSERT INTO bot_files (uid, name, ftype, data, ts) '
                'VALUES (%s, %s, %s, %s, UTC_TIMESTAMP()) '
                'ON DUPLICATE KEY UPDATE ftype=VALUES(ftype), data=VALUES(data), ts=UTC_TIMESTAMP()',
                (int(uid), name, ftype, payload))
        conn.close()
        logger.info("TiDB backup: %s/%s", uid, name)
        return True
    except Exception as e:
        logger.error("TiDB backup_file failed %s/%s: %s", uid, name, e)
        return False


def delete_file_backup(uid, name):
    if not _state['enabled']:
        return
    try:
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('DELETE FROM bot_files WHERE uid=%s AND name=%s', (int(uid), name))
        conn.close()
        logger.info("TiDB deleted backup: %s/%s", uid, name)
    except Exception as e:
        logger.error("TiDB delete failed: %s", e)


def list_backed_up_files():
    """Return [(uid, name, ftype)] of everything in TiDB."""
    if not _state['enabled']:
        return []
    try:
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('SELECT uid, name, ftype FROM bot_files')
            rows = cur.fetchall()
        conn.close()
        return [(int(u), n, t) for u, n, t in rows]
    except Exception as e:
        logger.error("TiDB list failed: %s", e)
        return []


def restore_all_files(upload_root):
    """Write every backed-up file back to disk under upload_root/<uid>/<name>.
    Returns number restored."""
    if not _state['enabled']:
        return 0
    count = 0
    try:
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('SELECT uid, name, data FROM bot_files')
            rows = cur.fetchall()
        conn.close()
        for uid, name, data in rows:
            folder = os.path.join(upload_root, str(uid))
            os.makedirs(folder, exist_ok=True)
            final = os.path.join(folder, name)
            try:
                raw = base64.b64decode(data)
                with open(final, 'wb') as f:
                    f.write(raw)
                count += 1
            except Exception:
                continue
        if count:
            logger.info("TiDB restored %d files", count)
        return count
    except Exception as e:
        logger.error("TiDB restore failed: %s", e)
        return count


# ─────────────── sqlite backup ───────────────

def backup_sqlite(db_path):
    if not _state['enabled'] or not os.path.exists(db_path):
        return False
    try:
        with open(db_path, 'rb') as f:
            payload = base64.b64encode(f.read()).decode('ascii')
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('INSERT INTO bot_sqlite (id, data, ts) VALUES (0, %s, UTC_TIMESTAMP()) '
                        'ON DUPLICATE KEY UPDATE data=VALUES(data), ts=UTC_TIMESTAMP()', (payload,))
        conn.close()
        logger.info("TiDB sqlite backup: %d bytes", len(payload))
        return True
    except Exception as e:
        logger.error("TiDB sqlite backup failed: %s", e)
        return False


def restore_sqlite(db_path):
    """Pull sqlite DB from TiDB and overwrite local (if local missing/corrupt)."""
    if not _state['enabled']:
        return False
    try:
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('SELECT data FROM bot_sqlite WHERE id=0')
            row = cur.fetchone()
        conn.close()
        if not row:
            return False
        raw = base64.b64decode(row[0])
        os.makedirs(os.path.dirname(db_path), exist_ok=True)
        with open(db_path, 'wb') as f:
            f.write(raw)
        logger.info("TiDB sqlite restored: %d bytes", len(raw))
        return True
    except Exception as e:
        logger.error("TiDB sqlite restore failed: %s", e)
        return False


# ─────────────── bot_state (json blobs) ───────────────

def set_state(key, value):
    """Store an arbitrary JSON value under a key."""
    if not _state['enabled']:
        return False
    try:
        import json
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('INSERT INTO bot_state (k, v, ts) VALUES (%s, %s, UTC_TIMESTAMP()) '
                        'ON DUPLICATE KEY UPDATE v=VALUES(v), ts=UTC_TIMESTAMP()',
                        (key, json.dumps(value)))
        conn.close()
        return True
    except Exception as e:
        logger.error("TiDB set_state failed: %s", e)
        return False


def list_state_keys(prefix=''):
    """Return all keys in bot_state matching a prefix."""
    if not _state['enabled']:
        return []
    try:
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            if prefix:
                cur.execute('SELECT k FROM bot_state WHERE k LIKE %s', (prefix + '%',))
            else:
                cur.execute('SELECT k FROM bot_state')
            rows = cur.fetchall()
        conn.close()
        return [r[0] for r in rows]
    except Exception as e:
        logger.error("TiDB list_state_keys failed: %s", e)
        return []


def get_state(key, default=None):
    if not _state['enabled']:
        return default
    try:
        import json
        conn, _ = _connect_any()
        with conn.cursor() as cur:
            cur.execute('SELECT v FROM bot_state WHERE k=%s', (key,))
            row = cur.fetchone()
        conn.close()
        if not row:
            return default
        return json.loads(row[0])
    except Exception as e:
        logger.error("TiDB get_state failed: %s", e)
        return default


def start(background=True):
    """Connect at startup; optionally in a background thread. Retries forever
    so a transient network hiccup at boot doesn't permanently disable TiDB."""
    def _init():
        while True:
            try:
                if check_connection():
                    return
            except Exception as e:
                logger.error("TiDB init error: %s", e)
            time.sleep(60)

    if background:
        t = threading.Thread(target=_init, daemon=True)
        t.start()
        _state['thread'] = t
    else:
        _init()


def init():
    start(background=True)


def init_sync():
    try:
        check_connection()
    except Exception as e:
        logger.error("TiDB init error: %s", e)