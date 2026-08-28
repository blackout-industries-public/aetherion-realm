import { q } from '../utils/db'

// The debug plane: one row per venue, and for each the verdict on what its runs
// are actually doing. Built because diagnosing this by hand meant writing the
// same CASE expression into a mysql client every time, and the interesting
// column - WHY a run produced nothing - exists nowhere else.

// Why a run ended, in the order the evidence is decisive. A run that never got
// in cannot have failed inside; a party that killed nothing while dying is a
// different problem from one that killed nothing while untouched, and calling
// both "failed" is what hid the split for a week.
const CASE_SQL = `
  CASE
    WHEN h.entered_at = 0 AND h.reached_door_at = 0 THEN 'lost on the road'
    WHEN h.entered_at = 0                           THEN 'died at the door'
    WHEN h.outcome = 'wiped'                        THEN 'wiped out'
    WHEN h.encounters > 0 AND h.bosses_downed >= h.encounters THEN 'cleared'
    WHEN h.bosses_downed > 0                        THEN 'partial'
    WHEN h.deaths >= 3                              THEN 'ground down'
    ELSE 'idle inside'
  END`

export default defineCachedEventHandler(async event => {
  const hours = Math.min(Math.max(Number(getQuery(event).hours) || 24, 1), 168)
  const since = `UNIX_TIMESTAMP() - ${hours * 3600}`
  // Interrupted rows never observed their own ending, so they would poison every
  // average here; they are counted separately rather than silently dropped.
  const live = `h.ended_at > 0 AND h.outcome <> 'interrupted'`

  const [venues, cases, kills, interrupted] = await Promise.all([
    q(`SELECT h.map, h.dungeon, h.is_raid AS isRaid,
              COUNT(*) AS runs,
              SUM(h.entered_at > 0) AS entered,
              MAX(h.encounters) AS encounters,
              SUM(h.bosses_downed) AS bossesTotal,
              MAX(h.bosses_downed) AS deepest,
              SUM(h.encounters > 0 AND h.bosses_downed >= h.encounters) AS cleared,
              SUM(h.bosses_downed > 0) AS scored,
              ROUND(AVG(h.deaths), 1) AS deaths,
              ROUND(AVG(h.wipes), 2) AS wipes,
              ROUND(AVG((h.ended_at - h.started_at) / 60)) AS mins,
              ROUND(AVG(NULLIF(h.avg_ilvl, 0))) AS ilvl,
              MAX(h.started_at) AS lastAt
       FROM acore_characters.aetherion_run_history h
       WHERE ${live} AND h.started_at > ${since}
       GROUP BY h.map, h.dungeon, h.is_raid`),
    q(`SELECT h.map, ${CASE_SQL} AS verdict, COUNT(*) AS n
       FROM acore_characters.aetherion_run_history h
       WHERE ${live} AND h.started_at > ${since}
       GROUP BY h.map, verdict`),
    q(`SELECT k.map, k.name AS boss, COUNT(*) AS n
       FROM acore_characters.aetherion_run_kills k
       WHERE k.at > ${since} GROUP BY k.map, k.name`),
    q(`SELECT map, COUNT(*) AS n FROM acore_characters.aetherion_run_history
       WHERE outcome = 'interrupted' AND started_at > ${since} GROUP BY map`),
  ])

  const casesBy = new Map<number, Record<string, number>>()
  for (const r of cases) {
    const m = casesBy.get(Number(r.map)) ?? {}
    m[String(r.verdict)] = Number(r.n)
    casesBy.set(Number(r.map), m)
  }
  const killsBy = new Map<number, { boss: string; n: number }[]>()
  for (const r of kills) {
    const list = killsBy.get(Number(r.map)) ?? []
    list.push({ boss: String(r.boss), n: Number(r.n) })
    killsBy.set(Number(r.map), list)
  }
  const interruptedBy = new Map<number, number>(
    interrupted.map(r => [Number(r.map), Number(r.n)]))

  const rows = venues.map(v => {
    const map = Number(v.map)
    const runs = Number(v.runs)
    const verdicts = casesBy.get(map) ?? {}
    // The headline is whichever verdict happened most - the thing to fix first.
    const [worst, worstN] = Object.entries(verdicts)
      .filter(([k]) => k !== 'cleared')
      .sort((a, b) => b[1] - a[1])[0] ?? ['-', 0]
    const encounters = Number(v.encounters ?? 0)
    return {
      map, dungeon: v.dungeon, isRaid: !!Number(v.isRaid),
      runs, entered: Number(v.entered),
      encounters,
      cleared: Number(v.cleared), scored: Number(v.scored),
      clearPct: runs ? Math.round((Number(v.cleared) / runs) * 100) : 0,
      scorePct: runs ? Math.round((Number(v.scored) / runs) * 100) : 0,
      // How much of the venue the best run got through.
      depthPct: encounters ? Math.round((Number(v.deepest) / encounters) * 100) : 0,
      deepest: Number(v.deepest), bosses: Number(v.bossesTotal),
      deaths: Number(v.deaths ?? 0), wipes: Number(v.wipes ?? 0),
      mins: Number(v.mins ?? 0), ilvl: Number(v.ilvl ?? 0),
      lastAt: Number(v.lastAt) * 1000,
      verdicts, worst, worstN: Number(worstN),
      interrupted: interruptedBy.get(map) ?? 0,
      killed: (killsBy.get(map) ?? []).sort((a, b) => b.n - a.n),
    }
  }).sort((a, b) => b.runs - a.runs)

  const totals = rows.reduce((t, r) => {
    t.runs += r.runs; t.cleared += r.cleared; t.scored += r.scored; t.bosses += r.bosses
    for (const [k, n] of Object.entries(r.verdicts)) t.verdicts[k] = (t.verdicts[k] ?? 0) + n
    return t
  }, { runs: 0, cleared: 0, scored: 0, bosses: 0, verdicts: {} as Record<string, number> })

  return { at: Date.now(), hours, totals, venues: rows }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'debug',
})
