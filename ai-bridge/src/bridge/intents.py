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
    "queue_bg":     re.compile(r"\b(queue|que|bg|battleground|warsong|wsg|arathi|"
                               r"alterac|av|ab|eots|eye of the storm)\b", re.I),
    "queue_dungeon": re.compile(r"\b(queue|que|dungeon|rdf|lfd|instance|heroic)\b", re.I),
    "give_lead":    re.compile(r"\b(lead(er)?|leadership|promote)\b", re.I),
    "lead_run":     re.compile(r"\b(lead (us|the way|on|through)|take (us|point)|"
                               r"guide (us|me)|take the lead|"
                               r"clear (the|this) (dungeon|place|instance))\b", re.I),
    "buff":         re.compile(r"\b(buff|bless|blessing|fort(itude)?|mark|kings|"
                               r"might|wisdom|intellect)\b", re.I),
}

ALLOWED = tuple(_GATES)

# Self-actions are trusted on the model's own judgment: a wrongly moved bot or a
# stray buff costs nothing, and demanding that the player's words pre-match a
# regex is how "do it" and every paraphrase ended in a friendly reply and no
# action. Only the invites keep the strict ask-gate - those touch OTHER people,
# and a hallucinated one is spam a regex should keep impossible.
_TRUSTED = frozenset(
    {"come", "follow", "stay", "buff", "queue_bg", "queue_dungeon", "give_lead",
     "lead_run"})

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
    "queuebg": "queue_bg", "queue_bg": "queue_bg", "bgqueue": "queue_bg",
    "queuedungeon": "queue_dungeon", "queue_dungeon": "queue_dungeon",
    "dungeonqueue": "queue_dungeon", "rdf": "queue_dungeon", "lfd": "queue_dungeon",
    "buff": "buff", "buffs": "buff",
    "lead": "give_lead", "leader": "give_lead", "give_lead": "give_lead",
    "leadon": "lead_run", "lead_on": "lead_run", "lead_run": "lead_run",
    "leadrun": "lead_run", "takepoint": "lead_run",
}

INSTRUCTION = (
    "If you are agreeing to do one of these, begin your line with exactly one tag:\n"
    "[GINVITE] you are inviting them to your guild\n"
    "[PINVITE] you are inviting them to your group\n"
    "[COME] you are going to travel to them\n"
    "[FOLLOW] you will follow them\n"
    "[STAY] you will wait here\n"
    "[BUFF] you will cast your buffs on them and the group right now\n"
    "[QUEUEBG] you are getting the battleground queue started for them\n"
    "[QUEUEDUNGEON] you are queueing them in the dungeon finder\n"
    "[LEAD] you are handing them the group lead\n"
    "[LEADON] you will take point and lead the group through this dungeon\n"
    "If none apply, use no tag. Never use a tag you were not asked for.\n"
    "When they ask you to DO something and a tag fits, act - use the tag.\n"
    "Examples:\n"
    "They say: inv me / need a party -> [PINVITE] got you, sending it\n"
    "They say: queue us for av -> [QUEUEBG] on it\n"
    "They say: queue random dungeon -> [QUEUEDUNGEON] queueing us now\n"
    "They say: can i get kings? -> [BUFF] coming right up\n"
    "They say: pass me lead -> [LEAD] all yours\n"
    "They say: lead us through the dungeon -> [LEADON] follow me, stay close"
)


def assume(reply: str, player_message: str) -> str | None:
    """The fallback for a model that agrees in words but forgets the tag.

    Only when exactly ONE gate matches the player's ask is the intent adopted -
    an ambiguous ask stays conversation - and a refusal always stays a refusal.
    Observed live: "Need party!" answered "yes, i can help you level up" and
    nobody sent the invite.
    """
    if _REFUSAL.search(reply):
        return None
    matched = [name for name, gate in _GATES.items() if gate.search(player_message)]
    if len(matched) != 1:
        return None
    return matched[0]


def could_act(player_message: str) -> bool:
    """Whether any intent gate matches - i.e. the message might be an ask.

    The reflex layer must never swallow such a message: a canned "k" instead
    of a model pass is how "Queue BG" got an agreement and no queue.
    """
    return any(g.search(player_message) for g in _GATES.values())


def extract(reply: str, player_message: str) -> tuple[str | None, str]:
    """Split an optional intent tag off a reply and validate it against the request."""
    match = _TAG.match(reply)
    if not match:
        return None, reply

    stripped = reply[match.end():].strip()
    intent = _ALIASES.get(match.group("tag").lower())
    if not intent:
        return None, stripped

    # Trusted self-actions act on the model's judgment alone; the invites may
    # only confirm an action the player actually raised.
    if intent not in _TRUSTED and not _GATES[intent].search(player_message):
        return None, stripped

    # An action must match the words. A refusal keeps its line but loses its tag.
    if _REFUSAL.search(stripped):
        return None, stripped

    return intent, stripped
