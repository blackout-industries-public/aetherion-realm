import net from 'node:net'
import { getPool } from '../utils/db'
import { instanceNames } from '../utils/places'

type Row = {
  guid: number; name: string; race: number; class: number; level: number
  map: number; zone: number; x: number; y: number; is_bot: number
  health: number; instance_id: number; in_group: number; death_expire_time: number
  today_kills: number
}

type Entity = {
  guid: number; name: string; level: number; map: number; zone: number
  x: number; y: number; cls: number; race: number; faction: string; bot: boolean
  dead: boolean; ghost: boolean; instance: number; grouped: boolean
  travelling: boolean; onTrip: boolean; atDoor: boolean; prof: number; kills: number
  rpg: number; trade: string | null
  // The instance they are actually standing in, kept because `map` is rewritten to the
  // outdoor door for plotting and would otherwise lose it.
  instanceMap: number; place: string | null
}

// Alliance races: human, dwarf, night elf, gnome, draenei.
const ALLIANCE = new Set([1, 3, 4, 7, 11])

// Last non-empty snapshot. A restarting worldserver reports nobody online, and a
// dashboard that blanks itself during a restart is useless precisely when you are
// watching it - so the previous state is served instead, clearly marked stale.
let lastGood: { at: number; entities: Entity[] } | null = null

function probe(host: string, port: number, timeoutMs = 1200): Promise<boolean> {
  return new Promise(resolve => {
    const sock = new net.Socket()
    const done = (ok: boolean) => { sock.destroy(); resolve(ok) }
    sock.setTimeout(timeoutMs)
    sock.once('connect', () => done(true))
    sock.once('timeout', () => done(false))
    sock.once('error', () => done(false))
    sock.connect(port, host)
  })
}

// Who is on a journey, straight from the assembler's own telemetry. Inferring this
// from position deltas does not work: positions save once a minute while the page
// polls every five seconds, so a walking bot reads as idle almost all of the time.
// Instance maps have no place on a continent, so a quarter of the realm would simply
// not be drawn. Their dungeon's outdoor door is where those characters actually are
// from the world's point of view, so they are plotted there instead.
const DOORS = `
  SELECT att.target_map AS instance_map, at.map AS outdoor_map, at.x, at.y
  FROM acore_world.areatrigger at
  JOIN acore_world.areatrigger_teleport att ON att.ID = at.entry
  WHERE att.Name LIKE '%Entrance%' AND att.Name NOT LIKE '%Inside%'
    AND att.Name NOT LIKE '%Exit%'
    -- Some triggers named "Entrance" lead back out to a continent. Without this the
    -- lookup gains keys 0/1/530/571 and every outdoor character gets relocated onto
    -- an instance map.
    AND att.target_map NOT IN (0, 1, 530, 571)
`

// Professions, as a bitmask. 2500 characters x 11 skills is far too much to ship as
// arrays; one integer per character keeps the payload flat and the client decodes it.
export const PROFESSIONS = [
  { bit: 0, skill: 186, name: 'Mining' },
  { bit: 1, skill: 182, name: 'Herbalism' },
  { bit: 2, skill: 393, name: 'Skinning' },
  { bit: 3, skill: 171, name: 'Alchemy' },
  { bit: 4, skill: 202, name: 'Engineering' },
  { bit: 5, skill: 164, name: 'Blacksmithing' },
  { bit: 6, skill: 165, name: 'Leatherworking' },
  { bit: 7, skill: 755, name: 'Jewelcrafting' },
  { bit: 8, skill: 773, name: 'Inscription' },
  { bit: 9, skill: 197, name: 'Tailoring' },
  { bit: 10, skill: 333, name: 'Enchanting' },
]

const SKILL_BITS = `
  SELECT cs.guid, BIT_OR(CASE cs.skill
    ${PROFESSIONS.map(p => `WHEN ${p.skill} THEN ${1 << p.bit}`).join(' ')}
    ELSE 0 END) AS mask
  FROM acore_characters.character_skills cs
  WHERE cs.skill IN (${PROFESSIONS.map(p => p.skill).join(',')})
  GROUP BY cs.guid
`

// RPG status per bot, exported by the worldserver each assembler tick. Absence of a
// row means idle. Values: 1 GoGrind, 2 GoCamp, 3 WanderRandom, 4 WanderNpc, 5 DoQuest,
// 6 TravelFlight, 7 Rest, 8 OutdoorPvp (PlayerbotAIConfig.h NewRpgStatus).
const ACTIVITY = `SELECT guid, status FROM acore_characters.aetherion_bot_activity`

// A trade skill-up is honest evidence the bot was just working that profession; ten
// minutes is the freshness window before it stops being "now".
const RECENT_TRADE = `
  SELECT e.guid, e.detail
  FROM aetherion_ai.bot_events e
  WHERE e.kind = 'profession' AND e.ts > UNIX_TIMESTAMP() - 600
  ORDER BY e.id
`

