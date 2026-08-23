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
