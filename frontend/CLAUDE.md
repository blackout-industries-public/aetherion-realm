# Aetherion Observatory — the realm dashboard

Read-only Nuxt 4 portal over the live AzerothCore realm (2,500 bots).
Production: container `warcraft-map` on nlucansk@10.10.25.194, LAN-visible at
http://10.10.25.194:3000. It must never be able to affect the realm - every
endpoint is SELECT-only.

## Architecture

- `app/app.vue` is the shell: tab bar, central polling loop, and `select(name)`
  which opens the bot detail rail (history + personality from the ai-bridge).
  The right-hand EventRail streams `aetherion_ai.bot_events`.
- Two data patterns, both current:
  - **Shell-fed** (original tabs): `app.vue` does the `useFetch` and passes
    props - WorldView, GroupsView, SocietyView, PvpView, RaceView, EconView,
    GuildsView, OpsView, QuestPanels.
  - **Self-fetching** (newer panels): the component owns its endpoint +
    30-60s `setInterval`, takes no required props, emits `('select', name)` -
    MarketView, WealthView, PulseView, IndustryView. Prefer this pattern for
    new panels; it keeps app.vue wiring to one mount line.

## Tabs → views → endpoints

| Tab | Component(s) | Endpoint(s) |
|---|---|---|
| WORLD | WorldView (map, hotspots, professions) | /api/world |
| PVE | GroupsView + QuestPanels | /api/assembler, /api/quests |
| PVP | PvpView | /api/pvp |
| RACE | RaceView | /api/race |
| ECON | EconView + PulseView + IndustryView | /api/econ, /api/pulse, /api/industry |
| MARKET | MarketView + WealthView | /api/market, /api/wealth |
| SOCIETY | SocietyView | /api/society, /api/guilds, /api/combat, /api/llm |
| GUILDS | GuildsView | /api/guild |
| OPS | OpsView | /api/ops, /api/assembler, /api/llm |
| (rail) | EventRail + bot detail | /api/events, /api/bot/* |

## Data sources

- MySQL via `server/utils/db.ts`: one pool, `q(sql)` returns `[]` on error and
  logs each distinct failure once (surfaced on the OPS tab) - endpoints
  degrade to empty sections rather than 500. Endpoints that call
  `getPool().query` directly (world.get.ts) DO 500 on DB errors.
  Credentials come from Nuxt runtimeConfig, overridden at runtime by
  `NUXT_DB_*` env vars; compose maps `NUXT_DB_PASSWORD` from
  `${DOCKER_DB_ROOT_PASSWORD}` which lives in the SERVER-LOCAL `frontend/.env`
  (never in git, exists in no working copy - do not rsync-delete it).
- Schemas: `acore_characters` (characters, character_skills, mail,
  auctionhouse, item_instance, guild*) plus the realm's own telemetry tables:
  - `aetherion_econ_events(id, ts, kind, guid, item, count, detail)` - kinds:
    vendor_sell (detail=copper), ah_post/ah_listed/ah_sold/ah_bought
    (detail=price copper), ah_expired, mail_money (detail=copper), mail_item,
    mail_collect (count=letters), craft (item=product, detail=spellId),
    bank_deposit, bank_withdraw, destroy, repair_paid (detail=bill copper),
    gather_route (item=node GO entry).
  - `aetherion_needs(guid, need_type, target, amount, free_money, since_ts)` -
    live state, overwritten per ledger pass; need_type='errand' rows are the
    verdict census (target: vendor/mailbox/trainer/ah/focus/gather).
  - `aetherion_gold_now`, `aetherion_gold_bands` (5-min supply history).
  - `acore_world.item_template` / `gameobject_template` for names.
  - `aetherion_ai.bot_events` - the bridge's event stream (pvp, death, loot...).
- ai-bridge (FastAPI, http://ai-bridge:8090) serves bot personality/history
  and /metrics (Prometheus + JSON by Accept header).

## Data semantics worth knowing

- `characters.todayKills` counts HONOURABLE-KILL CREDITS: every nearby group
  member gets one per enemy death (40-man BGs mint ~5-6x the distinct-event
  count in bot_events). Rollover to yesterdayKills happens per-player on
  honor updates.
- Money is copper; render as one-decimal gold ("81.6g"), silver/copper tiers
  below 1g. `CAST(NULLIF(detail,'') AS SIGNED)` is the idiom for summing
  copper out of `detail`.

## Design system

- Tokens in `app/theme.ts` (`T`, `FONT`, `STATE`, `fmt`) - single dark warm
  oklch theme, no light mode. Gold is the only accent; green/red are semantic;
  STATE colors are fixed per activity, never reused for series.
- Primitives: `UiPanel` (cap/note/flush/fill), `UiSpark` (column sparkline,
  end-only labels, optional second series), `UiBars` (single-hue ranked rows).
  Panel grammar and chart rules: `design-kit/docs/guides/observatory-language.md`.
- Character names are always clickable (emit `select`) and render brighter
  than surrounding text. Numbers are always FONT.mono with tabular-nums.
- `frontend/design-kit/` is a React twin of tokens + primitives for
  claude.ai/design (synced via /design-sync). The Vue side is canonical -
  mirror any theme/primitive change into design-kit before re-syncing.

## Build, run, deploy

- All Node tooling in Docker, pnpm only:
  `docker run --rm -v $PWD/frontend:/app -w /app node:22-alpine sh -c "corepack enable && pnpm install --frozen-lockfile && pnpm run build"`
- Deploy (frontend only): rsync `frontend/` to nlucansk@10.10.25.194:/opt/warcraft/frontend/
  with `--exclude node_modules --exclude .nuxt --exclude .output --exclude .env`
  (NEVER --delete without those excludes), then
  `ssh ... 'cd /opt/warcraft/frontend && docker compose up -d --build'`.
  Full-stack deploys: `scripts/deploy-econ.sh nlucansk@10.10.25.194`.
- CI: validate.yml builds the frontend on every push (pnpm 11 needs
  `allowBuilds` in pnpm-workspace.yaml for esbuild).
- `frontend/maps/*.jpg` are gitignored client art, bind-mounted read-only into
  the container - present on the server and in working copies, never in git.

## Known warts

- A harmless "Hydration completed but contains mismatches" console warning
  from SSR'd relative-time strings.
- The AH_FLOW-style kind lists in endpoints must be extended when a new
  econ event kind is added, or the new counter silently reads zero.
