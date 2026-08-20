import net from 'node:net'
import { getPool } from '../utils/db'

type Row = {
  guid: number; name: string; race: number; class: number; level: number
  map: number; zone: number; x: number; y: number; is_bot: number
}

type Entity = {
  guid: number; name: string; level: number; map: number; zone: number
  x: number; y: number; cls: number; race: number; faction: string; bot: boolean
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

export default defineEventHandler(async () => {
  const realmUp = await probe('ac-worldserver', 8085)

  const [rows] = await getPool().query<any[]>(`
    SELECT c.guid, c.name, c.race, c.class, c.level, c.map, c.zone,
           ROUND(c.position_x) AS x, ROUND(c.position_y) AS y,
           (t.account_id IS NOT NULL) AS is_bot
    FROM acore_characters.characters c
    LEFT JOIN acore_playerbots.playerbots_account_type t ON t.account_id = c.account
    WHERE c.online = 1
  `)

  const entities: Entity[] = (rows as Row[]).map(r => ({
    guid: r.guid, name: r.name, level: r.level, map: r.map, zone: r.zone,
    x: r.x, y: r.y, cls: r.class, race: r.race,
    faction: ALLIANCE.has(r.race) ? 'alliance' : 'horde',
    bot: !!r.is_bot,
  }))

  if (entities.length) {
    lastGood = { at: Date.now(), entities }
    return { at: lastGood.at, realmUp, stale: false, entities }
  }

  if (lastGood) {
    return { at: lastGood.at, realmUp, stale: true, entities: lastGood.entities }
  }

  return { at: Date.now(), realmUp, stale: false, entities: [] }
})
