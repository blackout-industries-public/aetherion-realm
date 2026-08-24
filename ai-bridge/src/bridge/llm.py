"""OpenAI-compatible client plus the admission control from BRD s26.

The scheduler exists because 1500 bots can generate far more chatter than any local
model can serve. Human-directed traffic must never queue behind bot small talk, so
low-priority work is dropped outright rather than delayed once capacity is gone.
"""
from __future__ import annotations

import asyncio
import json
import logging
import random
import re
import time
from collections import defaultdict
from enum import IntEnum

import httpx

from .config import settings

log = logging.getLogger("bridge.llm")


class Priority(IntEnum):
    DIRECT_WHISPER = 0
    PARTY = 1
    GUILD = 2
    EVENT_REACTION = 3
    AMBIENT = 4


# Anything at or below this always gets a slot; the rest yields under load.
PROTECTED = Priority.GUILD

# Models are told to answer as a player. Some still narrate or wrap in quotes, and a
# stray "As an AI" must never reach a player, so the output is scrubbed, not trusted.
_STRIP = re.compile(r"^\s*[\"'*]+|[\"'*]+\s*$")
# Harmony-format models leak their channel scaffolding into content. Observed
# live, whispered verbatim to a player: <|channel|>commentary to=final
# <|constrain|>json<|message|>{"text":"[GINVITE] test, you're in!"}. The words
# the character typed are inside the JSON envelope; everything else is the
# model talking to itself.
_SCAFFOLD_TOKEN = re.compile(r"<\|[^<>|]{1,24}\|>")
_SCAFFOLD_FRAMING = re.compile(
    r"\b(?:commentary\s+)?to=final\b|^\s*(?:json|commentary|analysis|assistantfinal)\b[: ]*",
    re.I)
_JSON_TEXT = re.compile(r"\{[^{}]*\"text\"[^{}]*\}")
# Models habitually prefix the speaker ("Sylindia: sure") or open with stage
# direction ("*whisper*: sure"). Both must go; only typed words reach the player.
_SPEAKER = re.compile(r"^\s*[A-Za-z][\w' -]{0,30}\s*:\s*")
_NARRATION = re.compile(r"\*[^*]{1,60}\*")
_THINK = re.compile(r"<think>.*?</think>", re.DOTALL | re.IGNORECASE)
_REFUSAL = re.compile(r"\b(as an ai|language model|i'?m an ai|cannot assist)\b", re.I)

# Reasoning leaked into `content` instead of `reasoning_content`. Observed live:
# "The user is roleplaying a battleground scenario. I need to respond as Soldeyn..."
# published straight into /say. Truncating such a reply just yields shorter garbage,
# so anything matching is rejected outright and becomes a canned fallback.
_META = re.compile(
    r"\b(the (user|player) (is|wants|asked)"
    r"|i (need|should|have) to (respond|reply|answer|say)"
    r"|i(?:'| a)m (?:being )?asked"
    r"|as (?:the )?\w+,? a (?:cheerful|grumpy|blunt|dry|weary|cocky|wary|earnest)"
    r"|this is a common"
    r"|let me (think|respond|reply)"
    r"|my (response|reply) should"
    r"|in character[,:]? i"
    r"|\banalysis\b|\bwe need to\b)", re.I)

# A typed chat line is short. Anything this long is a leaked monologue, not speech.
_HARD_MAX = 400


class Pacing:
    """Ambient chatter comes in waves, not on a metronome.

    A fixed interval reads as machinery the moment you watch it for a minute. Real
    chat has lulls and flurries, so the gap between unprompted lines is drawn from a
    phase that itself changes every few minutes.
    """

    # (name, min_gap, max_gap, min_phase_s, max_phase_s, weight)
    PHASES = [
        ("quiet",  35.0, 80.0, 180, 420, 30),
        ("normal", 10.0, 26.0, 300, 700, 50),
        ("busy",    3.0,  9.0,  60, 240, 20),
    ]

    def __init__(self) -> None:
        self.phase = "normal"
        self._until = 0.0
        self._gap = 15.0

    def _reroll(self, now: float) -> None:
        names = [p[0] for p in self.PHASES]
        weights = [p[5] for p in self.PHASES]
        self.phase = random.choices(names, weights=weights)[0]
        spec = next(p for p in self.PHASES if p[0] == self.phase)
        self._until = now + random.uniform(spec[3], spec[4])

    def gap(self, now: float) -> float:
        if now >= self._until:
            self._reroll(now)
        spec = next(p for p in self.PHASES if p[0] == self.phase)
        # Fresh draw per line, so even within a phase it is not evenly spaced.
        return random.uniform(spec[1], spec[2])


class Dropped(Exception):
    """Rejected before inference: no capacity, or the bot is on cooldown."""


