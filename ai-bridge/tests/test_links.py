"""Pure-logic tests for bridge.links: item/achievement link formatting and
token/name substitution.
"""

from __future__ import annotations

from bridge.links import achievement_link, item_link, substitute


def test_item_link_uses_the_quality_colour_and_known_fallback() -> None:
    epic = item_link(12345, "Sword", 4)
    assert epic == "|cffa335ee|Hitem:12345:0:0:0:0:0:0:0:0|h[Sword]|h|r"
    # Quality codes are 0-7; anything outside that (bad data) must not raise, and
    # falls back to the common/white colour rather than crashing metric/chat output.
    unknown_quality = item_link(1, "Odd Item", 99)
    assert unknown_quality.startswith("|cffffffff|Hitem:1:")


def test_achievement_link_format() -> None:
    link = achievement_link(456, "Level 60", player_guid=0)
    assert link == "|cffffff00|Hachievement:456:0:0:0:0:0:0:0|h[Level 60]|h|r"


def test_substitute_without_an_item_replaces_the_token_with_plain_it() -> None:
    text = substitute("check out [[ITEM]] its great", None)
    assert text == "check out it its great"


def test_substitute_replaces_the_token_case_and_space_insensitively() -> None:
    item = {"entry": 1, "name": "Thing", "quality": 2}
    text = substitute("selling [[ item ]] cheap", item)
    assert item_link(1, "Thing", 2) in text
    assert "[[" not in text


def test_substitute_upgrades_a_bare_item_name_exactly_once() -> None:
    # The model sometimes ignores the token and types the real name - it should be
    # upgraded to a real link, but only the first occurrence (count=1 in the source).
    item = {"entry": 7, "name": "Thunderfury", "quality": 5}
    text = substitute("selling Thunderfury, yes really Thunderfury", item)
    link = item_link(7, "Thunderfury", 5)
    assert text.count(link) == 1
    assert "Thunderfury" in text.split(link, 1)[1]  # second mention stays bare


def test_substitute_leaves_text_alone_with_no_token_and_no_name_match() -> None:
    item = {"entry": 7, "name": "Thunderfury", "quality": 5}
    text = substitute("just chatting about nothing in particular", item)
    assert text == "just chatting about nothing in particular"
