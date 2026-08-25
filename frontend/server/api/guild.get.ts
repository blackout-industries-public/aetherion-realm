import { getPool } from '../utils/db'

// Guilds form on their own here - nobody asked for sixty of them. This backs a tab of
// their own because the Society panel only had room for a top-nine list.

// Standings. Achievements and boss counts come in as sub-selects rather than more joins
// on the main aggregate, which would multiply the member rows.
const STANDINGS = `
  SELECT g.guildid AS id, g.name,
         lc.name AS master, lc.level AS masterLevel, lc.class AS masterClass,
         COUNT(*) AS members,
         SUM(c.online) AS online,
         ROUND(AVG(c.level), 1) AS avgLevel,
         MAX(c.level) AS topLevel,
         SUM(c.level = 80) AS atCap,
         ROUND(SUM(c.money) / 10000) AS gold,
         SUM(c.totalKills) AS kills,
         SUM(c.race IN (1,3,4,7,11)) AS alliance,
         COALESCE(p.grouped, 0) AS grouped,
         COALESCE(a.achv, 0) AS achievements
  FROM acore_characters.guild g
  JOIN acore_characters.guild_member gm ON gm.guildid = g.guildid
  JOIN acore_characters.characters  c  ON c.guid = gm.guid
  LEFT JOIN acore_characters.characters lc ON lc.guid = g.leaderguid
  LEFT JOIN (
    SELECT gm3.guildid, COUNT(*) AS grouped
    FROM acore_characters.group_member gmem
    JOIN acore_characters.guild_member gm3 ON gm3.guid = gmem.memberGuid
    GROUP BY gm3.guildid
  ) p ON p.guildid = g.guildid
  LEFT JOIN (
    SELECT gm4.guildid, COUNT(*) AS achv
    FROM acore_characters.character_achievement ca
    JOIN acore_characters.guild_member gm4 ON gm4.guid = ca.guid
    GROUP BY gm4.guildid
  ) a ON a.guildid = g.guildid
  GROUP BY g.guildid, g.name, lc.name, lc.level, lc.class, p.grouped, a.achv
  ORDER BY avgLevel DESC, members DESC
`

// What each guild has actually done in the last hour. This is the panel that makes the
// tab live rather than a roster listing.
const PULSE = `
  SELECT g.guildid AS id,
         COUNT(*) AS events,
         COUNT(DISTINCT e.guid) AS active,
         SUM(e.kind = 'level')    AS levels,
         SUM(e.kind = 'loot')     AS loot,
         SUM(e.kind = 'death')    AS deaths,
         SUM(e.kind = 'instance') AS instances
  FROM aetherion_ai.bot_events e
  JOIN acore_characters.guild_member gm ON gm.guid = e.guid
  JOIN acore_characters.guild g ON g.guildid = gm.guildid
  WHERE e.ts > UNIX_TIMESTAMP() - 3600
  GROUP BY g.guildid
`

// Which bosses a guild's members are bound to having killed. Only possible because
// dungeonencounter_dbc is seeded - see scripts/seed-encounters.sh.
// Grouped by encounter id with aggregated name columns: grouping by the COALESCE
// aliases trips ONLY_FULL_GROUP_BY, and this query shipped broken that way - the
// error surfaced the moment q() started logging instead of swallowing.
const PROGRESS = `
  SELECT gm.guildid AS id,
         COALESCE(MIN(ct.name), MIN(ie.comment)) AS boss,
         COALESCE(MIN(dat.comment), CONCAT('Map ', MIN(i.map))) AS instance
  FROM acore_characters.guild_member gm
  JOIN acore_characters.character_instance ci ON ci.guid = gm.guid
  JOIN acore_characters.instance i ON i.id = ci.instance
  JOIN acore_world.dungeonencounter_dbc de
    ON de.MapID = i.map AND de.Difficulty = i.difficulty
  JOIN acore_world.instance_encounters ie ON ie.entry = de.ID
  LEFT JOIN acore_world.creature_template ct
    ON ct.entry = ie.creditEntry AND ie.creditType = 0
  LEFT JOIN acore_world.dungeon_access_template dat
    ON dat.map_id = i.map AND dat.difficulty = 0
  WHERE i.completedEncounters & (1 << de.Bit)
  GROUP BY gm.guildid, ie.entry
`


