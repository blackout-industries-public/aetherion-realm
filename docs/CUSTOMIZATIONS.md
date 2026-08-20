# Customizations

Everything in this realm that is not stock upstream, and why it exists. Written for
publishing: an outsider should be able to reproduce the whole setup from this file
plus `MANIFEST.md`.

Nothing here modifies AzerothCore's game logic. The changes are an operations overlay
plus one additive module feature (the LLM bridge).

---

## 1. Repository layout

| Path | Purpose | Upstream? |
|---|---|---|
| `overlay/` | `.env` + compose override applied on top of upstream's compose file | custom |
| `scripts/` | Bootstrap, configure, backup/restore, smoke tests, ops helpers | custom |
| `patches/llm/` | The Phase 2 module patch and its idempotent applier | custom |
| `ai-bridge/` | Standalone LLM adapter service (own compose stack) | custom |
| `ops/systemd/` | Unit files for autostart and the backup timer | custom |
| `docs/` | Acceptance tracking, raid matrix, this file | custom |
| `azerothcore/` | Pinned upstream clone; disposable, rebuilt by `bootstrap.sh` | upstream |

---

## 2. Changes to upstream source

All applied by `patches/llm/apply.sh`, which `git checkout`s the touched files first
so it is idempotent and can upgrade itself in place. Every anchor is asserted, so if
upstream moves the code the patch **fails loudly** rather than silently no-opping.

### 2.1 New files

`modules/mod-playerbots/src/Ai/Llm/LlmBridge.{h,cpp}` - the entire game-side bridge.
Module sources are auto-globbed by AzerothCore's `AutoCollect.cmake`, so no CMake
change is needed.

### 2.2 `modules/mod-playerbots/src/Bot/PlayerbotAI.cpp`

`HandleCommands()` has an empty branch where the command parser gives up on a chat
message. That branch now forwards the message to the bridge. This is the ideal seam:
messages that *are* commands (`follow`, `stay`, `attack`) keep working untouched, and
only conversation reaches the model.

### 2.3 `modules/mod-playerbots/src/Script/Playerbots.cpp`

| Hook | Change |
|---|---|
| `OnBeforeWorldInitialized` | `sLlmBridge->LoadConfig()` |
| `OnUpdate(diff)` | `sLlmBridge->Drain(diff)` - delivers replies on the world thread |
| `OnPlayerLogin` | schedules a greeting from a bot that already knows the player |
| `OnPlayerCanUseChat(..., Channel*)` | public-channel replies, chance-gated |
| `OnPlayerLevelChanged` | **new override** - level-up reactions |
| `OnPlayerJustDied` | **new override** - death reactions |
| `OnPlayerLootItem` | **new override** - rare+ loot reactions |

### 2.4 `modules/mod-playerbots/conf/playerbots.conf.dist`

An `AiPlayerbot.Llm.*` block appended. All defaults are off or conservative.

### 2.5 `azerothcore/apps/docker/Dockerfile`

Applied by `scripts/bootstrap.sh`, not the LLM patch. Upstream hardcodes
`-j $(nproc)+1`; on a small host that is a guaranteed OOM, so the job count became a
`BUILD_JOBS` build arg.

---

## 3. Compose overrides (`overlay/docker-compose.override.yml`)

| Change | Why |
|---|---|
| MySQL tuning, `--skip-log-bin` | No replica and no PITR requirement; halves write IO |
| DB/SOAP/RA published on loopback only | Only 3724 and 8085 should face the LAN |
| `AC_PLAYERBOTS_DATABASE_INFO` on db-import | Upstream omits it entirely |
| `./modules` mounted read-only into worldserver | **Required** - see 6.1 |
| Healthchecks, log rotation, 900s worldserver `start_period` | Loading maps plus 1500 bots is slow; a short period flaps the container |

---

## 3a. Inference path (BRD s20)

```
worldserver -> ai-bridge -> Bifrost gateway -> LM Studio -> model
```

