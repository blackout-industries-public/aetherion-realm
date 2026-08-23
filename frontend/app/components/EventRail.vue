<script setup lang="ts">
import { T, FONT, fmt } from '../theme'
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

const emit = defineEmits<{ select: [string]; clear: [] }>()

// The recorder stores "moved to zone 4395". Naming the zone is the client's job -
// it already holds the table, and a raw id in a feed is unreadable.
const unnamed = (s: string) => /^Zone \d+$/.test(s)

// The per-character feed comes straight from the bridge, which stores raw ids. The
// same naming the live feed gets is applied here rather than showing "zone 3703".
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
</script>

<template>
  <aside
    :style="{
      borderLeft: `1px solid ${T.line}`, background: T.bg,
      display: 'flex', flexDirection: 'column', minHeight: 0, height: '100%',
      overflow: 'hidden',
    }"
  >
    <!-- Detail replaces the feed rather than sitting beside it: the rail is narrow,
         and reading one character is a different job from watching the realm. -->
    <template v-if="selected">
      <div :style="{ padding: '14px 16px 12px', borderBottom: `1px solid ${T.line}`, flex: 'none' }">
        <button
          :style="{
            appearance: 'none', background: 'none', border: 'none', padding: 0,
            cursor: 'pointer', fontFamily: FONT.display, fontWeight: 600,
            fontSize: '10.5px', letterSpacing: '.16em', color: T.dim, marginBottom: '9px',
          }"
          @click="emit('clear')"
        >← BACK TO FEED</button>

        <div :style="{ display: 'flex', alignItems: 'baseline', gap: '9px', flexWrap: 'wrap' }">
          <span
            :style="{
              fontSize: '25px', fontWeight: 600, letterSpacing: '.01em',
              color: CLASS_COLOR[detail?.cls ?? 0] ?? T.textHi,
            }"
          >{{ selected }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.muted }">
            {{ detail?.level ? `level ${detail.level}` : '' }}
          </span>
        </div>

        <div
          v-if="detail?.class || detail?.race || detail?.guild"
          :style="{ fontSize: '14px', color: T.textMid, marginTop: '3px' }"
        >
          {{ [detail?.race, detail?.class].filter(Boolean).join(' ') }}<span
            v-if="detail?.guild"
          > · &lt;{{ detail.guild }}&gt;</span>
        </div>
        <div
          v-if="detail?.zone"
          :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint, marginTop: '3px', letterSpacing: '.06em' }"
        >{{ zoneName(detail.zone).toLowerCase() }}</div>

        <p
          v-if="activity"
          :style="{
            margin: '10px 0 0', padding: '8px 10px', fontSize: '13.5px',
            lineHeight: 1.45, color: T.text,
            borderLeft: `2px solid ${T.goldDim}`, background: T.panel,
          }"
        >{{ activity }}</p>

        <div v-if="detail?.personality" :style="{ display: 'flex', gap: '6px', marginTop: '10px', flexWrap: 'wrap' }">
          <span
            v-for="tagText in [detail.personality.archetype, detail.personality.temperament, detail.personality.interest].filter(Boolean)"
            :key="tagText"
            :style="{
              border: `1px solid ${T.line}`, color: T.textMid, fontFamily: FONT.display,
              fontWeight: 600, fontSize: '10.5px', letterSpacing: '.12em',
              padding: '3px 7px', textTransform: 'uppercase',
            }"
          >{{ tagText }}</span>
        </div>
      </div>

      <div :style="{ flex: 1, overflow: 'auto', minHeight: 0, padding: '4px 0' }">
        <p
          v-if="loading"
          :style="{ padding: '14px 16px', color: T.muted, fontSize: '13.5px', margin: 0 }"
        >Reading…</p>

        <p
          v-else-if="!detail?.feed?.length"
          :style="{ padding: '14px 16px', color: T.muted, fontSize: '13.5px', margin: 0, lineHeight: 1.45 }"
        >Nothing recorded for this character yet. The recorder only writes when something
          happens to them.</p>

        <div
          v-for="(ev, i) in detail?.feed ?? []"
          :key="i"
          :style="{
            padding: '9px 16px', borderBottom: `1px solid ${T.lineFaint}`,
            display: 'grid', gridTemplateColumns: '44px minmax(0, 1fr)', gap: '10px',
          }"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint, paddingTop: '2px' }">
            {{ fmt.ago(ev.ts * 1000) }}
          </span>
          <span :style="{ fontSize: '14px', color: T.text, lineHeight: 1.35, overflowWrap: 'anywhere' }">{{ humanize(ev.detail) }}</span>
        </div>
      </div>
    </template>

    <template v-else>
      <div :style="{ padding: '14px 16px 10px', borderBottom: `1px solid ${T.line}`, flex: 'none' }">
        <div
          :style="{
            fontFamily: FONT.display, fontWeight: 600, fontSize: '10.5px',
            letterSpacing: '.16em', color: T.dim,
          }"
        >NOW · BOT_EVENTS</div>
        <div :style="{ fontSize: '13px', color: T.muted, marginTop: '5px', lineHeight: 1.35 }">
          Click anyone on the map, in a group, or in chat to read them.
        </div>
      </div>

      <div :style="{ flex: 1, overflow: 'auto', minHeight: 0, padding: '4px 0' }">
        <p
          v-if="!ready || !events.length"
          :style="{ padding: '14px 16px', color: T.muted, fontSize: '13.5px', margin: 0, lineHeight: 1.45 }"
        >No activity recorded yet.</p>

        <button
          v-for="e in events"
          :key="e.id"
          :style="{
            width: '100%', textAlign: 'left', appearance: 'none', background: 'none',
            border: 'none', borderBottom: `1px solid ${T.lineFaint}`, cursor: 'pointer',
            padding: '9px 16px', display: 'grid', gridTemplateColumns: '44px minmax(0, 1fr)',
            gap: '10px',
          }"
          @click="emit('select', e.who)"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint, paddingTop: '2px' }">
            {{ fmt.clock(e.at) }}
          </span>
          <span>
            <span :style="{ fontSize: '14.5px', color: T.textHi, lineHeight: 1.3, display: 'block', overflowWrap: 'anywhere' }">
              <span :style="{ color: CLASS_COLOR[e.cls] ?? T.textHi, fontWeight: 500 }">{{ e.who }}</span>
              {{ ' ' }}{{ sentence(e) }}
            </span>
            <span
              :style="{
                fontFamily: FONT.mono, fontSize: '10.5px', letterSpacing: '.08em',
                color: T.faint, marginTop: '3px', display: 'block',
              }"
            >{{ where(e) }}</span>
          </span>
        </button>
      </div>
    </template>
  </aside>
</template>