// What the vault holds right now. Tabs are counted because a guild with no tab
// cannot receive items at all, which is the difference between an empty vault
// and a broken one.
const VAULT = `
  SELECT g.guildid AS id,
         g.BankMoney AS bankCopper,
         COALESCE(bi.items, 0) AS items,
         COALESCE(bt.tabs, 0)  AS tabs
  FROM acore_characters.guild g
  LEFT JOIN (
    SELECT guildid, COUNT(*) AS items FROM acore_characters.guild_bank_item GROUP BY guildid
  ) bi ON bi.guildid = g.guildid
  LEFT JOIN (
    SELECT guildid, COUNT(*) AS tabs FROM acore_characters.guild_bank_tab GROUP BY guildid
  ) bt ON bt.guildid = g.guildid
`

// A day of vault movement, attributed through the depositor's guild membership.
// Tithes and repairs carry copper; deposits and withdrawals carry item counts.
const VAULT_FLOW = `
  SELECT gm.guildid AS id,
         SUM(e.kind = 'guild_tithe')                                              AS tithes,
         SUM(IF(e.kind = 'guild_tithe', e.count, 0))                              AS titheCopper,
         SUM(IF(e.kind = 'guild_bank_deposit', e.count, 0))                       AS deposited,
         SUM(IF(e.kind = 'guild_bank_withdraw', e.count, 0))                      AS withdrawn,
         SUM(e.kind = 'guild_repair')                                             AS repairs,
         SUM(IF(e.kind = 'guild_repair', CAST(NULLIF(e.detail,'') AS SIGNED), 0)) AS repairCopper
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.guild_member gm ON gm.guid = e.guid
  WHERE e.kind IN ('guild_tithe','guild_bank_deposit','guild_bank_withdraw','guild_repair')
    AND e.ts > UNIX_TIMESTAMP() - 86400
  GROUP BY gm.guildid
`

// The movements themselves, newest first: a vault nobody can watch working reads
// as decoration.
const VAULT_RECENT = `
  SELECT e.ts, e.kind, e.count, e.detail,
         c.name AS actor, g.name AS guild, it.name AS item, it.Quality AS quality
  FROM acore_characters.aetherion_econ_events e
  JOIN acore_characters.characters c ON c.guid = e.guid
  JOIN acore_characters.guild_member gm ON gm.guid = e.guid
  JOIN acore_characters.guild g ON g.guildid = gm.guildid
  LEFT JOIN acore_world.item_template it ON it.entry = e.item
  WHERE e.kind IN ('guild_tithe','guild_bank_deposit','guild_bank_withdraw','guild_repair')
  ORDER BY e.ts DESC
  LIMIT 14
`

const TAB_NAMES = ['Materials', 'Consumables', 'Gear']

const tidy = (s: string) =>
  String(s ?? '').replace(/\s*-\s*\d+\s*man.*$/i, '').split(',').pop()!.trim()

