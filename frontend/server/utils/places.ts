import { getPool } from './db'

// Zone ids come from the client's AreaTable.dbc, which this realm does not load, so a
// curated list covers the outdoor world and everything else falls back to its id.
// Instances are different: the world database names every one of them, so an instance
// map id can be resolved properly instead of guessed at.
let cache: { at: number; byMap: Record<number, string> } | null = null
const TTL_MS = 10 * 60 * 1000

// Instance zone ids are not in any table, but characters standing inside an instance
// report both their map and their zone. Pairing the two names every instance zone the
// realm actually uses, without a hand-curated list that would always be incomplete.
let zoneCache: { at: number; byZone: Record<number, string> } | null = null

export async function instanceZoneNames(): Promise<Record<number, string>> {
  if (zoneCache && Date.now() - zoneCache.at < TTL_MS) return zoneCache.byZone

  const byZone: Record<number, string> = {}
  try {
    const maps = await instanceNames()
    const [rows] = await getPool().query<any[]>(
      `SELECT DISTINCT map, zone FROM acore_characters.characters WHERE zone > 0`)
    for (const r of rows as any[]) {
      const name = maps[r.map]
      if (name && !byZone[r.zone]) byZone[r.zone] = name
    }
  } catch {
    // Characters table unreachable; callers fall back to the raw id.
  }

  zoneCache = { at: Date.now(), byZone }
  return byZone
}

export async function instanceNames(): Promise<Record<number, string>> {
  if (cache && Date.now() - cache.at < TTL_MS) return cache.byMap

  const byMap: Record<number, string> = {}
  try {
    const [rows] = await getPool().query<any[]>(
      `SELECT map_id, comment FROM acore_world.dungeon_access_template
       WHERE difficulty = 0 AND comment <> ''`)
    for (const r of rows as any[]) {
      // Comments carry qualifiers that read badly in a sentence: a size suffix
      // ("Naxxramas - 10man"), an abbreviation, and a complex prefix on the wings
      // ("Ulduar,Halls of Stone"). The wing is the part a reader recognises.
      byMap[r.map_id] = String(r.comment)
        .replace(/\s*-\s*\d+\s*man.*$/i, '')
        .replace(/\s*\((?:DM|WC)\)\s*$/i, '')
        .split(',').pop()!
        .trim()
    }
  } catch {
    // World schema unreachable; callers fall back to the raw id.
  }

  cache = { at: Date.now(), byMap }
  return byMap
}
