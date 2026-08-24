<script setup lang="ts">
import { computed, ref, watch } from 'vue'
import { T, V, FONT, STATE, fmt } from '../theme'
import { CLASS_COLOR, zoneName } from '../data'

type FeedEvent = {
  id: number; guid: number; who: string; cls: number; level: number
  faction: string; kind: string; tag: string; movedToZone: number | null
  detail: string; zone: number; at: number; place: string | null
  movedToName: string | null
}

const props = defineProps<{
  events: FeedEvent[]
  ready: boolean
  selected: string | null
  detail: any | null
  activity: string | null
  loading: boolean
}>()

const emit = defineEmits<{ select: [string]; clear: []; map: [] }>()

// The recorder stores "moved to zone 4395". Naming the zone is the client's job -
// it already holds the table, and a raw id in a feed is unreadable.
const unnamed = (s: string) => /^Zone \d+$/.test(s)

function humanize(text: string) {
  return String(text ?? '').replace(/moved to zone (\d+)/, (whole, id) => {
    const name = zoneName(Number(id))
    return unnamed(name) ? whole : `moved to ${name}`
  })
}

function sentence(e: FeedEvent) {
  if (e.movedToZone === null) return e.detail
  const zone = zoneName(e.movedToZone)
  if (!unnamed(zone)) return `arrived in ${zone}`
  // Instance zone ids are absent from the client table this realm does not load; the
  // server derives them from where characters actually stand.
  return `arrived in ${e.movedToName ?? e.place ?? zone}`
}

function where(e: FeedEvent) {
  const zone = zoneName(e.zone)
  const place = unnamed(zone) && e.place ? e.place : zone
  return [place.toLowerCase(), e.tag].filter(Boolean).join(' · ')
}

// --- live feed: kind filter + pause -------------------------------------------

const CHIPS = ['ALL', 'COMBAT', 'ECONOMY', 'TRAVEL', 'DEATHS'] as const
const filter = ref<(typeof CHIPS)[number]>('ALL')
const paused = ref(false)

// Chip buckets over the kinds the recorder actually emits; anything unmapped
// (party, level) still flows through ALL rather than vanishing.
const KMAP: Record<string, string[]> = {
  COMBAT: ['pvp', 'revive'],
  ECONOMY: ['loot'],
  TRAVEL: ['zone', 'instance'],
  DEATHS: ['death'],
}

// Data colours, fixed across palettes: travel wears the travelling yellow,
// deaths the mortal red, money the money var - the tick is a legend, not chrome.
const KC: Record<string, string> = {
  zone: STATE.travelling.color,
  instance: STATE.instance.color,
  pvp: 'oklch(0.68 0.19 25)',
  death: STATE.dead.color,
  revive: STATE.grinding.color,
  loot: V.money,
  party: T.green,
  level: STATE.town.color,
}

// Pausing freezes what is on screen; the poll keeps running underneath.
const shown = ref<FeedEvent[]>([...props.events])
watch(() => props.events, list => { if (!paused.value) shown.value = list })
watch(paused, p => { if (!p) shown.value = props.events })

const visibleEvents = computed(() =>
  filter.value === 'ALL'
    ? shown.value
    : shown.value.filter(e => KMAP[filter.value]?.includes(e.kind)))

// --- dossier ------------------------------------------------------------------

const money = (c: number) =>
  c >= 10000 ? `${(c / 10000).toFixed(1)}g` : c >= 100 ? `${Math.round(c / 100)}s` : `${c}c`

// Three cells from what the bridge actually knows. Carried gold leads when the
// history carries it; until then the cells stay honest rather than invented.
const stats = computed(() => {
  const d = props.detail
  if (!d?.feed) return []
  const dayAgo = Date.now() / 1000 - 86400
  const day = d.feed.filter((e: any) => Number(e.ts) > dayAgo)
  const deaths = day.filter((e: any) =>
    e.kind === 'death' || /^died\b/.test(String(e.detail ?? ''))).length
  const cells: { v: string; l: string; color: string }[] = []
  if (d.money != null) cells.push({ v: money(Number(d.money)), l: 'CARRIED', color: V.moneyBright })
  cells.push({ v: fmt.int(day.length), l: 'EVENTS 24H', color: V.textHi })
  cells.push({ v: fmt.int(deaths), l: 'DEATHS 24H', color: V.textHi })
  if (cells.length < 3) cells.push({ v: fmt.int(d.knows?.length ?? 0), l: 'KNOWS', color: V.textHi })
  return cells.slice(0, 3)
})

const spark = computed(() => {
  const feed = props.detail?.feed ?? []
  const now = Date.now() / 1000
  const buckets = Array<number>(24).fill(0)
  for (const e of feed) {
    const age = now - Number(e.ts)
    if (age < 0 || age >= 86400) continue
    buckets[23 - Math.floor(age / 3600)]!++
  }
  return buckets
})
const sparkPeak = computed(() => Math.max(1, ...spark.value))
const sparkAny = computed(() => spark.value.some(v => v > 0))
</script>