Bifrost fronts inference rather than the bridge calling a model host directly. The
bridge only ever speaks OpenAI-compatible HTTP, so adopting the gateway was two
environment variables and no code change:

```
LLM_BASE_URL=https://bifrost.cluster.blackout.industries/v1
MODEL_INTERACTIVE=lm studio/openai-gpt-oss-20b-...
```

Notes learned wiring this up:

- **Bifrost prefixes model ids with the provider name** (`lm studio/...`). The id from
  LM Studio's own `/v1/models` will not resolve through the gateway.
- `enforce_auth_on_inference` is false, so `/v1/*` needs no key; only `/api/*` (admin)
  does. Admin credentials live in the `bifrost-admin-credentials` secret.
- Providers are stored in Bifrost's SQLite config store, **not** in the mounted
  `config.json`, so they are managed through the admin API or UI.
- Gateway overhead is negligible - measured p50 ~4s, same as direct.
- The realm keeps working when the gateway is down; failures degrade to canned
  replies exactly as a model outage does.

## 4. The AI Bridge (`ai-bridge/`)

A separate compose stack on purpose: it must be able to crash, restart or be shut
down entirely without the realm noticing (BRD s3.1). It shares only the database
network. The game calls it; it never calls the game.

| Endpoint | Purpose |
|---|---|
| `POST /game/whisper` | Chat reply as **plain text**; `204` means stay quiet |
| `POST /game/event` | Reaction to a real world event |
| `POST /game/greet` | Returns `"<bot_guid>\n<line>"` - the bridge picks who greets |
| `GET /bot/{name}`, `/health`, `/metrics` | Inspection |

The core emits JSON but never parses it. Keeping a JSON parser out of the game server
is deliberate; plain text responses cost nothing and cannot crash the world thread.

Features: identity from the character DB, personality derived from character GUID,
tiered memory (SQLite), weighted ambient topics, bursty pacing, reflex short-circuit,
priority budgeting, output sanitising.

---

## 5. Configuration reference

All set by `scripts/configure.sh`, overridable by environment variable.

### Playerbots (non-LLM)

| Key | Value | Why |
|---|---|---|
| `AiPlayerbot.LevelBrackets.Enabled` | 1 | Without it every bot drifts to 80 and low zones empty |
| `AiPlayerbot.MinRandomBots` / `Max` | 1500 | Staged per BRD s8 |
| `AiPlayerbot.RandomBotGuildCount` | 60 | More guild society |
| `AiPlayerbot.RandomBotGuildSizeMax` | 25 | |
| `Playerbots.Updates.EnableDatabases` | 1 | Module reads it but ships no default |
| `PlayerbotsDatabase.WorkerThreads` / `SynchThreads` | 1 | Same |

### LLM bridge

| Key | Default | Notes |
|---|---|---|
| `Llm.Enabled` | 0 | Off unless explicitly enabled |
| `Llm.Host` / `Port` | ai-bridge / 8090 | Reached over the compose network |
| `Llm.TimeoutMs` / `MaxInFlight` | 15000 / 8 | Caps worldserver thread count |
| `Llm.ClaimWindowMs` | 10000 | Exactly one bot answers a given message |
| `Llm.RequireHumanWitness` | 1 | No inference for chat nobody receives |
| `Llm.SameFactionOnly` | 1 | Cross-faction lines are unreadable anyway |
| `Llm.SayRange` | 45 | Roughly /say audible range |
| `Llm.ReactWhisper/Party/Guild/Say` | 1/1/1/1 | Which channels bots answer |
| `Llm.ChannelReplyChance` | 40 | Public channels are chance-gated |
| `Llm.AmbientEnabled` / `IntervalMs` | 1 / 8000 | Offer rate; the bridge does real pacing |
| `Llm.AmbientUseSay` | 1 | Local `/say` beats zone-wide channels for presence |
| `Llm.AmbientMaxDepth` | 2 | Bot-to-bot threads terminate |
| `Llm.GreetOnLogin` / `GreetDelayMs` | 1 / 15000 | Lands after the loading screen |
| `Llm.EventsEnabled` | 1 | |
| `Llm.EventChanceLevelUp/Death/Loot` | 100/40/35 | Loot is filtered to rare+ |

