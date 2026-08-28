// Keeps the cache hot so no reader ever pays for a cold one.
//
// Without this the first visitor after a deploy still waits the full four
// seconds for the race tables, and so does the first visitor after any window
// expires while nobody is looking. The server is the right one to absorb that
// cost, so it fetches its own endpoints on a timer.

const WARM = [
  '/api/race', '/api/econ', '/api/assembler', '/api/world', '/api/ops',
  '/api/guild', '/api/society', '/api/pvp', '/api/quests', '/api/guilds',
  '/api/combat', '/api/llm', '/api/events', '/api/progression', '/api/debug',
  '/api/gearmarket', '/api/market', '/api/wealth', '/api/pulse', '/api/industry',
  '/api/runs',
]

export default defineNitroPlugin(() => {
  const warm = async () => {
    for (const path of WARM) {
      // Sequential on purpose: this is background work and there is no reason to
      // throw twenty concurrent queries at the realm's database.
      try { await $fetch(path) } catch { /* a broken endpoint degrades on its own */ }
    }
  }

  // Let the server finish coming up before the first sweep.
  setTimeout(warm, 2000)
  setInterval(warm, 30_000)
})
