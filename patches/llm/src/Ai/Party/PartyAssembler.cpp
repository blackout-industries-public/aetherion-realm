#include "PartyAssembler.h"

#include "Chat.h"
#include "Config.h"
#include "Group.h"
#include "GroupMgr.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "InstanceScript.h"
#include "LFGMgr.h"
#include "Formations.h"
#include "NeedsLedger.h"   // persona: who goes back for old content
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
    // Group low guid -> the history row this party is running for. Mirrored beside
    // ownership so a map thread can attribute a removal to a run without a group
    // lookup, and without the group id alone having to mean anything. Membership of
    // this map is also what OnRun answers: a group is on a run exactly while it has a
    // live trip, and every trip is bounded by its own tick budget.
    std::unordered_map<uint32, uint32> sRunIdMirror;
    bool sDriveGrouped = false;

    // Defined further down among the telemetry helpers; declared here because trip
    // setup writes a history field long before that point in the file.
    std::string Sql(std::string value);

    // The instance-script vocabulary of the two venues that have to be asked before
    // they will start. These are the script enums from the core's own headers -
    // VioletHold/violet_hold.h and FrozenHalls/HallsOfReflection/halls_of_reflection.h -
    // which sit outside the module's include path, so they are restated rather than
    // guessed. Each is checked against those headers whenever the core is bumped.
    constexpr uint32 kMapVioletHold = 608;
    constexpr uint32 kMapHallsOfReflection = 668;
    constexpr uint32 kVhEncounterStatus = 30;    // DATA_ENCOUNTER_STATUS
    constexpr int32 kVhStartInstance = 1;        // ACTION_START_INSTANCE
    constexpr uint32 kHorIntro = 4;              // DATA_INTRO
    constexpr uint32 kHorWaveNumber = 8;         // DATA_WAVE_NUMBER
    constexpr uint32 kHorShowTrash = 11;         // ACTION_SHOW_TRASH
    // A venue that will not take the ask is asked a few times and then left alone.
    constexpr uint32 kVenueNudges = 4;

    // How many ticks of continuous fighting the steering waits out before it insists
    // again. Long enough for any real pull to finish, short enough that a party
    // deadlocked in a fight it cannot win still gets moved along.
    constexpr uint32 kCombatHoldTicks = 8;

    // What counts as getting somewhere. A party closes roughly twenty yards of ground
    // a tick once it is fighting its way in, so five is a generous floor, and six
    // ticks of failing to clear it is four minutes of a walk that is going nowhere.
    constexpr float kAimProgressYards = 5.0f;
    constexpr uint32 kAimStallTicks = 6;
    // Ground lost that means the party was moved rather than that it failed to walk -
    // three ticks' worth, so ordinary jitter around a target does not clear the count.
    constexpr float kAimResetYards = 60.0f;

    // Old content is only worth going back for once a character has outgrown it by
    // enough that the trip is a collection rather than a progression run.
    constexpr uint8 kCollectorMinLevel = 70;

    // What a twenty-five needs on the bench. The muster's own answer uses the same
    // number, so asking it early is asking exactly the question the wait exists for.
    constexpr uint32 kMusterBench = 25;

    // Venues that run long for their party size. Violet Hold is a five-man on paper,
    // but its twelve waves take as long as a raid wing does, so the ordinary dwell
    // clock expired with the party still mid-assault and nothing to show for it.
    struct VenueClock
    {
        uint32 map;
        uint32 mult;
    };
    constexpr VenueClock kVenueClocks[] = {
        { kMapVioletHold, 2 },
    };

    uint32 VenueClockMult(uint32 mapId)
    {
        for (VenueClock const& venue : kVenueClocks)
            if (venue.map == mapId)
                return venue.mult;
        return 1;
    }
}

bool PartyAssembler::Owns(uint32 groupLowGuid)
{
    std::lock_guard<std::mutex> lock(sOwnedMx);
    return sOwnedMirror.find(groupLowGuid) != sOwnedMirror.end();
}

