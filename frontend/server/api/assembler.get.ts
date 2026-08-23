import { getPool } from '../utils/db'
import { playerbotsConf, confNum } from '../utils/realm'

// Trip state lives in the worldserver's memory. PartyAssembler mirrors it into these
// two tables once per tick, which is the only way the dashboard can see a party that
// is halfway to a dungeon.
const STATS = `SELECT * FROM acore_characters.aetherion_assembler WHERE id = 1`

const TRIPS = `
  SELECT group_id, leader, leader_level, size, is_raid, min_level, max_level,
         dungeon, dungeon_map, phase, via, remaining_yards, ticks,
         leader_class, via_place, via_actor
  FROM acore_characters.aetherion_party_trips
  ORDER BY FIELD(phase,'summoning','travelling','inside'), remaining_yards ASC
`

// Total groups on the realm, which is legitimately larger than the assembler's own
// count: upstream forms its own proximity groups too.
const GROUP_COUNT = `
  SELECT COUNT(*) AS n, SUM(groupType & 2 = 2) AS raids FROM acore_characters.groups
`

const MEMBERS = `
  SELECT group_id, guid, name, class, level, is_leader, role
  FROM acore_characters.aetherion_party_members
  ORDER BY is_leader DESC, FIELD(role,'tank','healer','dps'), level DESC
`

// Boss progress for a group's own instance. The path is forced: there is no
// group_instance table in this build, so the character binding is the only bridge from
// a group to the instance row. BIT_COUNT of the encounter mask is the kill count.
const PROGRESS = `
  SELECT t.group_id, MAX(BIT_COUNT(i.completedEncounters)) AS bosses
  FROM acore_characters.aetherion_party_trips t
  JOIN acore_characters.group_member gm ON gm.guid = t.group_id
  JOIN acore_characters.character_instance ci ON ci.guid = gm.memberGuid
  JOIN acore_characters.instance i ON i.id = ci.instance AND i.map = t.dungeon_map
  GROUP BY t.group_id
`

// Which bosses, not just how many. The bit index lives only in dungeonencounter_dbc,
// which scripts/seed-encounters.sh fills from the client's own DBC - so this returns
// nothing until that has run, and the board falls back to the count above.
// creditType 1 encounters are credited by spell and have no creature, hence COALESCE.
const ENCOUNTERS = `
  SELECT t.group_id, de.Bit AS bit,
         COALESCE(ct.name, ie.comment)               AS boss,
         MAX(i.completedEncounters & (1 << de.Bit)) > 0 AS killed
  FROM acore_characters.aetherion_party_trips t
  JOIN acore_characters.group_member gm ON gm.guid = t.group_id
  JOIN acore_characters.character_instance ci ON ci.guid = gm.memberGuid
  JOIN acore_characters.instance i ON i.id = ci.instance AND i.map = t.dungeon_map
  JOIN acore_world.dungeonencounter_dbc de
    ON de.MapID = i.map AND de.Difficulty = i.difficulty
  JOIN acore_world.instance_encounters ie ON ie.entry = de.ID
  LEFT JOIN acore_world.creature_template ct
    ON ct.entry = ie.creditEntry AND ie.creditType = 0
  WHERE t.phase = 'inside'
  GROUP BY t.group_id, de.Bit, boss
  ORDER BY t.group_id, de.Bit
`

// What a party has actually done since it zoned in. There is no combat log on this
// realm, so deaths and loot are the only evidence that anything happened in there.
const ACTIVITY = `
  SELECT m.group_id, e.kind, COUNT(*) AS n
  FROM acore_characters.aetherion_party_members m
  JOIN acore_characters.aetherion_party_trips t ON t.group_id = m.group_id
  JOIN aetherion_ai.bot_events e
    ON e.guid = m.guid AND e.kind IN ('death','loot')
   AND e.ts > UNIX_TIMESTAMP() - (t.ticks * ?)
  WHERE t.phase = 'inside'
  GROUP BY m.group_id, e.kind
`

// Clear rate per dungeon. Only maps with a few runs behind them, so one lucky party
// does not read as a 100% clear rate.
const CLEAR_RATE = `
  SELECT COALESCE(d.name, CONCAT('Map ', i.map)) AS dungeon,
         d.gate AS gate,
         COUNT(*) AS instances,
         SUM(i.completedEncounters > 0) AS withKill,
         MAX(BIT_COUNT(i.completedEncounters)) AS bestRun,
         b.bosses AS bosses
  FROM acore_characters.instance i
  LEFT JOIN (SELECT map_id, MIN(min_level) AS gate, MIN(comment) AS name
             FROM acore_world.dungeon_access_template GROUP BY map_id) d ON d.map_id = i.map
  LEFT JOIN (SELECT c.map, COUNT(DISTINCT ie.entry) AS bosses
             FROM acore_world.instance_encounters ie
             JOIN acore_world.creature c ON c.id = ie.creditEntry
             WHERE ie.creditType = 0 GROUP BY c.map) b ON b.map = i.map
  GROUP BY i.map, d.name, d.gate, b.bosses
  HAVING instances >= 3
  ORDER BY instances DESC
  LIMIT 14
`

