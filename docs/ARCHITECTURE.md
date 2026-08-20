# Architecture

## The rule everything follows

**Gameplay never depends on inference.** The realm must be fully playable with the AI
host switched off, unreachable, or returning nonsense. Every design decision below
follows from that.

## Processes

| Process | Stack | Lifecycle |
|---|---|---|
| `ac-authserver`, `ac-worldserver`, `ac-database` | AzerothCore + mod-playerbots | `azerothcore/` compose stack |
| `ai-bridge` | FastAPI | its own stack - may crash or be stopped freely |
| `warcraft-map` | Nuxt | its own stack - read-only view |

Separate compose stacks on purpose: the bridge and dashboard must be restartable
without touching the realm, and must not be able to take it down.

## How chat reaches a model

```
player types  ─▶ OnPlayerCanUseChat (module script)
              ─▶ PlayerbotAI::HandleCommands
                   command parser fails  ─▶ this is conversation, not an order
              ─▶ LlmBridge::Submit        [detached thread - never blocks the world]
              ─▶ POST /game/whisper
              ─▶ Bifrost ─▶ LM Studio
              ─▶ LlmBridge::Drain         [world thread - delivers the reply]
```

Three properties matter:

1. **The command parser gets first refusal.** `follow`, `stay` and `attack` behave
   exactly as before; only what the parser rejects is treated as conversation.
2. **The HTTP call is on a detached thread**, capped by `MaxInFlight`. A slow or dead
   bridge cannot stall the world thread.
3. **Replies are delivered on the world thread**, with both players re-resolved by
   GUID - either may have logged out while inference ran.

## Why the core emits JSON but never parses it

AzerothCore bundles no JSON library. Rather than add one to the game server, the
bridge answers **plain text**, with `204` meaning "say nothing". Intents ride on an
optional leading `#ACT:<intent>` line. The fragile parsing stays outside the process
that must not crash.

## Spending inference only where it counts

At 1500 bots the naive design would generate far more chatter than any local model
can serve, and most of it would have no audience.

- **Witness gate** (core-side) - no request is made unless a real player would
  actually receive the line: within earshot for `/say`, in the guild for guild chat,
  same faction and zone for channels.
- **Claim guard** - one message reaches many bots; the first to claim it answers and
  the rest stay quiet.
- **Reflex table** (bridge-side) - "ty", "grats", "lol" and similar are answered from
  a canned list, costing nothing.
- **Priority budget** - human-directed traffic outranks bot chatter, and chatter is
  dropped rather than queued when capacity runs out.

## Bot identity

Game state is authoritative and read live from `acore_characters`. Personality is
**derived from the character GUID**, not stored - which is what makes it survive a
lost memory volume by construction. Memory (conversations, relationships) lives in
SQLite in the bridge, keyed by the same GUID.

## Parties

Upstream forms groups only between bots that can see each other, which on a large
realm produces two-bot parties and nothing more. `PartyAssembler` builds complete
parties directly: pick a leader, select level- and role-compatible bots anywhere in
the world, and construct the group through the same API the dungeon finder uses.

The party then **travels to the dungeon on foot** using the bots' own RPG movement -
walking, flight paths, terrain routing. Teleporting them in would be simpler and
would look nothing like players.

## Data flow for the dashboard

Nitro server routes query `acore_characters` read-only. Positions are projected using
the client's own `WorldMapArea.dbc` bounds, so dots land exactly where the in-game map
would put them. When the world server restarts, the API serves the last known
snapshot rather than blanking - a dashboard that empties during a restart is useless
precisely when you are watching it.