class Scheduler:
    def __init__(self) -> None:
        self._sem = asyncio.Semaphore(settings.max_concurrent)
        self._last_bot: dict[int, float] = defaultdict(float)
        self._last_ambient = 0.0
        self._next_ambient_gap = 0.0
        self.pacing = Pacing()
        self.stats: dict[str, int] = defaultdict(int)

    def admit(self, bot_guid: int, priority: Priority) -> None:
        now = time.monotonic()
        if priority >= Priority.AMBIENT:
            if now - self._last_ambient < self._next_ambient_gap:
                self.stats["dropped_ambient_rate"] += 1
                raise Dropped("ambient rate limit")
            # Claim the slot at admission, not on success: a failed call still consumed
            # capacity, and retrying immediately would defeat the limit.
            self._last_ambient = now
            self._next_ambient_gap = max(settings.ambient_min_interval,
                                         self.pacing.gap(now))
            self.stats[f"phase_{self.pacing.phase}"] += 1

        if priority > PROTECTED:
            if now - self._last_bot[bot_guid] < settings.per_bot_cooldown:
                self.stats["dropped_bot_cooldown"] += 1
                raise Dropped("per-bot cooldown")
            if self._sem.locked():
                self.stats["dropped_saturated"] += 1
                raise Dropped("inference saturated")
        self._last_bot[bot_guid] = now

    def slot(self) -> asyncio.Semaphore:
        return self._sem


class LLM:
    def __init__(self) -> None:
        self._client: httpx.AsyncClient | None = None
        self.scheduler = Scheduler()
        self.latencies: list[float] = []

    async def start(self) -> None:
        self._client = httpx.AsyncClient(
            base_url=settings.llm_base_url,
            headers={"Authorization": f"Bearer {settings.llm_api_key}"},
            timeout=httpx.Timeout(settings.timeout_ambient),
        )

    async def close(self) -> None:
        if self._client:
            await self._client.aclose()

    async def models(self) -> list[str]:
        assert self._client is not None
        r = await self._client.get("/models", timeout=5.0)
        r.raise_for_status()
        return [m["id"] for m in r.json().get("data", [])]

    async def complete(self, messages: list[dict[str, str]], *, priority: Priority,
                       bot_guid: int, speaker_name: str = "",
                       bypass_admission: bool = False) -> str:
        assert self._client is not None, "LLM.start() was not awaited"
        # A retry is not new traffic, it is the completion of work already admitted.
        # Re-admitting it means the rate limit the first attempt just consumed drops
        # the retry, so a rejected reply could never recover.
        if not bypass_admission:
            self.scheduler.admit(bot_guid, priority)

        # Anything a human is waiting on gets the low-latency model; rate-limited
        # background chatter can afford a slower, more characterful one.
        model = (settings.model_interactive if priority <= Priority.PARTY
                 else settings.model_background)
        deadline = (settings.timeout_direct if priority <= Priority.PARTY
                    else settings.timeout_ambient)

        started = time.monotonic()
        async with self.scheduler.slot():
            r = await self._client.post(
                "/chat/completions",
                json={
                    "model": model,
                    "messages": messages,
                    "temperature": 0.85,
                    # Must be generous, not tight. These models spend the budget on
                    # hidden reasoning first; starve it and `content` comes back empty
                    # with finish_reason=length. The reply length is controlled by the
                    # prompt and by sanitize(), not by this.
                    "max_tokens": 800,
                    # gpt-oss honours this and it is the single biggest latency lever:
                    # ~6.5s -> ~1.5s. Models that do not support it ignore it.
                    "reasoning_effort": settings.reasoning_effort,
                    "stream": False,
                },
                timeout=deadline,
            )
        if r.status_code >= 400:
            # The backend explains rejections in the body; raise_for_status throws it
            # away, which turns a one-line fix into guesswork.
            log.error("backend %s: %s", r.status_code, r.text[:400])
        r.raise_for_status()
        self.latencies.append(time.monotonic() - started)
        del self.latencies[:-500]

        message = r.json()["choices"][0]["message"]
        raw = message.get("content") or ""
        cleaned = self.sanitize(raw, speaker_name)
        if not cleaned:
            # Distinguish "model said nothing" from "sanitiser ate it" - the two have
            # completely different fixes, and without the raw text neither is diagnosable.
            log.warning("empty after sanitize: raw=%r reasoning_chars=%d",
                        raw[:200], len(message.get("reasoning_content") or ""))
        return cleaned

    @staticmethod
    def sanitize(text: str, speaker_name: str = "") -> str:
        # The framing tic also arrives bare, with no tokens at all - observed
        # live: 'json [GINVITE] you got it, welcome aboard.' No character's
        # line starts with the word json.
        text = re.sub(r"^\s*(?:json|assistantfinal)\b[:,]?\s*", "", text, flags=re.I)
        if "<|" in text or text.lstrip().startswith("{"):
            # Prefer the JSON envelope's own "text" - it is the actual line.
            last = None
            for last in _JSON_TEXT.finditer(text):
                pass
            if last is not None:
                try:
                    inner = json.loads(last.group(0)).get("text")
                except ValueError:
                    inner = None
                if isinstance(inner, str) and inner.strip():
                    text = inner
            text = _SCAFFOLD_TOKEN.sub(" ", text)
            text = _SCAFFOLD_FRAMING.sub("", text)
            if "<|" in text:
                return ""   # scrubbing failed; a fallback beats leaked markup
        text = _THINK.sub("", text)                 # reasoning models leak scratchpads
        text = _NARRATION.sub("", text)
        text = " ".join(text.split())
        text = _SPEAKER.sub("", text, count=1)
        text = _STRIP.sub("", text).strip()
        if _REFUSAL.search(text) or _META.search(text):
            return ""
        if len(text) > _HARD_MAX:
            return ""
        if len(text) > settings.max_reply_chars:
            cut = text[: settings.max_reply_chars]
            text = cut.rsplit(" ", 1)[0] + "..."
        return text
