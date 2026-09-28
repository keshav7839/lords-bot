"""Background migration: Telegram episodes -> Google Drive (runs on HF Space).

Enabled when env DRIVE_MIGRATE=1 and RCLONE_CONF_B64 (base64 rclone.conf) is set.
Parallel async worker pool (DRIVE_WORKERS, default 8) with its own Telethon
client. Only migrates DRIVE_MIN_EP..DRIVE_MAX_EP (defaults 0..999999).
Progress in drive_map.json, served via /storyhub/drive-map.
"""
import asyncio
import base64
import json
import logging
import os
import stat
import subprocess
import threading
import time
import urllib.request
import zipfile
from pathlib import Path

logger = logging.getLogger('storyhub.drive')

RCLONE_URL = 'https://downloads.rclone.org/rclone-current-linux-amd64.zip'


def ensure_rclone(data_dir):
    b = Path(data_dir) / 'rclone'
    if b.exists() and os.access(b, os.X_OK):
        return
    logger.info("drive: downloading rclone...")
    zpath = str(Path(data_dir) / 'rclone.zip')
    urllib.request.urlretrieve(RCLONE_URL, zpath)
    with zipfile.ZipFile(zpath) as z:
        for n in z.namelist():
            if n.endswith('/rclone') and '/test' not in n:
                with z.open(n) as src, open(str(b), 'wb') as dst:
                    dst.write(src.read())
                break
    os.remove(zpath)
    b.chmod(b.stat().st_mode | stat.S_IEXEC)
    logger.info("drive: rclone ready")


def write_conf(data_dir):
    c = Path(data_dir) / 'rclone.conf'
    raw = os.environ.get('RCLONE_CONF_B64', '')
    if not raw:
        raise RuntimeError("RCLONE_CONF_B64 not set")
    c.write_text(base64.b64decode(raw).decode())
    c.chmod(0o600)
    return str(c)


