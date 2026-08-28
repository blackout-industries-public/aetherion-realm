import { getPool } from '../utils/db'
import { playerbotsConf, confBool, confNum, realmFacts } from '../utils/realm'
import { archetypeSplit } from '../utils/archetype'

const BRIDGE = process.env.NUXT_BRIDGE_URL || 'http://ai-bridge:8090'

// Recent conversation, newest first. Only assistant lines are bot speech; user lines
// are whoever spoke to them.
const CHATTER = `
  SELECT t.id, t.bot_guid, t.role, t.content, t.ts, t.speaker,
         c.name, c.class AS cls, c.level
  FROM aetherion_ai.turns t
  LEFT JOIN acore_characters.characters c ON c.guid = t.bot_guid
  ORDER BY t.id DESC
  LIMIT 24
`

const BOT_GUIDS = `
  SELECT c.guid FROM acore_characters.characters c
  JOIN acore_playerbots.playerbots_account_type t ON t.account_id = c.account
  WHERE c.online = 1
`

export default defineCachedEventHandler(async () => {
  const pool = getPool()
  const conf = await playerbotsConf()
  const facts = await realmFacts()

  // The in-game hook and the bridge are independent: the bridge can be healthy while
  // the worldserver is not calling it, which is exactly the state this panel exists to
  // make visible.
  const hookEnabled = confBool(conf, 'AiPlayerbot.Llm.Enabled', false)

  const [health, metrics, chatter, guids] = await Promise.all([
    $fetch<any>(`${BRIDGE}/health`, { timeout: 3000 }).catch(() => null),
    $fetch<any>(`${BRIDGE}/metrics`, { timeout: 3000 }).catch(() => null),
    q(CHATTER),
    q(BOT_GUIDS),
  ])

  const lat = metrics?.latency_seconds ?? {}

  return {
    at: Date.now(),
    hook: {
      enabled: hookEnabled,
      host: conf['AiPlayerbot.Llm.Host'] ?? 'ai-bridge',
      port: confNum(conf, 'AiPlayerbot.Llm.Port', 8090),
      requiresWitness: confBool(conf, 'AiPlayerbot.Llm.RequireHumanWitness', true),
      maxInFlight: confNum(conf, 'AiPlayerbot.Llm.MaxInFlight', 8),
      sayRange: confNum(conf, 'AiPlayerbot.Llm.SayRange', 45),
    },
    bridge: {
      up: !!health,
      backend: health?.llm_backend ?? 'unknown',
      model: health?.model_interactive ?? facts.llmModel,
      reasoning: health?.reasoning_effort ?? 'unknown',
      url: health?.llm_url ?? facts.llmBaseUrl,
      served: metrics?.counters?.served ?? 0,
      p50: typeof lat.p50 === 'number' ? lat.p50 : null,
      p95: typeof lat.p95 === 'number' ? lat.p95 : null,
      mean: typeof lat.mean === 'number' ? lat.mean : null,
    },
    chatter: (chatter as any[]).map(t => ({
      id: t.id,
      guid: t.bot_guid,
      who: t.role === 'assistant' ? (t.name ?? `#${t.bot_guid}`) : t.speaker,
      bot: t.role === 'assistant',
      cls: t.role === 'assistant' ? (t.cls ?? 0) : 0,
      text: t.content,
      at: Math.round(Number(t.ts) * 1000),
    })),
    archetypes: archetypeSplit((guids as any[]).map(g => g.guid)),
  }
}, {
  // The realm changes on a minute's timescale, so a reader cannot tell
  // twenty seconds of staleness from live - but they can certainly tell
  // four seconds of waiting. Stale answers are served instantly while a
  // refresh runs behind them.
  maxAge: 20, swr: true, staleMaxAge: 600, name: 'llm',
})
