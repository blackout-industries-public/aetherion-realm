#include "LlmBridge.h"

#include "Channel.h"
#include "ChannelMgr.h"
#include "Config.h"
#include "GameTime.h"
#include "Log.h"
#include "DBCStores.h"
#include "LFGMgr.h"
#include "ObjectAccessor.h"
#include "Group.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PartyAssembler.h"
#include "Playerbots.h"   // GET_PLAYERBOT_AI
#include "RandomPlayerbotMgr.h"
#include "Random.h"
#include "SharedDefines.h"
#include "World.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <boost/asio/ip/tcp.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <functional>
#include <vector>
#include <sstream>
#include <thread>

namespace
{
    // The core bundles no JSON parser, so the bridge answers in plain text and the
    // only JSON here is what we emit. Escaping is therefore the whole problem.
    std::string JsonEscape(std::string const& in)
    {
        std::ostringstream out;
        for (char const c : in)
        {
            switch (c)
            {
                case '"':  out << "\\\""; break;
                case '\\': out << "\\\\"; break;
                case '\n': out << "\\n";  break;
                case '\r': out << "\\r";  break;
                case '\t': out << "\\t";  break;
                default:
                    // Control bytes are illegal raw inside a JSON string and a player
                    // can put them in a message, so they are dropped rather than sent.
                    if (static_cast<unsigned char>(c) >= 0x20)
                        out << c;
                    break;
            }
        }
        return out.str();
    }

    uint64 NowMs()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
    }
}

LlmBridge* LlmBridge::instance()
{
    static LlmBridge instance;
    return &instance;
}

void LlmBridge::LoadConfig()
{
    // Default off. Enabling inference is an explicit decision, never a side effect
    // of installing this build.
    _enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.Enabled", false);
    _host = sConfigMgr->GetOption<std::string>("AiPlayerbot.Llm.Host", "ai-bridge");
    _port = std::to_string(sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.Port", 8090));
    _timeoutMs = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.TimeoutMs", 15000);
    _maxInFlight = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.MaxInFlight", 8);
    _claimWindowMs = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.ClaimWindowMs", 10000);
    _requireWitness = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.RequireHumanWitness", true);
    _sayRange = sConfigMgr->GetOption<float>("AiPlayerbot.Llm.SayRange", 45.0f);
    _sameFactionOnly = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.SameFactionOnly", true);

    _reactWhisper = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.ReactWhisper", true);
    _reactParty = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.ReactParty", true);
    _reactGuild = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.ReactGuild", true);
    _reactSay = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.ReactSay", false);
    _channelReplyChance = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.ChannelReplyChance", 5);
    _sayReplyChance = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.SayReplyChance", 25);

    _ambientEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.AmbientEnabled", false);
    _ambientIntervalMs = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.AmbientIntervalMs", 300000);
    _ambientMaxDepth = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.AmbientMaxDepth", 1);
    _ambientUseSay = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.AmbientUseSay", true);

    _guildAdEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.GuildAdEnabled", true);
    _guildAdIntervalMs = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.GuildAdIntervalMs", 420000);
    _guildAdTimer = 0;
    _greetOnLogin = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.GreetOnLogin", true);
    _greetDelayMs = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.GreetDelayMs", 15000);
    _eventsEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Llm.EventsEnabled", true);
    _eventChanceLevelUp = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.EventChanceLevelUp", 100);
    _eventChanceDeath = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.EventChanceDeath", 40);
    _eventChanceLoot = sConfigMgr->GetOption<int32>("AiPlayerbot.Llm.EventChanceLoot", 35);
    _ambientTimer = 0;

    if (_enabled)
        LOG_INFO("playerbots",
                 "LLM bridge enabled -> {}:{} (timeout {} ms, max in flight {}, ambient {})",
                 _host, _port, _timeoutMs, _maxInFlight, _ambientEnabled ? "on" : "off");
}

bool LlmBridge::WantsChatType(uint32 chatType) const
{
    switch (chatType)
    {
        case CHAT_MSG_WHISPER:
            return _reactWhisper;
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            return _reactParty;
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
            return _reactGuild;
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
            return _reactSay;
        default:
            return false;
    }
}

bool LlmBridge::RollChannelReply() const
{
    return _channelReplyChance > 0 && urand(1, 100) <= _channelReplyChance;
}

bool LlmBridge::RollSayReply() const
{
    return _sayReplyChance > 0 && urand(1, 100) <= _sayReplyChance;
}

Player* LlmBridge::PickResponder(Player* near) const
{
    // RandomPlayerbotMgr::GetRandomPlayer() returns a REAL player, not a bot - using
    // it here silently disabled ambient chatter entirely.
    if (!near)
        near = sRandomPlayerbotMgr.GetRandomPlayer();

    uint32 const zone = near ? near->GetZoneId() : 0;

    // Preference order matters: chatter the player cannot see does nothing for how
    // populated the world feels. Nearby beats same-zone beats anywhere.
    std::vector<Player*> nearby, sameZone, anywhere;

    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || bot->isDead() || bot == near)
            continue;
        if (!GET_PLAYERBOT_AI(bot))
            continue;

        if (_sameFactionOnly && near && bot->GetTeamId() != near->GetTeamId())
            continue;

        if (zone && bot->GetZoneId() == zone)
        {
            // /say carries about 25 yards; 40 keeps the pool from being empty while
            // still landing in or near earshot.
            if (near && bot->GetMapId() == near->GetMapId() && bot->GetDistance(near) <= 40.0f)
                nearby.push_back(bot);
            else
                sameZone.push_back(bot);
        }
        else if (anywhere.size() < 64)   // enough for a fallback pick; no need to list 1500
            anywhere.push_back(bot);
    }

    std::vector<Player*> const& pool =
        !nearby.empty() ? nearby : (!sameZone.empty() ? sameZone : anywhere);
    if (pool.empty())
        return nullptr;

    return pool[urand(0, pool.size() - 1)];
}

