import json
import os
import time
import asyncio
from pathlib import Path

DATA_DIR = Path(__file__).parent.parent / "data"
EMPIRE_FILE = DATA_DIR / "empires.json"

async def init_db():
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    if not EMPIRE_FILE.exists():
        with open(EMPIRE_FILE, 'w') as f:
            json.dump({}, f)

async def get_empire(user_id: int):
    try:
        with open(EMPIRE_FILE) as f:
            data = json.load(f)
        return data.get(str(user_id))
    except Exception:
        pass
    return None

async def create_empire(user_id: int, name: str):
    try:
        with open(EMPIRE_FILE) as f:
            data = json.load(f)
        data[str(user_id)] = {"user_id": user_id, "name": name, "kingdom_level": 1, "treasury": 1000, "vault": 0, "shield_hours": 0}
        with open(EMPIRE_FILE, 'w') as f:
            json.dump(data, f, indent=2)
    except Exception:
        pass

async def get_empire_status(empire: dict):
    level = empire.get("kingdom_level", 1)
    treasury = empire.get("treasury", 1000)
    vault = empire.get("vault", 0)
    shield = empire.get("shield_hours", 0)
    return (
        f"🏰 <b>Empire Overview</b>\n\n"
        f"👑 Kingdom Level: <b>{level}</b>\n"
        f"💰 Treasury: <b>{treasury:,}</b> gold\n"
        f"🏦 Vault: <b>{vault:,}</b> gold\n"
        f"🛡 Shield: <b>{shield}h</b> remaining\n\n"
        f"🏗 <b>Build</b>: /build\n"
        f"⛏ <b>Mine</b>: /mine list\n"
        f"⚔️ <b>Train</b>: /train barbarian 10\n"
        f"🕵️ <b>Spy</b>: /spy @user\n"
        f"🔬 <b>Research</b>: /research"
    )

async def collect_all_mines(user_id: int):
    try:
        empire = await get_empire(user_id)
        if empire:
            gold = empire.get("kingdom_level", 1) * 50
            empire["treasury"] += gold
            with open(EMPIRE_FILE, 'w') as f:
                json.dump(await _load_data(), f, indent=2)
            return {"gold": gold}
    except Exception:
        pass
    return {}

async def _load_data():
    try:
        with open(EMPIRE_FILE) as f:
            return json.load(f)
    except Exception:
        return {}

async def settle_to_vault(user_id: int, amount: int):
    try:
        data = await _load_data()
        empire = data.get(str(user_id))
        if empire and empire.get("treasury", 0) >= amount:
            fee = int(amount * 0.05)
            empire["treasury"] -= amount
            empire["vault"] += amount - fee
            with open(EMPIRE_FILE, 'w') as f:
                json.dump(data, f, indent=2)
            return amount - fee, fee
    except Exception:
        pass
    return None, None

# Initialize on import
asyncio.ensure_future(init_db())
