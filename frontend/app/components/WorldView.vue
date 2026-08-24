<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { MAPS, CLASS_COLOR, zoneName } from '../data'
import { ZONE_MAPS } from '../zonemaps'
import { FONT, STATE, V, type StateKey, fmt, spell, titled } from '../theme'
import UiFlow from './UiFlow.vue'

type Entity = {
  guid: number; name: string; level: number; map: number; zone: number
  x: number; y: number; cls: number; race: number; faction: string; bot: boolean
  dead: boolean; ghost: boolean; instance: number; grouped: boolean
  travelling: boolean; onTrip: boolean; atDoor: boolean
  instanceMap: number; place: string | null
  prof: number; kills: number
  rpg: number; trade: string | null
}

type Profession = { bit: number; skill: number; name: string }

const props = defineProps<{
  entities: Entity[]
  professions: Profession[]
  moving: Set<number>
  trails: Map<number, { map: number; x: number; y: number }[]>
  trailTick: number
  selectedGuid: number | null
  refreshAgo: number
  refreshEvery: number
  // Per-zone event heat, 24h: rows of {zone, what, kind, n} from the recorder.
  heat?: { zone: number; what: string; kind: string; n: number }[]
}>()

const emit = defineEmits<{ select: [string] }>()

// Sequential ramps, one hue each, monotone in lightness, low end measured at >=3:1
// against the plate. A single hue is the rule for magnitude - a rainbow ramp invents
// category boundaries that are not in the data. Data colors: never themed.
const RAMPS: Record<string, string[]> = {
  density: ['#2d7baf', '#1e97d0', '#00b4ed', '#22d1ff', '#6debff'],
  pvp:     ['#b45341', '#dc5f52', '#ff6f6b', '#ff8c90', '#ffb3b7'],
  prof:    ['#2e885b', '#3ca368', '#52be76', '#78d78b', '#a3eda8'],
}

const LAYERS = [
  { key: 'activity', label: 'ACTIVITY',    name: 'Activity',    hint: 'what they do',        note: 'what each character is doing' },
  { key: 'density',  label: 'HOTSPOTS',    name: 'Hotspots',    hint: 'where they are',      note: 'where the population actually is' },
  { key: 'pvp',      label: 'WORLD PVP',   name: 'World PvP',   hint: 'kills scored here',   note: 'where honourable kills happened, last 24h' },
  { key: 'prof',     label: 'PROFESSIONS', name: 'Professions', hint: 'trades worked here',  note: 'where herbs, veins and skins were taken, last 24h' },
] as const
type LayerKey = (typeof LAYERS)[number]['key']

const layer = ref<LayerKey>('activity')
const profBit = ref(0)
const spotlight = ref<StateKey | null>(null)

const selectedMap = ref(571)

// Deep world: a zone id from ZONE_MAPS, or null for the continent view. Presence in
// that generated table is the capability check - no art, no dive.
const zoneId = ref<number | null>(null)
const svgEl = ref<SVGSVGElement | null>(null)

const activeBounds = computed(() => {
  if (zoneId.value !== null) {
    const z = ZONE_MAPS[zoneId.value]
    if (z) return { map: z.map, left: z.left, right: z.right, top: z.top, bottom: z.bottom }
  }
  const m = MAPS[selectedMap.value]!
  return { map: selectedMap.value, left: m.left, right: m.right, top: m.top, bottom: m.bottom }
})

const artHref = computed(() =>
  zoneId.value !== null ? `/maps/zone-${zoneId.value}.jpg` : `/maps/${selectedMap.value}.jpg`)

// World coords -> zone rect test. LocLeft/LocRight are world Y (left is the larger
// value), LocTop/LocBottom are world X - same convention the projection uses.
function inRect(x: number, y: number, b: { left: number; right: number; top: number; bottom: number }) {
  return y <= b.left && y >= b.right && x <= b.top && x >= b.bottom
}

