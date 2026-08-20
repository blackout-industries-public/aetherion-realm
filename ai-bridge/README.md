# AI Bridge

Phase 2 component from BRD s21. Sits between the game and the model host, owns all
game-specific intelligence, and exposes one HTTP API.

```
worldserver --> AI Bridge --> LM Studio (OpenAI-compatible) --> model
                    |
                    +-- acore_characters   (identity, read-only)
                    +-- /data/memory.sqlite (conversation + relationships)
```

The arrow only points one way. The bridge never calls the game, which is what makes
BRD s3.1 true: turn the MacBook off and the realm is unaffected.

## API

| Endpoint | Purpose |
|---|---|
| `GET /health` | Bridge status plus live backend reachability |
| `GET /bot/{name}` | Resolved identity and derived personality |
| `POST /chat` | `{bot_name\|bot_guid, speaker, message, channel}` -> `{reply, source}` |
| `GET /metrics` | Counters and latency percentiles (BRD s32) |

`source` is one of `llm`, `fallback`, `dropped`, `unknown_bot`. The caller can always
ignore the response; it never receives an error status for a backend problem.

## Running

```bash
cd /opt/warcraft/ai-bridge
docker compose up -d --build
curl -s localhost:8090/health
```

Config is env-driven (`LLM_BASE_URL`, `MODEL_FAST`, `MODEL_QUALITY`, timeouts, rate
limits). Deployed as its own compose stack so it shares no lifecycle with the realm.

## Model selection - read before changing MODEL_*

Two models, split by whether a human is waiting:

| Setting | Used for | Needs |
|---|---|---|
| `MODEL_INTERACTIVE` | whisper, party | low latency - a person is staring at the screen |
| `MODEL_BACKGROUND` | guild, ambient | rate limited anyway, so latency is invisible |

Most locally hosted models are reasoning models: they emit chain-of-thought into
`reasoning_content`, spend the whole token budget on it and return **empty**
`content`. Measured on this host, same prompt, `reasoning_effort: low`:

| Model | Latency | Reasoning chars | Usable |
|---|---|---|---|
| `openai-gpt-oss-20b-...` | **1.6 s** | 0 | yes |
| `qwen/qwen3.8-27b` | 11.7 s | 489 | yes, but slow |
| `glm-4.7-flash-...` | 10.2 s | 2063 | yes, but slow |
| `qwen/qwen3.6-35b-a3b` | 12.2 s | 2682 | no - empty content |
| `google/gemma-4-26b-a4b` | 15.3 s | 2664 | no - empty content |

Before adopting a model, confirm it returns non-empty `content` with
`reasoning_tokens == 0`. Keep `max_tokens` generous (800): starving the budget
produces empty replies with `finish_reason=length`, not shorter ones.

`REASONING_EFFORT` is the single biggest latency lever (6.5 s -> 1.6 s on gpt-oss).
It is the OpenAI field, so only `low`/`medium`/`high` are valid - LM Studio rejects
`off` with HTTP 400 even though its own per-model setting uses `on`/`off`, and it
logs a warning when a model does not understand the value it was given.

## Output is scrubbed, not trusted

Seen live in `/say`, published to a player:

> [Soldeyn] says: The user is roleplaying a battleground scenario. I need to
> respond as Soldeyn, a cheerful Blood Elf Hunter focused on BGs...

The model emitted its reasoning as ordinary `content`, not `reasoning_content`, so
nothing upstream flagged it and the length cap merely truncated it into shorter
garbage. `_META` in `llm.py` now rejects such replies outright rather than trimming
them, and a rejected reply gets one retry with a blunter instruction before falling
back to a canned line.

Rejection beats truncation here: half a leaked monologue is still a leaked monologue.
The patterns are anchored on giveaway phrasing ("the user is", "i need to respond",
"my response should") and are checked against benign lines that must survive, such as
"the user interface addon keeps breaking".

## A trap worth remembering

`docker-compose.yml` must not carry model defaults. `VAR: ${VAR:-}` writes an empty
string rather than omitting the variable, and an empty string still counts as "set" -
so a compose default silently shadows the verified default in `config.py`. That is
why `_str()` treats empty as unset, and why compose no longer names a model at all.

## Constraints that are not obvious

- **Exactly one system message, first.** gpt-oss's chat template hard-fails with
  "System message must be at the beginning" on a second one, so relationship context
  is folded into the single system prompt rather than appended.
- **Output is scrubbed, not trusted.** Models prefix the speaker name, wrap replies in
  quotes, narrate in asterisks and occasionally break character. `sanitize()` strips
  all of it and returns empty on a refusal, which becomes a canned fallback.
- **Low priority work is dropped, not queued.** A human whisper must never wait behind
  bot chatter, so ambient traffic is rejected outright once capacity is gone.
