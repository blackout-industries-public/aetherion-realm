// Personality lives in the AI Bridge, not the game database - it is derived from the
// character GUID rather than stored. Proxied server-side so the browser never needs
// to reach the bridge directly, and so a bridge outage degrades to "unknown" rather
// than a failed page.
const BRIDGE = process.env.NUXT_BRIDGE_URL || 'http://ai-bridge:8090'

export default defineEventHandler(async event => {
  const name = getRouterParam(event, 'name') ?? ''
  if (!/^[A-Za-z]{2,12}$/.test(name))
    throw createError({ statusCode: 400, statusMessage: 'bad name' })

  try {
    return await $fetch(`${BRIDGE}/bot/${name}`, { timeout: 3000 })
  } catch {
    return { found: false, unavailable: true }
  }
})
