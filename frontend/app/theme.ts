// Design tokens for the Observatory. Values are the design's own, kept in one place so
// a panel can never drift into a slightly different brown.
export const T = {
  bg: 'oklch(0.145 0.016 52)',
  panel: 'oklch(0.19 0.019 52)',
  panelAlt: 'oklch(0.235 0.018 52)',
  raised: 'oklch(0.215 0.024 54)',

  line: 'oklch(0.31 0.026 60)',
  lineSoft: 'oklch(0.27 0.02 54)',
  lineFaint: 'oklch(0.235 0.018 52)',
  inset: 'inset 0 1px 0 oklch(0.5 0.05 78 / .18)',

  textHi: 'oklch(0.95 0.01 74)',
  text: 'oklch(0.9 0.014 74)',
  textMid: 'oklch(0.83 0.022 74)',
  body: 'oklch(0.885 0.018 74)',
  muted: 'oklch(0.745 0.026 72)',
  dim: 'oklch(0.68 0.035 74)',
  faint: 'oklch(0.645 0.032 70)',

  gold: 'oklch(0.80 0.10 88)',
  goldDim: 'oklch(0.62 0.09 88)',
  goldBright: 'oklch(0.86 0.07 88)',
  red: 'oklch(0.62 0.16 22)',
  green: 'oklch(0.74 0.085 158)',
} as const

export const FONT = {
  display: 'Cinzel, Georgia, serif',
  body: 'Spectral, Georgia, serif',
  mono: "'Cutive Mono', ui-monospace, monospace",
} as const

// Chrome palettes. T above stays the Argent Gold source of truth; these are the
// same surface, line, text and accent roles re-tuned per palette. Only chrome
// varies - STATE, CLASS_COLOR, faction, rarity and semantic green/red never
// change with the palette. In 'moonlit', gold is reserved for money alone.
export type PaletteName = 'gold' | 'violet' | 'verdigris' | 'moonlit'

export interface Palette {
  bg: string; bgAlt: string; panel: string; panelDeep: string; panelOpen: string
  raised: string; raisedHi: string; overlay: string
  line: string; lineSoft: string; lineFaint: string; lineAccent: string; lineAccentSoft: string
  track: string; inset: string
  textHi: string; text: string; textMid: string; body: string
  muted: string; dim: string; faint: string; faint2: string
  accent: string; accentBright: string; accentDim: string; accentBar: string
  money: string; moneyBright: string; moneyDim: string
  cornerTick: string; mapHi: string; mapLo: string; pipBorder: string
}

