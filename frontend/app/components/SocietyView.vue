<script setup lang="ts">
import { computed } from 'vue'
import { FONT, V, fmt, spell, titled } from '../theme'
import { CLASS_COLOR } from '../data'

const props = defineProps<{
  guilds: any | null; combat: any | null; llm: any | null; society: any | null
}>()

const emit = defineEmits<{ select: [string] }>()

const CLASS_NAME: Record<number, string> = {
  1: 'Warrior', 2: 'Paladin', 3: 'Hunter', 4: 'Rogue', 5: 'Priest',
  6: 'Death Knight', 7: 'Shaman', 8: 'Mage', 9: 'Warlock', 11: 'Druid',
}

// Fixed data colors: semantic death/revive, rarity, faction. They never follow the
// chrome palette. 'pro' is the one archetype that rides the accent, per the handoff.
const DEATH = 'oklch(0.66 0.19 25)'
const REVIVE = 'oklch(0.62 0.10 158)'
const RARITY: Record<string, string> = {
  uncommon: 'oklch(0.74 0.10 158)', rare: 'oklch(0.70 0.13 250)', epic: 'oklch(0.68 0.15 305)',
}
const FACTION: Record<string, string> = {
  alliance: 'oklch(0.68 0.13 250)', horde: 'oklch(0.62 0.17 25)',
}
const ARCH_COLOR: Record<string, string> = {
  normal: 'oklch(0.74 0.085 158)',
  pro: V.accent,
  clueless: 'oklch(0.78 0.12 45)',
  afk: 'oklch(0.56 0.028 68)',
  scumbag: 'oklch(0.68 0.10 302)',
  roleplayer: 'oklch(0.82 0.08 350)',
  goldseller: 'oklch(0.62 0.16 22)',
}

// Panel chrome, prototype-exact. Local rather than UiPanel so every chrome color here
// resolves through the palette variables.
const PANEL = { border: `1px solid ${V.line}`, background: V.panel, boxShadow: V.inset }
const HEAD = {
  display: 'flex', justifyContent: 'space-between', alignItems: 'baseline',
  gap: '10px', padding: '10px 12px 6px',
}
const CAP = {
  fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
  letterSpacing: '.16em', color: V.dim, textTransform: 'uppercase', whiteSpace: 'nowrap',
}
const NOTE = { fontFamily: FONT.mono, fontSize: '10px', color: V.faint, whiteSpace: 'nowrap' }
const BODY = { padding: '0 12px 12px' }
const TRACK = { height: '7px', background: V.track, position: 'relative' }
const VAL = { fontFamily: FONT.mono, fontSize: '11.5px', color: V.text, fontVariantNumeric: 'tabular-nums' }

const lede = computed(() => {
  const n = props.guilds?.totalGuilds ?? 0
  const bosses = props.guilds?.bossesBeaten ?? 0
  if (!n) return 'No guilds have formed yet.'
  return `${titled(spell(n))} guild${n === 1 ? '' : 's'} ${n === 1 ? 'has' : 'have'} formed without ` +
    `anyone asking them to. ${bosses ? `${titled(spell(bosses))} boss${bosses === 1 ? '' : 'es'} ${bosses === 1 ? 'has' : 'have'} died to them.` : 'No boss has died to them yet.'}`
})

const hookOn = computed(() => !!props.llm?.hook?.enabled)
const bridgeUp = computed(() => !!props.llm?.bridge?.up)
const hookStats = computed(() => [
  { l: 'p50 latency', v: props.llm?.bridge?.p50 != null ? `${props.llm.bridge.p50.toFixed(2)}s` : '—' },
  { l: 'served', v: fmt.int(props.llm?.bridge?.served ?? 0) },
  { l: 'max in flight', v: props.llm?.hook?.maxInFlight ?? '—' },
  { l: 'reasoning', v: props.llm?.bridge?.reasoning ?? '—' },
])

const chatter = computed(() => (props.llm?.chatter ?? []).slice(0, 8))

