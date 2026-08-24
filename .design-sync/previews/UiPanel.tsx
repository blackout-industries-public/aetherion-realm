import { UiPanel, T, FONT } from 'aetherion-observatory'

// Content mirrors real Observatory screens - the realm's own numbers, so the
// card reads like the product, not like placeholder art.

export function Canonical() {
  return (
    <div style={{ width: 360 }}>
      <UiPanel cap="Conflict" note="realm totals">
        <p style={{ fontSize: 13, color: T.body, margin: 0, lineHeight: 1.5 }}>
          The house holds 904 listings from 109 sellers. 43 sales completed in
          the last day.
        </p>
      </UiPanel>
    </div>
  )
}

export function StatStrip() {
  const stats = [
    { v: '774,354', l: 'honourable kills today' },
    { v: '1,764', l: 'characters killing' },
    { v: '60,293', l: 'top honour held' },
    { v: '1,848', l: 'have honour' },
  ]
  return (
    <div style={{ width: 360 }}>
      <UiPanel cap="Conflict" note="realm totals">
        <div style={{ display: 'grid', gridTemplateColumns: '1fr 1fr', gap: '10px 14px' }}>
          {stats.map(s => (
            <div key={s.l}>
              <div style={{ fontFamily: FONT.mono, fontSize: 19, color: T.textHi }}>{s.v}</div>
              <div style={{ fontSize: 11.5, color: T.muted }}>{s.l}</div>
            </div>
          ))}
        </div>
      </UiPanel>
    </div>
  )
}

export function FlushList() {
  const rows = [
    { name: 'Gwenve', deed: 'sold 2x Silverleaf to Gomdon', price: '80c' },
    { name: 'Unmann', deed: 'sold 2x Light Leather to Mordri', price: '1.0s' },
    { name: 'Uget', deed: 'bought Ardent Custodian', price: '81.6g' },
  ]
  return (
    <div style={{ width: 360 }}>
      <UiPanel cap="Recent trades" note="newest first" flush>
        {rows.map((r, i) => (
          <div
            key={r.name}
            style={{
              display: 'flex', justifyContent: 'space-between', gap: 10,
              padding: '6px 12px', fontSize: 12.5, color: T.body,
              borderTop: i ? `1px solid ${T.lineFaint}` : 'none',
            }}
          >
            <span>
              <span style={{ color: T.textHi }}>{r.name}</span> {r.deed}
            </span>
            <span style={{ fontFamily: FONT.mono, fontSize: 11.5, color: T.gold }}>{r.price}</span>
          </div>
        ))}
      </UiPanel>
    </div>
  )
}
