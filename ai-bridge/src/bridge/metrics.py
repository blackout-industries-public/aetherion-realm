"""Prometheus exposition for the realm economy (Economy BRD E9.3).

Hand-rolled text format rather than the client library: every series here is a
straight SQL aggregate scraped on demand, so a dependency would buy nothing and
the bridge keeps importing with only what it already ships.

Each query is fenced on its own. The econ tables are created by the worldserver
module and may not exist yet on a fresh realm, and a scrape that 500s over one
missing table would blank every Grafana panel instead of one.
"""
from __future__ import annotations

import logging

import aiomysql

from .memory import SCHEMA

log = logging.getLogger("bridge.metrics")

POPULATION = """
    SELECT COUNT(*), COALESCE(SUM(money), 0)
    FROM acore_characters.characters WHERE online = 1
"""

LEVEL_BANDS = """
    SELECT FLOOR(level / 10), COUNT(*)
    FROM acore_characters.characters WHERE online = 1
    GROUP BY 1
"""

# funded mirrors the dashboard's definition (frontend econ.get.ts): a need with
# amount 0 is a placeholder, not a satisfied need, so it never counts as funded.
NEEDS = """
    SELECT need_type, COUNT(*), SUM(amount > 0 AND free_money >= amount)
    FROM acore_characters.aetherion_needs
    GROUP BY need_type
"""

ECON_EVENTS = """
    SELECT kind, COUNT(*)
    FROM acore_characters.aetherion_econ_events
    GROUP BY kind
"""

AUCTIONS = """
    SELECT COUNT(*), COUNT(DISTINCT itemowner)
    FROM acore_characters.auctionhouse
"""

FIRSTS = f"""
    SELECT COUNT(*) FROM {SCHEMA}.milestones WHERE kind = 'first_level'
"""


def _escape(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n")


def _emit(out: list[str], name: str, mtype: str, help_text: str,
          samples: list[tuple[dict[str, str] | None, int]]) -> None:
    out.append(f"# HELP {name} {help_text}")
    out.append(f"# TYPE {name} {mtype}")
    for labels, value in samples:
        if labels:
            body = ",".join(f'{k}="{_escape(str(v))}"' for k, v in labels.items())
            out.append(f"{name}{{{body}}} {value}")
        else:
            out.append(f"{name} {value}")


async def _rows(cur, sql: str) -> list | None:
    """None means the query itself failed - the caller skips that family.

    An empty result is different: the table exists and the honest value is zero.
    """
    try:
        await cur.execute(sql)
        return list(await cur.fetchall())
    except aiomysql.Error as exc:
        log.debug("metric query skipped: %s", exc)
        return None


async def prometheus_text(pool: aiomysql.Pool) -> str:
    out: list[str] = []
    async with pool.acquire() as conn, conn.cursor() as cur:
        rows = await _rows(cur, POPULATION)
        if rows:
            online, copper = rows[0]
            _emit(out, "aetherion_bots_online", "gauge",
                  "Characters currently online.", [(None, int(online))])
            _emit(out, "aetherion_gold_supply_copper", "gauge",
                  "Total copper held by online characters.", [(None, int(copper))])

        rows = await _rows(cur, LEVEL_BANDS)
        if rows is not None:
            counts = {int(band): int(n) for band, n in rows}
            # All nine bands, zero-filled: a band draining to empty should show
            # a zero, not a gap Grafana renders as "no data".
            _emit(out, "aetherion_level_bots", "gauge",
                  "Online characters per 10-level band (band = FLOOR(level/10)).",
                  [({"band": str(b)}, counts.get(b, 0)) for b in range(9)])

        rows = await _rows(cur, NEEDS)
        if rows is not None:
            _emit(out, "aetherion_needs", "gauge",
                  "Open needs in the ledger, by need type.",
                  [({"type": t}, int(n)) for t, n, _ in rows])
            _emit(out, "aetherion_needs_funded", "gauge",
                  "Needs whose holder can already afford them, by need type.",
                  [({"type": t}, int(funded or 0)) for t, _, funded in rows])

        rows = await _rows(cur, ECON_EVENTS)
        if rows is not None:
            _emit(out, "aetherion_econ_events_total", "counter",
                  "Economy events logged by the NeedsLedger, by kind (all time).",
                  [({"kind": k}, int(n)) for k, n in rows])

        rows = await _rows(cur, AUCTIONS)
        if rows:
            listings, sellers = rows[0]
            _emit(out, "aetherion_ah_listings", "gauge",
                  "Live auction house listings.", [(None, int(listings))])
            _emit(out, "aetherion_ah_sellers", "gauge",
                  "Distinct characters with at least one live listing.",
                  [(None, int(sellers))])

        rows = await _rows(cur, FIRSTS)
        if rows:
            _emit(out, "aetherion_race_level_firsts", "gauge",
                  "Level milestones claimed as realm firsts.",
                  [(None, int(rows[0][0]))])

    return "\n".join(out) + "\n"
