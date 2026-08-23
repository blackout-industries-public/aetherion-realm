"""Runtime configuration. Everything is env-driven so the same image runs against
LM Studio today and Bifrost later without a rebuild."""
from __future__ import annotations

import os
from dataclasses import dataclass, field


def _str(name: str, default: str) -> str:
    """Treat an empty env var as unset.

    Compose writes `VAR: ${VAR:-}` as an empty string rather than omitting it, and a
    bare os.getenv would then shadow the verified default with "".
    """
    return os.getenv(name) or default


def _int(name: str, default: int) -> int:
    return int(os.getenv(name) or default)


def _float(name: str, default: float) -> float:
    return float(os.getenv(name) or default)


@dataclass(frozen=True)
class Settings:
    # The bridge only ever speaks OpenAI-compatible HTTP. LM Studio serves that
    # directly; Bifrost serves the same shape, so swapping is a URL change.
    llm_base_url: str = _str("LLM_BASE_URL", "http://10.10.42.46:1234/v1")
    llm_api_key: str = _str("LLM_API_KEY", "not-needed")

    # BRD s30 splits models by cost; on this hardware the split that matters is
    # latency. INTERACTIVE serves anything a human is waiting on and must be fast.
    # BACKGROUND serves rate-limited chatter where 10s is invisible, so it can be a
    # slower model with more character.
    #
    # Measured here: gpt-oss-20b 1.6s with zero reasoning tokens; qwen3.8-27b 11.7s;
    # qwen3.6-a3b and gemma-4-26b return EMPTY content (all budget spent reasoning).
    # Verify reasoning_tokens == 0 and non-empty content before adopting a model.
    model_interactive: str = _str(
        "MODEL_INTERACTIVE", "openai-gpt-oss-20b-claude-4.5-opus-heretic-uncensored-i1")
    model_background: str = _str(
        "MODEL_BACKGROUND", "openai-gpt-oss-20b-claude-4.5-opus-heretic-uncensored-i1")

    # OpenAI-compatible field; LM Studio maps it onto its own per-model setting and
    # only accepts low/medium/high here ("off" is rejected with HTTP 400).
    reasoning_effort: str = _str("REASONING_EFFORT", "low")

    db_host: str = _str("DB_HOST", "ac-database")
    db_port: int = _int("DB_PORT", 3306)
    db_user: str = _str("DB_USER", "root")
    db_password: str = _str("DB_PASSWORD", "")
    db_characters: str = _str("DB_CHARACTERS", "acore_characters")

    memory_path: str = _str("MEMORY_PATH", "/data/memory.sqlite")

    # BRD s27. A reply that arrives after its moment has passed is worthless, so
    # these are hard deadlines, not targets: past them the request is abandoned.
    # The BRD wants sub-3s direct replies. A 20B model on a laptop measures ~5-6s, so
    # these are set to what the hardware actually does; the gap is real and tracked.
    timeout_direct: float = _float("TIMEOUT_DIRECT", 20.0)
    timeout_ambient: float = _float("TIMEOUT_AMBIENT", 45.0)

    # BRD s26. Concurrency is capped so a burst of bot chatter can never starve
    # the one thing that matters: a human waiting on a reply.
    # Measured on this host: aggregate throughput rises to ~160 tok/s at 8 concurrent
    # while per-request latency barely moves (3.3s at 4, 3.4s at 8), so 8 is free.
    max_concurrent: int = _int("MAX_CONCURRENT", 8)
    max_reply_chars: int = _int("MAX_REPLY_CHARS", 240)

    # Paces unprompted openers only. Loosened from 60s because at 1500 bots the world
    # read as empty; the saturation and priority checks are what actually protect
    # human-facing latency, not this interval.
    ambient_min_interval: float = _float("AMBIENT_MIN_INTERVAL", 12.0)
    per_bot_cooldown: float = _float("PER_BOT_COOLDOWN", 3.0)

    history_turns: int = _int("HISTORY_TURNS", 8)

    # Activity recorder. The game schema keeps no history, so state is sampled and
    # only changes are written - a row per bot per tick would be almost all noise.
    history_enabled: bool = _str("HISTORY_ENABLED", "1") not in ("0", "false", "no")
    history_interval: float = _float("HISTORY_INTERVAL", 60.0)
    history_retention_days: int = _int("HISTORY_RETENTION_DAYS", 7)
    # Uncommon and above. Below this the feed is buried in soul shards and grey
    # vendor trash from 2500 characters looting continuously.
    loot_min_quality: int = _int("LOOT_MIN_QUALITY", 2)


settings = Settings()
