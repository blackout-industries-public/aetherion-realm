import { q } from '../utils/db'

// Who holds the gold: the same ledgers econ.get.ts reads, sliced by holder
// instead of by need.

const RICHEST = `
  SELECT guid, name, level, class, money, online
  FROM acore_characters.characters
  ORDER BY money DESC LIMIT 10
`

// 500g-wide buckets fit the live spread (min ~13g, mass between 1k and 4k,
// max ~4.1k); the top bucket is open-ended so inflation widens it rather
// than the chart.
const BUCKETS = `
  SELECT LEAST(FLOOR(money / 5000000), 8) AS band, COUNT(*) AS n
  FROM acore_characters.characters
  WHERE online = 1
  GROUP BY band ORDER BY band
`

const SUPPLY_NOW = `
  SELECT SUM(money) AS total, COUNT(*) AS n
  FROM acore_characters.aetherion_gold_now
`

const SUPPLY_POINTS = `
  SELECT ts, SUM(total) AS total FROM acore_characters.aetherion_gold_bands
  WHERE ts > UNIX_TIMESTAMP() - 86400 GROUP BY ts ORDER BY ts
`

// detail carries copper on both kinds - the sale's real money delta on
// vendor_sell, the amount taken from a mailbox on mail_money - verified
// against the NeedsLedger LogEvent call sites.
const EARNERS = `
  SELECT e.guid, c.name, c.level,
         SUM(CAST(NULLIF(e.detail,'') AS SIGNED)) AS earned,
         SUM(e.kind = 'vendor_sell') AS sells,
         SUM(e.kind = 'mail_money') AS collects
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind IN ('vendor_sell','mail_money') AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.guid, c.name, c.level
  ORDER BY earned DESC LIMIT 8
`

// Only flows whose copper is actually logged: repair_paid detail is the repair
// bill; AH deposits and the house cut are recorded nowhere (ah_post detail is
// the buyout price), so the sink side is labelled partial in the UI.
const FLOWS = `
  SELECT e.kind, COUNT(*) AS n,
         SUM(CAST(NULLIF(e.detail,'') AS SIGNED)) AS copper
  FROM acore_characters.aetherion_econ_events e
  WHERE e.kind IN ('vendor_sell','mail_money','repair_paid')
    AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.kind
`

export default defineEventHandler(async () => {
  const [richest, buckets, supplyNow, supplyPts, earners, flows] = await Promise.all([
    q(RICHEST), q(BUCKETS), q(SUPPLY_NOW), q(SUPPLY_POINTS), q(EARNERS), q(FLOWS),
  ])

  const flowMap = Object.fromEntries(flows.map(r => [r.kind, {
    n: Number(r.n), copper: Number(r.copper ?? 0),
  }]))

  return {
    at: Date.now(),
    richest: richest.map(r => ({
      guid: Number(r.guid), name: r.name, level: Number(r.level),
      cls: Number(r.class), money: Number(r.money), online: Number(r.online) === 1,
    })),
    buckets: buckets.map(r => ({ band: Number(r.band), n: Number(r.n) })),
    supply: {
      total: supplyNow[0] ? Number(supplyNow[0].total ?? 0) : 0,
      bots: supplyNow[0] ? Number(supplyNow[0].n ?? 0) : 0,
      points: supplyPts.map(r => ({ ts: Number(r.ts) * 1000, total: Number(r.total) })),
    },
    earners: earners.map(r => ({
      guid: Number(r.guid), name: r.name, level: Number(r.level),
      earned: Number(r.earned ?? 0), sells: Number(r.sells ?? 0),
      collects: Number(r.collects ?? 0),
    })),
    flows: {
      vendor: flowMap['vendor_sell'] ?? { n: 0, copper: 0 },
      mail: flowMap['mail_money'] ?? { n: 0, copper: 0 },
      repairs: flowMap['repair_paid'] ?? { n: 0, copper: 0 },
    },
  }
})