// Which dungeons the assembler actually sends parties to, from the recorder rather than
// the live snapshot, so it covers the whole session.
const DEMAND = `
  SELECT SUBSTRING(e.detail, 9) AS dungeon,
         COUNT(*) AS entries,
         COUNT(DISTINCT e.guid) AS bots,
         SUM(e.ts > UNIX_TIMESTAMP() - 3600) AS lastHour,
         ROUND(AVG(c.level), 1) AS avgLevel
  FROM aetherion_ai.bot_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  WHERE e.kind = 'instance' AND e.detail LIKE 'entered %'
  GROUP BY dungeon
  ORDER BY entries DESC
  LIMIT 12
`

// How much of the tick budget each travel mode burns. This is the panel for deciding
// whether FootRange, StallTicks or InsideTicks need moving.
const TRIP_CLOCK = `
  SELECT t.via,
         IF(t.phase = 'inside', 'inside', 'en route') AS clock,
         COUNT(*) AS parties,
         ROUND(AVG(t.ticks), 1) AS avgTicks,
         MAX(t.ticks) AS maxTicks,
         SUM(t.ticks >= 36) AS nearExpiry,
         ROUND(AVG(t.remaining_yards)) AS avgYards
  FROM acore_characters.aetherion_party_trips t
  GROUP BY t.via, IF(t.phase = 'inside', 'inside', 'en route')
  ORDER BY parties DESC
`

// Role shapes actually being assembled, against the tank/healer/dps intent.
const SHAPES = `
  SELECT m.group_id AS id,
         SUM(m.role = 'tank') AS tanks,
         SUM(m.role = 'healer') AS healers,
         SUM(m.role = 'dps') AS dps
  FROM acore_characters.aetherion_party_members m
  GROUP BY m.group_id
`

// Raid maps, listed explicitly. There is no raid flag anywhere in SQL on this build -
// map_dbc and lfgdungeons_dbc both ship empty - and matching on name is worse: a regex
// with 'Aman' in it for Zul'Aman silently swallows Uldaman, which is a five-man.
const RAID_MAPS = [249,309,409,469,509,531,532,533,534,544,548,550,564,565,568,580,
                   603,615,616,624,631,649,724]

const RAID_STATE = `
  SELECT COALESCE(dat.comment, CONCAT('Map ', i.map)) AS raid,
         COUNT(DISTINCT i.id) AS instances,
         SUM(i.completedEncounters > 0) AS withKill,
         SUM(BIT_COUNT(i.completedEncounters)) AS bosses
  FROM acore_characters.instance i
  LEFT JOIN acore_world.dungeon_access_template dat
         ON dat.map_id = i.map AND dat.difficulty = 0
  WHERE i.map IN (${RAID_MAPS.join(',')})
  GROUP BY raid, i.map
  ORDER BY instances DESC
`

// Entries recorded over the day, matched by map rather than by name.
const RAID_ENTRIES = `
  SELECT TRIM(SUBSTRING_INDEX(SUBSTRING_INDEX(dat.comment, ',', -1), ' - ', 1)) AS raid,
         COUNT(*) AS entries, COUNT(DISTINCT e.guid) AS bots
  FROM aetherion_ai.bot_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  JOIN acore_world.dungeon_access_template dat
    ON dat.map_id IN (${RAID_MAPS.join(',')})
   AND dat.difficulty = 0
   -- The recorder stores the cleaned wing name ("Icecrown Citadel"), not the raw
   -- comment ("Icecrown Citadel - 10man Normal"), so the same trimming is applied here.
   -- aetherion_ai is utf8mb4_0900_ai_ci and acore_world is utf8mb4_unicode_ci, so this
   -- comparison raises Error 1267 without an explicit collation.
   AND e.detail COLLATE utf8mb4_unicode_ci = CONCAT('entered ',
         TRIM(SUBSTRING_INDEX(SUBSTRING_INDEX(dat.comment, ',', -1), ' - ', 1)))
  WHERE e.kind = 'instance' AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY raid
  ORDER BY entries DESC
  LIMIT 12
`

const PHASE_STATUS: Record<string, { label: string; tone: string }> = {
  travelling: { label: 'TRAVELLING', tone: 'travel' },
  summoning: { label: 'AT THE DOOR', tone: 'door' },
  inside: { label: 'INSIDE', tone: 'inside' },
}

