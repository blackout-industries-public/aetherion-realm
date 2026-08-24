<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, fmt } from '../theme'
import UiPanel from './UiPanel.vue'
import UiSpark from './UiSpark.vue'
import UiBars from './UiBars.vue'

const emit = defineEmits<{ select: [string] }>()

const { data: pulse, refresh } = await useFetch<any>('/api/pulse')

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => { refresh() }, 45_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

// One decimal past the gold mark; below a gold the s/c tiers read better than 0.0g.
function gold(copper: number) {
  if (copper >= 10000) return `${(copper / 10000).toFixed(1)}g`
  if (copper >= 100) return `${Math.floor(copper / 100)}s`
  return `${copper}c`
}

const KIND_LABEL: Record<string, string> = {
  vendor_sell: 'vendor sells',
  craft: 'crafts',
  ah_listed: 'auctions listed',
  mail_collect: 'mail runs',
  gather_route: 'gather nodes',
}
const KINDS = ['vendor_sell', 'craft', 'ah_listed', 'mail_collect', 'gather_route']

// Small multiples over stacking: vendor_sell peaks in the hundreds per hour
// while gather_route stays in single digits, so one stacked axis would flatten
// four of the five series into unreadable slivers. Each kind gets its own
// ceiling; the shared 24-slot hour grid keeps time comparable across sparks.
const hourGrid = computed(() => {
  const rows = pulse.value?.hourly ?? []
  if (!rows.length) return []
  const lastHr = Math.floor(Date.now() / 3600_000) * 3600_000
  const hours = Array.from({ length: 24 }, (_, i) => lastHr - (23 - i) * 3600_000)
  const byCell = new Map(rows.map((r: any) => [`${r.ts}:${r.kind}`, r.n]))
  return KINDS.map(kind => ({
    kind,
    label: KIND_LABEL[kind]!,
    total: rows.filter((r: any) => r.kind === kind).reduce((a: number, r: any) => a + r.n, 0),
    points: hours.map(h => ({ label: fmt.clock(h), value: Number(byCell.get(`${h}:${kind}`) ?? 0) })),
  }))
})

const ERRAND_LABEL: Record<string, string> = {
  vendor: 'vendor run', mailbox: 'mail run', trainer: 'trainer visit',
  ah: 'auction house', focus: 'focus craft', gather: 'gather trip',
}

const errandRows = computed(() =>
  (pulse.value?.errands ?? []).map((e: any) => ({
    label: ERRAND_LABEL[e.target] ?? e.target, value: e.n,
  })))

const errandTotal = computed(() =>
  (pulse.value?.errands ?? []).reduce((a: number, e: any) => a + e.n, 0))

// Persona duty is the archetype's errand appetite - shown as the note so the
// census reads "who they are" and "how hard they work the economy" together.
const personaRows = computed(() =>
  (pulse.value?.personas ?? []).map((p: any) => ({
    label: p.name, value: p.n, note: `${p.duty}% duty`,
  })))

const NEED_LABEL: Record<string, string> = {
  repair: 'repairs', training: 'training', mount: 'mounts', gear: 'better gear',
  ammo: 'ammunition', materials: 'materials', errand: 'errands',
}

const lede = computed(() => {
  const p = pulse.value
  if (!p) return 'Waiting for the first pulse.'
  const hourTotal = hourGrid.value.reduce((a, k) => a + k.total, 0)
  const letters = p.mail?.letters ?? 0
  return `${fmt.int(hourTotal)} economic acts in the last 24 hours, ` +
    `${fmt.int(errandTotal.value)} errands underway, ` +
    `${fmt.int(letters)} letters waiting in the post.`
})