const mortality = computed(() => props.society?.mortality ?? [])
const mortPeak = computed(() =>
  Math.max(1, ...mortality.value.map((m: any) => Math.max(m.deaths, m.revives))))

const whoDies = computed(() =>
  (props.society?.whoDies ?? [])
    .filter((w: any) => w.deaths > 0)
    .map((w: any) => ({
      name: CLASS_NAME[w.cls] ?? `Class ${w.cls}`,
      color: CLASS_COLOR[w.cls] ?? V.body,
      deaths: w.deaths,
      note: `${w.perBot.toFixed(2)}/bot`,
    })))
const diesMax = computed(() => Math.max(1, ...whoDies.value.map((w: any) => w.deaths)))
const diesNote = computed(() => {
  const p = props.society?.classPop
  if (!p?.lo) return 'level 55+, last 24h'
  const pop = p.lo === p.hi ? `${p.lo}` : `${p.lo}–${p.hi}`
  return `level 55+, last 24h · ${pop} per class`
})

const lootMix = computed(() => {
  const rows = props.society?.loot ?? []
  const sum = (k: string) => rows.reduce((n: number, r: any) => n + (r[k] || 0), 0)
  return (['uncommon', 'rare', 'epic'] as const)
    .map(k => ({ name: k, value: sum(k), color: RARITY[k]! }))
    .filter(r => r.value > 0)
})
const lootMax = computed(() => Math.max(1, ...lootMix.value.map(r => r.value)))

const balance = computed(() =>
  (props.society?.balance ?? []).map((b: any) => ({
    name: b.race, value: b.chars, note: `lvl ${b.avgLevel}`,
    color: FACTION[b.faction] ?? V.dim,
  })))
const balanceMax = computed(() => Math.max(1, ...balance.value.map((b: any) => b.value)))

