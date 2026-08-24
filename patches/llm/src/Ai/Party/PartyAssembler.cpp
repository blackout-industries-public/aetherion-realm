#include "PartyAssembler.h"

#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "GroupMgr.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
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
#include <unordered_map>
#include <mutex>
#include <cmath>
#include <string>
#include <vector>

// The dungeon-finder types and role flags live in this namespace, as in the module's
// own LfgActions.cpp.
using namespace lfg;

namespace
{
    // Mirror of _assembled for cross-thread reads. The world thread writes it once per
    // tick; map threads only ever take the lock for a set lookup.
    std::mutex sOwnedMx;
    std::unordered_set<uint32> sOwnedMirror;
    std::unordered_set<uint32> sDrivenGuids;
    bool sDriveGrouped = false;
}

bool PartyAssembler::Owns(uint32 groupLowGuid)
{
    std::lock_guard<std::mutex> lock(sOwnedMx);
    return sOwnedMirror.find(groupLowGuid) != sOwnedMirror.end();
}

bool PartyAssembler::DriveGroupedBots()
{
    return sDriveGrouped;
}

bool PartyAssembler::IsDriven(uint32 charLowGuid)
{
    std::lock_guard<std::mutex> lock(sOwnedMx);
    return sDrivenGuids.find(charLowGuid) != sDrivenGuids.end();
}

void PartyAssembler::SyncOwnedMirror()
{
    // Who is being steered right now. Leader only while travelling - forcing waiting
    // members active would set their follow strategy loose on a master half a
    // continent away. During the summon, everyone, so stragglers accept promptly.
    // Inside needs nothing: non-overworld bots are already forced active upstream.
    std::unordered_set<uint32> driven;
    for (auto const& entry : _trips)
    {
        if (entry.second.phase == Phase::Inside)
            continue;
        Group* group = sGroupMgr->GetGroupByGUID(entry.first);
        if (!group)
            continue;
        if (entry.second.phase == Phase::Summoning)
        {
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* member = ref->GetSource())
                    driven.insert(member->GetGUID().GetCounter());
        }
        else
        {
            driven.insert(group->GetLeaderGUID().GetCounter());
        }
    }

    std::lock_guard<std::mutex> lock(sOwnedMx);
    sOwnedMirror.clear();
    sOwnedMirror.insert(_assembled.begin(), _assembled.end());
    sDrivenGuids = std::move(driven);
}

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
    _perTick = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.PerTick", 3);
    _minLevel = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.MinLevel", 15);
    _levelSpread = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.LevelSpread", 4);
    _teleport = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.Teleport", true);
    _sameMapOnly = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.SameMapOnly", true);
    _arriveRange = sConfigMgr->GetOption<float>("AiPlayerbot.Party.ArriveRange", 60.0f);
    _maxTripTicks = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.MaxTripTicks", 40);
    _gatherRange = sConfigMgr->GetOption<float>("AiPlayerbot.Party.GatherRange", 400.0f);
    _stallTicks = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.StallTicks", 2);
    _insideTicks = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.InsideTicks", 20);
    _sweepPerTick = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.SweepPerTick", 25);
    _huntRange = sConfigMgr->GetOption<float>("AiPlayerbot.Party.HuntRange", 8.0f);
    _nearestChoices = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.NearestChoices", 4);
    // Zero would index past the end of the candidate list.
    if (!_nearestChoices)
        _nearestChoices = 1;
    _footRange = sConfigMgr->GetOption<float>("AiPlayerbot.Party.FootRange", 1200.0f);
    _portalPct = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.PortalPct", 50);
    _raidPct = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.RaidPct", 20);
    _raidSize = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.RaidSize", 10);
    _raid25Pct = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.Raid25Pct", 25);
    _raidHeroicPct = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.RaidHeroicPct", 15);
    _musterEveryMin = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.MusterEveryMin", 45);
    _musterTimeoutMin = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.MusterTimeoutMin", 12);
    _wipeRetries = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.WipeRetries", 3);
    _queueLfg = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.QueueLfg", true);
    _travelToDungeon = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.TravelToDungeon", true);
    sDriveGrouped = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.DriveGroupedBots", false);

    if (_travelToDungeon && _entrances.empty())
    {
        LoadEntrances();
        LoadInsides();
        LoadInstanceSpawns();
        LoadBossPositions();
        EnsureTelemetryTables();
    }
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
        "SELECT att.target_map, at.map, at.x, at.y, at.z, "
        "       att.target_position_x, att.target_position_y, att.target_position_z, "
        "       COALESCE(dat.min_level, 0) "
        "FROM areatrigger at "
        "JOIN areatrigger_teleport att ON att.ID = at.entry "
        "LEFT JOIN dungeon_access_template dat "
        "       ON dat.map_id = att.target_map AND dat.difficulty = 0 "
        "WHERE att.Name LIKE '%Entrance%' AND att.Name NOT LIKE '%Inside%' "
        "  AND att.Name NOT LIKE '%Exit%'");

    if (!result)
    {
        LOG_WARN("playerbots", "Party assembler: no dungeon entrances found");
        return;
    }

    // Gear floors for every mode. The door itself no longer hard-checks gear
    // (that made one green member a veto); these feed the scaled-appetite
    // math instead.
    _ilvlFloor.clear();
    if (QueryResult floors = WorldDatabase.Query(
            "SELECT map_id, difficulty, min_avg_item_level FROM dungeon_access_template "
            "WHERE min_avg_item_level > 0"))
    {
        do
        {
            Field* f = floors->Fetch();
            _ilvlFloor[(f[0].Get<uint32>() << 8) | f[1].Get<uint8>()] = f[2].Get<uint16>();
        } while (floors->NextRow());
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
        // The far side of the same trigger is where the portal drops you, so every
        // door we know also gives us an arrival point - raids included.
        _insides[target] = Entrance{target, f[5].Get<float>(), f[6].Get<float>(),
                                    f[7].Get<float>()};
        if (uint32 const floorLevel = f[8].Get<uint32>())
            _mapMinLevel[target] = floorLevel;
    } while (result->NextRow());

    for (auto const& e : _entrances)
        if (MapEntry const* m = sMapStore.LookupEntry(e.first))
            if (m->IsRaid())
                ++_raidMapCount;

    LOG_INFO("playerbots", "Party assembler: loaded {} dungeon entrances ({} raid maps)",
             _entrances.size(), _raidMapCount);
}

void PartyAssembler::LoadInstanceSpawns()
{
    // Only maps we actually send parties to, and only a sample of each: the point is to
    // have somewhere to walk towards, not a complete spawn list.
    QueryResult result = WorldDatabase.Query(
        "SELECT map, position_x, position_y, position_z FROM creature "
        "WHERE map IN (SELECT DISTINCT target_map FROM areatrigger_teleport)");
    if (!result)
        return;

    do
    {
        Field* f = result->Fetch();
        uint32 const map = f[0].Get<uint32>();
        auto& list = _spawns[map];
        if (list.size() >= _spawnsPerMap)
            continue;
        list.push_back(Entrance{map, f[1].Get<float>(), f[2].Get<float>(), f[3].Get<float>()});
    } while (result->NextRow());

    LOG_INFO("playerbots", "Party assembler: loaded spawn points for {} instance maps",
             _spawns.size());
}

