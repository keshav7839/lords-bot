"""
Sticker Fetcher — fetch Telegram sticker packs → local cache + TiDB pool.

Usage (owner only):
  /fetchsticker kiss KissMe      → fetch pack "KissMe" into kiss pool
  /fetchsticker slap SlapPack    → fetch pack "SlapPack" into slap pool
  /fetchsticker random AnimeXOXO → fetch into random pool
  /loadstickers kiss             → reload kiss pool from local cache
  /stickerstats                  → show all pools + local counts

Algorithm (like @im_bakabot):
  1. Fetch sticker set via Telethon GetStickerSetRequest
  2. For each sticker: extract file_id, download .webp to local
  3. Store file_ids in TiDB pool (gif_{action})
  4. When serving: pick random from local files, send via file_id
  5. If local missing → fall back to TiDB file_id → re-download on next access
"""

import os
import json
import random
import time
import logging
import asyncio
from pathlib import Path
from typing import Optional

logger = logging.getLogger("sticker_fetcher")

# ── Paths ──
STICKER_BASE = Path(os.getenv(
    "STICKER_CACHE_DIR",
    "/sdcard/Download/Telegram/Anyyking/stickers"
))

# ── Default sticker packs per action ──
DEFAULT_PACKS = {
    "kiss":   ["Kissme1", "Lovekiss_sticker", "hugkiss"],
    "slap":   ["SlapStickers", "SlapPack"],
    "hug":    ["HugsandKissesSPACK", "AnimeHugs"],
    "smash":  ["SmashStickers"],
    "bites":  ["BiteStickers"],
    "fetch":  ["AnimeXOXO"],
    "random": ["AnimeXOXO"],
    "love":   ["LoveStickers", "LovePack"],
    "pat":    ["PatStickers"],
    "wave":   ["WaveStickers"],
}

# ── Telegram API constants ──
TG_API_ID = int(os.getenv("TG_API_ID", "0"))
TG_API_HASH = os.getenv("TG_API_HASH", "")
TG_SESSION = os.getenv("TG_SESSION_PATH", "/sdcard/session/keshavvee_fresh.session.txt")


def _get_action_dir(action: str) -> Path:
    """Get/create local cache directory for an action."""
    d = STICKER_BASE / action.lower()
    d.mkdir(parents=True, exist_ok=True)
    return d


def _load_local_index(action: str) -> dict:
    """Load local sticker index (file_id → local_path mapping)."""
    idx_path = _get_action_dir(action) / "_index.json"
    if idx_path.exists():
        try:
            return json.loads(idx_path.read_text())
        except Exception:
            pass
    return {}


def _save_local_index(action: str, index: dict) -> None:
    """Save local sticker index."""
    idx_path = _get_action_dir(action) / "_index.json"
    idx_path.write_text(json.dumps(index, indent=2))


