# Contributing

## Ground rules

1. **Gameplay never depends on inference.** If a change makes the realm degrade when
   the AI host is off, it is wrong regardless of how good it looks.
2. **Never block the world thread.** Anything that does I/O runs on a worker thread
   and delivers results back through a queue drained on the world thread.
3. **Patches assert.** Every upstream anchor is asserted before replacement. A patch
   that silently no-ops is worse than one that fails.
4. **Measure before and after.** Tick diff percentiles (`ra.sh "server info"`) for
   anything touching the core; `/metrics` for anything touching inference.

## Repository layout

Version-controlled overlay only. The upstream clone lives in `azerothcore/` and is
gitignored - `scripts/bootstrap.sh` recreates it at pinned commits.

## Working on the C++ patches

`patches/llm/` contains additive features for mod-playerbots plus `apply.sh`, which
applies them idempotently.

```bash
vim patches/llm/src/Ai/...        # edit the feature
./patches/llm/apply.sh            # re-apply (checks out pristine files first)
cd azerothcore && docker compose build ac-worldserver
```

`apply.sh` runs `git checkout` on every file it touches before patching, so it is
both idempotent and able to upgrade an older version of the patch in place.

**Anchors must assert.** Follow the existing pattern:

```python
assert ANCHOR in src, "<what> anchor not found; upstream changed"
```

When upstream moves the code, the bootstrap fails loudly with that message. Update
the anchor - never weaken the assertion to make it pass.

**Config over rebuild.** New behaviour should be gated by a config key so it can be
switched off without a 40-minute compile. Add the default to `apply.sh`'s config
block and wire an environment override in `scripts/configure.sh`.

## Working on the AI Bridge

Python, FastAPI, `uv` for dependencies, runs in Docker.

```bash
cd ai-bridge
docker compose up -d --build
curl -s localhost:8090/health
```

Things that will bite you, all learned the hard way:

- **Exactly one system message, and it must be first.** Some chat templates hard-fail
  on a second one.
- **Model output is untrusted.** Reasoning has leaked into `content` and reached
  players. Reject such replies outright - truncating a leaked monologue just yields a
  shorter leak.
- **Never let compose carry model defaults.** `VAR: ${VAR:-}` writes an empty string,
  which still counts as set and silently shadows the verified default in `config.py`.
- **Before adopting a model**, confirm it returns non-empty `content` with
  `reasoning_tokens == 0`. Most local models fail this.

## Working on the dashboard

Nuxt 4, `pnpm`, runs in Docker. Read-only access to the game database.

Coordinates are projected using the client's own `WorldMapArea.dbc` bounds. Do not
adjust them by eye - if dots look wrong, the art is wrong (it must be cropped to the
1002x668 content area of the padded 1024x768 tile grid).

## Commit conventions

Explain **why**, not what. The diff already says what changed.

```
Reject model replies containing meta-commentary

Reasoning leaked into `content` rather than `reasoning_content`, so nothing
upstream flagged it and the length cap truncated it into shorter garbage.
Rejection beats truncation: half a leaked monologue is still a leak.
```

Comments follow the same rule - explain the non-obvious reason a line exists, never
restate the code.

## Before opening a PR

```bash
./scripts/smoke.sh                       # 13 checks, exit code = failures
./scripts/status.sh                      # sanity-check population and tick
bash -n scripts/*.sh                     # shell syntax
```

If you touched the core, include before/after tick percentiles at a stated bot count.

## Licensing

AzerothCore and mod-playerbots are AGPL v3, and `patches/llm/` modifies
mod-playerbots - contributions there are AGPL v3 and the source must be offered to
anyone the server is offered to.

**Never commit** game client data, DBC files, map art, or any credential. Map art for
the dashboard is generated locally from a client you own.