const mail = computed(() => pulse.value?.mail ?? null)
const col = computed(() => pulse.value?.collections ?? null)
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

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(320px, 1fr))', gap: '16px', alignItems: 'start' }">
      <UiPanel cap="Activity by hour" note="last 24h, one row per act">
        <p
          v-if="!hourGrid.length"
          :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
        >No economic events in the last 24 hours.</p>
        <div
          v-for="k in hourGrid"
          :key="k.kind"
          :style="{ padding: '6px 0', borderBottom: `1px solid ${T.lineFaint}` }"
        >
          <div :style="{ display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', marginBottom: '3px' }">
            <span :style="{ fontSize: '12.5px', color: T.text }">{{ k.label }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: k.total ? T.goldBright : T.faint, fontVariantNumeric: 'tabular-nums' }">
              {{ fmt.int(k.total) }}
            </span>
          </div>
          <UiSpark :points="k.points" />
        </div>
      </UiPanel>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Errand census" :note="`${fmt.int(errandTotal)} live verdicts`">
          <p
            v-if="!errandRows.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No errand verdicts standing right now.</p>
          <UiBars v-else :rows="errandRows" />
        </UiPanel>

        <UiPanel v-if="personaRows.length" cap="Personas" note="who the population is">
          <UiBars :rows="personaRows" labelWidth="90px" />
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.5 }">
            Disposition is bound to the character: professions plus a stable roll.
            Duty is the share of idle beats spent on economy errands.
          </p>
        </UiPanel>

        <UiPanel cap="Mail health" :note="mail ? `${fmt.int(mail.letters)} letters pending` : ''">
          <p
            v-if="!mail"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >The mail table is unreadable.</p>
          <template v-else>
            <div :style="{ display: 'flex', gap: '16px', flexWrap: 'wrap', fontFamily: FONT.mono, fontSize: '11.5px', marginBottom: '8px' }">
              <span :style="{ color: T.goldBright }" :title="`${fmt.int(mail.withMoney)} letters carrying ${gold(mail.copper)} total`">
                {{ fmt.int(mail.withMoney) }} with money ({{ gold(mail.copper) }})
              </span>
              <span :style="{ color: T.text }">{{ fmt.int(mail.withItems) }} with items</span>
              <span :style="{ color: T.faint }">{{ fmt.int(mail.husks) }} empty husks</span>
            </div>
            <div v-if="col" :style="{ display: 'flex', gap: '16px', flexWrap: 'wrap', fontFamily: FONT.mono, fontSize: '11px', color: T.muted, marginBottom: '8px' }">
              <span :style="{ color: col.letters1h ? T.green : T.faint }">
                {{ fmt.int(col.letters1h) }} collected in 1h ({{ fmt.int(col.runs1h) }} runs)
              </span>
              <span>{{ fmt.int(col.letters24h) }} in 24h ({{ fmt.int(col.runs24h) }} runs)</span>
            </div>
            <div
              v-for="(c, i) in pulse?.collectors ?? []"
              :key="c.guid"
              :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '3px 0', alignItems: 'baseline' }"
            >
              <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
              <button
                :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
                @click="emit('select', c.name)"
              >{{ c.name }}</button>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.textHi, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(c.letters) }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ fmt.int(c.runs) }} runs</span>
            </div>
          </template>
        </UiPanel>

        <UiPanel cap="Needs funding" note="funded = they could afford it today">
          <p
            v-if="!(pulse?.needs ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No needs recorded yet.</p>
          <div
            v-for="n in pulse?.needs ?? []"
            :key="n.type"
            :style="{ display: 'grid', gridTemplateColumns: '104px minmax(40px, 1fr) auto', gap: '10px', alignItems: 'center', padding: '4px 0' }"
          >
            <span :style="{ fontSize: '13px', color: T.body, overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }">
              {{ NEED_LABEL[n.type] ?? n.type }}
            </span>
            <span
              :style="{ height: '7px', background: 'oklch(0.26 0.02 56)', position: 'relative', borderRadius: '1px' }"
              :title="n.priced === 0
                ? `${fmt.int(n.n)} needs, none priced yet`
                : `${fmt.int(n.funded)} of ${fmt.int(n.n)} funded`"
            >
              <span
                :style="{
                  position: 'absolute', inset: '0 auto 0 0',
                  width: `${Math.max(n.funded > 0 ? 2 : 0, (n.funded / Math.max(1, n.n)) * 100)}%`,
                  background: n.funded === n.n ? T.green : T.goldDim,
                  borderRadius: '1px 3px 3px 1px',
                }"
              />
            </span>
            <span :style="{ display: 'flex', alignItems: 'baseline', gap: '6px', justifyContent: 'flex-end' }">
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.text, fontVariantNumeric: 'tabular-nums' }">
                {{ fmt.int(n.n) }}
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: n.priced === 0 ? T.faint : (n.funded < n.priced ? T.red : T.faint), minWidth: '62px', textAlign: 'right' }">
                {{ n.priced === 0 ? 'unpriced' : `${fmt.int(n.funded)} funded` }}
              </span>
            </span>
          </div>
        </UiPanel>
      </div>
    </div>
  </section>
</template>
