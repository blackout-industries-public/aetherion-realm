import { q } from '../utils/db'

// Production and gathering. Craft events log item=product and count=yield per
// cast (EconCraftAction.cpp); gather_route logs one row per node arrival with
// item=GO entry (patch_econ_idle.py), so COUNT(*) is trips, not stacks looted.

const CRAFT_TOTALS = `
  SELECT COUNT(*) AS casts, SUM(count) AS items, COUNT(DISTINCT guid) AS crafters
  FROM acore_characters.aetherion_econ_events
  WHERE kind = 'craft' AND ts > UNIX_TIMESTAMP() - 86400
`

const CRAFT_PRODUCTS = `
  SELECT e.item, it.name, SUM(e.count) AS crafted, COUNT(*) AS casts,
         COUNT(DISTINCT e.guid) AS crafters
  FROM acore_characters.aetherion_econ_events e
  LEFT JOIN acore_world.item_template it ON it.entry = e.item
  WHERE e.kind = 'craft' AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.item, it.name
  ORDER BY crafted DESC LIMIT 10
`

const TOP_CRAFTERS = `
  SELECT e.guid, c.name, c.level, SUM(e.count) AS crafted,
         COUNT(DISTINCT e.item) AS products
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind = 'craft' AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.guid, c.name, c.level
  ORDER BY crafted DESC LIMIT 8
`

const GATHER_TOTALS = `
  SELECT COUNT(*) AS trips, COUNT(DISTINCT guid) AS gatherers
  FROM acore_characters.aetherion_econ_events
  WHERE kind = 'gather_route' AND ts > UNIX_TIMESTAMP() - 86400
`

const GATHER_HOURLY = `
  SELECT FLOOR((UNIX_TIMESTAMP() - ts) / 3600) AS hours_ago, COUNT(*) AS trips
  FROM acore_characters.aetherion_econ_events
  WHERE kind = 'gather_route' AND ts > UNIX_TIMESTAMP() - 86400
  GROUP BY hours_ago
`

const TOP_GATHERERS = `
  SELECT e.guid, c.name, c.level, COUNT(*) AS trips, COUNT(DISTINCT e.item) AS nodes
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind = 'gather_route' AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.guid, c.name, c.level
  ORDER BY trips DESC LIMIT 8
`

const NODES_VISITED = `
  SELECT e.item, gt.name, COUNT(*) AS visits, COUNT(DISTINCT e.guid) AS gatherers
  FROM acore_characters.aetherion_econ_events e
  LEFT JOIN acore_world.gameobject_template gt ON gt.entry = e.item
  WHERE e.kind = 'gather_route' AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY e.item, gt.name
  ORDER BY visits DESC LIMIT 10
`

// One row per profession: practitioner count among online bots plus the current
// leader. Window functions keep it a single scan of character_skills.
const PROFESSIONS = `
  SELECT skill, n, topval, name FROM (
    SELECT s.skill, COUNT(*) OVER (PARTITION BY s.skill) AS n,
           s.value AS topval, c.name,
           ROW_NUMBER() OVER (PARTITION BY s.skill ORDER BY s.value DESC, c.name) AS rn
    FROM acore_characters.character_skills s
    JOIN acore_characters.characters c ON c.guid = s.guid AND c.online = 1
    WHERE s.skill IN (171,164,333,202,165,197,182,186,393)
  ) x WHERE rn = 1 ORDER BY n DESC
`

export default defineEventHandler(async () => {
  const [craftTotals, products, crafters, gatherTotals, gatherHourly, gatherers,
         nodes, professions] = await Promise.all([
    q(CRAFT_TOTALS), q(CRAFT_PRODUCTS), q(TOP_CRAFTERS),
    q(GATHER_TOTALS), q(GATHER_HOURLY), q(TOP_GATHERERS),
    q(NODES_VISITED), q(PROFESSIONS),
  ])

  // Dense 24 buckets, oldest first, so the spark never shifts when hours are empty.
  const byHour = new Map(gatherHourly.map(r => [Number(r.hours_ago), Number(r.trips)]))
  const hourly = Array.from({ length: 24 }, (_, i) => {
    const h = 23 - i
    return { hoursAgo: h, trips: byHour.get(h) ?? 0 }
  })

  return {
    at: Date.now(),
    craft: {
      casts: Number(craftTotals[0]?.casts ?? 0),
      items: Number(craftTotals[0]?.items ?? 0),
      crafters: Number(craftTotals[0]?.crafters ?? 0),
    },
    products: products.map(r => ({
      item: Number(r.item), name: r.name ?? `item ${r.item}`,
      crafted: Number(r.crafted ?? 0), casts: Number(r.casts),
      crafters: Number(r.crafters),
    })),
    crafters: crafters.map(r => ({
      guid: r.guid, name: r.name, level: Number(r.level),
      crafted: Number(r.crafted ?? 0), products: Number(r.products),
    })),
    gather: {
      trips: Number(gatherTotals[0]?.trips ?? 0),
      gatherers: Number(gatherTotals[0]?.gatherers ?? 0),
    },
    gatherHourly: hourly,
    gatherers: gatherers.map(r => ({
      guid: r.guid, name: r.name, level: Number(r.level),
      trips: Number(r.trips), nodes: Number(r.nodes),
    })),
    nodes: nodes.map(r => ({
      entry: Number(r.item), name: r.name ?? `object ${r.item}`,
      visits: Number(r.visits), gatherers: Number(r.gatherers),
    })),
    professions: professions.map(r => ({
      skill: Number(r.skill), n: Number(r.n),
      top: Number(r.topval), leader: r.name,
    })),
  }
})