// Click on the plate dives into the zone under the cursor. Inverse-project the SVG
// point to world coords, then pick the smallest containing zone (rects overlap a
// little at borders; the smaller one is the one the cursor is visually inside).
function diveAt(evt: MouseEvent) {
  if (zoneId.value !== null || !svgEl.value) return
  const pt = svgEl.value.createSVGPoint()
  pt.x = evt.clientX; pt.y = evt.clientY
  const ctm = svgEl.value.getScreenCTM()
  if (!ctm) return
  const p = pt.matrixTransform(ctm.inverse())
  const b = activeBounds.value
  const worldY = b.left - (p.x / 1000) * (b.left - b.right)
  const worldX = b.top - (p.y / 560) * (b.top - b.bottom)

  let best: number | null = null
  let bestArea = Infinity
  for (const [id, z] of Object.entries(ZONE_MAPS)) {
    if (z.map !== selectedMap.value || !inRect(worldX, worldY, z)) continue
    const area = Math.abs((z.left - z.right) * (z.top - z.bottom))
    if (area < bestArea) { bestArea = area; best = Number(id) }
  }
  if (best !== null) { zoneId.value = best; hasArt.value = true }
}
const hovered = ref<Entity | null>(null)
const hasArt = ref(true)

watch(selectedMap, () => { hasArt.value = true; zoneId.value = null; spotlight.value = null; hovered.value = null })
watch(zoneId, () => { spotlight.value = null })

// The client's own projection: normalised X from world Y against LocLeft/LocRight,
// normalised Y from world X against LocTop/LocBottom. Using the real DBC bounds is
// what makes dots line up with the actual map art.
function project(e: { map: number; x: number; y: number }) {
  const b = activeBounds.value
  if (e.map !== b.map) return { cx: -50, cy: -50 }
  return {
    cx: ((b.left - e.y) / (b.left - b.right)) * 1000,
    cy: ((b.top - e.x) / (b.top - b.bottom)) * 560,
  }
}

// Precedence: fatal states, then a fresh trade skill-up (honest evidence of working a
// profession right now), then what the RPG engine says the bot chose to do, then the
// assembler's journey flag, then the coarse fallbacks.
function stateOf(e: Entity): StateKey {
  if (e.dead) return 'dead'
  if (e.ghost) return 'ghost'
  if (e.instance > 0) return 'instance'
  if (e.trade) return 'working'
  switch (e.rpg) {
    case 5: return 'questing'
    case 1: case 3: return 'grinding'
    case 2: case 6: return 'travelling'
    case 4: case 7: return 'town'
  }
  if (e.travelling || props.moving.has(e.guid)) return 'travelling'
  if (e.grouped) return 'grouped'
  return 'idle'
}

const visible = computed(() => {
  const base = props.entities.filter(e => e.map === selectedMap.value)
  if (zoneId.value === null) return base
  // Position, not the zone column: bots plotted at a dungeon door were relocated to
  // door coordinates, and the door belongs on the zone map it stands in.
  const z = ZONE_MAPS[zoneId.value]
  return z ? base.filter(e => inRect(e.x, e.y, z)) : base
})

// Population density is the only layer still weighed by who stands where;
// the event layers below draw where things HAPPENED instead - a killer who
// hearthed to Dalaran no longer paints Dalaran red.
function weightOf(_e: Entity): number {
  return 1
}

// Zone bubbles for the event layers: the recorder stamps every event with the
// zone it happened in; the bubble anchors at the centroid of that zone's
// present characters. A zone with heat but nobody currently home has no
// anchor and is listed in the note instead of guessed onto the map.
const zoneBubbles = computed(() => {
  if (layer.value !== 'pvp' && layer.value !== 'prof') return { bubbles: [] as any[], total: 0, unanchored: 0 }
  const wantKind = layer.value === 'pvp' ? 'pvp' : 'profession'
  const profWord = layer.value === 'prof' && profName.value ? profName.value.split(' ')[0] : null
  const byZone = new Map<number, number>()
  let total = 0
  for (const row of props.heat ?? []) {
    if (row.kind !== wantKind) continue
    if (profWord && row.what !== profWord) continue
    byZone.set(row.zone, (byZone.get(row.zone) ?? 0) + Number(row.n))
    total += Number(row.n)
  }
  const anchors = new Map<number, { x: number; y: number; n: number }>()
  for (const e of visible.value) {
    if (!byZone.has(e.zone)) continue
    const { cx, cy } = project(e)
    if (cx < 0 || cx > 1000 || cy < 0 || cy > 560) continue
    const a = anchors.get(e.zone) ?? { x: 0, y: 0, n: 0 }
    a.x += cx; a.y += cy; a.n++
    anchors.set(e.zone, a)
  }
  const ramp = RAMPS[layer.value]!
  const max = Math.max(1, ...byZone.values())
  const bubbles: any[] = []
  let unanchored = 0
  for (const [zone, n] of byZone) {
    const a = anchors.get(zone)
    if (!a || !a.n) { unanchored += n; continue }
    const t = n / max
    bubbles.push({
      zone, n,
      x: a.x / a.n, y: a.y / a.n,
      r: 9 + Math.sqrt(n / max) * 26,
      fill: ramp[t > 0.62 ? 4 : t > 0.38 ? 3 : t > 0.2 ? 2 : t > 0.08 ? 1 : 0]!,
      name: zoneName(zone),
    })
  }
  return { bubbles, total, unanchored }
})

