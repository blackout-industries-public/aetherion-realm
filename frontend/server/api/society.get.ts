import { getPool } from '../utils/db'

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
// population is exactly 250 per class by construction, which makes the comparison fair
// without any further weighting.
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

// Level bands with wealth and how many dinged in the last day.
const LADDER = `
  SELECT p.band_order AS bandOrder, p.band, p.chars, p.avg_gold AS avgGold,
         COALESCE(d.dings, 0) AS dings
  FROM (
    SELECT CASE WHEN level=80 THEN 9 WHEN level>=70 THEN 8 WHEN level>=60 THEN 7
                WHEN level>=50 THEN 6 WHEN level>=40 THEN 5 WHEN level>=30 THEN 4
                WHEN level>=20 THEN 3 WHEN level>=10 THEN 2 ELSE 1 END AS band_order,
           CASE WHEN level=80 THEN '80' WHEN level>=70 THEN '70-79' WHEN level>=60 THEN '60-69'
                WHEN level>=50 THEN '50-59' WHEN level>=40 THEN '40-49' WHEN level>=30 THEN '30-39'
                WHEN level>=20 THEN '20-29' WHEN level>=10 THEN '10-19' ELSE '1-9' END AS band,
           COUNT(*) AS chars, ROUND(AVG(money)/10000) AS avg_gold
    FROM acore_characters.characters WHERE online = 1 GROUP BY band_order, band
  ) p
  LEFT JOIN (
    SELECT CASE WHEN lv=80 THEN 9 WHEN lv>=70 THEN 8 WHEN lv>=60 THEN 7 WHEN lv>=50 THEN 6
                WHEN lv>=40 THEN 5 WHEN lv>=30 THEN 4 WHEN lv>=20 THEN 3
                WHEN lv>=10 THEN 2 ELSE 1 END AS band_order, COUNT(*) AS dings
    FROM (SELECT CAST(REGEXP_SUBSTR(detail,'[0-9]+$') AS UNSIGNED) AS lv
          FROM aetherion_ai.bot_events
          WHERE kind='level' AND ts > UNIX_TIMESTAMP() - 86400) x
    GROUP BY band_order
  ) d ON d.band_order = p.band_order
  ORDER BY p.band_order
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
  const [mortality, whoDies, ladder, loot, balance] = await Promise.all([
    q(MORTALITY), q(WHO_DIES), q(LADDER), q(LOOT), q(BALANCE),
  ])

  const factions = new Map<string, { chars: number; atCap: number }>()
  for (const r of balance) {
    const f = factions.get(r.faction) ?? { chars: 0, atCap: 0 }
    f.chars += Number(r.chars); f.atCap += Number(r.atCap)
    factions.set(r.faction, f)
  }

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
    ladder: ladder.map(r => ({
      band: r.band, chars: Number(r.chars),
      avgGold: Number(r.avgGold), dings: Number(r.dings),
    })),
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
