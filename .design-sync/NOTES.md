# Design-sync notes for this repo

- The design system source is NOT the app itself: the dashboard is Vue/Nuxt
  (frontend/app), and Claude Design consumes React. frontend/design-kit/ holds
  deliberate React ports of the three Vue primitives plus a verbatim token
  twin (src/theme.ts mirrors frontend/app/theme.ts). The Vue side is canonical.
- All Node tooling runs in Docker (repo boundary): kit build in node:22-alpine,
  converter + render check in mcr.microsoft.com/playwright:v1.57.0-noble with
  PLAYWRIGHT_BROWSERS_PATH=/ms-playwright and playwright@1.57.0 installed in
  .ds-sync (version must match the image tag).
- pnpm 11 blocks esbuild's postinstall; .ds-sync/pnpm-workspace.yaml carries
  allowBuilds for it. Installs use pnpm, not npm (repo boundary).
- Bare data components (UiBars, UiSpark) assume the dark plate - previews wrap
  them in background T.bg or their text disappears on the white card ground.
  Same rule applies to any future bare component.
- All three components use cardMode column (stories are ~340px wide panels).
- Fonts are remote Google Fonts by design ([FONT_REMOTE] is expected) - the
  app itself loads them from fonts.googleapis.com.

## Re-sync risks

- PORT DRIFT is the standing risk: any change to frontend/app/theme.ts or the
  three Ui*.vue components must be mirrored into frontend/design-kit/src by
  hand before re-syncing - nothing detects the divergence automatically.
  Diff theme values first; they are the likeliest to move.
- Preview data is inlined realm numbers from 2026-08-24 (errand census, trade
  feed, mail-run hours). Cosmetic staleness only; refresh when re-authoring.
- The playwright image tag and the pinned playwright version in .ds-sync must
  move together.
- Dashboard views beyond the three primitives (EconView etc.) are app screens,
  not DS components - deliberately out of scope; the guidelines document their
  patterns instead.
