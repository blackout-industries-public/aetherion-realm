"""AI Bridge HTTP API (BRD s21).

The game calls this; this never calls the game. That direction is deliberate - it is
what makes BRD s3.1 hold, that the realm keeps working when the model host is off.
Every failure path here returns a usable answer rather than an error, so the caller
can fire-and-forget and ignore us entirely if we are down.
"""
from __future__ import annotations

import contextlib
import logging
import random
import statistics
from typing import Literal

import httpx
from fastapi import FastAPI, Request, Response
from pydantic import BaseModel, Field

from .config import settings
from .history import HistoryRecorder
from .identity import IdentityStore
from .llm import LLM, Dropped, Priority
from .memory import Memory
from .metrics import prometheus_text
from .links import substitute as substitute_links
from .reflex import canned_chat, canned_event
from . import intents, topics

log = logging.getLogger("bridge")
logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")

CHANNEL_PRIORITY = {
    "whisper": Priority.DIRECT_WHISPER,
    "party": Priority.PARTY,
    "guild": Priority.GUILD,
    "event": Priority.EVENT_REACTION,
    # Bot answering bot. Deliberately above ambient: the opener is what needs pacing,
    # and throttling the reply the same way turns an exchange into two unrelated lines.
    "banter": Priority.EVENT_REACTION,
    # A returning player is a human waiting on a reply, so it ranks with whispers.
    "greet": Priority.DIRECT_WHISPER,
    "ambient": Priority.AMBIENT,
}

# Used whenever inference is unavailable, too slow, or produced nothing usable.
# A bot that shrugs is in character; a bot that goes silent looks broken.
FALLBACKS = ["one sec", "busy right now", "hm?", "cant talk, mid pull", "yeah?"]

identity = IdentityStore()
memory = Memory()
llm = LLM()
history = HistoryRecorder()


@contextlib.asynccontextmanager
async def lifespan(_: FastAPI):
    await identity.start()
    await memory.start()
    await llm.start()
    await history.start()
    log.info("bridge up; llm=%s interactive=%s background=%s effort=%s",
             settings.llm_base_url, settings.model_interactive,
             settings.model_background, settings.reasoning_effort)
    yield
    await history.close()
    await llm.close()
    await memory.close()
    await identity.close()


app = FastAPI(title="Aetherion AI Bridge", version="0.1.0", lifespan=lifespan)


class ChatRequest(BaseModel):
    bot_guid: int | None = None
    bot_name: str | None = None
    speaker: str = Field(min_length=1, max_length=32)
    message: str = Field(min_length=1, max_length=1000)
    channel: Literal["whisper", "party", "guild", "event", "banter", "greet",
                     "ambient"] = "whisper"


class ChatResponse(BaseModel):
    reply: str
    bot: str | None = None
    intent: str | None = None
    source: Literal["llm", "reflex", "fallback", "dropped", "unknown_bot"]


def _phrase(req: "ChatRequest", message: str | None = None) -> str:
    """Render the incoming line as the bot would experience it.

    Channel matters: "someone whispered me" and "someone said this in guild chat"
    warrant different replies, and without the distinction every answer reads like a
    private conversation.
    """
    message = message if message is not None else req.message
    if req.channel in ("ambient", "greet"):
        # Already a directive to the bot, not something a player said. Wrapping it as
        # speech produced replies like "hello again. who are you" - the model answering
        # the instruction instead of acting on it.
        return message
    if req.channel == "event":
        # Already third-person narration of something that happened in the world.
        return f"You just witnessed this: {message} React in one short line."
    verb = {"whisper": "whispers to you",
            "party": "says in party chat",
            "guild": "says in guild chat",
            "banter": "says in the trade channel",
            "event": "-- something happened:"}.get(req.channel, "says")
    return f"{req.speaker} {verb}: {message}"


@app.get("/health")
async def health() -> dict:
    backend, models = "down", []
    try:
        models = await llm.models()
        backend = "up"
    except (httpx.HTTPError, OSError) as exc:
        log.warning("llm backend unreachable: %s", exc)
    return {
        "status": "ok",                      # the bridge itself; see backend below
        "llm_backend": backend,
        "llm_url": settings.llm_base_url,
        "models_available": len(models),
        "model_interactive": settings.model_interactive,
        "model_background": settings.model_background,
        "reasoning_effort": settings.reasoning_effort,
    }


@app.get("/bot/{name}")
async def bot_info(name: str) -> dict:
    bot = await identity.by_name(name)
    if not bot:
        return {"found": False}
    return {
        "found": True, "guid": bot.guid, "name": bot.name, "race": bot.race,
        "class": bot.klass, "level": bot.level, "guild": bot.guild,
        "online": bot.online,
        "personality": {
            "archetype": bot.personality.archetype,
            "temperament": bot.personality.temperament,
            "verbosity": bot.personality.verbosity,
            "interest": bot.personality.interest,
        },
    }