class Migrator:
    def __init__(self, hub):
        self.hub = hub
        self.map_file = hub.data_dir / 'drive_map.json'
        self.map = {}
        self.lock = None
        self.running = False
        self.current = None
        self.done = 0
        self.failed = 0
        self.total = 0
        self.workers = int(os.environ.get('DRIVE_WORKERS', '8'))
        self.min_ep = int(os.environ.get('DRIVE_MIN_EP', '0'))
        self.max_ep = int(os.environ.get('DRIVE_MAX_EP', '999999'))

    def load(self):
        try:
            self.map = json.loads(self.map_file.read_text())
        except Exception:
            self.map = {}

    def save(self):
        self.map_file.write_text(json.dumps(self.map))

    def rc(self, conf, *args, timeout=600):
        env = {**os.environ, 'RCLONE_CONFIG': conf}
        return subprocess.run(['rclone'] + list(args), capture_output=True,
                              text=True, timeout=timeout, env=env)

    def prescan(self, conf):
        try:
            r = self.rc(conf, 'lsjson', 'gdrive:storyhub/', timeout=300)
            if r.returncode != 0:
                logger.error("drive prescan FAIL: %s", r.stderr[:150])
                return
            live = {}
            for f in json.loads(r.stdout or '[]'):
                name = f.get('Name', '')
                if name.startswith('EP') and name.endswith('.mp3'):
                    try:
                        live[int(name[2:-4])] = f.get('ID')
                    except ValueError:
                        continue
            # DROP stale entries: mapped EP whose file no longer exists on Drive
            pruned = 0
            for mid in list(self.map.keys()):
                v = self.map[mid]
                if v.get('ep') not in live:
                    del self.map[mid]
                    pruned += 1
            # ADD/backfill from live listing
            for ep, fid in live.items():
                hit = None
                for mid, v in self.map.items():
                    if v.get('ep') == ep:
                        hit = mid
                        break
                if hit:
                    self.map[hit]['fid'] = fid
                else:
                    self.map[f'drive:{fid}'] = {'ep': ep, 'name': f'EP{ep}.mp3', 'fid': fid}
            self.save()
            logger.info("drive prescan: pruned %d stale, %d mapped", pruned, len(self.map))
        except Exception as e:
            logger.error("drive prescan error: %s", e)

    def push_catalog(self, conf):
        """Maintain catalog.json on Drive so the native app always has fresh index."""
        try:
            titles = {}
            for e in (self.hub._episodes or []):
                titles[e['ep_num']] = (e.get('title', ''), e.get('size_mb', 0))
            eps = {}
            for mid, v in self.map.items():
                if not v.get('fid') or not v.get('ep'):
                    continue
                t, sz = titles.get(v['ep'], ('', 0))
                eps[str(v['ep'])] = {'fid': v['fid'], 'title': (t or '')[:100], 'size': sz}
            import time as _t
            cat = {'updated': _t.time(), 'count': len(eps), 'eps': eps}
            cp = str(Path(str(self.hub.data_dir)) / 'catalog.json')
            Path(cp).write_text(json.dumps(cat))
            r = self.rc(conf, 'copyto', cp, 'gdrive:storyhub/catalog.json', timeout=300)
            if r.returncode == 0:
                logger.info("drive: catalog pushed (%d eps)", len(eps))
            else:
                logger.warning("drive catalog push FAIL: %s", r.stderr.strip()[:120])
        except Exception as e:
            logger.error("drive catalog push error: %s", e)

    async def _one(self, sem, loop, conf, client, tmpdir, e):
        mid, ep = e['id'], e['ep_num']
        name = f'EP{ep}.mp3'
        async with sem:
            self.current = ep
            try:
                msg = await asyncio.wait_for(
                    client.get_messages('me', ids=mid), timeout=120)
                if not msg or not msg.document:
                    return
                p = os.path.join(tmpdir, name)
                if os.path.exists(p):
                    os.remove(p)
                await asyncio.wait_for(
                    client.download_media(msg, file=p), timeout=300)
                r = await loop.run_in_executor(
                    None, lambda: self.rc(conf, 'copyto', p,
                                          f'gdrive:storyhub/{name}', timeout=900))
                try:
                    os.remove(p)
                except Exception:
                    pass
                if r.returncode != 0:
                    async with self.lock:
                        self.failed += 1
                    logger.warning("drive EP%d upload FAIL: %s", ep, r.stderr.strip()[:120])
                    return
                fid = None
                try:
                    r2 = await loop.run_in_executor(
                        None, lambda: self.rc(conf, 'lsjson', f'gdrive:storyhub/{name}', timeout=120))
                    fid = json.loads(r2.stdout or '[]')[0]['ID']
                except Exception:
                    pass
                async with self.lock:
                    self.map[str(mid)] = {'ep': ep, 'name': name, 'fid': fid}
                    for k in [k for k, v in self.map.items()
                              if k.startswith('drive:') and v.get('ep') == ep]:
                        del self.map[k]
                    self.done += 1
                    if self.done % 25 == 0:
                        self.save()
                        logger.info("drive: %d/%d done, %d failed (EP%d)",
                                    self.done, self.total, self.failed, ep)
                        try:
                            await loop.run_in_executor(None, lambda: self.push_catalog(conf))
                        except Exception as ex:
                            logger.error("drive catalog: %s", ex)
                    else:
                        self.save()
            except Exception as ex:
                async with self.lock:
                    self.failed += 1
                logger.error("drive EP%d error: %s", ep, str(ex)[:150])

    async def _go(self, conf, eps):
        from telethon import TelegramClient
        from telethon.sessions import StringSession
        ss = self.hub._get_session()
        if not ss:
            raise RuntimeError("no session")
        client = TelegramClient(
            StringSession(ss), self.hub.api_id, self.hub.api_hash,
            connection_retries=10, timeout=60, request_retries=5)
        await client.start()
        me = await client.get_me()
        logger.info("drive: TG connected as %s, %d workers", getattr(me, 'first_name', '?'), self.workers)
        self.lock = asyncio.Lock()
        loop = asyncio.get_event_loop()
        tmpdir = str(self.hub.data_dir / 'dtmp')
        os.makedirs(tmpdir, exist_ok=True)
        sem = asyncio.Semaphore(self.workers)
        # two passes: second pass retries failures
        for p in range(2):
            if p == 1:
                done_eps = {v['ep'] for v in self.map.values() if v.get('fid') or v.get('name')}
                eps = [e for e in eps if e['ep_num'] not in done_eps]
                if not eps:
                    break
                logger.info("drive: retry pass, %d left", len(eps))
            await asyncio.gather(*(self._one(sem, loop, conf, client, tmpdir, e) for e in eps))
        try:
            await client.disconnect()
        except Exception:
            pass

    def run(self):
        self.running = True
        try:
            data_dir = str(self.hub.data_dir)
            ensure_rclone(data_dir)
            os.environ['PATH'] = data_dir + os.pathsep + os.environ.get('PATH', '')
            conf = write_conf(data_dir)
            self.load()
            # drop entries outside range (e.g. Drive was wiped / range changed)
            self.map = {k: v for k, v in self.map.items()
                        if self.min_ep <= v.get('ep', -1) <= self.max_ep}
            self.save()
            self.prescan(conf)
            while not self.hub._loaded or not self.hub._episodes:
                logger.info("drive: waiting for episode list...")
                time.sleep(30)
            done_eps = {v['ep'] for v in self.map.values()}
            eps = sorted(
                [e for e in self.hub._episodes
                 if e['ep_num'] not in done_eps
                 and self.min_ep <= e['ep_num'] <= self.max_ep],
                key=lambda x: x['ep_num'])
            self.total = len(eps)
            logger.info("drive: migrating %d episodes (EP%d-%d, %d workers)",
                        self.total, self.min_ep, self.max_ep, self.workers)
            loop = asyncio.new_event_loop()
            asyncio.set_event_loop(loop)
            loop.run_until_complete(self._go(conf, eps))
            self.save()
            try:
                self.push_catalog(conf)
            except Exception as ex:
                logger.error("drive catalog final: %s", ex)
            logger.info("drive migration DONE: %d ok, %d failed", self.done, self.failed)
        except Exception as e:
            logger.error("drive migrator died: %s", e, exc_info=True)
        finally:
            self.running = False


def start_if_enabled(hub):
    if os.environ.get('DRIVE_MIGRATE') != '1':
        return None
    if not os.environ.get('RCLONE_CONF_B64'):
        logger.warning("drive: DRIVE_MIGRATE=1 but no RCLONE_CONF_B64")
        return None
    m = Migrator(hub)
    threading.Thread(target=m.run, daemon=True, name='drive_migrate').start()
    hub.drive_migrator = m
    return m
