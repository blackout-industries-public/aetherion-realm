import { getPool } from '../utils/db'
import { instanceNames, instanceZoneNames } from '../utils/places'

// The bridge's activity recorder writes here, keyed by character guid. It is the only
// continuous record of what bots actually did, so it backs the live feed.
const SQL = `
  SELECT e.id, e.guid, e.ts, e.kind, e.detail,
         c.name, c.class AS cls, c.level, c.zone, c.race, c.map
  FROM aetherion_ai.bot_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  ORDER BY e.id DESC
  LIMIT 60
`

// Only the kinds the recorder actually emits. Anything new falls through to its own
// name rather than being silently mislabelled.
const TAG: Record<string, string> = {
  zone: 'travel',
  party: 'group',
  instance: 'instance',
  level: 'progress',
  death: 'world pvp',
  revive: 'world pvp',
  loot: 'loot',
  pvp: 'world pvp',
}

const ALLIANCE = new Set([1, 3, 4, 7, 11])

export default defineCachedEventHandler(async () => {
  let rows: any[] = []
  const [names, zoneNames] = await Promise.all([instanceNames(), instanceZoneNames()])
  try {
    rows = (await getPool().query<any[]>(SQL))[0] as any[]
  } catch {
    // Schema absent until the bridge has recorded its first event.
    return { at: Date.now(), ready: false, events: [] }
  }

  return {
    at: Date.now(),
    ready: true,
    events: rows.map(r => {
      // "moved to zone 4395" carries an id the client can name properly; lift it out
      // rather than printing a raw number at the reader.
      const zoneMatch = /^moved to zone (\d+)$/.exec(r.detail ?? '')
      // "entered instance on map 129" names a place the world database knows.
      const detail = String(r.detail ?? '').replace(
        /instance on map (\d+)/,
        (whole: string, id: string) => names[Number(id)] ? names[Number(id)]! : whole)
      return {
        id: r.id,
        guid: r.guid,
        who: r.name,
        cls: r.cls ?? 0,
        level: r.level ?? 0,
        faction: ALLIANCE.has(r.race) ? 'alliance' : 'horde',
        kind: r.kind,
        tag: TAG[r.kind] ?? r.kind,
        // Set when the event is a zone change, so the client can render the name.
        movedToZone: zoneMatch ? Number(zoneMatch[1]) : null,
        // Named here when it is an instance zone the client's table cannot cover.
        movedToName: zoneMatch ? (zoneNames[Number(zoneMatch[1])] ?? null) : null,
        detail,
        // Set when the character is standing in a named instance, so the feed can say
        // where they are instead of printing a zone id nothing can resolve.
        place: names[r.map] ?? null,
        zone: r.zone ?? 0,
        at: Math.round(Number(r.ts) * 1000),
      }
    }),
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'events',
})