@app.post("/chat", response_model=ChatResponse)
async def chat(req: ChatRequest) -> ChatResponse:
    bot = (await identity.by_guid(req.bot_guid) if req.bot_guid
           else await identity.by_name(req.bot_name or ""))
    if not bot:
        return ChatResponse(reply="", source="unknown_bot")

    priority = CHANNEL_PRIORITY[req.channel]

    # Ambient topic is chosen here, not in the game, so the mix can be retuned without
    # rebuilding the worldserver.
    message = req.message
    item: dict | None = None
    if req.channel == "ambient":
        item = await identity.sellable_item(bot.guid)
        size = await identity.guild_mates(bot.guild) if bot.guild else 0
        topic = topics.choose(has_guild=bool(bot.guild), has_item=bool(item))
        message = topics.render(topic, level=bot.level, guild=bot.guild,
                                item=item["name"] if item else None, guild_size=size)
        llm.scheduler.stats[f"topic_{topic}"] += 1

    # Exactly one system message, and it must come first. Several chat templates
    # (gpt-oss among them) hard-fail with "System message must be at the beginning"
    # on a second one, so relationship context is folded in rather than appended.
    system = bot.system_prompt()
    if rel := await memory.relationship(bot.guid, req.speaker):
        if line := rel.describe(req.speaker):
            system = f"{system}\n{line}"

    # Actions only make sense where a human is addressing the bot directly.
    if req.channel in ("whisper", "party", "guild"):
        system = f"{system}\n{intents.INSTRUCTION}"

    messages = [{"role": "system", "content": system}]
    messages += await memory.history(bot.guid, req.speaker)
    messages.append({"role": "user", "content": _phrase(req, message)})

    # Reflex first. Spending seconds of inference to produce "np" is the worst trade
    # available, and it is a large share of what gets said.
    if (quick := canned_chat(req.message, str(bot.guid))) is not None:
        llm.scheduler.stats["reflex"] += 1
        await memory.record(bot.guid, req.speaker, req.message, quick)
        return ChatResponse(reply=quick, bot=bot.name, source="reflex")

    try:
        reply = await llm.complete(messages, priority=priority, bot_guid=bot.guid,
                                   speaker_name=bot.name)
    except Dropped as exc:
        log.info("dropped %s for %s: %s", req.channel, bot.name, exc)
        return ChatResponse(reply="", bot=bot.name, source="dropped")
    except (httpx.HTTPError, OSError, KeyError, ValueError) as exc:
        # BRD s28: log it, answer anyway, never propagate the failure to the game.
        log.warning("inference failed for %s: %s", bot.name, exc)
        llm.scheduler.stats["errors"] += 1
        return ChatResponse(reply=random.choice(FALLBACKS), bot=bot.name, source="fallback")

    if not reply:
        llm.scheduler.stats["rejected"] += 1
        try:
            retry = messages + [{"role": "system",
                                 "content": "Your previous answer was discarded. Reply with "
                                            "ONLY the words the character types. No commentary."}]
            reply = await llm.complete(retry, priority=priority, bot_guid=bot.guid,
                                       speaker_name=bot.name, bypass_admission=True)
        except (Dropped, httpx.HTTPError, OSError, KeyError, ValueError):
            reply = ""

    if not reply:
        llm.scheduler.stats["empty"] += 1
        return ChatResponse(reply=random.choice(FALLBACKS), bot=bot.name, source="fallback")

    # Actions are only meaningful when a human addressed the bot directly.
    intent = None
    if req.channel in ("whisper", "party", "guild"):
        intent, reply = intents.extract(reply, req.message)
        if intent:
            llm.scheduler.stats[f"intent_{intent}"] += 1

    if not reply:
        return ChatResponse(reply="", bot=bot.name, source="dropped", intent=intent)

    # Applied after sanitising: the escape sequence contains characters the cleaner
    # would otherwise mangle.
    reply = substitute_links(reply, item)

    # Ambient, banter and greet are the bot talking unprompted. Recording them would
    # build a conversation history with itself and drag later replies off-topic.
    if req.channel not in ("ambient", "banter", "greet"):
        await memory.record(bot.guid, req.speaker, req.message, reply)

    llm.scheduler.stats["served"] += 1
    return ChatResponse(reply=reply, bot=bot.name, source="llm", intent=intent)


@app.post("/game/whisper")
async def game_whisper(req: ChatRequest) -> Response:
    """Plain-text variant for the worldserver.

    The core bundles no JSON parser, so it emits JSON and reads back a bare string.
    An empty 204 means "say nothing" - dropped, rate limited, or unknown bot - and the
    game simply stays quiet rather than having to interpret anything.
    """
    result = await chat(req)
    if not result.reply:
        return Response(status_code=204)

    # An intent is announced on a leading "#ACT:" line so the game needs no parser.
    # The server still decides whether the action is legal; this is only a request.
    body = f"#ACT:{result.intent}\n{result.reply}" if result.intent else result.reply
    return Response(content=body, media_type="text/plain; charset=utf-8")


class EventRequest(BaseModel):
    bot_guid: int
    speaker: str = Field(min_length=1, max_length=32)
    event_type: str = Field(min_length=1, max_length=32)
    detail: str = Field(default="", max_length=200)


