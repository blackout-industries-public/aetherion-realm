<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { MAPS, CLASS_COLOR, zoneName } from '../data'
import { ZONE_MAPS } from '../zonemaps'
import { T, FONT, STATE, type StateKey, fmt, spell, titled } from '../theme'

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
}>()

const emit = defineEmits<{ select: [string] }>()

// Sequential ramps, one hue each, monotone in lightness, low end measured at >=3:1
// against the plate. A single hue is the rule for magnitude - a rainbow ramp invents
// category boundaries that are not in the data.
const RAMPS: Record<string, string[]> = {
  density: ['#2d7baf', '#1e97d0', '#00b4ed', '#22d1ff', '#6debff'],
  pvp:     ['#b45341', '#dc5f52', '#ff6f6b', '#ff8c90', '#ffb3b7'],
  prof:    ['#2e885b', '#3ca368', '#52be76', '#78d78b', '#a3eda8'],
}

const LAYERS = [
  { key: 'activity', label: 'Activity',   hint: 'what each character is doing' },
  { key: 'density',  label: 'Hotspots',   hint: 'where the population actually is' },
  { key: 'pvp',      label: 'World PvP',  hint: "today's kills, by where the killer stands" },
  { key: 'prof',     label: 'Professions', hint: 'who carries a gathering or crafting skill' },
] as const
type LayerKey = (typeof LAYERS)[number]['key']

const layer = ref<LayerKey>('activity')
const profBit = ref(0)

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
  const worldX = b.top - (p.y / 667) * (b.top - b.bottom)

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
const artOpacity = ref(0.9)
const hasArt = ref(true)

watch(selectedMap, () => { hasArt.value = true; zoneId.value = null })

