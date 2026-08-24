import { q } from '../utils/db'

// Economy BRD E1.6/E1.7/E1.8: the observe-only economy panel. Needs come from the
// in-server NeedsLedger's per-minute export; gold and destruction history append
// server-side so restarts do not erase the baselines.

const NEEDS_SUMMARY = `
  SELECT need_type, COUNT(*) AS n,
         SUM(amount > 0 AND free_money >= amount) AS funded,
         SUM(amount) AS total
  FROM acore_characters.aetherion_needs
  WHERE need_type != 'persona'
  GROUP BY need_type
`

// The operator story: watch a need arise, then watch whether it gets satisfied.
// Longest-starved first - these are the bots the economy exists to serve.
const STARVED = `
  SELECT n.guid, c.name, c.level, n.need_type, n.target, n.amount, n.free_money, n.since_ts
  FROM acore_characters.aetherion_needs n
  JOIN acore_characters.characters c ON c.guid = n.guid
  WHERE n.amount > n.free_money AND n.amount > 0
    AND n.need_type NOT IN ('errand', 'persona')
  ORDER BY n.since_ts ASC LIMIT 12
`

const GOLD_BANDS = `
  SELECT band, n, total FROM (
    SELECT FLOOR(level/10) AS band, COUNT(*) AS n, SUM(money) AS total
    FROM acore_characters.aetherion_gold_now GROUP BY band
  ) x ORDER BY band
`

const GOLD_HISTORY = `
  SELECT ts, SUM(total) AS total FROM acore_characters.aetherion_gold_bands
  WHERE ts > UNIX_TIMESTAMP() - 7*86400 GROUP BY ts ORDER BY ts
`

// E1.7: gold_bands appends a snapshot every ~5 min, so per-ts SUM(total) is the
// whole fleet's copper at that instant; the slope over ~1h smooths snapshot
// noise without hiding a real trend. 24h of points feeds the sparkline.
const INCOME_POINTS = `
  SELECT ts, SUM(total) AS total FROM acore_characters.aetherion_gold_bands
  WHERE ts > UNIX_TIMESTAMP() - 86400 GROUP BY ts ORDER BY ts
`

// E9.1 groundwork: who actually earns. detail carries the copper received on
// vendor_sell events; the characters join makes each row a clickable name.
const TOP_SELLERS = `
  SELECT e.guid, c.name, c.level,
         SUM(CAST(e.detail AS SIGNED)) AS earned, COUNT(*) AS sells
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind = 'vendor_sell' AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.guid, c.name, c.level
  ORDER BY earned DESC LIMIT 8
`

const EVENTS_24H = `
  SELECT kind, COUNT(*) AS n, SUM(count) AS items
  FROM acore_characters.aetherion_econ_events
  WHERE ts > UNIX_TIMESTAMP() - 86400 GROUP BY kind
`

const RECENT_DESTROYED = `
  SELECT e.ts, e.guid, e.item, e.count, e.detail, it.name,
         c.name AS who
  FROM acore_characters.aetherion_econ_events e
  LEFT JOIN acore_world.item_template it ON it.entry = e.item
  LEFT JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind = 'destroy' ORDER BY e.id DESC LIMIT 12
`

const MARKET = `
  SELECT COUNT(*) AS listings, COUNT(DISTINCT itemowner) AS owners
  FROM acore_characters.auctionhouse
`

// The auction lifecycle as counts: attempts (ah_post) exceeding listed rows is
// the deposit-rejection story; sold vs expired is the demand story.
const AH_FLOW = `
  SELECT kind, COUNT(*) AS n, SUM(count) AS items,
         SUM(CAST(NULLIF(detail,'') AS SIGNED)) AS copper
  FROM acore_characters.aetherion_econ_events
  WHERE kind IN ('ah_post','ah_listed','ah_sold','ah_bought','ah_expired','mail_money','mail_item','craft','bank_deposit','bank_withdraw','mail_collect','gather_route')
    AND ts > UNIX_TIMESTAMP() - 86400
  GROUP BY kind
`

