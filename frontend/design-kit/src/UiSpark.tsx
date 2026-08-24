import { T, FONT } from './theme'

export interface SparkPoint {
  label: string
  value: number
  /** Optional second series, drawn behind - for paired counts like deaths vs revives. */
  second?: number
}

export interface UiSparkProps {
  points: SparkPoint[]
  hue?: string
  secondHue?: string
  secondLabel?: string
  label?: string
}

/**
 * Column sparkline for short time series (hours of a day, days of a week).
 * Only the first and last labels render - a number on every column is the
 * classic way to make a small chart unreadable; per-column detail lives in
 * the hover title. An optional second series draws behind the first at
 * reduced opacity for paired counts.
 */
export function UiSpark({
  points,
  hue = 'oklch(0.74 0.14 85)',
  secondHue = 'oklch(0.55 0.06 158)',
  secondLabel,
  label,
}: UiSparkProps) {
  const ceiling = Math.max(1, ...points.map(p => Math.max(p.value, p.second ?? 0)))
  const n = points.length
  const endLabels = new Set(
    n <= 2 ? points.map(p => p.label) : [points[0]!.label, points[n - 1]!.label])

  return (
    <div>
      <div style={{ display: 'flex', alignItems: 'flex-end', gap: '2px', height: '54px' }}>
        {points.map(p => (
          <span
            key={p.label}
            style={{ flex: 1, position: 'relative', height: '100%', display: 'flex', alignItems: 'flex-end' }}
            title={`${p.label} · ${p.value}${p.second !== undefined ? ` / ${p.second}` : ''}`}
          >
            {p.second !== undefined ? (
              <span
                style={{
                  position: 'absolute', left: 0, right: 0, bottom: 0,
                  height: `${(p.second / ceiling) * 100}%`,
                  background: secondHue, opacity: 0.55, borderRadius: '2px 2px 0 0',
                }}
              />
            ) : null}
            <span
              style={{
                position: 'relative', width: '100%',
                height: `${(p.value / ceiling) * 100}%`,
                minHeight: p.value > 0 ? '2px' : '0',
                background: hue, borderRadius: '2px 2px 0 0',
              }}
            />
          </span>
        ))}
      </div>

      <div style={{ display: 'flex', justifyContent: 'space-between', marginTop: '5px' }}>
        {points.filter(x => endLabels.has(x.label)).map(p => (
          <span key={p.label} style={{ fontFamily: FONT.mono, fontSize: '9.5px', color: T.faint }}>
            {p.label}
          </span>
        ))}
      </div>

      {label ? (
        <div style={{ display: 'flex', alignItems: 'center', gap: '12px', marginTop: '6px' }}>
          <span style={{ display: 'flex', alignItems: 'center', gap: '5px' }}>
            <span style={{ width: '7px', height: '7px', background: hue }} />
            <span style={{ fontSize: '12px', color: T.muted }}>{label}</span>
          </span>
          {secondLabel ? (
            <span style={{ display: 'flex', alignItems: 'center', gap: '5px' }}>
              <span style={{ width: '7px', height: '7px', background: secondHue, opacity: 0.55 }} />
              <span style={{ fontSize: '12px', color: T.muted }}>{secondLabel}</span>
            </span>
          ) : null}
          <span style={{ marginLeft: 'auto', fontFamily: FONT.mono, fontSize: '10.5px', color: T.faint }}>
            peak {ceiling.toLocaleString('en-GB')}
          </span>
        </div>
      ) : null}
    </div>
  )
}
