"""Pure-logic tests for bridge.reflex: canned_chat() / canned_event()."""

from __future__ import annotations

from bridge.reflex import canned_chat, canned_event


def test_canned_chat_matches_a_known_greeting_deterministically() -> None:
    reply = canned_chat("hi", "botA")
    assert reply in ("hey", "yo", "hi", "sup")
    assert canned_chat("hi", "botA") == reply  # same (message, seed) never rerolls


def test_canned_chat_ignores_punctuation_and_case() -> None:
    # Normalisation feeds into the same seed string, so a punctuated/uppercase
    # variant of an already-tested message must pick the identical reply.
    assert canned_chat("HI!!", "botA") == canned_chat("hi", "botA")


def test_canned_chat_is_anchored_not_a_substring_match() -> None:
    # "thanks" alone is reflex; "thanks man" is not the same channel-chat shape and
    # must fall through to the model rather than get a canned "np".
    assert canned_chat("thanks man", "botA") is None


def test_canned_chat_rejects_empty_or_blank_input() -> None:
    assert canned_chat("", "botA") is None
    assert canned_chat("   ", "botA") is None


def test_canned_chat_rejects_messages_over_the_length_cap() -> None:
    assert canned_chat("this is definitely not a short reflex line", "botA") is None


def test_canned_event_known_and_unknown_event_types() -> None:
    assert canned_event("levelup", "botA") in ("grats", "gz", "grats!", "nice, gz")
    assert canned_event("not_a_real_event", "botA") is None