void PartyAssembler::LoadBossPositions()
{
    // creditType 0 means the encounter is credited by killing a creature, so its entry
    // resolves to a spawn. The other 34 encounters are credited by spell and have no
    // position; trash steering covers those maps.
    QueryResult result = WorldDatabase.Query(
        "SELECT c.map, c.position_x, c.position_y, c.position_z "
        "FROM instance_encounters ie "
        "JOIN creature c ON c.id = ie.creditEntry "
        "WHERE ie.creditType = 0 "
        "GROUP BY c.map, c.position_x, c.position_y, c.position_z");
    if (!result)
        return;

    do
    {
        Field* f = result->Fetch();
        uint32 const map = f[0].Get<uint32>();
        _bosses[map].push_back(Entrance{map, f[1].Get<float>(), f[2].Get<float>(),
                                        f[3].Get<float>()});
    } while (result->NextRow());

    LOG_INFO("playerbots", "Party assembler: loaded boss positions for {} instance maps",
             _bosses.size());
}

void PartyAssembler::LoadInsides()
{
    // Where the portal drops you. lfg_dungeon_template stores the arrival point for
    // each dungeon; the map comes from the LFG store, since the table has no map id.
    QueryResult result = WorldDatabase.Query(
        "SELECT dungeonId, position_x, position_y, position_z FROM lfg_dungeon_template");
    if (!result)
        return;

    do
    {
        Field* f = result->Fetch();
        uint32 const dungeonId = f[0].Get<uint32>();
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(dungeonId);
        if (!dungeon)
            continue;
        _insides.emplace(dungeon->MapID, Entrance{dungeon->MapID, f[1].Get<float>(),
                                                  f[2].Get<float>(), f[3].Get<float>()});
    } while (result->NextRow());

    LOG_INFO("playerbots", "Party assembler: loaded {} instance arrival points", _insides.size());
}

bool PartyAssembler::EnterInstance(Group* group, Trip const& trip)
{
    auto const inside = _insides.find(trip.dungeonMap);
    if (inside == _insides.end())
        return false;

    uint32 moved = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->IsBeingTeleported())
            continue;
        member->TeleportTo(trip.dungeonMap, inside->second.x, inside->second.y,
                           inside->second.z, 0.0f);
        ++moved;
    }

    if (moved)
        LOG_INFO("playerbots", "Party assembler: party enters {} ({} members)",
                 trip.name, moved);
    return moved > 0;
}

uint32 PartyAssembler::SendGroupOutside(Group* group, uint32 dungeonMap)
{
    auto const door = _entrances.find(dungeonMap);
    if (door == _entrances.end())
        return 0;

    uint32 moved = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || member->IsBeingTeleported())
            continue;
        if (member->GetMapId() != dungeonMap)
            continue;   // already outside

        member->TeleportTo(door->second.map, door->second.x, door->second.y,
                           door->second.z, 0.0f);
        ++moved;
    }
    return moved;
}

void PartyAssembler::SweepStrandedBots()
{
    uint32 swept = 0;

    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd() && swept < _sweepPerTick; ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            continue;
        if (!GET_PLAYERBOT_AI(bot) || IsRealPlayer(GET_PLAYERBOT_AI(bot)->GetMaster()))
            continue;
        if (bot->InBattleground() || bot->InBattlegroundQueue())
            continue;

        // Only somewhere we know a way out of.
        auto const door = _entrances.find(bot->GetMapId());
        if (door == _entrances.end())
            continue;

        if (Group* group = bot->GetGroup())
        {
            // A group this object is still steering is not stranded.
            if (_trips.count(group->GetGUID().GetCounter()))
                continue;
            // Nor is one the dungeon finder put together: that is upstream running its
            // own content, and dragging it out of the instance would break the run.
            if (group->isLFGGroup())
                continue;
        }

        bot->TeleportTo(door->second.map, door->second.x, door->second.y,
                        door->second.z, 0.0f);
        ++swept;
    }

    if (swept)
        LOG_INFO("playerbots", "Party assembler: sent {} stranded bots back outside", swept);
}

