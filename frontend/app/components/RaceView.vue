<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, fmt, spell, titled } from '../theme'
import { CLASS_COLOR } from '../data'
import UiPanel from './UiPanel.vue'

const props = defineProps<{ race: any | null }>()
const emit = defineEmits<{ select: [string] }>()

const FACTION = { alliance: 'oklch(0.68 0.13 250)', horde: 'oklch(0.62 0.17 25)' } as const

const started = computed(() => props.race?.raceStart ?? null)

// Elapsed since the race gun, or wall-clock when no race has been declared yet.
function stamp(at: number) {
  if (!started.value) return fmt.clock(at)
  const s = Math.max(0, Math.round((at - started.value) / 1000))
  const d = Math.floor(s / 86400), h = Math.floor((s % 86400) / 3600), m = Math.floor((s % 3600) / 60)
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`
}

const lede = computed(() => {
  const r = props.race
  if (!r) return 'Waiting for race data.'
  if (!started.value) {
    return 'No race has been declared. Firsts below are counted from when the recorder ' +
      'began watching; a server wipe writes the starting gun and resets this board.'
  }
  const top = r.standings?.[0]
  return `The race began ${fmt.ago(started.value)} ago. ` +
    (top ? `${top.name} leads at level ${top.level}.` : 'Nobody has moved yet.')
})

const thresholds = computed(() => {
  const byT = new Map<number, any>()
  for (const f of props.race?.levelFirsts ?? []) byT.set(f.threshold, f)
  return [10, 20, 30, 40, 50, 60, 70, 80].map(t => ({ t, first: byT.get(t) ?? null }))
})

// The shape of the whole field, so the race reads as a wave and not just a podium.
const shapeBars = computed(() => {
  const rows = props.race?.shape ?? []
  const max = Math.max(1, ...rows.map((r: any) => r.n))
  return rows.map((r: any) => ({ ...r, h: Math.max(2, (r.n / max) * 100) }))
})
</script>

<template>
  <section :style="{ display: 'grid', gridTemplateRows: 'auto 1fr', gap: '14px', padding: '20px 22px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${T.goldDim}`, paddingLeft: '15px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '17.5px',
        lineHeight: 1.4, color: T.textMid, maxWidth: '64ch',
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '16px', alignItems: 'start' }">
      <UiPanel cap="Level firsts" :note="started ? 'elapsed from the gun' : 'since recording began'">
        <div
          v-for="row in thresholds"
          :key="row.t"
          :style="{
            display: 'grid', gridTemplateColumns: '44px 1fr auto', gap: '10px',
            padding: '6px 0', alignItems: 'baseline',
            borderBottom: `1px solid ${T.lineFaint}`,
          }"
        >
          <span :style="{ fontFamily: FONT.display, fontWeight: 700, fontSize: '15px', color: row.first ? T.goldBright : 'oklch(0.42 0.03 62)' }">
            {{ row.t }}
          </span>
          <span v-if="row.first" :style="{ minWidth: 0 }">
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '14px', color: T.textHi }"
              @click="emit('select', row.first.who)"
            >{{ row.first.who }}</button>
          </span>
          <span v-else :style="{ fontSize: '13px', color: T.faint }">unclaimed</span>
          <span v-if="row.first" :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted }">
            {{ stamp(row.first.at) }}
          </span>
        </div>
      </UiPanel>

      <UiPanel cap="Standings" note="level, then who got there first">
        <div
          v-for="s in race?.standings ?? []"
          :key="s.guid"
          :style="{ display: 'grid', gridTemplateColumns: '26px 8px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: s.rank <= 3 ? T.goldBright : T.faint }">
            {{ s.rank }}
          </span>
          <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: FACTION[s.faction as 'alliance'|'horde'], marginTop: '3px' }" />
          <button
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13.5px', color: CLASS_COLOR[s.cls] ?? T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
            @click="emit('select', s.name)"
          >{{ s.name }}</button>
          <span :style="{ fontFamily: FONT.mono, fontSize: '12.5px', color: T.textHi }">{{ s.level }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ fmt.int(s.quests) }} q</span>
        </div>
      </UiPanel>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Boss firsts" :note="`${race?.bossFirstsTotal ?? 0} claimed`">
          <p
            v-if="!(race?.bossFirsts ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No boss has fallen for the first time since recording began.</p>
          <div
            v-for="b in race?.bossFirsts ?? []"
            :key="b.boss"
            :style="{ padding: '5px 0', borderBottom: `1px solid ${T.lineFaint}` }"
          >
            <div :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px' }">
              <span :style="{ fontSize: '13.5px', color: T.text, minWidth: 0, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ b.boss }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.muted, whiteSpace: 'nowrap' }">{{ stamp(b.at) }}</span>
            </div>
            <div v-if="b.who" :style="{ fontSize: '11.5px', color: T.faint, marginTop: '2px' }">{{ b.who }}</div>
          </div>
        </UiPanel>

        <UiPanel v-if="(race?.clears ?? []).length" cap="Full clears">
          <div
            v-for="c in race.clears"
            :key="c.instance"
            :style="{ display: 'flex', justifyContent: 'space-between', gap: '10px', padding: '4px 0' }"
          >
            <span :style="{ fontSize: '13.5px', color: T.green }">{{ c.instance }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.muted }">{{ stamp(c.at) }}</span>
          </div>
        </UiPanel>

        <UiPanel cap="The field" note="online, by level">
          <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '1px', height: '52px' }">
            <span
              v-for="b in shapeBars"
              :key="b.level"
              :style="{ flex: 1, height: `${b.h}%`, background: b.level === 80 ? T.gold : 'oklch(0.55 0.08 200)', borderRadius: '1px 1px 0 0' }"
              :title="`level ${b.level}: ${b.n}`"
            />
          </div>
          <div :style="{ display: 'flex', justifyContent: 'space-between', marginTop: '4px', fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }">
            <span>1</span><span>80</span>
          </div>
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            After a wipe this reads as a wave moving right; today it is a parked pyramid.
          </p>
        </UiPanel>
      </div>
    </div>
  </section>
</template>