export const PALETTES: Record<PaletteName, Palette> = {
  gold: {
    bg: 'oklch(0.145 0.016 52)', bgAlt: 'oklch(0.165 0.018 50)',
    panel: 'oklch(0.19 0.019 52)', panelDeep: 'oklch(0.16 0.017 50)', panelOpen: 'oklch(0.205 0.02 54)',
    raised: 'oklch(0.215 0.024 54)', raisedHi: 'oklch(0.225 0.022 54)', overlay: 'oklch(0.16 0.017 50 / .94)',
    line: 'oklch(0.31 0.026 60)', lineSoft: 'oklch(0.27 0.02 54)', lineFaint: 'oklch(0.235 0.018 52)',
    lineAccent: 'oklch(0.45 0.06 82 / .45)', lineAccentSoft: 'oklch(0.42 0.05 78 / .55)',
    track: 'oklch(0.26 0.02 56)', inset: 'inset 0 1px 0 oklch(0.5 0.05 78 / .18)',
    textHi: 'oklch(0.95 0.01 74)', text: 'oklch(0.9 0.014 74)', textMid: 'oklch(0.83 0.022 74)', body: 'oklch(0.885 0.018 74)',
    muted: 'oklch(0.745 0.026 72)', dim: 'oklch(0.68 0.035 74)', faint: 'oklch(0.645 0.032 70)', faint2: 'oklch(0.55 0.03 68)',
    accent: 'oklch(0.80 0.10 88)', accentBright: 'oklch(0.86 0.07 88)', accentDim: 'oklch(0.62 0.09 88)', accentBar: 'oklch(0.72 0.13 88)',
    money: 'oklch(0.80 0.10 88)', moneyBright: 'oklch(0.86 0.07 88)', moneyDim: 'oklch(0.62 0.09 88)',
    cornerTick: 'oklch(0.66 0.09 86 / .6)', mapHi: 'oklch(0.20 0.022 58)', mapLo: 'oklch(0.155 0.017 52)', pipBorder: 'oklch(0.42 0.03 62)',
  },
  violet: {
    bg: 'oklch(0.148 0.014 300)', bgAlt: 'oklch(0.168 0.015 300)',
    panel: 'oklch(0.19 0.017 300)', panelDeep: 'oklch(0.163 0.015 300)', panelOpen: 'oklch(0.205 0.017 300)',
    raised: 'oklch(0.215 0.021 300)', raisedHi: 'oklch(0.225 0.019 300)', overlay: 'oklch(0.163 0.015 300 / .94)',
    line: 'oklch(0.31 0.022 300)', lineSoft: 'oklch(0.27 0.017 300)', lineFaint: 'oklch(0.235 0.015 300)',
    lineAccent: 'oklch(0.45 0.07 300 / .45)', lineAccentSoft: 'oklch(0.42 0.06 300 / .55)',
    track: 'oklch(0.245 0.016 300)', inset: 'inset 0 1px 0 oklch(0.55 0.06 300 / .2)',
    textHi: 'oklch(0.95 0.008 300)', text: 'oklch(0.9 0.011 300)', textMid: 'oklch(0.83 0.015 300)', body: 'oklch(0.885 0.013 300)',
    muted: 'oklch(0.745 0.02 300)', dim: 'oklch(0.68 0.028 300)', faint: 'oklch(0.645 0.026 300)', faint2: 'oklch(0.55 0.024 300)',
    accent: 'oklch(0.74 0.13 300)', accentBright: 'oklch(0.82 0.10 300)', accentDim: 'oklch(0.58 0.11 300)', accentBar: 'oklch(0.70 0.12 300)',
    money: 'oklch(0.74 0.13 300)', moneyBright: 'oklch(0.82 0.10 300)', moneyDim: 'oklch(0.58 0.11 300)',
    cornerTick: 'oklch(0.68 0.09 300 / .6)', mapHi: 'oklch(0.195 0.019 305)', mapLo: 'oklch(0.152 0.014 300)', pipBorder: 'oklch(0.40 0.026 300)',
  },
  verdigris: {
    bg: 'oklch(0.148 0.014 215)', bgAlt: 'oklch(0.168 0.015 212)',
    panel: 'oklch(0.19 0.016 212)', panelDeep: 'oklch(0.163 0.015 212)', panelOpen: 'oklch(0.205 0.016 210)',
    raised: 'oklch(0.215 0.02 210)', raisedHi: 'oklch(0.225 0.018 210)', overlay: 'oklch(0.163 0.015 212 / .94)',
    line: 'oklch(0.31 0.022 210)', lineSoft: 'oklch(0.27 0.016 210)', lineFaint: 'oklch(0.235 0.014 212)',
    lineAccent: 'oklch(0.45 0.06 195 / .45)', lineAccentSoft: 'oklch(0.42 0.05 200 / .55)',
    track: 'oklch(0.245 0.015 210)', inset: 'inset 0 1px 0 oklch(0.52 0.05 195 / .2)',
    textHi: 'oklch(0.95 0.008 200)', text: 'oklch(0.9 0.01 202)', textMid: 'oklch(0.83 0.013 203)', body: 'oklch(0.885 0.012 204)',
    muted: 'oklch(0.745 0.018 205)', dim: 'oklch(0.68 0.025 205)', faint: 'oklch(0.645 0.024 205)', faint2: 'oklch(0.55 0.022 205)',
    accent: 'oklch(0.75 0.11 190)', accentBright: 'oklch(0.82 0.09 190)', accentDim: 'oklch(0.58 0.09 195)', accentBar: 'oklch(0.70 0.10 192)',
    money: 'oklch(0.75 0.11 190)', moneyBright: 'oklch(0.82 0.09 190)', moneyDim: 'oklch(0.58 0.09 195)',
    cornerTick: 'oklch(0.66 0.08 192 / .6)', mapHi: 'oklch(0.195 0.018 215)', mapLo: 'oklch(0.152 0.013 212)', pipBorder: 'oklch(0.40 0.024 208)',
  },
  moonlit: {
    bg: 'oklch(0.148 0.010 260)', bgAlt: 'oklch(0.168 0.011 260)',
    panel: 'oklch(0.19 0.012 260)', panelDeep: 'oklch(0.163 0.011 260)', panelOpen: 'oklch(0.205 0.012 260)',
    raised: 'oklch(0.215 0.014 260)', raisedHi: 'oklch(0.225 0.013 260)', overlay: 'oklch(0.163 0.011 260 / .94)',
    line: 'oklch(0.31 0.015 260)', lineSoft: 'oklch(0.27 0.012 260)', lineFaint: 'oklch(0.235 0.011 260)',
    lineAccent: 'oklch(0.5 0.03 260 / .45)', lineAccentSoft: 'oklch(0.46 0.025 260 / .55)',
    track: 'oklch(0.245 0.011 260)', inset: 'inset 0 1px 0 oklch(0.6 0.02 260 / .18)',
    textHi: 'oklch(0.95 0.006 260)', text: 'oklch(0.9 0.008 260)', textMid: 'oklch(0.84 0.01 260)', body: 'oklch(0.885 0.009 260)',
    muted: 'oklch(0.745 0.014 260)', dim: 'oklch(0.68 0.018 260)', faint: 'oklch(0.645 0.017 260)', faint2: 'oklch(0.55 0.015 260)',
    accent: 'oklch(0.86 0.02 260)', accentBright: 'oklch(0.92 0.015 260)', accentDim: 'oklch(0.62 0.02 260)', accentBar: 'oklch(0.72 0.03 260)',
    // Gold becomes meaning, not decoration: only money wears it.
    money: 'oklch(0.80 0.10 88)', moneyBright: 'oklch(0.86 0.07 88)', moneyDim: 'oklch(0.62 0.09 88)',
    cornerTick: 'oklch(0.72 0.02 260 / .6)', mapHi: 'oklch(0.195 0.014 260)', mapLo: 'oklch(0.152 0.011 260)', pipBorder: 'oklch(0.42 0.015 260)',
  },
}