bool PartyAssembler::OnRun(uint32 groupLowGuid)
{
    std::lock_guard<std::mutex> lock(sOwnedMx);
    return sRunIdMirror.find(groupLowGuid) != sRunIdMirror.end();
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

int32 PartyAssembler::FactionWithFullBench() const
{
    uint32 counts[2] = {0, 0};
    for (Player* bot : Candidates())
        if (bot->GetLevel() >= 80)
            ++counts[bot->GetTeamId() == TEAM_HORDE ? 1 : 0];

    bool const alliance = counts[TEAM_ALLIANCE] >= kMusterBench;
    bool const horde = counts[TEAM_HORDE] >= kMusterBench;
    if (!alliance && !horde)
        return -1;
    // Both sides ready is a good problem: take the deeper bench, so the raid that
    // forms is the one least likely to strip the pool the five-mans draw from.
    if (alliance && horde)
        return counts[TEAM_HORDE] > counts[TEAM_ALLIANCE] ? TEAM_HORDE : TEAM_ALLIANCE;
    return alliance ? TEAM_ALLIANCE : TEAM_HORDE;
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

    // Every live trip, not only the ones that got a history row: the run id is the
    // payload, but presence is the answer OnRun gives, and a trip whose ledger write
    // failed is still a party mid-journey.
    std::unordered_map<uint32, uint32> runIds;
    for (auto const& entry : _trips)
        runIds.emplace(entry.first, entry.second.runId);

    std::lock_guard<std::mutex> lock(sOwnedMx);
    sOwnedMirror.clear();
    sOwnedMirror.insert(_assembled.begin(), _assembled.end());
    sDrivenGuids = std::move(driven);
    sRunIdMirror = std::move(runIds);
}

void PartyAssembler::NoteRemoval(Player const* bot, char const* site, bool suppressed)
{
    Group const* group = bot ? bot->GetGroup() : nullptr;
    if (!group)
        return;

    uint32 const grp = group->GetGUID().GetCounter();
    uint32 runId = 0;
    {
        std::lock_guard<std::mutex> lock(sOwnedMx);
        if (sOwnedMirror.find(grp) == sOwnedMirror.end())
            return;
        if (auto const it = sRunIdMirror.find(grp); it != sRunIdMirror.end())
            runId = it->second;
    }

    CharacterDatabase.Execute(
        "INSERT INTO aetherion_member_loss (at, run_id, group_id, guid, name, site,"
        " suppressed, map) VALUES (UNIX_TIMESTAMP(), {}, {}, {}, '{}', '{}', {}, {})",
        runId, grp, bot->GetGUID().GetCounter(), Sql(bot->GetName()), Sql(site),
        suppressed ? 1 : 0, bot->GetMapId());
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
    _insideTicksRaidMult =
        sConfigMgr->GetOption<int32>("AiPlayerbot.Party.InsideTicksRaidMult", 3);
    // Zero would hand every raid a budget of nothing and expire it on arrival.
    if (!_insideTicksRaidMult)
        _insideTicksRaidMult = 1;
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
    _shortAbortLimit = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.ShortAbortLimit", 3);
    _collectorPct = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.CollectorPct", 60);
    _queueLfg = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.QueueLfg", true);
    _travelToDungeon = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.TravelToDungeon", true);
    sDriveGrouped = sConfigMgr->GetOption<bool>("AiPlayerbot.Party.DriveGroupedBots", false);

    if (_travelToDungeon && _entrances.empty())
    {
        LoadEntrances();
        LoadInsides();
        LoadInstanceSpawns();
        LoadBossPositions();
        // After the doors: a gate is only worth redirecting to if we know the way in.
        LoadQuestGates();
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
        // Only somewhere that is actually an instance. Two triggers stand INSIDE a
        // dungeon, are named "Entrance", and teleport out to the continent - so
        // Eastern Kingdoms itself landed in this table, pointing at a spot inside
        // Shadowfang Keep. The stranded-bot sweep reads this table as "maps we know a
        // way out of", so every ungrouped bot standing in Eastern Kingdoms was
        // "stranded": twenty-five a tick were teleported INTO Shadowfang Keep, swept
        // back out to Silverpine the next tick, and pumped straight back in - for the
        // whole uptime, spawning a fresh instance each time.
        MapEntry const* targetMap = sMapStore.LookupEntry(target);
        if (!targetMap || !targetMap->IsDungeon())
            continue;
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

    // How long the place actually takes to cross. Measured on this realm: a party
    // closes about twenty yards of ground per tick once it is fighting its way in -
    // the same rate in a dungeon that finishes as in one that never does. Azjol-Nerub
    // works because its bosses are 190 yards from the door; the Forge of Souls has
    // never once been finished because its two stand at 500 and 810, which is more
    // walking than the whole dwell budget buys. So the clock is scaled by the ground
    // rather than by a list of map ids: any wing that is long gets the time its
    // length demands, including ones nobody has complained about yet.
    for (auto const& entry : _bosses)
    {
        auto const inside = _insides.find(entry.first);
        if (inside == _insides.end())
            continue;

        float farthest = 0.0f;
        for (Entrance const& boss : entry.second)
        {
            float const dx = boss.x - inside->second.x;
            float const dy = boss.y - inside->second.y;
            farthest = std::max(farthest, std::sqrt(dx * dx + dy * dy));
        }

        // One extra budget per 400 yards of reach, capped so a mistake in the data
        // cannot hand a single party the whole evening.
        uint32 const mult = std::min<uint32>(4, 1 + uint32(farthest / 400.0f));
        if (mult > 1)
            _travelMult[entry.first] = mult;
    }

    LOG_INFO("playerbots",
             "Party assembler: loaded boss positions for {} instance maps, {} of them "
             "long enough to need a wider clock",
             _bosses.size(), _travelMult.size());
}

uint32 PartyAssembler::TravelMultFor(uint32 mapId) const
{
    auto const it = _travelMult.find(mapId);
    return it == _travelMult.end() ? 1u : it->second;
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

    // Violet Hold's areatrigger lands you in Lieutenant Sinclari's antechamber, on the
    // wrong side of the prison door: the party stood there while the assault it had
    // just started ran without it. The instance's own late-join teleport - the one
    // Sinclari's "send me in now" option uses - puts players in the chamber itself.
    // Hard-coded rather than corrected in the table: the trigger position is right for
    // a player walking in through the door, it is only wrong as a destination.
    _insides[kMapVioletHold] = Entrance{kMapVioletHold, 1830.53f, 803.94f, 44.34f};

    LOG_INFO("playerbots", "Party assembler: loaded {} instance arrival points", _insides.size());
}

void PartyAssembler::LoadQuestGates()
{
    _questGates.clear();
    _teaches.clear();
    _unearnable.clear();

    // Where a gating quest is earned is not written down anywhere as such, so it is
    // derived: quest_poi says which map a quest's objectives are on, and for a
    // dungeon attunement that map is the dungeon you have to run. Deriving it beats
    // listing the chains here - the world data is the authority, and a realm that
    // edits its access rows gets the matching behaviour for free.
    QueryResult result = WorldDatabase.Query(
        "SELECT dat.map_id, dar.requirement_type, dar.requirement_id, dar.faction, "
        "       COALESCE((SELECT MIN(p.MapID) FROM quest_poi p "
        "                  WHERE p.QuestID = dar.requirement_id), 0) "
        "FROM dungeon_access_requirements dar "
        "JOIN dungeon_access_template dat ON dat.id = dar.dungeon_access_id "
        "WHERE dat.difficulty = 0");
    if (!result)
        return;

    do
    {
        Field* f = result->Fetch();
        uint32 const gatedMap = f[0].Get<uint32>();
        uint32 const type = f[1].Get<uint8>();
        uint32 const requirement = f[2].Get<uint32>();
        uint32 const faction = f[3].Get<uint8>();
        uint32 const earnedOn = f[4].Get<uint32>();

        // 0 is an achievement and 2 is an item; neither has a run behind it that a
        // party could be pointed at, so the destination is simply never offered.
        if (type != 1)
        {
            _unearnable.insert(gatedMap);
            continue;
        }

        // The quest has to be finished inside an instance this object knows a door
        // to. Chains that finish out in the world - the Caverns of Time attunements
        // run through Tanaris, not through a dungeon - have no run to redirect to.
        if (!earnedOn || earnedOn == gatedMap || !_entrances.count(earnedOn))
        {
            _unearnable.insert(gatedMap);
            continue;
        }

        // Faction 2 on the row means the door asks the same of both sides.
        for (uint32 team = 0; team < 2; ++team)
        {
            if (faction < 2 && faction != team)
                continue;
            _questGates[gatedMap][team] = QuestGate{requirement, earnedOn};
            _teaches[earnedOn][team] = requirement;
        }
        // A door that also has an earnable route is not unearnable after all; the
        // rows arrive in no particular order, so this is settled per row.
        _unearnable.erase(gatedMap);
    } while (result->NextRow());

    for (uint32 const mapId : _unearnable)
        LOG_INFO("playerbots",
                 "Party assembler: map {} asks for something no party can go and earn "
                 "- it will not be chosen",
                 mapId);

    LOG_INFO("playerbots", "Party assembler: {} attunement gates, {} dungeons teach one",
             uint32(_questGates.size()), uint32(_teaches.size()));
}

uint32 PartyAssembler::ResolveGate(Group* group, Player const* leader, uint32 wantMap,
                                   uint32& earnQuest) const
{
    earnQuest = 0;
    uint32 const team = leader->GetTeamId() == TEAM_HORDE ? 1u : 0u;
    uint32 map = wantMap;

    // The access check runs member by member, so one member short of the quest
    // splits the party on the doorstep rather than turning the whole group back.
    // The run they need is therefore the one the least-progressed of them needs.
    auto const everyoneHolds = [group](uint32 questId)
    {
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && !member->GetQuestRewardStatus(questId))
                    return false;
        return true;
    };

    // For a door with no run behind it - a heroic key, an achievement, a chain that
    // finishes out in the world - there is nothing to redirect to, so the question is
    // simply whether they may walk in. Asked of the core's own check rather than
    // assumed: a party that does happen to satisfy it should not be turned away here.
    auto const everyoneSatisfies = [group](uint32 mapId)
    {
        DungeonProgressionRequirements const* ar =
            sObjectMgr->GetAccessRequirement(mapId, DUNGEON_DIFFICULTY_NORMAL);
        if (!ar)
            return true;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            if (Player* member = ref->GetSource())
                if (member->IsInWorld() && !member->Satisfy(ar, mapId))
                    return false;
        return true;
    };

    // Follow the chain back towards the shallow end. Bounded rather than looped
    // until it settles: a cycle in the access rows would otherwise spin here.
    for (uint32 hop = 0; hop < 4; ++hop)
    {
        auto const gate = _questGates.find(map);
        if (gate == _questGates.end())
        {
            if (_unearnable.count(map) && !everyoneSatisfies(map))
                return 0;
            break;
        }
        uint32 const needs = gate->second[team].quest;
        if (!needs || everyoneHolds(needs))
            break;   // the door is already open to all of them
        uint32 const earnedOn = gate->second[team].earnedOn;
        if (!earnedOn || earnedOn == map)
            return 0;
        map = earnedOn;
    }

    // Whatever they ended up running, they carry the quest it teaches if the door it
    // opens is still shut to them. That is what makes an ordinary Forge of Souls run
    // also the first step towards the Halls, without anybody planning it.
    if (auto const teaches = _teaches.find(map); teaches != _teaches.end())
    {
        uint32 const quest = teaches->second[team];
        if (quest && !everyoneHolds(quest))
            earnQuest = quest;
    }
    return map;
}

bool PartyAssembler::DungeonInfo(Player const* leader, uint32 mapId, Entrance& where,
                                 std::string& name) const
{
    auto const door = _entrances.find(mapId);
    if (door == _entrances.end())
        return false;
    // Same continent rule as the ordinary pick: the party walks to its door, and
    // nothing walks across an ocean.
    if (door->second.map != leader->GetMapId())
        return false;

    where = door->second;
    name = "a dungeon";
    for (uint32 i = 0; i < sLFGDungeonStore.GetNumRows(); ++i)
    {
        LFGDungeonEntry const* dungeon = sLFGDungeonStore.LookupEntry(i);
        if (!dungeon || dungeon->TypeID != LFG_TYPE_DUNGEON || dungeon->MapID != mapId)
            continue;
        if (dungeon->MinLevel && leader->GetLevel() < dungeon->MinLevel)
            return false;
        if (dungeon->Name[0])
            name = dungeon->Name[0];
        return true;
    }
    return false;
}

uint32 PartyAssembler::OfferGateQuest(Group* group, uint32 questId) const
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return 0;

    uint32 offered = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        // A human picks up their own quests; putting one in their log uninvited is
        // not this object's business.
        if (!member || !member->IsInWorld() || !GET_PLAYERBOT_AI(member))
            continue;
        // Already carrying it, already finished it, or no room in the log for it.
        if (member->GetQuestStatus(questId) != QUEST_STATUS_NONE)
            continue;
        if (member->GetQuestRewardStatus(questId))
            continue;
        if (!member->CanAddQuest(quest, false))
            continue;

        // No questgiver, deliberately: the NPC who hands this out stands in a hub
        // the party will never walk through, and the prerequisite step before it is
        // an outdoor chain no bot runs. AddQuest reads the argument for one thing
        // only - copying a timer from another player on a shared timed quest - so a
        // null one is exactly right here.
        member->AddQuest(quest, nullptr);
        ++offered;
    }
    return offered;
}

