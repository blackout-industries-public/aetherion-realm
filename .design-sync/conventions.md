# Aetherion Observatory conventions

Single-theme system: one dark warm plate, no light mode. Every design paints
its own ground - never leave a host-default background showing.

## Setup

No provider is needed. Components are self-styling via inline styles from the
token module. Two rules make a design look right:

1. The page (or outermost container) must carry the plate:
   `background: var(--aeth-bg)` (or `T.bg` in JS) with `color: var(--aeth-body)`
   and `font-family: var(--aeth-font-body)`. `styles.css` already applies this
   to `body`.
2. Bare data components (`UiBars`, `UiSpark`) belong inside a `UiPanel` or on
   the plate - their text is warm-light and vanishes on white.

## Styling idiom

No CSS class vocabulary exists. Style your own layout glue with inline styles
using either the JS tokens (`import { T, FONT } from 'aetherion-observatory'`,
e.g. `style={{ color: T.muted, fontFamily: FONT.mono }}`) or the CSS custom
properties defined in `styles.css`: `--aeth-bg`, `--aeth-panel`,
`--aeth-panel-alt`, `--aeth-raised`, `--aeth-line`, `--aeth-line-soft`,
`--aeth-line-faint`, `--aeth-inset`, `--aeth-text-hi`, `--aeth-text`,
`--aeth-text-mid`, `--aeth-body`, `--aeth-muted`, `--aeth-dim`, `--aeth-faint`,
`--aeth-gold`, `--aeth-gold-dim`, `--aeth-gold-bright`, `--aeth-red`,
`--aeth-green`, `--aeth-bar-track`, and `--aeth-state-*` (idle, working, town,
travelling, questing, grinding, grouped, instance, ghost, dead).

Type roles: `--aeth-font-display` (Cinzel) only for small-caps panel caps and
page brand; `--aeth-font-body` (Spectral) for prose; `--aeth-font-mono`
(Cutive Mono) for every number, timestamp, and stat - always with
`font-variant-numeric: tabular-nums` when digits align. Gold is the accent;
green/red are semantic (good/bad) and never decorative. Money renders as
one-decimal gold via `fmt.gold(copper)`.

## Where the truth lives

Read `styles.css` (tokens and plate) and each component's `.prompt.md` and
`.d.ts` under `components/general/`. Exports: `UiPanel`, `UiSpark`, `UiBars`,
plus token objects `T`, `FONT`, `STATE` and helpers `fmt`, `spell`.

## Idiomatic build

```tsx
import { UiPanel, UiBars, T, FONT } from 'aetherion-observatory'

<div style={{ background: T.bg, padding: 14 }}>
  <UiPanel cap="Errand census" note="live">
    <UiBars rows={[{ label: 'mailbox', value: 694 }, { label: 'gather', value: 587 }]} />
    <div style={{ fontFamily: FONT.mono, fontSize: 11, color: T.faint, marginTop: 8 }}>
      updated 45s ago
    </div>
  </UiPanel>
</div>
```
