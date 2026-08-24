<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, V, fmt, spell } from '../theme'
import { CLASS_COLOR } from '../data'
import UiPanel from './UiPanel.vue'
import UiTimeline from './UiTimeline.vue'

const props = defineProps<{ race: any | null }>()
const emit = defineEmits<{ select: [string] }>()

const FACTION = { alliance: 'oklch(0.68 0.13 250)', horde: 'oklch(0.62 0.17 25)' } as const
// Data colour, not chrome: the field is the same blue in every palette.
const FIELD = 'oklch(0.55 0.08 200)'

const started = computed(() => props.race?.raceStart ?? null)

// Without a declared gun the clock runs from the earliest recorded first, so the
// strip still reads left-to-right in time instead of collapsing.
const clockStart = computed(() => {
  if (started.value) return started.value
  const ts = [
    ...(props.race?.levelFirsts ?? []).map((f: any) => f.at),
    ...(props.race?.bossFirsts ?? []).map((b: any) => b.at),
  ]
  return ts.length ? Math.min(...ts) : null
})

const frac = (at: number) => {
  const from = clockStart.value
  const span = (props.race?.at ?? Date.now()) - (from ?? 0)
  return from == null || span <= 0 ? 0 : (at - from) / span
}

function stamp(at: number) {
  const from = started.value ?? clockStart.value
  if (!from) return fmt.clock(at)
  const s = Math.max(0, Math.round((at - from) / 1000))
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60)
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`
}

function sinceWords(ms: number) {
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000))
  const d = Math.floor(s / 86400), h = Math.floor(s / 3600)
  if (d >= 2) return `${spell(d)} days`
  if (d === 1) return 'a day'
  if (h >= 2) return `${spell(h)} hours`
  if (h === 1) return 'an hour'
  return 'under an hour'
}

const lede = computed(() => {
  const r = props.race
  if (!r) return 'Waiting for race data.'
  if (!started.value) {
    return 'No race has been declared. Firsts below are counted from when the recorder ' +
      'began watching; a server wipe writes the starting gun and resets this board.'
  }
  const top = r.standings?.[0]
  return `The race began ${sinceWords(started.value)} ago. ` +
    (top ? `${top.name} leads at level ${top.level}.` : 'Nobody has moved yet.')
})

const clockNote = computed(() =>
  started.value
    ? `gun → now · circles are level firsts in the claimant's class colour · ` +
      `diamonds are boss firsts`
    : 'stopped')

// A plot needs a race. Without a declared gun every first predates the watch
// and the marks collapse into a blob at zero - honest words beat a wrong
// picture, so the strip only renders once a wipe writes the starting gun.
const clockPlottable = computed(() => started.value != null)

const clockMarks = computed(() =>
  (props.race?.levelFirsts ?? []).map((f: any) => ({
    at: frac(f.at), label: String(f.threshold),
    color: CLASS_COLOR[f.cls] ?? V.textHi,
    title: `${f.who} · ${stamp(f.at)}`,
  })))

// Every boss first is on file, but 60+ diamonds is noise: keep the earliest, then
// only those far enough from the last kept one to stay readable.
const clockBosses = computed(() => {
  const kept: { at: number; label: string; title: string }[] = []
  let last = -1
  for (const b of props.race?.bossFirsts ?? []) {
    const p = frac(b.at)
    if (p - last < 0.06) continue
    last = p
    // Base name only: parenthetical dungeon suffixes truncate into noise at
    // diamond-label size.
    const base = String(b.boss).split('(')[0]!.trim()
    kept.push({
      at: p, label: base.split(' ').pop() ?? base,
      title: `${b.boss} · ${stamp(b.at)}`,
    })
  }
  return kept
})

const thresholds = computed(() => {
  const byT = new Map<number, any>()
  for (const f of props.race?.levelFirsts ?? []) byT.set(f.threshold, f)
  return [10, 20, 30, 40, 50, 60, 70, 80].map(t => ({ t, first: byT.get(t) ?? null }))
})

const bossRows = computed(() =>
  (props.race?.bossFirsts ?? []).slice(0, 10).map((b: any) => ({
    ...b, names: String(b.who ?? '').split(',').map((s: string) => s.trim()).filter(Boolean),
  })))