void PartyAssembler::AdvanceTrips()
{
    for (auto it = _trips.begin(); it != _trips.end();)
    {
        Group* group = sGroupMgr->GetGroupByGUID(it->first);
        Trip& trip = it->second;

        // Give up on a journey that is going nowhere rather than re-asserting a
        // destination forever. A party already inside gets a longer budget: that timer
        // is how long it runs the place, not how long it is allowed to travel.
        uint32 const budget = trip.phase == Phase::Inside ? _insideTicks : _maxTripTicks;
        if (!group || ++trip.ticks > budget)
        {
            // The ledger's verdict on this journey: a run that got inside
            // simply ended (the frontend grades it by bosses downed); one
            // that never zoned tells exactly where it died.
            EndRun(trip.runId, trip.dungeonMap,
                   !group ? "disbanded"
                   : trip.phase == Phase::Inside ? "ended"
                   : trip.phase == Phase::Summoning ? "door_timeout"
                                                    : "travel_timeout");
            // Release a finished run so its bots rejoin the candidate pool. Left
            // grouped they would hold a slot in the cap for the rest of the uptime.
            if (group && trip.phase == Phase::Inside)
            {
                // Out of the instance first. Disbanding alone leaves five characters
                // standing in a dungeon nothing will ever move them out of.
                uint32 const out = SendGroupOutside(group, trip.dungeonMap);
                if (out)
                    LOG_INFO("playerbots",
                             "Party assembler: {} leave {} after their run",
                             out, trip.name);
                group->Disband();
            }
            it = _trips.erase(it);
            continue;
        }

        Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        if (!leader || !GET_PLAYERBOT_AI(leader))
        {
            EndRun(trip.runId, trip.dungeonMap, "leader_lost");
            it = _trips.erase(it);
            continue;
        }

        if (trip.phase == Phase::Travelling)
        {
            float const dx = leader->GetPositionX() - trip.door.x;
            float const dy = leader->GetPositionY() - trip.door.y;
            float const dist = std::sqrt(dx * dx + dy * dy);

            if (leader->GetMapId() == trip.door.map && dist <= _arriveRange)
            {
                trip.phase = Phase::Summoning;
                ++_statArrived;
                if (trip.runId)
                    CharacterDatabase.Execute(
                        "UPDATE aetherion_run_history SET reached_door_at = UNIX_TIMESTAMP()"
                        " WHERE id = {}", trip.runId);
                LOG_INFO("playerbots", "Party assembler: {} reached {} - summoning the party",
                         leader->GetName(), trip.name);
            }
            else
            {
                // The RPG state machine re-rolls its own status on a timer, so a
                // destination set once is forgotten long before a 3000-yard walk
                // finishes. Re-asserting each tick is what actually keeps them going.
                GET_PLAYERBOT_AI(leader)->rpgInfo.ChangeToGoGrind(
                    WorldPosition(trip.door.map, trip.door.x, trip.door.y, trip.door.z));

                // Whether that actually moved anyone is a separate question: grouped
                // bots are skipped by the random-bot manager, so many leaders never take
                // a step. Finish the journey for them rather than letting the party
                // stand in a city until the trip expires.
                bool const moved = trip.lastDist == 0.0f || dist + 20.0f < trip.lastDist;
                trip.lastDist = dist;
                if (moved)
                    trip.stalls = 0;
                else if (++trip.stalls >= _stallTicks)
                {
                    trip.stalls = 0;
                    leader->TeleportTo(trip.door.map, trip.door.x, trip.door.y,
                                       trip.door.z, 0.0f);
                    ++_statStalls;
                    LOG_INFO("playerbots",
                             "Party assembler: {} took the flight path to {} ({:.0f} yards)",
                             leader->GetName(), trip.name, dist);
                }
                ++it;
                continue;
            }
        }

        if (trip.phase == Phase::Inside)
        {
            // Wipe watch. A raid that has fully fallen does what a determined
            // guild does: steadies itself and pulls again - resurrected at the
            // instance door with a fresh clock - until repeated wipes break
            // its spirit and the run ends as 'wiped', not a silent timeout.
            // Without this, dead bots released to a graveyard OUTSIDE the
            // instance and the run burned out with nobody home.
            uint32 alive = 0, present = 0;
            for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
                if (Player* m = mi->GetSource())
                    if (m->IsInWorld())
                    {
                        ++present;
                        if (m->IsAlive())
                            ++alive;
                    }
            if (present && !alive)
            {
                ++trip.wipes;
                if (trip.runId)
                    CharacterDatabase.Execute(
                        "UPDATE aetherion_run_history SET wipes = {} WHERE id = {}",
                        trip.wipes, trip.runId);
                if (trip.wipes > _wipeRetries)
                {
                    LOG_INFO("playerbots",
                             "Party assembler: wipe {} in {} breaks the raid - they call it",
                             trip.wipes, trip.name);
                    EndRun(trip.runId, trip.dungeonMap, "wiped");
                    SendGroupOutside(group, trip.dungeonMap);
                    group->Disband();
                    it = _trips.erase(it);
                    continue;
                }
                for (GroupReference* mi = group->GetFirstMember(); mi != nullptr;
                     mi = mi->next())
                    if (Player* m = mi->GetSource())
                        if (m->IsInWorld() && !m->IsAlive())
                        {
                            // Same recovery the random-bot manager applies on
                            // this thread: on their feet, bones gone, combat
                            // strategies rebuilt.
                            m->ResurrectPlayer(0.5f);
                            m->SpawnCorpseBones();
                            if (PlayerbotAI* mAI = GET_PLAYERBOT_AI(m))
                                mAI->ResetStrategies(false);
                        }
                // Reseat everyone at the inside arrival point and restart the
                // dwell clock - determination buys a whole fresh attempt.
                EnterInstance(group, trip);
                trip.ticks = 0;
                LOG_INFO("playerbots",
                         "Party assembler: {} wiped in {} (wipe {}) - they steady "
                         "themselves and pull again",
                         leader->GetName(), trip.name, trip.wipes);
                ++it;
                continue;
            }

            // Bosses first; trash only where no boss position is known. Held as a
            // pointer rather than an iterator: the two maps are separate containers and
            // comparing an iterator from one against the other's end() is undefined.
            std::vector<Entrance> const* packs = nullptr;
            if (auto const bossIt = _bosses.find(trip.dungeonMap);
                bossIt != _bosses.end() && !bossIt->second.empty())
                packs = &bossIt->second;
            else if (auto const trashIt = _spawns.find(trip.dungeonMap);
                     trashIt != _spawns.end() && !trashIt->second.empty())
                packs = &trashIt->second;

            if (packs && leader->GetMapId() == trip.dungeonMap)
            {
                // Head for the nearest pack that is not already on top of us. Members
                // follow the leader, so steering one character walks the whole party
                // through the dungeon.
                Entrance const* best = nullptr;
                float bestDist = 0.0f;
                for (Entrance const& spot : *packs)
                {
                    float const dx = leader->GetPositionX() - spot.x;
                    float const dy = leader->GetPositionY() - spot.y;
                    float const dist = std::sqrt(dx * dx + dy * dy);
                    // Only skip a target we are practically standing on. Skipping
                    // anything within a generous radius sent a party that had closed to
                    // 20 yards off towards a different boss, so it oscillated between
                    // them and committed to neither.
                    if (dist < _huntRange)
                        continue;
                    if (!best || dist < bestDist)
                    {
                        best = &spot;
                        bestDist = dist;
                    }
                }

                if (best)
                    GET_PLAYERBOT_AI(leader)->rpgInfo.ChangeToGoGrind(
                        WorldPosition(trip.dungeonMap, best->x, best->y, best->z));
            }
            ++it;
            continue;
        }

        if (trip.phase == Phase::Summoning)
        {
            // Meeting stones sit at dungeon entrances, so "leader arrives, everyone
            // else gets summoned" is exactly what a real group does.
            uint32 summoned = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == leader || member->IsBeingTeleported())
                    continue;
                if (member->GetMapId() == leader->GetMapId() &&
                    member->GetDistance(leader) <= _arriveRange)
                    continue;

                float x, y, z;
                leader->GetClosePoint(x, y, z, member->GetObjectSize(), 6.0f,
                                      frand(0.0f, 2.0f * static_cast<float>(M_PI)));
                member->TeleportTo(leader->GetMapId(), x, y, z, leader->GetOrientation());
                ++summoned;
            }

            if (summoned)
                LOG_INFO("playerbots", "Party assembler: summoned {} to the stone at {}",
                         summoned, trip.name);

            // Zone in on the following tick, once summons have landed.
            if (!summoned)
            {
                if (!EnterInstance(group, trip))
                {
                    EndRun(trip.runId, trip.dungeonMap, "enter_failed");
                    it = _trips.erase(it);
                    continue;
                }
                trip.phase = Phase::Inside;
                trip.ticks = 0;
                ++_statEntered;
                if (trip.runId)
                    CharacterDatabase.Execute(
                        "UPDATE aetherion_run_history SET entered_at = UNIX_TIMESTAMP()"
                        " WHERE id = {}", trip.runId);
            }
        }

        ++it;
    }
}

float PartyAssembler::PartyAvgIlvl(Group* group)
{
    float sum = 0;
    uint32 n = 0;
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* member = itr->GetSource())
        {
            sum += member->GetAverageItemLevelForDF();
            ++n;
        }
    return n ? sum / n : 0.f;
}

// The middle path between a hard gate and a free-for-all: at or above the
// floor the full knob applies; the chance ramps linearly to zero twenty item
// levels below it. The party's AVERAGE decides, so one green member drags the
// odds instead of vetoing the run, and an all-green party still stays home.
uint32 PartyAssembler::GearScaledPct(Group* group, uint32 mapId, uint8 difficulty,
                                     uint32 fullPct) const
{
    auto const it = _ilvlFloor.find((mapId << 8) | difficulty);
    if (it == _ilvlFloor.end())
        return fullPct;
    float const avg = PartyAvgIlvl(group);
    if (avg >= it->second)
        return fullPct;
    float const deficit = float(it->second) - avg;
    if (deficit >= 20.f)
        return 0;
    return uint32(float(fullPct) * (1.f - deficit / 20.f));
}

