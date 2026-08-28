import { instanceNames } from '../utils/places'

// The realm's progression frontier, all eras. Two kinds of gate exist and the
// game stores them differently: dungeon_access_requirements carries the ones
// the core enforces at the door (TBC heroic keys, Caverns of Time, the Icecrown
// five-mans, the heroic raid achievements), while classic and TBC raid
// attunements are enforced by scripts and gameobject conditions, so only their
// quest ids can be named here. Everything else - names, boss counts, who holds
// what - is read from the world and character tables.

// Blizzard removed these requirements in 3.0, so on a Wrath realm they are
// history rather than a door: shown because the progression story starts here,
// flagged so nobody reads a zero as a blocked party.
const RAID_GATES = [
  { era: 'classic', map: 409, quests: [7848, 7487] },
  { era: 'classic', map: 249, quests: [6502] },
  { era: 'classic', map: 469, quests: [7761] },
  { era: 'classic', map: 531, quests: [8743] },
  { era: 'tbc', map: 532, quests: [9837] },
  { era: 'tbc', map: 534, quests: [10445, 13432] },
  { era: 'tbc', map: 564, quests: [10949] },
]

// The old world worth walking back into: every raid released before Wrath.
const OLD_RAIDS: Record<string, number[]> = {
  classic: [249, 309, 409, 469, 509, 531],
  tbc: [532, 534, 544, 548, 550, 564, 565, 568, 580],
}

const ERAS = [
  { key: 'classic', name: 'Classic', level: 60 },
  { key: 'tbc', name: 'Burning Crusade', level: 70 },
  { key: 'wrath', name: 'Wrath', level: 80 },
]

// Requirement types as the core reads them.
const KIND = ['achievement', 'quest', 'item'] as const

// Map ids sort by expansion except for the Caverns of Time, which sit among the
// classic ranges but belong to Burning Crusade.
const eraOf = (map: number) =>
  map === 269 || map === 560 ? 'tbc' : map >= 600 ? 'wrath' : map >= 500 ? 'tbc' : 'classic'

const strip = (s: string) =>
  String(s ?? '').replace(/\s*-\s*\d+\s*man.*$/i, '')
    .replace(/\s*-\s*(?:normal|heroic).*$/i, '')
    .replace(/\s*\((?:DM|WC|ZG|BWL|AQ\d+)\)\s*$/i, '')
    .split('/')[0]!.trim()

const tidy = (s: string) => strip(s).split(',').pop()!.trim()

// A raid is named by its first segment; only wings hide behind the comma.
const raidName = (s: string) => strip(s).split(',')[0]!.trim()

