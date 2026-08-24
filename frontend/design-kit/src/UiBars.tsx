import { T, FONT } from './theme'

export interface BarRow {
  label: string
  value: number
  /** Small right-aligned mono annotation after the value. */
  note?: string
  /** Per-row color override; rows without one share the single hue. */
  tone?: string
}

export interface UiBarsProps {
  rows: BarRow[]
  /** Single hue for magnitude. A multi-hue ramp would invent categories the data lacks. */
  hue?: string
  /** Fixed denominator, when the reader should compare against a cap rather than the max. */
  max?: number
  unit?: string
  labelWidth?: string
  /** When set, row labels become clickable and report their label. */
  onPick?: (label: string) => void
}

/**
 * Horizontal bar rows for ranked or compared magnitudes - one hue, a thin
 * mark on a recessive track, mono tabular values on the right. The track is
 * the 100% reference: pass max when the comparison is against a cap rather
 * than the largest row.
 */
export function UiBars({
  rows,
  hue = 'oklch(0.72 0.13 88)',
  max,
  unit,
  labelWidth = '112px',
  onPick,
}: UiBarsProps) {
  const ceiling = max ?? Math.max(1, ...rows.map(r => r.value))
  return (
    <div>
      {rows.map(r => (
        <div
          key={r.label}
          style={{
            display: 'grid',
            gridTemplateColumns: `${labelWidth} minmax(40px, 1fr) auto`,
            gap: '10px', alignItems: 'center', padding: '3px 0',
          }}
        >
          <span
            style={{
              fontSize: '13px', color: T.body, overflow: 'hidden',
              textOverflow: 'ellipsis', whiteSpace: 'nowrap',
              cursor: onPick ? 'pointer' : 'default',
            }}
            title={r.label}
            onClick={onPick ? () => onPick(r.label) : undefined}
          >{r.label}</span>

          <span style={{ height: '7px', background: 'oklch(0.26 0.02 56)', position: 'relative', borderRadius: '1px' }}>
            <span
              style={{
                position: 'absolute', inset: '0 auto 0 0',
                width: `${Math.max(r.value > 0 ? 2 : 0, (r.value / ceiling) * 100)}%`,
                background: r.tone ?? hue,
                borderRadius: '1px 3px 3px 1px',
              }}
            />
          </span>

          <span style={{ display: 'flex', alignItems: 'baseline', gap: '6px', justifyContent: 'flex-end' }}>
            <span style={{ fontFamily: FONT.mono, fontSize: '11.5px', color: T.text, fontVariantNumeric: 'tabular-nums' }}>
              {r.value.toLocaleString('en-GB')}
              {unit ? <span style={{ color: T.faint }}>{unit}</span> : null}
            </span>
            {r.note ? (
              <span style={{ fontFamily: FONT.mono, fontSize: '10px', color: T.faint, minWidth: '38px', textAlign: 'right' }}>
                {r.note}
              </span>
            ) : null}
          </span>
        </div>
      ))}
    </div>
  )
}
