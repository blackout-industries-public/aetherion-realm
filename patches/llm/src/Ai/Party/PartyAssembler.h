/*
 * Background party assembly.
 *
 * Upstream forms groups only from bots that can currently see each other
 * ("nearest friendly players"). On a realm with 1500 bots spread over four
 * continents that reliably produces two-bot parties and nothing larger, because a
 * pair that groups up then travels together and rarely meets a third ungrouped bot.
 * Two-bot parties can never run a dungeon, so the dungeon finder never has anything
 * to work with.
 *
 * This assembles complete parties directly: pick a leader, select level- and
 * role-compatible bots anywhere in the world, build the group through the same API
 * the dungeon finder uses, and optionally bring everyone to the leader.
 */
#ifndef _PLAYERBOT_PARTYASSEMBLER_H
#define _PLAYERBOT_PARTYASSEMBLER_H

#include "Common.h"
#include "ObjectGuid.h"

#include "Position.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

class Group;
class Player;

class PartyAssembler
{
public:
    static PartyAssembler* instance();

    void LoadConfig();
    bool IsEnabled() const { return _enabled; }

    // World thread only. Throttled internally.
    void Tick(uint32 diff);

private:
    bool AssembleOne();

    // Puts the finished party into the dungeon finder. Without this the assembler
    // only produces parties that stand around: LfgJoinAction lets a bot queue only
    // when it leads its own group, and it has no reason to do so on its own.
    bool QueueForDungeon(Player* leader) const;
    static uint32 RoleMask(Player const* bot);

    // Outdoor entrance per instance map, read once from areatrigger data. Travelling
    // to the entrance and walking in is what makes this read as a party going to a
    // dungeon rather than a party blinking into one.
    void LoadEntrances();
    bool SendPartyToDungeon(Group* group, Player* leader) const;

    struct Entrance
    {
        uint32 map{0};
        float x{0.f}, y{0.f}, z{0.f};
    };
    std::unordered_map<uint32, Entrance> _entrances;   // instance map id -> outdoor spot
    std::vector<Player*> Candidates() const;

    // Roughly "can hold threat" / "can heal", by class. Deliberately loose: a party
    // of five that is merely plausible beats waiting for a perfect one.
    static bool CanTank(Player const* bot);
    static bool CanHeal(Player const* bot);

    bool _enabled{false};
    uint32 _intervalMs{60000};
    uint32 _targetSize{5};
    uint32 _maxParties{20};
    uint32 _minLevel{15};
    uint32 _levelSpread{4};
    bool _teleport{true};
    bool _queueLfg{true};
    bool _travelToDungeon{true};

    uint32 _timer{0};

    // Parties this assembler built, so it can stop once the world holds enough of
    // them rather than grouping every bot on the realm.
    std::unordered_set<uint32> _assembled;
};

#define sPartyAssembler PartyAssembler::instance()

#endif
