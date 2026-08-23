<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, fmt } from '../theme'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'
import UiSpark from './UiSpark.vue'

const FRESH_TONE: Record<string, string> = {
  live: T.green, lagging: T.gold, stale: T.red, unknown: T.dim,
}

const props = defineProps<{ ops: any | null; assembler: any | null; llm: any | null }>()

// Verification probes issued by hand against the bridge. Anything above this is real
// in-game traffic, which is what A12 actually asks for.
const PROBE_BASELINE = 3

// Acceptance criteria from the BRD. Statuses that can be checked live are checked;
// the rest are stated as the document records them rather than invented here.
const acceptance = computed(() => {
  const o = props.ops
  const a = props.assembler
  const up = (n: string) => !!o?.containers?.find((c: any) => c.name === n)?.up
  return [
    { id: 'A1', text: 'Builds from a bare host', met: true },
    { id: 'A2', text: 'Survives reboot unattended', met: true },
    { id: 'A3', text: `${fmt.int(o?.botPopulation ?? 0)} bots across brackets`, met: up('ac-worldserver') },
    { id: 'A4', text: 'Backups prune and restore', met: true },
    { id: 'A5', text: 'Parties larger than two', met: (a?.cycle?.formed ?? 0) > 0 },
    { id: 'A6', text: 'Parties enter dungeons', met: (a?.cycle?.entered ?? 0) > 0 },
    { id: 'A7', text: 'Raids form and enter', met: (a?.cycle?.raids ?? 0) > 0 },
    { id: 'A8', text: 'Bots fight in the open world', met: !!o?.pvpEnabled },
    { id: 'A9', text: 'Live world on real map art', met: true },
    { id: 'A10', text: 'In-character, no leakage', met: true },
    { id: 'A11', text: 'Zero spend with no human', met: true },
    // Enabling the hook is not the same as observing a conversation. Every LLM path
    // needs a real player in world, so this stays short of "met" until one appears.
    {
      id: 'A12', text: 'Bots converse via the LLM',
      met: !!o?.llmHookEnabled && (props.llm?.bridge?.served ?? 0) > PROBE_BASELINE,
      pending: !!o?.llmHookEnabled,
      note: 'needs a player in world',
    },
  ]
})

const metCount = computed(() => acceptance.value.filter(a => a.met).length)

const ingest = computed(() =>
  (props.ops?.ingest ?? []).map((i: any) => ({ label: i.bucket, value: i.events })))

const footprint = computed(() =>
  (props.ops?.footprint ?? []).map((f: any) => ({
    label: f.db.replace(/^acore_/, ''), value: f.totalMb, note: `${f.tables} tbl`,
  })))

const census = computed(() => props.ops?.census ?? [])

// A boot every so often is normal; twenty in a day is a finding, so the number is
// stated rather than buried in a sparkline.
const churn = computed(() => props.ops?.churn ?? null)