uint32 PartyAssembler::SettleGateQuest(Group* group, uint32 questId) const
{
    Quest const* quest = sObjectMgr->GetQuestTemplate(questId);
    if (!quest)
        return 0;

    uint32 settled = 0;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* member = ref->GetSource();
        if (!member || !member->IsInWorld() || !GET_PLAYERBOT_AI(member))
            continue;
        if (member->GetQuestStatus(questId) != QUEST_STATUS_COMPLETE)
            continue;
        if (!member->CanRewardQuest(quest, false))
            continue;

        // The bot stands in for the questgiver. Not a null one: the reward-mail and
        // reward-spell branches dereference that argument without checking. Handing
        // it the player is what the core itself does when it rewards a tracking
        // quest, and a player guid never resolves to a creature, so the branch that
        // would have an NPC cast the reward spell simply does not fire.
        member->RewardQuest(quest, 0, member, false);
        ++settled;
    }
    return settled;
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

void PartyAssembler::RecoverInside(Group* group, Player* leader, Trip& trip)
{
    std::unordered_set<uint32> now;
    uint32 alive = 0, present = 0, fighting = 0;
    for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
        if (Player* m = mi->GetSource())
        {
            now.insert(m->GetGUID().GetCounter());
            if (!m->IsInWorld())
                continue;
            ++present;
            if (m->IsAlive())
                ++alive;
            if (m->IsInCombat())
                ++fighting;
        }

    // Anyone who was here last tick and is not here now. Recorded against this run's
    // own id, which is the only identifier that survives group ids being reused.
    // The roster only ever holds online members, so a member still in the group has
    // logged out rather than left - a different departure worth telling apart.
    for (uint32 const gone : trip.roster)
        if (now.find(gone) == now.end())
        {
            ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(gone);
            Player* who = ObjectAccessor::FindConnectedPlayer(guid);
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_member_loss (at, run_id, group_id, guid, name,"
                " site, suppressed, map) VALUES (UNIX_TIMESTAMP(), {}, {}, {}, '{}',"
                " '{}', 0, {})",
                trip.runId, group->GetGUID().GetCounter(), gone,
                who ? Sql(who->GetName()) : std::string("?"),
                group->IsMember(guid) ? "roster_offline" : "roster_left",
                who ? who->GetMapId() : 0);
        }
    trip.roster = std::move(now);

    // Members that ended up off the dungeon map. Nothing else brings them back: the
    // follow strategy cannot cross a map boundary, so a member teleported out - by a
    // graveyard release, or by anything else that moves a bot for its own reasons -
    // was simply gone for the rest of the run while still counting as a member.
    auto const inside = _insides.find(trip.dungeonMap);
    if (inside != _insides.end() && leader->GetMapId() == trip.dungeonMap)
    {
        uint32 recalled = 0;
        for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
        {
            Player* m = mi->GetSource();
            if (!m || m == leader || !m->IsInWorld() || m->IsBeingTeleported())
                continue;
            if (m->GetMapId() == trip.dungeonMap)
                continue;
            m->TeleportTo(trip.dungeonMap, inside->second.x, inside->second.y,
                          inside->second.z, 0.0f);
            ++recalled;
        }
        if (recalled)
            LOG_INFO("playerbots", "Party assembler: {} rejoin the party in {}",
                     recalled, trip.name);
    }

    // A death that is not a wipe was nobody's job. The wipe watch only fires when the
    // whole party is down, so a member killed by trash stayed a corpse for the rest of
    // the run - and the random-bot manager's own revive cycle, which ungroups and
    // teleports away, is what eventually collected them. Left to the wipe watch when
    // nobody is standing: that path counts the wipe and reseats them at the door.
    if (!alive || alive == present || fighting)
        return;

    uint32 raised = 0;
    for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
    {
        Player* m = mi->GetSource();
        if (!m || !m->IsInWorld() || m->IsAlive())
            continue;
        m->ResurrectPlayer(0.5f);
        m->SpawnCorpseBones();
        if (PlayerbotAI* mAI = GET_PLAYERBOT_AI(m))
        {
            mAI->ResetStrategies(false);
            // ResetStrategies restores the free-bot defaults, which is exactly the
            // moment a revived member queues for a battleground and walks out.
            mAI->ChangeStrategy(m == leader ? "-lfg,-bg" : "+follow,-lfg,-bg",
                                BOT_STATE_NON_COMBAT);
        }
        ++raised;
    }
    if (raised)
        LOG_INFO("playerbots", "Party assembler: {} back on their feet in {}",
                 raised, trip.name);
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
        // Humans run dungeons at human pace: an adopted run gets six times the
        // dwell clock a bot-only sweep needs.
        // A raid is a whole evening rather than a wing sweep, so one uniform dwell
        // clock shows it the door several bosses short of the end - which is what
        // 148 runs ending at an average of 0.4 bosses was measuring. The two
        // multipliers never stack: whichever venue is the more patient one wins.
        uint32 mult = 1;
        if (trip.phase == Phase::Inside)
        {
            if (group && group->isRaidGroup())
                mult = _insideTicksRaidMult;
            // A long venue is long for a five-man too, so the two are compared rather
            // than combined - a raid in a long venue is still one evening, not two.
            mult = std::max(mult, VenueClockMult(trip.dungeonMap));
            // And the ground itself has a vote: a wing whose bosses stand hundreds of
            // yards past the door needs that walk paid for before any of it counts.
            mult = std::max(mult, TravelMultFor(trip.dungeonMap));
        }
        if (trip.adopted)
            mult = std::max<uint32>(mult, 6);
        uint32 const budget =
            (trip.phase == Phase::Inside ? _insideTicks : _maxTripTicks) * mult;
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
            // Never for an adopted run: the group belongs to the player, so the
            // clock only stops the steering and closes the ledger row.
            if (group && trip.adopted)
                ReleaseAdopted(group);
            if (group && trip.phase == Phase::Inside && !trip.adopted)
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
            if (trip.adopted)
                ReleaseAdopted(group);
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
            // The ledger's entry stamp, written once the leader is actually standing
            // in the place. It used to be written the moment the teleports were
            // issued, which is not the same thing: the core turns a party away at the
            // door when a member fails an access requirement, and the run was still
            // recorded as having entered. That is how Halls of Reflection - which no
            // character on this realm can enter at all - read as a wing that got in
            // ten times and killed nothing. A run that never arrives now ends with
            // entered_at still zero, which says exactly what happened.
            if (!trip.arrived && leader->GetMapId() == trip.dungeonMap)
            {
                trip.arrived = true;
                ++_statEntered;
                if (trip.runId)
                    CharacterDatabase.Execute(
                        "UPDATE aetherion_run_history SET entered_at = UNIX_TIMESTAMP()"
                        " WHERE id = {}", trip.runId);
            }

            // Party discipline, re-asserted every tick. Death recovery calls
            // ResetStrategies, which restores the free-bot default set - battleground
            // queue, dungeon finder, wander - and the single strip done at formation
            // is long gone by the time anyone dies. Revived members drifted off into
            // battleground queues, the group sagged below two members, and the core
            // disbanded it out from under the run: 67 of 234 recorded runs ended that
            // way, at an average of 0.09 bosses. Re-stripping heals whatever polluted
            // them, whether or not this object knows the source. Asked first rather
            // than applied blindly: adding a strategy rebuilds the engine's whole
            // trigger list, which is worth paying for the handful of members that
            // actually drifted and not for the several hundred that did not.
            for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
            {
                Player* m = mi->GetSource();
                if (!m)
                    continue;
                PlayerbotAI* mAI = GET_PLAYERBOT_AI(m);
                if (!mAI)
                    continue;

                // The queues come off everyone. A leader that walks into a
                // battleground ends the run exactly as surely as a member that does.
                if (mAI->HasStrategy("lfg", BOT_STATE_NON_COMBAT) ||
                    mAI->HasStrategy("bg", BOT_STATE_NON_COMBAT))
                    mAI->ChangeStrategy("-lfg,-bg", BOT_STATE_NON_COMBAT);

                // Following is for the party only. The leader is steered by
                // destination rather than by a master, and a follow strategy would
                // have it chase a master it does not have instead of walking to the
                // boss the steering below just pointed it at.
                if (m != leader && !mAI->HasStrategy("follow", BOT_STATE_NON_COMBAT))
                    mAI->ChangeStrategy("+follow", BOT_STATE_NON_COMBAT);
            }

            // Deaths that are not wipes, and members that ended up somewhere else.
            // Ordered before the wipe watch so its "nobody is alive" test sees the
            // party this tick actually left the run with.
            RecoverInside(group, leader, trip);

            // Wipe watch. A raid that has fully fallen does what a determined
            // guild does: steadies itself and pulls again - resurrected at the
            // instance door with a fresh clock - until repeated wipes break
            // its spirit and the run ends as 'wiped', not a silent timeout.
            // Without this, dead bots released to a graveyard OUTSIDE the
            // instance and the run burned out with nobody home.
            uint32 alive = 0, present = 0;
            bool humanAboard = false;
            for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
                if (Player* m = mi->GetSource())
                    if (m->IsInWorld())
                    {
                        ++present;
                        if (m->IsAlive())
                            ++alive;
                        if (!GET_PLAYERBOT_AI(m))
                            humanAboard = true;
                    }
            if (trip.adopted && !humanAboard)
            {
                trip.adopted = false;
                LOG_INFO("playerbots",
                         "Party assembler: the player left {} - the run is the bots' now",
                         trip.name);
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
                    if (trip.adopted)
                        ReleaseAdopted(group);
                    if (!trip.adopted)
                    {
                        SendGroupOutside(group, trip.dungeonMap);
                        group->Disband();
                    }
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
                            {
                                mAI->ResetStrategies(false);
                                // Re-strip on the spot rather than leaving the freshly
                                // restored defaults a whole tick to act on: this is the
                                // exact moment a revived member would queue for a
                                // battleground and walk out of the instance. Same rule
                                // as the discipline pass above - the leader keeps its
                                // destination steering instead of a master to follow.
                                mAI->ChangeStrategy(m == leader ? "-lfg,-bg"
                                                                : "+follow,-lfg,-bg",
                                                    BOT_STATE_NON_COMBAT);
                            }
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

            // Attunement is taken the moment it is finished, not at the end of the
            // run: the quest completes as the last boss falls, and a party that
            // times out twenty minutes later still walks away with the next door
            // open. Server-side because the NPC who takes it stands inside this
            // instance and the module's turn-in only fires within talking distance.
            if (trip.earnQuest)
                if (uint32 const earned = SettleGateQuest(group, trip.earnQuest))
                {
                    Quest const* gate = sObjectMgr->GetQuestTemplate(trip.earnQuest);
                    LOG_INFO("playerbots",
                             "Party assembler: {} of {}'s party earned {} in {}",
                             earned, leader->GetName(),
                             gate ? gate->GetTitle() : std::to_string(trip.earnQuest),
                             trip.name);
                }

            // Two venues hold their content behind an opening request. Attempted over
            // several ticks rather than once: the party may still be landing on the
            // first, and an attempt the instance refuses costs nothing.
            if (!trip.started && trip.nudges < kVenueNudges &&
                (trip.dungeonMap == kMapVioletHold ||
                 trip.dungeonMap == kMapHallsOfReflection))
            {
                ++trip.nudges;
                if (StartVenueEvent(leader, trip))
                {
                    trip.started = true;
                    LOG_INFO("playerbots",
                             "Party assembler: {} asks for the event to start in {}",
                             leader->GetName(), trip.name);
                }
            }

            // Bosses first; trash where no boss position is known, and again once
            // every boss has had its visit. Held as pointers rather than iterators:
            // the two maps are separate containers and comparing an iterator from one
            // against the other's end() is undefined.
            std::vector<Entrance> const* bossSpots = nullptr;
            if (auto const bossIt = _bosses.find(trip.dungeonMap);
                bossIt != _bosses.end() && !bossIt->second.empty())
                bossSpots = &bossIt->second;
            std::vector<Entrance> const* trashSpots = nullptr;
            if (auto const trashIt = _spawns.find(trip.dungeonMap);
                trashIt != _spawns.end() && !trashIt->second.empty())
                trashSpots = &trashIt->second;

            if ((bossSpots || trashSpots) && leader->GetMapId() == trip.dungeonMap)
            {
                // A boss the party has stood on is finished with, for good. The list
                // is spawn data loaded once at startup, so a dead boss still occupies
                // its spot: a leader that steps eight yards off it is nearest to it
                // again, walks back, arrives, is sent at the next boss, drifts far
                // enough for the corpse to win again, and spends the rest of the
                // clock oscillating between the two. That is what "kills one boss and
                // then nothing" was. Every Obsidian Sanctum kill this realm has ever
                // logged is Vesperon, the drake nearest the arrival point, 11 for 11 -
                // never Sartharion 8 yards further out, never a second encounter
                // anywhere, and no run in 573 has ever downed three.
                if (bossSpots)
                    for (uint32 i = 0; i < bossSpots->size(); ++i)
                    {
                        Entrance const& spot = (*bossSpots)[i];
                        float const dx = leader->GetPositionX() - spot.x;
                        float const dy = leader->GetPositionY() - spot.y;
                        if (dx * dx + dy * dy >= _huntRange * _huntRange)
                            continue;
                        // Once per boss per run, which is also the only record of how
                        // deep a party actually got: the ledger's boss count is read
                        // from an instance row that may already have been pruned.
                        if (trip.visitedBosses.insert(i).second)
                            LOG_INFO("playerbots",
                                     "Party assembler: {}'s party reaches boss {} of "
                                     "{} in {}",
                                     leader->GetName(), uint32(trip.visitedBosses.size()),
                                     uint32(bossSpots->size()), trip.name);
                    }

                // Head for the nearest boss nobody has been to yet. Members follow the
                // leader, so steering one character walks the whole party through the
                // dungeon.
                Entrance const* best = nullptr;
                float bestDist = 0.0f;
                uint32 bestBoss = kNoBossAim;
                if (bossSpots)
                    for (uint32 i = 0; i < bossSpots->size(); ++i)
                    {
                        if (trip.visitedBosses.count(i))
                            continue;
                        Entrance const& spot = (*bossSpots)[i];
                        float const dx = leader->GetPositionX() - spot.x;
                        float const dy = leader->GetPositionY() - spot.y;
                        float const dist = std::sqrt(dx * dx + dy * dy);
                        if (!best || dist < bestDist)
                        {
                            best = &spot;
                            bestDist = dist;
                            bestBoss = i;
                        }
                    }

                // Nothing left worth a name: hunt trash rather than stand still for
                // the rest of the dwell clock.
                if (!best && trashSpots)
                    for (Entrance const& spot : *trashSpots)
                    {
                        float const dx = leader->GetPositionX() - spot.x;
                        float const dy = leader->GetPositionY() - spot.y;
                        float const dist = std::sqrt(dx * dx + dy * dy);
                        // Only skip a target we are practically standing on. Skipping
                        // anything within a generous radius sent a party that had
                        // closed to 20 yards off towards a different pack, so it
                        // oscillated between them and committed to neither.
                        if (dist < _huntRange)
                            continue;
                        if (!best || dist < bestDist)
                        {
                            best = &spot;
                            bestDist = dist;
                        }
                    }

                // Nobody is re-tasked mid-fight. The mover has no combat guard of its
                // own beyond the one this patch chain adds, and a leader sent to the
                // next pack while the current one is still swinging walks the whole
                // party out of the fight - members follow the leader, the pack resets,
                // and the run nets a couple of dozen yards per tick against a wing
                // five hundred yards long. Held rather than cancelled: the
                // destination stands, so the walk resumes the moment the fight ends.
                bool fighting = leader->IsInCombat();
                for (GroupReference* mi = group->GetFirstMember(); !fighting && mi != nullptr;
                     mi = mi->next())
                    if (Player* m = mi->GetSource())
                        if (m->IsInWorld() && m->IsInCombat())
                            fighting = true;

                if (fighting && trip.combatTicks < kCombatHoldTicks)
                {
                    ++trip.combatTicks;
                }
                else if (best)
                {
                    // A fight that never ends - one member kiting, or a pack that
                    // cannot be reached - would otherwise hold the party still until
                    // the clock runs out, so past the hold the steering resumes.
                    trip.combatTicks = 0;

                    // A boss the party cannot get any closer to is not a boss to keep
                    // walking at. Naxxramas keeps its wings behind teleporters no
                    // mover paths through, so a leader aimed at the nearest one simply
                    // stands in the entry hub: 38 recorded runs there spent over an
                    // hour inside apiece without a single kill or even a wipe. Give up
                    // on that one and try the next. Only counted out of combat - a
                    // party holding still because it is fighting is not stuck.
                    if (bestBoss != kNoBossAim && !fighting)
                    {
                        // Closing on it resets the count, and so does being much
                        // further from it than the last reading: a wipe reseats the
                        // whole party at the arrival point, which is not the party
                        // failing to walk - measuring that as failure would retire a
                        // boss they had merely died on the way to.
                        if (bestBoss != trip.aimBoss ||
                            bestDist < trip.aimDist - kAimProgressYards ||
                            bestDist > trip.aimDist + kAimResetYards)
                        {
                            trip.aimBoss = bestBoss;
                            trip.aimDist = bestDist;
                            trip.aimStalls = 0;
                        }
                        else if (++trip.aimStalls > kAimStallTicks)
                        {
                            trip.visitedBosses.insert(bestBoss);
                            trip.aimBoss = kNoBossAim;
                            trip.aimStalls = 0;
                            LOG_INFO("playerbots",
                                     "Party assembler: {}'s party cannot reach a boss "
                                     "in {} ({} yards and no closer) - moving on",
                                     leader->GetName(), trip.name, uint32(bestDist));
                            ++it;
                            continue;
                        }
                    }

                    // Only when it actually changed. Re-issuing the same destination
                    // restarts the mover's no-progress clock, which is what decides
                    // whether it should fall back to a teleport.
                    if (std::fabs(best->x - trip.aimX) > 1.0f ||
                        std::fabs(best->y - trip.aimY) > 1.0f ||
                        GET_PLAYERBOT_AI(leader)->rpgInfo.GetStatus() != RPG_GO_GRIND)
                    {
                        trip.aimX = best->x;
                        trip.aimY = best->y;
                        GET_PLAYERBOT_AI(leader)->rpgInfo.ChangeToGoGrind(
                            WorldPosition(trip.dungeonMap, best->x, best->y, best->z));
                    }
                }
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
                // Entry is recorded on arrival, not here: EnterInstance reports how
                // many teleports it issued, not how many landed.
                trip.phase = Phase::Inside;
                trip.ticks = 0;
            }
        }

        ++it;
    }
}

