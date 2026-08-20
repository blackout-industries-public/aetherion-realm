# Version Manifest

Reproducible-build record required by BRD s6. Update this file on every accepted
upgrade; it is the rollback target.

| Component | Pin |
|---|---|
| Host OS | Ubuntu 26.04 LTS (resolute), kernel 7.0.0-30-generic |
| Docker Engine | 29.7.2 |
| Docker Compose | v5.5.0 |
| AzerothCore fork | `mod-playerbots/azerothcore-wotlk` branch `Playerbot` |
| AzerothCore commit | `efe123fab543c5faf3c477674ec17a18fd59f09f` (2026-08-14) |
| mod-playerbots | `mod-playerbots/mod-playerbots` `8d9f6aa6bc6d45f9ae0ee0675b9b1f8aa6937312` (2026-08-14) |
| mod-ah-bot | `azerothcore/mod-ah-bot` `a680cc1c98290713e9b3d3289544af78e5186dc1` (2025-11-09) |
| Client data | wowgaming/client-data `v20.0` (enUS) |
| Database | MySQL 8.4 (MariaDB avoided - deadlocks reported with Playerbots) |
| Build image base | ubuntu:24.04, clang, ninja |
| Compiler | clang (ubuntu:24.04 default), ninja, RelWithDebInfo, static scripts+modules |
| DB schema version | auth 12 new/9 archived, characters 9/19, world 666/2199, playerbots 31 queries |
| Config revision | see git log of this repo |

## Repository layout

```
/opt/warcraft/
  azerothcore/     pinned upstream clone (disposable; rebuilt by bootstrap.sh)
  overlay/         .env + docker-compose.override.yml   <- version controlled
  config/          generated .conf files, tuned by configure.sh
  logs/            worldserver/authserver logs, build log
  backups/         daily / weekly / monthly dumps
  scripts/         bootstrap, configure, backup, restore, upgrade, status
  ops/systemd/     unit files
```

## Deviations from the BRD

| BRD | Decision | Why |
|---|---|---|
| s38 prefers a native build | Docker Compose | Explicitly requested. Also insulates the build from Ubuntu 26.04, which is newer than anything AzerothCore tests against. |
| s5.1 16 GB RAM min | Host has 7.3 GB | Needs a Proxmox-side change. Bot population capped at Stage A until raised. |
| s5.1 SSD/NVMe | Virtual disk reports rotational | Verify the Proxmox backing store. |
| s16 daily dumps | Binary logging disabled | No replica and no point-in-time-recovery requirement; halves write IO on a constrained host. Recovery granularity is therefore one day. |


## Upstream defects worked around

Three bugs in upstream's Docker setup prevent a Playerbots realm from starting; all
three are handled in `overlay/docker-compose.override.yml` or `scripts/bootstrap.sh`.

| Problem | Symptom | Workaround |
|---|---|---|
| Worldserver image does not ship `modules/` | `Directory ".../mod-playerbots/data/sql/playerbots/base/" not exist`, then a restart loop | Mount `./modules` read-only into `ac-worldserver` |
| `dbimport` never registers the Playerbots DB (`Main.cpp` adds only Login/Character/World) | `acore_playerbots` created but left empty, no error | Let the worldserver populate it; the module's own `DatabaseScript` does this via `Playerbots.Updates.EnableDatabases` |
| Upstream compose omits `AC_PLAYERBOTS_DATABASE_INFO` | Playerbots DB unreachable | Set it on both worldserver and db-import |

`playerbots.conf.dist` also omits three keys the module reads at startup
(`Playerbots.Updates.EnableDatabases`, `PlayerbotsDatabase.WorkerThreads`,
`PlayerbotsDatabase.SynchThreads`); `configure.sh` writes them explicitly.

## Operational notes

- The worldserver console needs an interactive TTY, so automation goes through
  Remote Access on loopback 3443 (`scripts/ra.sh`). RA speaks CRLF; bare LF hangs.
- Accounts are created with `scripts/create-account.sh`, which reproduces
  `AccountMgr::CreateAccount`'s SRP6 derivation rather than driving the console.
- ufw does not filter Docker-published ports. That is acceptable here only because
  the published set is exactly {3724, 8085} plus loopback-bound 3306/7878/3443.
