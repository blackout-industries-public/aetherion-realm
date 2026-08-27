import { q } from '../../utils/db'

// The live armory: what a character is actually wearing, what it is made of, and
// what it has done. Every field is read from the realm's own tables, so this is
// the character as the server sees it this second - not a snapshot written for
// the dashboard's benefit.

// Equipment slot order is the client's, and the names are the ones a player uses.
const SLOTS = [
  'Head', 'Neck', 'Shoulders', 'Shirt', 'Chest', 'Waist', 'Legs', 'Feet',
  'Wrists', 'Hands', 'Finger 1', 'Finger 2', 'Trinket 1', 'Trinket 2', 'Back',
  'Main hand', 'Off hand', 'Ranged', 'Tabard',
]

// The gear metric the dungeon finder itself uses skips these, so the armory
// reports the same average rather than a prettier one of its own.
const ILVL_SKIP = new Set([3, 18, 17, 16])

// skillline_dbc ships empty on this realm, so the names come from the same
// curated list the professions panel uses. Weapon and defence skills are left
// out on purpose: on a level-80 bot they are all maxed and say nothing.
const SKILL_NAMES: Record<number, string> = {
  182: 'Herbalism', 186: 'Mining', 393: 'Skinning',
  164: 'Blacksmithing', 165: 'Leatherworking', 171: 'Alchemy',
  197: 'Tailoring', 202: 'Engineering', 333: 'Enchanting',
  755: 'Jewelcrafting', 773: 'Inscription',
  129: 'First Aid', 185: 'Cooking', 356: 'Fishing', 762: 'Riding',
}

export default defineEventHandler(async event => {
  const name = getRouterParam(event, 'name') ?? ''
  if (!/^[A-Za-z]{2,12}$/.test(name))
    throw createError({ statusCode: 400, statusMessage: 'bad name' })

  const who = await q(
    `SELECT guid, race, class, level, gender, money, totalKills, online
     FROM acore_characters.characters WHERE name = ?`, [name])
  if (!who.length) return { found: false }
  const c = who[0]
  const guid = Number(c.guid)

  const [gear, stats, skills, achieved, tracked, bank, guild] = await Promise.all([
    q(`SELECT ci.slot, it.name, it.Quality AS quality, it.ItemLevel AS ilvl,
              it.class AS cls, it.subclass AS sub
       FROM acore_characters.character_inventory ci
       JOIN acore_characters.item_instance ii ON ii.guid = ci.item
       JOIN acore_world.item_template it ON it.entry = ii.itemEntry
       WHERE ci.guid = ? AND ci.bag = 0 AND ci.slot <= 18`, [guid]),
    q(`SELECT * FROM acore_characters.character_stats WHERE guid = ?`, [guid]),
    q(`SELECT skill, value, max FROM acore_characters.character_skills
       WHERE guid = ? ORDER BY value DESC`, [guid]),
    q(`SELECT achievement, date FROM acore_characters.character_achievement
       WHERE guid = ? ORDER BY date DESC`, [guid]),
    // Criteria counters exist in bulk, but achievement_criteria_dbc is empty on
    // this realm, so there is no way to say WHAT each counter counts. The total
    // is honest; a list of unlabelled numbers would not be.
    q(`SELECT COUNT(*) AS tracked, MAX(counter) AS best
       FROM acore_characters.character_achievement_progress
       WHERE guid = ? AND counter > 0`, [guid]),
    // Personal vault: the bank slots plus whatever sits inside bank bags.
    q(`SELECT it.name, it.Quality AS quality, ii.count
       FROM acore_characters.character_inventory ci
       JOIN acore_characters.item_instance ii ON ii.guid = ci.item
       JOIN acore_world.item_template it ON it.entry = ii.itemEntry
       WHERE ci.guid = ? AND ((ci.bag = 0 AND ci.slot BETWEEN 39 AND 66)
                              OR ci.bag IN (SELECT item FROM acore_characters.character_inventory
                                            WHERE guid = ? AND bag = 0 AND slot BETWEEN 67 AND 74))
       ORDER BY it.Quality DESC, it.name`, [guid, guid]),
    q(`SELECT g.name FROM acore_characters.guild g
       JOIN acore_characters.guild_member gm ON gm.guildid = g.guildid
       WHERE gm.guid = ?`, [guid]),
  ])

  const worn = gear.map(r => ({
    slot: SLOTS[Number(r.slot)] ?? `Slot ${r.slot}`,
    slotId: Number(r.slot),
    name: r.name,
    quality: Number(r.quality ?? 0),
    ilvl: Number(r.ilvl ?? 0),
  })).sort((a, b) => a.slotId - b.slotId)

  // Averaged over the same fifteen slots the dungeon finder counts, empties
  // included - an empty slot really does make a character worse geared.
  const counted = worn.filter(w => !ILVL_SKIP.has(w.slotId))
  const avgIlvl = Math.round(counted.reduce((n, w) => n + w.ilvl, 0) / 15)

  const s = stats[0] ?? {}
  return {
    found: true,
    name, guid,
    level: Number(c.level), race: Number(c.race), cls: Number(c.class),
    online: !!Number(c.online),
    gold: Math.round(Number(c.money ?? 0) / 10000),
    kills: Number(c.totalKills ?? 0),
    guild: guild[0]?.name ?? null,
    avgIlvl,
    empties: 15 - counted.filter(w => w.ilvl > 0).length,
    gear: worn,
    // character_stats is never written on this realm, so the sheet says so
    // rather than showing a block of zeroes that look like a broken character.
    stats: stats.length ? s : null,
    skills: skills.filter(r => SKILL_NAMES[Number(r.skill)])
      .map(r => ({ name: SKILL_NAMES[Number(r.skill)]!, value: Number(r.value), max: Number(r.max) })),
    achievements: {
      total: achieved.length,
      lastAt: achieved.length ? Number(achieved[0].date) * 1000 : null,
      tracked: Number(tracked[0]?.tracked ?? 0),
    },
    bank: {
      count: bank.length,
      items: bank.slice(0, 24).map(r => ({
        name: r.name, quality: Number(r.quality ?? 0), count: Number(r.count ?? 1),
      })),
    },
  }
})
