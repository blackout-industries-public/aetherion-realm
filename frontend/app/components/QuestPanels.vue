<script setup lang="ts">
import { computed } from 'vue'
import { T, FONT, fmt } from '../theme'
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

const questers = computed(() =>
  (props.quests?.questers ?? []).map((q: any) => ({
    label: q.name, value: q.completed, note: `lvl ${q.level}`,
    tone: CLASS_COLOR[q.cls] ?? T.gold,
  })))

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
        <div :style="{ fontFamily: FONT.mono, fontSize: '19px', color: T.textHi }">{{ s.v }}</div>
        <div :style="{ fontSize: '11.5px', color: T.muted }">{{ s.l }}</div>
      </div>
    </div>

    <UiBars :rows="logSplit" label-width="112px" />
  </UiPanel>

  <UiPanel v-if="tempo.length" cap="Quest tempo" note="completions per hour">
    <UiSpark :points="tempo" hue="oklch(0.74 0.13 140)" label="quests handed in" />
  </UiPanel>

  <UiPanel v-else cap="Quest tempo">
    <p :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }">
      The recorder has just started watching quest counts. A completion has to happen
      after a baseline sample before anything can be plotted here.
    </p>
  </UiPanel>

  <UiPanel v-if="bands.length" cap="Who actually quests" note="completed per bot">
    <UiBars :rows="bands" label-width="72px" hue="oklch(0.72 0.13 140)" />
    <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: T.faint, lineHeight: 1.45 }">
      Questing is almost entirely a level-cap activity here. The lower bands hold quests
      but rarely finish them.
    </p>
  </UiPanel>

  <UiPanel v-if="popular.length" cap="Most-carried quests" note="right now">
    <UiBars :rows="popular" label-width="150px" hue="oklch(0.72 0.12 200)" />
  </UiPanel>

  <UiPanel v-if="questers.length" cap="Top questers" note="completed">
    <UiBars :rows="questers" label-width="104px" />
  </UiPanel>
</template>
