import { getPool } from '../utils/db'
import { instanceNames } from '../utils/places'

// playersInfo is a formatted debug string, not structured data. Each participant
// appears as "Name (GUID Full: ... Low: <guid>, acc: N, ip: ..., guild: <id>)".
const PARTICIPANT = /Low:\s*(\d+),[^)]*?guild:\s*(\d+)/g

export default defineEventHandler(async () => {
  const pool = getPool()
  const INSTANCES = await instanceNames()

  const [guildRows] = await pool.query<any[]>(`
    SELECT g.guildid, g.name, COUNT(gm.guid) AS members,
           SUM(c.online) AS online, MIN(c.level) AS minLevel,
           MAX(c.level) AS maxLevel, ROUND(AVG(c.level)) AS avgLevel,
           SUM(c.race IN (1,3,4,7,11)) AS alliance
    FROM acore_characters.guild g
    JOIN acore_characters.guild_member gm ON gm.guildid = g.guildid
    JOIN acore_characters.characters c    ON c.guid = gm.guid
    GROUP BY g.guildid, g.name
  `)

  const beatenRows = await q(`
    SELECT COALESCE(ct.name, ie.comment) AS boss,
           dat.comment                   AS instance,
           COUNT(*)                      AS kills
    FROM acore_characters.instance i
    JOIN acore_world.dungeonencounter_dbc de
      ON de.MapID = i.map AND de.Difficulty = i.difficulty
    JOIN acore_world.instance_encounters ie ON ie.entry = de.ID
    LEFT JOIN acore_world.creature_template ct
      ON ct.entry = ie.creditEntry AND ie.creditType = 0
    LEFT JOIN acore_world.dungeon_access_template dat
      ON dat.map_id = i.map AND dat.difficulty = 0
    WHERE i.completedEncounters & (1 << de.Bit)
    GROUP BY boss, instance
    ORDER BY kills DESC, boss
    LIMIT 12
  `)

  const [killRows] = await pool.query<any[]>(`
    SELECT le.time, le.map, le.difficulty, le.creditEntry, le.playersInfo,
           ct.name AS boss
    FROM acore_characters.log_encounter le
    LEFT JOIN acore_world.creature_template ct ON ct.entry = le.creditEntry
    ORDER BY le.time DESC LIMIT 200
  `)

  // Attribute each kill to the guilds that were present.
  const guildKills = new Map<number, number>()
  const kills = (killRows as any[]).map(r => {
    const guilds = new Set<number>()
    let players = 0
    for (const m of String(r.playersInfo ?? '').matchAll(PARTICIPANT)) {
      players++
      const gid = Number(m[2])
      if (gid) guilds.add(gid)
    }
    for (const gid of guilds) guildKills.set(gid, (guildKills.get(gid) ?? 0) + 1)

    return {
      at: r.time,
      boss: r.boss ?? `Encounter ${r.creditEntry}`,
      instance: INSTANCES[r.map] ?? `Map ${r.map}`,
      heroic: r.difficulty > 0,
      players,
      guilds: [...guilds],
    }
  })

  const guilds = (guildRows as any[]).map(g => ({
    id: g.guildid,
    name: g.name,
    members: g.members,
    online: Number(g.online ?? 0),
    minLevel: g.minLevel,
    maxLevel: g.maxLevel,
    avgLevel: g.avgLevel,
    faction: g.alliance > g.members / 2 ? 'alliance' : 'horde',
    bossKills: guildKills.get(g.guildid) ?? 0,
  })).sort((a, b) => b.bossKills - a.bossKills || b.avgLevel - a.avgLevel)

  return {
    at: Date.now(),
    totalGuilds: guilds.length,
    totalKills: kills.length,
    // Distinct bosses beaten anywhere on the realm - the closest thing to a
    // server-wide progression figure this schema supports.
    bossesBeaten: new Set(kills.map(k => `${k.instance}:${k.boss}`)).size,
    // Named boss kills from live instance state. Empty until seed-encounters.sh runs.
    beaten: ((beatenRows ?? []) as any[]).map(r => ({
      boss: r.boss,
      instance: String(r.instance ?? '').replace(/\s*-\s*\d+\s*man.*$/i, '').split(',').pop()!.trim(),
      kills: Number(r.kills) || 0,
    })),
    guilds: guilds.slice(0, 20),
    kills: kills.slice(0, 15),
  }
})