const JOURNEYS = `
  SELECT m.guid, t.phase
  FROM acore_characters.aetherion_party_members m
  JOIN acore_characters.aetherion_party_trips t ON t.group_id = m.group_id
`

let profCache: { at: number; masks: Map<number, number> } | null = null

async function professionMasks(): Promise<Map<number, number>> {
  if (profCache && Date.now() - profCache.at < 300_000) return profCache.masks
  const masks = new Map<number, number>()
  try {
    const [rows] = await getPool().query<any[]>(SKILL_BITS)
    for (const r of rows as any[]) masks.set(r.guid, Number(r.mask) || 0)
  } catch { /* skills unreadable; the professions layer just shows nothing */ }
  profCache = { at: Date.now(), masks }
  return masks
}

export default defineEventHandler(async () => {
  const realmUp = await probe('ac-worldserver', 8085)

  // Kept out of the main query so a cold start without these tables degrades to
  // "no journeys" rather than failing the whole map.
  // Skills change on a scale of hours; the map polls every few seconds. Cached so the
  // heaviest query on this endpoint does not run 20 times a minute.
  const profs = await professionMasks()

  const journeys = new Map<number, string>()
  try {
    const [rows] = await getPool().query<any[]>(JOURNEYS)
    for (const r of rows as any[]) journeys.set(r.guid, r.phase)
  } catch { /* telemetry not written yet */ }

  const rpgByGuid = new Map<number, number>()
  for (const r of await q(ACTIVITY)) rpgByGuid.set(r.guid, Number(r.status))

  // Last skill-up wins; the detail already reads "Mining 154".
  const tradeByGuid = new Map<number, string>()
  for (const r of await q(RECENT_TRADE)) tradeByGuid.set(r.guid, r.detail)

  // First door wins, matching how the assembler picks one.
  const doors = new Map<number, { map: number; x: number; y: number }>()
  try {
    const [rows] = await getPool().query<any[]>(DOORS)
    for (const r of rows as any[]) {
      if (!doors.has(r.instance_map))
        doors.set(r.instance_map, { map: r.outdoor_map, x: r.x, y: r.y })
    }
  } catch { /* world schema unreachable */ }

  const placeNames = await instanceNames()

  const [rows] = await getPool().query<any[]>(`
    SELECT c.guid, c.name, c.race, c.class, c.level, c.map, c.zone,
           ROUND(c.position_x) AS x, ROUND(c.position_y) AS y,
           (t.account_id IS NOT NULL) AS is_bot,
           c.health, c.instance_id, c.death_expire_time, c.todayKills AS today_kills,
           (gm.memberGuid IS NOT NULL) AS in_group
    FROM acore_characters.characters c
    LEFT JOIN acore_playerbots.playerbots_account_type t ON t.account_id = c.account
    LEFT JOIN acore_characters.group_member gm ON gm.memberGuid = c.guid
    WHERE c.online = 1
  `)

  const entities: Entity[] = (rows as Row[]).map(r => {
    // Anywhere the continent table does not cover is an instance; draw them at its door.
    const door = doors.get(r.map)
    const atDoor = !!door
    return {
    instanceMap: atDoor ? r.map : 0,
    place: atDoor ? (placeNames[r.map] ?? null) : null,
    guid: r.guid, name: r.name, level: r.level,
    map: door ? door.map : r.map,
    zone: r.zone,
    x: door ? door.x : r.x, y: door ? door.y : r.y,
    atDoor,
    cls: r.class, race: r.race,
    faction: ALLIANCE.has(r.race) ? 'alliance' : 'horde',
    bot: !!r.is_bot,
    // Enough to derive what a bot is doing without a combat log: dead, inside an
    // instance, or grouped. Movement is worked out client-side by comparing polls.
    // WoW puts a ghost at exactly 1 HP, so health 1 with a corpse that has not yet
    // expired means the character is dead and running back. death_expire_time alone
    // is useless here: it is never cleared on resurrection.
    dead: r.health === 0,
    ghost: r.health <= 1 && r.death_expire_time * 1000 > Date.now(),
    instance: r.instance_id,
    grouped: !!r.in_group,
    // 'travelling' and 'summoning' both mean the party is still en route.
    travelling: journeys.get(r.guid) === 'travelling' || journeys.get(r.guid) === 'summoning',
    onTrip: journeys.has(r.guid),
    prof: profs.get(r.guid) ?? 0,
    kills: r.today_kills ?? 0,
    rpg: rpgByGuid.get(r.guid) ?? 0,
    trade: tradeByGuid.get(r.guid) ?? null,
    }
  })

  if (entities.length) {
    lastGood = { at: Date.now(), entities }
    return { at: lastGood.at, realmUp, stale: false, entities, professions: PROFESSIONS }
  }

  if (lastGood) {
    return { at: lastGood.at, realmUp, stale: true, entities: lastGood.entities, professions: PROFESSIONS }
  }

  return { at: Date.now(), realmUp, stale: false, entities: [], professions: PROFESSIONS }
})