// Two Northrend instances keep every one of their bosses behind a conversation.
// Violet Hold's assault begins only when someone tells Lieutenant Sinclari to get
// his people to safety; the Halls of Reflection waves begin only after the Jaina or
// Sylvanas intro. Nobody holds those conversations, which is why 27 recorded Violet
// Hold runs and 10 Halls runs killed nothing at all between them.
//
// The conversation itself is out of reach. Halls offers its gossip options only to a
// player who has finished the Battered Hilt quest chain - no character on this realm
// has, bot or otherwise - so there is no option to select, however the request is
// delivered. So the trip asks the instance script for the same thing the gossip
// handler would ask it for, one step further down.
//
// Safe from here: MapMgr::Update joins its worker threads before the world script
// hook that drives this runs, so no map is mid-update while the call lands - the same
// reason the teleports and resurrections above are safe on this thread.
bool PartyAssembler::StartVenueEvent(Player* leader, Trip const& trip)
{
    // Only meaningful once the leader is actually standing in the place.
    if (leader->GetMapId() != trip.dungeonMap)
        return false;

    InstanceScript* instance = leader->GetInstanceScript();
    if (!instance)
        return false;

    if (trip.dungeonMap == kMapVioletHold)
    {
        // The instance refuses the action unless the assault has not begun, so this
        // check only keeps the log honest about what was asked and answered.
        if (instance->GetData(kVhEncounterStatus) != NOT_STARTED)
            return false;
        instance->DoAction(kVhStartInstance);
        return true;
    }

    if (trip.dungeonMap == kMapHallsOfReflection)
    {
        // Setting the intro done a second time would advance the wave counter again
        // and skip a wave, so a hall already under way is left alone.
        if (instance->GetData(kHorWaveNumber) != 0)
            return false;
        // Both halves, in the order the intro performs them: the spirits standing in
        // each wave are drawn first, then the first wave is released. Releasing
        // without drawing leaves the hall empty and the run stuck.
        instance->SetData(kHorShowTrash, 1);
        instance->SetData(kHorIntro, DONE);
        return true;
    }

    return false;
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
    Option chosen =
        options[urand(0, std::min<size_t>(options.size(), _nearestChoices) - 1)];

    if (!GET_PLAYERBOT_AI(leader))
        return false;

    // Attunement. A door that asks for a quest nobody in the party holds is not a
    // destination to quietly drop - it is a destination with a run in front of it.
    // Walk the chain back to the deepest step they can actually enter, go there
    // instead, and carry the quest that opens the next door. Over successive runs a
    // party works its way forward: Forge of Souls, then the Pit, then the Halls.
    uint32 earnQuest = 0;
    uint32 const runMap = ResolveGate(group, leader, chosen.dungeonMap, earnQuest);
    if (!runMap)
        return false;   // nothing in this chain is open to them; another pick next tick

    if (runMap != chosen.dungeonMap)
    {
        Entrance where;
        std::string name;
        if (!DungeonInfo(leader, runMap, where, name))
            return false;

        Quest const* gate = sObjectMgr->GetQuestTemplate(earnQuest);
        LOG_INFO("playerbots",
                 "Party assembler: {} lack {} for {} - running {} to earn it",
                 leader->GetName(),
                 gate ? gate->GetTitle() : std::to_string(earnQuest),
                 chosen.name, name);

        chosen = Option{where, name, runMap};
    }

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

    // The quest goes in the log before they set off, so every boss they kill on the
    // way through counts. Nothing else about the run changes: it is an ordinary trip
    // that happens to open a door on its way past.
    if (earnQuest)
    {
        trip.earnQuest = earnQuest;
        uint32 const carrying = OfferGateQuest(group, earnQuest);
        Quest const* gate = sObjectMgr->GetQuestTemplate(earnQuest);
        if (carrying)
            LOG_INFO("playerbots",
                     "Party assembler: {} of {}'s party carry {} into {}",
                     carrying, leader->GetName(),
                     gate ? gate->GetTitle() : std::to_string(earnQuest), chosen.name);
        if (trip.runId && gate)
            CharacterDatabase.Execute(
                "UPDATE aetherion_run_history SET attunement = '{}' WHERE id = {}",
                Sql(gate->GetTitle()), trip.runId);
    }

    ++_statTrips;
    return true;
}

