// Demographics and the shape of life on the realm. Every query here was checked against
// live data first; several obvious-looking panels were dropped because their tables turn
// out to be frozen rather than quiet - see the notes on each.

// Deaths and revives per hour. The only recorded evidence that combat happens at all:
// this schema has no combat log.
const MORTALITY = `
  SELECT DATE_FORMAT(FROM_UNIXTIME(ts), '%Y-%m-%d %H:00') AS hour_bucket,
         SUM(kind='death')  AS deaths,
         SUM(kind='revive') AS revives,
         COUNT(DISTINCT CASE WHEN kind='death' THEN guid END) AS bots_died
  FROM aetherion_ai.bot_events
  WHERE kind IN ('death','revive') AND ts > UNIX_TIMESTAMP() - 86400
  GROUP BY hour_bucket
  ORDER BY hour_bucket
`

// Deaths per class, normalised by how many of that class are actually exposed. The
// population is seeded uniformly per class by construction, which makes the comparison
// fair without any further weighting - but the exact headcount is read live (CLASS_POP)
// rather than asserted, because reseeds have changed it before.
const WHO_DIES = `
  SELECT p.class AS cls,
         p.online_55plus            AS atRisk,
         COALESCE(d.deaths, 0)      AS deaths,
         COALESCE(d.avg_level, 0)   AS avgLevel,
         ROUND(COALESCE(d.deaths,0) / p.online_55plus, 3) AS perBot
  FROM (
    SELECT class, SUM(level >= 55 AND online = 1) AS online_55plus
    FROM acore_characters.characters GROUP BY class
  ) p
  LEFT JOIN (
    SELECT c.class, COUNT(*) AS deaths, ROUND(AVG(c.level),1) AS avg_level
    FROM aetherion_ai.bot_events e
    JOIN acore_characters.characters c ON c.guid = e.guid
    WHERE e.kind = 'death' AND e.ts > UNIX_TIMESTAMP() - 86400
      AND c.level >= 55 AND c.online = 1
    GROUP BY c.class
  ) d ON d.class = p.class
  WHERE p.online_55plus > 0
  ORDER BY perBot DESC
`

// The per-class headcount range, so the "N per class" claim on screen is measured,
// not remembered.
const CLASS_POP = `
  SELECT MIN(n) AS lo, MAX(n) AS hi
  FROM (SELECT COUNT(*) AS n FROM acore_characters.characters GROUP BY class) c
`

// Loot by rarity per hour. The recorder only writes uncommon and above, so this is a
// proxy for "how much content is actually being killed".
const LOOT = `
  SELECT FROM_UNIXTIME(bucket, '%H:00') AS hr, rare, uncommon, epic, total, looters
  FROM (
    SELECT ts - (ts MOD 3600) AS bucket,
           SUM(detail LIKE '%(rare)%')     AS rare,
           SUM(detail LIKE '%(uncommon)%') AS uncommon,
           SUM(detail LIKE '%(epic)%')     AS epic,
           COUNT(*)                        AS total,
           COUNT(DISTINCT guid)            AS looters
    FROM aetherion_ai.bot_events
    WHERE kind='loot' AND ts > UNIX_TIMESTAMP() - 21600
    GROUP BY ts - (ts MOD 3600)
  ) b ORDER BY bucket
`

const BALANCE = `
  SELECT CASE WHEN race IN (1,3,4,7,11) THEN 'alliance' ELSE 'horde' END AS faction,
         CASE race WHEN 1 THEN 'Human' WHEN 2 THEN 'Orc' WHEN 3 THEN 'Dwarf'
                   WHEN 4 THEN 'Night Elf' WHEN 5 THEN 'Undead' WHEN 6 THEN 'Tauren'
                   WHEN 7 THEN 'Gnome' WHEN 8 THEN 'Troll' WHEN 10 THEN 'Blood Elf'
                   WHEN 11 THEN 'Draenei' ELSE 'Other' END AS race,
         COUNT(*) AS chars, ROUND(AVG(level),1) AS avgLevel,
         SUM(level=80) AS atCap
  FROM acore_characters.characters WHERE online = 1
  GROUP BY faction, race ORDER BY faction, chars DESC
`


export default defineEventHandler(async () => {
  const [mortality, whoDies, classPop, loot, balance] = await Promise.all([
    q(MORTALITY), q(WHO_DIES), q(CLASS_POP), q(LOOT), q(BALANCE),
  ])

  const factions = new Map<string, { chars: number; atCap: number }>()
  for (const r of balance) {
    const f = factions.get(r.faction) ?? { chars: 0, atCap: 0 }
    f.chars += Number(r.chars); f.atCap += Number(r.atCap)
    factions.set(r.faction, f)
  }

  const pop = classPop[0]

  return {
    at: Date.now(),
    mortality: mortality.map(r => ({
      hour: String(r.hour_bucket).slice(11, 16),
      deaths: Number(r.deaths), revives: Number(r.revives), bots: Number(r.bots_died),
    })),
    whoDies: whoDies.map(r => ({
      cls: Number(r.cls), atRisk: Number(r.atRisk), deaths: Number(r.deaths),
      avgLevel: Number(r.avgLevel), perBot: Number(r.perBot),
    })),
    classPop: pop ? { lo: Number(pop.lo), hi: Number(pop.hi) } : null,
    loot: loot.map(r => ({
      hour: r.hr, rare: Number(r.rare), uncommon: Number(r.uncommon),
      epic: Number(r.epic), total: Number(r.total), looters: Number(r.looters),
    })),
    balance: balance.map(r => ({
      faction: r.faction, race: r.race, chars: Number(r.chars),
      avgLevel: Number(r.avgLevel), atCap: Number(r.atCap),
    })),
    factions: [...factions.entries()].map(([faction, v]) => ({ faction, ...v })),
  }
})
