# Phase 2 Readiness Notes

Phase 2 (LLM social layer) is gated on Phase 1 acceptance and is **not**
implemented here. This file records only the decisions Phase 1 has already made
that Phase 2 depends on, so nothing has to be undone later.

## What Phase 1 has locked in

- **Bot identity is persistent and addressable.** Random bots are real characters
  in `acore_characters`, so `character_guid` is a stable join key for an external
  personality/memory store. Nothing in Phase 1 recreates bots from scratch on
  restart.
- **The worldserver is reachable non-interactively.** SOAP is enabled and
  published on loopback (7878). That is the seam an AI Bridge can use to push a
  whisper or party line back into the world without linking anything into the
  core.
- **Gameplay has no outbound dependency.** No component of the running realm
  calls out to anything. Powering off a future Bifrost/Ollama host cannot affect
  combat, movement, questing or logins, which is BRD s3.1.

## Built (2026-08-19)

The AI Bridge exists and works: `ai-bridge/`, deployed as its own compose stack on
port 8090 (loopback only). Backed by LM Studio on the MacBook at 10.10.42.46:1234.

Verified end to end:

- Identity resolved live from `acore_characters`; personality derived from the
  character GUID, so it is stable by construction rather than by storage.
- Multi-turn conversation with working recall ("what did i say my spec was?" ->
  "ret pally. stop testing me lol").
- Memory and personality both survive a container restart.
- Distinct, consistent personalities across bots.
- Latency ~1.5-5 s; the BRD's sub-3 s direct-reply target is met at the low end.
- Backend failures degrade to canned in-character replies, never to errors.

## What Phase 2 still has to build

**The game-side hook** - the only remaining piece, and the only one that touches the
core. Today the bridge is driven by HTTP calls made by hand; nothing in the game
calls it yet.

The seam is `mod-playerbots/src/Script/Playerbots.cpp`, in
`OnPlayerCanUseChat(Player* player, uint32 type, uint32 lang, std::string& msg,
Player* receiver)`. That hook already receives the sender, the message and the bot
being whispered, which is exactly the payload `/chat` wants.

Two hard requirements for that patch:

1. It must not block the world thread. The HTTP call belongs on a worker thread with
   the result delivered back through the bot's normal chat path - never a synchronous
   call inside the chat hook.
2. A bridge that is down, slow or returning nonsense must be indistinguishable from a
   bridge that was never installed.

Bifrost is not in the path yet. It does not need to be: the bridge speaks
OpenAI-compatible HTTP, and Bifrost serves the same shape, so adopting it is a change
to `LLM_BASE_URL` and nothing else.

## Constraint worth stating early

The LLM never receives authority over the server. Responses are language only,
validated against an allow-listed intent schema. Group joins, invites and any
game action remain deterministic Playerbot logic (BRD s29).
