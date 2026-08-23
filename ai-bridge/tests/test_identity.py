"""Pure-logic tests for bridge.identity: personality derivation and prompt text.

IdentityStore.start()/_fetchone() need a live aiomysql pool and are out of scope,
but IdentityStore._build() is a @staticmethod that only maps an already-fetched
row tuple - it needs no pool, so it is exercised directly with fake rows.
"""

from __future__ import annotations

from bridge.identity import (
    ARCHETYPES,
    INTEREST,
    TEMPERAMENT,
    VERBOSITY,
    BotIdentity,
    IdentityStore,
    Personality,
    derive_personality,
)

_ARCHETYPE_NAMES = {name for name, _, _ in ARCHETYPES}


def test_derive_personality_is_deterministic_for_the_same_guid() -> None:
    # BRD s33: personality must survive a restart, which only holds if it is a pure
    # function of the GUID rather than anything stored.
    assert derive_personality(424242) == derive_personality(424242)


def test_derive_personality_fields_stay_within_the_declared_vocabulary() -> None:
    for guid in (1, 2, 3, 1000, 999999):
        p = derive_personality(guid)
        assert p.temperament in TEMPERAMENT
        assert p.verbosity in VERBOSITY
        assert p.interest in INTEREST
        assert p.archetype in _ARCHETYPE_NAMES


def test_derive_personality_varies_across_guids() -> None:
    # Not a statistical/distribution test - just confirms the hash actually feeds
    # the traits instead of every bot collapsing onto one archetype.
    archetypes = {derive_personality(guid).archetype for guid in range(1, 60)}
    assert len(archetypes) > 1


def test_personality_describe_includes_every_trait() -> None:
    p = Personality(
        temperament="dry",
        verbosity="chatty",
        interest="pvp ganking",
        archetype="pro",
        archetype_prompt="Terse raider text.",
    )
    text = p.describe()
    assert "dry" in text and "chatty" in text and "pvp ganking" in text
    assert "Terse raider text." in text


def test_system_prompt_mentions_guild_only_when_present() -> None:
    p = Personality(
        temperament="dry",
        verbosity="short",
        interest="raiding",
        archetype="normal",
        archetype_prompt="text",
    )
    guilded = BotIdentity(
        guid=1,
        name="Bob",
        race="Human",
        klass="Warrior",
        level=60,
        zone=1,
        online=True,
        guild="Emerald Dream",
        personality=p,
    )
    guildless = BotIdentity(
        guid=2,
        name="Sam",
        race="Orc",
        klass="Mage",
        level=60,
        zone=1,
        online=True,
        guild=None,
        personality=p,
    )
    assert "of the guild <Emerald Dream>" in guilded.system_prompt()
    assert "of the guild" not in guildless.system_prompt()


def test_build_returns_none_for_a_missing_row() -> None:
    assert IdentityStore._build(None) is None


def test_build_maps_known_ids_and_falls_back_for_unknown_ones() -> None:
    known = IdentityStore._build((1, "Bob", 1, 1, 60, 12, 1, "Emerald Dream"))
    assert known.race == "Human" and known.klass == "Warrior" and known.online is True

    unknown = IdentityStore._build((2, "Sam", 999, 999, 10, 1, 0, None))
    assert unknown.race == "Unknown" and unknown.klass == "Adventurer"
    assert unknown.online is False and unknown.guild is None
    # personality is derived from the guid, not stored - must match calling it directly
    assert unknown.personality == derive_personality(2)
