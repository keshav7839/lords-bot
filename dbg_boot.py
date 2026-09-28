"""Launcher: enables faulthandler (SIGUSR1 stack dump) then runs the bot."""
import faulthandler
import os
import signal
import sys
from pathlib import Path

faulthandler.register(signal.SIGUSR1)

bot_dir = Path(__file__).resolve().parent / "Anyyking"
os.chdir(str(bot_dir))
sys.path.insert(0, str(bot_dir))
sys.argv = ["anyybest.py"]
exec(compile((bot_dir / "anyybest.py").read_text(encoding="utf-8"), "anyybest.py", "exec"))