async def fetch_sticker_pack(
    pack_short_name: str,
    action: str,
    owner_id: int = 0,
) -> dict:
    """
    Fetch a Telegram sticker pack and cache locally.
    Returns: {"ok": True, "count": N, "action": "kiss", "pack": "PackName"}
    """
    if not TG_API_ID or not TG_API_HASH:
        return {"ok": False, "error": "TG_API_ID / TG_API_HASH not set"}

    try:
        from telethon import TelegramClient
        from telethon.tl.functions.messages import GetStickerSetRequest
        from telethon.tl.types import InputStickerSetShortName
    except ImportError:
        return {"ok": False, "error": "telethon not installed"}

    session_path = TG_SESSION
    if not os.path.exists(session_path):
        return {"ok": False, "error": f"Session not found: {session_path}"}

    client = TelegramClient(session_path, TG_API_ID, TG_API_HASH)

    try:
        await client.start()
        sticker_set = await client(GetStickerSetRequest(
            stickerset=InputStickerSetShortName(short_name=pack_short_name),
            hash=0
        ))
    except Exception as e:
        return {"ok": False, "error": f"Failed to fetch pack: {e}"}
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    documents = sticker_set.documents
    if not documents:
        return {"ok": False, "error": "Pack is empty"}

    action_dir = _get_action_dir(action)
    index = _load_local_index(action)
    count = 0

    for doc in documents:
        file_id = str(doc.id)
        if file_id in index:
            continue  # already cached

        # Download sticker to local
        local_name = f"{pack_short_name}_{file_id}.webp"
        local_path = action_dir / local_name

        try:
            # Reconnect for download
            dl_client = TelegramClient(session_path, TG_API_ID, TG_API_HASH)
            await dl_client.start()
            await dl_client.download_media(doc, str(local_path))
            await dl_client.disconnect()
        except Exception as e:
            logger.warning(f"Download failed for {file_id}: {e}")
            local_path = None

        index[file_id] = {
            "file_id": file_id,
            "access_hash": str(doc.access_hash),
            "pack": pack_short_name,
            "local_path": str(local_path) if local_path and local_path.exists() else None,
            "cached_at": int(time.time()),
        }
        count += 1

    _save_local_index(action, index)

    # Update TiDB pool
    try:
        from anyybest import get_sticker_setting, save_sticker_setting
        data = await asyncio.to_thread(get_sticker_setting, f"gif_{action}") or {}
        pool = data.get("pool", [])
        existing = set(pool)

        for file_id in index:
            if file_id not in existing:
                pool.append(file_id)

        data["pool"] = pool
        if pool:
            data["file_id"] = pool[-1]
        data["packs"] = list(set(data.get("packs", []) + [pack_short_name]))
        await asyncio.to_thread(save_sticker_setting, f"gif_{action}", data)
    except Exception as e:
        logger.warning(f"TiDB update failed: {e}")

    return {
        "ok": True,
        "count": count,
        "total": len(index),
        "action": action,
        "pack": pack_short_name,
    }


async def reload_from_local(action: str) -> dict:
    """Reload sticker pool from local cache files."""
    index = _load_local_index(action)
    if not index:
        return {"ok": False, "error": "No local stickers found"}

    # Verify files still exist
    valid = {}
    for file_id, info in index.items():
        lp = info.get("local_path")
        if lp and os.path.exists(lp):
            valid[file_id] = info

    _save_local_index(action, valid)

    # Update TiDB pool
    try:
        from anyybest import get_sticker_setting, save_sticker_setting
        data = await asyncio.to_thread(get_sticker_setting, f"gif_{action}") or {}
        data["pool"] = list(valid.keys())
        if valid:
            data["file_id"] = list(valid.keys())[-1]
        await asyncio.to_thread(save_sticker_setting, f"gif_{action}", data)
    except Exception as e:
        logger.warning(f"TiDB update failed: {e}")

    return {"ok": True, "count": len(valid), "action": action}


def get_local_sticker(action: str) -> Optional[str]:
    """Pick a random local sticker file_id for an action."""
    index = _load_local_index(action)
    if not index:
        return None

    valid = {fid: info for fid, info in index.items()
             if info.get("local_path") and os.path.exists(info["local_path"])}
    if not valid:
        return None

    return random.choice(list(valid.keys()))


def get_sticker_stats() -> dict:
    """Get stats for all sticker pools."""
    stats = {}
    for action_dir in STICKER_BASE.iterdir():
        if action_dir.is_dir() and not action_dir.name.startswith("_"):
            index = _load_local_index(action_dir.name)
            local_count = sum(
                1 for info in index.values()
                if info.get("local_path") and os.path.exists(info["local_path"])
            )
            stats[action_dir.name] = {
                "total_indexed": len(index),
                "local_cached": local_count,
                "packs": list(set(info.get("pack", "") for info in index.values())),
            }
    return stats


def batch_fetch_sync(packs: list[tuple[str, str]]) -> list[dict]:
    """
    Batch fetch multiple packs. Each item: (pack_short_name, action).
    Runs synchronously for use in thread pool.
    """
    results = []
    for pack_name, action in packs:
        try:
            loop = asyncio.new_event_loop()
            result = loop.run_until_complete(
                fetch_sticker_pack(pack_name, action)
            )
            loop.close()
            results.append(result)
        except Exception as e:
            results.append({"ok": False, "error": str(e), "pack": pack_name})
    return results
