"""
Smart Model Tester — auto-discovers, tests, and ranks all AI models.

On startup: tests all models, removes dead ones, ranks by speed.
Periodically re-tests to catch models coming back online.

Integration: import and call `run_model_audit()` at bot startup.
"""

import time
import os
import json
import logging
import asyncio
from typing import Dict, List, Tuple, Optional
from dataclasses import dataclass, field, asdict
from pathlib import Path

logger = logging.getLogger("model_tester")

# ── Model test result ──
@dataclass
class ModelTest:
    provider: str
    model: str
    alive: bool = False
    latency: float = 99.0
    tokens: int = 0
    reply: str = ""
    error: str = ""
    tested_at: float = 0.0
    rank: int = 999

# ── Provider configs ──
PROVIDER_CONFIGS = {
    "groq": {
        "url": "https://api.groq.com/openai/v1/chat/completions",
        "models_url": "https://api.groq.com/openai/v1/models",
        "key_env": "GROQ_API_KEY",
        "key": "",
        "exclude_patterns": ["whisper", "safeguard", "prompt", "guard", "compound", "orpheus"],
    },
    "cerebras": {
        "url": "https://api.cerebras.ai/v1/chat/completions",
        "models_url": "https://api.cerebras.ai/v1/models",
        "key": "",
        "exclude_patterns": [],
    },
    "pollinations": {
        "url": "https://text.pollinations.ai/openai/chat/completions",
        "models_url": None,
        "key": None,
        "static_models": ["openai", "mistral"],
        "exclude_patterns": [],
    },
}

# ── Results cache ──
_results_file = Path(os.getenv("MODEL_TEST_RESULTS", "/tmp/model_test_results.json"))
_rankings: Dict[str, List[ModelTest]] = {}
_last_audit: float = 0.0
AUDIT_INTERVAL = 3600  # re-test every hour


async def _fetch_provider_models(provider: str, config: dict) -> List[str]:
    """Fetch available model list from a provider."""
    models_url = config.get("models_url")
    if not models_url:
        return config.get("static_models", [])

    key = config.get("key", "")
    if not key:
        return []

    try:
        r = await asyncio.to_thread(
            lambda: __import__("httpx").get(
                models_url,
                headers={"Authorization": f"Bearer {key}"},
                timeout=10,
            )
        )
        if r.status_code == 200:
            data = r.json().get("data", [])
            models = [m["id"] for m in data]
            # Filter excluded patterns
            exclude = config.get("exclude_patterns", [])
            models = [m for m in models if not any(x in m for x in exclude)]
            return models
        else:
            logger.warning(f"[model_tester] {provider}: list models HTTP {r.status_code}")
            return []
    except Exception as e:
        logger.warning(f"[model_tester] {provider}: list models failed: {e}")
        return []


async def _test_single_model(provider: str, config: dict, model: str) -> ModelTest:
    """Test a single model, return result."""
    result = ModelTest(provider=provider, model=model)
    key = config.get("key", "")
    url = config.get("url", "")

    if not url:
        result.error = "no url"
        return result

    headers = {"Content-Type": "application/json"}
    if key:
        headers["Authorization"] = f"Bearer {key}"

    payload = {
        "model": model,
        "messages": [{"role": "user", "content": "Say hi in 5 words"}],
        "max_tokens": 30,
    }

    t0 = time.time()
    try:
        r = await asyncio.to_thread(
            lambda: __import__("httpx").post(url, headers=headers, json=payload, timeout=15)
        )
        elapsed = round(time.time() - t0, 2)
        result.latency = elapsed
        result.tested_at = time.time()

        if r.status_code == 200:
            data = r.json()
            choice = data.get("choices", [{}])[0]
            msg = choice.get("message", {})
            content = msg.get("content", "")
            reasoning = msg.get("reasoning", "")
            tokens = data.get("usage", {}).get("total_tokens", 0)

            result.alive = True
            result.tokens = tokens
            result.reply = (content or reasoning or "")[:80]
        else:
            err = r.json().get("error", {}).get("message", r.text[:100]) if r.text else ""
            result.error = f"HTTP {r.status_code}: {err[:80]}"
    except Exception as e:
        elapsed = round(time.time() - t0, 2)
        result.latency = elapsed
        result.error = str(e)[:100]

    return result


