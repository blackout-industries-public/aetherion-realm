import { getPool } from '../utils/db'

// Skill ids are stable game data. Gathering and crafting are separated because the
// interesting question is not "who has Mining" but whether the realm's economy has both
// halves of a supply chain.
const TRADES = [
  { skill: 186, name: 'Mining', kind: 'gathering' },
  { skill: 182, name: 'Herbalism', kind: 'gathering' },
  { skill: 393, name: 'Skinning', kind: 'gathering' },
  { skill: 164, name: 'Blacksmithing', kind: 'crafting' },
  { skill: 165, name: 'Leatherworking', kind: 'crafting' },
  { skill: 171, name: 'Alchemy', kind: 'crafting' },
  { skill: 197, name: 'Tailoring', kind: 'crafting' },
  { skill: 202, name: 'Engineering', kind: 'crafting' },
  { skill: 333, name: 'Enchanting', kind: 'crafting' },
  { skill: 755, name: 'Jewelcrafting', kind: 'crafting' },
  { skill: 773, name: 'Inscription', kind: 'crafting' },
]
const SECONDARY = [
  { skill: 129, name: 'First Aid' },
  { skill: 185, name: 'Cooking' },
  { skill: 356, name: 'Fishing' },
]

const ALL_SKILLS = [...TRADES, ...SECONDARY].map(t => t.skill).join(',')

// Holders and where they are along the 1-450 track. A count alone would not show whether
// the realm is full of apprentices or grand masters.
const SPREAD = `
  SELECT cs.skill,
         COUNT(*)                  AS holders,
         ROUND(AVG(cs.value))      AS avgSkill,
         SUM(cs.value >= 450)      AS grandMaster,
         SUM(cs.value >= 375 AND cs.value < 450) AS master,
         SUM(cs.value >= 225 AND cs.value < 375) AS journeyman,
         SUM(cs.value <  225)      AS apprentice
  FROM acore_characters.character_skills cs
  JOIN acore_characters.characters c ON c.guid = cs.guid AND c.online = 1
  WHERE cs.skill IN (${ALL_SKILLS})
  GROUP BY cs.skill
`

const TOP_ARTISANS = `
  SELECT c.name, c.class AS cls, c.level, cs.skill, cs.value
  FROM acore_characters.character_skills cs
  JOIN acore_characters.characters c ON c.guid = cs.guid AND c.online = 1
  WHERE cs.skill IN (${TRADES.map(t => t.skill).join(',')}) AND cs.value >= 400
  ORDER BY cs.value DESC, c.level DESC
  LIMIT 14
`

// What the realm is actually carrying. Quality is the only economic signal that moves:
// the auction house has been frozen since it was seeded.
const GEAR = `
  SELECT it.Quality AS quality, COUNT(*) AS items
  FROM acore_characters.item_instance ii
  JOIN acore_world.item_template it ON it.entry = ii.itemEntry
  GROUP BY it.Quality ORDER BY it.Quality DESC
`

const BEST_ITEMS = `
  SELECT it.name, it.Quality AS quality, it.ItemLevel AS ilvl, COUNT(*) AS copies
  FROM acore_characters.item_instance ii
  JOIN acore_world.item_template it ON it.entry = ii.itemEntry
  WHERE it.Quality >= 4
  GROUP BY it.name, it.Quality, it.ItemLevel
  ORDER BY it.Quality DESC, it.ItemLevel DESC
  LIMIT 12
`

// The auction house, stated as it is. Seeded, never traded against.
const AUCTION = `
  SELECT COUNT(*) AS listings,
         SUM(a.buyoutprice > 0) AS withBuyout,
         SUM(a.lastbid > 0)     AS everBid,
         SUM(a.buyguid > 0)     AS everBought,
         ROUND(AVG(a.buyoutprice) / 10000) AS avgBuyoutGold
  FROM acore_characters.auctionhouse a
`


const QUALITY = ['poor', 'common', 'uncommon', 'rare', 'epic', 'legendary', 'artifact']

export default defineCachedEventHandler(async () => {
  const [spread, artisans, gear, best, auction] = await Promise.all([
    q(SPREAD), q(TOP_ARTISANS), q(GEAR), q(BEST_ITEMS), q(AUCTION),
  ])

  const bySkill = new Map<number, any>()
  for (const r of spread) bySkill.set(Number(r.skill), r)

  const shape = (t: { skill: number; name: string; kind?: string }) => {
    const r = bySkill.get(t.skill)
    if (!r) return null
    return {
      skill: t.skill, name: t.name, kind: t.kind ?? 'secondary',
      holders: Number(r.holders), avgSkill: Number(r.avgSkill),
      grandMaster: Number(r.grandMaster), master: Number(r.master),
      journeyman: Number(r.journeyman), apprentice: Number(r.apprentice),
    }
  }

  const trades = TRADES.map(shape).filter(Boolean) as any[]
  const secondary = SECONDARY.map(shape).filter(Boolean) as any[]
  const a = auction[0] ?? {}

  return {
    at: Date.now(),
    trades,
    secondary,
    totals: {
      gathering: trades.filter(t => t.kind === 'gathering').reduce((n, t) => n + t.holders, 0),
      crafting: trades.filter(t => t.kind === 'crafting').reduce((n, t) => n + t.holders, 0),
      grandMasters: trades.reduce((n, t) => n + t.grandMaster, 0),
    },
    artisans: artisans.map(r => ({
      name: r.name, cls: Number(r.cls), level: Number(r.level),
      trade: TRADES.find(t => t.skill === Number(r.skill))?.name ?? `Skill ${r.skill}`,
      value: Number(r.value),
    })),
    gear: gear.map(r => ({
      quality: Number(r.quality),
      label: QUALITY[Number(r.quality)] ?? `q${r.quality}`,
      items: Number(r.items),
    })),
    best: best.map(r => ({
      name: r.name, quality: Number(r.quality),
      ilvl: Number(r.ilvl), copies: Number(r.copies),
    })),
    auction: {
      listings: Number(a.listings ?? 0),
      withBuyout: Number(a.withBuyout ?? 0),
      everBid: Number(a.everBid ?? 0),
      everBought: Number(a.everBought ?? 0),
      avgBuyoutGold: Number(a.avgBuyoutGold ?? 0),
    },
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'professions',
})