// Binned rather than a blurred kernel: a grid cell is an honest count of characters in
// a known area, and it stays readable at 2500 points where overlapping dots do not.
const CELL = 40
const COLS = Math.ceil(1000 / CELL)

const heat = computed(() => {
  if (layer.value !== 'density') return { cells: [] as any[], max: 0, total: 0 }
  const grid = new Map<number, number>()
  let total = 0
  for (const e of visible.value) {
    const w = weightOf(e)
    if (!w) continue
    const { cx, cy } = project(e)
    if (cx < 0 || cx > 1000 || cy < 0 || cy > 560) continue
    const key = Math.floor(cy / CELL) * COLS + Math.floor(cx / CELL)
    grid.set(key, (grid.get(key) ?? 0) + w)
    total += w
  }
  const max = Math.max(1, ...grid.values())
  const ramp = RAMPS[layer.value]!
  const cells = [...grid.entries()].map(([key, n]) => {
    // Relative to the busiest cell, so the scale always spans the data in view.
    const t = n / max
    const step = t > 0.62 ? 4 : t > 0.38 ? 3 : t > 0.20 ? 2 : t > 0.08 ? 1 : 0
    return {
      key, n,
      x: (key % COLS) * CELL, y: Math.floor(key / COLS) * CELL,
      fill: ramp[step]!,
      // Low cells stay faint so the busy ones carry the eye.
      opacity: 0.2 + step * 0.14,
    }
  })
  return { cells, max, total }
})

const rampSteps = computed(() => RAMPS[layer.value] ?? [])
const activeLayer = computed(() => LAYERS.find(l => l.key === layer.value)!)
const profName = computed(() => props.professions?.[profBit.value]?.name ?? '')

const heatNote = computed(() => {
  const base = layer.value === 'prof' && profName.value
    ? `Professions · ${profName.value} worked here, last 24h`
    : `${activeLayer.value.name} · ${activeLayer.value.note}`
  if (layer.value === 'pvp' || layer.value === 'prof') {
    const zb = zoneBubbles.value
    const bits = [`${fmt.int(zb.total)} events`]
    if (zb.unanchored)
      bits.push(`${fmt.int(zb.unanchored)} in zones nobody is home to plot`)
    return `${base} · ${bits.join(' · ')}`
  }
  return heat.value.total ? `${base} · ${fmt.int(heat.value.total)} total` : base
})

// Counts per profession, so the selector says how many it would show.
const profCounts = computed(() => {
  const out = new Map<number, number>()
  for (const p of props.professions ?? []) {
    let n = 0
    for (const e of visible.value) if (e.prof & (1 << p.bit)) n++
    out.set(p.bit, n)
  }
  return out
})

const mapCounts = computed(() => {
  const counts: Record<number, number> = {}
  for (const e of props.entities) counts[e.map] = (counts[e.map] ?? 0) + 1
  return counts
})

const dots = computed(() => visible.value.map(e => {
  const st = stateOf(e)
  const { cx, cy } = project(e)
  return {
    e, st, cx, cy,
    r: e.bot ? (st === 'idle' ? 3 : 4.1) : 6.5,
    color: STATE[st].color,
    // Spotlight is an activity-layer idea; on heat layers the few dots left are
    // the ones worth keeping bright.
    opacity: layer.value === 'activity' && spotlight.value && st !== spotlight.value
      ? 0.1 : st === 'idle' ? 0.65 : 1,
  }
}))

