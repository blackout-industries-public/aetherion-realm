import { q } from '../utils/db'

// The run-history ledger: every assembler journey, durable for two weeks.
// The assembler writes identity, phases and outcome; this endpoint grades
// results and enriches deaths and drops from the recorder's event stream.

const RUNS = `
  SELECT h.id, h.started_at, h.ended_at, h.dungeon, h.map, h.is_raid, h.difficulty,
         h.size, h.leader, h.leader_class, h.avg_ilvl, h.via,
         h.reached_door_at, h.entered_at, h.outcome, h.bosses_downed,
         enc.total AS bosses_total
  FROM acore_characters.aetherion_run_history h
  LEFT JOIN (SELECT MapID, Difficulty, COUNT(*) AS total
             FROM acore_world.dungeonencounter_dbc GROUP BY MapID, Difficulty) enc
    ON enc.MapID = h.map AND enc.Difficulty = h.difficulty
  ORDER BY h.started_at DESC LIMIT 40
`

// Deaths and notable drops inside the run's own window. Joining on entered_at
// keeps travel deaths out of the run's ledger line.
const ACTIVITY = `
  SELECT m.run_id, e.kind, COUNT(*) AS n
  FROM acore_characters.aetherion_run_members m
  JOIN acore_characters.aetherion_run_history h ON h.id = m.run_id
  JOIN aetherion_ai.bot_events e ON e.guid = m.guid
    AND h.entered_at > 0 AND e.ts >= h.entered_at
    AND e.ts <= IF(h.ended_at > 0, h.ended_at, UNIX_TIMESTAMP())
    AND e.kind IN ('death','loot')
  WHERE h.started_at > UNIX_TIMESTAMP() - 7*86400
  GROUP BY m.run_id, e.kind
`

export default defineEventHandler(async () => {
  const [runs, activity] = await Promise.all([q(RUNS), q(ACTIVITY)])

  const acted = new Map<number, { deaths: number; drops: number }>()
  for (const r of activity as any[]) {
    const e = acted.get(Number(r.run_id)) ?? { deaths: 0, drops: 0 }
    if (r.kind === 'death') e.deaths = Number(r.n)
    else e.drops = Number(r.n)
    acted.set(Number(r.run_id), e)
  }

  return {
    at: Date.now(),
    runs: (runs as any[]).map(r => {
      const downed = Number(r.bosses_downed ?? 0)
      const total = Number(r.bosses_total ?? 0)
      // 'ended' is the assembler's neutral verdict for a run that got inside;
      // the grade comes from what it actually killed.
      const outcome = r.outcome === 'ended'
        ? (total > 0 && downed >= total ? 'cleared' : downed > 0 ? 'partial' : 'fruitless')
        : r.outcome
      const isRaid = !!Number(r.is_raid)
      const diff = Number(r.difficulty)
      const ended = Number(r.ended_at) || null
      const entered = Number(r.entered_at) || null
      return {
        id: Number(r.id),
        started: Number(r.started_at) * 1000,
        dungeon: r.dungeon,
        isRaid,
        heroic: isRaid ? diff === 2 || diff === 3 : diff === 1,
        size: Number(r.size),
        leader: r.leader,
        leaderClass: Number(r.leader_class),
        avgIlvl: Number(r.avg_ilvl),
        via: r.via,
        outcome,
        downed, total,
        deaths: acted.get(Number(r.id))?.deaths ?? 0,
        drops: acted.get(Number(r.id))?.drops ?? 0,
        // Minutes inside, or the whole attempt when they never zoned in.
        mins: ended
          ? Math.max(1, Math.round((ended - (entered ?? Number(r.started_at))) / 60))
          : null,
        underway: !ended,
      }
    }),
  }
})