bool LlmBridge::HasHumanWitness(Player* bot, uint32 chatType) const
{
    if (!_requireWitness || !bot)
        return true;

    // GetPlayers() is the real-player list. It is tiny on a private realm, so this
    // check costs nothing compared to the inference it avoids.
    std::vector<Player*> humans = sRandomPlayerbotMgr.GetPlayers();
    if (humans.empty())
        return false;

    for (Player* human : humans)
    {
        if (!human || !human->IsInWorld() || GET_PLAYERBOT_AI(human))
            continue;

        switch (chatType)
        {
            case CHAT_MSG_SAY:
            case CHAT_MSG_YELL:
                // Same faction required. A Horde bot's line reaches an Alliance player
                // as gibberish at best, so generating it spends inference on something
                // nobody can read. Playerbots' own built-in chatter still fills the
                // world with cross-faction noise for free.
                if (_sameFactionOnly && human->GetTeamId() != bot->GetTeamId())
                    break;
                if (human->GetMapId() == bot->GetMapId() && human->GetDistance(bot) <= _sayRange)
                    return true;
                break;
            case CHAT_MSG_GUILD:
            case CHAT_MSG_OFFICER:
                if (bot->GetGuildId() && human->GetGuildId() == bot->GetGuildId())
                    return true;
                break;
            case CHAT_MSG_CHANNEL:
                // City channels are both zone scoped and faction scoped.
                if (_sameFactionOnly && human->GetTeamId() != bot->GetTeamId())
                    break;
                if (human->GetZoneId() == bot->GetZoneId())
                    return true;
                break;
            case CHAT_MSG_PARTY:
            case CHAT_MSG_PARTY_LEADER:
            case CHAT_MSG_RAID:
            case CHAT_MSG_RAID_LEADER:
                if (bot->GetGroup() && bot->GetGroup()->IsMember(human->GetGUID()))
                    return true;
                break;
            default:
                return true;   // whispers always have a human on one end
        }
    }

    return false;
}

