import { q } from '../utils/db'

// The economy's heartbeat: hourly event pulse, the live errand census, mail
// throughput and the funding state of every recorded need.

// Aligned to full-hour buckets so the client can zero-fill a fixed 24-slot
// grid; the window starts 23 whole hours back plus the current partial hour.
const HOURLY = `
  SELECT FLOOR(ts/3600)*3600 AS hr, kind, COUNT(*) AS n
  FROM acore_characters.aetherion_econ_events
  WHERE ts > (FLOOR(UNIX_TIMESTAMP()/3600)-23)*3600
    AND kind IN ('vendor_sell','craft','ah_listed','mail_collect','gather_route')
  GROUP BY hr, kind ORDER BY hr
`

const ERRANDS = `
  SELECT target, COUNT(*) AS n
  FROM acore_characters.aetherion_needs
  WHERE need_type = 'errand'
  GROUP BY target ORDER BY n DESC
`

// Every surviving mail row is an uncollected letter; collection zeroes money
// and strips items before the row is deleted, so the split is a partition.
const MAIL_PENDING = `
  SELECT COUNT(*) AS letters,
         SUM(money > 0) AS with_money,
         SUM(money = 0 AND has_items = 1) AS with_items,
         SUM(money = 0 AND has_items = 0) AS husks,
         SUM(money) AS copper
  FROM acore_characters.mail
`

// mail_collect logs count = letters queued at one mailbox visit.
const COLLECTIONS = `
  SELECT SUM(ts > UNIX_TIMESTAMP()-3600) AS runs_1h,
         SUM(CASE WHEN ts > UNIX_TIMESTAMP()-3600 THEN count ELSE 0 END) AS letters_1h,
         COUNT(*) AS runs_24h,
         SUM(count) AS letters_24h
  FROM acore_characters.aetherion_econ_events
  WHERE kind = 'mail_collect' AND ts > UNIX_TIMESTAMP()-86400
`

const TOP_COLLECTORS = `
  SELECT e.guid, c.name, SUM(e.count) AS letters, COUNT(*) AS runs
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind = 'mail_collect' AND e.ts > UNIX_TIMESTAMP()-86400
  GROUP BY e.guid, c.name
  ORDER BY letters DESC LIMIT 5
`

// priced distinguishes "unfunded" from "unpriced": errand and materials rows
// carry amount = 0 by design, and a zero-funded bar there would read as famine.
const NEEDS_FUNDING = `
  SELECT need_type, COUNT(*) AS n,
         SUM(amount > 0) AS priced,
         SUM(amount > 0 AND free_money >= amount) AS funded
  FROM acore_characters.aetherion_needs
  GROUP BY need_type ORDER BY n DESC
`

export default defineEventHandler(async () => {
  const [hourly, errands, mailPending, collections, collectors, needs] = await Promise.all([
    q(HOURLY), q(ERRANDS), q(MAIL_PENDING), q(COLLECTIONS), q(TOP_COLLECTORS), q(NEEDS_FUNDING),
  ])

  const mp = mailPending[0]
  const col = collections[0]

  return {
    at: Date.now(),
    hourly: hourly.map(r => ({ ts: Number(r.hr) * 1000, kind: r.kind, n: Number(r.n) })),
    errands: errands.map(r => ({ target: r.target, n: Number(r.n) })),
    mail: mp
      ? {
          letters: Number(mp.letters), withMoney: Number(mp.with_money ?? 0),
          withItems: Number(mp.with_items ?? 0), husks: Number(mp.husks ?? 0),
          copper: Number(mp.copper ?? 0),
        }
      : null,
    collections: col
      ? {
          runs1h: Number(col.runs_1h ?? 0), letters1h: Number(col.letters_1h ?? 0),
          runs24h: Number(col.runs_24h ?? 0), letters24h: Number(col.letters_24h ?? 0),
        }
      : null,
    collectors: collectors.map(r => ({
      guid: r.guid, name: r.name, letters: Number(r.letters), runs: Number(r.runs),
    })),
    needs: needs.map(r => ({
      type: r.need_type, n: Number(r.n),
      priced: Number(r.priced ?? 0), funded: Number(r.funded ?? 0),
    })),
  }
})