export default defineEventHandler(async () => {
  const [standings, pulse, progress, vault, vaultFlow, vaultRecent] = await Promise.all([
    q(STANDINGS), q(PULSE), q(PROGRESS), q(VAULT), q(VAULT_FLOW), q(VAULT_RECENT),
  ])

  const pulseById = new Map<number, any>()
  for (const r of pulse) pulseById.set(r.id, r)

  const bossesById = new Map<number, { boss: string; instance: string }[]>()
  for (const r of progress) {
    const list = bossesById.get(r.id) ?? []
    list.push({ boss: r.boss, instance: tidy(r.instance) })
    bossesById.set(r.id, list)
  }

  const vaultById = new Map<number, any>()
  for (const r of vault) vaultById.set(r.id, r)
  const flowById = new Map<number, any>()
  for (const r of vaultFlow) flowById.set(r.id, r)

  const guilds = standings.map(g => {
    const p = pulseById.get(g.id)
    return {
      id: g.id,
      name: g.name,
      master: g.master ?? null,
      masterClass: Number(g.masterClass ?? 0),
      members: Number(g.members),
      online: Number(g.online ?? 0),
      avgLevel: Number(g.avgLevel),
      topLevel: Number(g.topLevel),
      atCap: Number(g.atCap),
      gold: Number(g.gold ?? 0),
      kills: Number(g.kills ?? 0),
      grouped: Number(g.grouped ?? 0),
      achievements: Number(g.achievements ?? 0),
      faction: Number(g.alliance) > Number(g.members) / 2 ? 'alliance' : 'horde',
      activity: p
        ? {
            events: Number(p.events), active: Number(p.active),
            levels: Number(p.levels), loot: Number(p.loot),
            deaths: Number(p.deaths), instances: Number(p.instances),
          }
        : { events: 0, active: 0, levels: 0, loot: 0, deaths: 0, instances: 0 },
      bosses: bossesById.get(g.id) ?? [],
      vault: (() => {
        const v = vaultById.get(g.id)
        const f = flowById.get(g.id)
        return {
          gold: Math.round(Number(v?.bankCopper ?? 0) / 10000),
          items: Number(v?.items ?? 0),
          tabs: Number(v?.tabs ?? 0),
          tithes: Number(f?.tithes ?? 0),
          titheGold: Math.round(Number(f?.titheCopper ?? 0) / 10000),
          deposited: Number(f?.deposited ?? 0),
          withdrawn: Number(f?.withdrawn ?? 0),
          repairs: Number(f?.repairs ?? 0),
          repairGold: Math.round(Number(f?.repairCopper ?? 0) / 10000),
        }
      })(),
    }
  })

  return {
    at: Date.now(),
    total: guilds.length,
    // Realm-wide roll-ups, so the tab can lead with a sentence rather than a table.
    totals: {
      members: guilds.reduce((n, g) => n + g.members, 0),
      online: guilds.reduce((n, g) => n + g.online, 0),
      atCap: guilds.reduce((n, g) => n + g.atCap, 0),
      activeLastHour: guilds.filter(g => g.activity.events > 0).length,
      withBosses: guilds.filter(g => g.bosses.length > 0).length,
    },
    // The shared purse: what every vault holds, and a day of movement through it.
    vault: {
      gold: guilds.reduce((n, g) => n + g.vault.gold, 0),
      items: guilds.reduce((n, g) => n + g.vault.items, 0),
      stocked: guilds.filter(g => g.vault.items > 0).length,
      withTabs: guilds.filter(g => g.vault.tabs > 0).length,
      day: {
        tithes: guilds.reduce((n, g) => n + g.vault.tithes, 0),
        titheGold: guilds.reduce((n, g) => n + g.vault.titheGold, 0),
        deposited: guilds.reduce((n, g) => n + g.vault.deposited, 0),
        withdrawn: guilds.reduce((n, g) => n + g.vault.withdrawn, 0),
        repairs: guilds.reduce((n, g) => n + g.vault.repairs, 0),
        repairGold: guilds.reduce((n, g) => n + g.vault.repairGold, 0),
      },
      recent: vaultRecent.map(r => ({
        at: Number(r.ts),
        kind: String(r.kind).replace('guild_', ''),
        actor: r.actor,
        guild: r.guild,
        item: r.item ?? null,
        quality: Number(r.quality ?? 0),
        count: Number(r.count ?? 0),
        // Deposits stamp the destination tab in detail; repairs stamp the bill.
        tab: r.kind === 'guild_bank_deposit' || r.kind === 'guild_bank_withdraw'
          ? TAB_NAMES[Number(r.detail)] ?? null
          : null,
        gold: r.kind === 'guild_tithe'
          ? Math.round(Number(r.count ?? 0) / 10000)
          : r.kind === 'guild_repair'
            ? Math.round(Number(r.detail ?? 0) / 10000)
            : 0,
      })),
    },
    guilds,
  }
})