void PartyAssembler::ReleaseAdopted(Group* group)
{
    if (!group)
        return;

    // The ownership shield goes with the steering. The group belongs to the player,
    // so an entry left behind would keep the module's ordinary behaviour off it for
    // the rest of the uptime. The cross-thread mirror is rebuilt from this set at the
    // end of the tick, exactly as it is for every other change to it.
    _assembled.erase(group->GetGUID().GetCounter());

    // Hand the bots back to the human: master restored, strategies rebuilt,
    // following the player again. With no human left they simply go free.
    Player* human = nullptr;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* m = ref->GetSource())
            if (m->IsInWorld() && !GET_PLAYERBOT_AI(m))
            {
                human = m;
                break;
            }

    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* m = ref->GetSource())
            if (PlayerbotAI* mAI = GET_PLAYERBOT_AI(m))
            {
                mAI->SetMaster(human);
                mAI->ResetStrategies(false);
                mAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
            }
}

bool PartyAssembler::AdoptRun(Player* requester, Player* claimedBot)
{
    if (!requester || !requester->IsInWorld())
        return false;

    Group* group = requester->GetGroup();
    if (!group)
        return false;

    // Already being driven - the ask is satisfied.
    if (_trips.count(group->GetGUID().GetCounter()))
        return true;

    Map* map = requester->GetMap();
    uint32 const mapId = requester->GetMapId();
    if (!map || !map->IsDungeon())
        return false;

    // Steering needs somewhere to steer to: seeded bosses, or trash spawns as
    // the fallback the Inside branch already uses.
    auto const bossIt = _bosses.find(mapId);
    auto const trashIt = _spawns.find(mapId);
    if ((bossIt == _bosses.end() || bossIt->second.empty()) &&
        (trashIt == _spawns.end() || trashIt->second.empty()))
        return false;

    // A bot must hold the crown to be steered. When the human leads, the
    // claimed bot - or failing that any bot member standing in the instance -
    // is promoted; the player taking the crown back later is the natural
    // off-switch (the leader_lost branch quietly closes the run).
    Player* leader = ObjectAccessor::FindConnectedPlayer(group->GetLeaderGUID());
    if (!leader)
        return false;
    if (!GET_PLAYERBOT_AI(leader))
    {
        Player* promote =
            claimedBot && claimedBot->GetGroup() == group && claimedBot->GetMapId() == mapId
                ? claimedBot : nullptr;
        if (!promote)
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
                if (Player* m = ref->GetSource())
                    if (GET_PLAYERBOT_AI(m) && m->IsInWorld() && m->GetMapId() == mapId)
                    {
                        promote = m;
                        break;
                    }
        if (!promote)
            return false;
        group->ChangeLeader(promote->GetGUID());
        leader = promote;
    }

    // The point of adoption: the party is the LEADER's to run. Every bot
    // member re-masters to the leader - they follow it and open fire when it
    // pulls, not when the human does - the leader goes masterless so the
    // steering can drive it (a mastered bot's follow-master strategy overrides
    // travel), and an arrow formation keeps the caravan spread by role instead
    // of clumped on one spot.
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
    {
        Player* m = ref->GetSource();
        if (!m || !m->IsInWorld())
            continue;
        PlayerbotAI* mAI = GET_PLAYERBOT_AI(m);
        if (!mAI)
            continue;
        mAI->SetMaster(m == leader ? nullptr : leader);
        mAI->ResetStrategies(false);
        mAI->ChangeStrategy("+follow,-lfg,-bg", BOT_STATE_NON_COMBAT);
        if (FormationValue* fv = (FormationValue*)mAI->GetAiObjectContext()
                                     ->GetValue<Formation*>("formation"))
            fv->Load("arrow");
    }

    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{mapId, Entrance{}, map->GetMapName(), Phase::Inside, Travel::Foot,
             map->GetMapName(), leader->GetName(), 0};
    trip.adopted = true;
    trip.runId = RecordRunStart(group, leader, trip.name, mapId, group->isRaidGroup(),
                                uint8(requester->GetDifficulty(map->IsRaid())),
                                Travel::Foot, 0);

    // Register the adopted group as owned, the same way a formed one is. Without it
    // the module's teardown paths - which stand down only for groups Owns() answers
    // for - dismantle the run from underneath: every bot member's "leave the group
    // when a random bot leads it" check fires the moment the crown is passed.
    // AssembleOne can leave the mirror to the end of the tick it runs inside;
    // adoption arrives from the intent drain instead, so it publishes its own.
    _assembled.insert(group->GetGUID().GetCounter());
    SyncOwnedMirror();

    ++_statTrips;
    LOG_INFO("playerbots",
             "Party assembler: {} takes point in {} - adopted run for {} (run {})",
             leader->GetName(), trip.name, requester->GetName(), trip.runId);
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
        " attunement VARCHAR(48) NOT NULL DEFAULT '',"
        " flavor VARCHAR(16) NOT NULL DEFAULT '',"
        " KEY idx_started (started_at), KEY idx_map (map)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // The history table is durable, so schema growth has to migrate in place -
    // MySQL 8 has no ADD COLUMN IF NOT EXISTS, hence the probe.
    if (!CharacterDatabase.Query("SHOW COLUMNS FROM aetherion_run_history LIKE 'wipes'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN wipes TINYINT UNSIGNED NOT NULL DEFAULT 0");

    // Which attunement a run was for, so the history reads as the chain it is rather
    // than as a party inexplicably running the Forge of Souls for the fifth time.
    if (!CharacterDatabase.Query(
            "SHOW COLUMNS FROM aetherion_run_history LIKE 'attunement'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN attunement VARCHAR(48) NOT NULL DEFAULT ''");

    // What kind of run this was. Empty means ordinary progression; 'collector' is a
    // trip back into content the realm has outgrown.
    if (!CharacterDatabase.Query("SHOW COLUMNS FROM aetherion_run_history LIKE 'flavor'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN flavor VARCHAR(16) NOT NULL DEFAULT ''");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_run_members ("
        " run_id INT UNSIGNED NOT NULL, guid INT UNSIGNED NOT NULL,"
        " name VARCHAR(24) NOT NULL, class TINYINT UNSIGNED NOT NULL,"
        " level TINYINT UNSIGNED NOT NULL, role VARCHAR(8) NOT NULL,"
        " PRIMARY KEY (run_id, guid)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Diagnostic. Party attrition was being chased by matching departed bots against
    // stored rosters, which cannot work: group ids are reused across restarts, so a
    // removal matches a stale underway row as readily as the live one. This says who
    // left which run, from which call site, at the moment it happened.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_member_loss ("
        " id INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " at INT UNSIGNED NOT NULL, run_id INT UNSIGNED NOT NULL,"
        " group_id INT UNSIGNED NOT NULL, guid INT UNSIGNED NOT NULL,"
        " name VARCHAR(24) NOT NULL, site VARCHAR(24) NOT NULL,"
        " suppressed TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " map INT UNSIGNED NOT NULL DEFAULT 0, KEY at (at), KEY run_id (run_id)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
    CharacterDatabase.Execute(
        "DELETE FROM aetherion_member_loss WHERE at < UNIX_TIMESTAMP() - 3*86400");

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
        " updated_at INT UNSIGNED NOT NULL,"
        " last_muster_at INT UNSIGNED NOT NULL DEFAULT 0"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // The muster clock counted ticks of continuous uptime, so every restart put it
    // back to zero and a deploy cadence shorter than MusterEveryMin meant it never
    // finished. Kept on disk instead, and seeded from the wall time since the last
    // muster, so a restart costs the realm nothing.
    if (!CharacterDatabase.Query(
            "SHOW COLUMNS FROM aetherion_assembler LIKE 'last_muster_at'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_assembler"
            " ADD COLUMN last_muster_at INT UNSIGNED NOT NULL DEFAULT 0");

    if (QueryResult since = CharacterDatabase.Query(
            "SELECT GREATEST(0, UNIX_TIMESTAMP() - last_muster_at) FROM aetherion_assembler"
            " WHERE id = 1 AND last_muster_at > 0"))
    {
        uint32 const secondsPerTick = std::max<uint32>(1, _intervalMs / 1000);
        uint32 const elapsed = (*since)[0].Get<uint32>() / secondsPerTick;
        uint32 const ticksPerMin = std::max<uint32>(1, 60000u / std::max<uint32>(1, _intervalMs));
        // Never more than a full cooldown: a realm that has been down for a week
        // should start its first muster promptly, not owe itself a hundred of them.
        _musterCooldownTicks = std::min(elapsed, _musterEveryMin * ticksPerMin);
        LOG_INFO("playerbots",
                 "Party assembler: muster clock resumes {} ticks in, of {}",
                 _musterCooldownTicks, _musterEveryMin * ticksPerMin);
    }
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
        // Upsert rather than REPLACE: REPLACE deletes the row first, which would take
        // the muster clock with it every tick.
        "INSERT INTO aetherion_assembler (id, formed, raids, trips, stalls, arrived, "
        "entered, active, entrances, arrival_points, raid_maps, updated_at) "
        "VALUES (1, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, UNIX_TIMESTAMP()) "
        "ON DUPLICATE KEY UPDATE formed=VALUES(formed), raids=VALUES(raids), "
        "trips=VALUES(trips), stalls=VALUES(stalls), arrived=VALUES(arrived), "
        "entered=VALUES(entered), active=VALUES(active), entrances=VALUES(entrances), "
        "arrival_points=VALUES(arrival_points), raid_maps=VALUES(raid_maps), "
        "updated_at=VALUES(updated_at)",
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

bool PartyAssembler::SendPartyToOldContent(Group* group, Player* leader)
{
    if (_entrances.empty())
        return false;

    // The door refuses members one at a time, so the whole party has to clear it.
    uint8 level = leader->GetLevel();
    for (GroupReference* itr = group->GetFirstMember(); itr != nullptr; itr = itr->next())
        if (Player* member = itr->GetSource())
            level = std::min<uint8>(level, member->GetLevel());
    if (level < kCollectorMinLevel)
        return false;

    uint32 const size = group->GetMembersCount();

    struct Option
    {
        Entrance where;
        std::string name;
        uint32 mapId;
        bool raid;
    };
    std::vector<Option> options;

    for (auto const& entry : _entrances)
    {
        uint32 const mapId = entry.first;
        Entrance const& door = entry.second;

        MapEntry const* map = sMapStore.LookupEntry(mapId);
        if (!map || !map->IsDungeon())
            continue;

        // What counts as old is decided by which expansion built the place, not by
        // a level gap. A Wrath dungeon whose floor happens to sit twenty levels down
        // is still this realm's current content, and Karazhan at floor sixty-eight
        // still is not.
        if (map->Expansion() >= EXPANSION_WRATH_OF_THE_LICH_KING)
            continue;

        // Deliberately not restricted to the leader's own continent, unlike every
        // other destination. Old content is almost all in the Eastern Kingdoms and
        // Kalimdor while a level-eighty realm lives in Northrend, so the same-continent
        // rule would silently rule out every candidate and the persona would never
        // once act. Crossing is already solved: BeginTravel hearths or portals the
        // party to a capital on the destination's continent whenever the door is on
        // another map, and only the last stretch is walked.

        // Being massively over-geared is the entire point, so gear never vetoes
        // here. The level floor and the access rows still do - an unattuned party
        // would simply be turned away at the portal.
        auto const floorIt = _mapMinLevel.find(mapId);
        if (floorIt != _mapMinLevel.end() && level < floorIt->second)
            continue;

        bool const raid = map->IsRaid();
        Difficulty const diff =
            raid ? Difficulty(RAID_DIFFICULTY_10MAN_NORMAL) : Difficulty(DUNGEON_DIFFICULTY_NORMAL);

        if (raid)
        {
            // Old raids carry a large player cap, which is what lets five modern
            // characters walk into a forty-man instance at all. A map whose cap is
            // genuinely smaller than this group would shed members at the portal.
            MapDifficulty const* md = GetMapDifficultyData(mapId, RAID_DIFFICULTY_10MAN_NORMAL);
            if (!md || (md->maxPlayers && md->maxPlayers < size))
                continue;
        }

        bool everyone = true;
        if (DungeonProgressionRequirements const* ar =
                sObjectMgr->GetAccessRequirement(mapId, diff))
            for (GroupReference* itr = group->GetFirstMember(); everyone && itr != nullptr;
                 itr = itr->next())
                if (Player* member = itr->GetSource())
                    everyone = member->Satisfy(ar, mapId);
        if (!everyone)
            continue;

        options.push_back({door, map->name[0] ? map->name[0] : "somewhere long forgotten",
                           mapId, raid});
    }

    if (options.empty())
        return false;

    // A free pick across the whole list rather than the nearest few. Distance does
    // not order these the way it orders a local dungeon run: the party hearths to
    // the destination's continent either way, so the yards between here and there
    // say nothing about which ruin is worth visiting, and picking widely is what
    // spreads collectors across old content instead of queueing them all at one
    // door.
    Option const& chosen = options[urand(0, options.size() - 1)];

    // A raid map will not admit a party group at all, whatever its level. Converting
    // costs nothing for a five-man and is what makes Molten Core reachable.
    if (chosen.raid && !group->isRaidGroup())
    {
        group->ConvertToRaid();
        group->SetRaidDifficulty(RAID_DIFFICULTY_10MAN_NORMAL);
    }
    else if (!chosen.raid)
    {
        group->SetDungeonDifficulty(DUNGEON_DIFFICULTY_NORMAL);
    }

    float const away = PlanarDistance(leader, chosen.where);
    Departure const start = BeginTravel(group, leader, chosen.where);

    LOG_INFO("playerbots",
             "Party assembler: {} takes a party of {} back to {} ({:.0f} yards, {}) - "
             "collecting what the realm left behind",
             leader->GetName(), size, chosen.name, away, TravelName(start.how));

    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{chosen.mapId, chosen.where, chosen.name, Phase::Travelling, start.how,
             start.place, start.actor, 0};
    trip.collector = true;
    trip.runId = RecordRunStart(group, leader, chosen.name, chosen.mapId, chosen.raid,
                                uint8(chosen.raid ? RAID_DIFFICULTY_10MAN_NORMAL
                                                  : DUNGEON_DIFFICULTY_NORMAL),
                                start.how, uint32(away));
    if (trip.runId)
        CharacterDatabase.Execute(
            "UPDATE aetherion_run_history SET flavor = 'collector' WHERE id = {}", trip.runId);

    ++_statTrips;
    return true;
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
    _refusedShape = false;

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

    // When the near bench simply has nobody who can hold threat or heal, those two
    // seats are filled from anywhere on the realm rather than left empty. The
    // same-map rule exists so a party reads as bots that met, but it is applied to a
    // pool already cut by faction and level spread, and on four continents that
    // routinely leaves a bench with no tank and no healer on it at all: measured
    // across 731 runs, only 36 percent formed the intended one tank, one healer and
    // three damage, while 28 percent set out with no healer or no tank whatsoever.
    // Distance costs nothing here - the summoning phase pulls every member to the
    // leader's meeting stone anyway, and formation gathers them before that.
    if (tanksNeeded || healersNeeded)
    {
        std::vector<Player*> wide;
        for (Player* bot : pool)
        {
            // Strictly the ones the near bench could not offer, so nobody can be
            // seated twice.
            if (bot == leader || bot->GetMapId() == leader->GetMapId())
                continue;
            if (bot->GetTeamId() != leader->GetTeamId())
                continue;
            uint32 const diff = bot->GetLevel() > leader->GetLevel()
                                    ? bot->GetLevel() - leader->GetLevel()
                                    : leader->GetLevel() - bot->GetLevel();
            if (diff > _levelSpread)
                continue;
            if (wantRaid && bot->GetLevel() < raidFloor)
                continue;
            // Somebody already inside an instance is in the middle of something,
            // whatever this object thinks it knows about their group.
            Map* where = bot->GetMap();
            if (!where || where->Instanceable())
                continue;
            wide.push_back(bot);
        }

        auto takeWide = [&picked, &wide, leader](auto predicate, char const* role) {
            auto it = std::find_if(wide.begin(), wide.end(), predicate);
            if (it == wide.end())
                return false;
            LOG_INFO("playerbots",
                     "Party assembler: {} seated as {} from the wider bench for {}",
                     (*it)->GetName(), role, leader->GetName());
            picked.push_back(*it);
            wide.erase(it);
            return true;
        };

        while (tanksNeeded &&
               takeWide([](Player* p) { return PlayerbotAI::IsTank(p); }, "tank"))
            --tanksNeeded;
        while (healersNeeded &&
               takeWide([](Player* p) { return PlayerbotAI::IsHeal(p); }, "healer"))
            --healersNeeded;
    }

    // A five-man with no healer is a wipe with extra steps. Refusing to form it and
    // re-rolling next tick costs nothing - the bots stay on the bench and another
    // leader will draw a better hand. The counter is the escape valve: a thin level
    // bracket that genuinely holds no healer would otherwise never form anything
    // again, so after a few refusals in a row the next party goes as it stands.
    uint32& refusals = _shortAborts[wantRaid ? 1 : 0];
    if (tanksNeeded || healersNeeded)
    {
        if (++refusals <= _shortAbortLimit)
        {
            _refusedShape = true;
            return false;
        }
        LOG_INFO("playerbots",
                 "Party assembler: {} sets out {} tank(s) and {} healer(s) short - "
                 "the bench has offered none for {} attempts running",
                 leader->GetName(), tanksNeeded, healersNeeded, refusals);
        refusals = 0;
    }
    else
    {
        refusals = 0;
    }

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
    {
        // A collector goes back for what the realm has moved past. Tried first and
        // allowed to fail: on a continent with no old doorway within reach the party
        // just runs the current tier like everybody else, so this can never strand
        // a group or starve progression - the persona share is the ceiling.
        if (_collectorPct && NeedsLedger::IsCollector(leader) &&
            urand(1, 100) <= _collectorPct)
            travelling = SendPartyToOldContent(group, leader);

        if (!travelling)
            travelling = wantRaid ? SendPartyToRaid(group, leader)
                                  : SendPartyToDungeon(group, leader);
    }

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
            // A bench that can already seat a twenty-five has nothing to wait for.
            // The cooldown exists to GROW a thin bench by holding capped bots back
            // from ordinary formation; when the bench is already deep the wait buys
            // nothing and costs everything, because the counter only advances on
            // continuous uptime. A deploy cadence shorter than MusterEveryMin meant
            // the muster was never once called - and since ordinary formation would
            // need 25 compatible bots on a single map, which essentially never
            // happens, the muster is the only road to a 25-man. The realm was
            // starving its own raids every time it restarted.
            // Paced, though. Measured on the first cut of this: with a bench several
            // hundred deep the test passes every tick, and the assembler raised a
            // twenty-five every forty-five seconds - which is not "raids are possible
            // again", it is raids crowding out everything else. MusterTimeoutMin is
            // the shorter of the two existing knobs and means "how long one muster may
            // take", so one muster per that window is the coherent pace; a thin bench
            // still waits out the full MusterEveryMin.
            int32 const ready = _musterCooldownTicks >= _musterTimeoutMin * ticksPerMin
                                    ? FactionWithFullBench()
                                    : -1;
            if (ready >= 0)
            {
                _musterCooldownTicks = 0;
                _musterAgeTicks = 0;
                _musterTeam = ready;
                CharacterDatabase.Execute(
                    "UPDATE aetherion_assembler SET last_muster_at = UNIX_TIMESTAMP()"
                    " WHERE id = 1");
                LOG_INFO("playerbots",
                         "Party assembler: the {} bench already seats a raid - the muster "
                         "is called at once",
                         _musterTeam == TEAM_ALLIANCE ? "Alliance" : "Horde");
            }
            else if (++_musterCooldownTicks >= _musterEveryMin * ticksPerMin)
            {
                _musterCooldownTicks = 0;
                _musterAgeTicks = 0;
                _musterTeam = urand(0, 1) ? TEAM_ALLIANCE : TEAM_HORDE;
                CharacterDatabase.Execute(
                    "UPDATE aetherion_assembler SET last_muster_at = UNIX_TIMESTAMP()"
                    " WHERE id = 1");
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
    // produce a second in the same tick either. A refusal over party shape is the
    // exception - the pool was fine, the hand it dealt was not - so the tick deals
    // again rather than surrendering the remaining slots to one unlucky leader.
    for (uint32 i = 0; i < _perTick && _assembled.size() < _maxParties; ++i)
        if (!AssembleOne() && !_refusedShape)
            break;

    SweepStrandedBots();
    SyncOwnedMirror();
    WriteTelemetry();
}