// The client's own projection: normalised X from world Y against LocLeft/LocRight,
// normalised Y from world X against LocTop/LocBottom. Using the real DBC bounds is
// what makes dots line up with the actual map art.
function project(e: { map: number; x: number; y: number }) {
  const b = activeBounds.value
  if (e.map !== b.map) return { cx: -50, cy: -50 }
  return {
    cx: ((b.left - e.y) / (b.left - b.right)) * 1000,
    cy: ((b.top - e.x) / (b.top - b.bottom)) * 667,
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

// What each character contributes to the active layer. Zero means it is not part of
// this view at all, which is different from "a cell with no one in it".
function weightOf(e: Entity): number {
  if (layer.value === 'pvp') return e.kills || 0
  if (layer.value === 'prof') return (e.prof & (1 << profBit.value)) ? 1 : 0
  return 1
}

// Binned rather than a blurred kernel: a grid cell is an honest count of characters in
// a known area, and it stays readable at 2500 points where overlapping dots do not.
const CELL = 40
const COLS = Math.ceil(1000 / CELL)
const ROWS = Math.ceil(667 / CELL)

const heat = computed(() => {
  if (layer.value === 'activity') return { cells: [] as any[], max: 0, total: 0 }
  const grid = new Map<number, number>()
  let total = 0
  for (const e of visible.value) {
    const w = weightOf(e)
    if (!w) continue
    const { cx, cy } = project(e)
    if (cx < 0 || cx > 1000 || cy < 0 || cy > 667) continue
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
    r: e.bot ? (st === 'idle' ? 3.4 : st === 'instance' ? 5 : 4.6) : 7,
    color: STATE[st].color,
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

const humans = computed(() => props.entities.filter(e => !e.bot).length)

const lede = computed(() => {
  const n = props.entities.length
  if (!n) return 'Nobody is online. The realm is up but empty.'
  const who = humans.value === 0
    ? 'None of them are people.'
    : `${titled(spell(humans.value))} of them ${humans.value === 1 ? 'is a person' : 'are people'}.`
  return `${titled(spell(n))} character${n === 1 ? '' : 's'} ${n === 1 ? 'is' : 'are'} awake in Azeroth right now. ${who}`
})

const refreshPct = computed(() =>
  Math.min(100, Math.round((props.refreshAgo / props.refreshEvery) * 100)))
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateRows: 'auto auto 1fr auto', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'flex', gap: '16px', flexWrap: 'wrap', alignItems: 'flex-end' }">
      <div :style="{ display: 'flex', gap: '1px', flexWrap: 'wrap' }">
        <button
          v-for="l in LAYERS"
          :key="l.key"
          :title="l.hint"
          :style="{
            appearance: 'none', cursor: 'pointer', padding: '8px 13px',
            background: layer === l.key ? T.raised : 'transparent',
            border: `1px solid ${layer === l.key ? T.goldDim : T.line}`,
            color: layer === l.key ? T.goldBright : T.muted,
            fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
            letterSpacing: '.14em', textTransform: 'uppercase',
          }"
          @click="layer = l.key"
        >{{ l.label }}</button>
      </div>

      <div
        v-if="layer === 'prof'"
        :style="{ display: 'flex', gap: '1px', flexWrap: 'wrap' }"
      >
        <button
          v-for="p in professions"
          :key="p.bit"
          :style="{
            appearance: 'none', cursor: 'pointer', padding: '6px 9px',
            background: profBit === p.bit ? 'oklch(0.26 0.03 155)' : 'transparent',
            border: `1px solid ${profBit === p.bit ? RAMPS.prof[2] : T.line}`,
            color: profBit === p.bit ? RAMPS.prof[4] : T.muted,
            fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.04em',
          }"
          @click="profBit = p.bit"
        >{{ p.name }} <span :style="{ color: T.faint }">{{ profCounts.get(p.bit) ?? 0 }}</span></button>
      </div>
    </div>

    <div v-if="zoneId !== null" :style="{ display: 'flex', gap: '10px', alignItems: 'center' }">
      <button
        :style="{
          appearance: 'none', cursor: 'pointer', padding: '7px 14px',
          background: 'transparent', border: `1px solid ${T.line}`, color: T.muted,
          fontFamily: FONT.display, fontWeight: 600, fontSize: '10.5px',
          letterSpacing: '.14em', textTransform: 'uppercase',
        }"
        @click="zoneId = null"
      >← {{ MAPS[selectedMap]?.name }}</button>
      <span :style="{ fontFamily: FONT.display, fontWeight: 700, fontSize: '14px', letterSpacing: '.12em', textTransform: 'uppercase', color: T.goldBright }">
        {{ zoneName(zoneId).toUpperCase() }}
      </span>
      <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">
        {{ visible.length }} here
      </span>
    </div>

    <div v-else :style="{ display: 'flex', gap: '1px', flexWrap: 'wrap' }">
      <button
        v-for="(m, id) in MAPS"
        :key="id"
        :style="{
          appearance: 'none', cursor: 'pointer', padding: '7px 14px',
          background: Number(id) === selectedMap ? T.raised : 'transparent',
          border: `1px solid ${Number(id) === selectedMap ? T.goldDim : T.line}`,
          color: Number(id) === selectedMap ? T.goldBright : T.muted,
          fontFamily: FONT.display, fontWeight: 600, fontSize: '10.5px',
          letterSpacing: '.14em', textTransform: 'uppercase', textAlign: 'left',
        }"
        @click="selectedMap = Number(id)"
      >
        <span style="display:block">{{ m.name }}</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint, letterSpacing: '.06em' }">
          {{ fmt.int(mapCounts[Number(id)] ?? 0) }}
        </span>
      </button>
    </div>

    <div
      :style="{
        position: 'relative', minHeight: 0, overflow: 'hidden',
        border: '1px solid oklch(0.42 0.05 78 / .55)',
        background: 'oklch(0.185 0.02 56)',
        boxShadow: 'inset 0 0 0 1px oklch(0.14 0.014 50), inset 0 0 90px oklch(0.09 0.01 45 / .9)',
      }"
    >
      <svg
        ref="svgEl"
        viewBox="0 0 1000 667"
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
          x="0" y="0" width="1000" height="667"
          preserveAspectRatio="none"
          :opacity="artOpacity"
          style="filter:saturate(0.2) brightness(0.4) contrast(1.12)"
          @error="hasArt = false"
        />

        <defs>
          <radialGradient id="vig" cx="50%" cy="50%" r="72%">
            <stop offset="55%" stop-color="oklch(0.1 0.012 45)" stop-opacity="0" />
            <stop offset="100%" stop-color="oklch(0.1 0.012 45)" stop-opacity="0.85" />
          </radialGradient>
        </defs>
        <rect x="0" y="0" width="1000" height="667" fill="url(#vig)" />

        <!-- Survey grid, as in the design: gives the plate structure without competing
             with the dots. -->
        <g :stroke="'oklch(0.32 0.02 52)'" stroke-width="0.6" opacity="0.5">
          <line v-for="gx in 15" :key="`v${gx}`" :x1="gx * 64" y1="0" :x2="gx * 64" y2="667" />
          <line v-for="gy in 10" :key="`h${gy}`" x1="0" :y1="gy * 64" x2="1000" :y2="gy * 64" />
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
            <title>{{ c.n }} {{ layer === 'pvp' ? 'kills' : 'characters' }}</title>
          </rect>
        </g>

        <path
          v-if="trailPath"
          :d="trailPath"
          fill="none"
          :stroke="T.gold"
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
          :stroke="d.e.bot ? 'oklch(0.11 0.01 50 / .95)' : T.textHi"
          :stroke-width="d.e.bot ? 1.4 : 1.8"
          :opacity="d.st === 'idle' ? 0.7 : 1"
          style="cursor:pointer"
          @mouseenter="hovered = d.e"
          @mouseleave="hovered = null"
          @click.stop="emit('select', d.e.name)"
        />
      </svg>

      <span
        v-for="c in [
          { top: '7px', left: '7px', bt: 1, bl: 1 },
          { top: '7px', right: '7px', bt: 1, br: 1 },
          { bottom: '7px', left: '7px', bb: 1, bl: 1 },
          { bottom: '7px', right: '7px', bb: 1, br: 1 },
        ]"
        :key="`${c.top ?? ''}${c.left ?? ''}${c.right ?? ''}`"
        :style="{
          position: 'absolute', width: '16px', height: '16px', pointerEvents: 'none',
          top: c.top, left: c.left, right: c.right, bottom: c.bottom,
          borderTop: c.bt ? '1px solid oklch(0.66 0.09 86 / .6)' : undefined,
          borderBottom: c.bb ? '1px solid oklch(0.66 0.09 86 / .6)' : undefined,
          borderLeft: c.bl ? '1px solid oklch(0.66 0.09 86 / .6)' : undefined,
          borderRight: c.br ? '1px solid oklch(0.66 0.09 86 / .6)' : undefined,
        }"
      />

      <div
        v-if="hovered"
        :style="{
          position: 'absolute', left: '12px', bottom: '12px', pointerEvents: 'none',
          border: `1px solid ${T.line}`, background: 'oklch(0.16 0.017 50 / .95)',
          padding: '8px 11px', maxWidth: '320px',
        }"
      >
        <div :style="{ fontSize: '15px', color: CLASS_COLOR[hovered.cls] ?? T.textHi, fontWeight: 600 }">
          {{ hovered.name }}
        </div>
        <div :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.muted, marginTop: '3px', letterSpacing: '.06em' }">
          level {{ hovered.level }} ·
          {{ (hovered.place ?? zoneName(hovered.zone)).toLowerCase() }} ·
          {{ hovered.trade ? hovered.trade.toLowerCase() : STATE[stateOf(hovered)].label }}
        </div>
      </div>
    </div>

    <div>
      <div :style="{ display: 'flex', alignItems: 'center', gap: '18px', flexWrap: 'wrap' }">
        <!-- A magnitude scale needs its ends labelled or the colour means nothing. -->
        <span v-if="layer !== 'activity'" :style="{ display: 'flex', alignItems: 'center', gap: '9px' }">
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">
            {{ layer === 'pvp' ? '0 kills' : '0' }}
          </span>
          <span :style="{ display: 'flex', gap: '2px' }">
            <span
              v-for="(c, i) in rampSteps"
              :key="i"
              :style="{ width: '26px', height: '9px', background: c, opacity: 0.2 + i * 0.14 }"
            />
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.text }">
            {{ heat.max }} per cell
          </span>
          <span :style="{ fontSize: '13px', color: T.muted }">
            {{ activeLayer.label }}<template v-if="layer === 'prof'"> · {{ profName }}</template>
            <template v-if="heat.total"> · {{ fmt.int(heat.total) }} total</template>
          </span>
        </span>

        <span v-if="layer === 'activity'" v-for="s in legend" :key="s.k" :style="{ display: 'flex', alignItems: 'center', gap: '7px' }">
          <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: s.color, flex: 'none' }" />
          <span :style="{ fontSize: '13px', color: T.body }">{{ s.label }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.faint }">{{ fmt.int(s.n) }}</span>
        </span>

        <span :style="{ marginLeft: 'auto', display: 'flex', alignItems: 'center', gap: '9px' }">
          <span :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.16em', color: T.dim }">
            POSITION REFRESH
          </span>
          <span :style="{ width: '120px', height: '3px', background: T.lineSoft, position: 'relative' }">
            <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${refreshPct}%`, background: T.gold }" />
          </span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted }">{{ Math.round(refreshAgo) }}s</span>
        </span>
      </div>

      <p :style="{ margin: '10px 0 0', fontSize: '12.5px', color: T.faint, lineHeight: 1.45, maxWidth: '96ch' }">
        Positions are written every {{ refreshEvery }}s. Travelling comes from the assembler's own
        trip records rather than being guessed from those writes. Characters inside an instance have
        no continent position, so they are drawn on their dungeon's door. Hover to trace a path;
        large ringed dots are human players. Click anywhere on a continent to dive into that
        zone; the layers and legend follow you down.
      </p>
    </div>
  </section>
</template>
