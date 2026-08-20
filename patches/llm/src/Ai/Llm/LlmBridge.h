/*
 * Aetherion Phase 2: asynchronous link from the worldserver to the external AI Bridge.
 *
 * Constraints from the BRD:
 *  - s3.1 gameplay never depends on the AI. Disabled by default; if the bridge is
 *    down the only difference is that bots stop making conversation.
 *  - s3.3 no inference blocks the world. Requests run on detached threads; replies
 *    are delivered from the world thread in Drain().
 *  - s26 human-directed traffic outranks bot chatter, and chatter is dropped rather
 *    than queued when capacity runs out.
 */
#ifndef _PLAYERBOT_LLMBRIDGE_H
#define _PLAYERBOT_LLMBRIDGE_H

#include "Common.h"
#include "ObjectGuid.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Player;

class LlmBridge
{
public:
    static LlmBridge* instance();

    void LoadConfig();
    bool IsEnabled() const { return _enabled; }
    bool WantsChatType(uint32 chatType) const;

    // One message reaches many bots: a party message is offered to every bot in the
    // party, a channel message to every bot in the channel. Exactly one of them should
    // answer, so the first to claim a given (speaker, type, text) within the claim
    // window wins and the rest stay quiet.
    bool TryClaim(ObjectGuid speaker, uint32 chatType, std::string const& message);

    // Public channels can contain every bot on the realm, so a reply there is a dice
    // roll rather than a certainty. Kept here so the hook site stays declarative.
    bool RollChannelReply() const;
    bool RollSayReply() const;

    // Picks a bot to speak. Prefers one standing in the same zone as a real player:
    // chatter nobody can see does nothing for how populated the world feels, and a
    // uniform pick over 1500 bots almost never lands near the human.
    Player* PickResponder(Player* near = nullptr) const;

    // True only if a real human would actually receive this line. Inference spent on
    // a conversation nobody is present for is pure waste, and at 1500 bots that is
    // the overwhelming majority of possible chatter.
    bool HasHumanWitness(Player* bot, uint32 chatType) const;

    // World thread. Returns immediately; performs no I/O on the caller.
    void Submit(Player* bot, Player* speaker, std::string const& message, uint32 chatType,
                uint32 channelId = 0, uint32 depth = 0);

    // A human just entered the world. Schedules a greeting from a bot that already
    // knows them - a greeting from a stranger is noise, so the bridge decides who.
    void OnHumanLogin(Player* human);

    // Something real happened. Grounding reactions in actual game state is also the
    // cheapest hallucination fix there is: the bot is told, not left to guess.
    void OnGameEvent(Player* human, std::string const& eventType, std::string const& detail);

    // World thread. Delivers replies, drives ambient chatter, fires due greetings.
    void Drain(uint32 diff);

private:
    struct Reply
    {
        ObjectGuid botGuid;
        ObjectGuid speakerGuid;
        std::string text;
        uint32 chatType{0};
        uint32 channelId{0};
        uint32 depth{0};
        // Allow-listed action the bridge says the bot agreed to. Advisory only: the
        // server re-checks every precondition before doing anything (BRD s29).
        std::string intent;
    };

    void ExecuteIntent(Player* bot, Player* speaker, std::string const& intent);

    // Nothing crossing the thread boundary may hold a Player*: it can be freed while
    // the request is in flight. GUIDs and copied strings only.
    void Worker(ObjectGuid botGuid, ObjectGuid speakerGuid, std::string botName,
                std::string speakerName, std::string message, uint32 chatType,
                uint32 channelId, uint32 depth);
    void GreetWorker(ObjectGuid humanGuid, std::string humanName);
    void EventWorker(ObjectGuid botGuid, ObjectGuid humanGuid, std::string humanName,
                     std::string eventType, std::string detail);

    // Shared by all three workers. Returns false on any transport or status failure;
    // callers treat that as "stay silent", never as an error to surface.
    bool HttpPost(std::string const& path, std::string const& json, std::string& body);

    void QueueReply(Reply const& reply);

    void Deliver(Reply const& reply);
    void TickAmbient(uint32 diff);
    char const* ChannelLabel(uint32 chatType, uint32 depth) const;

    bool _enabled{false};
    std::string _host{"ai-bridge"};
    std::string _port{"8090"};
    uint32 _timeoutMs{15000};
    uint32 _maxInFlight{8};
    uint32 _claimWindowMs{10000};
    float _sayRange{45.0f};
    bool _requireWitness{true};
    bool _sameFactionOnly{true};

    bool _reactWhisper{true};
    bool _reactParty{true};
    bool _reactGuild{true};
    bool _reactSay{false};
    uint32 _channelReplyChance{5};
    uint32 _sayReplyChance{25};

    bool _ambientEnabled{false};
    uint32 _ambientIntervalMs{300000};
    uint32 _ambientTimer{0};
    // A bot answering another bot is what makes chatter look like conversation, but it
    // is also how a feedback loop starts. Hard depth cap, not a probability.
    uint32 _ambientMaxDepth{1};
    // Local /say rather than a chat channel. Channels reach the whole zone but read
    // as background noise; /say happens visibly next to the player.
    bool _ambientUseSay{true};

    bool _greetOnLogin{true};
    // Long enough that the greeting lands after the loading screen, not during it.
    uint32 _greetDelayMs{15000};
    bool _eventsEnabled{true};
    uint32 _eventChanceLevelUp{100};
    uint32 _eventChanceDeath{40};
    uint32 _eventChanceLoot{35};

    std::atomic<uint32> _inFlight{0};

    std::mutex _mutex;
    std::vector<Reply> _replies;

    struct PendingGreet
    {
        ObjectGuid humanGuid;
        std::string name;
        uint32 remainingMs;
    };
    // World thread only; no lock needed.
    std::vector<PendingGreet> _greets;

    std::mutex _claimMutex;
    std::unordered_map<std::size_t, uint64> _claims;
};

#define sLlmBridge LlmBridge::instance()

#endif
