# Runbook

Symptom-first. Every entry below has actually happened on this realm.

## Clients cannot log in

**Auth fails immediately.** Check `ac-authserver` is up and 3724 is reachable:

```bash
docker ps | grep authserver
timeout 3 bash -c '</dev/tcp/127.0.0.1/3724' && echo ok
```

**Auth succeeds, then hangs at "Logging in to game server".** The realmlist row points
somewhere the client cannot reach - almost always because the host's IP moved:

```bash
./scripts/ra.sh "server info"    # is the world server even up
REALM_ADDRESS=10.0.0.x ./scripts/configure.sh
```

This is the single most common failure after a DHCP lease change. Reserve the address.

## World server restart-loops on startup

Check the log first: `docker logs ac-worldserver | tail -50`.

**`Directory ".../mod-playerbots/data/sql/playerbots/base/" not exist`** - the module
tree is not mounted into the world server. The compose override mounts `./modules`
read-only; confirm it survived an edit.

**Database connection refused** - `ac-database` is not healthy yet. The world server
depends on its healthcheck, so this means MySQL itself failed. Check its log.

## Bots are not appearing

```bash
./scripts/status.sh   # characters online vs total
```

- **0 online, 1000+ total** - `AiPlayerbot.RandomBotAutologin` is off, or the world
  server is still loading. Bot login takes minutes after a restart.
- **All bots at max level** - `LevelBrackets.Enabled` is off. Without it every bot
  drifts to 80 and the low zones empty out.
- **`acore_playerbots` has 0 tables** - the world server, not `dbimport`, owns that
  schema. See CUSTOMIZATIONS.md 6.2.

## The realm feels slow

```bash
./scripts/ra.sh "server info"    # tick diff percentiles
free -h                          # is it swapping
```

Rising p99 with flat CPU means memory pressure. Bots cost roughly 3-4 MB each; 1500
bots need 16 GB comfortably. Reduce `BOT_POPULATION` before anything else.

## Bots have stopped talking

The AI layer fails silently by design - gameplay must not depend on it.

```bash
curl -s localhost:8090/health    # llm_backend: up?
curl -s localhost:8090/metrics   # counters: errors, dropped, empty
docker logs ai-bridge | tail -30
```

- **`llm_backend: down`** - the model host is unreachable. The realm is unaffected;
  bots fall back to canned lines.
- **`empty` or `rejected` climbing** - the model is returning unusable output. Almost
  always a reasoning model whose chain-of-thought consumed the token budget. Check
  `reasoning_tokens == 0` for your model.
- **`dropped_*` climbing** - rate limiting, which is working as intended. Raise
  `MAX_CONCURRENT` only if the host has headroom.
- **Nothing at all in the log** - no human is online. The witness gate suppresses
  inference nobody would receive.

## A bot said something it should not have

Model output is scrubbed, never trusted. If reasoning or meta-commentary reaches a
player, the `_META` patterns in `ai-bridge/src/bridge/llm.py` missed a phrasing. Add
it, and add the line to the test list - rejection beats truncation, because half a
leaked monologue is still a leak.

## Restoring after data loss

```bash
ls -lt backups/daily/
./scripts/verify-restore.sh backups/daily/db-<stamp>.sql.gz   # prove it first
./scripts/restore.sh backups/daily/db-<stamp>.sql.gz          # destructive
```

`restore.sh` stops the game services, restores, and restarts them. It requires typing
the realm name to proceed. Verify before restoring, never after.

## A patch stopped applying after an upstream bump

```
AssertionError: <anchor> not found; upstream changed
```

Deliberate. The patches assert on every anchor so a silent no-op is impossible. Open
the named file, find where the code moved, and update the anchor in
`patches/llm/*.py`. Do not weaken the assertion.

## Emergency: turn the AI off

```bash
LLM_ENABLED=0 ./scripts/configure.sh
cd azerothcore && docker compose restart ac-worldserver
```

Or just stop the bridge - `docker stop ai-bridge`. The realm keeps running either way;
that property is the whole point of the split.

## Emergency: stop bots forming parties

```bash
PARTY_ASSEMBLER=0 BOT_GROUP_NEARBY=0 ./scripts/configure.sh
cd azerothcore && docker compose restart ac-worldserver
```