Bridge-side (environment): `LLM_BASE_URL`, `MODEL_INTERACTIVE`, `MODEL_BACKGROUND`,
`REASONING_EFFORT`, `MAX_CONCURRENT`, `AMBIENT_MIN_INTERVAL`, `PER_BOT_COOLDOWN`.

---

## 6. Upstream defects worked around

### 6.1 Worldserver image ships no `modules/`

Only the db-import target copies `modules`. The worldserver is what actually
auto-populates `acore_playerbots`, so without the module SQL on disk it aborts at
startup and restart-loops with:

```
Database Playerbots is empty, auto populating it...
>> Directory ".../mod-playerbots/data/sql/playerbots/base/" not exist
```

Fixed by mounting `./modules` read-only into the worldserver.

### 6.2 `dbimport` cannot import the playerbots schema

`src/tools/dbimport/Main.cpp` registers only Login/Character/World. It creates an
empty `acore_playerbots` and skips it **without erroring**. Raising
`Updates.EnableDatabases` does not help - the database is never registered. The
worldserver owns that schema instead, via the module's own `DatabaseScript`.

### 6.3 Upstream compose omits `AC_PLAYERBOTS_DATABASE_INFO`

### 6.4 `playerbots.conf.dist` omits keys the module reads

`Playerbots.Updates.EnableDatabases`, `PlayerbotsDatabase.WorkerThreads`,
`PlayerbotsDatabase.SynchThreads` are read at startup but have no shipped entries.

### 6.5 `addclass` applies no level filter

`.playerbots bot addclass` picks the first available character of that class at any
level. Harmless because `init=<quality>` builds its factory with `master->GetLevel()`,
but it means the bot arrives underlevelled. `levelup` uses `bot->GetLevel()` and so
will **not** level anything - a trap worth knowing.

---

## 7. Non-obvious constraints

- **Random bots cannot be a raid team.** Every management command beyond
  `add`/`remove` is refused with "You can only use this command on addclass bots".
  Use the addclass pool (`playerbots_account_type`, `account_type = 2`).
- **The worldserver console needs a TTY**, so automation goes through Remote Access on
  loopback 3443 (`scripts/ra.sh`). RA speaks CRLF; bare LF hangs the session.
- **Accounts are created via SRP6** (`scripts/create-account.sh`) rather than the
  console, reproducing `AccountMgr::CreateAccount` exactly.
- **Exactly one system message, first.** gpt-oss's chat template hard-fails with
  "System message must be at the beginning" on a second one.
- **Model output is scrubbed, not trusted.** Reasoning has leaked into `content` and
  reached players; such replies are rejected outright, not truncated.
- **Patch scripts must assert their anchors.** A replacement that silently no-ops
  produced two live bugs at once: an unbound `intent` (HTTP 500 on every ambient
  line) and a memory guard that stopped applying, so bots began recording their own
  unprompted chatter as conversation history.
- **An action must match the words.** The model tagged `[GINVITE]` on the reply
  "you're not in the plans right now" - acting on the tag would have invited someone
  while refusing them. Replies that read as a refusal keep the line but lose the tag.
- **Compose must not carry model defaults.** `VAR: ${VAR:-}` writes an empty string,
  which still counts as set and silently shadows verified defaults.

---

## 8. Licensing note before publishing

AzerothCore and mod-playerbots are AGPL v3. The patch in `patches/llm/` modifies
mod-playerbots and must be published under the same terms, with the source made
available to anyone the server is offered to. `ai-bridge/` is a separate process
communicating over HTTP and is not a derived work, but publishing it alongside costs
nothing and makes the setup reproducible.

No Blizzard client data, DBC files or copyrighted game assets are in this repository.
Client data is downloaded at build time from `wowgaming/client-data`.
