<script setup lang="ts">
import { computed, onMounted, onUnmounted } from 'vue'
import { T, FONT, fmt, spell } from '../theme'
import UiPanel from './UiPanel.vue'
import UiBars from './UiBars.vue'
import UiSpark from './UiSpark.vue'

const emit = defineEmits<{ select: [string] }>()

const { data: industry, refresh } = await useFetch<any>('/api/industry')

let timer: ReturnType<typeof setInterval> | undefined
onMounted(() => { timer = setInterval(() => refresh(), 45_000) })
onUnmounted(() => { if (timer) clearInterval(timer) })

const PROF_NAME: Record<number, string> = {
  171: 'Alchemy', 164: 'Blacksmithing', 333: 'Enchanting', 202: 'Engineering',
  165: 'Leatherworking', 197: 'Tailoring', 182: 'Herbalism', 186: 'Mining',
  393: 'Skinning',
}

const lede = computed(() => {
  const d = industry.value
  if (!d || (d.craft.casts === 0 && d.gather.trips === 0)) {
    return 'No craft or gather events in the last day. When bots work their ' +
      'trades, this page shows what the realm produces and who produces it.'
  }
  return `${fmt.int(d.craft.items)} items crafted by ${fmt.int(d.craft.crafters)} ` +
    `bots in the last day, and ${spell(d.gather.trips)} gathering trip` +
    `${d.gather.trips === 1 ? '' : 's'} completed by ${spell(d.gather.gatherers)} ` +
    `gatherer${d.gather.gatherers === 1 ? '' : 's'}.`
})

const productRows = computed(() =>
  (industry.value?.products ?? []).map((p: any) => ({
    label: p.name, value: p.crafted, note: `${fmt.int(p.crafters)} bots`,
  })))

const nodeRows = computed(() =>
  (industry.value?.nodes ?? []).map((n: any) => ({
    label: n.name, value: n.visits, note: `${fmt.int(n.gatherers)} bots`,
  })))

const tripSpark = computed(() =>
  (industry.value?.gatherHourly ?? []).map((h: any) => ({
    label: h.hoursAgo === 0 ? 'now' : `-${h.hoursAgo}h`, value: h.trips,
  })))
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
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Craft output" :note="industry?.craft ? `${fmt.int(industry.craft.items)} items, 24h` : ''">
          <p
            v-if="!productRows.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Nothing crafted in the last 24h.</p>
          <UiBars v-else :rows="productRows" label-width="150px" />
        </UiPanel>

        <UiPanel cap="Top crafters" note="items made, 24h">
          <p
            v-if="!(industry?.crafters ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >Nobody has crafted in the last 24h.</p>
          <div
            v-for="(cr, i) in industry?.crafters ?? []"
            :key="cr.guid"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
              @click="emit('select', cr.name)"
            >{{ cr.name }} <span :style="{ color: T.faint }">lv {{ cr.level }}</span></button>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.goldBright, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(cr.crafted) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ cr.products }} recipe{{ cr.products === 1 ? '' : 's' }}</span>
          </div>
        </UiPanel>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px' }">
        <UiPanel cap="Gathering" :note="industry?.gather ? `${fmt.int(industry.gather.trips)} trips, 24h` : ''">
          <UiSpark :points="tripSpark" label="node visits per hour" />

          <div :style="{ fontFamily: FONT.display, fontWeight: 600, fontSize: '10px', letterSpacing: '.16em', color: T.dim, textTransform: 'uppercase', margin: '12px 0 4px' }">Top gatherers</div>
          <p
            v-if="!(industry?.gatherers ?? []).length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No gathering trips in the last 24h.</p>
          <div
            v-for="(g, i) in industry?.gatherers ?? []"
            :key="g.guid"
            :style="{ display: 'grid', gridTemplateColumns: '20px 1fr auto auto', gap: '9px', padding: '4px 0', alignItems: 'baseline' }"
          >
            <span :style="{ fontFamily: FONT.mono, fontSize: '11px', color: i < 3 ? T.goldBright : T.faint }">{{ i + 1 }}</span>
            <button
              :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '13px', color: T.text, textAlign: 'left', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
              @click="emit('select', g.name)"
            >{{ g.name }} <span :style="{ color: T.faint }">lv {{ g.level }}</span></button>
            <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: T.goldBright, fontVariantNumeric: 'tabular-nums' }">{{ fmt.int(g.trips) }}</span>
            <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint }">{{ g.nodes }} node{{ g.nodes === 1 ? '' : 's' }}</span>
          </div>
        </UiPanel>

        <UiPanel cap="Nodes visited" note="by node type, 24h">
          <p
            v-if="!nodeRows.length"
            :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
          >No nodes visited in the last 24h.</p>
          <UiBars v-else :rows="nodeRows" label-width="150px" />
        </UiPanel>
      </div>

      <UiPanel cap="Profession ladder" note="online bots; skill cap 450">
        <p
          v-if="!(industry?.professions ?? []).length"
          :style="{ margin: 0, fontSize: '13px', color: T.muted, lineHeight: 1.5 }"
        >No profession skills recorded among online bots.</p>
        <div
          v-for="p in industry?.professions ?? []"
          :key="p.skill"
          :style="{ display: 'grid', gridTemplateColumns: '110px auto 1fr auto', gap: '10px', padding: '5px 0', alignItems: 'baseline', borderBottom: `1px solid ${T.lineFaint}` }"
        >
          <span :style="{ fontSize: '13px', color: T.text }">{{ PROF_NAME[p.skill] ?? `Skill ${p.skill}` }}</span>
          <span
            :style="{ fontFamily: FONT.mono, fontSize: '11px', color: T.muted, fontVariantNumeric: 'tabular-nums' }"
            :title="`${fmt.int(p.n)} online bots know ${PROF_NAME[p.skill] ?? 'this trade'}`"
          >{{ fmt.int(p.n) }}</span>
          <button
            :style="{ appearance: 'none', background: 'none', border: 'none', padding: 0, cursor: 'pointer', fontSize: '12px', color: T.textHi, textAlign: 'right', overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap' }"
            :title="`${p.leader} leads at ${p.top}`"
            @click="emit('select', p.leader)"
          >{{ p.leader }}</button>
          <span :style="{ fontFamily: FONT.mono, fontSize: '12px', color: p.top >= 450 ? T.green : T.goldBright, fontVariantNumeric: 'tabular-nums' }">{{ p.top }}</span>
        </div>
      </UiPanel>
    </div>
  </section>
</template>
