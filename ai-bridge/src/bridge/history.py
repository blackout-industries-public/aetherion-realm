"""Per-bot activity history.

Nothing in the game schema records what a character did over time - only its current
state. This samples every online character periodically and writes an event whenever
something changes, which is what turns a snapshot into a feed.

Deltas only. Writing a row per bot per tick would be 1500 rows a minute of mostly
nothing; writing only changes keeps it to what actually happened.
"""
from __future__ import annotations

import asyncio
import logging
import time

import aiomysql

from .config import settings
from .memory import SCHEMA

log = logging.getLogger("bridge.history")

DDL = f"""CREATE TABLE IF NOT EXISTS {SCHEMA}.bot_events (
    id       BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    guid     INT UNSIGNED    NOT NULL,
    ts       DOUBLE          NOT NULL,
    kind     VARCHAR(24)     NOT NULL,
    detail   VARCHAR(160)    NOT NULL,
    zone     INT UNSIGNED    NOT NULL DEFAULT 0,
    PRIMARY KEY (id),
    KEY events_lookup (guid, id),
    KEY events_heat (kind, ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"""

# In-place migration for tables created before events carried a location.
# MySQL 8 has no ADD COLUMN IF NOT EXISTS, so the duplicate error is the
# idempotence check.
MIGRATE_ZONE = f"ALTER TABLE {SCHEMA}.bot_events ADD COLUMN zone INT UNSIGNED NOT NULL DEFAULT 0"
MIGRATE_HEAT_KEY = f"ALTER TABLE {SCHEMA}.bot_events ADD KEY events_heat (kind, ts)"

SAMPLE = """
    SELECT c.guid, c.name, c.level, c.zone, c.map, c.online, c.health,
           c.instance_id, c.totalKills, c.money,
           (gm.memberGuid IS NOT NULL) AS grouped,
           (SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r
             WHERE r.guid = c.guid) AS quests_done
    FROM acore_characters.characters c
    LEFT JOIN acore_characters.group_member gm ON gm.memberGuid = c.guid
    WHERE c.online = 1
"""


# Instance maps are named in the world database, so an event can say where a character
# went instead of printing a map id at the reader.
PLACES = """
    SELECT map_id, comment FROM acore_world.dungeon_access_template
    WHERE difficulty = 0 AND comment <> ''
"""

# New rows only. item_instance.guid is monotonic, so a single indexed range scan finds
# everything looted since the last tick without touching the other 76,000 rows.
# Classes 6 and 11 are projectiles and quivers. Ammo is bought and consumed in stacks,
# and every stack is its own row, so leaving it in buries the feed under one character
# restocking arrows.
LOOT = """
    SELECT ii.guid, ii.owner_guid, it.name, it.Quality
    FROM acore_characters.item_instance ii
    JOIN acore_world.item_template it ON it.entry = ii.itemEntry
    JOIN acore_characters.characters c ON c.guid = ii.owner_guid AND c.online = 1
    WHERE ii.guid > %s AND it.Quality >= %s AND it.class NOT IN (6, 11)
    ORDER BY ii.guid
    LIMIT 200
"""

QUALITY_WORD = {2: "uncommon", 3: "rare", 4: "epic", 5: "legendary", 6: "artifact"}

# Realm firsts. The UNIQUE(kind, detail) key is the whole mechanism: INSERT IGNORE
# means only the first writer of "first_level 50" ever lands, atomically - no
# check-then-insert race between samples.
MILESTONES_DDL = f"""CREATE TABLE IF NOT EXISTS {SCHEMA}.milestones (
    id      BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    ts      DOUBLE          NOT NULL,
    kind    VARCHAR(24)     NOT NULL,
    detail  VARCHAR(160)    NOT NULL,
    guid    INT UNSIGNED    NULL,
    who     VARCHAR(120)    NOT NULL DEFAULT '',
    PRIMARY KEY (id),
    UNIQUE KEY first_only (kind, detail)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4"""

LEVEL_MILESTONES = (10, 20, 30, 40, 50, 60, 70, 80)

# (map, difficulty, bit) -> boss name, for detecting realm-first boss kills from the
# completedEncounters bitmask.
ENCOUNTERS = """
    SELECT de.MapID, de.Difficulty, de.Bit, COALESCE(ct.name, ie.comment) AS boss
    FROM acore_world.dungeonencounter_dbc de
    JOIN acore_world.instance_encounters ie ON ie.entry = de.ID
    LEFT JOIN acore_world.creature_template ct
      ON ct.entry = ie.creditEntry AND ie.creditType = 0
"""

INSTANCE_BITS = """
    SELECT id, map, difficulty, completedEncounters
    FROM acore_characters.instance WHERE completedEncounters > 0
"""

# Trade skills worth narrating. A skill-up only happens while actually performing the
# profession, so a delta here is honest evidence the bot was just mining, skinning...
TRADES = {
    164: "Blacksmithing", 165: "Leatherworking", 171: "Alchemy", 182: "Herbalism",
    186: "Mining", 197: "Tailoring", 202: "Engineering", 333: "Enchanting",
    393: "Skinning", 755: "Jewelcrafting", 773: "Inscription",
    129: "First Aid", 185: "Cooking", 356: "Fishing",
}

