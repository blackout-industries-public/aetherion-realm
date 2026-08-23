import { createHash } from 'node:crypto'

// Mirrors ai-bridge/src/bridge/identity.py exactly: sha256("aetherion:<guid>"), then a
// weighted pick from byte 3. Reproduced here rather than asked of the bridge because the
// dashboard needs the split across every bot on the realm, and the bridge only answers
// one character at a time. If the bridge's table changes, this must change with it.
const ARCHETYPES: [string, number][] = [
  ['normal', 44],
  ['pro', 12],
  ['clueless', 12],
  ['afk', 10],
  ['scumbag', 8],
  ['roleplayer', 7],
  ['goldseller', 7],
]

const TOTAL = ARCHETYPES.reduce((n, [, w]) => n + w, 0)

// Labels the dashboard shows. The bridge's internal names are blunter than what
// belongs on screen.
export const ARCHETYPE_LABEL: Record<string, string> = {
  normal: 'average',
  pro: 'competent',
  clueless: 'careless',
  afk: 'absent',
  scumbag: 'opportunistic',
  roleplayer: 'in character',
  goldseller: 'spammer',
}

export function archetypeFor(guid: number): string {
  const digest = createHash('sha256').update(`aetherion:${guid}`).digest()
  const roll = digest[3]! % TOTAL
  let upto = 0
  for (const [name, weight] of ARCHETYPES) {
    upto += weight
    if (roll < upto) return name
  }
  return ARCHETYPES[0]![0]
}

export function archetypeSplit(guids: number[]) {
  const counts = new Map<string, number>()
  for (const g of guids) {
    const a = archetypeFor(g)
    counts.set(a, (counts.get(a) ?? 0) + 1)
  }
  return ARCHETYPES
    .map(([name]) => ({
      key: name,
      label: ARCHETYPE_LABEL[name] ?? name,
      count: counts.get(name) ?? 0,
      pct: guids.length ? Math.round(((counts.get(name) ?? 0) / guids.length) * 100) : 0,
    }))
    .filter(a => a.count > 0)
}