export default defineCachedEventHandler(async () => {
  const NAMES = await instanceNames()

  const dbGates = await q(`
    SELECT r.requirement_type AS type, r.requirement_id AS id, r.faction,
           t.map_id AS map, t.difficulty, r.requirement_note AS note, r.comment AS label
    FROM acore_world.dungeon_access_requirements r
    JOIN acore_world.dungeon_access_template t ON t.id = r.dungeon_access_id
  `)

  // One row per (gate, requirement): the same key opens several dungeons and the
  // same dungeon is gated on both difficulties, so they collapse by requirement.
  type Gate = {
    era: string; kind: string; id: number; name: string; opens: Set<string>
    holders: number; inProgress: number; heroic: boolean; legacy: boolean
  }
  const gates = new Map<string, Gate>()
  const questIds = new Set<number>()
  const itemIds = new Set<number>()
  const achIds = new Set<number>()

  for (const r of dbGates) {
    const kind = KIND[Number(r.type)] ?? 'other'
    const key = `${kind}:${r.id}`
    const g = gates.get(key) ?? {
      era: eraOf(Number(r.map)), kind, id: Number(r.id),
      name: Number(r.type) === 0
        ? String(r.label || r.note || '').trim()
        : tidy(r.note || r.label || ''),
      opens: new Set<string>(),
      holders: 0, inProgress: 0, heroic: Number(r.difficulty) > 0, legacy: false,
    }
    g.opens.add(NAMES[Number(r.map)] ? tidy(NAMES[Number(r.map)]!) : `Map ${r.map}`)
    gates.set(key, g)
    if (kind === 'quest') questIds.add(Number(r.id))
    else if (kind === 'item') itemIds.add(Number(r.id))
    else if (kind === 'achievement') achIds.add(Number(r.id))
  }

  for (const g of RAID_GATES) {
    for (const quest of g.quests) questIds.add(quest)
    gates.set(`quest:${g.quests[0]}`, {
      era: g.era, kind: 'quest', id: g.quests[0]!, name: '',
      opens: new Set([NAMES[g.map] ? tidy(NAMES[g.map]!) : `Map ${g.map}`]),
      holders: 0, inProgress: 0, heroic: false, legacy: true,
    })
  }
  // Alternate ids for the same attunement roll up into its primary row.
  const alias = new Map<number, number>()
  for (const g of RAID_GATES) for (const quest of g.quests) alias.set(quest, g.quests[0]!)

  const inList = (s: Set<number>) => [...s].join(',') || '0'
  const [questNames, itemNames, rewarded, active, held, earned, frontier, earning,
         liveKills, namedKills, collectorCount, collectorRuns,
         huntPhases, huntRecent, huntWho, legendary] =
    await Promise.all([
      q(`SELECT ID AS id, LogTitle AS name FROM acore_world.quest_template WHERE ID IN (${inList(questIds)})`),
      q(`SELECT entry AS id, name FROM acore_world.item_template WHERE entry IN (${inList(itemIds)})`),
      q(`SELECT quest AS id, COUNT(*) AS n FROM acore_characters.character_queststatus_rewarded
         WHERE quest IN (${inList(questIds)}) GROUP BY quest`),
      q(`SELECT quest AS id, COUNT(*) AS n FROM acore_characters.character_queststatus
         WHERE quest IN (${inList(questIds)}) GROUP BY quest`),
      q(`SELECT itemEntry AS id, COUNT(DISTINCT owner_guid) AS n FROM acore_characters.item_instance
         WHERE itemEntry IN (${inList(itemIds)}) GROUP BY itemEntry`),
      q(`SELECT achievement AS id, COUNT(DISTINCT guid) AS n FROM acore_characters.character_achievement
         WHERE achievement IN (${inList(achIds)}) GROUP BY achievement`),
      // Every old raid, from OUR durable ledger rather than the core's `instance`
      // table - that one is live state, pruned on reset, so a cleared raid
      // silently reverts to untouched. Depth of the best clear is the honest
      // frontier measure; summing runs would double-count repeat kills.
      q(`SELECT t.map_id AS map, MIN(t.comment) AS label,
                COUNT(h.id) AS visits, COALESCE(MAX(h.bosses_downed), 0) AS killed,
                (SELECT COUNT(*) FROM acore_world.dungeonencounter_dbc d2
                  WHERE d2.MapID = t.map_id AND d2.Difficulty = 0) AS bosses
         FROM acore_world.dungeon_access_template t
         LEFT JOIN acore_characters.aetherion_run_history h ON h.map = t.map_id
         WHERE t.difficulty = 0
           AND t.map_id IN (${[...Object.values(OLD_RAIDS)].flat().join(',')})
         GROUP BY t.map_id`),
      q(`SELECT i.map AS map,
                COUNT(DISTINCT IF(i.completedEncounters & (1 << de.Bit), de.ID, NULL)) AS killed
         FROM acore_characters.instance i
         JOIN acore_world.dungeonencounter_dbc de
           ON de.MapID = i.map AND de.Difficulty = i.difficulty
         WHERE i.map IN (${[...Object.values(OLD_RAIDS)].flat().join(',')})
         GROUP BY i.map`),
      q(`SELECT le.map AS map, ct.name AS boss
         FROM acore_characters.log_encounter le
         LEFT JOIN acore_world.creature_template ct ON ct.entry = le.creditEntry
         WHERE ct.name IS NOT NULL GROUP BY le.map, ct.name
         UNION
         SELECT i.map AS map, COALESCE(ct2.name, ie.comment) AS boss
         FROM acore_characters.instance i
         JOIN acore_world.dungeonencounter_dbc de
           ON de.MapID = i.map AND de.Difficulty = i.difficulty
         JOIN acore_world.instance_encounters ie ON ie.entry = de.ID
         LEFT JOIN acore_world.creature_template ct2
           ON ct2.entry = ie.creditEntry AND ie.creditType = 0
         WHERE i.completedEncounters & (1 << de.Bit)
         GROUP BY i.map, boss
         UNION
         SELECT k.map AS map, k.name AS boss
         FROM acore_characters.aetherion_run_kills k GROUP BY k.map, k.name`),
      // Attunement runs the assembler is undertaking right now.
      q(`SELECT attunement AS name, COUNT(*) AS runs,
                SUM(ended_at = 0) AS underway
         FROM acore_characters.aetherion_run_history
         WHERE attunement <> '' GROUP BY attunement ORDER BY runs DESC`),
      // The collectors themselves: how many bots have the disposition, and what
      // their expeditions into old content have done.
      q(`SELECT COUNT(*) AS n FROM acore_characters.aetherion_needs
         WHERE need_type = 'persona' AND target = 'collector'`),
      q(`SELECT dungeon, leader, size, bosses_downed AS bosses, outcome,
                ended_at = 0 AS live, started_at
         FROM acore_characters.aetherion_run_history
         WHERE flavor = 'collector' ORDER BY started_at DESC LIMIT 8`),
      // The hunters. detail is "<phase>|<mapId>|<field3>"; field3 carries the
      // rare's name on aim, kill and died. A 'bonus' is a rare killed in passing
      // and is deliberately NOT counted as a hunt closing.
      q(`SELECT SUBSTRING_INDEX(detail, '|', 1) AS phase,
                COUNT(*) AS n, COUNT(DISTINCT guid) AS bots
         FROM acore_characters.aetherion_econ_events
         WHERE kind = 'rare_hunt' GROUP BY phase`),
      q(`SELECT e.ts, SUBSTRING_INDEX(e.detail, '|', 1) AS phase,
                SUBSTRING_INDEX(e.detail, '|', -1) AS quarry, e.count AS lvl, c.name AS bot
         FROM acore_characters.aetherion_econ_events e
         JOIN acore_characters.characters c ON c.guid = e.guid
         WHERE e.kind = 'rare_hunt'
           AND SUBSTRING_INDEX(e.detail, '|', 1) IN ('aim','kill','died')
         ORDER BY e.ts DESC LIMIT 8`),
      q(`SELECT
           (SELECT COUNT(*) FROM acore_characters.aetherion_needs
             WHERE need_type = 'persona' AND target = 'hunter') AS hunters,
           (SELECT COUNT(*) FROM acore_characters.aetherion_needs
             WHERE need_type = 'errand' AND target = 'rare') AS hunting`),
      // Legendaries in existence on the realm - the collector's scoreboard.
      q(`SELECT it.name, COUNT(DISTINCT ii.owner_guid) AS owners
         FROM acore_characters.item_instance ii
         JOIN acore_world.item_template it ON it.entry = ii.itemEntry
         WHERE it.Quality = 5 GROUP BY it.name ORDER BY owners DESC LIMIT 10`),
    ])

  const nameById = (rows: any[]) =>
    new Map<number, string>(rows.map(r => [Number(r.id), String(r.name ?? '')]))
  const countById = (rows: any[]) =>
    new Map<number, number>(rows.map(r => [Number(r.id), Number(r.n ?? 0)]))

  const qNames = nameById(questNames)
  const iNames = nameById(itemNames)
  const rewardedBy = countById(rewarded)
  const activeBy = countById(active)
  const heldBy = countById(held)
  const earnedBy = countById(earned)

  for (const g of gates.values()) {
    if (g.kind === 'quest') {
      g.name = qNames.get(g.id) || g.name || `Quest ${g.id}`
      // Alternate ids of the same attunement count towards the primary row.
      for (const [from, to] of alias) {
        if (to !== g.id) continue
        g.holders += rewardedBy.get(from) ?? 0
        g.inProgress += activeBy.get(from) ?? 0
      }
      if (!alias.has(g.id)) {
        g.holders = rewardedBy.get(g.id) ?? 0
        g.inProgress = activeBy.get(g.id) ?? 0
      }
    } else if (g.kind === 'item') {
      g.name = iNames.get(g.id) || g.name || `Item ${g.id}`
      g.holders = heldBy.get(g.id) ?? 0
    } else if (g.kind === 'achievement') {
      g.name = g.name || `Achievement ${g.id}`
      g.holders = earnedBy.get(g.id) ?? 0
    }
  }

  const visitsBy = new Map<number, any>(frontier.map(r => [Number(r.map), r]))
  const liveBy = new Map<number, number>(liveKills.map(r => [Number(r.map), Number(r.killed ?? 0)]))
  // Which bosses actually fell, by name, from three sources: the core's raid log
  // (Wrath only), the live instance bits (any map, until it resets) and the
  // assembler's own kill ledger (any map, durable). A count alone cannot tell a
  // raid's final boss from an optional one standing nearer the door.
  const namedBy = new Map<number, string[]>()
  for (const r of namedKills) {
    const list = namedBy.get(Number(r.map)) ?? []
    list.push(String(r.boss))
    namedBy.set(Number(r.map), list)
  }
  const eras = ERAS.map(e => ({
    ...e,
    gates: Object.values([...gates.values()]
      .filter(g => g.era === e.key)
      .reduce((acc: Record<string, any>, g) => {
        const opens = [...g.opens].sort()
        const key = `${g.name}|${opens.join()}`
        const row = acc[key] ?? {
          kind: g.kind, name: g.name, heroic: g.heroic, legacy: g.legacy,
          opens, holders: 0, inProgress: 0,
        }
        row.holders += g.holders
        row.inProgress += g.inProgress
        acc[key] = row
        return acc
      }, {}))
      .sort((a, b) => Number(a.legacy) - Number(b.legacy) ||
                      b.holders - a.holders || a.name.localeCompare(b.name)),
    raids: (OLD_RAIDS[e.key] ?? []).map(map => {
      const r = visitsBy.get(map)
      return {
        map,
        name: r?.label ? raidName(r.label) : NAMES[map] ? raidName(NAMES[map]!) : `Map ${map}`,
        visits: Number(r?.visits ?? 0),
        killed: Math.max(Number(r?.killed ?? 0), liveBy.get(map) ?? 0,
                         (namedBy.get(map) ?? []).length),
        beaten: (namedBy.get(map) ?? []).sort(),
        bosses: Number(r?.bosses ?? 0),
      }
    }).sort((a, b) => b.killed - a.killed || a.name.localeCompare(b.name)),
  }))

  const raids = eras.flatMap(e => e.raids)
  return {
    at: Date.now(),
    eras,
    // The frontier in one line: how much of the old world has ever been beaten.
    old: {
      raids: raids.length,
      visited: raids.filter(r => r.visits > 0).length,
      bosses: raids.reduce((n, r) => n + r.bosses, 0),
      killed: raids.reduce((n, r) => n + r.killed, 0),
      visits: raids.reduce((n, r) => n + r.visits, 0),
    },
    collectors: {
      bots: Number(collectorCount[0]?.n ?? 0),
      runs: collectorRuns.length,
      live: collectorRuns.filter(r => Number(r.live)).length,
      bosses: collectorRuns.reduce((n, r) => n + Number(r.bosses ?? 0), 0),
      recent: collectorRuns.map(r => ({
        dungeon: r.dungeon, leader: r.leader, size: Number(r.size),
        bosses: Number(r.bosses ?? 0), outcome: r.outcome,
        live: !!Number(r.live), at: Number(r.started_at) * 1000,
      })),
    },
    hunters: (() => {
      const by = new Map<string, any>(huntPhases.map(r => [String(r.phase), r]))
      const n = (k: string) => Number(by.get(k)?.n ?? 0)
      return {
        bots: Number(huntWho[0]?.hunters ?? 0),
        hunting: Number(huntWho[0]?.hunting ?? 0),
        aims: n('aim'), reached: n('reach'), kills: n('kill'),
        bonus: n('bonus'), died: n('died'), gone: n('gone'),
        recent: huntRecent.map(r => ({
          at: Number(r.ts) * 1000, phase: String(r.phase),
          quarry: String(r.quarry), lvl: Number(r.lvl ?? 0), bot: r.bot,
        })),
      }
    })(),
    earning: earning.map(r => ({
      name: r.name, runs: Number(r.runs), underway: Number(r.underway ?? 0),
    })),
    legendaries: legendary.map(r => ({ name: r.name, owners: Number(r.owners) })),
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'progression',
})
