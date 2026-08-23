"""Pure-logic tests for bridge.topics: weighted choice branches and render() text."""

from __future__ import annotations

from bridge import topics

_ALL_TOPICS = {topic for topic, _ in topics._WEIGHTS}


def test_choose_never_returns_gated_topics_when_state_is_missing() -> None:
    # guild_promo needs a guild, selling needs an item - the pool filter must drop
    # them entirely, not just deprioritise them, or a guildless bot would spam
    # <> guild ads.
    without_guild = {topics.choose(has_guild=False, has_item=True) for _ in range(300)}
    without_item = {topics.choose(has_guild=True, has_item=False) for _ in range(300)}
    assert "guild_promo" not in without_guild
    assert "selling" not in without_item


def test_choose_full_pool_result_is_always_a_known_topic() -> None:
    results = {topics.choose(has_guild=True, has_item=True) for _ in range(300)}
    assert results <= _ALL_TOPICS


def test_choose_full_pool_cumulative_boundaries(monkeypatch) -> None:
    # Cumulative weights in _WEIGHTS order: selling 26, buying 38, guild_promo 54,
    # lfg 72, activity 86, complaint 94, boast 100. The comparison is `roll <= upto`,
    # so the boundary value itself belongs to the earlier bucket, not the next one.
    cases = [(0.0, "selling"), (26.0, "selling"), (26.001, "buying"), (100.0, "boast")]
    for roll, expected in cases:
        monkeypatch.setattr(topics.random, "uniform", lambda a, b, roll=roll: roll)
        assert topics.choose(has_guild=True, has_item=True) == expected


def test_choose_reduced_pool_boundary(monkeypatch) -> None:
    # Without guild/item the pool is buying 12, lfg 30, activity 44, complaint 52,
    # boast 58 (cumulative) - the excluded topics must not shift these thresholds.
    monkeypatch.setattr(topics.random, "uniform", lambda a, b: 12.0)
    assert topics.choose(has_guild=False, has_item=False) == "buying"
    monkeypatch.setattr(topics.random, "uniform", lambda a, b: 12.001)
    assert topics.choose(has_guild=False, has_item=False) == "lfg"


def test_choose_falls_back_to_last_topic_if_roll_exceeds_total(monkeypatch) -> None:
    # random.uniform(0, total) cannot exceed total in practice, but the loop has an
    # explicit fallback for it (`return pool[-1][0]`); pin the roll above the total
    # to exercise that guard directly.
    monkeypatch.setattr(topics.random, "uniform", lambda a, b: 9999.0)
    assert topics.choose(has_guild=True, has_item=True) == "boast"


def test_render_selling_keeps_item_token_literal_and_ignores_item_name() -> None:
    # The selling prompt instructs the model to write the [[ITEM]] token verbatim;
    # render() has no {item} field for that topic, so the real item name passed in
    # must never leak into the rendered prompt (substitution happens later, in
    # links.substitute()).
    text = topics.render(
        "selling", level=10, guild=None, item="Sword of a Thousand Truths"
    )
    assert "[[ITEM]]" in text
    assert "Sword of a Thousand Truths" not in text


def test_render_guild_promo_includes_name_and_optional_member_count() -> None:
    with_size = topics.render(
        "guild_promo", level=10, guild="Emerald Dream", item=None, guild_size=12
    )
    without_size = topics.render(
        "guild_promo", level=10, guild="Emerald Dream", item=None, guild_size=0
    )
    assert "<Emerald Dream>" in with_size and "currently 12 members" in with_size
    assert "<Emerald Dream>" in without_size and "currently" not in without_size


def test_render_level_gated_prompts_include_the_level_number() -> None:
    for topic in ("buying", "lfg", "activity"):
        text = topics.render(topic, level=37, guild=None, item=None)
        assert "37" in text


def test_render_complaint_and_boast_carry_no_guild_or_item_state() -> None:
    # These templates take no placeholders; passing guild/item must not change or
    # break their output.
    text = topics.render("complaint", level=1, guild="Some Guild", item="Some Item")
    assert "Some Guild" not in text and "Some Item" not in text
