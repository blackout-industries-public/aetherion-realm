import net from 'node:net'
import os from 'node:os'
import { readFile } from 'node:fs/promises'
import { getPool, q, dbLastError } from '../utils/db'
import { realmFacts, playerbotsConf, confBool } from '../utils/realm'

// The worldserver writes a row per boot, so this is the only honest record of how often
// the realm has actually restarted.
const UPTIME = `
  SELECT starttime, uptime, maxplayers
  FROM acore_auth.uptime ORDER BY starttime DESC LIMIT 8
`
const CHURN = `
  SELECT SUM(starttime > UNIX_TIMESTAMP()-86400)  AS boots24h,
         SUM(starttime > UNIX_TIMESTAMP()-604800) AS boots7d,
         COUNT(*)                                 AS bootsAll,
         ROUND(AVG(uptime)/3600, 2)               AS avgSessionH,
         ROUND(MAX(uptime)/3600, 2)               AS bestSessionH
  FROM acore_auth.uptime
`

// Is each feed still being written to? A container can report healthy while writing
// nothing, which is exactly how the LLM turns log went twenty hours stale unnoticed.
const FRESHNESS = `
  SELECT feed, last_write AS lastWrite, age_sec AS age, n
  FROM (
    SELECT 'assembler' AS feed, updated_at AS last_write,
           UNIX_TIMESTAMP()-updated_at AS age_sec, trips AS n
    FROM acore_characters.aetherion_assembler WHERE id = 1
    UNION ALL
    SELECT 'bot events', FLOOR(MAX(ts)), UNIX_TIMESTAMP()-FLOOR(MAX(ts)), COUNT(*)
    FROM aetherion_ai.bot_events
    UNION ALL
    SELECT 'econ events', FLOOR(MAX(ts)), UNIX_TIMESTAMP()-FLOOR(MAX(ts)), COUNT(*)
    FROM acore_characters.aetherion_econ_events
    UNION ALL
    SELECT 'llm turns', FLOOR(MAX(ts)), UNIX_TIMESTAMP()-FLOOR(MAX(ts)), COUNT(*)
    FROM aetherion_ai.turns
    UNION ALL
    SELECT 'llm relationships', FLOOR(MAX(last_seen)), UNIX_TIMESTAMP()-FLOOR(MAX(last_seen)), COUNT(*)
    FROM aetherion_ai.relationship
  ) f
`

// Whole five-minute buckets only; the in-progress bucket would always read as a dip.
const INGEST = `
  SELECT FROM_UNIXTIME(FLOOR(ts/300)*300, '%H:%i') AS bucket,
         COUNT(*) AS events, ROUND(COUNT(*)/5, 1) AS perMin
  FROM aetherion_ai.bot_events
  WHERE ts >= (FLOOR(UNIX_TIMESTAMP()/300)*300) - 3600
    AND ts <  (FLOOR(UNIX_TIMESTAMP()/300)*300)
  GROUP BY FLOOR(ts/300)*300, bucket
  ORDER BY FLOOR(ts/300)*300
`

// data_length is reliable. table_rows from the same view is an InnoDB sampling estimate
// and was measured 5x understated, so it is deliberately not selected here.
const FOOTPRINT = `
  SELECT table_schema AS db, COUNT(*) AS tables,
         ROUND(SUM(data_length+index_length)/1048576, 1) AS totalMb
  FROM information_schema.tables
  WHERE table_schema IN ('acore_world','acore_characters','acore_playerbots','acore_auth','aetherion_ai')
  GROUP BY table_schema ORDER BY SUM(data_length+index_length) DESC
`

// The six rows the redesign's census card asks for, all countable exactly.
const CENSUS = `
  SELECT 'characters' AS metric, COUNT(*) AS value FROM acore_characters.characters
  UNION ALL SELECT 'online now',      COUNT(*) FROM acore_characters.characters WHERE online=1
  UNION ALL SELECT 'guilds',          COUNT(*) FROM acore_characters.guild
  UNION ALL SELECT 'auctions live',   COUNT(*) FROM acore_characters.auctionhouse
  UNION ALL SELECT 'mail in flight',  COUNT(*) FROM acore_characters.mail
  UNION ALL SELECT 'items instanced', COUNT(*) FROM acore_characters.item_instance
`


