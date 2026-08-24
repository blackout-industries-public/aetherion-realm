import { q } from '../utils/db'

// Battleground and arena maps. There is no raid/bg flag anywhere in SQL on this build -
// map_dbc ships empty - so the list is explicit rather than inferred from a name.
const BATTLEGROUNDS: Record<number, { name: string; short: string; cap: number }> = {
  30:  { name: 'Alterac Valley', short: 'AV', cap: 80 },
  489: { name: 'Warsong Gulch', short: 'WSG', cap: 20 },
  529: { name: 'Arathi Basin', short: 'AB', cap: 30 },
  566: { name: 'Eye of the Storm', short: 'EotS', cap: 30 },
  607: { name: 'Strand of the Ancients', short: 'SotA', cap: 30 },
  628: { name: 'Isle of Conquest', short: 'IoC', cap: 80 },
}

const ARENAS: Record<number, string> = {
  559: 'Nagrand Arena',
  562: "Blade's Edge Arena",
  572: 'Ruins of Lordaeron',
  617: 'Dalaran Sewers',
  618: 'Ring of Valor',
}

const ALL_PVP_MAPS = [...Object.keys(BATTLEGROUNDS), ...Object.keys(ARENAS)].join(',')

// Who is standing in a battleground or arena right now. This is live occupancy, not a
// match record: match records need Battleground.StoreStatistics.Enable.
const OCCUPANCY = `
  SELECT c.map, COUNT(*) AS chars,
         SUM(c.race IN (1,3,4,7,11)) AS alliance,
         ROUND(AVG(c.level)) AS avgLevel
  FROM acore_characters.characters c
  WHERE c.online = 1 AND c.map IN (${ALL_PVP_MAPS})
  GROUP BY c.map
`

// characters.todayKills is HONOURABLE kills and nothing else: Player.cpp:6335 only
// increments it inside `if (uVictim->IsPlayer())`, and only for an opposing-faction
// victim above grey level, outside an arena. NPC kills never touch this counter.
//
// It does not distinguish battleground kills from open-world ones, so the split below is
// by where the killer is standing NOW. That is exact for anyone currently in a
// battleground and approximate for the rest, since a character who has left a
// battleground carries its kills out with them.
const HEADLINE = `
  SELECT SUM(todayKills) AS killsToday,
         SUM(totalKills) AS killsLifetime,
         SUM(todayKills > 0) AS killersToday,
         MAX(totalHonorPoints) AS topHonor,
         SUM(totalHonorPoints > 0) AS withHonor,
         SUM(arenaPoints > 0) AS withArena,
         SUM(CASE WHEN map IN (${ALL_PVP_MAPS}) THEN todayKills ELSE 0 END) AS killsInBg,
         SUM(CASE WHEN map NOT IN (${ALL_PVP_MAPS}) THEN todayKills ELSE 0 END) AS killsInWorld
  FROM acore_characters.characters WHERE online = 1
`

const TOP_KILLERS = `
  SELECT c.name, c.class AS cls, c.level, c.zone, c.map,
         c.todayKills AS today, c.totalKills AS total, c.totalHonorPoints AS honor,
         c.race IN (1,3,4,7,11) AS alliance
  FROM acore_characters.characters c
  WHERE c.online = 1 AND c.todayKills > 0
  ORDER BY c.todayKills DESC, c.totalKills DESC
  LIMIT 12
`

// Hourly PvP tempo. 'pvp' events are honourable kills as the recorder sees them.
const TEMPO = `
  SELECT DATE_FORMAT(FROM_UNIXTIME(ts), '%H:00') AS hour,
         SUM(kind = 'pvp')    AS kills,
         SUM(kind = 'death')  AS deaths
  FROM aetherion_ai.bot_events
  WHERE kind IN ('pvp','death') AND ts > UNIX_TIMESTAMP() - 43200
  GROUP BY hour, FLOOR(ts / 3600)
  ORDER BY FLOOR(ts / 3600)
`

// Populated only once Battleground.StoreStatistics.Enable is on; empty is a valid state
// and the UI says so rather than rendering a blank panel.
const MATCHES = `
  SELECT id, winner_faction AS winner, bracket_id AS bracket,
         type, date
  FROM acore_characters.pvpstats_battlegrounds
  ORDER BY id DESC LIMIT 12
`

const ARENA_TEAMS = `
  SELECT at.arenaTeamId AS id, at.name, at.type, at.rating,
         at.seasonGames, at.seasonWins, at.rank
  FROM acore_characters.arena_team at
  WHERE at.seasonGames > 0 OR at.rating > 0
  ORDER BY at.rating DESC LIMIT 10
`


export default defineEventHandler(async () => {
  const [occ, headline, killers, tempo, matches, teams] = await Promise.all([
    q(OCCUPANCY), q(HEADLINE), q(TOP_KILLERS), q(TEMPO), q(MATCHES), q(ARENA_TEAMS),
  ])

  const h = headline[0] ?? {}

  const battlegrounds = occ
    .filter(r => BATTLEGROUNDS[r.map])
    .map(r => {
      const bg = BATTLEGROUNDS[r.map]!
      const chars = Number(r.chars)
      return {
        map: r.map, name: bg.name, short: bg.short,
        chars, cap: bg.cap,
        alliance: Number(r.alliance), horde: chars - Number(r.alliance),
        avgLevel: Number(r.avgLevel),
        // Against the map's own capacity, so a full AV and a full WSG read differently.
        fill: Math.min(100, Math.round((chars / bg.cap) * 100)),
      }
    })
    .sort((a, b) => b.chars - a.chars)

  const arenas = occ
    .filter(r => ARENAS[r.map])
    .map(r => ({ map: r.map, name: ARENAS[r.map]!, chars: Number(r.chars) }))

  return {
    at: Date.now(),
    headline: {
      killsToday: Number(h.killsToday ?? 0),
      killsLifetime: Number(h.killsLifetime ?? 0),
      killersToday: Number(h.killersToday ?? 0),
      topHonor: Number(h.topHonor ?? 0),
      withHonor: Number(h.withHonor ?? 0),
      withArena: Number(h.withArena ?? 0),
      killsInBg: Number(h.killsInBg ?? 0),
      killsInWorld: Number(h.killsInWorld ?? 0),
      inBattlegrounds: battlegrounds.reduce((n, b) => n + b.chars, 0),
      inArenas: arenas.reduce((n, a) => n + a.chars, 0),
    },
    battlegrounds,
    arenas,
    killers: killers.map(k => ({
      name: k.name, cls: Number(k.cls), level: Number(k.level),
      zone: Number(k.zone), map: Number(k.map),
      today: Number(k.today), total: Number(k.total), honor: Number(k.honor),
      faction: Number(k.alliance) ? 'alliance' : 'horde',
      // A kill scored while standing in a battleground is a battleground kill.
      inBg: !!BATTLEGROUNDS[k.map],
    })),
    tempo: tempo.map(t => ({ hour: t.hour, kills: Number(t.kills), deaths: Number(t.deaths) })),
    matches: matches.map(m => ({
      id: m.id, winner: Number(m.winner), bracket: Number(m.bracket),
      type: Number(m.type), at: m.date ? new Date(m.date).getTime() : null,
    })),
    // Distinguishes "no matches recorded" from "statistics are switched off".
    matchesRecorded: matches.length > 0,
    arenaTeams: teams.map(t => ({
      id: t.id, name: t.name, size: Number(t.type), rating: Number(t.rating),
      games: Number(t.seasonGames), wins: Number(t.seasonWins),
    })),
  }
})