bool LlmBridge::TryClaim(ObjectGuid speaker, uint32 chatType, std::string const& message)
{
    // Case-folded: the same say reaches this through two paths, one of which
    // lowercases first, and "Need party!" vs "need party!" earned two claims
    // and two repliers.
    std::string folded(message);
    std::transform(folded.begin(), folded.end(), folded.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    std::size_t key = std::hash<std::string>{}(folded);
    key ^= std::hash<uint64>{}(speaker.GetRawValue()) + 0x9e3779b9 + (key << 6) + (key >> 2);
    // chatType is deliberately NOT part of the key. One /say reaches bots both
    // through the say hook (typed SAY) and each bot's command queue (typed
    // WHISPER), and keying on type let both paths claim - two bots answered
    // every say. A speaker repeating the identical line on two channels inside
    // the claim window and wanting two answers is not a real case.
    (void)chatType;

    uint64 const now = NowMs();

    std::lock_guard<std::mutex> guard(_claimMutex);

    // Opportunistic sweep. The map only ever holds recent messages, so this stays
    // small without needing a separate timer.
    if (_claims.size() > 512)
        for (auto it = _claims.begin(); it != _claims.end();)
            it = (now - it->second > _claimWindowMs) ? _claims.erase(it) : std::next(it);

    auto const existing = _claims.find(key);
    if (existing != _claims.end() && now - existing->second <= _claimWindowMs)
        return false;

    _claims[key] = now;
    return true;
}

void LlmBridge::Submit(Player* bot, Player* speaker, std::string const& message, uint32 chatType,
                       uint32 channelId, uint32 depth)
{
    if (!_enabled || !bot || !speaker || message.empty())
        return;

    if (!HasHumanWitness(bot, chatType))
        return;

    // Bound concurrency here as well as in the bridge: this cap protects the
    // worldserver's thread count, which the bridge knows nothing about.
    if (_inFlight.load() >= _maxInFlight)
        return;

    // One in-flight reply per REAL speaker, regardless of message text. The
    // claim map dedupes identical text, but the same say reaches bots through
    // paths that mangle it differently (the command parser claims its leftover
    // substring), and every such alias produced a second replier. Two answers
    // to one human never make sense; sequential questions still flow because
    // the slot frees the moment the reply is delivered. Bots keep their own
    // lane so banter depth is unaffected.
    if (depth == 0 && !GET_PLAYERBOT_AI(speaker))
    {
        uint64 const now = NowMs();
        std::lock_guard<std::mutex> lock(_speakerBusyMutex);
        auto it = _speakerBusy.find(speaker->GetGUID().GetRawValue());
        // Self-healing: a slot older than the bridge timeout belongs to a
        // reply that died in transit and must not gag the speaker forever.
        if (it != _speakerBusy.end() && now - it->second < uint64(_timeoutMs) + 5000)
            return;
        _speakerBusy[speaker->GetGUID().GetRawValue()] = now;
    }

    _inFlight.fetch_add(1);

    // What this bot last said out loud, so a reply to its own broadcast lands
    // with the thought still in hand - "me" answering "who wants to team up"
    // used to draw a blank because the canned announcer, not the model, asked.
    std::string recentSay;
    {
        std::lock_guard<std::mutex> lock(_broadcastMutex);
        auto it = _lastBroadcast.find(bot->GetGUID().GetRawValue());
        if (it != _lastBroadcast.end() && NowMs() - it->second.second < 180000)
            recentSay = it->second.first;
    }

    std::thread(&LlmBridge::Worker, this, bot->GetGUID(), speaker->GetGUID(),
                std::string(bot->GetName()), std::string(speaker->GetName()), message,
                chatType, channelId, depth, recentSay)
        .detach();
}

void LlmBridge::NoteBroadcast(Player* bot, std::string const& line)
{
    if (!_enabled || !bot || line.empty())
        return;
    std::lock_guard<std::mutex> lock(_broadcastMutex);
    _lastBroadcast[bot->GetGUID().GetRawValue()] = {line, NowMs()};
}

char const* LlmBridge::ChannelLabel(uint32 chatType, uint32 depth) const
{
    // A reply to another bot is banter, not an unprompted opener. It gets its own
    // budget so an exchange reads as a conversation instead of arriving a minute late.
    if (depth > 0)
        return "banter";

    switch (chatType)
    {
        case CHAT_MSG_WHISPER:
            return "whisper";
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            return "party";
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
            return "guild";
        case CHAT_MSG_CHANNEL:
            return "guild";  // shares the guild budget: public, but not human-directed
        case CHAT_MSG_SAY:
        case CHAT_MSG_YELL:
            // Its own label: "ambient" never carries the action instruction,
            // and a spoken ask is the most direct ask there is.
            return "say";
        default:
            return "ambient";
    }
}

bool LlmBridge::HttpPost(std::string const& path, std::string const& json, std::string& body)
{
    try
    {
        boost::asio::ip::tcp::iostream stream;
        stream.expires_after(std::chrono::milliseconds(_timeoutMs));
        stream.connect(_host, _port);
        if (!stream)
            return false;

        // Connection: close lets the body be read to EOF, which avoids implementing
        // Content-Length or chunked decoding inside the game server.
        stream << "POST " << path << " HTTP/1.1\r\n"
               << "Host: " << _host << ":" << _port << "\r\n"
               << "Content-Type: application/json\r\n"
               << "Content-Length: " << json.size() << "\r\n"
               << "Connection: close\r\n\r\n"
               << json << std::flush;

        std::ostringstream received;
        received << stream.rdbuf();
        std::string const response = received.str();

        std::size_t const split = response.find("\r\n\r\n");
        if (split == std::string::npos)
            return false;

        // 204 is the bridge saying "stay quiet" - a normal outcome, not a failure.
        if (response.compare(0, 12, "HTTP/1.1 200") != 0)
            return false;

        body = response.substr(split + 4);
        while (!body.empty() && (body.back() == '\n' || body.back() == '\r'))
            body.pop_back();

        return !body.empty();
    }
    catch (std::exception const& e)
    {
        // Expected and unremarkable: the bridge may simply be switched off.
        LOG_DEBUG("playerbots", "LLM bridge {} failed: {}", path, e.what());
        return false;
    }
}

void LlmBridge::QueueReply(Reply const& reply)
{
    std::lock_guard<std::mutex> guard(_mutex);
    // Never let a stalled world thread grow this without bound.
    if (_replies.size() < 256)
        _replies.push_back(reply);
}

void LlmBridge::Worker(ObjectGuid botGuid, ObjectGuid speakerGuid, std::string botName,
                       std::string speakerName, std::string message, uint32 chatType,
                       uint32 channelId, uint32 depth, std::string recentSay)
{
    std::ostringstream payload;
    payload << "{\"bot_guid\":" << botGuid.GetCounter()
            << ",\"speaker\":\"" << JsonEscape(speakerName)
            << "\",\"message\":\"" << JsonEscape(message)
            << "\",\"channel\":\"" << ChannelLabel(chatType, depth) << "\"";
    if (!recentSay.empty())
        payload << ",\"recent_say\":\"" << JsonEscape(recentSay) << "\"";
    payload << "}";

    std::string body;
    if (!HttpPost("/game/whisper", payload.str(), body))
    {
        FreeSpeaker(speakerGuid);
        _inFlight.fetch_sub(1);
        return;
    }

    // An optional leading "#ACT:<intent>" line announces an action the bot agreed to.
    // Parsed here rather than in Deliver so the world thread only ever sees fields.
    std::string intent;
    if (body.rfind("#ACT:", 0) == 0)
    {
        std::size_t const nl = body.find('\n');
        if (nl != std::string::npos)
        {
            intent = body.substr(5, nl - 5);
            body = body.substr(nl + 1);
        }
    }

    if (!body.empty())
        QueueReply({botGuid, speakerGuid, body, chatType, channelId, depth, intent});
    else
        FreeSpeaker(speakerGuid);

    _inFlight.fetch_sub(1);
}

void LlmBridge::FreeSpeaker(ObjectGuid speaker)
{
    std::lock_guard<std::mutex> lock(_speakerBusyMutex);
    _speakerBusy.erase(speaker.GetRawValue());
}

void LlmBridge::GreetWorker(ObjectGuid humanGuid, std::string humanName)
{
    // The bridge owns relationship history, so it chooses the greeter and answers
    // "<bot_guid>\n<line>" - keeping JSON parsing out of the game server entirely.
    std::string body;
    if (HttpPost("/game/greet?speaker=" + humanName, "{}", body))
    {
        std::size_t const nl = body.find('\n');
        if (nl != std::string::npos)
        {
            uint32 const guidLow = atoi(body.substr(0, nl).c_str());
            std::string const line = body.substr(nl + 1);
            if (guidLow && !line.empty())
                QueueReply({ObjectGuid::Create<HighGuid::Player>(guidLow), humanGuid,
                            line, CHAT_MSG_WHISPER, 0, 0});
        }
    }

    _inFlight.fetch_sub(1);
}

void LlmBridge::EventWorker(ObjectGuid botGuid, ObjectGuid humanGuid, std::string humanName,
                            std::string eventType, std::string detail)
{
    std::ostringstream payload;
    payload << "{\"bot_guid\":" << botGuid.GetCounter()
            << ",\"speaker\":\"" << JsonEscape(humanName)
            << "\",\"event_type\":\"" << JsonEscape(eventType)
            << "\",\"detail\":\"" << JsonEscape(detail) << "\"}";

    std::string body;
    if (HttpPost("/game/event", payload.str(), body))
        QueueReply({botGuid, humanGuid, body, CHAT_MSG_SAY, 0, 0});

    _inFlight.fetch_sub(1);
}

void LlmBridge::OnHumanLogin(Player* human)
{
    if (!_enabled || !_greetOnLogin || !human || GET_PLAYERBOT_AI(human))
        return;

    _greets.push_back({human->GetGUID(), human->GetName(), _greetDelayMs});
}

void LlmBridge::OnGameEvent(Player* human, std::string const& eventType,
                            std::string const& detail)
{
    if (!_enabled || !_eventsEnabled || !human || GET_PLAYERBOT_AI(human))
        return;

    uint32 chance = 100;
    if (eventType == "died")
        chance = _eventChanceDeath;
    else if (eventType == "loot")
        chance = _eventChanceLoot;
    else if (eventType == "levelup")
        chance = _eventChanceLevelUp;

    if (chance < 100 && urand(1, 100) > chance)
        return;

    Player* bot = PickResponder(human);
    if (!bot || _inFlight.load() >= _maxInFlight)
        return;

    _inFlight.fetch_add(1);
    std::thread(&LlmBridge::EventWorker, this, bot->GetGUID(), human->GetGUID(),
                std::string(human->GetName()), eventType, detail)
        .detach();
}

void LlmBridge::Deliver(Reply const& reply)
{
    // Re-resolved rather than carried across the thread boundary: either player may
    // have logged out while inference was running.
    Player* bot = ObjectAccessor::FindConnectedPlayer(reply.botGuid);
    if (!bot)
        return;

    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI)
        return;

    switch (reply.chatType)
    {
        case CHAT_MSG_WHISPER:
        {
            Player* speaker = ObjectAccessor::FindConnectedPlayer(reply.speakerGuid);
            if (speaker)
                bot->Whisper(reply.text, LANG_UNIVERSAL, speaker);
            break;
        }
        case CHAT_MSG_PARTY:
        case CHAT_MSG_PARTY_LEADER:
            botAI->SayToParty(reply.text);
            break;
        case CHAT_MSG_RAID:
        case CHAT_MSG_RAID_LEADER:
            botAI->SayToRaid(reply.text);
            break;
        case CHAT_MSG_GUILD:
        case CHAT_MSG_OFFICER:
            botAI->SayToGuild(reply.text);
            break;
        case CHAT_MSG_CHANNEL:
        {
            // SayToChannel fails silently when the bot has not joined that zone's
            // instance of the channel - which is the normal state for a bot that was
            // teleported in. Falling back to /say keeps the line visible instead of
            // dropping it into nothing.
            ChatChannelId const id = static_cast<ChatChannelId>(
                reply.channelId ? reply.channelId : ChatChannelId::GENERAL);
            if (!botAI->SayToChannel(reply.text, id))
                botAI->Say(reply.text);
            break;
        }
        default:
            botAI->Say(reply.text);
            break;
    }

    if (!reply.intent.empty())
        if (Player* speaker = ObjectAccessor::FindConnectedPlayer(reply.speakerGuid))
            ExecuteIntent(bot, speaker, reply.intent);

    // One bot answering another is what turns chatter into conversation. Depth is
    // capped so this terminates rather than feeding itself. Applies to /say too,
    // otherwise local ambient lines would never get a reply.
    if ((reply.chatType == CHAT_MSG_CHANNEL || reply.chatType == CHAT_MSG_SAY) &&
        reply.depth < _ambientMaxDepth)
    {
        if (Player* other = PickResponder(bot))
            Submit(other, bot, reply.text, reply.chatType, reply.channelId, reply.depth + 1);
    }
}

