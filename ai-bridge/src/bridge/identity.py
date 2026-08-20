"""Bot identity and personality.

Game state is authoritative and read live from the character database; personality
is derived deterministically from the character GUID. Deriving rather than storing
is what makes BRD s33's "personality persists across restart" true by construction:
the same character always produces the same traits, even if the memory volume is
lost entirely.
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass

import aiomysql

from .config import settings

RACES = {1: "Human", 2: "Orc", 3: "Dwarf", 4: "Night Elf", 5: "Undead", 6: "Tauren",
         7: "Gnome", 8: "Troll", 10: "Blood Elf", 11: "Draenei"}
CLASSES = {1: "Warrior", 2: "Paladin", 3: "Hunter", 4: "Rogue", 5: "Priest",
           6: "Death Knight", 7: "Shaman", 8: "Mage", 9: "Warlock", 11: "Druid"}

TEMPERAMENT = ["blunt", "cheerful", "impatient", "dry", "earnest", "weary", "cocky", "wary"]

# How competent and how engaged the player behind the character is. Weighted so most
# of the realm reads as ordinary - archetypes only land as characters when they are
# the exception rather than the norm.
ARCHETYPES: list[tuple[str, int, str]] = [
    ("normal", 44,
     "You are a competent, unremarkable player. Nothing exaggerated."),
    ("pro", 12,
     "You are a serious raider. Terse, min-maxing, heavy on jargon (gs, dps, cd, "
     "sunder, LFM). Impatient with people who waste your time. You never explain "
     "basics and you assume everyone knows them."),
    ("clueless", 12,
     "You barely understand the game. You misuse terms, ask things everyone knows, "
     "and get names of places and abilities slightly wrong. Type sloppily with "
     "occasional typos and missing punctuation. Never realise you are wrong."),
    ("afk", 10,
     "You are barely at the keyboard. Reply in one to three words at most, often "
     "late and unhelpfully: 'afk', 'brb', 'k', 'sec', 'was afk'. Never elaborate."),
    ("scumbag", 8,
     "You are out for yourself. You lowball, overcharge, hint at ninja looting and "
     "needing on offspec, and you are rude about it. Never openly admit to scamming."),
    ("roleplayer", 7,
     "You speak in character as your race and class, slightly archaic and earnest. "
     "You never use out-of-game abbreviations."),
    ("goldseller", 7,
     "You obsess over gold and markets. Every topic circles back to prices, profit "
     "margins and undercutting."),
]
VERBOSITY = ["clipped", "short", "chatty"]
INTEREST = ["battlegrounds", "raiding", "gold-making", "exploring", "professions",
            "achievements", "pvp ganking", "levelling alts"]


@dataclass(frozen=True)
class Personality:
    temperament: str
    verbosity: str
    interest: str
    archetype: str
    archetype_prompt: str

    def describe(self) -> str:
        return (f"You are {self.temperament} in tone, {self.verbosity} in how much you "
                f"say, and you care mostly about {self.interest}.\n"
                f"{self.archetype_prompt}")


@dataclass(frozen=True)
class BotIdentity:
    guid: int
    name: str
    race: str
    klass: str
    level: int
    zone: int
    online: bool
    guild: str | None
    personality: Personality

    def system_prompt(self) -> str:
        guild = f" of the guild <{self.guild}>" if self.guild else ""
        return (
            f"You are {self.name}, a level {self.level} {self.race} {self.klass}"
            f"{guild} in World of Warcraft: Wrath of the Lich King.\n"
            f"{self.personality.describe()}\n"
            "You are a player character, not an NPC and not an assistant. Speak the way "
            "someone types in a game chat window: lowercase is fine, abbreviations are "
            "fine, one or two short sentences at most.\n"
            "Never mention being an AI, a language model, or a bot. Never break character. "
            "Never describe actions in asterisks. Only ever produce the words you would "
            "type into chat.\n"
            "Output ONLY that line. Do not explain your reasoning, do not restate the "
            "situation, do not refer to yourself in the third person, and do not write "
            "anything about what you are going to say.\n"
            "You cannot actually perform game actions through this conversation. If you are "
            "asked to do something, answer in character about whether you are willing - the "
            "game itself decides what actually happens."
        )


def derive_personality(guid: int) -> Personality:
    # Hash the GUID rather than seeding a global RNG: no shared state, no ordering
    # dependence, and stable across processes and restarts.
    digest = hashlib.sha256(f"aetherion:{guid}".encode()).digest()

    # Weighted pick from a stable hash byte, so a bot keeps its archetype forever.
    total = sum(w for _, w, _ in ARCHETYPES)
    roll = digest[3] % total
    upto = 0
    name, prompt = ARCHETYPES[0][0], ARCHETYPES[0][2]
    for candidate, weight, text in ARCHETYPES:
        upto += weight
        if roll < upto:
            name, prompt = candidate, text
            break

    return Personality(
        temperament=TEMPERAMENT[digest[0] % len(TEMPERAMENT)],
        verbosity=VERBOSITY[digest[1] % len(VERBOSITY)],
        interest=INTEREST[digest[2] % len(INTEREST)],
        archetype=name,
        archetype_prompt=prompt,
    )


class IdentityStore:
    def __init__(self) -> None:
        self._pool: aiomysql.Pool | None = None

    async def start(self) -> None:
        self._pool = await aiomysql.create_pool(
            host=settings.db_host, port=settings.db_port,
            user=settings.db_user, password=settings.db_password,
            db=settings.db_characters, minsize=1, maxsize=4, autocommit=True,
        )

    async def close(self) -> None:
        if self._pool:
            self._pool.close()
            await self._pool.wait_closed()

    async def _fetchone(self, sql: str, args: tuple) -> tuple | None:
        assert self._pool is not None, "IdentityStore.start() was not awaited"
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(sql, args)
            return await cur.fetchone()

    async def by_guid(self, guid: int) -> BotIdentity | None:
        row = await self._fetchone(
            "SELECT c.guid, c.name, c.race, c.class, c.level, c.zone, c.online, g.name "
            "FROM characters c "
            "LEFT JOIN guild_member gm ON gm.guid = c.guid "
            "LEFT JOIN guild g ON g.guildid = gm.guildid "
            "WHERE c.guid = %s", (guid,))
        return self._build(row)

    async def online_guids(self, guids: list[int]) -> set[int]:
        """Filter a candidate list down to who is actually in the world right now."""
        if not guids:
            return set()
        assert self._pool is not None
        placeholders = ",".join(["%s"] * len(guids))
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(
                f"SELECT guid FROM characters WHERE online=1 AND guid IN ({placeholders})",
                tuple(guids))
            return {row[0] for row in await cur.fetchall()}

    async def sellable_item(self, guid: int) -> dict | None:
        """Something the bot genuinely owns and could plausibly hawk.

        Grounding trade chatter in real inventory is what stops bots advertising
        items that do not exist - the most obvious tell that chat is generated.
        """
        # Mirrors Item::CanBeTraded, which turns on IsSoulBound() - not on the
        # template's bonding. Filtering by bonding was both wrong in one direction
        # (a BOE the bot already equipped is bound and unsellable) and wrong in the
        # other (it excluded quest-flagged items that trade perfectly well, and there
        # were no non-soulbound ones in bags anyway).
        #
        # Conjured items are excluded because they cannot be auctioned, and greys
        # because advertising vendor trash is not something a player does.
        row = await self._fetchone(
            "SELECT it.entry, it.name, it.Quality FROM character_inventory ci "
            "JOIN item_instance ii ON ii.guid = ci.item "
            "JOIN acore_world.item_template it ON it.entry = ii.itemEntry "
            "WHERE ci.guid = %s "
            "  AND (ii.flags & 1) = 0 "   # not soulbound - the authoritative check
            "  AND (it.Flags & 2) = 0 "   # not conjured
            "  AND it.Quality >= 1 "      # skip grey vendor trash
            # weapons, armour, trade goods, recipes, misc (pets, mounts)
            "  AND it.class IN (2, 4, 7, 9, 15) "
            "ORDER BY RAND() LIMIT 1", (guid,))
        return {"entry": row[0], "name": row[1], "quality": row[2]} if row else None

    async def guild_mates(self, guild_name: str) -> int:
        row = await self._fetchone(
            "SELECT COUNT(*) FROM guild g JOIN guild_member gm ON gm.guildid = g.guildid "
            "WHERE g.name = %s", (guild_name,))
        return row[0] if row else 0

    async def by_name(self, name: str) -> BotIdentity | None:
        row = await self._fetchone(
            "SELECT c.guid, c.name, c.race, c.class, c.level, c.zone, c.online, g.name "
            "FROM characters c "
            "LEFT JOIN guild_member gm ON gm.guid = c.guid "
            "LEFT JOIN guild g ON g.guildid = gm.guildid "
            "WHERE c.name = %s", (name,))
        return self._build(row)

    @staticmethod
    def _build(row: tuple | None) -> BotIdentity | None:
        if not row:
            return None
        guid, name, race, klass, level, zone, online, guild = row
        return BotIdentity(
            guid=guid, name=name,
            race=RACES.get(race, "Unknown"), klass=CLASSES.get(klass, "Adventurer"),
            level=level, zone=zone, online=bool(online), guild=guild,
            personality=derive_personality(guid),
        )