const memPct = computed(() => {
  const h = props.ops?.host
  if (!h?.memTotal) return 0
  return Math.round((h.memUsed / h.memTotal) * 100)
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
    >Every part of the realm is a container, and the whole thing can be rebuilt from a bare
      host by its own scripts.</p>

    <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(auto-fit, minmax(276px, 1fr))', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Containers" :note="ops?.address">
          <div
            v-for="c in ops?.containers ?? []"
            :key="c.name"
            :style="{ display: 'grid', gridTemplateColumns: '8px 1fr auto', gap: '10px', alignItems: 'baseline', padding: '6px 0' }"
          >
            <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: c.up ? T.green : T.red, marginTop: '5px' }" />
            <span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '13px', color: T.text, display: 'block' }">{{ c.name }}</span>
              <span :style="{ fontSize: '12px', color: T.faint }">{{ c.note }}</span>
            </span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ c.port }}</span>
          </div>
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            Status is a TCP connect, not the Docker socket — the dashboard is read-only by
            construction and never gets that privilege.
          </p>
        </UiPanel>

        <UiPanel cap="Host" :note="`${ops?.host?.cores ?? '—'} cores`">
          <div :style="{ display: 'flex', justifyContent: 'space-between', fontFamily: FONT.mono, fontSize: '11.5px', color: T.muted }">
            <span>memory</span>
            <span :style="{ color: T.text }">
              {{ ops?.host ? `${fmt.bytes(ops.host.memUsed)} / ${fmt.bytes(ops.host.memTotal)}` : '—' }}
            </span>
          </div>
          <div :style="{ height: '4px', background: T.lineSoft, marginTop: '6px', position: 'relative' }">
            <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${memPct}%`, background: memPct > 85 ? T.red : T.goldDim }" />
          </div>
          <p :style="{ margin: '9px 0 0', fontSize: '12px', color: T.faint, lineHeight: 1.45 }">
            Survived an unplanned reboot with no human intervention.
          </p>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel
          v-if="ops?.freshness?.length"
          cap="Feed freshness"
          note="is anything still writing?"
        >
          <div
            v-for="f in ops.freshness"
            :key="f.feed"
            :style="{ display: 'grid', gridTemplateColumns: '10px 1fr auto auto', gap: '9px', alignItems: 'baseline', padding: '4px 0' }"
          >
            <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: FRESH_TONE[f.status], marginTop: '4px' }" />
            <span :style="{ fontSize: '13px', color: T.body }">{{ f.feed }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ fmt.int(f.n) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: FRESH_TONE[f.status], minWidth: '54px', textAlign: 'right' }">
              {{ f.age === null ? '—' : f.age < 90 ? 'live' : fmt.ago(Date.now() - f.age * 1000) }}
            </span>
          </div>
          <p
            v-if="ops?.dbError"
            :style="{ margin: '8px 0 0', fontSize: '12px', color: T.red, lineHeight: 1.45, fontFamily: FONT.mono }"
          >db: {{ ops.dbError.message }}</p>
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            A container can report healthy while writing nothing — which is how the LLM
            turn log went twenty hours stale unnoticed.
          </p>
        </UiPanel>

        <UiPanel v-if="ingest.length" cap="Event ingest" note="5-minute buckets, last hour">
          <UiSpark :points="ingest" hue="oklch(0.70 0.13 240)" label="events recorded" />
        </UiPanel>

        <UiPanel v-if="churn" cap="Restart churn" :note="`${churn.bootsAll} boots recorded`">
          <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }">
            <div v-for="s in [
              { v: churn.boots24h, l: 'boots in 24h' },
              { v: churn.boots7d, l: 'boots in 7d' },
              { v: `${churn.avgSessionH}h`, l: 'avg session' },
              { v: `${churn.bestSessionH}h`, l: 'longest session' },
            ]" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '18px', color: churn.boots24h > 6 && s.l === 'boots in 24h' ? T.gold : T.textHi }">{{ s.v }}</div>
              <div :style="{ fontSize: '11.5px', color: T.muted }">{{ s.l }}</div>
            </div>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel v-if="footprint.length" cap="Database footprint" note="MB on disk">
          <UiBars :rows="footprint" unit=" MB" hue="oklch(0.68 0.11 200)" label-width="92px" />
          <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }">
            Sizes are exact. Row estimates from the same view were measured 5x understated,
            so they are deliberately not shown.
          </p>
        </UiPanel>

        <UiPanel v-if="census.length" cap="Census">
          <div
            v-for="c in census"
            :key="c.metric"
            :style="{ display: 'flex', justifyContent: 'space-between', padding: '3px 0', fontSize: '13px' }"
          >
            <span :style="{ color: T.body }">{{ c.metric }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.text, fontVariantNumeric: 'tabular-nums' }">
              {{ fmt.int(c.value) }}
            </span>
          </div>
        </UiPanel>
      </div>

      <UiPanel cap="Upstream · pinned">
        <div v-for="p in ops?.pins ?? []" :key="p.name" :style="{ padding: '6px 0' }">
          <div :style="{ fontSize: '13.5px', color: T.text }">{{ p.name }}</div>
          <div :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint, wordBreak: 'break-all' }">{{ p.commit }}</div>
        </div>
        <p :style="{ margin: '8px 0 0', fontSize: '12px', color: T.muted, lineHeight: 1.45 }">
          Tracking a moving branch is prohibited. A force-push upstream would otherwise
          change the realm silently.
        </p>
      </UiPanel>

      <UiPanel cap="Acceptance" :note="`${metCount} of ${acceptance.length}`">
        <div
          v-for="a in acceptance"
          :key="a.id"
          :style="{ display: 'grid', gridTemplateColumns: '30px 1fr auto', gap: '9px', alignItems: 'baseline', padding: '4px 0' }"
        >
          <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }">{{ a.id }}</span>
          <span :style="{ fontSize: '13.5px', color: T.body }">{{ a.text }}</span>
          <span
            :style="{
              fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px',
              letterSpacing: '.12em', whiteSpace: 'nowrap',
              color: a.met ? T.green : a.pending ? T.gold : T.red,
            }"
            :title="a.note"
          >{{ a.met ? 'MET' : a.pending ? 'READY' : 'OPEN' }}</span>
        </div>
        <p
          v-if="acceptance.some(a => !a.met && a.note)"
          :style="{ margin: '9px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.4 }"
        >
          A12 is wired and verified as far as it can be without a person: every LLM path
          requires a real player in world, so it needs someone to log in and speak.
        </p>
      </UiPanel>
    </div>
  </section>
</template>
