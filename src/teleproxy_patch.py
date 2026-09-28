import os
import functools

_DEFAULT_PROXY = 'https://tgproxy-pages.pages.dev'


def apply():
    raw = os.environ.get('TG_API_PROXY', '') or _DEFAULT_PROXY
    proxy_base = raw.rstrip('/')

    try:
        import telegram
        import telegram.ext
        from telegram._utils.defaultvalue import DefaultValue as DV

        # ------------------------------------------------------------------ #
        # 1. Patch ApplicationBuilder.build()  (already worked)              #
        # ------------------------------------------------------------------ #
        orig_build = telegram.ext.ApplicationBuilder.build

        def patched_build(self):
            if isinstance(self._base_url, DV):
                self.base_url(proxy_base + '/bot')
            return orig_build(self)

        telegram.ext.ApplicationBuilder.build = patched_build

        # ------------------------------------------------------------------ #
        # 2. Patch Bot.__init__ — override default base_url / base_file_url  #
        # ------------------------------------------------------------------ #
        orig_bot_init = telegram.Bot.__init__

        @functools.wraps(orig_bot_init)
        def patched_bot_init(self, token, base_url="https://api.telegram.org/bot",
                             base_file_url="https://api.telegram.org/file/bot",
                             **kwargs):
            if base_url == "https://api.telegram.org/bot":
                base_url = proxy_base + '/bot'
            if base_file_url == "https://api.telegram.org/file/bot":
                base_file_url = proxy_base + '/file/bot'
            return orig_bot_init(self, token, base_url=base_url,
                                 base_file_url=base_file_url, **kwargs)

        telegram.Bot.__init__ = patched_bot_init

        # ------------------------------------------------------------------ #
        # 3. Patch ExtBot.__init__ — same treatment                         #
        # ------------------------------------------------------------------ #
        orig_extbot_init = telegram.ext.ExtBot.__init__

        @functools.wraps(orig_extbot_init)
        def patched_extbot_init(self, token,
                                base_url="https://api.telegram.org/bot",
                                base_file_url="https://api.telegram.org/file/bot",
                                **kwargs):
            if base_url == "https://api.telegram.org/bot":
                base_url = proxy_base + '/bot'
            if base_file_url == "https://api.telegram.org/file/bot":
                base_file_url = proxy_base + '/file/bot'
            return orig_extbot_init(self, token, base_url=base_url,
                                    base_file_url=base_file_url, **kwargs)

        telegram.ext.ExtBot.__init__ = patched_extbot_init

    except Exception as exc:
        import sys
        print("[teleproxy_patch] failed:", exc, file=sys.stderr)