@app.get("/bot/{name}/history")
async def bot_history(name: str) -> dict:
    """Everything known about one bot: identity, personality, activity, conversation.

    Two very different sources merged into one feed - sampled state changes, and the
    conversations the bridge itself handled.
    """
    bot = await identity.by_name(name)
    if not bot:
        return {"found": False}

    events = await history.events(bot.guid)

    async with memory._pool.acquire() as conn, conn.cursor() as cur:  # noqa: SLF001
        await cur.execute(
            "SELECT speaker, role, content, ts FROM turns WHERE bot_guid=%s "
            "ORDER BY id DESC LIMIT 20", (bot.guid,))
        turns = [{"ts": ts, "kind": "chat",
                  "detail": f"{'said' if role == 'assistant' else speaker + ' said'}: {content}"}
                 for speaker, role, content, ts in await cur.fetchall()]

        await cur.execute(
            "SELECT speaker, exchanges, last_seen FROM relationship WHERE bot_guid=%s "
            "ORDER BY exchanges DESC LIMIT 8", (bot.guid,))
        knows = [{"speaker": sp, "exchanges": n, "last": ls}
                 for sp, n, ls in await cur.fetchall()]

    feed = sorted(events + turns, key=lambda e: e["ts"], reverse=True)[:40]

    return {
        "found": True,
        "guid": bot.guid, "name": bot.name, "race": bot.race, "class": bot.klass,
        "level": bot.level, "guild": bot.guild, "online": bot.online, "zone": bot.zone,
        "personality": {
            "archetype": bot.personality.archetype,
            "temperament": bot.personality.temperament,
            "verbosity": bot.personality.verbosity,
            "interest": bot.personality.interest,
        },
        "knows": knows,
        "feed": feed,
    }


@app.post("/game/event")
async def game_event(req: EventRequest) -> Response:
    """A bot reacts to something that actually happened in the world (BRD s25).

    Grounded in real game state rather than invention, which is the cheapest
    hallucination fix available: the bot is told what happened instead of guessing.
    """
    bot = await identity.by_guid(req.bot_guid)
    if not bot:
        return Response(status_code=204)

    # Most world events deserve a reflex, not a considered opinion.
    if (quick := canned_event(req.event_type, str(bot.guid) + req.detail)) is not None:
        llm.scheduler.stats["reflex"] += 1
        return Response(content=quick, media_type="text/plain; charset=utf-8")

    result = await chat(ChatRequest(bot_guid=req.bot_guid, speaker=req.speaker,
                                    message=req.detail or req.event_type,
                                    channel="event"))
    if not result.reply:
        return Response(status_code=204)
    return Response(content=result.reply, media_type="text/plain; charset=utf-8")


@app.post("/game/greet")
async def game_greet(speaker: str, zone: int = 0) -> Response:
    """Pick a bot that already knows this player and have it say hello.

    Returns "<bot_guid>\n<line>" so the caller does not need a JSON parser. A 204
    means nobody here knows them yet - a greeting from a stranger is just noise.
    """
    candidates = await memory.best_acquaintance(speaker)
    if not candidates:
        return Response(status_code=204)

    online = await identity.online_guids([guid for guid, _ in candidates])
    chosen = next((guid for guid, _ in candidates if guid in online), None)
    if chosen is None:
        return Response(status_code=204)

    bot = await identity.by_guid(chosen)
    if not bot:
        return Response(status_code=204)

    rel = await memory.relationship(bot.guid, speaker)
    exchanges = rel.exchanges if rel else 0
    result = await chat(ChatRequest(
        bot_guid=bot.guid, speaker=speaker, channel="greet",
        message=(f"{speaker}, someone you have spoken with {exchanges} times before, "
                 f"has just logged in. Whisper them a short greeting, in character. "
                 f"Do not ask who they are - you know them.")))
    if not result.reply:
        return Response(status_code=204)

    return Response(content=f"{bot.guid}\n{result.reply}",
                    media_type="text/plain; charset=utf-8")


# response_model=None: FastAPI cannot build a Pydantic model from the
# Response|dict union this route legitimately returns.
@app.get("/metrics", response_model=None)
async def metrics(request: Request) -> Response | dict:
    # One path, two consumers. Prometheus announces itself in Accept
    # (text/plain;version=0.0.4 or openmetrics); the dashboard fetches with a
    # generic Accept and keeps its JSON contract, so neither side needs a
    # config change.
    accept = request.headers.get("accept", "")
    if "text/plain" in accept or "openmetrics" in accept:
        body = await prometheus_text(memory._pool)  # noqa: SLF001
        return Response(content=body,
                        media_type="text/plain; version=0.0.4; charset=utf-8")
    lat = sorted(llm.latencies)
    def pct(p: float) -> float | None:
        return round(lat[min(int(len(lat) * p), len(lat) - 1)], 3) if lat else None
    return {
        "counters": dict(llm.scheduler.stats),
        "samples": len(lat),
        "latency_seconds": {
            "mean": round(statistics.fmean(lat), 3) if lat else None,
            "p50": pct(0.50), "p95": pct(0.95), "p99": pct(0.99),
        },
    }
