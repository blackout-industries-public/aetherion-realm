"""Tiered conversation memory (BRD s23), stored in MySQL.

Tier 1 is the recent transcript, replayed verbatim. Tier 2 is a per-pair relationship
counter that costs one row and gives the bot a sense of familiarity.

Lives in its own schema on the realm's MySQL server rather than in SQLite. Durability
was never the deciding factor - the backup covers either - but sharing the server
means memory lands in the same dump as everything else, and it can be queried
alongside game data, which a file in a container volume cannot.

Personality is deliberately NOT stored: it is derived from the character GUID, so it
survives a lost database entirely. Only learned state belongs here.
"""
from __future__ import annotations

import time
from dataclasses import dataclass

import aiomysql

from .config import settings

SCHEMA = "aetherion_ai"

DDL = [
    f"CREATE DATABASE IF NOT EXISTS {SCHEMA} "
    "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_general_ci",

    f"""CREATE TABLE IF NOT EXISTS {SCHEMA}.turns (
        id        BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
        bot_guid  INT UNSIGNED    NOT NULL,
        speaker   VARCHAR(32)     NOT NULL,
        role      ENUM('user','assistant') NOT NULL,
        content   TEXT            NOT NULL,
        ts        DOUBLE          NOT NULL,
        PRIMARY KEY (id),
        KEY turns_lookup (bot_guid, speaker, id)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4""",

    f"""CREATE TABLE IF NOT EXISTS {SCHEMA}.relationship (
        bot_guid   INT UNSIGNED NOT NULL,
        speaker    VARCHAR(32)  NOT NULL,
        exchanges  INT UNSIGNED NOT NULL DEFAULT 0,
        first_seen DOUBLE       NOT NULL,
        last_seen  DOUBLE       NOT NULL,
        PRIMARY KEY (bot_guid, speaker)
    ) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4""",
]


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
    def __init__(self) -> None:
        self._pool: aiomysql.Pool | None = None

    async def start(self) -> None:
        # Connect without a default schema first: the schema may not exist yet.
        bootstrap = await aiomysql.connect(
            host=settings.db_host, port=settings.db_port,
            user=settings.db_user, password=settings.db_password, autocommit=True)
        try:
            async with bootstrap.cursor() as cur:
                for statement in DDL:
                    await cur.execute(statement)
        finally:
            bootstrap.close()

        self._pool = await aiomysql.create_pool(
            host=settings.db_host, port=settings.db_port,
            user=settings.db_user, password=settings.db_password,
            db=SCHEMA, minsize=1, maxsize=4, autocommit=True)

    async def close(self) -> None:
        if self._pool:
            self._pool.close()
            await self._pool.wait_closed()

    async def history(self, bot_guid: int, speaker: str) -> list[dict[str, str]]:
        assert self._pool is not None
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            # Newest N, then reversed: the tail of a conversation is what matters, but
            # the model still needs it in chronological order.
            await cur.execute(
                "SELECT role, content FROM turns WHERE bot_guid=%s AND speaker=%s "
                "ORDER BY id DESC LIMIT %s",
                (bot_guid, speaker, settings.history_turns))
            rows = await cur.fetchall()
        return [{"role": r, "content": c} for r, c in reversed(rows)]

    async def relationship(self, bot_guid: int, speaker: str) -> Relationship | None:
        assert self._pool is not None
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(
                "SELECT exchanges, first_seen, last_seen FROM relationship "
                "WHERE bot_guid=%s AND speaker=%s", (bot_guid, speaker))
            row = await cur.fetchone()
        return Relationship(*row) if row else None

    async def best_acquaintance(self, speaker: str, limit: int = 8) -> list[tuple[int, int]]:
        """Bots that know this player, most familiar first.

        Used to pick who greets someone on login: a greeting from a stranger is
        noise, a greeting from someone you ran a dungeon with is the whole point.
        """
        assert self._pool is not None
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(
                "SELECT bot_guid, exchanges FROM relationship WHERE speaker=%s "
                "ORDER BY exchanges DESC, last_seen DESC LIMIT %s", (speaker, limit))
            return list(await cur.fetchall())

    async def record(self, bot_guid: int, speaker: str, prompt: str, reply: str) -> None:
        assert self._pool is not None
        now = time.time()
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.executemany(
                "INSERT INTO turns (bot_guid, speaker, role, content, ts) "
                "VALUES (%s,%s,%s,%s,%s)",
                [(bot_guid, speaker, "user", prompt, now),
                 (bot_guid, speaker, "assistant", reply, now)])
            await cur.execute(
                "INSERT INTO relationship (bot_guid, speaker, exchanges, first_seen, last_seen) "
                "VALUES (%s,%s,1,%s,%s) "
                "ON DUPLICATE KEY UPDATE exchanges = exchanges + 1, last_seen = VALUES(last_seen)",
                (bot_guid, speaker, now, now))

    async def forget(self, bot_guid: int) -> None:
        assert self._pool is not None
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute("DELETE FROM turns WHERE bot_guid=%s", (bot_guid,))
            await cur.execute("DELETE FROM relationship WHERE bot_guid=%s", (bot_guid,))