const pct = (v: number, max: number) => `${Math.max(v > 0 ? 2 : 0, Math.round((v / max) * 100))}%`
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

    <div :style="{ display: 'grid', gridTemplateColumns: '1.15fr 1fr', gap: '16px', alignItems: 'start' }">
      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px', minWidth: 0 }">
        <!-- The hook and the bridge fail independently. Saying which one is down is the
             difference between a useful panel and a red light. -->
        <section
          :style="{
            border: `1px solid ${hookOn && bridgeUp ? V.line : 'oklch(0.62 0.16 22)'}`,
            background: hookOn && bridgeUp ? V.panel : 'oklch(0.21 0.045 25)',
            boxShadow: V.inset, padding: '11px 13px',
          }"
        >
          <div :style="{ ...CAP, color: hookOn && bridgeUp ? V.dim : 'oklch(0.62 0.16 22)' }">
            In-game LLM hook · {{ hookOn ? 'enabled' : 'disabled' }}
          </div>
          <p :style="{ margin: '7px 0 0', fontSize: '13.5px', color: V.textMid, lineHeight: 1.45 }">
            <template v-if="!bridgeUp">
              The bridge is not answering. Bots fall back to the canned reflex table until it returns.
            </template>
            <template v-else-if="!hookOn">
              The bridge is healthy and the model responds, but the worldserver is not calling it.
              Set <code :style="{ fontFamily: FONT.mono }">LLM_ENABLED=1</code> and restart the worldserver.
            </template>
            <template v-else>
              Live on {{ llm?.bridge?.model || 'the configured model' }}.
              <template v-if="llm?.hook?.requiresWitness">
                Calls only happen with a real player within {{ llm?.hook?.sayRange }} yards — an empty
                realm costs nothing.
              </template>
            </template>
          </p>
          <div :style="{ display: 'grid', gridTemplateColumns: 'repeat(4, 1fr)', gap: '10px', marginTop: '11px' }">
            <div v-for="s in hookStats" :key="s.l">
              <div :style="{ fontFamily: FONT.mono, fontSize: '15px', color: V.textHi }">{{ s.v }}</div>
              <div :style="{ fontSize: '11px', color: V.faint }">{{ s.l }}</div>
            </div>
          </div>
        </section>

        <section :style="PANEL">
          <header :style="HEAD">
            <span :style="CAP">Chatter</span>
            <span :style="NOTE">{{ chatter.length }} recent turns</span>
          </header>
          <div :style="BODY">
            <p
              v-if="!chatter.length"
              :style="{ margin: 0, fontSize: '13.5px', color: V.muted, lineHeight: 1.45 }"
            >Nothing said yet. Bots only reach the model when a real player is present.</p>
            <button
              v-for="c in chatter"
              :key="c.id"
              :style="{
                width: '100%', appearance: 'none', background: 'none', border: 'none',
                padding: '4px 0', cursor: c.bot ? 'pointer' : 'default', textAlign: 'left',
                display: 'grid', gridTemplateColumns: '88px 1fr', gap: '9px', alignItems: 'baseline',
              }"
              @click="c.bot && emit('select', c.who)"
            >
              <span
                :class="{ nm: c.bot }"
                :style="{
                  fontSize: '13.5px', color: c.bot ? (CLASS_COLOR[c.cls] ?? V.textHi) : V.dim,
                  overflow: 'hidden', textOverflow: 'ellipsis', whiteSpace: 'nowrap',
                }"
              >{{ c.who }}</span>
              <span :style="{ fontSize: '13.5px', color: V.textMid, lineHeight: 1.4 }">{{ c.text }}</span>
            </button>
            <p
              v-if="chatter.length"
              :style="{ margin: '8px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }"
            >Dim speakers are humans; coloured names are bots you can click.</p>
          </div>
        </section>

        <section :style="PANEL">
          <header :style="HEAD">
            <span :style="CAP">Cognitive archetypes</span>
            <span :style="NOTE">seeded from guid</span>
          </header>
          <div :style="BODY">
            <div :style="{ display: 'flex', height: '10px', gap: '1px', marginBottom: '9px' }">
              <span
                v-for="a in llm?.archetypes ?? []"
                :key="a.key"
                :style="{ width: `${a.pct}%`, background: ARCH_COLOR[a.key] ?? V.dim }"
                :title="`${a.label} ${a.pct}%`"
              />
            </div>
            <div :style="{ display: 'flex', flexWrap: 'wrap', gap: '8px 16px' }">
              <span v-for="a in llm?.archetypes ?? []" :key="a.key" :style="{ display: 'flex', alignItems: 'center', gap: '6px' }">
                <span :style="{ width: '7px', height: '7px', background: ARCH_COLOR[a.key] ?? V.dim, flex: 'none' }" />
                <span :style="{ fontSize: '13px', color: V.body }">{{ a.label }}</span>
                <span :style="{ fontFamily: FONT.mono, fontSize: '10.5px', color: V.faint }">{{ a.pct }}%</span>
              </span>
            </div>
            <p :style="{ margin: '9px 0 0', fontSize: '11.5px', color: V.faint, lineHeight: 1.45 }">
              Derived from each character's guid, so a bot keeps its temperament across restarts.
            </p>
          </div>
        </section>
      </div>

      <div :style="{ display: 'flex', flexDirection: 'column', gap: '14px', minWidth: 0 }">
        <section v-if="mortality.length" :style="PANEL">
          <header :style="HEAD">
            <span :style="CAP">Mortality</span>
            <span :style="NOTE">deaths vs revives, 24h · peak {{ fmt.int(mortPeak) }}</span>
          </header>
          <div :style="BODY">
            <!-- Revives paint behind deaths at half strength: one shared scale, so a bar
                 taller than its shadow means the hour cost more than it gave back. -->
            <div :style="{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '48px' }">
              <span
                v-for="m in mortality"
                :key="m.hour"
                :title="`${m.hour} — ${m.deaths} deaths, ${m.revives} revives`"
                :style="{ flex: 1, position: 'relative', height: '100%', display: 'flex', alignItems: 'flex-end' }"
              >
                <span :style="{ position: 'absolute', left: 0, right: 0, bottom: 0, height: `${Math.round((m.revives / mortPeak) * 100)}%`, background: REVIVE, opacity: .5 }" />
                <span :style="{ position: 'relative', width: '100%', height: `${Math.round((m.deaths / mortPeak) * 100)}%`, background: DEATH }" />
              </span>
            </div>
            <div :style="{ display: 'flex', alignItems: 'center', gap: '12px', marginTop: '6px' }">
              <span :style="{ display: 'flex', alignItems: 'center', gap: '5px' }">
                <span :style="{ width: '7px', height: '7px', background: DEATH }" />
                <span :style="{ fontSize: '12px', color: V.muted }">deaths</span>
              </span>
              <span :style="{ display: 'flex', alignItems: 'center', gap: '5px' }">
                <span :style="{ width: '7px', height: '7px', background: REVIVE, opacity: .5 }" />
                <span :style="{ fontSize: '12px', color: V.muted }">revives</span>
              </span>
              <span :style="{ marginLeft: 'auto', fontFamily: FONT.mono, fontSize: '10px', color: V.faint }">the only recorded evidence of combat</span>
            </div>
          </div>
        </section>

        <section v-if="whoDies.length" :style="PANEL">
          <header :style="HEAD">
            <span :style="CAP">Who dies</span>
            <span :style="NOTE">{{ diesNote }}</span>
          </header>
          <div :style="BODY">
            <div
              v-for="d in whoDies"
              :key="d.name"
              :style="{ display: 'grid', gridTemplateColumns: '96px 1fr auto auto', gap: '9px', alignItems: 'center', padding: '3px 0' }"
            >
              <span :style="{ fontSize: '13px', color: d.color }">{{ d.name }}</span>
              <span :style="TRACK">
                <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: pct(d.deaths, diesMax), background: d.color }" />
              </span>
              <span :style="VAL">{{ fmt.int(d.deaths) }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, minWidth: '56px', textAlign: 'right' }">{{ d.note }}</span>
            </div>
          </div>
        </section>

        <section v-if="lootMix.length" :style="PANEL">
          <header :style="HEAD">
            <span :style="CAP">Loot mix</span>
            <span :style="NOTE">6h, uncommon and above</span>
          </header>
          <div :style="BODY">
            <div
              v-for="l in lootMix"
              :key="l.name"
              :style="{ display: 'grid', gridTemplateColumns: '80px 1fr auto', gap: '9px', alignItems: 'center', padding: '3px 0' }"
            >
              <span :style="{ fontSize: '13px', color: l.color }">{{ l.name }}</span>
              <span :style="TRACK">
                <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: pct(l.value, lootMax), background: l.color }" />
              </span>
              <span :style="VAL">{{ fmt.int(l.value) }}</span>
            </div>
          </div>
        </section>

        <section v-if="balance.length" :style="PANEL">
          <header :style="HEAD">
            <span :style="CAP">Faction and race</span>
            <span :style="NOTE">online</span>
          </header>
          <div :style="BODY">
            <div
              v-for="b in balance"
              :key="b.name"
              :style="{ display: 'grid', gridTemplateColumns: '80px 1fr auto auto', gap: '9px', alignItems: 'center', padding: '3px 0' }"
            >
              <span :style="{ fontSize: '13px', color: V.body }">{{ b.name }}</span>
              <span :style="TRACK">
                <span :style="{ position: 'absolute', inset: '0 auto 0 0', width: pct(b.value, balanceMax), background: b.color }" />
              </span>
              <span :style="VAL">{{ fmt.int(b.value) }}</span>
              <span :style="{ fontFamily: FONT.mono, fontSize: '10px', color: V.faint, minWidth: '42px', textAlign: 'right' }">{{ b.note }}</span>
            </div>
          </div>
        </section>
      </div>
    </div>
  </section>
</template>

<style scoped>
.nm:hover { text-decoration: underline; }
</style>