SKILLS = f"""
    SELECT cs.guid, cs.skill, cs.value
    FROM acore_characters.character_skills cs
    JOIN acore_characters.characters c ON c.guid = cs.guid AND c.online = 1
    WHERE cs.skill IN ({",".join(str(k) for k in TRADES)})
"""


class HistoryRecorder:
    def __init__(self) -> None:
        self._pool: aiomysql.Pool | None = None
        self._task: asyncio.Task | None = None
        self._prev: dict[int, tuple] = {}
        self._places: dict[int, str] = {}
        self._skills: dict[tuple[int, int], int] = {}
        self._bosses: dict[tuple[int, int, int], str] = {}
        self._enc_totals: dict[tuple[int, int], int] = {}
        self._inst_bits: dict[int, int] = {}
        # Set from the current maximum on startup so the first tick does not replay
        # every item the realm has ever created as fresh loot.
        self._max_item: int | None = None

    async def start(self) -> None:
        if not settings.history_enabled:
            return
        self._pool = await aiomysql.create_pool(
            host=settings.db_host, port=settings.db_port,
            user=settings.db_user, password=settings.db_password,
            minsize=1, maxsize=2, autocommit=True)
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(DDL)
            for migration in (MIGRATE_ZONE, MIGRATE_HEAT_KEY):
                try:
                    await cur.execute(migration)
                except Exception:
                    pass  # already applied - the duplicate error is the check
            await cur.execute(PLACES)
            for map_id, comment in await cur.fetchall():
                # "Ulduar,Halls of Stone - 10man" - the wing is the recognisable part.
                name = comment.split(",")[-1]
                name = name.split(" - ")[0].strip()
                if name:
                    self._places[int(map_id)] = name
            await cur.execute("SELECT COALESCE(MAX(guid), 0) FROM acore_characters.item_instance")
            self._max_item = int((await cur.fetchone())[0])
            await cur.execute(MILESTONES_DDL)
            await cur.execute(ENCOUNTERS)
            for map_id, diff, bit, boss in await cur.fetchall():
                self._bosses[(int(map_id), int(diff), int(bit))] = boss
                key = (int(map_id), int(diff))
                self._enc_totals[key] = self._enc_totals.get(key, 0) + 1
        self._task = asyncio.create_task(self._loop())
        log.info("history recorder started (every %ss, keeping %s days)",
                 settings.history_interval, settings.history_retention_days)

    async def close(self) -> None:
        if self._task:
            self._task.cancel()
        if self._pool:
            self._pool.close()
            await self._pool.wait_closed()

    async def _loop(self) -> None:
        while True:
            try:
                await asyncio.sleep(settings.history_interval)
                await self._sample()
            except asyncio.CancelledError:
                raise
            except Exception as exc:                     # noqa: BLE001
                # A recorder failure must never take the bridge down with it.
                log.warning("history sample failed: %s", exc)

    async def _sample(self) -> None:
        assert self._pool is not None
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(SAMPLE)
            rows = await cur.fetchall()

            now = time.time()
            events: list[tuple] = []
            milestones: list[tuple] = []

            for (guid, name, level, zone, map_id, _online, health,
                 instance, kills, money, grouped, quests_done) in rows:
                current = (level, zone, map_id, health > 0, instance, kills,
                           bool(grouped), quests_done)
                previous = self._prev.get(guid)
                self._prev[guid] = current
                if previous is None:
                    continue          # first sighting is not an event

                p_level, p_zone, p_map, p_alive, p_inst, p_kills, p_grouped, p_quests = previous

                # Every event carries the zone it happened in - that is what
                # lets the map draw heat where things ACTUALLY occur instead
                # of where their actors happen to stand at read time.
                def ev(kind: str, detail: str) -> None:
                    events.append((guid, now, kind, detail, zone))

                if level > p_level:
                    ev("level", f"reached level {level}")
                    # Race milestones. A bot can cross several thresholds between two
                    # samples, so every threshold in the gap is claimed, not just the
                    # final level.
                    for t in LEVEL_MILESTONES:
                        if p_level < t <= level:
                            milestones.append((now, "first_level", str(t), guid, name))
                if zone != p_zone:
                    ev("zone", f"moved to zone {zone}")
                if p_alive and health == 0:
                    ev("death", "died")
                if not p_alive and health > 0:
                    ev("revive", "resurrected")
                if instance and not p_inst:
                    where = self._places.get(map_id, f"an instance on map {map_id}")
                    ev("instance", f"entered {where}")
                if p_inst and not instance:
                    ev("instance", "left the instance")
                if kills > p_kills:
                    ev("pvp", f"honourable kill ({kills} total)")
                if quests_done > p_quests:
                    handed_in = quests_done - p_quests
                    ev("quest", f"completed {handed_in} quest{'s' if handed_in > 1 else ''}")
                if grouped and not p_grouped:
                    ev("party", "joined a party")
                if p_grouped and not grouped:
                    ev("party", "left the party")

            events.extend(await self._loot(cur, now))
            events.extend(await self._skillups(cur, now))
            milestones.extend(await self._boss_firsts(cur, now))

            if milestones:
                await cur.executemany(
                    f"INSERT IGNORE INTO {SCHEMA}.milestones (ts, kind, detail, guid, who) "
                    "VALUES (%s,%s,%s,%s,%s)", milestones)

            if events:
                await cur.executemany(
                    f"INSERT INTO {SCHEMA}.bot_events (guid, ts, kind, detail, zone) "
                    "VALUES (%s,%s,%s,%s,%s)", events)

            # Trim on the same tick rather than with a separate schedule.
            cutoff = now - settings.history_retention_days * 86400
            await cur.execute(f"DELETE FROM {SCHEMA}.bot_events WHERE ts < %s LIMIT 5000",
                              (cutoff,))

    async def _loot(self, cur, now: float) -> list[tuple]:
        """Items that appeared since the last tick, for characters still online.

        Poor and common drops are skipped: at this population they would bury
        everything else under a stream of soul shards and grey vendor trash.
        """
        if self._max_item is None:
            return []

        await cur.execute(LOOT, (self._max_item, settings.loot_min_quality))
        rows = await cur.fetchall()
        if not rows:
            return []

        # Several stacks of one item land as several rows. Collapsed into a single
        # event with a count, rather than repeating the same line six times.
        seen: dict[tuple[int, str], list] = {}
        order: list[tuple[int, str]] = []
        for item_guid, owner, name, quality in rows:
            self._max_item = max(self._max_item, int(item_guid))
            key = (int(owner), str(name))
            if key not in seen:
                seen[key] = [int(quality), 0]
                order.append(key)
            seen[key][1] += 1

        out: list[tuple] = []
        for owner, name in order:
            quality, count = seen[(owner, name)]
            word = QUALITY_WORD.get(quality, "")
            label = f"looted {name}"
            if count > 1:
                label += f" x{count}"
            if word:
                label += f" ({word})"
            # The owner's zone from this same tick's sample - loot happens
            # where the looter stands.
            prev = self._prev.get(owner)
            out.append((owner, now, "loot", label[:160], prev[1] if prev else 0))
        return out

    async def _skillups(self, cur, now: float) -> list[tuple]:
        """Trade skill deltas since the last sample. First sighting is the baseline."""
        await cur.execute(SKILLS)
        rows = await cur.fetchall()

        out: list[tuple] = []
        first_run = not self._skills
        for guid, skill, value in rows:
            key = (guid, skill)
            prev = self._skills.get(key)
            self._skills[key] = value
            if first_run or prev is None or value <= prev:
                continue
            prev = self._prev.get(guid)
            out.append((guid, now, "profession", f"{TRADES[skill]} {value}",
                        prev[1] if prev else 0))
        return out

    async def _boss_firsts(self, cur, now: float) -> list[tuple]:
        """Newly-set encounter bits since the last sample -> realm-first candidates.

        The UNIQUE key makes repeats free, so this only has to detect NEW bits, not
        decide firstness. First run is a baseline: kills from before the recorder
        started get no timestamp rather than a wrong one.
        """
        await cur.execute(INSTANCE_BITS)
        rows = await cur.fetchall()

        first_run = not self._inst_bits and rows
        out: list[tuple] = []
        for inst_id, map_id, diff, mask in rows:
            prev = self._inst_bits.get(inst_id, 0)
            self._inst_bits[inst_id] = mask
            if first_run:
                continue
            new_bits = mask & ~prev
            if not new_bits:
                continue

            place = self._places.get(int(map_id), f"map {map_id}")
            who = ""
            await cur.execute(
                "SELECT c.name FROM acore_characters.character_instance ci "
                "JOIN acore_characters.characters c ON c.guid = ci.guid "
                "WHERE ci.instance = %s LIMIT 4", (inst_id,))
            names = [r[0] for r in await cur.fetchall()]
            if names:
                who = ", ".join(names)

            for bit in range(32):
                if not (new_bits >> bit) & 1:
                    continue
                boss = self._bosses.get((int(map_id), int(diff), bit))
                if boss:
                    out.append((now, "first_boss", f"{boss} ({place})", None, who))

            total = self._enc_totals.get((int(map_id), int(diff)), 0)
            if total and bin(mask).count("1") >= total:
                out.append((now, "first_clear", place, None, who))
        return out

    async def events(self, guid: int, limit: int = 40) -> list[dict]:
        if not self._pool:
            return []
        async with self._pool.acquire() as conn, conn.cursor() as cur:
            await cur.execute(
                f"SELECT ts, kind, detail FROM {SCHEMA}.bot_events WHERE guid=%s "
                "ORDER BY id DESC LIMIT %s", (guid, limit))
            return [{"ts": ts, "kind": k, "detail": d} for ts, k, d in await cur.fetchall()]