export default defineEventHandler(async () => {
  const pool = getPool()
  const conf = await playerbotsConf()
  const tickSeconds = Math.max(1, Math.round(confNum(conf, 'AiPlayerbot.Party.IntervalMs', 45000) / 1000))

  const [statRows, tripRows, groupRows, memberRows, progressRows, activityRows,
         encounterRows, clearRows, demandRows, clockRows, shapeRows,
         raidStateRows, raidEntryRows] = await Promise.all([
    q(STATS),
    q(TRIPS),
    q(GROUP_COUNT),
    q(MEMBERS),
    q(PROGRESS),
    q(ACTIVITY, tickSeconds),
    q(ENCOUNTERS),
    q(CLEAR_RATE),
    q(DEMAND),
    q(TRIP_CLOCK),
    q(SHAPES),
    q(RAID_STATE),
    q(RAID_ENTRIES),
  ])
  const stats = statRows[0]
  const trips = tripRows
  const groups = groupRows[0]

  // One pass into buckets: the board renders a roster per row and re-scanning the flat
  // list for every group would be quadratic in the number of parties.
  const byGroup = new Map<number, any[]>()
  for (const m of (memberRows ?? []) as any[]) {
    const list = byGroup.get(m.group_id) ?? []
    list.push({
      guid: m.guid, name: m.name, cls: m.class, level: m.level,
      leader: !!m.is_leader, role: m.role,
    })
    byGroup.set(m.group_id, list)
  }

  const encounters = new Map<number, { name: string; killed: boolean }[]>()
  for (const r of (encounterRows ?? []) as any[]) {
    const list = encounters.get(r.group_id) ?? []
    list.push({ name: r.boss, killed: !!Number(r.killed) })
    encounters.set(r.group_id, list)
  }

  const bosses = new Map<number, number>()
  for (const r of (progressRows ?? []) as any[]) bosses.set(r.group_id, Number(r.bosses) || 0)

  const acted = new Map<number, { death: number; loot: number }>()
  for (const r of (activityRows ?? []) as any[]) {
    const e = acted.get(r.group_id) ?? { death: 0, loot: 0 }
    if (r.kind === 'death') e.death = Number(r.n)
    else e.loot = Number(r.n)
    acted.set(r.group_id, e)
  }

  const footRange = confNum(conf, 'AiPlayerbot.Party.FootRange', 1200)
  const maxParties = confNum(conf, 'AiPlayerbot.Party.MaxParties', 80)

  const board = (trips ?? []).map((t: any) => {
    // How far along the journey is, for the progress rule under each row. Anything
    // that started beyond foot range is measured against its own starting distance.
    const span = Math.max(t.remaining_yards, footRange)
    const pct = t.phase === 'inside' ? 100
      : Math.max(0, Math.min(100, Math.round((1 - t.remaining_yards / span) * 100)))

    return {
      id: t.group_id,
      label: `${t.is_raid ? 'Raid' : 'Party'} #${t.group_id}`,
      isRaid: !!t.is_raid,
      leader: t.leader,
      size: `${t.size}/${t.is_raid ? 10 : 5}`,
      levels: t.min_level === t.max_level ? `${t.min_level}` : `${t.min_level}-${t.max_level}`,
      dest: t.dungeon,
      dungeonMap: t.dungeon_map,
      via: String(t.via).toUpperCase(),
      // Where the shortcut landed and, for a portal, who cast it. "HEARTHED" alone
      // does not say the party is now standing in Dalaran.
      viaPlace: t.via_place || null,
      viaActor: t.via_actor || null,
      leaderClass: t.leader_class ?? 0,
      members: byGroup.get(t.group_id) ?? [],
      // ticks resets to zero on zone-in, so for an inside group it is dwell time.
      dwellMins: t.phase === 'inside' ? Math.round((t.ticks * tickSeconds) / 60) : null,
      bosses: bosses.get(t.group_id) ?? 0,
      encounters: encounters.get(t.group_id) ?? [],
      deaths: acted.get(t.group_id)?.death ?? 0,
      looted: acted.get(t.group_id)?.loot ?? 0,
      remaining: t.phase === 'inside' ? '—' : `${t.remaining_yards.toLocaleString()} yd`,
      status: PHASE_STATUS[t.phase]?.label ?? 'FORMING',
      tone: PHASE_STATUS[t.phase]?.tone ?? 'forming',
      pct,
    }
  })

  const inside = board.filter((b: any) => b.tone === 'inside').length
  const atDoor = board.filter((b: any) => b.tone === 'door').length
  const travelling = board.filter((b: any) => b.tone === 'travel').length
  const active = Number(stats?.active ?? 0)

  return {
    at: Date.now(),
    // Present only once the worldserver has written a tick. Without this the panels
    // would render zeroes that look like a broken assembler rather than a cold start.
    ready: !!stats,
    funnel: [
      { key: 'forming', label: 'forming', value: Math.max(0, active - board.length) },
      { key: 'travelling', label: 'travelling', value: travelling },
      { key: 'door', label: 'at the door', value: atDoor },
      { key: 'inside', label: 'inside', value: inside },
    ],
    cycle: {
      formed: Number(stats?.formed ?? 0),
      raids: Number(stats?.raids ?? 0),
      trips: Number(stats?.trips ?? 0),
      stalls: Number(stats?.stalls ?? 0),
      arrived: Number(stats?.arrived ?? 0),
      entered: Number(stats?.entered ?? 0),
    },
    doors: {
      entrances: Number(stats?.entrances ?? 0),
      arrivalPoints: Number(stats?.arrival_points ?? 0),
      raidMaps: Number(stats?.raid_maps ?? 0),
    },
    // Read from the live playerbots.conf, so this panel is what the server loaded.
    config: [
      ['IntervalMs', confNum(conf, 'AiPlayerbot.Party.IntervalMs', 45000)],
      ['PerTick', confNum(conf, 'AiPlayerbot.Party.PerTick', 3)],
      ['MaxParties', maxParties],
      ['RaidPct', confNum(conf, 'AiPlayerbot.Party.RaidPct', 20)],
      ['RaidSize', confNum(conf, 'AiPlayerbot.Party.RaidSize', 10)],
      ['FootRange', `${footRange} yd`],
      ['PortalPct', confNum(conf, 'AiPlayerbot.Party.PortalPct', 50)],
      ['NearestChoices', confNum(conf, 'AiPlayerbot.Party.NearestChoices', 4)],
      ['StallTicks', confNum(conf, 'AiPlayerbot.Party.StallTicks', 2)],
      ['ArriveRange', `${confNum(conf, 'AiPlayerbot.Party.ArriveRange', 60)} yd`],
      ['InsideTicks', confNum(conf, 'AiPlayerbot.Party.InsideTicks', 20)],
    ].map(([k, v]) => ({ k, v: String(v) })),
    clearRate: (clearRows as any[]).map(r => ({
      dungeon: String(r.dungeon).replace(/\s*-\s*\d+\s*man.*$/i, '').split(',').pop()!.trim(),
      gate: Number(r.gate ?? 0),
      instances: Number(r.instances),
      withKill: Number(r.withKill),
      pct: Math.round((Number(r.withKill) / Number(r.instances)) * 100),
      bestRun: Number(r.bestRun ?? 0),
      bosses: Number(r.bosses ?? 0),
    })),
    demand: (demandRows as any[]).map(r => ({
      dungeon: r.dungeon, entries: Number(r.entries), bots: Number(r.bots),
      lastHour: Number(r.lastHour), avgLevel: Number(r.avgLevel),
    })),
    clock: (clockRows as any[]).map(r => ({
      via: r.via, clock: r.clock, parties: Number(r.parties),
      avgTicks: Number(r.avgTicks), maxTicks: Number(r.maxTicks),
      nearExpiry: Number(r.nearExpiry), avgYards: Number(r.avgYards ?? 0),
    })),
    shapes: (() => {
      // Collapsed to the handful of shapes that actually occur, biggest first.
      const counts = new Map<string, number>()
      for (const r of shapeRows as any[]) {
        const key = `${r.tanks}/${r.healers}/${r.dps}`
        counts.set(key, (counts.get(key) ?? 0) + 1)
      }
      return [...counts.entries()]
        .map(([shape, n]) => ({ shape, n }))
        .sort((a, b) => b.n - a.n)
        .slice(0, 8)
    })(),
    raids: {
      formed: Number(stats?.raids ?? 0),
      inFlight: board.filter((b: any) => b.isRaid).length,
      // Configured share versus what the reachability gate actually lets through.
      configuredPct: confNum(conf, 'AiPlayerbot.Party.RaidPct', 20),
      actualPct: stats?.formed ? Math.round((Number(stats.raids) / Number(stats.formed)) * 1000) / 10 : 0,
      instances: (raidStateRows as any[]).map(r => ({
        raid: String(r.raid).replace(/\s*-\s*\d+\s*man.*$/i, '').split(',').pop()!.trim(),
        instances: Number(r.instances), withKill: Number(r.withKill), bosses: Number(r.bosses),
      })),
      entries: (raidEntryRows as any[]).map(r => ({
        raid: String(r.raid).replace(/\s*-\s*\d+\s*man.*$/i, '').split(',').pop()!.trim(),
        entries: Number(r.entries), bots: Number(r.bots),
      })),
    },
    activeParties: active,
    maxParties,
    totalGroups: Number(groups?.n ?? 0),
    raidGroups: Number(groups?.raids ?? 0),
    board,
  }
})