bool PartyAssembler::SendPartyToDungeon(Group* group, Player* leader)
{
    if (_entrances.empty())
        return false;

    uint8 const level = leader->GetLevel();

    // Candidate dungeons the party is the right level for and that we know a door to.
    // Name travels with the entrance: keeping them in separate variables logged one
    // dungeon's name against another's coordinates.
    struct Option { Entrance where; std::string name; uint32 dungeonMap; };
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

        // The entrance must be on the continent the party is standing on. RPG
        // movement walks; it cannot path across an ocean, so a cross-map target
        // leaves the party standing exactly where it formed.
        if (it->second.map != leader->GetMapId())
            continue;

        options.push_back({it->second, dungeon->Name[0] ? dungeon->Name[0] : "a dungeon",
                           dungeon->MapID});
    }

    if (options.empty())
        return false;

    // Closest-first, then a random pick among the nearest few: keeps some variety
    // without sending a party to the far side of the continent.
    std::sort(options.begin(), options.end(), [leader](Option const& a, Option const& b) {
        return PlanarDistance(leader, a.where) < PlanarDistance(leader, b.where);
    });
    Option const& chosen =
        options[urand(0, std::min<size_t>(options.size(), _nearestChoices) - 1)];

    if (!GET_PLAYERBOT_AI(leader))
        return false;

    // Heroic mode for the walked-in party: the map must carry the mode, then
    // the gear-scaled roll decides. Access rows still hard-gate the non-gear
    // requirements (levels, quests, attunements) member by member.
    Difficulty dungeonDiff = DUNGEON_DIFFICULTY_NORMAL;
    if (_raidHeroicPct && GetMapDifficultyData(chosen.dungeonMap, DUNGEON_DIFFICULTY_HEROIC) &&
        urand(1, 100) <=
            GearScaledPct(group, chosen.dungeonMap, DUNGEON_DIFFICULTY_HEROIC, _raidHeroicPct))
    {
        bool everyone = true;
        DungeonProgressionRequirements const* ar =
            sObjectMgr->GetAccessRequirement(chosen.dungeonMap, DUNGEON_DIFFICULTY_HEROIC);
        if (ar)
            for (GroupReference* itr = group->GetFirstMember(); everyone && itr != nullptr;
                 itr = itr->next())
                if (Player* member = itr->GetSource())
                    everyone = member->Satisfy(ar, chosen.dungeonMap);
        if (everyone)
            dungeonDiff = DUNGEON_DIFFICULTY_HEROIC;
    }
    group->SetDungeonDifficulty(dungeonDiff);

    float const away = PlanarDistance(leader, chosen.where);
    Departure const start = BeginTravel(group, leader, chosen.where);
    Travel const how = start.how;

    LOG_INFO("playerbots", "Party assembler: {} heads for {}{} ({:.0f} yards, {}) - "
             "party waits for the summon",
             leader->GetName(), chosen.name,
             dungeonDiff == DUNGEON_DIFFICULTY_HEROIC ? " (heroic)" : "",
             away, TravelName(how));

    // Only the leader travels. The rest wait where they are, exactly as a group waits
    // in a city for a summon rather than all running separately.
    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{chosen.dungeonMap, chosen.where, chosen.name, Phase::Travelling, how,
             start.place, start.actor, 0};
    trip.runId = RecordRunStart(group, leader, chosen.name, chosen.dungeonMap, false,
                                uint8(dungeonDiff), how, uint32(away));
    ++_statTrips;
    return true;
}

namespace
{
    // Where a hearthstone or a mage portal actually lands you. One entry per faction
    // per continent; the neutral cities serve both.
    struct Capital
    {
        uint32 map;
        uint32 teamId;   // TEAM_NEUTRAL means either faction
        float x, y, z;
        char const* name;
    };

    Capital const kCapitals[] = {
        {   0, TEAM_ALLIANCE, -8833.38f,   628.62f,   94.00f, "Stormwind"  },
        {   0, TEAM_HORDE,     1633.75f,   240.19f,   65.10f, "Undercity"  },
        {   1, TEAM_ALLIANCE,  9947.52f,  2482.73f, 1316.21f, "Darnassus"  },
        {   1, TEAM_HORDE,     1629.36f, -4373.39f,   31.26f, "Orgrimmar"  },
        { 530, TEAM_NEUTRAL,  -1887.00f,  5359.00f,  -12.40f, "Shattrath"  },
        { 571, TEAM_NEUTRAL,   5804.15f,   624.77f,  647.77f, "Dalaran"    },
    };
}

namespace
{
    // Dungeon names carry apostrophes - Onyxia's Lair, Zul'Drak, Ahn'kahet - which
    // would break the statement outright and open an injection path besides.
    std::string Sql(std::string value)
    {
        CharacterDatabase.EscapeString(value);
        return value;
    }
}

char const* PartyAssembler::PhaseName(Phase phase)
{
    switch (phase)
    {
        case Phase::Summoning: return "summoning";
        case Phase::Inside:    return "inside";
        default:               return "travelling";
    }
}

uint32 PartyAssembler::RecordRunStart(Group* group, Player* leader, std::string const& name,
                                      uint32 mapId, bool isRaid, uint8 difficulty, Travel how,
                                      uint32 startYards)
{
    uint32 const id = ++_runSeq;
    CharacterDatabase.Execute(
        "INSERT INTO aetherion_run_history (id, group_id, started_at, dungeon, map, is_raid,"
        " difficulty, size, leader, leader_class, avg_ilvl, via, start_yards)"
        " VALUES ({}, {}, UNIX_TIMESTAMP(), '{}', {}, {}, {}, {}, '{}', {}, {}, '{}', {})",
        id, group->GetGUID().GetCounter(), Sql(name), mapId, isRaid ? 1 : 0, uint32(difficulty),
        group->GetMembersCount(), Sql(leader->GetName()), uint32(leader->getClass()),
        uint32(PartyAvgIlvl(group)), TravelName(how), startYards);

    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* m = itr->GetSource())
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_run_members (run_id, guid, name, class, level, role)"
                " VALUES ({}, {}, '{}', {}, {}, '{}')",
                id, m->GetGUID().GetCounter(), Sql(m->GetName()), uint32(m->getClass()),
                m->GetLevel(), PlayerbotAI::IsTank(m) ? "tank" : PlayerbotAI::IsHeal(m) ? "healer" : "dps");
    return id;
}

void PartyAssembler::EndRun(uint32 runId, uint32 mapId, char const* outcome)
{
    if (!runId)
        return;
    // Boss progress snapshots from the members' instance save at ending time,
    // in pure SQL - the C++ side never has to marshal encounter state. The
    // COALESCE keeps an earlier count when the save is already gone.
    CharacterDatabase.Execute(
        "UPDATE aetherion_run_history SET ended_at = UNIX_TIMESTAMP(), outcome = '{}',"
        " bosses_downed = COALESCE((SELECT MAX(BIT_COUNT(i.completedEncounters))"
        "   FROM aetherion_run_members m"
        "   JOIN character_instance ci ON ci.guid = m.guid"
        "   JOIN instance i ON i.id = ci.instance AND i.map = {}"
        "  WHERE m.run_id = {}), bosses_downed)"
        " WHERE id = {}",
        outcome, mapId, runId, runId);
}

