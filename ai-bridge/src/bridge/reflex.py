"""Canned responses for chat that does not deserve inference.

A 20B model spending 3 seconds to produce "grats" is the worst trade in the system,
and reflex traffic is a large share of everything bots say. Matching it here frees
the whole inference budget for conversation that actually matters (BRD s26).

Deliberately conservative: anything that is not obviously reflex falls through to the
model. A wrong canned answer is far more damaging than a missed saving.
"""
from __future__ import annotations

import hashlib
import re

# Matched against the full message, lowercased and stripped of punctuation. Anchored
# on both ends on purpose - "thanks" is reflex, "thanks, but what about the boss?"
# is a real question.
_REFLEX: list[tuple[re.Pattern, tuple[str, ...]]] = [
    (re.compile(r"^(hi|hey|hello|yo|sup|hiya)$"),
     ("hey", "yo", "hi", "sup")),
    (re.compile(r"^(gz|gratz|grats|congrats|congratulations)$"),
     ("ty", "thanks", "cheers")),
    (re.compile(r"^(ty|thx|thanks|thank you|tyvm)$"),
     ("np", "anytime", "no worries", "sure")),
    (re.compile(r"^(lol|lmao|haha|hah|rofl|xd)$"),
     ("heh", "lol", "ha")),
    (re.compile(r"^(ok|okay|k|kk|sure|alright|aight)$"),
     ("k", "aye", "sure")),
    (re.compile(r"^(bye|cya|gtg|night|gn|later)$"),
     ("cya", "later", "o/")),
    (re.compile(r"^(afk|brb|one sec|sec)$"),
     ("k", "np")),
    (re.compile(r"^(rip|f|oof)$"),
     ("rip", "oof", "happens")),
    (re.compile(r"^(wb|welcome back|welcome)$"),
     ("ty", "thanks", "o/")),
    (re.compile(r"^(gl|good luck|glhf|hf)$"),
     ("u2", "ty", "same")),
    (re.compile(r"^(inv|invite|inv me|inv pls|invite pls)$"),
     ("sec", "sure", "one sec")),
    (re.compile(r"^(wts|wtb|wtt)$"),
     ("what", "?", "link it")),
    (re.compile(r"^(where|where\?|where are you|w8|wait)$"),
     ("sec", "coming", "otw")),
    (re.compile(r"^(y|yes|yep|yeah|ya|yup)$"),
     ("k", "aye", "ok")),
    (re.compile(r"^(n|no|nope|nah)$"),
     ("k", "fair", "np")),
    (re.compile(r"^(sup|wassup|whats up|how are you|hows it going)$"),
     ("grinding", "same as always", "questing")),
    (re.compile(r"^(pst|whisper me|w me)$"),
     ("k", "sec")),
    (re.compile(r"^(omw|otw|coming|on my way)$"),
     ("k", "ty", "waiting")),
]

# Reactions to world events. Most level-ups deserve "grats", not a considered opinion.
_EVENT: dict[str, tuple[str, ...]] = {
    "levelup": ("grats", "gz", "grats!", "nice, gz"),
    "died": ("rip", "oof", "you good?", "ouch"),
    "achievement": ("gz", "nice one", "grats"),
    "loot": ("nice drop", "grats on that", "lucky"),
}

_PUNCT = re.compile(r"[^\w\s]")


def _pick(options: tuple[str, ...], seed: str) -> str:
    # Deterministic per (bot, message) rather than random: the same bot answering the
    # same thing twice should not visibly reroll, and it keeps tests reproducible.
    idx = hashlib.sha256(seed.encode()).digest()[0] % len(options)
    return options[idx]


def canned_chat(message: str, seed: str) -> str | None:
    normalised = _PUNCT.sub("", message.strip().lower())
    if not normalised or len(normalised) > 24:
        return None
    for pattern, options in _REFLEX:
        if pattern.match(normalised):
            return _pick(options, seed + normalised)
    return None


def canned_event(event_type: str, seed: str) -> str | None:
    options = _EVENT.get(event_type)
    return _pick(options, seed + event_type) if options else None
