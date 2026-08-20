import { getPool } from '../utils/db'

// `groups` is a reserved word in MySQL 8, hence the backticks.
const SQL = `
  -- groupType is a bitmask (GROUPTYPE_RAID = 0x02, GROUPTYPE_LFG = 0x08); there is
  -- no isRaid column. The LFG bit shows whether a group came from the dungeon finder
  -- rather than a nearby invite, which is the distinction worth debugging.
  SELECT g.guid AS gid, g.leaderGuid,
         (g.groupType & 2) AS isRaid, (g.groupType & 8) AS viaLfg,
         c.guid, c.name, c.level, c.class AS cls, c.race, c.map, c.zone,
         c.online, c.instance_id,
         ROUND(c.position_x) AS x, ROUND(c.position_y) AS y,
         (t.account_id IS NOT NULL) AS is_bot
  FROM acore_characters.\`groups\` g
  JOIN acore_characters.group_member gm ON gm.guid = g.guid
  JOIN acore_characters.characters c    ON c.guid = gm.memberGuid
  LEFT JOIN acore_playerbots.playerbots_account_type t ON t.account_id = c.account
  ORDER BY g.guid, c.level DESC
`

const ALLIANCE = new Set([1, 3, 4, 7, 11])

export default defineEventHandler(async () => {
  const [rows] = await getPool().query<any[]>(SQL)

  const groups = new Map<number, any>()
  for (const r of rows as any[]) {
    let g = groups.get(r.gid)
    if (!g) {
      g = {
        id: r.gid, leaderGuid: r.leaderGuid,
        raid: !!r.isRaid, viaLfg: !!r.viaLfg,
        members: [], inInstance: false, allBots: true,
      }
      groups.set(r.gid, g)
    }
    g.members.push({
      guid: r.guid, name: r.name, level: r.level, cls: r.cls,
      faction: ALLIANCE.has(r.race) ? 'alliance' : 'horde',
      map: r.map, zone: r.zone, x: r.x, y: r.y,
      online: !!r.online, instance: r.instance_id, bot: !!r.is_bot,
      leader: r.guid === r.leaderGuid,
    })
    if (r.instance_id > 0) g.inInstance = true
    if (!r.is_bot) g.allBots = false
  }

  const list = [...groups.values()].map(g => ({
    ...g,
    size: g.members.length,
    online: g.members.filter((m: any) => m.online).length,
    // A group spread across maps is mid-travel or half logged out; worth seeing.
    maps: [...new Set(g.members.map((m: any) => m.map))],
    minLevel: Math.min(...g.members.map((m: any) => m.level)),
    maxLevel: Math.max(...g.members.map((m: any) => m.level)),
  })).sort((a, b) => b.size - a.size)

  return {
    at: Date.now(),
    total: list.length,
    inInstances: list.filter(g => g.inInstance).length,
    raids: list.filter(g => g.raid).length,
    groups: list,
  }
})