void PartyAssembler::EnsureTelemetryTables()
{
    CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS aetherion_party_trips");
    CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS aetherion_party_members");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_party_trips ("
        " group_id INT UNSIGNED NOT NULL PRIMARY KEY,"
        " leader VARCHAR(24) NOT NULL, leader_level TINYINT UNSIGNED NOT NULL,"
        " size TINYINT UNSIGNED NOT NULL, is_raid TINYINT UNSIGNED NOT NULL,"
        " min_level TINYINT UNSIGNED NOT NULL, max_level TINYINT UNSIGNED NOT NULL,"
        " dungeon VARCHAR(64) NOT NULL, dungeon_map INT UNSIGNED NOT NULL,"
        " phase VARCHAR(16) NOT NULL, via VARCHAR(16) NOT NULL,"
        " remaining_yards INT UNSIGNED NOT NULL, ticks INT UNSIGNED NOT NULL,"
        " leader_class TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " via_place VARCHAR(32) NOT NULL DEFAULT '',"
        " via_actor VARCHAR(24) NOT NULL DEFAULT ''"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Composition, so the board can show who is actually in the party rather than a
    // bare count. Class travels with each row because the dashboard colours by it.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_party_members ("
        " group_id INT UNSIGNED NOT NULL, guid INT UNSIGNED NOT NULL,"
        " name VARCHAR(24) NOT NULL, class TINYINT UNSIGNED NOT NULL,"
        " level TINYINT UNSIGNED NOT NULL, is_leader TINYINT UNSIGNED NOT NULL,"
        " role VARCHAR(8) NOT NULL,"
        " PRIMARY KEY (group_id, guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // What every bot is doing, from the RPG status machine - the map's activity layer
    // reads this. Only non-idle rows are written: absence means idle, and that keeps
    // the rewrite to ~900 rows instead of 2500.
    CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS aetherion_bot_activity");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_bot_activity ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " status TINYINT UNSIGNED NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Run history is DURABLE - never dropped on boot, unlike the live mirrors
    // above. It answers "why did that run fail" days later: identity, gearing
    // at formation, how far it got, and how it ended.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_run_history ("
        " id INT UNSIGNED NOT NULL PRIMARY KEY,"
        " group_id INT UNSIGNED NOT NULL,"
        " started_at INT UNSIGNED NOT NULL,"
        " ended_at INT UNSIGNED NOT NULL DEFAULT 0,"
        " dungeon VARCHAR(64) NOT NULL, map INT UNSIGNED NOT NULL,"
        " is_raid TINYINT UNSIGNED NOT NULL, difficulty TINYINT UNSIGNED NOT NULL,"
        " size TINYINT UNSIGNED NOT NULL,"
        " leader VARCHAR(24) NOT NULL, leader_class TINYINT UNSIGNED NOT NULL,"
        " avg_ilvl SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
        " via VARCHAR(16) NOT NULL DEFAULT '',"
        " start_yards INT UNSIGNED NOT NULL DEFAULT 0,"
        " reached_door_at INT UNSIGNED NOT NULL DEFAULT 0,"
        " entered_at INT UNSIGNED NOT NULL DEFAULT 0,"
        " outcome VARCHAR(16) NOT NULL DEFAULT 'underway',"
        " bosses_downed TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " wipes TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " KEY idx_started (started_at), KEY idx_map (map)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // The history table is durable, so schema growth has to migrate in place -
    // MySQL 8 has no ADD COLUMN IF NOT EXISTS, hence the probe.
    if (!CharacterDatabase.Query("SHOW COLUMNS FROM aetherion_run_history LIKE 'wipes'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN wipes TINYINT UNSIGNED NOT NULL DEFAULT 0");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_run_members ("
        " run_id INT UNSIGNED NOT NULL, guid INT UNSIGNED NOT NULL,"
        " name VARCHAR(24) NOT NULL, class TINYINT UNSIGNED NOT NULL,"
        " level TINYINT UNSIGNED NOT NULL, role VARCHAR(8) NOT NULL,"
        " PRIMARY KEY (run_id, guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Two weeks of history is the analysis window; members prune with runs.
    CharacterDatabase.Execute(
        "DELETE m FROM aetherion_run_members m JOIN aetherion_run_history h"
        " ON h.id = m.run_id WHERE h.started_at < UNIX_TIMESTAMP() - 14*86400");
    CharacterDatabase.Execute(
        "DELETE FROM aetherion_run_history WHERE started_at < UNIX_TIMESTAMP() - 14*86400");

    if (QueryResult seq =
            CharacterDatabase.Query("SELECT COALESCE(MAX(id), 0) FROM aetherion_run_history"))
        _runSeq = (*seq)[0].Get<uint32>();

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_assembler ("
        " id TINYINT UNSIGNED NOT NULL PRIMARY KEY,"
        " formed INT UNSIGNED NOT NULL, raids INT UNSIGNED NOT NULL,"
        " trips INT UNSIGNED NOT NULL, stalls INT UNSIGNED NOT NULL,"
        " arrived INT UNSIGNED NOT NULL, entered INT UNSIGNED NOT NULL,"
        " active INT UNSIGNED NOT NULL, entrances INT UNSIGNED NOT NULL,"
        " arrival_points INT UNSIGNED NOT NULL, raid_maps INT UNSIGNED NOT NULL,"
        " updated_at INT UNSIGNED NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
}

void PartyAssembler::WriteTelemetry()
{
    // At most _maxParties rows, rewritten wholesale: simpler than reconciling and
    // cheap at this size.
    CharacterDatabase.Execute("TRUNCATE TABLE aetherion_party_trips");
    CharacterDatabase.Execute("TRUNCATE TABLE aetherion_party_members");
    CharacterDatabase.Execute("TRUNCATE TABLE aetherion_bot_activity");

    // Batched inserts: one statement per ~400 rows keeps each packet small while the
    // whole export stays a handful of async statements per tick.
    {
        std::string values;
        uint32 batched = 0;
        for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
             it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
        {
            Player* bot = it->second;
            if (!bot || !bot->IsInWorld())
                continue;
            PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot);
            if (!botAI)
                continue;
            uint32 const status = uint32(botAI->rpgInfo.GetStatus());
            if (!status)
                continue;   // idle is the absence of a row

            if (!values.empty())
                values += ",";
            values += "(" + std::to_string(bot->GetGUID().GetCounter()) + "," +
                      std::to_string(status) + ")";
            if (++batched >= 400)
            {
                CharacterDatabase.Execute(
                    "INSERT INTO aetherion_bot_activity (guid, status) VALUES " + values);
                values.clear();
                batched = 0;
            }
        }
        if (!values.empty())
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_bot_activity (guid, status) VALUES " + values);
    }

    for (auto const& entry : _trips)
    {
        Group* group = sGroupMgr->GetGroupByGUID(entry.first);
        if (!group)
            continue;
        Trip const& trip = entry.second;

        Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
        if (!leader)
            continue;

        uint8 lo = 255, hi = 0, size = 0;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* m = ref->GetSource();
            if (!m)
                continue;
            lo = std::min<uint8>(lo, m->GetLevel());
            hi = std::max<uint8>(hi, m->GetLevel());
            ++size;

            // Role is the same loose test used to build the party, so what the board
            // shows is what the assembler actually reasoned about.
            char const* role = PlayerbotAI::IsTank(m) ? "tank" : PlayerbotAI::IsHeal(m) ? "healer" : "dps";
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_party_members (group_id, guid, name, class, level, "
                "is_leader, role) VALUES ({}, {}, '{}', {}, {}, {}, '{}')",
                entry.first, m->GetGUID().GetCounter(), Sql(m->GetName()),
                uint32(m->getClass()), m->GetLevel(), m == leader ? 1 : 0, role);
        }
        if (!size)
            continue;

        uint32 remaining = 0;
        if (trip.phase != Phase::Inside)
            remaining = static_cast<uint32>(PlanarDistance(leader, trip.door));

        CharacterDatabase.Execute(
            "INSERT INTO aetherion_party_trips (group_id, leader, leader_level, size, "
            "is_raid, min_level, max_level, dungeon, dungeon_map, phase, via, "
            "remaining_yards, ticks, leader_class, via_place, via_actor) "
            "VALUES ({}, '{}', {}, {}, {}, {}, {}, '{}', {}, '{}', '{}', {}, {}, {}, "
            "'{}', '{}')",
            entry.first, Sql(leader->GetName()), leader->GetLevel(), size,
            group->isRaidGroup() ? 1 : 0, lo, hi, Sql(trip.name),
            trip.dungeonMap, PhaseName(trip.phase), TravelName(trip.how),
            remaining, trip.ticks, uint32(leader->getClass()),
            Sql(trip.place), Sql(trip.actor));
    }

    CharacterDatabase.Execute(
        "REPLACE INTO aetherion_assembler (id, formed, raids, trips, stalls, arrived, "
        "entered, active, entrances, arrival_points, raid_maps, updated_at) "
        "VALUES (1, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, UNIX_TIMESTAMP())",
        _statFormed, _statRaids, _statTrips, _statStalls, _statArrived, _statEntered,
        uint32(_assembled.size()), uint32(_entrances.size()), uint32(_insides.size()),
        _raidMapCount);
}

char const* PartyAssembler::TravelName(Travel how)
{
    switch (how)
    {
        case Travel::Hearth: return "hearthed";
        case Travel::Portal: return "portalled";
        default:             return "on foot";
    }
}

float PartyAssembler::PlanarDistance(Player const* from, Entrance const& to)
{
    float const dx = from->GetPositionX() - to.x;
    float const dy = from->GetPositionY() - to.y;
    return std::sqrt(dx * dx + dy * dy);
}

bool PartyAssembler::NearestCapital(uint32 map, uint32 teamId, Entrance& out,
                                    std::string& name) const
{
    for (Capital const& c : kCapitals)
    {
        if (c.map != map)
            continue;
        if (c.teamId != TEAM_NEUTRAL && c.teamId != teamId)
            continue;
        out = Entrance{c.map, c.x, c.y, c.z};
        name = c.name;
        return true;
    }
    return false;
}

Player* PartyAssembler::FindClassMember(Group* group, uint8 cls, uint8 minLevel)
{
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (member && member->getClass() == cls && member->GetLevel() >= minLevel)
            return member;
    }
    return nullptr;
}