// The shape of the whole field, so the race reads as a wave and not just a podium.
const shapeBars = computed(() => {
  const byLevel = new Map<number, number>()
  for (const r of props.race?.shape ?? []) byLevel.set(r.level, r.n)
  const max = Math.max(1, ...byLevel.values())
  return Array.from({ length: 80 }, (_, i) => {
    const level = i + 1, n = byLevel.get(level) ?? 0
    return { level, n, h: n ? Math.max(2, (n / max) * 100) : 0 }
  })
})
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
        lineHeight: 1.4, color: V.textMid,
      }"
    >{{ lede }}</p>

    <!-- Hand-rolled panel: the clock wants a 2px header gap and a wrapping note,
         which UiPanel's nowrap header cannot express. -->
    <section :style="{ border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset, flex: 'none' }">
      <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', gap: '10px', padding: '10px 12px 2px' }">
        <span :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '10px', letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' }">The race clock</span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, textAlign: 'right' }">{{ clockNote }}</span>
      </header>
      <div v-if="clockPlottable" :style="{ padding: '6px 26px 10px' }">
        <UiTimeline :marks="clockMarks" :bosses="clockBosses" />
      </div>
      <p v-else :style="{ margin: 0, padding: '8px 12px 12px', fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
        The clock is stopped. A server wipe writes the starting gun; from that
        moment this strip plots every level first and boss first against real
        elapsed time.
      </p>
    </section>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(3, 1fr)', gap: '16px', alignItems: 'start' }">
      <UiPanel cap="Level firsts" :note="started ? 'elapsed from the gun' : 'since recording began'">
        <div
          v-for="row in thresholds"
          :key="row.t"
          :style="{
            display: 'grid', gridTemplateColumns: '40px 1fr auto', gap: '10px',
            padding: '5.5px 0', alignItems: 'baseline',
            borderBottom: `1px solid ${V.lineFaint}`,
          }"
        >
          <span :style="{ fontFamily: FONT.display, fontWeight: 700, fontSize: '15px', color: row.first ? V.accentBright : V.pip }">
            {{ row.t }}
          </span>
          <button
            v-if="row.first"
            class="nm"
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.body, fontSize: '14px', color: CLASS_COLOR[row.first.cls] ?? V.textHi, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
            @click="emit('select', row.first.who)"
          >{{ row.first.who }}</button>
          <span v-else :style="{ fontSize: '13px', color: V.faint }">unclaimed</span>
          <span v-if="row.first" :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.muted }">
            {{ stamp(row.first.at) }}
          </span>
        </div>
      </UiPanel>

      <UiPanel cap="Standings" note="level, then who got there first">
        <div
          v-for="s in race?.standings ?? []"
          :key="s.guid"
          :style="{ display: 'grid', gridTemplateColumns: '24px 8px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: s.rank <= 3 ? V.accentBright : V.faint }">
            {{ s.rank }}
          </span>
          <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: FACTION[s.faction as 'alliance'|'horde'], alignSelf: 'center' }" />
          <button
            class="nm"
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.body, fontSize: '13.5px', color: CLASS_COLOR[s.cls] ?? V.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
            @click="emit('select', s.name)"
          >{{ s.name }}</button>
          <span :style="{ fontFamily: FONT.mono, fontSize: '12.5px', color: V.textHi }">{{ s.level }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ fmt.int(s.quests) }} q</span>
        </div>
      </UiPanel>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Boss firsts" :note="`${race?.bossFirstsTotal ?? 0} claimed`">
          <p
            v-if="!bossRows.length"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >No boss has fallen for the first time since recording began.</p>
          <div
            v-for="b in bossRows"
            :key="b.boss"
            :style="{ padding: '5px 0', borderBottom: `1px solid ${V.lineFaint}` }"
          >
            <div :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px' }">
              <span :style="{ fontSize: '13.5px', color: V.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ b.boss }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.muted, whiteSpace: 'nowrap' }">{{ stamp(b.at) }}</span>
            </div>
            <div :style="{ fontSize: '11.5px', color: V.faint, marginTop: '2px' }">
              <template v-if="b.instance">{{ b.instance }}<template v-if="b.names.length"> &middot; </template></template>
              <template v-for="(n, i) in b.names" :key="n">
                <button
                  class="nm"
                  :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontFamily: FONT.body, fontSize: '11.5px', color: V.muted }"
                  @click="emit('select', n)"
                >{{ n }}</button><template v-if="i < b.names.length - 1">, </template>
              </template>
            </div>
          </div>
        </UiPanel>

        <UiPanel cap="Full clears">
          <p
            v-if="!(race?.clears ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }"
          >No instance has been fully cleared yet.</p>
          <div
            v-for="c in race?.clears ?? []"
            :key="c.instance"
            :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px', padding: '3.5px 0' }"
          >
            <span :style="{ fontSize: '13.5px', color: T.green }">{{ c.instance }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.muted }">{{ stamp(c.at) }}</span>
          </div>
        </UiPanel>

        <UiPanel cap="The field" note="online, by level">
          <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '1px', height: '52px' }">
            <span
              v-for="b in shapeBars"
              :key="b.level"
              :style="{ flex: 1, height: `${b.h}%`, background: b.level === 80 ? V.accent : FIELD }"
              :title="`level ${b.level}: ${b.n}`"
            />
          </div>
          <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: V.faint }">
            <span>1</span><span>80</span>
          </div>
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
            After a wipe this reads as a wave moving right; the spike at 80 is the fleet parking at cap.
          </p>
        </UiPanel>
      </div>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline }
</style>
