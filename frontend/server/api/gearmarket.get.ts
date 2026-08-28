import { q } from '../utils/db'

// The gear lever, watched over days rather than minutes. Population item level
// moves far too slowly to judge in a session, so this reports the machinery
// feeding it: how many bots want gear, how many went shopping, what the market
// actually carries, and how much of what crafters make is wearable at all.
export default defineEventHandler(async () => {
  const [flow, need, shelf, crafts, buys] = await Promise.all([
    q(`SELECT kind, COUNT(*) AS n, COUNT(DISTINCT guid) AS bots
       FROM acore_characters.aetherion_econ_events
       WHERE kind IN ('gear_shop','gear_rescue','ah_bid','ah_bid_mats','ah_bought')
         AND ts > UNIX_TIMESTAMP() - 86400 GROUP BY kind`),
    q(`SELECT COUNT(*) AS want, ROUND(AVG(amount)) AS avgPrice
       FROM acore_characters.aetherion_needs
       WHERE need_type = 'gear' AND target = 'upgrade'`),
    // What is actually on the shelf. An auction house full of herbs cannot gear
    // anybody, which is the whole reason the buy side was idle for six days.
    q(`SELECT SUM(it.class IN (2,4)) AS equippable, COUNT(*) AS listings
       FROM acore_characters.auctionhouse a
       JOIN acore_characters.item_instance ii ON ii.guid = a.itemguid
       JOIN acore_world.item_template it ON it.entry = ii.itemEntry`),
    q(`SELECT SUM(it.class IN (2,4)) AS wearable, COUNT(*) AS total
       FROM acore_characters.aetherion_econ_events e
       JOIN acore_world.item_template it ON it.entry = e.item
       WHERE e.kind = 'craft' AND e.ts > UNIX_TIMESTAMP() - 86400`),
    q(`SELECT c.name AS bot, it.name AS item, it.ItemLevel AS ilvl, it.Quality AS quality,
              e.detail AS copper, e.ts
       FROM acore_characters.aetherion_econ_events e
       JOIN acore_characters.characters c ON c.guid = e.guid
       LEFT JOIN acore_world.item_template it ON it.entry = e.item
       WHERE e.kind IN ('ah_bought','ah_bid') AND it.class IN (2,4)
       ORDER BY e.ts DESC LIMIT 6`),
  ])

  const by = new Map<string, any>(flow.map(r => [String(r.kind), r]))
  const n = (k: string) => Number(by.get(k)?.n ?? 0)
  const c = crafts[0] ?? {}
  const s = shelf[0] ?? {}
  return {
    at: Date.now(),
    want: Number(need[0]?.want ?? 0),
    avgPrice: Number(need[0]?.avgPrice ?? 0),
    trips: n('gear_shop'), tripBots: Number(by.get('gear_shop')?.bots ?? 0),
    rescued: n('gear_rescue'), bids: n('ah_bid'), matBids: n('ah_bid_mats'),
    bought: n('ah_bought'),
    shelf: { equippable: Number(s.equippable ?? 0), listings: Number(s.listings ?? 0) },
    crafts: {
      wearable: Number(c.wearable ?? 0), total: Number(c.total ?? 0),
      pct: Number(c.total) ? Math.round((Number(c.wearable) / Number(c.total)) * 1000) / 10 : 0,
    },
    recent: buys.map(r => ({
      bot: r.bot, item: r.item, ilvl: Number(r.ilvl ?? 0),
      quality: Number(r.quality ?? 0), gold: Math.round(Number(r.copper ?? 0) / 10000),
      at: Number(r.ts) * 1000,
    })),
  }
})
