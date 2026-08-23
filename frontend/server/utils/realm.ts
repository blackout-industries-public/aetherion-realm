import { readFile } from 'node:fs/promises'

// The realm's own config directory, bind-mounted read-only. Reading the live file
// rather than duplicating values in the dashboard means the OPS and GROUPS panels
// cannot drift from what the worldserver actually loaded.
const CONFIG_DIR = process.env.NUXT_REALM_CONFIG || '/app/realm-config'

type Cached<T> = { at: number; value: T }
const TTL_MS = 15_000

let confCache: Cached<Record<string, string>> | null = null
let factsCache: Cached<RealmFacts> | null = null

export type RealmFacts = {
  realm: string
  address: string
  botPopulation: number
  llmModel: string
  llmBaseUrl: string
  pins: { name: string; commit: string }[]
  generatedAt: number
}

const NO_FACTS: RealmFacts = {
  realm: 'Aetherion', address: 'unknown', botPopulation: 0,
  llmModel: '', llmBaseUrl: '', pins: [], generatedAt: 0,
}

/** Parses `Key = Value` config lines, ignoring comments and stripping quotes. */
export async function playerbotsConf(): Promise<Record<string, string>> {
  if (confCache && Date.now() - confCache.at < TTL_MS) return confCache.value

  const out: Record<string, string> = {}
  try {
    const text = await readFile(`${CONFIG_DIR}/modules/playerbots.conf`, 'utf8')
    for (const line of text.split('\n')) {
      const trimmed = line.trim()
      if (!trimmed || trimmed.startsWith('#')) continue
      const eq = trimmed.indexOf('=')
      if (eq < 0) continue
      const key = trimmed.slice(0, eq).trim()
      const value = trimmed.slice(eq + 1).trim().replace(/^"|"$/g, '')
      if (key) out[key] = value
    }
  } catch {
    // Mount missing or unreadable. Callers fall back to their own defaults rather
    // than the page failing over a config file.
  }
  confCache = { at: Date.now(), value: out }
  return out
}

export async function realmFacts(): Promise<RealmFacts> {
  if (factsCache && Date.now() - factsCache.at < TTL_MS) return factsCache.value
  let value = NO_FACTS
  try {
    value = { ...NO_FACTS, ...JSON.parse(await readFile(`${CONFIG_DIR}/realm-facts.json`, 'utf8')) }
  } catch {
    // configure.sh has not run since this feature landed.
  }
  factsCache = { at: Date.now(), value }
  return value
}

export const confNum = (c: Record<string, string>, key: string, fallback: number) => {
  const n = Number(c[key])
  return Number.isFinite(n) ? n : fallback
}

export const confBool = (c: Record<string, string>, key: string, fallback: boolean) =>
  c[key] === undefined ? fallback : c[key] === '1' || c[key].toLowerCase() === 'true'
