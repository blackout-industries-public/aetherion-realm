<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, V, fmt } from '../theme'

// Dot and age tones per freshness status. live/stale are semantic green/red and never
// re-theme; lagging borrows the chrome accent as in the handoff.
const DOT_TONE: Record<string, string> = {
  live: T.green, lagging: V.accent, stale: T.red, unknown: V.dim,
}
const AGE_TONE: Record<string, string> = {
  live: V.faint, lagging: V.accent, stale: T.red, unknown: V.faint,
}

const INGEST_HUE = 'oklch(0.70 0.13 240)'
const FOOT_HUE = 'oklch(0.68 0.11 200)'
const FAULT_RED = 'oklch(0.72 0.13 22)'
const FAULT_BORDER = 'oklch(0.42 0.09 25 / .6)'

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

const freshness = computed(() => props.ops?.freshness ?? [])

// The alert strip points here: the panel wears the fault in its own border.
const faulted = computed(() =>
  !!props.ops?.dbError || freshness.value.some((f: any) => f.status === 'stale'))

const ingest = computed(() => {
  const rows = props.ops?.ingest ?? []
  const peak = Math.max(1, ...rows.map((i: any) => i.events))
  return {
    peak: rows.length ? Math.max(...rows.map((i: any) => i.events)) : 0,
    bars: rows.map((i: any) => ({
      bucket: i.bucket, h: `${Math.round((i.events / peak) * 100)}%`,
    })),
  }
})

const footprint = computed(() => {
  const rows = props.ops?.footprint ?? []
  const max = Math.max(1, ...rows.map((f: any) => f.totalMb))
  return rows.map((f: any) => ({
    name: f.db.replace(/^acore_/, ''),
    mb: f.totalMb, tables: f.tables,
    w: `${Math.round((f.totalMb / max) * 100)}%`,
  }))
})

const census = computed(() => props.ops?.census ?? [])

// A boot every so often is normal; twenty in a day is a finding, so the number is
// stated rather than buried in a sparkline.
const sessionSpan = (h: number) => (h >= 48 ? `${Math.round(h / 24)}d` : `${h}h`)
const churnCells = computed(() => {
  const c = props.ops?.churn
  if (!c) return null
  return [
    { v: String(c.boots24h), l: 'boots in 24h', hot: c.boots24h > 6 },
    { v: String(c.boots7d), l: 'boots in 7d' },
    { v: sessionSpan(c.avgSessionH), l: 'avg session' },
    { v: sessionSpan(c.bestSessionH), l: 'longest session' },
  ]
})

const memPct = computed(() => {
  const h = props.ops?.host
  if (!h?.memTotal) return 0
  return Math.round((h.memUsed / h.memTotal) * 100)
})

// Panel chrome inline per the handoff so every surface re-themes through var(--*).
const panelS = { border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset }
const hdrS = { display: 'flex', justifyContent: 'space-between', alignItems: 'baseline', padding: '10px 12px 6px' }
const capS = { fontFamily: FONT.display, fontWeight: 600, fontSize: '10px', letterSpacing: '.16em', color: V.dim }
const noteS = { fontFamily: FONT.mono, fontSize: '10px', color: V.faint }
const bodyS = { padding: '0 12px 12px' }
</script>