// Reachability, not `docker ps`. Handing the dashboard the Docker socket to print a
// status dot would trade real privilege for cosmetic detail; a TCP connect proves the
// thing a reader actually cares about, which is whether the service answers.
const SERVICES = [
  { name: 'ac-worldserver', host: 'ac-worldserver', port: 8085, note: 'world simulation, bots, our C++ patches' },
  { name: 'ac-authserver', host: 'ac-authserver', port: 3724, note: 'SRP6 login, realm list' },
  { name: 'ac-database', host: 'ac-database', port: 3306, note: 'mysql 8.4 · five schemas' },
  { name: 'ai-bridge', host: 'ai-bridge', port: 8090, note: 'FastAPI LLM mediator' },
]

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

// Under Docker without lxcfs, /proc/meminfo is the host's. That is what we want here:
// the panel is about the machine the realm runs on, not this container.
async function hostMemory() {
  try {
    const text = await readFile('/proc/meminfo', 'utf8')
    const read = (key: string) => {
      const m = new RegExp(`^${key}:\\s+(\\d+) kB`, 'm').exec(text)
      return m ? Number(m[1]) * 1024 : 0
    }
    const total = read('MemTotal')
    const avail = read('MemAvailable')
    if (total) return { total, used: total - avail, source: 'host' as const }
  } catch { /* fall through */ }
  return { total: os.totalmem(), used: os.totalmem() - os.freemem(), source: 'container' as const }
}

export default defineCachedEventHandler(async () => {
  const [facts, conf, mem, states, uptime, churn, freshness, ingest, footprint, census] =
    await Promise.all([
      realmFacts(),
      playerbotsConf(),
      hostMemory(),
      Promise.all(SERVICES.map(s => probe(s.host, s.port))),
      q(UPTIME), q(CHURN), q(FRESHNESS), q(INGEST), q(FOOTPRINT), q(CENSUS),
    ])

  const containers = SERVICES.map((s, i) => ({ ...s, up: states[i] }))
  // This dashboard is itself part of the stack; it is trivially up if you are reading
  // it, and saying so is more honest than probing our own port.
  containers.push({
    name: 'warcraft-map', host: 'localhost', port: 3000,
    note: 'Nuxt dashboard · you are here', up: true,
  })

  return {
    at: Date.now(),
    realm: facts.realm,
    address: facts.address,
    botPopulation: facts.botPopulation,
    pins: facts.pins,
    factsGeneratedAt: facts.generatedAt,
    containers,
    host: {
      cores: os.cpus().length,
      memTotal: mem.total,
      memUsed: mem.used,
      memSource: mem.source,
    },
    boots: uptime.map(r => ({
      at: Number(r.starttime) * 1000,
      uptime: Number(r.uptime),
    })),
    churn: churn[0]
      ? {
          boots24h: Number(churn[0].boots24h), boots7d: Number(churn[0].boots7d),
          bootsAll: Number(churn[0].bootsAll),
          avgSessionH: Number(churn[0].avgSessionH), bestSessionH: Number(churn[0].bestSessionH),
        }
      : null,
    freshness: freshness.map(r => ({
      feed: r.feed,
      age: r.age === null ? null : Number(r.age),
      n: Number(r.n),
      // Thresholds match how often each writer is meant to run, not a generic guess.
      status: r.age === null ? 'unknown'
        : Number(r.age) < 120 ? 'live' : Number(r.age) < 900 ? 'lagging' : 'stale',
    })),
    ingest: ingest.map(r => ({ bucket: r.bucket, events: Number(r.events), perMin: Number(r.perMin) })),
    footprint: footprint.map(r => ({ db: r.db, tables: Number(r.tables), totalMb: Number(r.totalMb) })),
    census: census.map(r => ({ metric: r.metric, value: Number(r.value) })),
    dbError: dbLastError(),
    llmHookEnabled: confBool(conf, 'AiPlayerbot.Llm.Enabled', false),
    pvpEnabled: confBool(conf, 'AiPlayerbot.Pvp.Enabled', false),
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'ops',
})
