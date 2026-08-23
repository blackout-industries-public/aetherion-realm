import { q } from '../utils/db'

// The race to endgame. Firsts are recorded by the bridge as they happen - a first is
// an event in time, and it cannot be reconstructed later - with an INSERT IGNORE on
// UNIQUE(kind, detail) doing the arbitration.

// A race_start marker (written by the wipe script) restarts the clock; without one,
// firsts are "since the recorder started watching".
const MARKER = `
  SELECT ts, detail FROM aetherion_ai.milestones
  WHERE kind = 'race_start' ORDER BY id DESC LIMIT 1
`

const FIRSTS = `
  SELECT kind, detail, ts, guid, who FROM aetherion_ai.milestones
  WHERE kind IN ('first_level', 'first_boss', 'first_clear')
  ORDER BY ts
`

// Standings: highest level wins, earliest arrival breaks ties - the ding timestamp
// comes from the recorder's own event stream.
const STANDINGS = `
  SELECT c.guid, c.name, c.class AS cls, c.level, c.race,
         (SELECT MAX(e.ts) FROM aetherion_ai.bot_events e
           WHERE e.guid = c.guid AND e.kind = 'level') AS lastDing,
         (SELECT COUNT(*) FROM acore_characters.character_queststatus_rewarded r
           WHERE r.guid = c.guid) AS quests
  FROM acore_characters.characters c
  WHERE c.online = 1
  ORDER BY c.level DESC, lastDing ASC
  LIMIT 15
`

const LADDER_SHAPE = `
  SELECT c.level, COUNT(*) AS n
  FROM acore_characters.characters c WHERE c.online = 1
  GROUP BY c.level ORDER BY c.level
`

const ALLIANCE = new Set([1, 3, 4, 7, 11])

export default defineEventHandler(async () => {
  const [marker, firsts, standings, shape] = await Promise.all([
    q(MARKER), q(FIRSTS), q(STANDINGS), q(LADDER_SHAPE),
  ])

  const raceStart = marker[0] ? Number(marker[0].ts) * 1000 : null

  const levelFirsts = firsts
    .filter(f => f.kind === 'first_level')
    .map(f => ({
      threshold: Number(f.detail), who: f.who, guid: f.guid,
      at: Number(f.ts) * 1000,
    }))
    .sort((a, b) => a.threshold - b.threshold)

  const bossFirsts = firsts
    .filter(f => f.kind === 'first_boss')
    .map(f => ({ boss: f.detail, who: f.who, at: Number(f.ts) * 1000 }))

  const clears = firsts
    .filter(f => f.kind === 'first_clear')
    .map(f => ({ instance: f.detail, who: f.who, at: Number(f.ts) * 1000 }))

  return {
    at: Date.now(),
    raceStart,
    raceLabel: marker[0]?.detail ?? null,
    levelFirsts,
    bossFirsts: bossFirsts.slice(-20).reverse(),
    bossFirstsTotal: bossFirsts.length,
    clears: clears.slice(-10).reverse(),
    standings: standings.map((r, i) => ({
      rank: i + 1, guid: r.guid, name: r.name, cls: Number(r.cls),
      level: Number(r.level),
      faction: ALLIANCE.has(Number(r.race)) ? 'alliance' : 'horde',
      lastDing: r.lastDing ? Number(r.lastDing) * 1000 : null,
      quests: Number(r.quests ?? 0),
    })),
    shape: shape.map(r => ({ level: Number(r.level), n: Number(r.n) })),
  }
})
