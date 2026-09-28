import os
import re
import json
import asyncio
import threading
import logging
import queue
import time
import urllib.request
import urllib.parse
import subprocess
import shutil
from pathlib import Path
import traceback

logger = logging.getLogger('storyhub')

EP_PATTERN = re.compile(r'^Ep[._\s]+(\d+)[._\s]*[-\u2013\u2014]\s*(.*)', re.IGNORECASE)
EP_START = 450

# ─── Compressed Audio Support ──────────────────────────────────────────────
# Check for ffmpeg/ffprobe availability
def _has_ffmpeg():
    return shutil.which('ffmpeg') is not None and shutil.which('ffprobe') is not None

HAS_FFMPEG = _has_ffmpeg()

def _probe_duration(path):
    """Get audio duration in seconds using ffprobe."""
    try:
        result = subprocess.run(
            ['ffprobe', '-v', 'error', '-show_entries', 'format=duration',
             '-of', 'default=noprint_wrappers=1:nokey=1', str(path)],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0 and result.stdout.strip():
            return float(result.stdout.strip())
    except Exception:
        pass
    return None

def _transcode_to_opus(src_path, dst_path, bitrate_k=32):
    """Transcode audio to Opus at 1.5x speed. Returns True on success."""
    if not HAS_FFMPEG:
        return False
    try:
        result = subprocess.run(
            ['ffmpeg', '-y', '-i', str(src_path), '-af', 'atempo=1.5',
             '-c:a', 'libopus',
             '-b:a', f'{bitrate_k}k', '-vbr', 'on', '-compression_level', '10',
             '-application', 'voip', str(dst_path)],
            capture_output=True, timeout=300
        )
        return result.returncode == 0 and Path(dst_path).exists()
    except Exception as e:
        logger.warning("Transcode failed: %s", e)
        return False

def _get_compressed_path(original_path):
    """Get the compressed (Opus/OGG) version path."""
    p = Path(original_path)
    return p.with_suffix('.opus.ogg')

def _ensure_compressed(original_path):
    """Ensure compressed version exists, create if needed."""
    compressed_path = _get_compressed_path(original_path)
    if compressed_path.exists() and compressed_path.stat().st_size > 0:
        return compressed_path
    if _transcode_to_opus(original_path, compressed_path):
        logger.info("Transcoded %s -> %s (%.1f%% of original)",
                    original_path, compressed_path,
                    compressed_path.stat().st_size / max(1, Path(original_path).stat().st_size) * 100)
        return compressed_path
    return None
# ────────────────────────────────────────────────────────────────────────────

class StoryHub:
    def __init__(self):
        self._episodes = []
        self._loaded = False
        self._last_error = None
        self._dl_lock = threading.Lock()
        self.prefix = '/storyhub'
        self._compress_status = {'running': False, 'done': 0, 'failed': 0, 'total': 0, 'current_ep': 0, 'log': []}

        self.bot_token = os.environ.get('TELEGRAM_BOT_TOKEN', '')
        self.api_id = int(os.environ.get('STORYHUB_API_ID', '34910912'))
        self.api_hash = os.environ.get('STORYHUB_API_HASH', '5cf7509a1d1f9671e3cdc854e17b7d62')
        self.session_file = os.environ.get('STORYHUB_SESSION_FILE', '/data/storyhub_session.txt')
        self.owner_id = '7038720965'

        self.data_dir = Path(os.environ.get('HOSTING_DATA_DIR', '/data/hosting_data')) / 'storyhub'
        self.data_dir.mkdir(parents=True, exist_ok=True)
        self.episodes_db = self.data_dir / 'episodes.json'
        self.download_dir = self.data_dir / 'downloads'
        self.download_dir.mkdir(exist_ok=True)

    def _get_session(self):
        for path in [self.session_file, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'storyhub_session.txt')]:
            p = Path(path)
            if p.exists():
                ss = p.read_text().strip()
                if ss:
                    return ss
        return None

    def _tg_api(self, method, params=None, files=None):
        proxy = os.environ.get('TG_API_PROXY', '').rstrip('/')
        base = (proxy + '/bot') if proxy else 'https://api.telegram.org/bot'
        url = '%s%s/%s' % (base, self.bot_token, method)
        if files:
            boundary = '----FormBoundary7MA4YWxkTrZu0gW'
            body = b''
            if params:
                for k, v in params.items():
                    body += ('--%s\r\nContent-Disposition: form-data; name="%s"\r\n\r\n%s\r\n' % (boundary, k, v)).encode()
            for k, f in files.items():
                if isinstance(f, tuple):
                    fname, data = f
                    body += ('--%s\r\nContent-Disposition: form-data; name="%s"; filename="%s"\r\nContent-Type: audio/mpeg\r\n\r\n' % (boundary, k, fname)).encode()
                    body += data
                    body += b'\r\n'
            body += ('--%s--\r\n' % boundary).encode()
            req = urllib.request.Request(url, data=body)
            req.add_header('Content-Type', 'multipart/form-data; boundary=' + boundary)
        else:
            data = urllib.parse.urlencode(params or {}).encode()
            req = urllib.request.Request(url, data=data)
        ctx = __import__('ssl')._create_unverified_context()
        resp = urllib.request.urlopen(req, timeout=60, context=ctx)
        return json.loads(resp.read())

    def _forward_to_self(self, msg_id):
        try:
            r = self._tg_api('forwardMessage', {
                'chat_id': self.owner_id,
                'from_chat_id': self.owner_id,
                'message_id': str(msg_id),
            })
            if r.get('ok'):
                return r['result']['message_id']
        except Exception as e:
            logger.warning("forward failed for %d: %s", msg_id, e)
        return None

    async def _fetch_episodes(self):
        ss = self._get_session()
        if not ss:
            raise RuntimeError("No session")
        from telethon import TelegramClient
        from telethon.sessions import StringSession
        client = TelegramClient(StringSession(ss), self.api_id, self.api_hash)
        await client.start()
        try:
            ep_map = {}
            total = 0
            async for msg in client.iter_messages('me', limit=50000):
                total += 1
                txt = (msg.message or '').strip()
                m = EP_PATTERN.match(txt)
                if not m:
                    continue
                ep_num = int(m.group(1))
                if ep_num < EP_START:
                    continue
                title = m.group(2).strip() or txt[:80]
                filename = ''
                performer = ''
                size_mb = 0
                is_audio = False
                if msg.document:
                    for attr in msg.document.attributes:
                        cls = type(attr).__name__
                        if cls == 'DocumentAttributeAudio':
                            is_audio = True
                            performer = attr.performer or ''
                            if attr.title:
                                title = attr.title
                        elif cls == 'DocumentAttributeFilename':
                            filename = attr.file_name or ''
                    size_mb = round(msg.document.size / (1024 * 1024), 2)
                ep_map[ep_num] = {
                    'id': msg.id, 'ep_num': ep_num, 'title': title,
                    'filename': filename, 'performer': performer,
                    'size_mb': size_mb,
                    'date': msg.date.isoformat() if msg.date else '',
                    'has_audio': is_audio or bool(filename),
                }
                if total % 500 == 0:
                    logger.info("StoryHub: scanned %d, found %d", total, len(ep_map))
            episodes = [ep_map[n] for n in sorted(ep_map.keys())]
            self.episodes_db.write_text(json.dumps(episodes, indent=1))
            self._episodes = episodes
            self._loaded = True
            logger.info("StoryHub done: %d messages, %d episodes", total, len(episodes))
        finally:
            await client.disconnect()

    def _init(self):
        self._load_cached()
        ss = self._get_session()
        if not ss:
            self._last_error = "No session"
            return
        logger.info("StoryHub: fetching episodes...")
        try:
            loop = __import__('asyncio').new_event_loop()
            loop.run_until_complete(self._fetch_episodes())
            loop.close()
        except Exception as e:
            self._last_error = str(e)[:500]
            logger.error("StoryHub fetch failed: %s", e, exc_info=True)

    def _load_cached(self):
        if self.episodes_db.exists():
            try:
                self._episodes = json.loads(self.episodes_db.read_text())
                self._loaded = True
                logger.info("StoryHub: loaded %d cached episodes", len(self._episodes))
                restored = self._restore_compressed_ids()
                if restored:
                    logger.info("Restored %d compressed IDs from backup", restored)
            except Exception:
                pass

    # --- Persistent download worker (single connected Telethon client) ---
    def _ensure_worker(self):
        if getattr(self, '_dl_worker_started', False):
            return
        self._dl_worker_started = True
        self._dl_queue = queue.Queue()
        self._dl_ready = False
        self._dl_client = None
        threading.Thread(target=self._dl_worker_loop, daemon=True, name='storyhub_dl').start()

    def _dl_worker_loop(self):
        loop = asyncio.new_event_loop()
        asyncio.set_event_loop(loop)
        try:
            loop.run_until_complete(self._dl_worker_main())
        except Exception as e:
            logger.error("dl worker loop died: %s", e, exc_info=True)
            self._dl_ready = False

    async def _dl_worker_main(self):
        from telethon import TelegramClient
        from telethon.sessions import StringSession
        import asyncio as _aio
        delay = 20  # let the episode-scan client finish first
        logger.info("StoryHub: download worker starting in %ds...", delay)
        await _aio.sleep(delay)
        while True:
            try:
                ss = self._get_session()
                if not ss:
                    self._last_error = "No session (dl worker)"
                    await _aio.sleep(60)
                    continue
                client = TelegramClient(
                    StringSession(ss), self.api_id, self.api_hash,
                    connection_retries=10, timeout=60, request_retries=5,
                )
                await client.start()
                me = await client.get_me()
                logger.info("StoryHub: download worker connected as %s", getattr(me, 'first_name', '?'))
                self._dl_client = client
                self._dl_ready = True
                loop = _aio.get_event_loop()
                while True:
                    msg_id, event, box = await loop.run_in_executor(None, self._dl_queue.get)
                    try:
                        path, fname = await self._dl_one(client, msg_id)
                        box['path'], box['fname'] = path, fname
                    except Exception as e:
                        box['error'] = str(e)[:300]
                        logger.error("dl worker error %d: %s", msg_id, e)
                    finally:
                        event.set()
            except Exception as e:
                self._dl_ready = False
                logger.error("dl worker reconnect: %s", e)
                try:
                    await client.disconnect()
                except Exception:
                    pass
                await _aio.sleep(30)

    async def _dl_one(self, client, msg_id):
        msg = await client.get_messages('me', ids=msg_id)
        if not msg or not msg.document:
            return None, None
        fname = 'audio_%d.mp3' % msg_id
        for attr in msg.document.attributes:
            if type(attr).__name__ == 'DocumentAttributeFilename':
                fname = attr.file_name or fname
        p = self.download_dir / fname
        if not p.exists():
            tmp = str(p) + '.part'
            await client.download_media(msg, file=tmp)
            os.replace(tmp, str(p))
        return str(p), fname

    def _cleanup_downloads(self):
        now = time.time()
        max_age = 24 * 3600
        deleted = 0
        freed = 0
        try:
            for f in self.download_dir.iterdir():
                if f.is_file():
                    age = now - f.stat().st_mtime
                    if age > max_age:
                        sz = f.stat().st_size
                        f.unlink()
                        deleted += 1
                        freed += sz
        except Exception as e:
            logger.error("Cleanup error: %s", e)
        if deleted:
            logger.info("Cleanup: deleted %d files, freed %d MB", deleted, freed // (1024*1024))

    def _cleanup_loop(self):
        while True:
            time.sleep(3600)
            self._cleanup_downloads()

    # ─── Batch Compress: download→compress→upload→delete→next ────────────
    def _compress_all(self):
        """Main compress loop. Runs in a thread."""
        status = self._compress_status
        status['running'] = True
        status['done'] = 0
        status['failed'] = 0
        status['log'] = []
        try:
            eps_to_do = [e for e in self._episodes if not e.get('compressed_id') and e['ep_num'] >= 800]
            status['total'] = len(eps_to_do)
            if not eps_to_do:
                status['log'].append('All episodes already compressed')
                status['running'] = False
                return
            ss = self._get_session()
            if not ss:
                status['log'].append('No session file found')
                status['running'] = False
                return
            import asyncio as _aio
            loop = _aio.new_event_loop()
            _aio.set_event_loop(loop)
            loop.run_until_complete(self._compress_all_async(ss, eps_to_do))
            loop.close()
        except Exception as e:
            status['log'].append('Error: %s' % str(e)[:200])
            logger.error("compress_all error: %s", e, exc_info=True)
        finally:
            status['running'] = False

    async def _compress_all_async(self, ss, eps_to_do):
        from telethon import TelegramClient
        from telethon.sessions import StringSession
        status = self._compress_status
        client = TelegramClient(StringSession(ss), self.api_id, self.api_hash,
                                connection_retries=5, timeout=120, request_retries=3)
        await client.start()
        try:
            for ep in eps_to_do:
                msg_id = ep['id']
                ep_num = ep['ep_num']
                status['current_ep'] = ep_num
                status['log'].append('EP %d: downloading...' % ep_num)
                try:
                    # Download
                    path, fname = await self._dl_one(client, msg_id)
                    if not path or not os.path.exists(path):
                        status['log'].append('EP %d: download failed' % ep_num)
                        status['failed'] += 1
                        continue
                    orig_size = os.path.getsize(path)
                    # Compress
                    compressed_path = Path(path).with_suffix('.opus.ogg')
                    ok = _transcode_to_opus(Path(path), compressed_path)
                    if not ok or not compressed_path.exists():
                        status['log'].append('EP %d: compress failed' % ep_num)
                        status['failed'] += 1
                        self._delete_file(path)
                        continue
                    comp_size = compressed_path.stat().st_size
                    # Upload to Saved Messages
                    status['log'].append('EP %d: uploading %.1fMB...' % (ep_num, comp_size/(1024*1024)))
                    from telethon.types import DocumentAttributeAudio
                    caption = 'EP%d | %.1fMB orig | %.1fMB compressed | msg_id:%d' % (ep_num, orig_size/(1024*1024), comp_size/(1024*1024), msg_id)
                    sent = await client.send_file('me', str(compressed_path),
                                                  voice_note=True,
                                                  caption=caption,
                                                  attributes=[
                                                      DocumentAttributeAudio(
                                                          duration=0,
                                                          title='EP %d' % ep_num,
                                                          performer='StoryHub 1.5x',
                                                      )
                                                  ])
                    # Store compressed_id
                    ep['compressed_id'] = sent.id
                    self._save_episodes_db()
                    status['done'] += 1
                    status['log'].append('EP %d: done (id=%d, %.1fMB)' % (ep_num, sent.id, comp_size/(1024*1024)))
                    # Delete local files
                    self._delete_file(str(compressed_path))
                    self._delete_file(path)
                except Exception as e:
                    status['failed'] += 1
                    status['log'].append('EP %d: error %s' % (ep_num, str(e)[:100]))
                    logger.error("compress EP %d error: %s", ep_num, e, exc_info=True)
                    # Cleanup on error too
                    for p in [path if 'path' in dir() else None,
                              str(compressed_path) if 'compressed_path' in dir() and compressed_path.exists() else None]:
                        if p:
                            self._delete_file(p)
                    # Keep log manageable
                if len(status['log']) > 200:
                    status['log'] = status['log'][-100:]
            # Send summary to Saved Messages
            try:
                done_eps = [e for e in self._episodes if e.get('compressed_id')]
                total_size = sum(e.get('size_mb', 0) for e in done_eps)
                summary = 'StoryHub Compressed Summary\n'
                summary += 'Total: %d episodes compressed\n' % len(done_eps)
                summary += 'Original total: ~%.0fMB\n' % total_size
                summary += '\nEP | orig_size | compressed_msg_id\n'
                for e in sorted(done_eps, key=lambda x: x['ep_num']):
                    summary += 'EP%d | %sMB | %d\n' % (e['ep_num'], e.get('size_mb', 0), e['compressed_id'])
                await client.send_message('me', summary)
                status['log'].append('Summary sent to Saved Messages')
            except Exception as e:
                status['log'].append('Summary send failed: %s' % str(e)[:100])
            # Backup compressed IDs
            self._backup_compressed_ids()
        finally:
            await client.disconnect()

    def _delete_file(self, path):
        try:
            if path and os.path.exists(path):
                os.remove(path)
        except Exception:
            pass

    def _save_episodes_db(self):
        try:
            self.episodes_db.write_text(json.dumps(self._episodes, indent=1))
        except Exception as e:
            logger.error("save episodes_db failed: %s", e)

    def _backup_compressed_ids(self):
        """Save compressed IDs to separate backup file."""
        try:
            backup = {}
            for e in self._episodes:
                if e.get('compressed_id'):
                    backup[str(e['ep_num'])] = {
                        'id': e['id'],
                        'compressed_id': e['compressed_id'],
                        'size_mb': e.get('size_mb', 0)
                    }
            backup_path = self.data_dir / 'compressed_backup.json'
            backup_path.write_text(json.dumps(backup))
            logger.info("Backed up %d compressed IDs", len(backup))
        except Exception as e:
            logger.error("Backup failed: %s", e)

    def _restore_compressed_ids(self):
        """Restore compressed IDs from backup if episodes_db lost them."""
        try:
            backup_path = self.data_dir / 'compressed_backup.json'
            if not backup_path.exists():
                return 0
            backup = json.loads(backup_path.read_text())
            restored = 0
            for e in self._episodes:
                key = str(e['ep_num'])
                if key in backup and not e.get('compressed_id'):
                    e['compressed_id'] = backup[key]['compressed_id']
                    restored += 1
            if restored:
                self._save_episodes_db()
                logger.info("Restored %d compressed IDs from backup", restored)
            return restored
        except Exception as e:
            logger.error("Restore failed: %s", e)
            return 0

    def start(self):
        threading.Thread(target=self._init, daemon=True, name='storyhub_init').start()
        threading.Thread(target=self._cleanup_loop, daemon=True, name='storyhub_cleanup').start()
        self._ensure_worker()
        try:
            from . import drive_migrate
            drive_migrate.start_if_enabled(self)
        except Exception as e:
            logger.error("drive migrate start failed: %s", e)

    def debug_info(self):
        ss = self._get_session()
        dl_files = list(self.download_dir.iterdir()) if self.download_dir.exists() else []
        dl_size = sum(f.stat().st_size for f in dl_files if f.is_file())
        return {
            'loaded': self._loaded,
            'total': len(self._episodes),
            'compressed_count': len([e for e in self._episodes if e.get('compressed_id')]),
            'session_found': bool(ss),
            'session_file': self.session_file,
            'session_len': len(ss) if ss else 0,
            'episodes_db_exists': self.episodes_db.exists(),
            'last_error': getattr(self, '_last_error', None),
            'has_bot_token': bool(self.bot_token),
            'downloaded_files': len(dl_files),
            'downloaded_size_mb': dl_size // (1024*1024),
            'dl_worker_ready': getattr(self, '_dl_ready', False),
            'dl_queue_size': self._dl_queue.qsize() if hasattr(self, '_dl_queue') else -1,
            'compress': self._compress_status,
            'drive_migrate': self._migrate_status(),
        }

    def _migrate_status(self):
        m = getattr(self, 'drive_migrator', None)
        if not m:
            return {'enabled': os.environ.get('DRIVE_MIGRATE') == '1', 'running': False}
        try:
            mapped = len(json.loads((self.data_dir / 'drive_map.json').read_text()))
        except Exception:
            mapped = 0
        return {'enabled': True, 'running': m.running, 'current_ep': m.current,
                'done': m.done, 'failed': m.failed, 'total': m.total, 'mapped': mapped}

    def download_file(self, msg_id):
        filename = 'audio_%d.mp3' % msg_id
        path = str(self.download_dir / filename)
        if os.path.exists(path):
            return path, filename

        ss = self._get_session()
        if not ss:
            raise RuntimeError("No session")

        # Fast path: persistent worker (already connected)
        self._ensure_worker()
        if getattr(self, '_dl_ready', False):
            for attempt in range(2):
                event = threading.Event()
                box = {}
                self._dl_queue.put((msg_id, event, box))
                if not event.wait(timeout=240):
                    logger.warning("dl worker timeout %d (attempt %d)", msg_id, attempt + 1)
                    continue
                if box.get('error'):
                    raise RuntimeError("download failed: %s" % box['error'])
                return box.get('path'), box.get('fname')
            raise RuntimeError("download timed out, please retry")

        # Fallback: one-shot subprocess (worker not connected yet)
        with self._dl_lock:
            import subprocess
            script = '''
import asyncio
from telethon import TelegramClient
from telethon.sessions import StringSession
import sys

ss = sys.argv[1]
msg_id = int(sys.argv[2])
out_dir = sys.argv[3]

async def dl():
    client = TelegramClient(StringSession(ss), %d, '%s', connection_retries=5, timeout=60, request_retries=3)
    await client.start()
    msg = await client.get_messages('me', ids=msg_id)
    if not msg or not msg.document:
        await client.disconnect()
        print('NO_DOC')
        return
    fname = 'audio_%%d.mp3' %% msg_id
    for attr in msg.document.attributes:
        if type(attr).__name__ == 'DocumentAttributeFilename':
            fname = attr.file_name or fname
    import os
    p = os.path.join(out_dir, fname)
    if not os.path.exists(p):
        await client.download_media(msg, file=p)
    await client.disconnect()
    print('OK:' + fname)

asyncio.run(dl())
''' % (self.api_id, self.api_hash)
            result = subprocess.run(
                ['python3', '-c', script, ss, str(msg_id), str(self.download_dir)],
                capture_output=True, text=True, timeout=300
            )
            if result.returncode != 0:
                raise RuntimeError("download failed: %s" % (result.stderr or result.stdout))
            out = result.stdout.strip()
            if out.startswith('OK:'):
                fname = out[3:]
                return str(self.download_dir / fname), fname
            elif out == 'NO_DOC':
                return None, None
            else:
                raise RuntimeError("download failed: %s" % out)

    def register_routes(self, app, prefix='/storyhub'):
        from flask import request, jsonify, Response
        self.prefix = prefix

        @app.route(prefix + '/')
        def storyhub_index():
            return self._render_index(request)

        @app.route(prefix + '/api/episodes')
        def storyhub_api():
            page = max(1, int(request.args.get('page', 1)))
            q = request.args.get('q', '').strip()
            per_page = 50
            eps = self._episodes
            if q:
                ql = q.lower()
                eps = [e for e in eps if ql in (e['title'] + ' ' + e.get('filename', '') + ' ' + e.get('performer', '')).lower()]
            total = len(eps)
            total_pages = max(1, (total + per_page - 1) // per_page)
            start = (page - 1) * per_page
            return jsonify({
                'episodes': eps[start:start + per_page],
                'page': page, 'total_pages': total_pages,
                'total': total, 'loaded': self._loaded,
                'worker_ready': getattr(self, '_dl_ready', False),
            })

        @app.route(prefix + '/stream/<int:msg_id>')
        def storyhub_stream(msg_id):
            try:
                return self._serve_file(msg_id, request, inline=True)
            except Exception as e:
                import traceback
                logger.error("stream route error %d: %s\n%s", msg_id, e, traceback.format_exc())
                return jsonify({'error': str(e)}), 500

        @app.route(prefix + '/download/<int:msg_id>')
        def storyhub_download(msg_id):
            try:
                return self._serve_file(msg_id, request, inline=False)
            except Exception as e:
                import traceback
                logger.error("StoryHub serve error %d: %s\n%s", msg_id, e, traceback.format_exc())
                return jsonify({'error': str(e)}), 500

        @app.route(prefix + '/compressed/<int:msg_id>')
        def storyhub_compressed(msg_id):
            try:
                ep = self._find_episode_by_id(msg_id)
                ep_label = 'ep%s' % ep['ep_num'] if ep else str(msg_id)
                # If compressed_id exists, serve from Telegram
                if ep and ep.get('compressed_id'):
                    comp_id = ep['compressed_id']
                    path, fname = self.download_file(comp_id)
                    if path and os.path.exists(path):
                        from flask import Response
                        file_size = os.path.getsize(path)
                        def generate():
                            with open(path, 'rb') as f:
                                while True:
                                    chunk = f.read(262144)
                                    if not chunk:
                                        break
                                    yield chunk
                        return Response(generate(), mimetype='audio/ogg', headers={
                            'Content-Length': str(file_size),
                            'Accept-Ranges': 'bytes',
                            'Content-Disposition': 'attachment; filename="' + ep_label + '.m3a"',
                        })
                # Fallback: compress on-the-fly
                path, fname = self.download_file(msg_id)
                if not path or not os.path.exists(path):
                    return "Not found", 404
                compressed = _ensure_compressed(Path(path))
                if compressed and compressed.exists() and compressed != Path(path):
                    from flask import send_file
                    return send_file(str(compressed), mimetype='audio/ogg',
                                     as_attachment=True,
                                     download_name=ep_label + '.m3a')
                from flask import Response
                file_size = os.path.getsize(path)
                ctype = 'audio/mpeg'
                if fname.endswith('.m4a'):
                    ctype = 'audio/mp4'
                def generate():
                    with open(path, 'rb') as f:
                        while True:
                            chunk = f.read(262144)
                            if not chunk:
                                break
                            yield chunk
                return Response(generate(), mimetype=ctype, headers={
                    'Content-Length': str(file_size),
                    'Accept-Ranges': 'bytes',
                    'Content-Disposition': 'attachment; filename="' + ep_label + '.m3a"',
                })
            except Exception as e:
                import traceback
                logger.error("StoryHub compressed error %d: %s\n%s", msg_id, e, traceback.format_exc())
                return jsonify({'error': str(e)}), 500

        @app.route(prefix + '/status')
        def storyhub_status():
            return jsonify({
                'loaded': self._loaded,
                'total': len(self._episodes),
                'ep_start': self._episodes[0]['ep_num'] if self._episodes else 0,
                'ep_end': self._episodes[-1]['ep_num'] if self._episodes else 0,
            })

        @app.route(prefix + '/debug')
        def storyhub_debug():
            return jsonify(self.debug_info())

        @app.route(prefix + '/drive-map')
        def storyhub_drive_map():
            try:
                mp = json.loads((self.data_dir / 'drive_map.json').read_text())
            except Exception:
                mp = {}
            return jsonify(mp)

        @app.route(prefix + '/cleanup')
        def storyhub_cleanup():
            self._cleanup_downloads()
            files = list(self.download_dir.iterdir())
            total_size = sum(f.stat().st_size for f in files if f.is_file())
            return jsonify({
                'files': len(files),
                'size_mb': total_size // (1024*1024),
                'status': 'cleaned'
            })

        @app.route(prefix + '/retry')
        def storyhub_retry():
            self._last_error = None
            self._loaded = False
            self._episodes = []
            threading.Thread(target=self._init, daemon=True, name='storyhub_retry').start()
            return jsonify({'status': 'retrying'})

        @app.route(prefix + '/compress-all')
        def storyhub_compress_all():
            if self._compress_status.get('running'):
                return jsonify({'status': 'already_running', **self._compress_status})
            remaining = len([e for e in self._episodes if not e.get('compressed_id')])
            if remaining == 0:
                return jsonify({'status': 'all_done', 'total': len(self._episodes)})
            threading.Thread(target=self._compress_all, daemon=True, name='storyhub_compress').start()
            return jsonify({'status': 'started', 'remaining': remaining})

        @app.route(prefix + '/compress-status')
        def storyhub_compress_status():
            return jsonify(self._compress_status)

    def _find_episode_by_id(self, msg_id):
        for e in self._episodes:
            if e['id'] == msg_id:
                return e
        return None

    def _serve_file(self, msg_id, request, inline, force_name=None):
        import traceback
        from flask import Response
        try:
            path, fname = self.download_file(msg_id)
        except Exception as e:
            logger.error("StoryHub serve error %d: %s\n%s", msg_id, e, traceback.format_exc())
            return "Error: %s" % e, 500
        if not path or not os.path.exists(path):
            return "Not found", 404
        file_size = os.path.getsize(path)
        ctype = 'audio/mpeg'
        if fname.endswith('.m4a'):
            ctype = 'audio/mp4'
        elif fname.endswith('.ogg') or fname.endswith('.opus'):
            ctype = 'audio/ogg'
        if force_name:
            serve_name = force_name
        elif inline:
            serve_name = fname
        else:
            ep = self._find_episode_by_id(msg_id)
            if ep:
                serve_name = 'ep%d.m3a' % ep['ep_num']
            else:
                serve_name = fname
        disposition = 'inline' if inline else 'attachment'
        range_header = request.headers.get('Range')
        if range_header and inline:
            m = re.match(r'bytes=(\d+)-(\d*)', range_header)
            if m:
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else file_size - 1
                length = end - start + 1
                def generate():
                    with open(path, 'rb') as f:
                        f.seek(start)
                        remaining = length
                        while remaining > 0:
                            chunk = f.read(min(65536, remaining))
                            if not chunk:
                                break
                            remaining -= len(chunk)
                            yield chunk
                return Response(generate(), status=206, mimetype=ctype, headers={
                    'Content-Range': 'bytes %d-%d/%d' % (start, end, file_size),
                    'Accept-Ranges': 'bytes',
                    'Content-Length': str(length),
                    'Content-Disposition': '%s; filename="%s"' % (disposition, serve_name),
                })
        def generate_full():
            with open(path, 'rb') as f:
                while True:
                    chunk = f.read(262144)
                    if not chunk:
                        break
                    yield chunk
        return Response(generate_full(), mimetype=ctype, headers={
            'Content-Length': str(file_size),
            'Accept-Ranges': 'bytes',
            'Content-Disposition': '%s; filename="%s"' % (disposition, serve_name),
        })

    def _render_index(self, request):
        try:
            return self._render_index_inner(request)
        except Exception as e:
            import traceback
            tb = traceback.format_exc()
            logger.error("render error: %s\n%s", e, tb)
            return '<h1>Error</h1><pre>' + tb + '</pre>', 500

    def _render_index_inner(self, request):
        page = max(1, int(request.args.get('page', 1)))
        q = request.args.get('q', '').strip()
        per_page = 10
        MIN_EP = 800
        eps = [e for e in self._episodes if e['ep_num'] >= MIN_EP]
        if q:
            ql = q.lower()
            eps = [e for e in eps if ql in (e.get('title', '') + e.get('filename', '')).lower()]
        total = len(eps)
        total_pages = max(1, (total + per_page - 1) // per_page)
        page = min(page, total_pages)
        start = (page - 1) * per_page
        page_eps = eps[start:start + per_page]
        items = []
        for e in page_eps:
            epn = e['ep_num']
            eid = e['id']
            sz = e.get('size_mb', 0)
            items.append('<div class="st">Ep %d (%sMB)</div>' % (epn, sz))
            items.append('<a class="btn" href="%s/download/%d">DOWNLOAD</a>' % (self.prefix, eid))
            items.append('<a class="btn c" href="%s/compressed/%d">COMPRESSED</a>' % (self.prefix, eid))
        items_html = '\n'.join(items)
        nav = ''
        if page > 1:
            nav += '<a class="btn" href="?page=%d">&lt; PREV</a>' % (page - 1)
        nav += ' <b>%d/%d</b> ' % (page, total_pages)
        if page < total_pages:
            nav += '<a class="btn" href="?page=%d">NEXT &gt;</a>' % (page + 1)
        if not self._loaded:
            items_html = 'Loading...'
            nav = '<meta http-equiv="refresh" content="5">'
        html = '<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=128"><title>S</title><style>body{margin:0;padding:4px;background:#111;color:#fff;font:12px sans-serif}.st{color:#fc0;font-size:11px;margin:6px 0 2px}.btn{display:block;background:#333;color:#fff;text-decoration:none;padding:6px;text-align:center;margin:3px 0;border:1px solid #555}.btn:focus,.btn:hover{background:#fff;color:#000}hr{border:0;border-top:1px solid #333;margin:6px 0}b{font-size:11px}</style></head><body><div style="background:#e50914;padding:4px;text-align:center;font-weight:bold">STORIES</div><p style="font-size:10px;color:#aaa">%d ep</p><hr>%s<hr>%s</body></html>' % (total, nav, items_html)
        return html