async def test_all_models() -> Dict[str, List[ModelTest]]:
    """Test all models across all providers. Returns sorted results per provider."""
    global _rankings, _last_audit

    all_results: Dict[str, List[ModelTest]] = {}

    for provider, config in PROVIDER_CONFIGS.items():
        logger.info(f"[model_tester] Testing {provider}...")
        models = await _fetch_provider_models(provider, config)
        if not models:
            logger.warning(f"[model_tester] {provider}: no models found")
            continue

        logger.info(f"[model_tester] {provider}: testing {len(models)} models...")
        tasks = [_test_single_model(provider, config, m) for m in models]
        results = await asyncio.gather(*tasks)

        # Sort by latency (alive first)
        alive = sorted([r for r in results if r.alive], key=lambda x: x.latency)
        dead = [r for r in results if not r.alive]

        # Assign ranks
        for i, r in enumerate(alive):
            r.rank = i + 1

        all_results[provider] = alive + dead

        logger.info(f"[model_tester] {provider}: {len(alive)} alive, {len(dead)} dead")
        for r in alive:
            logger.info(f"  #{r.rank} {r.model}: {r.latency}s")

    _rankings = all_results
    _last_audit = time.time()

    # Save results
    _save_results(all_results)

    return all_results


def _save_results(results: Dict[str, List[ModelTest]]) -> None:
    """Save test results to file."""
    try:
        data = {}
        for provider, models in results.items():
            data[provider] = [asdict(m) for m in models]
        _results_file.write_text(json.dumps(data, indent=2))
    except Exception as e:
        logger.warning(f"[model_tester] Save failed: {e}")


def load_results() -> Dict[str, List[ModelTest]]:
    """Load cached test results."""
    global _rankings
    if _rankings:
        return _rankings

    try:
        if _results_file.exists():
            data = json.loads(_results_file.read_text())
            for provider, models in data.items():
                _rankings[provider] = [ModelTest(**m) for m in models]
            return _rankings
    except Exception:
        pass
    return {}


def get_fastest_models(provider: str = "groq", limit: int = 4) -> List[str]:
    """Get fastest working models for a provider."""
    results = load_results()
    provider_results = results.get(provider, [])
    alive = [r for r in provider_results if r.alive]
    alive.sort(key=lambda x: x.latency)
    return [r.model for r in alive[:limit]]


def get_alive_models(provider: str = "groq") -> List[ModelTest]:
    """Get all alive models for a provider, sorted by speed."""
    results = load_results()
    provider_results = results.get(provider, [])
    alive = [r for r in provider_results if r.alive]
    alive.sort(key=lambda x: x.latency)
    return alive


def get_dead_models(provider: str = "groq") -> List[ModelTest]:
    """Get all dead models for a provider."""
    results = load_results()
    return [r for r in results.get(provider, []) if not r.alive]


def should_reaudit() -> bool:
    """Check if it's time to re-test models."""
    return time.time() - _last_audit > AUDIT_INTERVAL


def get_status_report() -> str:
    """Get a human-readable status report."""
    results = load_results()
    if not results:
        return "No model test results yet. Run /testmodels first."

    lines = ["🤖 <b>Model Test Report</b>\n"]
    for provider, models in results.items():
        alive = [m for m in models if m.alive]
        dead = [m for m in models if not m.alive]
        lines.append(f"\n<b>{provider.upper()}</b>: {len(alive)} alive, {len(dead)} dead")

        if alive:
            lines.append("  Speed ranking:")
            for i, m in enumerate(alive, 1):
                lines.append(f"    #{i} <code>{m.model}</code>: {m.latency}s")

        if dead:
            lines.append("  Dead:")
            for m in dead:
                lines.append(f"    ❌ <code>{m.model}</code>: {m.error[:50]}")

    return "\n".join(lines)


async def run_model_audit() -> Dict[str, List[ModelTest]]:
    """Run full model audit. Called at bot startup."""
    logger.info("[model_tester] Starting model audit...")
    results = await test_all_models()

    # Log summary
    total_alive = sum(len([m for m in models if m.alive]) for models in results.values())
    total_dead = sum(len([m for m in models if not m.alive]) for models in results.values())
    logger.info(f"[model_tester] Audit complete: {total_alive} alive, {total_dead} dead")

    return results


# ── Auto-test on import ──
def init_model_tester():
    """Initialize model tester (load cached results)."""
    load_results()
    logger.info(f"[model_tester] Initialized with {sum(len(m) for m in _rankings.values())} cached results")
