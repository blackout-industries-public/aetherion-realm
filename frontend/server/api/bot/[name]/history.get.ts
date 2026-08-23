// Proxied server-side so the browser never talks to the bridge directly, and a
// bridge outage degrades to an empty feed rather than a broken page.
import { instanceNames, instanceZoneNames } from '../../../utils/places'

const BRIDGE = process.env.NUXT_BRIDGE_URL || 'http://ai-bridge:8090'

export default defineEventHandler(async event => {
  const name = getRouterParam(event, 'name') ?? ''
  if (!/^[A-Za-z]{2,12}$/.test(name))
    throw createError({ statusCode: 400, statusMessage: 'bad name' })

  try {
    const [history, places, zones] = await Promise.all([
      $fetch<any>(`${BRIDGE}/bot/${name}/history`, { timeout: 5000 }),
      instanceNames(),
      instanceZoneNames(),
    ])

    // Rows written before the recorder learned instance names still say "map 48".
    // Naming them here means old history reads the same as new history.
    if (Array.isArray(history?.feed)) {
      history.feed = history.feed.map((e: any) => ({
        ...e,
        detail: String(e.detail ?? '')
          .replace(/instance on map (\d+)/,
            (whole: string, id: string) => places[Number(id)] ?? whole)
          .replace(/moved to zone (\d+)/,
            (whole: string, id: string) => {
              const named = zones[Number(id)]
              return named ? `moved to ${named}` : whole
            }),
      }))
    }
    return history
  } catch {
    return { found: false, unavailable: true }
  }
})