<template>
  <aside
    :style="{
      borderLeft: `1px solid ${V.line}`, background: V.bg,
      display: 'flex', flexDirection: 'column', minHeight: 0, height: '100%',
      overflow: 'hidden',
    }"
  >
    <!-- Dossier replaces the feed rather than sitting beside it: the rail is narrow,
         and reading one character is a different job from watching the realm. -->
    <template v-if="selected">
      <div :style="{ padding: '13px 16px 12px', borderBottom: `1px solid ${V.line}`, flex: 'none' }">
        <button
          :style="{
            appearance: 'none', background: 'none', border: 'none', padding: 0,
            cursor: 'pointer', fontFamily: FONT.display, fontWeight: 600,
            fontSize: '9.5px', letterSpacing: '.16em', color: V.dim,
          }"
          @click="emit('clear')"
        >← LIVE FEED</button>

        <div :style="{ display: 'flex', alignItems: 'baseline', gap: '9px', marginTop: '9px', flexWrap: 'wrap' }">
          <span
            :style="{
              fontSize: '24px', fontWeight: 600, letterSpacing: '.01em',
              color: CLASS_COLOR[detail?.cls ?? 0] ?? V.textHi,
            }"
          >{{ selected }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.muted }">
            {{ detail?.level ? `level ${detail.level}` : '' }}
          </span>
        </div>

        <div
          v-if="detail?.class || detail?.race || detail?.guild"
          :style="{ fontSize: '13.5px', color: V.textMid, marginTop: '2px' }"
        >
          {{ [detail?.race, detail?.class].filter(Boolean).join(' ') }}<span
            v-if="detail?.guild"
          > · &lt;{{ detail.guild }}&gt;</span>
        </div>
        <div
          v-if="detail?.zone"
          :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint, marginTop: '3px', letterSpacing: '.06em' }"
        >{{ zoneName(detail.zone).toLowerCase() }}</div>

        <p
          v-if="activity"
          :style="{
            margin: '10px 0 0', padding: '8px 10px', fontSize: '13.5px',
            lineHeight: 1.45, color: V.text,
            borderLeft: `2px solid ${V.accentDim}`, background: V.panel,
          }"
        >{{ activity }}</p>

        <div :style="{ display: 'flex', gap: '6px', marginTop: '10px' }">
          <button
            :style="{
              appearance: 'none', background: 'none', flex: 1, textAlign: 'center',
              border: `1px solid ${V.accentDim}`, color: V.accentBright,
              fontFamily: FONT.display, fontWeight: 600, fontSize: '9px',
              letterSpacing: '.14em', padding: '6px 0', cursor: 'pointer',
            }"
            @click="emit('map')"
          >SHOW ON MAP</button>
          <button
            :style="{
              appearance: 'none', background: 'none', flex: 1, textAlign: 'center',
              border: `1px solid ${V.line}`, color: V.muted,
              fontFamily: FONT.display, fontWeight: 600, fontSize: '9px',
              letterSpacing: '.14em', padding: '6px 0', cursor: 'default', opacity: 0.6,
            }"
            title="Following is not wired yet"
          >FOLLOW</button>
        </div>

        <div v-if="detail?.personality" :style="{ display: 'flex', gap: '6px', marginTop: '10px', flexWrap: 'wrap' }">
          <span
            v-for="tagText in [detail.personality.archetype, detail.personality.temperament, detail.personality.interest].filter(Boolean)"
            :key="tagText"
            :style="{
              border: `1px solid ${V.line}`, color: V.textMid, fontFamily: FONT.display,
              fontWeight: 600, fontSize: '9.5px', letterSpacing: '.12em',
              padding: '3px 7px', textTransform: 'uppercase',
            }"
          >{{ tagText }}</span>
        </div>
      </div>

      <div v-if="stats.length" :style="{ display: 'flex', borderBottom: `1px solid ${V.line}`, flex: 'none' }">
        <div
          v-for="s in stats"
          :key="s.l"
          :style="{ flex: 1, padding: '10px 0 9px', textAlign: 'center', borderRight: `1px solid ${V.lineFaint}` }"
        >
          <div :style="{ fontFamily: FONT.mono, fontSize: '15px', color: s.color, fontVariantNumeric: 'tabular-nums' }">{{ s.v }}</div>
          <div :style="{ fontFamily: FONT.display, fontSize: '7.5px', fontWeight: 600, letterSpacing: '.16em', color: V.dim, marginTop: '2px' }">{{ s.l }}</div>
        </div>
      </div>

      <div v-if="sparkAny" :style="{ padding: '11px 16px 9px', flex: 'none' }">
        <div :style="{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between' }">
          <span :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.16em', color: V.dim }">ACTIVITY · 24H</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">peak {{ sparkPeak }} events</span>
        </div>
        <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '34px', marginTop: '7px' }">
          <span
            v-for="(v, i) in spark"
            :key="i"
            :style="{ flex: 1, height: `${(v / sparkPeak) * 100}%`, minHeight: v > 0 ? '2px' : '0', background: V.moneyDim }"
          />
        </div>
        <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9px', color: V.faint }">
          <span>24h ago</span><span>now</span>
        </div>
      </div>

      <div :style="{ flex: 1, overflow: 'auto', minHeight: 0, borderTop: `1px solid ${V.lineFaint}`, padding: '4px 0' }">
        <div :style="{ padding: '7px 16px 5px', fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px', letterSpacing: '.16em', color: V.dim }">THEIR LAST HOURS</div>
        <p
          v-if="loading"
          :style="{ padding: '7px 16px', color: V.muted, fontSize: '13.5px', margin: 0 }"
        >Reading…</p>

        <p
          v-else-if="!detail?.feed?.length"
          :style="{ padding: '7px 16px', color: V.muted, fontSize: '13.5px', margin: 0, lineHeight: 1.45 }"
        >Nothing recorded for this character yet. The recorder only writes when something
          happens to them.</p>

        <div
          v-for="(ev, i) in detail?.feed ?? []"
          :key="i"
          :style="{
            padding: '7px 16px', borderBottom: `1px solid ${V.lineFaint}`,
            display: 'grid', gridTemplateColumns: '38px minmax(0, 1fr)', gap: '9px',
          }"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint, paddingTop: '2px' }">
            {{ fmt.ago(ev.ts * 1000) }}
          </span>
          <span :style="{ fontSize: '13.5px', color: V.text, lineHeight: 1.35, overflowWrap: 'anywhere' }">{{ humanize(ev.detail) }}</span>
        </div>
      </div>
    </template>

    <template v-else>
      <div :style="{ padding: '12px 16px 10px', borderBottom: `1px solid ${V.line}`, flex: 'none' }">
        <div :style="{ display: 'flex', alignItems: 'baseline', justifyContent: 'space-between' }">
          <span :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '10.5px', letterSpacing: '.16em', color: V.dim }">LIVE FEED</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: paused ? V.accent : V.faint }">
            {{ paused ? 'paused' : 'streaming · 5s' }}
          </span>
        </div>
        <div :style="{ display: 'flex', gap: '5px', marginTop: '9px', flexWrap: 'wrap' }">
          <button
            v-for="ch in CHIPS"
            :key="ch"
            :style="{
              appearance: 'none', fontFamily: FONT.display, fontSize: '8.5px',
              fontWeight: 600, letterSpacing: '.14em', padding: '4px 8px',
              border: `1px solid ${filter === ch ? V.accentDim : V.line}`,
              background: filter === ch ? V.raised : 'transparent',
              color: filter === ch ? V.accentBright : V.muted, cursor: 'pointer',
            }"
            @click="filter = ch"
          >{{ ch }}</button>
          <button
            :style="{
              appearance: 'none', fontFamily: FONT.display, fontSize: '8.5px',
              fontWeight: 600, letterSpacing: '.14em', padding: '4px 8px',
              border: `1px solid ${paused ? V.accentDim : V.line}`,
              background: paused ? V.raised : 'transparent',
              color: paused ? V.accentBright : V.muted, cursor: 'pointer',
            }"
            @click="paused = !paused"
          >{{ paused ? 'RESUME' : 'PAUSE' }}</button>
        </div>
      </div>

      <div :style="{ flex: 1, overflow: 'auto', minHeight: 0 }">
        <p
          v-if="!ready || !visibleEvents.length"
          :style="{ padding: '14px 16px', color: V.muted, fontSize: '13.5px', margin: 0, lineHeight: 1.45 }"
        >{{ ready && shown.length ? 'Nothing of that kind recently.' : 'No activity recorded yet.' }}</p>

        <button
          v-for="e in visibleEvents"
          :key="e.id"
          class="hv-row"
          :style="{
            width: '100%', textAlign: 'left', appearance: 'none',
            border: 'none', borderBottom: `1px solid ${V.lineFaint}`,
            borderLeft: `3px solid ${KC[e.kind] ?? V.accent}`, cursor: 'pointer',
            padding: '8px 16px 8px 13px', display: 'grid',
            gridTemplateColumns: '38px minmax(0, 1fr)', gap: '9px',
          }"
          @click="emit('select', e.who)"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint, paddingTop: '2px' }">
            {{ fmt.clock(e.at) }}
          </span>
          <span>
            <span :style="{ fontSize: '14px', color: V.textHi, lineHeight: 1.3, display: 'block', overflowWrap: 'anywhere' }">
              <span :style="{ color: CLASS_COLOR[e.cls] ?? V.textHi, fontWeight: 500 }">{{ e.who }}</span>
              {{ ' ' }}{{ sentence(e) }}
            </span>
            <span
              :style="{
                fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.08em',
                color: V.faint, marginTop: '2px', display: 'block',
              }"
            >{{ where(e) }}</span>
          </span>
        </button>
      </div>
    </template>
  </aside>
</template>
