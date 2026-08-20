"""Allow-listed action intents (BRD s29).

The model never issues a command. It may only tag its reply with one intent from a
fixed vocabulary, and the tag is honoured only when the player's own message plausibly
asked for it. The server then decides whether the action is legal at all.

That second gate is the important one: without it, a model that hallucinates a tag
could invite people to guilds nobody asked to join.
"""
from __future__ import annotations

import re

# intent -> patterns that must appear in the PLAYER's message for the tag to count.
_GATES: dict[str, re.Pattern] = {
    # Must name a guild explicitly. Generic wording like "invite me" also matches a
    # party request, and a mistagged reply would send a guild invite nobody asked for.
    "guild_invite": re.compile(r"\b(guild|ginv|g ?inv|recruit(ing|ment)?)\b", re.I),
    "party_invite": re.compile(r"\b(invite|inv|group|party|grp|lfg|run|dungeon|heroic|raid)\b", re.I),
    "come":         re.compile(r"\b(come|here|meet|find me|where are you|bring)\b", re.I),
    "follow":       re.compile(r"\b(follow|with me|lead)\b", re.I),
    "stay":         re.compile(r"\b(stay|wait|hold|stop)\b", re.I),
}

ALLOWED = tuple(_GATES)

_TAG = re.compile(r"^\s*\[(?P<tag>[A-Z_]{3,16})\]\s*", re.I)

# The tag and the words can disagree - observed live: "[GINVITE] you're not in the
# plans right now". Acting on the tag there would invite someone while refusing them,
# so a reply that reads as a refusal drops its intent and stays pure flavour.
_REFUSAL = re.compile(
    r"\b(no|nope|nah|never|can'?t|cannot|won'?t|don'?t|denied|forget it|"
    r"piss off|maybe later|another time)\b"
    # Bare "not" catches the phrasings a list never will ("not in the plans right
    # now"), but "why not" and "not bad" are agreement, so they are excluded.
    r"|(?<!why )\bnot\b(?! (bad|a problem|an issue))", re.I)

_ALIASES = {
    "ginvite": "guild_invite", "guildinvite": "guild_invite", "guild_invite": "guild_invite",
    "pinvite": "party_invite", "invite": "party_invite", "party_invite": "party_invite",
    "come": "come", "follow": "follow", "stay": "stay",
}

INSTRUCTION = (
    "If you are agreeing to do one of these, begin your line with exactly one tag:\n"
    "[GINVITE] you are inviting them to your guild\n"
    "[PINVITE] you are inviting them to your group\n"
    "[COME] you are going to travel to them\n"
    "[FOLLOW] you will follow them\n"
    "[STAY] you will wait here\n"
    "If none apply, use no tag. Never use a tag you were not asked for."
)


def extract(reply: str, player_message: str) -> tuple[str | None, str]:
    """Split an optional intent tag off a reply and validate it against the request."""
    match = _TAG.match(reply)
    if not match:
        return None, reply

    stripped = reply[match.end():].strip()
    intent = _ALIASES.get(match.group("tag").lower())
    if not intent:
        return None, stripped

    # The model may only confirm an action the player actually raised.
    gate = _GATES[intent]
    if not gate.search(player_message):
        return None, stripped

    # An action must match the words. A refusal keeps its line but loses its tag.
    if _REFUSAL.search(stripped):
        return None, stripped

    return intent, stripped
