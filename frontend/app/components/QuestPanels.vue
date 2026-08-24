<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, V, fmt } from '../theme'
import { CLASS_COLOR, zoneName } from '../data'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'
import UiSpark from './UiSpark.vue'

const props = defineProps<{ quests: any | null }>()
const emit = defineEmits<{ select: [string] }>()

const head = computed(() => props.quests?.headline ?? {})

const popular = computed(() =>
  (props.quests?.popular ?? []).map((p: any) => ({
    label: p.quest,
    value: p.carrying,
    note: p.zone ? zoneName(p.zone).slice(0, 13) : `lvl ${p.level}`,
  })))

// Completion per bot, not the total: the totals just restate the population size, while
// the average shows that only the level cap actually quests.
const bands = computed(() =>
  (props.quests?.bands ?? []).map((b: any) => ({
    label: b.band,
    value: b.avgEach,
    note: `${fmt.int(b.bots)} bots`,
  })))

const questers = computed(() => {
  const rows = (props.quests?.questers ?? []).map((q: any) => ({
    name: q.name, value: q.completed, level: q.level,
    tone: CLASS_COLOR[q.cls] ?? V.text,
  }))
  const max = Math.max(1, ...rows.map((r: any) => r.value))
  return rows.map((r: any) => ({ ...r, w: `${Math.max(2, (r.value / max) * 100)}%` }))
})

const tempo = computed(() =>
  (props.quests?.tempo ?? []).map((t: any) => ({ label: t.hour, value: t.completed })))

// The log breaks into three states worth distinguishing: in progress, sitting complete
// and unturned, and abandoned as failed.
const logSplit = computed(() => {
  const h = head.value
  return [
    { label: 'in progress', value: h.inProgress ?? 0, tone: 'oklch(0.70 0.12 240)' },
    { label: 'ready to hand in', value: h.readyToHandIn ?? 0, tone: T.green },
    { label: 'failed', value: h.failed ?? 0, tone: T.red },
  ].filter(r => r.value > 0)
})
</script>

<template>
  <UiPanel cap="Quest log" :note="`${fmt.int(head.questing ?? 0)} questing`">
    <div :style="{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px', marginBottom: '12px' }">
      <div v-for="s in [
        { v: fmt.int(head.active ?? 0), l: `active · ${head.perBot ?? 0} each` },
        { v: fmt.int(head.completedQuests ?? 0), l: 'completed all-time' },
      ]" :key="s.l">
        <div :style="{ fontFamily: FONT.mono, fontSize: '19px', color: V.textHi }">{{ s.v }}</div>
        <div :style="{ fontSize: '11.5px', color: V.muted }">{{ s.l }}</div>
      </div>
    </div>

    <UiBars :rows="logSplit" label-width="112px" />
  </UiPanel>

  <UiPanel v-if="tempo.length" cap="Quest tempo" note="completions per hour">
    <UiSpark :points="tempo" hue="oklch(0.74 0.13 140)" label="quests handed in" />
  </UiPanel>

  <UiPanel v-else cap="Quest tempo">
    <p :style="{ margin: 0, fontSize: '13px', color: V.muted, lineHeight: 1.5 }">
      The recorder has just started watching quest counts. A completion has to happen
      after a baseline sample before anything can be plotted here.
    </p>
  </UiPanel>

  <UiPanel v-if="bands.length" cap="Who actually quests" note="completed per bot">
    <UiBars :rows="bands" label-width="72px" hue="oklch(0.72 0.13 140)" />
    <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
      Questing is almost entirely a level-cap activity here. The lower bands hold quests
      but rarely finish them.
    </p>
  </UiPanel>

  <UiPanel v-if="popular.length" cap="Most-carried quests" note="right now">
    <UiBars :rows="popular" label-width="150px" hue="oklch(0.72 0.12 200)" />
  </UiPanel>

  <UiPanel v-if="questers.length" cap="Top questers" note="completed">
    <div
      v-for="r in questers"
      :key="r.name"
      :style="{ display: 'grid', gridTemplateColumns: '104px minmax(40px, 1fr) auto', gap: '10px', alignItems: 'center', padding: '3px 0' }"
    >
      <button
        class="quester-name"
        :style="{
          appearance: 'none', background: 'none', border: 'none', padding: 0,
          cursor: 'pointer', textAlign: 'left', fontFamily: FONT.body,
          fontSize: '13px', color: r.tone, overflow: 'hidden',
          textOverflow: 'ellipsis', whiteSpace: 'nowrap',
        }"
        :title="r.name"
        @click="emit('select', r.name)"
      >{{ r.name }}</button>
      <span :style="{ height: '7px', background: V.track, position: 'relative', borderRadius: '1px' }">
        <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: r.w, background: r.tone, borderRadius: '1px 3px 3px 1px' }" />
      </span>
      <span :style="{ display: 'flex', alignItems: 'baseline', gap: '6px', justifyContent: 'flex-end' }">
        <span :style="{ fontFamily: FONT.mono, fontSize: '11.5px', color: V.text, fontVariantNumeric: 'tabular-nums' }">
          {{ fmt.int(r.value) }}
        </span>
        <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, minWidth: '38px', textAlign: 'right' }">
          lvl {{ r.level }}
        </span>
      </span>
    </div>
  </UiPanel>
</template>

<style scoped>
.quester-name:hover { text-decoration: underline; }
</style>