// One custom property per palette role, so chrome can restyle by swapping the
// data-aeth attribute on the shell root instead of re-rendering anything.
const VAR_NAME: Record<keyof Palette, string> = {
  bg: '--bg', bgAlt: '--bgA', panel: '--pnl', panelDeep: '--pnlD', panelOpen: '--pnlO',
  raised: '--rsd', raisedHi: '--rsdH', overlay: '--ovl',
  line: '--ln', lineSoft: '--lnS', lineFaint: '--lnF', lineAccent: '--lnA', lineAccentSoft: '--lnAS',
  track: '--trk', inset: '--ins',
  textHi: '--tHi', text: '--t', textMid: '--tMid', body: '--bdy',
  muted: '--mut', dim: '--dim', faint: '--fnt', faint2: '--fnt2',
  accent: '--acc', accentBright: '--accB', accentDim: '--accD', accentBar: '--accBar',
  money: '--mny', moneyBright: '--mnyB', moneyDim: '--mnyD',
  cornerTick: '--tick', mapHi: '--mapHi', mapLo: '--mapLo', pipBorder: '--pip',
}

const varBlock = (p: Palette) =>
  (Object.keys(p) as (keyof Palette)[]).map(k => `${VAR_NAME[k]}:${p[k]}`).join(';')

export const PALETTE_CSS =
  `:root{${varBlock(PALETTES.gold)}}\n` +
  (['violet', 'verdigris', 'moonlit'] as PaletteName[])
    .map(n => `[data-aeth="${n}"]{${varBlock(PALETTES[n])}}`)
    .join('\n')

