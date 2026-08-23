import { getPool } from '../utils/db'

// There is no combat log table and no death counter in this schema. What exists is
// honourable-kill tallies plus per-character health and corpse expiry, which between
// them are enough to see who is fighting and who has just died.
const SQL = `
  SELECT c.guid, c.name, c.level, c.class AS cls, c.race, c.map, c.zone,
         c.online, c.health, c.death_expire_time,
         c.totalKills, c.todayKills, c.totalHonorPoints, c.todayHonorPoints
  FROM acore_characters.characters c
  WHERE c.totalKills > 0 OR c.health = 0
     OR c.death_expire_time > UNIX_TIMESTAMP()
`

const ALLIANCE = new Set([1, 3, 4, 7, 11])

export default defineEventHandler(async () => {
  const [rows] = await getPool().query<any[]>(SQL)

  const all = (rows as any[]).map(r => ({
    guid: r.guid, name: r.name, level: r.level, cls: r.cls,
    faction: ALLIANCE.has(r.race) ? 'alliance' : 'horde',
    zone: r.zone, online: !!r.online,
    // death_expire_time is NOT cleared on resurrection - it keeps the last corpse's
    // expiry forever, so a bare "> 0" marked 925 living bots as dead. Only a corpse
    // that has not yet expired means the character is actually down.
    dead: r.health === 0 || r.death_expire_time * 1000 > Date.now(),
    kills: r.totalKills, killsToday: r.todayKills,
    honor: r.totalHonorPoints, honorToday: r.todayHonorPoints,
  }))

  const killers = all.filter(e => e.kills > 0)
    .sort((a, b) => b.kills - a.kills).slice(0, 12)
  const dead = all.filter(e => e.dead && e.online).slice(0, 12)

  return {
    at: Date.now(),
    totalKills: all.reduce((n, e) => n + e.kills, 0),
    killsToday: all.reduce((n, e) => n + e.killsToday, 0),
    fighters: killers.length,
    deadNow: dead.length,
    killers,
    dead,
  }
})
