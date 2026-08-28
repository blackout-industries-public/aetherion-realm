import { q } from '../utils/db'

// Market analytics: completed trades come from econ events, the live order book
// from auctionhouse rows. EconAuctionScript (NeedsLedger.cpp) logs ah_sold on
// the seller and ah_bought on the buyer in the same hook, with identical
// item/count and detail = the winning bid in copper - so sale prices are real
// and the buyer is recoverable by pairing the two events.

// The buyer subquery resolves to a guid rather than a name, so one outer join
// yields both the buyer's name and class without a second correlated lookup.
const TRADES = `
  SELECT t.ts, t.count, t.item,
         CAST(NULLIF(t.detail, '') AS SIGNED) AS price,
         cs.name AS seller, cs.class AS sellerCls, it.name AS itemName,
         cb.name AS buyer, cb.class AS buyerCls
  FROM (
    SELECT s.id, s.ts, s.count, s.item, s.detail, s.guid,
           (SELECT b.guid FROM acore_characters.aetherion_econ_events b
            WHERE b.kind = 'ah_bought' AND b.item = s.item AND b.count = s.count
              AND b.detail = s.detail AND b.ts BETWEEN s.ts - 3 AND s.ts + 3
            ORDER BY ABS(CAST(b.id AS SIGNED) - CAST(s.id AS SIGNED)) LIMIT 1) AS buyerGuid
    FROM acore_characters.aetherion_econ_events s
    WHERE s.kind = 'ah_sold'
    ORDER BY s.id DESC LIMIT 15
  ) t
  LEFT JOIN acore_characters.characters cs ON cs.guid = t.guid
  LEFT JOIN acore_characters.characters cb ON cb.guid = t.buyerGuid
  LEFT JOIN acore_world.item_template it ON it.entry = t.item
  ORDER BY t.id DESC
`

// Median approximated as AVG of non-zero buyouts - cheap, and close enough
// while per-class listing counts stay in the hundreds.
const DEPTH = `
  SELECT it.class AS cls, COUNT(*) AS listings,
         ROUND(AVG(NULLIF(ah.buyoutprice, 0))) AS avgBuyout,
         SUM(ah.buyoutprice) AS totalValue
  FROM acore_characters.auctionhouse ah
  JOIN acore_characters.item_instance ii ON ii.guid = ah.itemguid
  JOIN acore_world.item_template it ON it.entry = ii.itemEntry
  GROUP BY it.class ORDER BY listings DESC
`

const HOT = `
  SELECT it.entry, it.name, COUNT(*) AS listings, SUM(ii.count) AS units,
         MIN(NULLIF(ah.buyoutprice, 0)) AS minBuyout,
         ROUND(AVG(NULLIF(ah.buyoutprice, 0))) AS avgBuyout
  FROM acore_characters.auctionhouse ah
  JOIN acore_characters.item_instance ii ON ii.guid = ah.itemguid
  JOIN acore_world.item_template it ON it.entry = ii.itemEntry
  GROUP BY it.entry, it.name ORDER BY listings DESC LIMIT 10
`

const BIG_SALES = `
  SELECT s.ts, s.count, cs.name AS seller, cs.class AS sellerCls,
         it.name AS itemName,
         CAST(NULLIF(s.detail, '') AS SIGNED) AS price
  FROM acore_characters.aetherion_econ_events s
  LEFT JOIN acore_characters.characters cs ON cs.guid = s.guid
  LEFT JOIN acore_world.item_template it ON it.entry = s.item
  WHERE s.kind = 'ah_sold' AND s.ts > UNIX_TIMESTAMP() - 86400
  ORDER BY CAST(NULLIF(s.detail, '') AS SIGNED) DESC LIMIT 6
`

// Fallback when no sale completed in 24h: the panel then shows asking prices
// and says so, rather than inventing sale numbers.
const BIG_ASKS = `
  SELECT c.name AS seller, it.name AS itemName, ii.count, ah.buyoutprice
  FROM acore_characters.auctionhouse ah
  JOIN acore_characters.item_instance ii ON ii.guid = ah.itemguid
  JOIN acore_world.item_template it ON it.entry = ii.itemEntry
  LEFT JOIN acore_characters.characters c ON c.guid = ah.itemowner
  WHERE ah.buyoutprice > 0
  ORDER BY ah.buyoutprice DESC LIMIT 6
`

const STRIP = `
  SELECT
    (SELECT COUNT(*) FROM acore_characters.auctionhouse) AS listings,
    (SELECT COUNT(DISTINCT itemowner) FROM acore_characters.auctionhouse) AS sellers,
    (SELECT COUNT(*) FROM acore_characters.aetherion_econ_events
      WHERE kind = 'ah_sold' AND ts > UNIX_TIMESTAMP() - 86400) AS sold24,
    (SELECT COUNT(*) FROM acore_characters.aetherion_econ_events
      WHERE kind = 'ah_bought' AND ts > UNIX_TIMESTAMP() - 86400) AS bought24
`

// WotLK item_template.class ids.
const ITEM_CLASS: Record<number, string> = {
  0: 'Consumable', 1: 'Container', 2: 'Weapon', 3: 'Gem', 4: 'Armor',
  5: 'Reagent', 6: 'Projectile', 7: 'Trade Goods', 8: 'Generic', 9: 'Recipe',
  10: 'Money', 11: 'Quiver', 12: 'Quest', 13: 'Key', 14: 'Permanent',
  15: 'Miscellaneous', 16: 'Glyph',
}

export default defineCachedEventHandler(async () => {
  const [trades, depth, hot, bigSales, bigAsks, strip] = await Promise.all([
    q(TRADES), q(DEPTH), q(HOT), q(BIG_SALES), q(BIG_ASKS), q(STRIP),
  ])

  return {
    at: Date.now(),
    strip: strip[0]
      ? {
          listings: Number(strip[0].listings), sellers: Number(strip[0].sellers),
          sold24: Number(strip[0].sold24), bought24: Number(strip[0].bought24),
        }
      : null,
    trades: trades.map(r => ({
      at: Number(r.ts) * 1000,
      seller: r.seller ?? 'unknown', buyer: r.buyer ?? null,
      sellerCls: r.sellerCls == null ? null : Number(r.sellerCls),
      buyerCls: r.buyerCls == null ? null : Number(r.buyerCls),
      item: r.itemName ?? `item ${r.item}`, count: Number(r.count),
      price: r.price == null ? null : Number(r.price),
    })),
    depth: depth.map(r => ({
      name: ITEM_CLASS[Number(r.cls)] ?? `class ${r.cls}`,
      listings: Number(r.listings),
      avgBuyout: Number(r.avgBuyout ?? 0),
      totalValue: Number(r.totalValue ?? 0),
    })),
    hot: hot.map(r => ({
      name: r.name, listings: Number(r.listings), units: Number(r.units ?? 0),
      minBuyout: Number(r.minBuyout ?? 0), avgBuyout: Number(r.avgBuyout ?? 0),
    })),
    bigSales: bigSales.map(r => ({
      at: Number(r.ts) * 1000, seller: r.seller ?? 'unknown',
      sellerCls: r.sellerCls == null ? null : Number(r.sellerCls),
      item: r.itemName ?? 'unknown item', count: Number(r.count),
      price: r.price == null ? null : Number(r.price),
    })),
    bigAsks: bigAsks.map(r => ({
      seller: r.seller ?? 'unknown', item: r.itemName, count: Number(r.count),
      buyout: Number(r.buyoutprice),
    })),
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'market',
})
