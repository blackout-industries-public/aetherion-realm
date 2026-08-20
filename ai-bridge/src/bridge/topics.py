"""Ambient topic selection.

The first version asked every bot the same question and got the same shape of answer
back, which reads as generated. Real chat is mostly people selling things, advertising
guilds and looking for groups - so topics are weighted toward those and grounded in
real character state wherever possible.

Topic choice lives here rather than in the C++ hook so it can be retuned without
rebuilding the worldserver.
"""
from __future__ import annotations

import random

# (topic, weight). Trade and recruitment dominate deliberately: that is what a
# populated city channel actually looks like.
_WEIGHTS: list[tuple[str, int]] = [
    ("selling", 26),
    ("buying", 12),
    ("guild_promo", 16),
    ("lfg", 18),
    ("activity", 14),
    ("complaint", 8),
    ("boast", 6),
]

_PROMPTS = {
    "selling": (
        "You are trying to sell an item. Refer to it as exactly [[ITEM]] - write that "
        "token verbatim, do not replace it with a name. Advertise it in one short line "
        "the way players actually type in trade chat, with a price. Example shape: "
        "'WTS [[ITEM]] 40g pst'."),
    "buying": (
        "You want to buy something you need at level {level}. Ask for it in one short "
        "line, the way players type in trade chat."),
    "guild_promo": (
        "You are recruiting for your guild <{guild}>{size}. Advertise it in one short "
        "line, the way a real recruitment spam line reads."),
    "lfg": (
        "You are looking for a group for content appropriate to level {level}. Post one "
        "short LFG line using normal abbreviations."),
    "activity": (
        "Say one short line about what you are doing right now at level {level}."),
    "complaint": (
        "Complain in one short line about something mundane - repair costs, drop rates, "
        "a wipe, prices."),
    "boast": (
        "Brag briefly about something you just did. One short line."),
}

# Without a guild there is nothing to promote, and without an item nothing to sell.
_REQUIRES = {"guild_promo": "guild", "selling": "item"}


def choose(*, has_guild: bool, has_item: bool) -> str:
    pool = [
        (topic, weight) for topic, weight in _WEIGHTS
        if not (_REQUIRES.get(topic) == "guild" and not has_guild)
        and not (_REQUIRES.get(topic) == "item" and not has_item)
    ]
    total = sum(w for _, w in pool)
    roll = random.uniform(0, total)
    upto = 0.0
    for topic, weight in pool:
        upto += weight
        if roll <= upto:
            return topic
    return pool[-1][0]


def render(topic: str, *, level: int, guild: str | None, item: str | None,
           guild_size: int = 0) -> str:
    size = f", currently {guild_size} members" if guild_size else ""
    return _PROMPTS[topic].format(
        level=level, guild=guild or "", item=item or "something", size=size)
