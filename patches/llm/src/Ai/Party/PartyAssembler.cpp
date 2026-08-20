#include "PartyAssembler.h"

#include "Config.h"
#include "Group.h"
#include "GroupMgr.h"
#include "DatabaseEnv.h"
#include "LFGMgr.h"
#include "NewRpgInfo.h"
#include "NewRpgStrategy.h"
#include "Opcodes.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"   // IsRealPlayer
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"

#include <algorithm>
#include <string>
#include <vector>

// The dungeon-finder types and role flags live in this namespace, as in the module's
// own LfgActions.cpp.
using namespace lfg;

PartyAssembler* PartyAssembler::instance()
{
    static PartyAssembler instance;
    return &instance;
}

void PartyAssembler::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.Enabled", false);
    _intervalMs = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.IntervalMs", 60000);
    _targetSize = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.TargetSize", 5);
    _maxParties = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.MaxParties", 20);
    _minLevel = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.MinLevel", 15);
    _levelSpread = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.LevelSpread", 4);
    _teleport = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.Teleport", true);
    _queueLfg = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.QueueLfg", true);
    _travelToDungeon = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.TravelToDungeon", true);

    if (_travelToDungeon && _entrances.empty())
        LoadEntrances();
    _timer = 0;

    if (_enabled)
        LOG_INFO("playerbots",
                 "Party assembler enabled: every {} ms, size {}, max {} parties, "
                 "level {}+ within {}, teleport {}",
                 _intervalMs, _targetSize, _maxParties, _minLevel, _levelSpread,
                 _teleport ? "on" : "off");
}

void PartyAssembler::LoadEntrances()
{
    // areatrigger holds the outdoor trigger position; areatrigger_teleport says which
    // instance it leads to. "Entrance" in the name filters out the matching exit and
    // inside-the-instance triggers, which would send parties to the wrong side.
    QueryResult result = WorldDatabase.Query(
        "SELECT att.target_map, at.map, at.x, at.y, at.z "
        "FROM areatrigger at "
        "JOIN areatrigger_teleport att ON att.ID = at.entry "
        "WHERE att.Name LIKE '%Entrance%' AND att.Name NOT LIKE '%Inside%' "
        "  AND att.Name NOT LIKE '%Exit%'");

    if (!result)
    {
        LOG_WARN("playerbots", "Party assembler: no dungeon entrances found");
        return;
    }

    do
    {
        Field* f = result->Fetch();
        uint32 const target = f[0].Get<uint32>();
        // First entrance wins; several dungeons have more than one door.
        if (_entrances.find(target) != _entrances.end())
            continue;
        _entrances[target] = Entrance{f[1].Get<uint32>(), f[2].Get<float>(),
                                      f[3].Get<float>(), f[4].Get<float>()};
    } while (result->NextRow());

    LOG_INFO("playerbots", "Party assembler: loaded {} dungeon entrances", _entrances.size());
}

bool PartyAssembler::SendPartyToDungeon(Group* group, Player* leader) const
{
    if (_entrances.empty())
        return false;

    uint8 const level = leader->GetLevel();

    // Candidate dungeons the party is the right level for and that we know a door to.
    // Name travels with the entrance: keeping them in separate variables logged one
    // dungeon's name against another's coordinates.
    struct Option { Entrance where; std::string name; };
    std::vector<Option> options;

    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i);
        if (!dungeon || dungeon->TypeID != LFG_TYPE_DUNGEON)
            continue;
        if (!dungeon->MinLevel || level < dungeon->MinLevel || level > dungeon->MaxLevel)
            continue;

        auto const it = _entrances.find(dungeon->MapID);
        if (it == _entrances.end())
            continue;

        options.push_back({it->second, dungeon->Name[0] ? dungeon->Name[0] : "a dungeon"});
    }

    if (options.empty())
        return false;

    Option const& chosen = options[urand(0, options.size() - 1)];
    Entrance const& target = chosen.where;
    std::string const& chosenName = chosen.name;

    // Hand the destination to the bots' own RPG movement. They walk, take flight
    // paths and route around terrain exactly as they would to any other destination -
    // no teleporting, which is the entire point.
    uint32 sent = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member)
            continue;
        PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member);
        if (!memberAI)
            continue;

        memberAI->rpgInfo.ChangeToGoGrind(WorldPosition(target.map, target.x, target.y, target.z));
        ++sent;
    }

    if (sent)
        LOG_INFO("playerbots", "Party assembler: {}'s party of {} sets out for {} "
                 "(map {} at {:.0f},{:.0f})",
                 leader->GetName(), sent, chosenName, target.map, target.x, target.y);
    return sent > 0;
}