const AH_LISTINGS = `
  SELECT c.name AS seller, it.name AS item, ii.count, ah.buyoutprice, ah.time
  FROM acore_characters.auctionhouse ah
  JOIN acore_characters.item_instance ii ON ii.guid = ah.itemguid
  JOIN acore_world.item_template it ON it.entry = ii.itemEntry
  LEFT JOIN acore_characters.characters c ON c.guid = ah.itemowner
  ORDER BY ah.id DESC LIMIT 10
`

const AH_TOP = `
  SELECT e.kind, c.name, SUM(CAST(NULLIF(e.detail,'') AS SIGNED)) AS copper, COUNT(*) AS n
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind IN ('ah_sold','ah_bought') AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.kind, e.guid, c.name
  ORDER BY copper DESC LIMIT 12
`

export default defineEventHandler(async () => {
  const [needs, starved, bands, history, incomePts, sellers, events, destroyed, market,
         ahFlow, ahListings, ahTop] = await Promise.all([
    q(NEEDS_SUMMARY), q(STARVED), q(GOLD_BANDS), q(GOLD_HISTORY),
    q(INCOME_POINTS), q(TOP_SELLERS),
    q(EVENTS_24H), q(RECENT_DESTROYED), q(MARKET),
    q(AH_FLOW), q(AH_LISTINGS), q(AH_TOP),
  ])

  // Rate baseline: newest sample at least an hour old, so one late snapshot
  // cannot swing the number. A table younger than an hour falls back to its
  // earliest sample - the rate then reads "since measuring began".
  const pts = incomePts.map(r => ({ ts: Number(r.ts), total: Number(r.total) }))
  let copperPerHour: number | null = null
  let windowMinutes: number | null = null
  if (pts.length >= 2) {
    const last = pts[pts.length - 1]
    const base = [...pts].reverse().find(p => p.ts <= last.ts - 3600) ?? pts[0]
    const minutes = (last.ts - base.ts) / 60
    if (minutes > 0) {
      copperPerHour = Math.round(((last.total - base.total) * 60) / minutes)
      windowMinutes = Math.round(minutes)
    }
  }

  return {
    at: Date.now(),
    armed: needs.length > 0 || bands.length > 0,
    needs: needs.map(r => ({
      type: r.need_type, n: Number(r.n), funded: Number(r.funded ?? 0),
      total: Number(r.total ?? 0),
    })),
    starved: starved.map(r => ({
      guid: r.guid, name: r.name, level: Number(r.level),
      type: r.need_type, target: r.target,
      amount: Number(r.amount), free: Number(r.free_money),
      since: Number(r.since_ts) * 1000,
    })),
    goldBands: bands.map(r => ({ band: Number(r.band), n: Number(r.n), total: Number(r.total) })),
    goldHistory: history.map(r => ({ ts: Number(r.ts) * 1000, total: Number(r.total) })),
    income: {
      copperPerHour, windowMinutes,
      points: pts.map(p => ({ ts: p.ts * 1000, total: p.total })),
    },
    sellers: sellers.map(r => ({
      guid: r.guid, name: r.name, level: Number(r.level),
      earned: Number(r.earned ?? 0), sells: Number(r.sells),
    })),
    events: events.map(r => ({ kind: r.kind, n: Number(r.n), items: Number(r.items ?? 0) })),
    destroyed: destroyed.map(r => ({
      at: Number(r.ts) * 1000, who: r.who, item: r.name ?? `item ${r.item}`,
      count: Number(r.count), detail: r.detail,
    })),
    market: market[0]
      ? { listings: Number(market[0].listings), owners: Number(market[0].owners) }
      : null,
    ahFlow: Object.fromEntries(ahFlow.map(r => [r.kind, {
      n: Number(r.n), items: Number(r.items ?? 0), copper: Number(r.copper ?? 0),
    }])),
    ahListings: ahListings.map(r => ({
      seller: r.seller ?? 'unknown', item: r.item, count: Number(r.count),
      buyout: Number(r.buyoutprice), expires: Number(r.time) * 1000,
    })),
    ahTop: {
      sellers: ahTop.filter(r => r.kind === 'ah_sold')
        .map(r => ({ name: r.name, copper: Number(r.copper ?? 0), n: Number(r.n) })),
      buyers: ahTop.filter(r => r.kind === 'ah_bought')
        .map(r => ({ name: r.name, copper: Number(r.copper ?? 0), n: Number(r.n) })),
    },
  }
})