void PartyAssembler::PartySay(Group* group, Player* speaker, std::string const& text) const
{
    WorldPacket data;
    ChatHandler::BuildChatPacket(data, CHAT_MSG_PARTY, text, LANG_UNIVERSAL, CHAT_TAG_NONE,
                                 speaker->GetGUID(), speaker->GetName());
    group->BroadcastPacket(&data, false);
}

PartyAssembler::Departure PartyAssembler::BeginTravel(Group* group, Player* leader,
                                                      Entrance const& door)
{
    Departure out;

    // Anything beyond a comfortable run gets a shortcut, exactly as a player would
    // rather than jogging across a continent.
    if (PlanarDistance(leader, door) > _footRange || leader->GetMapId() != door.map)
    {
        Entrance city;
        std::string cityName;
        if (NearestCapital(door.map, leader->GetTeamId(), city, cityName))
        {
            Player* mage = FindClassMember(group, CLASS_MAGE, 40);
            if (mage && urand(1, 100) <= _portalPct)
            {
                out.how = Travel::Portal;
                out.place = cityName;
                out.actor = mage->GetName();
                PartySay(group, mage, "Portal to " + cityName + " is up.");
                for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                {
                    Player* member = ref->GetSource();
                    if (member && !member->IsBeingTeleported())
                        member->TeleportTo(city.map, city.x, city.y, city.z, 0.0f);
                }
            }
            else
            {
                out.how = Travel::Hearth;
                out.place = cityName;
                PartySay(group, leader,
                         "Hearthing to " + cityName + ", I will summon you at the stone.");
                if (!leader->IsBeingTeleported())
                    leader->TeleportTo(city.map, city.x, city.y, city.z, 0.0f);
            }
        }
    }

    // The last stretch is always on foot, whichever way they got to the city.
    if (PlayerbotAI* ai = GET_PLAYERBOT_AI(leader))
        ai->rpgInfo.ChangeToGoGrind(WorldPosition(door.map, door.x, door.y, door.z));

    return out;
}

bool PartyAssembler::HasRaidTarget(Player const* leader, uint8& lowestFloor) const
{
    uint8 const level = leader->GetLevel();
    bool found = false;
    for (auto const& entry : _entrances)
    {
        MapEntry const* map = sMapStore.LookupEntry(entry.first);
        if (!map || !map->IsRaid())
            continue;
        auto const floorIt = _mapMinLevel.find(entry.first);
        if (floorIt == _mapMinLevel.end() || level < floorIt->second)
            continue;
        if (entry.second.map != leader->GetMapId())
            continue;
        if (!found || floorIt->second < lowestFloor)
            lowestFloor = floorIt->second;
        found = true;
    }
    return found;
}

