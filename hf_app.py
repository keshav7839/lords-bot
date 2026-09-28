import os, sys, threading, logging, time, urllib.request, ssl

import bot_core
bot_core.start_all()

# Write StoryHub session string to /data if available
_session_src = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'storyhub_session.txt')
_session_dst = '/data/storyhub_session.txt'
try:
    os.makedirs('/data', exist_ok=True)
    if os.path.exists(_session_src):
        import shutil
        shutil.copy2(_session_src, _session_dst)
        bot_core.logger.info("Wrote StoryHub session to %s (%d bytes)", _session_dst, os.path.getsize(_session_dst))
    else:
        bot_core.logger.warning("Session source not found: %s", _session_src)
except Exception as e:
    bot_core.logger.error("Session copy failed: %s", e)

try:
    from flask import Flask, jsonify
    app = Flask(__name__)

    # Mount StoryHub at /storyhub/
    try:
        from storyhub import StoryHub
        storyhub = StoryHub()
        storyhub.register_routes(app, prefix='/storyhub')
        storyhub.start()
        bot_core.logger.info("StoryHub mounted at /storyhub/")
    except Exception as e:
        bot_core.logger.error("StoryHub init failed: %s", e)

    @app.after_request
    def add_cors(response):
        response.headers['Access-Control-Allow-Origin'] = '*'
        response.headers['Access-Control-Allow-Headers'] = 'Content-Type'
        response.headers['Access-Control-Allow-Methods'] = 'GET, OPTIONS'
        return response

    @app.route('/')
    def home():
        return f'HOSTING BOT [{bot_core.SPACE_NAME}] is running 24/7 | <a href="/storyhub/">StoryHub</a>'

    @app.route('/health')
    def health():
        return jsonify(**bot_core.fleet_health())

    @app.route('/debug')
    def debug():
        return jsonify(**bot_core.debug_state())

    @app.route('/download-app')
    def download_app():
        apk_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'storyhub-static', 'storyhub.apk')
        if os.path.exists(apk_path):
            from flask import send_file
            return send_file(apk_path, as_attachment=True, download_name='StoryHub.apk')
        return 'APK not found', 404

    @app.route('/download-storyxhub')
    def download_storyxhub():
        apk_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'storyhub-static', 'storyxhub.apk')
        if os.path.exists(apk_path):
            from flask import send_file
            return send_file(apk_path, as_attachment=True, download_name='Storyxhub.apk')
        return 'APK not found', 404

    @app.route('/storyhub-static/')
    def storyhub_static():
        html_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'storyhub-static', 'index.html')
        if os.path.exists(html_path):
            from flask import send_file
            return send_file(html_path)
        return 'Not found', 404

    def keep_alive_self():
        """Inbuilt uptime robot: periodically hit our own health endpoint so the
        Space never looks idle to HF's auto-sleep watchdog."""
        public = bot_core.HF_SPACE_URL.rstrip('/')
        port = int(os.environ.get('PORT', 7860))
        targets = ['http://127.0.0.1:%d/health' % port, public + '/health']
        ok = 0
        while True:
            for target in targets:
                try:
                    urllib.request.urlopen(target, timeout=15).read()
                    ok += 1
                except Exception as exc:
                    ok = 0
                    bot_core.logger.error("keep-alive ping %s FAILED: %s", target, exc)
            time.sleep(60)

    threading.Thread(target=keep_alive_self, daemon=True).start()
    port = int(os.environ.get('PORT', 7860))
    bot_core.logger.info("Health server + internal keep-alive on port %s", port)
    app.run(host='0.0.0.0', port=port)
except ImportError:
    import http.server
    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            self.send_response(200)
            self.end_headers()
            self.wfile.write(b'HOSTING BOT is running 24/7')
        def log_message(self, *a):
            pass
    port = int(os.environ.get('PORT', 7860))
    http.server.ThreadingHTTPServer(('0.0.0.0', port), H).serve_forever()
