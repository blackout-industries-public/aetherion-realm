import { getPool } from '../utils/db'

// Quest status codes as AzerothCore stores them. 2 and 4 never appear in this table -
// they are transient states the client is told about, not persisted.
const STATUS = { COMPLETE: 1, INCOMPLETE: 3, FAILED: 5 } as const

const HEADLINE = `
  SELECT COUNT(DISTINCT qs.guid) AS questing,
         COUNT(*)                AS active,
         SUM(qs.status = ${STATUS.COMPLETE})   AS readyToHandIn,
         SUM(qs.status = ${STATUS.INCOMPLETE}) AS inProgress,
         SUM(qs.status = ${STATUS.FAILED})     AS failed
  FROM acore_characters.character_queststatus qs
  JOIN acore_characters.characters c ON c.guid = qs.guid AND c.online = 1
`

const COMPLETED = `
  SELECT COUNT(DISTINCT r.guid) AS bots, COUNT(*) AS quests
  FROM acore_characters.character_queststatus_rewarded r
  JOIN acore_characters.characters c ON c.guid = r.guid AND c.online = 1
`

// What the realm is working on right now. QuestSortID is a zone id when positive.
const POPULAR = `
  SELECT qt.LogTitle AS quest, qt.QuestLevel AS lvl, qt.QuestSortID AS sort,
         COUNT(*) AS carrying,
         SUM(qs.status = ${STATUS.COMPLETE}) AS ready
  FROM acore_characters.character_queststatus qs
  JOIN acore_characters.characters c ON c.guid = qs.guid AND c.online = 1
  JOIN acore_world.quest_template qt ON qt.ID = qs.quest
  GROUP BY qt.LogTitle, qt.QuestLevel, qt.QuestSortID
  ORDER BY carrying DESC
  LIMIT 12
`

// Completion by level band. The interesting result is the shape, not the total: the
// low bands barely quest at all.
const BY_BAND = `
  SELECT CASE WHEN c.level >= 70 THEN '70-80' WHEN c.level >= 50 THEN '50-69'
              WHEN c.level >= 30 THEN '30-49' ELSE '1-29' END AS band,
         COUNT(DISTINCT c.guid) AS bots,
         COUNT(r.quest)         AS completed
  FROM acore_characters.characters c
  LEFT JOIN acore_characters.character_queststatus_rewarded r ON r.guid = c.guid
  WHERE c.online = 1
  GROUP BY band ORDER BY band
`

const TOP_QUESTERS = `
  SELECT c.name, c.class AS cls, c.level, COUNT(r.quest) AS completed
  FROM acore_characters.characters c
  JOIN acore_characters.character_queststatus_rewarded r ON r.guid = c.guid
  WHERE c.online = 1
  GROUP BY c.guid, c.name, c.class, c.level
  ORDER BY completed DESC
  LIMIT 12
`

// Quest completions per hour, from the recorder. Empty until the recorder has seen a
// completion, which needs one sampling interval to establish a baseline.
const TEMPO = `
  SELECT DATE_FORMAT(FROM_UNIXTIME(ts), '%H:00') AS hour, COUNT(*) AS completed
  FROM aetherion_ai.bot_events
  WHERE kind = 'quest' AND ts > UNIX_TIMESTAMP() - 43200
  GROUP BY hour, FLOOR(ts / 3600)
  ORDER BY FLOOR(ts / 3600)
`


export default defineCachedEventHandler(async () => {
  const [head, done, popular, bands, questers, tempo] = await Promise.all([
    q(HEADLINE), q(COMPLETED), q(POPULAR), q(BY_BAND), q(TOP_QUESTERS), q(TEMPO),
  ])

  const h = head[0] ?? {}
  const d = done[0] ?? {}

  return {
    at: Date.now(),
    headline: {
      questing: Number(h.questing ?? 0),
      active: Number(h.active ?? 0),
      readyToHandIn: Number(h.readyToHandIn ?? 0),
      inProgress: Number(h.inProgress ?? 0),
      failed: Number(h.failed ?? 0),
      perBot: Number(h.questing) ? Math.round((Number(h.active) / Number(h.questing)) * 10) / 10 : 0,
      completedBots: Number(d.bots ?? 0),
      completedQuests: Number(d.quests ?? 0),
    },
    popular: popular.map(r => ({
      quest: r.quest, level: Number(r.lvl),
      // A positive QuestSortID is a zone; negative values are category sorts.
      zone: Number(r.sort) > 0 ? Number(r.sort) : 0,
      carrying: Number(r.carrying), ready: Number(r.ready),
    })),
    bands: bands.map(r => ({
      band: r.band, bots: Number(r.bots), completed: Number(r.completed),
      avgEach: Number(r.bots) ? Math.round((Number(r.completed) / Number(r.bots)) * 10) / 10 : 0,
    })),
    questers: questers.map(r => ({
      name: r.name, cls: Number(r.cls), level: Number(r.level),
      completed: Number(r.completed),
    })),
    tempo: tempo.map(r => ({ hour: r.hour, completed: Number(r.completed) })),
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'quests',
})
