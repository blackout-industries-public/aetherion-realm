# Aetherion Realm

A self-hosted World of Warcraft: Wrath of the Lich King (3.3.5a) realm whose
population is simulated well enough to feel inhabited by one player.

Two layers, deliberately independent:

- **Gameplay** - AzerothCore plus Playerbots. Bots level, quest, fight, form parties
  and travel to dungeons. This works with no AI involved at all.
- **Social** - an AI Bridge gives bots personality, memory and conversation through a
  local LLM. Turn it off and the realm is unaffected.

That separation is the core design rule: **gameplay never depends on inference.**

```
                    ┌──────────────┐
   WoW 3.3.5a ─────▶│  worldserver │◀── authserver ──▶ MySQL
     client         │  + playerbots│
                    └──────┬───────┘
                           │ fire-and-forget HTTP
                           ▼
                    ┌──────────────┐     ┌─────────┐     ┌───────────┐
                    │  AI Bridge   │────▶│ Bifrost │────▶│ LM Studio │
                    └──────────────┘     └─────────┘     └───────────┘
                           ▲
                    ┌──────┴───────┐
                    │  Realm map   │  live dashboard of every bot and player
                    └──────────────┘
```

## Documentation

| Document | Purpose |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Bare Ubuntu VM to a running realm |
| [docs/OPERATIONS.md](docs/OPERATIONS.md) | Day-to-day management and every helper script |
| [docs/RUNBOOK.md](docs/RUNBOOK.md) | What to do when something breaks |
| [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) | How the pieces fit and why |
| [docs/CUSTOMIZATIONS.md](docs/CUSTOMIZATIONS.md) | Every deviation from upstream, and the bugs worked around |
| [CONTRIBUTING.md](CONTRIBUTING.md) | Conventions, especially for the upstream patches |
| [MANIFEST.md](MANIFEST.md) | Pinned revisions - the rollback target |

## What is in this repository

| Path | What it is |
|---|---|
| `overlay/` | `.env` template and the compose override applied over upstream's file |
| `scripts/` | Bootstrap, configure, backup/restore, smoke tests, ops helpers |
| `patches/llm/` | Additive C++ features for mod-playerbots, applied idempotently |
| `ai-bridge/` | The LLM adapter service (FastAPI, its own compose stack) |
| `frontend/` | Live realm map dashboard (Nuxt) |
| `ops/systemd/` | Unit files for autostart and scheduled backups |

**Not** in this repository: game client data, Blizzard assets, or any secret. The
pinned AzerothCore clone is recreated by `scripts/bootstrap.sh`.

## Quick start

```bash
git clone <this repo> /opt/warcraft && cd /opt/warcraft
./scripts/bootstrap.sh                        # pin sources, generate .env
cd azerothcore && docker compose build        # ~40 min first time
docker compose up -d                          # downloads client data, imports DB
cd /opt/warcraft && ./scripts/configure.sh    # tune realm, bots, economy
./scripts/smoke.sh                            # 13 mechanical checks
```

Full detail in [docs/INSTALL.md](docs/INSTALL.md).

## Licensing

AzerothCore and mod-playerbots are **AGPL v3**. `patches/llm/` modifies
mod-playerbots and is published under the same terms. `ai-bridge/` and `frontend/`
are separate processes communicating over HTTP.

No Blizzard client data, DBC files or map art is included. Map art for the dashboard
is generated from a client you already own - see `frontend/maps/README.md`.
