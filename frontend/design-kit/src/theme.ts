// Design tokens for the Aetherion Observatory. Values mirror the Vue app's
// frontend/app/theme.ts verbatim - this file is the React-side twin, and any
// drift between the two is a bug. Single-theme by design: the Observatory
// commits to one dark warm plate; there is no light mode.
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

// What a character is doing, in the map's own vocabulary. Pairwise OKLab
// separation was measured before shipping; idle stays recessive by design.
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

export const fmt = {
  int: (n: number) => n.toLocaleString('en-GB'),
  yards: (n: number) => `${n.toLocaleString('en-GB')} yd`,
  // WotLK money is stored in copper; the dashboard shows one-decimal gold.
  gold: (copper: number) => `${(copper / 10000).toFixed(1)}g`,
  ago(ms: number) {
    const s = Math.max(0, Math.round((Date.now() - ms) / 1000))
    if (s < 60) return `${s}s`
    if (s < 3600) return `${Math.round(s / 60)}m`
    if (s < 86400) return `${Math.round(s / 3600)}h`
    return `${Math.round(s / 86400)}d`
  },
}

const WORDS = ['zero', 'one', 'two', 'three', 'four', 'five', 'six', 'seven',
  'eight', 'nine', 'ten', 'eleven', 'twelve']
export const spell = (n: number) => (n < WORDS.length ? WORDS[n]! : fmt.int(n))