const legend = computed(() => {
  const counts = new Map<StateKey, number>()
  for (const e of visible.value) {
    const st = stateOf(e)
    counts.set(st, (counts.get(st) ?? 0) + 1)
  }
  return (Object.keys(STATE) as StateKey[])
    .map(k => ({ k, label: STATE[k].label, color: STATE[k].color, n: counts.get(k) ?? 0 }))
    .filter(s => s.n > 0)
})

// Top zones labelled at the mean of their people's projected positions: the label
// sits where the crowd is without a hand-placed coordinate table per continent.
const zoneLabels = computed(() => {
  if (zoneId.value !== null) return []
  const agg = new Map<number, { n: number; sx: number; sy: number }>()
  for (const e of visible.value) {
    const { cx, cy } = project(e)
    if (cx < 0 || cx > 1000 || cy < 0 || cy > 560) continue
    const a = agg.get(e.zone) ?? { n: 0, sx: 0, sy: 0 }
    a.n++; a.sx += cx; a.sy += cy
    agg.set(e.zone, a)
  }
  return [...agg.entries()]
    .sort((a, b) => b[1].n - a[1].n).slice(0, 6)
    .map(([id, a]) => ({
      id,
      x: Math.min(910, Math.max(90, a.sx / a.n)),
      y: Math.min(548, Math.max(16, a.sy / a.n - 12)),
      t: zoneName(id).toUpperCase(),
    }))
})

const hotZones = computed(() => {
  const counts = new Map<number, number>()
  for (const e of visible.value) counts.set(e.zone, (counts.get(e.zone) ?? 0) + 1)
  const top = [...counts.entries()].sort((a, b) => b[1] - a[1]).slice(0, 4)
  const max = Math.max(1, ...top.map(t => t[1]))
  return top.map(([id, n]) => ({ id, name: zoneName(id), n, w: `${Math.round((n / max) * 100)}%` }))
})

// Trails are accumulated client-side while the page is open, because nothing records
// where a character has been. Only the focused character is drawn: four hundred paths
// at once is noise, not information.
const trailPath = computed(() => {
  void props.trailTick
  const guid = hovered.value?.guid ?? props.selectedGuid
  if (!guid) return null
  const zone = zoneId.value !== null ? ZONE_MAPS[zoneId.value] : null
  const pts = (props.trails.get(guid) ?? []).filter(p =>
    p.map === selectedMap.value && (!zone || inRect(p.x, p.y, zone)))
  if (pts.length < 2) return null
  return pts.map((p, i) => {
    const { cx, cy } = project(p)
    return `${i ? 'L' : 'M'}${cx.toFixed(1)} ${cy.toFixed(1)}`
  }).join(' ')
})

// The state series is sampled here, not served: nothing server-side records what a
// continent was doing an hour ago, so the strip honestly begins when the page opens.
const flowStart = ref(Date.now())
const flowSamples = ref<Record<number, [number, number, number]>[]>([])
let lastFlowAt = 0
watch(() => props.trailTick, () => {
  if (!props.entities.length) return
  const now = Date.now()
  if (flowSamples.value.length && now - lastFlowAt < 25_000) return
  lastFlowAt = now
  if (!flowSamples.value.length) flowStart.value = now
  const by: Record<number, [number, number, number]> = {}
  for (const e of props.entities) {
    const st = stateOf(e)
    const i = st === 'questing' ? 0 : st === 'travelling' ? 1 : st === 'grinding' ? 2 : -1
    if (i < 0) continue
    ;(by[e.map] ??= [0, 0, 0])[i]++
  }
  flowSamples.value = [...flowSamples.value.slice(-359), by]
}, { immediate: true })

const flowSeries = computed(() => {
  const m = selectedMap.value
  const pick = (i: number) => flowSamples.value.map(s => s[m]?.[i] ?? 0)
  return [
    { label: STATE.questing.label, color: STATE.questing.color, values: pick(0) },
    { label: STATE.travelling.label, color: STATE.travelling.color, values: pick(1) },
    { label: STATE.grinding.label, color: STATE.grinding.color, values: pick(2) },
  ]
})
const flowRange = computed<[string, string]>(() => [fmt.clock(flowStart.value), 'now'])

