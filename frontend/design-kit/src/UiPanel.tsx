import type { CSSProperties, ReactNode } from 'react'
import { T, FONT } from './theme'

export interface UiPanelProps {
  /** Small-caps heading, set in the display face. */
  cap: string
  /** Right-aligned counter or status shown on the same line as the cap. */
  note?: string
  /** Removes inner padding for panels that host their own scrolling list. */
  flush?: boolean
  /** Fills the parent and lets the body scroll instead of the page. */
  fill?: boolean
  children?: ReactNode
}

/**
 * The Observatory's panel shell - every box on every screen wears this chrome:
 * a hairline border on the warm dark plate, an inset top highlight, a
 * small-caps Cinzel cap with an optional mono note on the same baseline.
 * Compose any content as children; pass flush for lists that manage their
 * own padding and fill when the panel should own its scroll.
 */
export function UiPanel({ cap, note, flush, fill, children }: UiPanelProps) {
  const section: CSSProperties = {
    border: `1px solid ${T.line}`,
    background: T.panel,
    boxShadow: T.inset,
    display: 'flex',
    flexDirection: 'column',
    minHeight: 0,
    minWidth: 0,
    ...(fill ? { height: '100%' } : {}),
  }
  return (
    <section style={section}>
      <header
        style={{
          display: 'flex', alignItems: 'baseline', justifyContent: 'space-between',
          gap: '10px', padding: '10px 12px 6px', flex: 'none',
        }}
      >
        <span
          style={{
            fontFamily: FONT.display, fontWeight: 600, fontSize: '10px',
            letterSpacing: '.16em', color: T.dim, textTransform: 'uppercase',
            whiteSpace: 'nowrap',
          }}
        >{cap}</span>
        {note ? (
          <span
            style={{
              fontFamily: FONT.mono, fontSize: '10px', letterSpacing: '.08em',
              color: T.faint, whiteSpace: 'nowrap',
            }}
          >{note}</span>
        ) : null}
      </header>
      <div
        style={{
          minHeight: 0, flex: fill ? 1 : 'none',
          overflow: fill ? 'auto' : 'visible',
          padding: flush ? '0' : '0 12px 12px',
        }}
      >
        {children}
      </div>
    </section>
  )
}