bool PartyAssembler::SendPartyToRaid(Group* group, Player* leader)
{
    if (_entrances.empty())
        return false;

    // The whole group must clear the door, not just the leader: TeleportTo
    // refuses each member individually, and a raid that sheds its low-level
    // half at the portal fights with whoever remains.
    uint8 level = leader->GetLevel();
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* member = itr->GetSource())
            level = std::min<uint8>(level, member->GetLevel());

    uint32 const size = group->GetMembersCount();

    struct Option { Entrance where; std::string name; uint32 raidMap; Difficulty diff; };
    std::vector<Option> options;

    // Every member must satisfy the access rows for the chosen difficulty -
    // this is what keeps a heroic Trial of the Grand Crusader pick from
    // marching an unattuned raid to a door that refuses it.
    auto allSatisfy = [group](uint32 mapId, Difficulty diff)
    {
        DungeonProgressionRequirements const* ar = sObjectMgr->GetAccessRequirement(mapId, diff);
        if (!ar)
            return true;
        for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
            if (Player* member = itr->GetSource())
                if (!member->Satisfy(ar, mapId))
                    return false;
        return true;
    };

    for (auto const& entry : _entrances)
    {
        uint32 const raidMap = entry.first;
        Entrance const& door = entry.second;

        MapEntry const* map = sMapStore.LookupEntry(raidMap);
        if (!map || !map->IsRaid())
            continue;

        // Stands in for attunement: without a level floor a group of sixties walks
        // all the way to Icecrown and is refused at the portal.
        auto const floorIt = _mapMinLevel.find(raidMap);
        if (floorIt == _mapMinLevel.end() || level < floorIt->second)
            continue;

        // Same continent, for the same reason a dungeon target must be: the leader
        // walks, and nothing paths across an ocean.
        if (door.map != leader->GetMapId())
            continue;

        // Fit the difficulty to the group's size and this map's actual modes.
        // Legacy raids expose only the 10-normal slot with a large player cap,
        // so a 25-strong group still qualifies there at that slot; a map whose
        // cap is genuinely 10 drops out for the big group instead of shedding
        // fifteen members at the portal.
        Difficulty diff;
        if (size > 10)
        {
            if (GetMapDifficultyData(raidMap, RAID_DIFFICULTY_25MAN_NORMAL))
                diff = RAID_DIFFICULTY_25MAN_NORMAL;
            else
            {
                MapDifficulty const* legacy =
                    GetMapDifficultyData(raidMap, RAID_DIFFICULTY_10MAN_NORMAL);
                if (!legacy || legacy->maxPlayers < size)
                    continue;
                diff = RAID_DIFFICULTY_10MAN_NORMAL;
            }
        }
        else
            diff = RAID_DIFFICULTY_10MAN_NORMAL;

        // Soft gear gate on the normal mode: a party averaging deeper than
        // fifteen below the floor skips this destination; slightly under
        // still marches. Nobody is individually vetoed on gear.
        {
            auto const fIt = _ilvlFloor.find((raidMap << 8) | uint8(diff));
            if (fIt != _ilvlFloor.end() &&
                PartyAvgIlvl(group) < float(fIt->second) - 15.f)
                continue;
        }

        Difficulty const heroic =
            size > 10 ? RAID_DIFFICULTY_25MAN_HEROIC : RAID_DIFFICULTY_10MAN_HEROIC;
        if (_raidHeroicPct && GetMapDifficultyData(raidMap, heroic) &&
            urand(1, 100) <=
                GearScaledPct(group, raidMap, uint8(heroic), _raidHeroicPct) &&
            allSatisfy(raidMap, heroic))
            diff = heroic;

        if (!allSatisfy(raidMap, diff))
            continue;

        options.push_back({door, map->name[0] ? map->name[0] : "a raid", raidMap, diff});
    }

    if (options.empty())
        return false;

    // Closest-first, then a random pick among the nearest few: keeps some variety
    // without sending a party to the far side of the continent.
    std::sort(options.begin(), options.end(), [leader](Option const& a, Option const& b) {
        return PlanarDistance(leader, a.where) < PlanarDistance(leader, b.where);
    });
    Option const& chosen =
        options[urand(0, std::min<size_t>(options.size(), _nearestChoices) - 1)];

    if (!GET_PLAYERBOT_AI(leader))
        return false;

    // Direct call, not the client opcode: the handler refuses changes once
    // anyone stands on a dungeon map, and every member here is outdoors at
    // trip start. Propagates to all members and persists.
    group->SetRaidDifficulty(chosen.diff);

    float const away = PlanarDistance(leader, chosen.where);
    Departure const start = BeginTravel(group, leader, chosen.where);
    Travel const how = start.how;

    LOG_INFO("playerbots",
             "Party assembler: {} leads a {}-strong raid to {} ({:.0f} yards, {}, difficulty {})",
             leader->GetName(), group->GetMembersCount(), chosen.name, away, TravelName(how),
             uint32(chosen.diff));

    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{chosen.raidMap, chosen.where, chosen.name, Phase::Travelling, how,
             start.place, start.actor, 0};
    trip.runId = RecordRunStart(group, leader, chosen.name, chosen.raidMap, true,
                                uint8(chosen.diff), how, uint32(away));
    ++_statTrips;
    return true;
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

    // Muster: while a faction musters, its level-capped candidates are held
    // back from ordinary formation so the free bench can grow - measured
    // without this, churn kept the bench at 8-9 and a 25 could never seat.
    // The moment the called can fill a 25 on one map, the raid rises from
    // them directly.
    Player* musterLeader = nullptr;
    if (_musterTeam >= 0)
    {
        std::vector<Player*> called;
        for (Player* bot : pool)
            if (bot->GetLevel() >= 80 && bot->GetTeamId() == TeamId(_musterTeam))
                called.push_back(bot);

        // Seat realm-wide: the whole ungrouped capped bench is barely above
        // 25, so demanding one map could never fill. Formation teleports
        // members to the leader anyway, which is exactly what a summon does.
        if (called.size() >= 25)
        {
            pool.swap(called);
            // Prefer a leader with a raid door on their own continent; a
            // mustered raid led from a doorless map would dissolve unspent.
            std::vector<Player*> doorled;
            uint8 floorScratch = 0;
            for (Player* bot : pool)
                if (HasRaidTarget(bot, floorScratch))
                    doorled.push_back(bot);
            musterLeader = doorled.empty() ? pool[urand(0, pool.size() - 1)]
                                           : doorled[urand(0, doorled.size() - 1)];
            LOG_INFO("playerbots",
                     "Party assembler: the muster answers - {} capped {} raise a raid",
                     pool.size(), _musterTeam == TEAM_ALLIANCE ? "Alliance" : "Horde");
            _musterTeam = -1;
        }
        else
        {
            pool.erase(std::remove_if(pool.begin(), pool.end(),
                           [this](Player* b)
                           {
                               return b->GetLevel() >= 80 &&
                                      b->GetTeamId() == TeamId(_musterTeam);
                           }),
                       pool.end());
            if (pool.size() < _targetSize)
                return false;
        }
    }

    // Leader first, then everyone compatible with the leader. Picking the leader at
    // random rather than by level keeps parties spread across the level brackets
    // instead of always forming at the cap.
    Player* leader = musterLeader ? musterLeader : pool[urand(0, pool.size() - 1)];

    // Only now, with a leader in hand, can we tell whether a raid is even possible:
    // it needs the size, the level and a raid entrance on this continent. A
    // mustered pool skips the rolls - the muster was the decision.
    uint8 raidFloor = 0;
    bool wantRaid = musterLeader
        ? HasRaidTarget(leader, raidFloor)
        : _raidPct && _raidSize > _targetSize && pool.size() >= _raidSize &&
              urand(1, 100) <= _raidPct && HasRaidTarget(leader, raidFloor);
    uint32 targetSize = wantRaid ? _raidSize : _targetSize;

    std::vector<Player*> compatible;
    for (Player* bot : pool)
    {
        if (bot == leader)
            continue;
        if (bot->GetTeamId() != leader->GetTeamId())
            continue;
        // Same continent at minimum: a party whose members are on different maps
        // cannot travel together and looks like five unrelated bots. A mustered
        // raid is the exception - its members are summoned to the leader.
        if (_sameMapOnly && !musterLeader && bot->GetMapId() != leader->GetMapId())
            continue;
        uint32 const diff = bot->GetLevel() > leader->GetLevel()
                                ? bot->GetLevel() - leader->GetLevel()
                                : leader->GetLevel() - bot->GetLevel();
        if (diff > _levelSpread)
            continue;
        // A member below every reachable raid's floor would sink the whole
        // trip: the door check uses the group's lowest level, so one such
        // pick turns the raid into an immediate dissolve.
        if (wantRaid && bot->GetLevel() < raidFloor)
            continue;
        compatible.push_back(bot);
    }

    // A raid grows to 25 only when the compatible bench can actually seat it -
    // chosen here, after filtering, so there is never a 25-intent that must
    // shed members later. A mustered pool takes the seat without a roll; the
    // muster existed for exactly this.
    if (wantRaid && compatible.size() + 1 >= 25 &&
        (musterLeader || (_raid25Pct && urand(1, 100) <= _raid25Pct)))
        targetSize = 25;

    if (compatible.size() + 1 < targetSize)
    {
        if (!wantRaid || compatible.size() + 1 < _targetSize)
            return false;
        wantRaid = false;
        targetSize = _targetSize;
    }

    // Role floors scale with the format: a party runs on one tank and one
    // healer, a 10-raid on two and three, a 25-raid on three and six - the
    // shape a real roster would bring, and what raid bosses' damage patterns
    // assume.
    std::vector<Player*> picked;
    auto take = [&picked, &compatible](auto predicate) {
        auto it = std::find_if(compatible.begin(), compatible.end(), predicate);
        if (it == compatible.end())
            return false;
        picked.push_back(*it);
        compatible.erase(it);
        return true;
    };

    // Selection is by TRUE role - the bot's talent-driven strategy, not its
    // class. Class-based picks seated six "tanks" in one ten-raid: every
    // plate wearer counted, and the filler stacked more. Best practice:
    // 1/1/3 for a party, 2/3/5 for a ten, 3/6/16 for a twenty-five.
    uint32 tanksNeeded = targetSize >= 25 ? 3 : wantRaid ? 2 : 1;
    uint32 healersNeeded = targetSize >= 25 ? 6 : wantRaid ? 3 : 1;
    if (PlayerbotAI::IsTank(leader) && tanksNeeded)
        --tanksNeeded;
    else if (PlayerbotAI::IsHeal(leader) && healersNeeded)
        --healersNeeded;
    while (tanksNeeded && take([](Player* p) { return PlayerbotAI::IsTank(p); }))
        --tanksNeeded;
    while (healersNeeded && take([](Player* p) { return PlayerbotAI::IsHeal(p); }))
        --healersNeeded;

    // Fill with damage first; only a short bench lets extra tanks or healers
    // ride along as makeshift dps.
    while (picked.size() + 1 < targetSize &&
           take([](Player* p) { return !PlayerbotAI::IsTank(p) && !PlayerbotAI::IsHeal(p); }))
    {
    }
    while (picked.size() + 1 < targetSize && !compatible.empty())
    {
        picked.push_back(compatible.back());
        compatible.pop_back();
    }

    if (picked.size() + 1 < targetSize)
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

    // Must happen before members are added: a party group reports itself full at five,
    // so converting afterwards leaves a raid that silently stayed a five-man.
    if (wantRaid)
    {
        group->ConvertToRaid();
        // Baseline every raid at 10-normal: members carry personal difficulty
        // between groups, so without this a bot who once raided heroic seeds
        // its next group with a lockout nobody chose. SendPartyToRaid sets the
        // real difficulty once the destination is known.
        group->SetRaidDifficulty(RAID_DIFFICULTY_10MAN_NORMAL);
    }

    uint32 added = 0;
    uint32 gathered = 0;
    for (Player* member : picked)
    {
        if (group->IsFull())
            break;
        if (!group->AddMember(member))
            continue;

        ++added;

        // Bind the member to its leader exactly as the invitation flow does. AddMember
        // skips that flow, and without a master the "follow" and "dps assist"
        // strategies every member already carries have nothing to act on - which is why
        // a party teleported into a dungeon stands on the entry point and never fights.
        if (PlayerbotAI* memberAI = GET_PLAYERBOT_AI(member))
        {
            memberAI->SetMaster(leader);
            memberAI->ResetStrategies();
            memberAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
            memberAI->Reset();
        }

        // Anyone joining from across the zone is brought to the leader. Without this
        // the party spends its first several minutes walking towards each other
        // instead of towards anything interesting.
        if (_teleport && !member->IsBeingTeleported() &&
            (member->GetMapId() != leader->GetMapId() ||
             member->GetDistance(leader) > _gatherRange))
        {
            float x, y, z;
            leader->GetClosePoint(x, y, z, member->GetObjectSize(), 5.0f,
                                  frand(0.0f, 2.0f * static_cast<float>(M_PI)));
            member->TeleportTo(leader->GetMapId(), x, y, z, leader->GetOrientation());
            ++gathered;
        }
    }

    if (!added)
    {
        group->Disband();
        return false;
    }

    _assembled.insert(group->GetGUID().GetCounter());
    ++_statFormed;
    if (wantRaid)
        ++_statRaids;

    LOG_INFO("playerbots", "Party assembler: {} (level {}) formed a {} of {} ({} gathered)",
             leader->GetName(), leader->GetLevel(), wantRaid ? "raid" : "party",
             added + 1, gathered);

    // Travel first: a party that walks to the door reads as players going somewhere.
    // The dungeon finder stays available as a fallback for parties with no reachable
    // dungeon at their level.
    bool travelling = false;
    if (_travelToDungeon)
        travelling = wantRaid ? SendPartyToRaid(group, leader)
                              : SendPartyToDungeon(group, leader);

    // The dungeon finder is the fallback for a party with no reachable dungeon at its
    // level. It is never a fallback for a raid: WotLK has no raid finder.
    if (!travelling && !wantRaid && _queueLfg)
        QueueForDungeon(leader);

    // A raid that cannot start its trip dissolves on the spot. The formation
    // gate only sees the leader; the trip check sees the whole group's levels
    // and gear, so it can be stricter - and a raid it refuses would otherwise
    // idle in the cap for the rest of the uptime with no timer to reap it.
    if (!travelling && wantRaid)
    {
        LOG_INFO("playerbots",
                 "Party assembler: {}'s raid found no reachable target and dissolves",
                 leader->GetName());
        group->Disband();
        return false;
    }

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

    // Muster clock. One tick is one assembler interval, so minutes convert at
    // the configured cadence rather than wall time - close enough for a
    // mechanism whose only job is "hold the bench for a while, then let go".
    if (_musterEveryMin)
    {
        uint32 const ticksPerMin = std::max<uint32>(1, 60000u / std::max<uint32>(1, _intervalMs));
        if (_musterTeam < 0)
        {
            if (++_musterCooldownTicks >= _musterEveryMin * ticksPerMin)
            {
                _musterCooldownTicks = 0;
                _musterAgeTicks = 0;
                _musterTeam = urand(0, 1) ? TEAM_ALLIANCE : TEAM_HORDE;
                LOG_INFO("playerbots", "Party assembler: the {} muster their capped ranks",
                         _musterTeam == TEAM_ALLIANCE ? "Alliance" : "Horde");
            }
        }
        else if (++_musterAgeTicks >= _musterTimeoutMin * ticksPerMin)
        {
            LOG_INFO("playerbots",
                     "Party assembler: the muster lapses with too few answering the call");
            _musterTeam = -1;
        }
    }

    // Journeys already under way are advanced every tick, not just when a new party
    // is formed - that is what keeps a leader walking to the door.
    AdvanceTrips();

    // Stop at the first failure: if the pool cannot produce one party it will not
    // produce a second in the same tick either.
    for (uint32 i = 0; i < _perTick && _assembled.size() < _maxParties; ++i)
        if (!AssembleOne())
            break;

    SweepStrandedBots();
    SyncOwnedMirror();
    WriteTelemetry();
}