<template>
  <section :style="{ padding: '18px 22px', display: 'flex', flexDirection: 'column', gap: '16px', minHeight: 0, height: '100%', overflow: 'auto' }">
    <p
      :style="{
        margin: 0, borderLeft: `2px solid ${V.accentDim}`, paddingLeft: '13px',
        fontFamily: FONT.body, fontStyle: 'italic', fontWeight: 300, fontSize: '16px',
        lineHeight: 1.4, color: V.textMid,
      }"
    >Every part of the realm is a container, and the whole thing can be rebuilt from a bare
      host by its own scripts.</p>

    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr 1fr', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <section :style="panelS">
          <header :style="hdrS">
            <span :style="capS">CONTAINERS</span>
            <span :style="noteS">{{ ops?.address }}</span>
          </header>
          <div :style="bodyS">
            <div
              v-for="c in ops?.containers ?? []"
              :key="c.name"
              :style="{ display: 'grid', gridTemplateColumns: '10px 1fr auto', gap: '10px', alignItems: 'baseline', padding: '5px 0' }"
            >
              <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: c.up ? T.green : T.red, alignSelf: 'center' }" />
              <span>
                <span :style="{ fontFamily: FONT.mono, fontSize: '12.5px', color: V.text, display: 'block' }">{{ c.name }}</span>
                <span :style="{ fontSize: '11.5px', color: V.faint }">{{ c.note }}</span>
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ c.port }}</span>
            </div>
            <p :style="{ margin: '6px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
              Status is a TCP connect, not the Docker socket — the dashboard is read-only
              by construction.
            </p>
          </div>
        </section>

        <section :style="panelS">
          <header :style="hdrS">
            <span :style="capS">HOST</span>
            <span :style="noteS">{{ ops?.host?.cores ?? '—' }} cores</span>
          </header>
          <div :style="bodyS">
            <div :style="{ display: 'flex', justifyContent: 'space-between', fontFamily: FONT.mono, fontSize: '11.5px', color: V.muted }">
              <span>memory</span>
              <span :style="{ color: V.text }">
                {{ ops?.host ? `${fmt.bytes(ops.host.memUsed)} / ${fmt.bytes(ops.host.memTotal)}` : '—' }}
              </span>
            </div>
            <div :style="{ height: '4px', background: V.lineSoft, marginTop: '6px', position: 'relative' }">
              <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: `${memPct}%`, background: memPct > 85 ? T.red : V.moneyDim }" />
            </div>
            <p :style="{ margin: '9px 0 0', fontSize: '12px', color: V.faint, lineHeight: 1.45 }">
              Survived an unplanned reboot with no human intervention.
            </p>
          </div>
        </section>

        <section v-if="census.length" :style="panelS">
          <header :style="hdrS">
            <span :style="capS">CENSUS</span>
          </header>
          <div :style="bodyS">
            <div
              v-for="c in census"
              :key="c.metric"
              :style="{ display: 'flex', justifyContent: 'space-between', padding: '3px 0', fontSize: '13px' }"
            >
              <span :style="{ color: V.body }">{{ c.metric }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: V.text, fontVariantNumeric: 'tabular-nums' }">
                {{ fmt.int(c.value) }}
              </span>
            </div>
          </div>
        </section>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <section v-if="freshness.length || ops?.dbError" :style="{ ...panelS, border: `1px solid ${faulted ? FAULT_BORDER : V.line}` }">
          <header :style="hdrS">
            <span :style="capS">FEED FRESHNESS</span>
            <span :style="noteS">is anything still writing?</span>
          </header>
          <div :style="bodyS">
            <div
              v-for="f in freshness"
              :key="f.feed"
              :style="{ display: 'grid', gridTemplateColumns: '10px 1fr auto auto', gap: '9px', alignItems: 'baseline', padding: '4px 0' }"
            >
              <span :style="{ width: '7px', height: '7px', borderRadius: '50%', background: DOT_TONE[f.status], alignSelf: 'center' }" />
              <span :style="{ fontSize: '13px', color: V.body }">{{ f.feed }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ fmt.int(f.n) }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: AGE_TONE[f.status], minWidth: '54px', textAlign: 'right' }">
                {{ f.age === null ? '—' : f.age < 90 ? 'live' : `${fmt.ago(Date.now() - f.age * 1000)} ago` }}
              </span>
            </div>
            <p
              v-if="ops?.dbError"
              :style="{ margin: '8px 0 0', fontSize: '12px', color: FAULT_RED, lineHeight: 1.45, fontFamily: FONT.mono }"
            >db: {{ ops.dbError.message }} — {{ fmt.ago(ops.dbError.at) }} ago</p>
            <p :style="{ margin: '8px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
              A container can report healthy while writing nothing — which is how the LLM
              turn log went twenty hours stale unnoticed.
            </p>
          </div>
        </section>

        <section v-if="ingest.bars.length" :style="panelS">
          <header :style="hdrS">
            <span :style="capS">EVENT INGEST</span>
            <span :style="noteS">5-minute buckets, last hour · peak {{ fmt.int(ingest.peak) }}</span>
          </header>
          <div :style="bodyS">
            <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '44px' }">
              <span
                v-for="b in ingest.bars"
                :key="b.bucket"
                :title="b.bucket"
                :style="{ flex: 1, height: b.h, background: INGEST_HUE }"
              />
            </div>
          </div>
        </section>

        <section v-if="churnCells" :style="panelS">
          <header :style="hdrS">
            <span :style="capS">RESTART CHURN</span>
            <span :style="noteS">{{ ops.churn.bootsAll }} boots recorded</span>
          </header>
          <div :style="{ ...bodyS, display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }">
            <div v-for="s in churnCells" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '18px', color: s.hot ? V.accent : V.textHi }">{{ s.v }}</div>
              <div :style="{ fontSize: '11.5px', color: V.muted }">{{ s.l }}</div>
            </div>
          </div>
        </section>

        <section v-if="footprint.length" :style="panelS">
          <header :style="hdrS">
            <span :style="capS">DATABASE FOOTPRINT</span>
            <span :style="noteS">MB on disk, exact</span>
          </header>
          <div :style="bodyS">
            <div
              v-for="f in footprint"
              :key="f.name"
              :style="{ display: 'grid', gridTemplateColumns: '92px 1fr auto auto', gap: '9px', alignItems: 'center', padding: '3px 0' }"
            >
              <span :style="{ fontSize: '13px', color: V.body }">{{ f.name }}</span>
              <span :style="{ height: '7px', background: V.track, position: 'relative' }">
                <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: f.w, background: FOOT_HUE }" />
              </span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(f.mb) }} MB</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">{{ f.tables }} tbl</span>
            </div>
          </div>
        </section>
      </div>

      <section :style="panelS">
        <header :style="hdrS">
          <span :style="capS">ACCEPTANCE</span>
          <span :style="noteS">{{ metCount }} of {{ acceptance.length }}</span>
        </header>
        <div :style="bodyS">
          <div
            v-for="a in acceptance"
            :key="a.id"
            :style="{ display: 'grid', gridTemplateColumns: '30px 1fr auto', gap: '9px', alignItems: 'baseline', padding: '4px 0' }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ a.id }}</span>
            <span :style="{ fontSize: '13.5px', color: V.body }">{{ a.text }}</span>
            <span
              :style="{
                fontFamily: FONT.display, fontWeight: 600, fontSize: '9.5px',
                letterSpacing: '.12em', whiteSpace: 'nowrap',
                color: a.met ? T.green : a.pending ? V.accent : T.red,
              }"
              :title="a.note"
            >{{ a.met ? 'MET' : a.pending ? 'READY' : 'OPEN' }}</span>
          </div>
          <p
            v-if="acceptance.some(a => !a.met && a.note)"
            :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }"
          >
            A12 is wired and verified as far as it can be without a person: every LLM path
            requires a real player in world, so it needs someone to log in and speak.
          </p>
        </div>
      </section>
    </div>
  </section>
</template>