void LlmBridge::ExecuteIntent(Player* bot, Player* speaker, std::string const& intent)
{
    PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
    if (!botAI || !speaker)
        return;

    // INFO on purpose: every accepted tag should be visible in the log, so a
    // reply that promised an act and did nothing is diagnosable in seconds.
    LOG_INFO("playerbots", "LLM intent '{}': {} for {}", intent, bot->GetName(),
             speaker->GetName());

    // Every precondition is re-checked here. The model's tag is a request; whether it
    // is legal is decided entirely by the server, so a hallucinated tag can at worst
    // be ignored - it can never grant the model authority it does not have.
    if (intent == "guild_invite")
    {
        if (!bot->GetGuildId() || speaker->GetGuildId())
            return;

        Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
        if (!guild || !guild->HasRankRight(bot, GR_RIGHT_INVITE))
            return;

        if (bot->GetTeamId() != speaker->GetTeamId() &&
            !sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_GUILD))
            return;

        guild->HandleInviteMember(bot->GetSession(), speaker->GetName());
        LOG_DEBUG("playerbots", "LLM intent: {} invited {} to guild", bot->GetName(),
                  speaker->GetName());
        return;
    }

    if (intent == "party_invite")
    {
        if (speaker->GetGroup() && bot->GetGroup() == speaker->GetGroup())
            return;
        if (speaker->GetGroup())
            return;   // already grouped elsewhere; inviting would fail anyway

        WorldPacket p;
        uint32 rolesMask = 0;
        p << speaker->GetName();
        p << rolesMask;
        bot->GetSession()->HandleGroupInviteOpcode(p);
        LOG_DEBUG("playerbots", "LLM intent: {} invited {} to group", bot->GetName(),
                  speaker->GetName());
        return;
    }

    // The movement intents deliberately go through the bot's own command parser - the
    // same path a player whispering "come" would take, with the same security checks.
    // The model therefore gains no capability a player does not already have.
    if (intent == "come" || intent == "follow" || intent == "stay" || intent == "buff")
    {
        // Pointer overload on purpose: the reference-taking overload is private.
        botAI->HandleCommand(CHAT_MSG_WHISPER, intent, speaker);
        return;
    }

    // Leadership goes through the module's own transfer command - the same
    // checks a whispered "give leader" would run. The command only works from
    // the ACTUAL leader, and the claimed bot usually is not it - "pass me
    // leader" got an "all yours" from a member while the crown never moved.
    // Any bot can hear the ask; the one that holds the lead executes it.
    if (intent == "give_lead")
    {
        Player* holder = bot;
        if (Group* group = bot->GetGroup())
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            if (!leader || !GET_PLAYERBOT_AI(leader))
                return;   // a human leads; their crown is not the bot's to hand over
            holder = leader;
        }
        if (PlayerbotAI* holderAI = GET_PLAYERBOT_AI(holder))
            holderAI->HandleCommand(CHAT_MSG_WHISPER, "give leader", speaker);
        return;
    }

    // Queue the group for the dungeon finder. Same leader rule as the
    // battleground queue below; the slot is the era's RANDOM dungeon, so the
    // finder picks the instance and everyone gets the role-check dialog. The
    // packet recipe mirrors the module's own LfgJoinAction.
    if (intent == "queue_dungeon")
    {
        Player* joiner = bot;
        if (Group* group = bot->GetGroup())
        {
            Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            if (!leader)
                return;
            if ((speaker && leader == speaker) || GET_PLAYERBOT_AI(leader))
                joiner = leader;
            else
                return;
        }
        uint32 const level = joiner->GetLevel();
        uint32 slot = 0;
        uint32 bestMin = 0;
        for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
        {
            LFGDungeonEntry const* entry = sLFGDungeonStore.LookupEntry(i);
            // Normal-difficulty randoms only: a heroic random hard-fails on
            // any undergeared member and the error is easy to miss.
            if (!entry || entry->TypeID != lfg::LFG_TYPE_RANDOM || entry->Difficulty != 0)
                continue;
            if (level < entry->MinLevel || level > entry->MaxLevel)
                continue;
            if (entry->MinLevel >= bestMin)
            {
                bestMin = entry->MinLevel;
                slot = entry->ID;
            }
        }
        if (!slot)
            return;
        uint32 const roles = lfg::PLAYER_ROLE_LEADER |
                             (PlayerbotAI::IsTank(joiner)   ? lfg::PLAYER_ROLE_TANK
                              : PlayerbotAI::IsHeal(joiner) ? lfg::PLAYER_ROLE_HEALER
                                                            : lfg::PLAYER_ROLE_DAMAGE);
        WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
        *data << roles;
        *data << bool(false);
        *data << bool(false);
        *data << uint8(1);
        *data << slot;
        *data << uint8(3) << uint8(0) << uint8(0) << uint8(0);
        *data << std::string();
        joiner->GetSession()->QueuePacket(data);
        LOG_INFO("playerbots", "LLM intent: {} starts dungeon queue slot {} (joiner {})",
                 bot->GetName(), slot, joiner->GetName());
        return;
    }

    // "Lead us through": the assembler adopts the speaker's live group as a
    // steered run - bot leader walks boss to boss, wipe determination and the
    // run ledger apply, and the player follows the caravan.
    if (intent == "lead_run")
    {
        if (!sPartyAssembler->AdoptRun(speaker, bot))
            LOG_INFO("playerbots",
                     "LLM intent: lead_run for {} declined (no group, not a known "
                     "dungeon, or no bot to promote)", speaker->GetName());
        return;
    }

    // Queue for a battleground through the same packet the PvP panel sends.
    // A free bot queues itself. A bot in the SPEAKER'S group honours the ask
    // the WoW way: the queue belongs to the leader, so the group join fires
    // from the leader's own session - at the leader's own spoken request -
    // and everyone queues together; bots take the invite, the player clicks
    // the popup.
    if (intent == "queue_bg")
    {
        Group* group = bot->GetGroup();
        Player* joiner = bot;
        uint8 asGroup = 0;
        if (group)
        {
            // Whoever leads starts the queue: the speaker when they lead, the
            // bot itself when it does, or the group's bot leader on the ask's
            // behalf. Only a DIFFERENT human leading stops the action - the
            // ask is not theirs to be volunteered for.
            Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
            if (!leader)
                return;
            if (speaker && leader == speaker)
                joiner = speaker;
            else if (GET_PLAYERBOT_AI(leader))
                joiner = leader;
            else
                return;
            asGroup = 1;
        }
        if (joiner->InBattleground() || joiner->InBattlegroundQueue())
        {
            LOG_INFO("playerbots", "LLM intent: {} already queued or inside - queue ask ignored",
                     joiner->GetName());
            return;
        }
        uint8 const level = joiner->GetLevel();
        // Group joins avoid the random battleground: its group path is the
        // core's flakiest and rejects silently. Alterac always seats a group.
        uint32 const bgType = asGroup && level >= 51 ? uint32(BATTLEGROUND_AV)
                              : level >= 80 ? uint32(BATTLEGROUND_RB)
                              : level >= 61 ? uint32(BATTLEGROUND_EY)
                              : level >= 51 ? uint32(BATTLEGROUND_AV)
                              : level >= 20 ? uint32(BATTLEGROUND_AB)
                                            : uint32(BATTLEGROUND_WS);
        WorldPacket* packet = new WorldPacket(CMSG_BATTLEMASTER_JOIN, 8 + 4 + 4 + 1);
        *packet << joiner->GetGUID();
        *packet << bgType;
        *packet << uint32(0);
        *packet << uint8(asGroup);
        joiner->GetSession()->QueuePacket(packet);
        // INFO on purpose: this is the observable proof the ask became an act.
        LOG_INFO("playerbots", "LLM intent: {} starts bg queue {} (asGroup {}, joiner {})",
                 bot->GetName(), bgType, uint32(asGroup), joiner->GetName());
    }
}