// Chrome references for inline styles. Same roles as Palette, resolved live by
// the browser against whichever data-aeth block is active.
export const V = {
  bg: 'var(--bg)', bgAlt: 'var(--bgA)', panel: 'var(--pnl)', panelDeep: 'var(--pnlD)',
  panelOpen: 'var(--pnlO)', raised: 'var(--rsd)', raisedHi: 'var(--rsdH)', overlay: 'var(--ovl)',
  line: 'var(--ln)', lineSoft: 'var(--lnS)', lineFaint: 'var(--lnF)',
  lineAccent: 'var(--lnA)', lineAccentSoft: 'var(--lnAS)',
  track: 'var(--trk)', inset: 'var(--ins)',
  textHi: 'var(--tHi)', text: 'var(--t)', textMid: 'var(--tMid)', body: 'var(--bdy)',
  muted: 'var(--mut)', dim: 'var(--dim)', faint: 'var(--fnt)', faint2: 'var(--fnt2)',
  accent: 'var(--acc)', accentBright: 'var(--accB)', accentDim: 'var(--accD)', accentBar: 'var(--accBar)',
  money: 'var(--mny)', moneyBright: 'var(--mnyB)', moneyDim: 'var(--mnyD)',
  tick: 'var(--tick)', mapHi: 'var(--mapHi)', mapLo: 'var(--mapLo)', pip: 'var(--pip)',
} as const

export const THEME_CHOICES: { key: PaletteName; name: string }[] = [
  { key: 'gold', name: 'Argent Gold' },
  { key: 'violet', name: 'Arcane Violet' },
  { key: 'verdigris', name: 'Verdigris' },
  { key: 'moonlit', name: 'Moonlit Silver' },
]

// What a character is doing, in the map's own vocabulary. "in combat" is deliberately
// absent: this schema has no combat log, so a corpse run - which is recorded - is shown
// instead of a number that could only be guessed at.
// Chosen for separation against a dark plate and against each other. Idle stays the
// dullest - it is the majority state, and a bright colour there just covers the states
// worth looking at - but not so dull that it disappears: measured against the terrain
// actually under each dot, the previous value left 38% of idle dots below 3:1.
// Expanded from six coarse buckets to the RPG engine's real vocabulary. Pairwise OKLab
// separation measured before shipping: worst circle pair 13.4 (idle vs town, and idle
// is recessive by design). 'working' competes on SHAPE - it renders as a diamond - so
// it stays out of the hue budget entirely.
export const STATE = {
  idle: { label: 'idle', color: 'oklch(0.72 0.02 250)' },
  working: { label: 'working a trade', color: 'oklch(0.80 0.12 60)' },
  town: { label: 'in town', color: 'oklch(0.64 0.14 255)' },
  travelling: { label: 'travelling', color: 'oklch(0.85 0.16 90)' },
  questing: { label: 'questing', color: 'oklch(0.85 0.18 140)' },
  grinding: { label: 'fighting', color: 'oklch(0.74 0.17 45)' },
  grouped: { label: 'with a party', color: 'oklch(0.97 0.005 74)' },
  instance: { label: 'in an instance', color: 'oklch(0.75 0.13 175)' },
  ghost: { label: 'on a corpse run', color: 'oklch(0.76 0.16 330)' },
  dead: { label: 'dead', color: 'oklch(0.62 0.23 20)' },
} as const

export type StateKey = keyof typeof STATE

// Panel chrome, reused everywhere so the shell of every box matches.
export const panelStyle = `border:1px solid ${T.line};background:${T.panel};box-shadow:${T.inset}`
export const capStyle =
  `font-family:${FONT.display};font-weight:600;font-size:10px;letter-spacing:.16em;` +
  `color:${T.dim};text-transform:uppercase;white-space:nowrap`
export const monoStyle = `font-family:${FONT.mono}`

export const fmt = {
  int: (n: number) => n.toLocaleString('en-GB'),
  yards: (n: number) => `${n.toLocaleString('en-GB')} yd`,
  bytes: (n: number) => `${(n / 1024 ** 3).toFixed(1)} GB`,
  ago(ms: number) {
    const s = Math.max(0, Math.round((Date.now() - ms) / 1000))
    if (s < 60) return `${s}s`
    if (s < 3600) return `${Math.round(s / 60)}m`
    if (s < 86400) return `${Math.round(s / 3600)}h`
    return `${Math.round(s / 86400)}d`
  },
  clock: (ms: number) =>
    new Date(ms).toLocaleTimeString('en-GB', { hour: '2-digit', minute: '2-digit' }),
}

// The lede on each screen is written from live numbers, in words, because a sentence
// that states the realm's condition reads better than a row of counters repeating it.
const WORDS = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven', 'eight',
  'nine', 'ten', 'eleven', 'twelve']
export const spell = (n: number) => (n < WORDS.length ? WORDS[n]! : fmt.int(n))
export const titled = (s: string) => s.charAt(0).toUpperCase() + s.slice(1)
