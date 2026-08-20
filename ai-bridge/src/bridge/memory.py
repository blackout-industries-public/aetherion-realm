"""Tiered conversation memory (BRD s23).

Tier 1 is the recent transcript, replayed verbatim. Tier 2 is a per-pair
relationship counter that costs one row and gives the bot a sense of familiarity.
Deliberately SQLite: the bridge must never be able to damage the game databases,
so it does not share a server with them.
"""
from __future__ import annotations

import time
from dataclasses import dataclass

import aiosqlite

from .config import settings

SCHEMA = """
CREATE TABLE IF NOT EXISTS turns (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    bot_guid  INTEGER NOT NULL,
    speaker   TEXT    NOT NULL,
    role      TEXT    NOT NULL,   -- 'user' (the human) or 'assistant' (the bot)
    content   TEXT    NOT NULL,
    ts        REAL    NOT NULL
);
CREATE INDEX IF NOT EXISTS turns_lookup ON turns (bot_guid, speaker, id);

CREATE TABLE IF NOT EXISTS relationship (
    bot_guid    INTEGER NOT NULL,
    speaker     TEXT    NOT NULL,
    exchanges   INTEGER NOT NULL DEFAULT 0,
    first_seen  REAL    NOT NULL,
    last_seen   REAL    NOT NULL,
    PRIMARY KEY (bot_guid, speaker)
);
"""


@dataclass(frozen=True)
class Relationship:
    exchanges: int
    first_seen: float
    last_seen: float

    def describe(self, speaker: str) -> str | None:
        if self.exchanges <= 1:
            return None
        days = (time.time() - self.first_seen) / 86400
        when = f" You first spoke about {int(days)} days ago." if days >= 1 else ""
        return (f"You have spoken with {speaker} {self.exchanges} times before."
                f"{when} You know them.")


class Memory:
    def __init__(self, path: str | None = None) -> None:
        self._path = path or settings.memory_path
        self._db: aiosqlite.Connection | None = None

    async def start(self) -> None:
        self._db = await aiosqlite.connect(self._path)
        await self._db.executescript(SCHEMA)
        await self._db.commit()

    async def close(self) -> None:
        if self._db:
            await self._db.close()

    async def history(self, bot_guid: int, speaker: str) -> list[dict[str, str]]:
        assert self._db is not None
        # Newest N, then reversed: the tail of a conversation is what matters, but the
        # model still needs it in chronological order.
        async with self._db.execute(
            "SELECT role, content FROM turns WHERE bot_guid=? AND speaker=? "
            "ORDER BY id DESC LIMIT ?", (bot_guid, speaker, settings.history_turns)
        ) as cur:
            rows = await cur.fetchall()
        return [{"role": r, "content": c} for r, c in reversed(rows)]

    async def relationship(self, bot_guid: int, speaker: str) -> Relationship | None:
        assert self._db is not None
        async with self._db.execute(
            "SELECT exchanges, first_seen, last_seen FROM relationship "
            "WHERE bot_guid=? AND speaker=?", (bot_guid, speaker)
        ) as cur:
            row = await cur.fetchone()
        return Relationship(*row) if row else None

    async def record(self, bot_guid: int, speaker: str, prompt: str, reply: str) -> None:
        assert self._db is not None
        now = time.time()
        await self._db.executemany(
            "INSERT INTO turns (bot_guid, speaker, role, content, ts) VALUES (?,?,?,?,?)",
            [(bot_guid, speaker, "user", prompt, now),
             (bot_guid, speaker, "assistant", reply, now)],
        )
        await self._db.execute(
            "INSERT INTO relationship (bot_guid, speaker, exchanges, first_seen, last_seen) "
            "VALUES (?,?,1,?,?) "
            "ON CONFLICT(bot_guid, speaker) DO UPDATE SET "
            "exchanges = exchanges + 1, last_seen = excluded.last_seen",
            (bot_guid, speaker, now, now),
        )
        await self._db.commit()

    async def best_acquaintance(self, speaker: str, limit: int = 8) -> list[tuple[int, int]]:
        """Bots that know this player, most familiar first.

        Used to pick who greets someone on login: a greeting from a stranger is
        noise, a greeting from someone you ran a dungeon with is the whole point.
        """
        assert self._db is not None
        async with self._db.execute(
            "SELECT bot_guid, exchanges FROM relationship WHERE speaker=? "
            "ORDER BY exchanges DESC, last_seen DESC LIMIT ?", (speaker, limit)
        ) as cur:
            return list(await cur.fetchall())

    async def forget(self, bot_guid: int) -> None:
        assert self._db is not None
        await self._db.execute("DELETE FROM turns WHERE bot_guid=?", (bot_guid,))
        await self._db.execute("DELETE FROM relationship WHERE bot_guid=?", (bot_guid,))
        await self._db.commit()