void LlmBridge::TickAmbient(uint32 diff)
{
    if (!_ambientEnabled)
        return;

    _ambientTimer += diff;
    if (_ambientTimer < _ambientIntervalMs)
        return;

    _ambientTimer = 0;

    Player* bot = PickResponder();
    if (!bot)
        return;

    // The bot is both speaker and subject here: nobody prompted this, it is the bot
    // deciding to say something unprompted.
    if (_ambientUseSay)
        Submit(bot, bot, "Say one short unprompted line out loud to whoever is nearby, "
                         "in character, about what you are doing right now.",
               CHAT_MSG_SAY, 0, 0);
    else
        Submit(bot, bot, "Say one short unprompted line in the public trade channel, "
                         "in character, about what you are doing right now.",
               CHAT_MSG_CHANNEL, ChatChannelId::TRADE, 0);
}

void LlmBridge::TickGuildAds(uint32 diff)
{
    if (!_guildAdEnabled)
        return;

    _guildAdTimer += diff;
    if (_guildAdTimer < _guildAdIntervalMs)
        return;
    _guildAdTimer = 0;

    // An officer with invite rights, so a "me!" answered in the channel can
    // become a real invite through the normal intent path.
    std::vector<Player*> officers;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || bot->isDead() || !bot->GetGuildId())
            continue;
        if (!GET_PLAYERBOT_AI(bot))
            continue;
        Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
        if (!guild || !guild->HasRankRight(bot, GR_RIGHT_INVITE))
            continue;
        officers.push_back(bot);
        if (officers.size() >= 64)
            break;
    }
    if (officers.empty())
        return;

    Player* bot = officers[urand(0, officers.size() - 1)];
    Guild* guild = sGuildMgr->GetGuildById(bot->GetGuildId());
    if (!guild)
        return;

    std::ostringstream ask;
    ask << "Write ONE short guild recruitment ad for the public channel. Your guild is "
           "named \"" << guild->GetName() << "\" - mention that name. Say what kind of "
           "players you want (a class, a role, or just friendly folk) in your own words. "
           "No quotes around the ad.";
    Submit(bot, bot, ask.str(), CHAT_MSG_CHANNEL,
           urand(0, 1) ? uint32(ChatChannelId::TRADE) : uint32(ChatChannelId::GENERAL), 0);
}

void LlmBridge::Drain(uint32 diff)
{
    if (!_enabled)
        return;

    std::vector<Reply> pending;
    {
        std::lock_guard<std::mutex> guard(_mutex);
        pending.swap(_replies);
    }

    for (Reply const& reply : pending)
    {
        // The delivery attempt is the outcome: the speaker's reply slot frees
        // here whether or not the players still exist.
        FreeSpeaker(reply.speakerGuid);
        Deliver(reply);
    }

    // Fire greetings that have come due. Iterated backwards so erasing is safe.
    for (std::size_t i = _greets.size(); i-- > 0;)
    {
        if (_greets[i].remainingMs > diff)
        {
            _greets[i].remainingMs -= diff;
            continue;
        }

        PendingGreet const greet = _greets[i];
        _greets.erase(_greets.begin() + i);

        if (_inFlight.load() < _maxInFlight)
        {
            _inFlight.fetch_add(1);
            std::thread(&LlmBridge::GreetWorker, this, greet.humanGuid, greet.name).detach();
        }
    }

    TickAmbient(diff);
    TickGuildAds(diff);
}
