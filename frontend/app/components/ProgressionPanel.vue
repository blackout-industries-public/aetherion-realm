<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, V, fmt, spell, titled } from '../theme'

const { data: prog, refresh } = await useFetch<any>('/api/progression')

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { refresh() }, 60_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

const panel = { border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset }
const cap = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase' as const,
}
const capMini = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '9px',
  letterSpacing: '.14em', color: V.faint2, marginBottom: '6px',
}

const old = computed(() => prog.value?.old ?? null)
const eras = computed(() => prog.value?.eras ?? [])
const earning = computed(() => prog.value?.earning ?? [])
const legendaries = computed(() => prog.value?.legendaries ?? [])

// The old world is a frontier, not a scoreboard: the sentence has to work when
// the answer is "almost none of it".
const lede = computed(() => {
  const o = old.value
  if (!o) return ''
  if (!o.killed) return `None of the ${fmt.int(o.bosses)} bosses in the old world have been beaten here.`
  return `${titled(spell(o.killed))} of ${fmt.int(o.bosses)} old-world bosses have been beaten, ` +
    `across ${spell(o.visited)} of ${spell(o.raids)} raids ever entered.`
})

const KIND_NOTE: Record<string, string> = {
  quest: 'quest', item: 'key', achievement: 'achievement',
}
</script>

<template>
  <div v-if="prog" :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '14px',
        lineHeight: 1.45, color: V.textMid,
      }"
    >{{ lede }}</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(300px, 1fr))', gap: '14px', alignItems: 'start' }">
      <section v-for="era in eras" :key="era.key" :style="panel">
        <header :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 12px 8px' }">
          <span :style="cap">{{ era.name }}</span>
          <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">level {{ era.level }}</span>
        </header>

        <div :style="{ padding: '0 12px 12px', display: 'flex', flexDirection: 'column', gap: '11px' }">
          <div v-if="era.gates.length">
            <div :style="capMini">GATES</div>
            <div
              v-for="g in era.gates"
              :key="g.name + g.opens.join()"
              :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '8px', alignItems: 'baseline', padding: '2px 0' }"
            >
              <span :style="{ minWidth: 0 }">
                <span :style="{ fontSize: '12.5px', color: g.holders ? V.textHi : V.muted }">{{ g.name }}</span>
                <span :style="{ fontSize: '11px', color: V.faint }"> · {{ g.opens.join(', ') }}</span>
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: g.holders ? T.green : V.faint, whiteSpace: 'nowrap' }">
                {{ g.holders ? fmt.int(g.holders) : '—' }}<span v-if="g.inProgress" :style="{ color: V.accent }"> +{{ fmt.int(g.inProgress) }}</span>
              </span>
            </div>
          </div>

          <div v-if="era.raids.length">
            <div :style="capMini">OLD RAIDS</div>
            <div
              v-for="r in era.raids"
              :key="r.map"
              :style="{ display: 'grid', gridTemplateColumns: '1fr 54px auto', gap: '9px', alignItems: 'center', padding: '2px 0' }"
            >
              <span :style="{ fontSize: '12.5px', color: r.killed ? V.body : V.muted, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">{{ r.name }}</span>
              <span :style="{ height: '6px', background: V.track, position: 'relative' }">
                <span
                  v-if="r.killed"
                  :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${Math.round((r.killed / Math.max(1, r.bosses)) * 100)}%`, background: T.green }"
                />
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: r.killed ? V.text : V.faint, whiteSpace: 'nowrap' }">
                {{ r.killed }}/{{ r.bosses }}<span v-if="r.visits" :style="{ color: V.faint }"> · {{ r.visits }} in</span>
              </span>
            </div>
          </div>
        </div>
      </section>
    </div>

    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '14px', alignItems: 'start' }">
      <section :style="panel">
        <header :style="{ padding: '10px 12px 6px' }"><span :style="cap">Being earned now</span></header>
        <div :style="{ padding: '0 12px 12px' }">
          <div
            v-for="e in earning"
            :key="e.name"
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '9px', alignItems: 'baseline', padding: '2px 0' }"
          >
            <span :style="{ fontSize: '12.5px', color: V.body }">{{ e.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.text, whiteSpace: 'nowrap' }">
              {{ e.runs }} run{{ e.runs === 1 ? '' : 's' }}<span v-if="e.underway" :style="{ color: T.green }"> · {{ e.underway }} out now</span>
            </span>
          </div>
          <p v-if="!earning.length" :style="{ margin: 0, fontSize: '12.5px', color: V.muted, lineHeight: 1.5 }">
            No party is working on an attunement.
          </p>
        </div>
      </section>

      <section :style="panel">
        <header :style="{ padding: '10px 12px 6px' }"><span :style="cap">Legendaries</span></header>
        <div :style="{ padding: '0 12px 12px' }">
          <div
            v-for="l in legendaries"
            :key="l.name"
            :style="{ display: 'grid', gridTemplateColumns: '1fr auto', gap: '9px', alignItems: 'baseline', padding: '2px 0' }"
          >
            <span :style="{ fontSize: '12.5px', color: 'oklch(0.74 0.16 60)' }">{{ l.name }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: V.text }">{{ l.owners }}</span>
          </div>
          <p v-if="!legendaries.length" :style="{ margin: 0, fontSize: '12.5px', color: V.muted, lineHeight: 1.5 }">
            Nothing legendary exists on this realm yet. Sulfuras and Thunderfury
            are still in the fire — their pieces drop in Molten Core, which no
            party has cleared.
          </p>
        </div>
      </section>
    </div>
  </div>
</template>