const lede = computed(() => {
  const n = visible.value.length
  const where = zoneId.value !== null ? zoneName(zoneId.value) : MAPS[selectedMap.value]!.name
  if (!n) return `Nobody walks ${where} right now.`
  const verb = selectedMap.value === 530 ? (n === 1 ? 'braves' : 'brave') : n === 1 ? 'walks' : 'walk'
  return `${titled(spell(n))} character${n === 1 ? '' : 's'} ${verb} ${where} right now.`
})

const refreshPct = computed(() =>
  Math.min(100, Math.round((props.refreshAgo / props.refreshEvery) * 100)))

const capStyle = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim,
} as const
const consolePanel = {
  border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset, flex: 'none',
} as const
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateColumns: '1fr 300px', minHeight: 0, minWidth: 0, height: '100%', overflow: 'hidden' }">
    <div :style="{ padding: '16px 0 14px 22px', display: 'flex', flexDirection: 'column', gap: '12px', minWidth: 0, minHeight: 0, overflow: 'hidden' }">
      <div :style="{ display: 'flex', alignItems: 'baseline', gap: '18px', flexWrap: 'wrap' }">
        <p
          :style="{
            margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
            fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
            lineHeight: 1.4, color: V.textMid,
          }"
        >{{ lede }}</p>

        <span v-if="zoneId === null" :style="{ display: 'flex', gap: '1px', marginLeft: 'auto', flexWrap: 'wrap' }">
          <button
            v-for="(m, id) in MAPS"
            :key="id"
            :style="{
              appearance: 'none', cursor: 'pointer', padding: '7px 12px',
              fontFamily: FONT.display, fontWeight: 600, fontSize: '10px', letterSpacing: '.12em',
              textTransform: 'uppercase',
              border: `1px solid ${Number(id) === selectedMap ? V.accentDim : V.line}`,
              background: Number(id) === selectedMap ? V.raised : 'transparent',
              color: Number(id) === selectedMap ? V.accentBright : V.muted,
            }"
            @click="selectedMap = Number(id)"
          >{{ m.name }} <span :style="{ fontFamily: FONT.mono, color: V.faint }">{{ fmt.int(mapCounts[Number(id)] ?? 0) }}</span></button>
        </span>

        <span v-else :style="{ display: 'flex', gap: '10px', marginLeft: 'auto', alignItems: 'baseline' }">
          <button
            :style="{
              appearance: 'none', cursor: 'pointer', padding: '7px 12px',
              background: 'transparent', border: `1px solid ${V.line}`, color: V.muted,
              fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
              letterSpacing: '.12em', textTransform: 'uppercase',
            }"
            @click="zoneId = null"
          >← {{ MAPS[selectedMap]?.name }}</button>
          <span :style="{ fontFamily: FONT.display, fontWeight: 700, fontSize: '13px', letterSpacing: '.12em', textTransform: 'uppercase', color: V.accentBright }">
            {{ zoneName(zoneId).toUpperCase() }}
          </span>
        </span>
      </div>

      <div
        :style="{
          position: 'relative', flex: 1, minHeight: 0, overflow: 'hidden',
          border: `1px solid ${V.lineAccentSoft}`,
          background: `radial-gradient(ellipse at 45% 40%, ${V.mapHi}, ${V.mapLo} 72%)`,
          boxShadow: 'inset 0 0 0 1px oklch(0.14 0.014 50), inset 0 0 110px oklch(0.07 0.008 45 / .9)',
        }"
      >
        <svg
          ref="svgEl"
          viewBox="0 0 1000 560"
          preserveAspectRatio="xMidYMid meet"
          :style="{ display: 'block', width: '100%', height: '100%', cursor: zoneId === null ? 'zoom-in' : 'default' }"
          @click="diveAt"
        >
          <!-- Desaturated and dimmed rather than faded: an opacity wash over brown art
               turns the whole plate to mud, while draining the colour leaves a neutral
               substrate that coloured dots can sit on top of. -->
          <image
            v-if="hasArt"
            :href="artHref"
            x="0" y="0" width="1000" height="560"
            preserveAspectRatio="none"
            opacity="0.9"
            style="filter:saturate(0.2) brightness(0.4) contrast(1.12)"
            @error="hasArt = false"
          />

          <defs>
            <radialGradient id="vig" cx="50%" cy="50%" r="72%">
              <stop offset="55%" stop-color="oklch(0.1 0.012 45)" stop-opacity="0" />
              <stop offset="100%" stop-color="oklch(0.1 0.012 45)" stop-opacity="0.85" />
            </radialGradient>
          </defs>
          <rect x="0" y="0" width="1000" height="560" fill="url(#vig)" />

          <!-- Survey grid, as in the design: gives the plate structure without competing
               with the dots. -->
          <g stroke="oklch(0.30 0.02 52)" stroke-width="0.6" opacity="0.45">
            <line v-for="gx in 15" :key="`v${gx}`" :x1="gx * 64" y1="0" :x2="gx * 64" y2="560" />
            <line v-for="gy in 8" :key="`h${gy}`" x1="0" :y1="gy * 64" x2="1000" :y2="gy * 64" />
          </g>

          <!-- Binned counts. Drawn under the dots so a selected character stays findable
               even with a layer on. -->
          <g v-if="heat.cells.length" shape-rendering="crispEdges">
            <rect
              v-for="c in heat.cells"
              :key="c.key"
              :x="c.x" :y="c.y" :width="CELL" :height="CELL"
              :fill="c.fill" :opacity="c.opacity"
            >
              <title>{{ c.n }} characters</title>
            </rect>
          </g>

          <!-- Event heat: one bubble per zone where things actually happened,
               sized by count, anchored at the zone's present population. -->
          <g v-if="zoneBubbles.bubbles.length">
            <circle
              v-for="b in zoneBubbles.bubbles"
              :key="b.zone"
              :cx="b.x" :cy="b.y" :r="b.r"
              :fill="b.fill" opacity="0.42"
              :stroke="b.fill" stroke-opacity="0.8" stroke-width="1"
            >
              <title>{{ b.name }} · {{ b.n }} {{ layer === 'pvp' ? 'kills scored' : 'harvests' }} in 24h</title>
            </circle>
          </g>

          <text
            v-for="z in zoneLabels"
            :key="z.id"
            :x="z.x" :y="z.y"
            text-anchor="middle"
            fill="oklch(0.55 0.035 72)"
            opacity="0.85"
            :style="{ fontFamily: FONT.mono, fontSize: '9px', letterSpacing: '.2em', pointerEvents: 'none' }"
          >{{ z.t }}</text>

          <path
            v-if="trailPath"
            :d="trailPath"
            fill="none"
            :stroke="V.accent"
            stroke-width="1.6"
            stroke-opacity="0.85"
            stroke-linejoin="round"
            stroke-linecap="round"
          />

          <rect
            v-for="d in (layer === 'activity' ? dots : []).filter(x => x.st === 'working')"
            :key="`w${d.e.guid}`"
            :x="d.cx - d.r" :y="d.cy - d.r" :width="d.r * 2" :height="d.r * 2"
            :transform="`rotate(45 ${d.cx} ${d.cy})`"
            :fill="d.color"
            :opacity="d.opacity"
            stroke="oklch(0.11 0.01 50 / .95)" stroke-width="1.2"
            style="cursor:pointer"
            @mouseenter="hovered = d.e"
            @mouseleave="hovered = null"
            @click.stop="emit('select', d.e.name)"
          />
          <circle
            v-for="d in (layer === 'activity' ? dots.filter(x => x.st !== 'working') : dots.filter(x => !x.e.bot || x.e.guid === selectedGuid))"
            :key="d.e.guid"
            :cx="d.cx" :cy="d.cy" :r="d.r"
            :fill="d.color"
            :stroke="d.e.bot ? 'oklch(0.11 0.01 50 / .95)' : V.textHi"
            :stroke-width="d.e.bot ? 1.2 : 1.8"
            :opacity="d.opacity"
            style="cursor:pointer"
            @mouseenter="hovered = d.e"
            @mouseleave="hovered = null"
            @click.stop="emit('select', d.e.name)"
          />
        </svg>

        <span :style="{ position: 'absolute', top: '8px', left: '8px', width: '16px', height: '16px', borderTop: `1px solid ${V.tick}`, borderLeft: `1px solid ${V.tick}`, pointerEvents: 'none' }" />
        <span :style="{ position: 'absolute', top: '8px', right: '8px', width: '16px', height: '16px', borderTop: `1px solid ${V.tick}`, borderRight: `1px solid ${V.tick}`, pointerEvents: 'none' }" />
        <span :style="{ position: 'absolute', bottom: '8px', left: '8px', width: '16px', height: '16px', borderBottom: `1px solid ${V.tick}`, borderLeft: `1px solid ${V.tick}`, pointerEvents: 'none' }" />
        <span :style="{ position: 'absolute', bottom: '8px', right: '8px', width: '16px', height: '16px', borderBottom: `1px solid ${V.tick}`, borderRight: `1px solid ${V.tick}`, pointerEvents: 'none' }" />

        <div
          v-if="hovered"
          :style="{
            position: 'absolute', left: '12px', bottom: '12px', pointerEvents: 'none',
            border: `1px solid ${V.line}`, background: V.overlay,
            padding: '8px 11px', maxWidth: '320px',
          }"
        >
          <div :style="{ fontSize: '15px', color: CLASS_COLOR[hovered.cls] ?? V.textHi, fontWeight: 600 }">
            {{ hovered.name }}
          </div>
          <div :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.muted, marginTop: '3px', letterSpacing: '.06em' }">
            level {{ hovered.level }} ·
            {{ (hovered.place ?? zoneName(hovered.zone)).toLowerCase() }} ·
            {{ hovered.trade ? hovered.trade.toLowerCase() : STATE[stateOf(hovered)].label }}
          </div>
        </div>

        <!-- A magnitude scale needs its ends labelled or the colour means nothing. -->
        <div
          v-else-if="layer !== 'activity'"
          :style="{
            position: 'absolute', left: '12px', bottom: '12px', pointerEvents: 'none',
            display: 'flex', alignItems: 'center', gap: '12px',
            border: `1px solid ${V.line}`, background: V.overlay, padding: '7px 12px',
          }"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">0</span>
          <span :style="{ display: 'flex', gap: '2px' }">
            <span
              v-for="(c, i) in rampSteps"
              :key="i"
              :style="{ width: '24px', height: '8px', background: c, opacity: 0.2 + i * 0.14 }"
            />
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.text }">{{ heat.max }} per cell</span>
          <span :style="{ fontSize: '12px', color: V.muted }">{{ heatNote }}</span>
        </div>
      </div>

      <div :style="{ flex: 'none' }">
        <UiFlow
          :series="flowSeries"
          cap="WHAT THE CONTINENT DID THIS SESSION"
          :range="flowRange"
          :height="62"
        />
        <p v-if="flowSamples.length < 2" :style="{ margin: '4px 0 0', fontSize: '11px', color: V.faint, lineHeight: 1.45 }">
          Nothing server-side records what a continent was doing an hour ago, so this trace
          accumulates while the page is open. Give it a minute or two.
        </p>
      </div>

      <p :style="{ margin: 0, fontSize: '12px', color: V.faint, lineHeight: 1.45, maxWidth: '100ch' }">
        Positions write every {{ refreshEvery }}s. Hover a dot for who it is; click to read them.
        Large ringed dots are human players. Click open ground to dive into that zone;
        characters inside an instance are drawn on their dungeon's door.
      </p>
    </div>

    <aside :style="{ padding: '16px 22px 16px 18px', display: 'flex', flexDirection: 'column', gap: '12px', overflow: 'auto', minHeight: 0 }">
      <section :style="consolePanel">
        <header :style="{ padding: '10px 12px 6px' }"><span :style="capStyle">LAYER</span></header>
        <div :style="{ padding: '0 12px 12px', display: 'flex', flexDirection: 'column', gap: '1px' }">
          <button
            v-for="l in LAYERS"
            :key="l.key"
            :title="l.note"
            :style="{
              appearance: 'none', display: 'flex', justifyContent: 'space-between',
              alignItems: 'center', gap: '8px', padding: '7px 10px', cursor: 'pointer',
              border: `1px solid ${layer === l.key ? V.accentDim : V.line}`,
              background: layer === l.key ? V.raised : 'transparent',
              fontFamily: FONT.display,
            }"
            @click="layer = l.key"
          >
            <span :style="{ fontSize: '10px', fontWeight: 600, letterSpacing: '.14em', color: layer === l.key ? V.accentBright : V.muted }">{{ l.label }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint2, textAlign: 'right' }">{{ l.hint }}</span>
          </button>

          <div v-if="layer === 'prof'" :style="{ display: 'flex', gap: '1px', flexWrap: 'wrap', marginTop: '7px' }">
            <button
              v-for="p in professions"
              :key="p.bit"
              :style="{
                appearance: 'none', cursor: 'pointer', padding: '5px 8px',
                background: profBit === p.bit ? 'oklch(0.26 0.03 155)' : 'transparent',
                border: `1px solid ${profBit === p.bit ? RAMPS.prof[2] : V.line}`,
                color: profBit === p.bit ? RAMPS.prof[4] : V.muted,
                fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.04em',
              }"
              @click="profBit = p.bit"
            >{{ p.name }} <span :style="{ color: V.faint }">{{ profCounts.get(p.bit) ?? 0 }}</span></button>
          </div>
        </div>
      </section>

      <section :style="consolePanel">
        <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 12px 6px' }">
          <span :style="capStyle">{{ zoneId !== null ? 'IN THIS ZONE' : 'ON THIS CONTINENT' }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ fmt.int(visible.length) }}</span>
        </header>
        <div :style="{ padding: '0 12px 12px' }">
          <button
            v-for="s in legend"
            :key="s.k"
            :style="{
              appearance: 'none', border: 'none', width: '100%', cursor: 'pointer',
              display: 'flex', justifyContent: 'space-between', alignItems: 'center',
              gap: '8px', minWidth: 0, padding: '3px 4px', fontFamily: FONT.body,
              background: spotlight === s.k ? V.raised : 'transparent',
            }"
            @click="spotlight = spotlight === s.k ? null : s.k"
          >
            <span
              :style="{
                display: 'flex', alignItems: 'center', gap: '7px', fontSize: '13px',
                color: spotlight && spotlight !== s.k ? V.faint : V.body,
                whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis', minWidth: 0,
              }"
            >
              <span
                :style="{
                  width: '7px', height: '7px', flex: 'none', background: s.color,
                  borderRadius: s.k === 'working' ? '0' : '50%',
                  transform: s.k === 'working' ? 'rotate(45deg)' : 'none',
                }"
              />{{ s.label }}
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.text }">{{ fmt.int(s.n) }}</span>
          </button>
          <p :style="{ margin: '8px 0 0', fontSize: '11px', color: V.faint, lineHeight: 1.45 }">
            {{ spotlight ? 'Spotlighting one state — click it again to clear.' : 'Click a state to spotlight those dots.' }}
          </p>
        </div>
      </section>

      <section :style="consolePanel">
        <header :style="{ padding: '10px 12px 6px' }"><span :style="capStyle">HOTTEST ZONES</span></header>
        <div :style="{ padding: '0 12px 12px' }">
          <div
            v-for="z in hotZones"
            :key="z.id"
            :style="{ display: 'grid', gridTemplateColumns: '1fr 64px 32px', gap: '8px', alignItems: 'center', padding: '2.5px 0' }"
          >
            <span :style="{ fontSize: '13px', color: V.body, whiteSpace: 'nowrap', overflow: 'hidden', textOverflow: 'ellipsis' }">{{ z.name }}</span>
            <span :style="{ height: '5px', background: V.track, position: 'relative' }">
              <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: z.w, background: V.accentBar }" />
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.text, textAlign: 'right' }">{{ z.n }}</span>
          </div>
        </div>
      </section>

      <div :style="{ marginTop: 'auto', display: 'flex', alignItems: 'center', gap: '9px', flex: 'none' }">
        <span :style="{ ...capStyle, fontSize: '9px' }">POSITION REFRESH</span>
        <span :style="{ flex: 1, height: '3px', background: V.lineSoft, position: 'relative' }">
          <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${refreshPct}%`, background: V.accent }" />
        </span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.muted }">{{ Math.round(refreshAgo) }}s</span>
      </div>
    </aside>
  </section>
</template>
