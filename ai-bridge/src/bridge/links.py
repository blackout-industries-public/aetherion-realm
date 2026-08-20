"""Client-side hyperlinks.

A bot that types an item's name reads as generated text; a bot that posts a real,
shift-clickable link reads as a player. The client only renders a link if the escape
sequence is exact, so these are built from the item's real entry id and quality
rather than from anything the model produced.
"""
from __future__ import annotations

import re

# ItemQualityColors from the core's SharedDefines.h, alpha stripped.
QUALITY_COLOR = {
    0: "9d9d9d", 1: "ffffff", 2: "1eff00", 3: "0070dd",
    4: "a335ee", 5: "ff8000", 6: "e6cc80", 7: "e6cc80",
}

ITEM_TOKEN = "[[ITEM]]"

# Models will not reproduce a pipe-escape sequence reliably, so they are given a
# placeholder and the real link is substituted afterwards.
_TOKEN_RE = re.compile(r"\[\[ *ITEM *\]\]", re.I)


def item_link(entry: int, name: str, quality: int) -> str:
    color = QUALITY_COLOR.get(quality, "ffffff")
    return f"|cff{color}|Hitem:{entry}:0:0:0:0:0:0:0:0|h[{name}]|h|r"


def achievement_link(achievement_id: int, name: str, player_guid: int = 0) -> str:
    # Achievement links are always the same gold; the trailing fields are the
    # completion date, which the client tolerates as zeroes.
    return (f"|cffffff00|Hachievement:{achievement_id}:{player_guid}:0:0:0:0:0:0"
            f"|h[{name}]|h|r")


def substitute(text: str, item: dict | None) -> str:
    """Replace the item placeholder with a real link, or the plain name as a fallback.

    Also catches the case where the model ignored the placeholder and typed the name
    directly, so the link still appears.
    """
    if not item:
        return _TOKEN_RE.sub("it", text)

    link = item_link(item["entry"], item["name"], item["quality"])
    if _TOKEN_RE.search(text):
        return _TOKEN_RE.sub(link, text)

    # Model wrote the bare name instead of the token - upgrade it in place, once.
    bare = re.compile(re.escape(item["name"]), re.I)
    if bare.search(text):
        return bare.sub(link, text, count=1)

    return text
