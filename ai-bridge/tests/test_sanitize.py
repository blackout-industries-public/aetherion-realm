"""The sanitizer's one absolute rule: model scaffolding never reaches a player.

Every payload here except the plain-line case was observed live in game chat
before the corresponding scrubber existed.
"""

from __future__ import annotations

from bridge.llm import LLM


def test_harmony_scaffold_unwraps_to_the_json_texts_line() -> None:
    # Whispered verbatim to a player before the scrubber existed.
    raw = ('<|channel|>commentary to=final <|constrain|>json'
           '<|message|>{"text":"[GINVITE] test, you\'re in!"}')
    assert LLM.sanitize(raw) == "[GINVITE] test, you're in!"


def test_bare_json_envelope_unwraps() -> None:
    assert LLM.sanitize('{"text": "sure, on my way"}') == "sure, on my way"


def test_scaffold_without_envelope_is_scrubbed_not_shipped() -> None:
    cleaned = LLM.sanitize("<|channel|>final<|message|>sure, on my way")
    assert "<|" not in cleaned
    assert "sure, on my way" in cleaned


def test_unsalvageable_scaffold_becomes_empty_for_the_fallback_path() -> None:
    # A lone opener the token regex cannot pair off must not leak either.
    assert LLM.sanitize("<|chan sure thing") == ""


def test_plain_lines_pass_untouched() -> None:
    assert LLM.sanitize("sure, give me the coords") == "sure, give me the coords"
