# Operations

Day-to-day management. Every script is idempotent and safe to re-run.

## Helper scripts

All live in `scripts/` and source `lib.sh` for shared helpers.

| Script | Purpose |
|---|---|
| `bootstrap.sh` | Clone and pin upstream, apply patches, stage compose config |
| `configure.sh` | Apply all tuned settings to the generated `.conf` files |
| `smoke.sh` | 13 mechanical acceptance checks; exit code = failures |
| `status.sh` | Population, level spread, zones, DB size, tick performance |
| `backup.sh` | Dump all four databases plus config; 7/4/3 retention |
| `verify-restore.sh` | Prove a dump restores, using a throwaway MySQL instance |
| `restore.sh <dump>` | Restore into the live databases (destructive; asks for confirmation) |
| `upgrade.sh` | Backup, re-pin, rebuild, migrate, smoke test |
| `create-account.sh` | Create or reset an account via SRP6, no console needed |
| `setup-ahbot.sh` | Give the auction-house bot its market identity |
| `ra.sh "<cmd>"` | Run world server console commands non-interactively |

## The console

The world server console requires a real TTY, so automation goes through Remote
Access on loopback 3443:

```bash
RA_USER=admin RA_PASS=... ./scripts/ra.sh "server info" "account onlinelist"
```

RA speaks **CRLF**; a bare LF hangs the session silently. `ra.sh` handles this.

For an interactive console: `docker compose attach ac-worldserver` (detach with
ctrl-p ctrl-q).

## Tuning the realm

`configure.sh` reads environment variables, so nothing needs editing by hand:

```bash
BOT_POPULATION=500 ./scripts/configure.sh          # bot count
LLM_ENABLED=1 LLM_AMBIENT=1 ./scripts/configure.sh # AI layer on
PARTY_ASSEMBLER=0 ./scripts/configure.sh           # stop bots forming parties
```

Restart `ac-worldserver` afterwards. A few settings can be reloaded live in-game with
`.playerbots bot reload`, but most need the restart.

### Settings worth knowing

| Variable | Default | Effect |
|---|---|---|
| `BOT_POPULATION` | 50 | Concurrent bots. Stage it: 50 → 200 → 500 → 1500 |
| `BOT_GROUP_NEARBY` | 1 | Bots form parties with bots they meet |
| `PARTY_ASSEMBLER` | 1 | Background assembly of full 5-man parties |
| `PARTY_TRAVEL` | 1 | Parties walk/fly to a dungeon rather than teleporting |
| `GROUPER_*` | leader-heavy | How ambitious bots are about party size |
| `LLM_ENABLED` | 0 | The AI layer as a whole |
| `LLM_AMBIENT` | 0 | Unprompted bot chatter |
| `LLM_CHANNEL_CHANCE` | 25 | Percent chance a public-channel line gets a reply |

Population is staged deliberately. Validate at each step with `status.sh` - watch
tick diff and host RAM before going higher.

## Watching it

```bash
./scripts/status.sh                             # snapshot
./scripts/ra.sh "server info"                   # tick diff percentiles
curl -s localhost:8090/metrics                  # AI request counters and latency
docker compose logs -f ac-worldserver           # live log
```

The dashboard at `http://<host>:3000` shows every bot and player on continent maps,
plus a Groups panel with party composition and instance state.

**Tick diff is the number that matters.** Mean under ~50 ms and p99 under ~150 ms is
healthy at 1500 bots. Rising p99 means the world is struggling before anything else
shows it.

## Backups

Automatic daily at 04:30 UTC via `warcraft-backup.timer`, with 7 daily, 4 weekly and
3 monthly retained.

```bash
./scripts/backup.sh          # on demand
./scripts/verify-restore.sh  # prove the newest dump actually restores
```

`verify-restore.sh` loads the dump into a disposable MySQL container and compares
table counts against the live server. It never touches live data, so it is safe to
run on a schedule. **An untested backup is not a backup.**

Binary logging is disabled, so recovery granularity is one day. That is a deliberate
trade for write performance; if you need point-in-time recovery, re-enable binlog in
the compose override and expect more disk churn.

## Upgrading

Never track a moving branch. Edit the commit pins in `overlay/.env`, snapshot the VM,
then:

```bash
./scripts/upgrade.sh   # backup -> re-pin -> rebuild -> migrate -> smoke test
```

Record accepted revisions in `MANIFEST.md`. Roll back by restoring the dump and
re-pinning the previous commits.

Upstream moves fast and the patches assert on every anchor they touch, so a patch
that no longer applies **fails loudly** during bootstrap rather than silently
producing a build without the feature.

## Ports

| Port | Bound to | Purpose |
|---|---|---|
| 3724 | all | Auth server - clients need this |
| 8085 | all | World server - clients need this |
| 3000 | all | Dashboard (no auth by design) |
| 3306 | loopback | MySQL |
| 7878 | loopback | SOAP |
| 3443 | loopback | Remote Access console |
| 8090 | loopback | AI Bridge |

Docker's published ports bypass ufw. The set above is deliberate: only what a client
or a browser needs faces the LAN.
