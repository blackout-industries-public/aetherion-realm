"""Pure-logic tests for bridge.intents.extract(): tag grammar, gating, refusals."""

from __future__ import annotations

import pytest

from bridge import intents


def test_allowed_matches_the_documented_intent_vocabulary() -> None:
    assert set(intents.ALLOWED) == {
        "guild_invite",
        "party_invite",
        "come",
        "follow",
        "stay",
        "queue_bg",
        "buff",
    }


@pytest.mark.parametrize(
    "reply, player_message, expected_intent, expected_text",
    [
        (
            "[GINVITE] sure, you're in!",
            "can I get a guild invite?",
            "guild_invite",
            "sure, you're in!",
        ),
        (
            "[PINVITE] joining you now",
            "lfg for a heroic?",
            "party_invite",
            "joining you now",
        ),
        ("[COME] on my way", "where are you? come here", "come", "on my way"),
        (
            "[FOLLOW] sticking with you",
            "follow me please",
            "follow",
            "sticking with you",
        ),
        ("[STAY] holding position", "wait here", "stay", "holding position"),
    ],
)
def test_accepts_a_gated_intent_and_strips_the_tag(
    reply: str,
    player_message: str,
    expected_intent: str,
    expected_text: str,
) -> None:
    intent, cleaned = intents.extract(reply, player_message)
    assert intent == expected_intent
    assert cleaned == expected_text
    assert "[" not in cleaned and "]" not in cleaned


def test_rejects_a_hallucinated_tag_the_player_never_asked_for() -> None:
    # STAY's gate requires stay/wait/hold/stop in the player's own message - without
    # it the tag must be dropped even though it is otherwise well-formed.
    intent, cleaned = intents.extract("[STAY] sure thing", "let's go kill the boss")
    assert intent is None
    assert cleaned == "sure thing"


def test_refusal_language_drops_the_intent_but_keeps_the_line() -> None:
    intent, cleaned = intents.extract("[COME] not right now", "come here")
    assert intent is None
    assert cleaned == "not right now"


@pytest.mark.parametrize("reply", ["[FOLLOW] not bad, sure", "[FOLLOW] why not, sure"])
def test_refusal_carveouts_do_not_falsely_trigger(reply: str) -> None:
    # "not bad" and "why not" read as agreement, not refusal - the _REFUSAL regex has
    # explicit exceptions for both; if either regresses, a confirmed follow silently
    # turns into a no-op.
    intent, _ = intents.extract(reply, "follow me")
    assert intent == "follow"


def test_unknown_tag_is_stripped_from_output_but_the_intent_is_rejected() -> None:
    intent, cleaned = intents.extract("[XYZZY] sure thing", "come here")
    assert intent is None
    assert cleaned == "sure thing"
    assert "XYZZY" not in cleaned and "[" not in cleaned


def test_tag_shorter_than_the_grammar_minimum_is_not_recognised_as_a_tag() -> None:
    # _TAG requires 3-16 chars inside the brackets; "AB" is too short to match at
    # all, so the whole line - brackets included - is treated as ordinary chat text.
    reply = "[AB] hi there"
    intent, cleaned = intents.extract(reply, "come here")
    assert intent is None
    assert cleaned == reply


def test_no_tag_present_returns_the_reply_untouched() -> None:
    reply = "sure, count me in"
    intent, cleaned = intents.extract(reply, "come here")
    assert intent is None
    assert cleaned == reply


def test_tag_must_lead_the_line_not_appear_mid_message() -> None:
    # The model is instructed to "begin your line" with a tag; one buried in prose
    # is left as literal text rather than honoured.
    reply = "well, [COME] sure"
    intent, cleaned = intents.extract(reply, "come here")
    assert intent is None
    assert cleaned == reply


def test_tag_matching_is_case_insensitive() -> None:
    intent, cleaned = intents.extract("[ginvite] sure thing", "need a guild invite")
    assert intent == "guild_invite"
    assert cleaned == "sure thing"