uint32 PartyAssembler::RoleMask(Player const* bot)
{
    uint32 mask = PLAYER_ROLE_DAMAGE;
    if (CanTank(bot))
        mask |= PLAYER_ROLE_TANK;
    if (CanHeal(bot))
        mask |= PLAYER_ROLE_HEALER;
    return mask;
}

bool PartyAssembler::QueueForDungeon(Player* leader) const
{
    // Not LFGMgr::GetRandomAndSeasonalDungeons: that returns Entry() values, which are
    // id + (type << 24), so feeding them to LookupEntry finds nothing and the list
    // comes back empty every time. The module keeps a plain list of usable dungeon ids
    // per faction, which is what its own LfgJoinAction iterates.
    std::vector<uint32> const& available =
        sRandomPlayerbotMgr.LfgDungeons[leader->GetTeamId()];
    if (available.empty())
        return false;

    uint8 const level = leader->GetLevel();

    LfgDungeonSet list;
    for (uint32 id : available)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(id);
        if (!dungeon)
            continue;
        if (dungeon->TypeID != LFG_TYPE_RANDOM && dungeon->TypeID != LFG_TYPE_DUNGEON &&
            dungeon->TypeID != LFG_TYPE_HEROIC)
            continue;
        if (dungeon->MinLevel && (level < dungeon->MinLevel || level > dungeon->MaxLevel))
            continue;
        // Same guard the module uses: stop offering a levelling dungeon once the
        // party has outgrown it by more than ten levels.
        if (level > dungeon->MinLevel + 10 && dungeon->TypeID == LFG_TYPE_DUNGEON)
            continue;
        list.insert(dungeon->ID);
    }

    if (list.empty())
    {
        LOG_DEBUG("playerbots", "Party assembler: no LFG dungeon fits level {}", level);
        return false;
    }

    // JoinLfg is not thread safe, so the request goes through the session queue -
    // the same route the module's own LfgJoinAction uses.
    WorldPacket* data = new WorldPacket(CMSG_LFG_JOIN);
    *data << static_cast<uint32>(RoleMask(leader));
    *data << static_cast<bool>(false);
    *data << static_cast<bool>(false);
    *data << static_cast<uint8>(list.size());
    for (uint32 id : list)
        *data << static_cast<uint32>(id);
    *data << static_cast<uint8>(3) << static_cast<uint8>(0)
          << static_cast<uint8>(0) << static_cast<uint8>(0);
    *data << std::string("0");
    leader->GetSession()->QueuePacket(data);

    LOG_INFO("playerbots", "Party assembler: {} queued the party for {} dungeon(s)",
             leader->GetName(), list.size());
    return true;
}

bool PartyAssembler::CanTank(Player const* bot)
{
    switch (bot->getClass())
    {
        case CLASS_WARRIOR:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_DEATH_KNIGHT:
            return true;
        default:
            return false;
    }
}

bool PartyAssembler::CanHeal(Player const* bot)
{
    switch (bot->getClass())
    {
        case CLASS_PRIEST:
        case CLASS_PALADIN:
        case CLASS_DRUID:
        case CLASS_SHAMAN:
            return true;
        default:
            return false;
    }
}

std::vector<Player*> PartyAssembler::Candidates() const
{
    std::vector<Player*> out;

    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || bot->isDead())
            continue;
        if (bot->GetGroup() || bot->IsBeingTeleported())
            continue;
        if (bot->GetLevel() < _minLevel)
            continue;
        if (!GET_PLAYERBOT_AI(bot))
            continue;
        // Never touch a bot that belongs to a real player.
        if (IsRealPlayer(GET_PLAYERBOT_AI(bot)->GetMaster()))
            continue;
        if (bot->InBattleground() || bot->InBattlegroundQueue())
            continue;

        out.push_back(bot);
    }

    return out;
}

bool PartyAssembler::AssembleOne()
{
    std::vector<Player*> pool = Candidates();
    if (pool.size() < _targetSize)
        return false;

    // Leader first, then everyone compatible with the leader. Picking the leader at
    // random rather than by level keeps parties spread across the level brackets
    // instead of always forming at the cap.
    Player* leader = pool[urand(0, pool.size() - 1)];

    std::vector<Player*> compatible;
    for (Player* bot : pool)
    {
        if (bot == leader)
            continue;
        if (bot->GetTeamId() != leader->GetTeamId())
            continue;
        uint32 const diff = bot->GetLevel() > leader->GetLevel()
                                ? bot->GetLevel() - leader->GetLevel()
                                : leader->GetLevel() - bot->GetLevel();
        if (diff > _levelSpread)
            continue;
        compatible.push_back(bot);
    }

    if (compatible.size() + 1 < _targetSize)
        return false;

    // Fill one tank and one healer before filling with damage, so the party is
    // plausible enough for the dungeon finder to accept the roles.
    std::vector<Player*> picked;
    auto take = [&picked, &compatible](auto predicate) {
        auto it = std::find_if(compatible.begin(), compatible.end(), predicate);
        if (it == compatible.end())
            return false;
        picked.push_back(*it);
        compatible.erase(it);
        return true;
    };

    if (!CanTank(leader))
        take([](Player* p) { return CanTank(p); });
    if (!CanHeal(leader))
        take([](Player* p) { return CanHeal(p); });

    while (picked.size() + 1 < _targetSize && !compatible.empty())
    {
        picked.push_back(compatible.back());
        compatible.pop_back();
    }

    if (picked.size() + 1 < _targetSize)
        return false;

    // Same construction the dungeon finder uses, so the group is a normal group in
    // every respect - no bespoke state to keep consistent.
    Group* group = new Group();
    if (!group->Create(leader))
    {
        delete group;
        return false;
    }
    sGroupMgr->AddGroup(group);

    uint32 added = 0;
    for (Player* member : picked)
    {
        if (group->IsFull())
            break;
        if (group->AddMember(member))
        {
            ++added;
            if (_teleport && member->GetMapId() != leader->GetMapId())
                continue;   // cross-map pulls are left to the group's own travel logic
        }
    }

    if (!added)
    {
        group->Disband();
        return false;
    }

    _assembled.insert(group->GetGUID().GetCounter());

    LOG_INFO("playerbots", "Party assembler: {} (level {}) formed a party of {}",
             leader->GetName(), leader->GetLevel(), added + 1);

    // Travel first: a party that walks to the door reads as players going somewhere.
    // The dungeon finder stays available as a fallback for parties with no reachable
    // dungeon at their level.
    if (!(_travelToDungeon && SendPartyToDungeon(group, leader)) && _queueLfg)
        QueueForDungeon(leader);

    return true;
}

void PartyAssembler::Tick(uint32 diff)
{
    if (!_enabled)
        return;

    _timer += diff;
    if (_timer < _intervalMs)
        return;
    _timer = 0;

    // Drop parties that have since disbanded, so the cap reflects reality.
    for (auto it = _assembled.begin(); it != _assembled.end();)
        it = sGroupMgr->GetGroupByGUID(*it) ? std::next(it) : _assembled.erase(it);

    if (_assembled.size() >= _maxParties)
        return;

    AssembleOne();
}
