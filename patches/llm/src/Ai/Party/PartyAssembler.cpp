#include "PartyAssembler.h"

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "Group.h"
#include "GroupMgr.h"
#include "DatabaseEnv.h"
#include "DBCEnums.h"   // DEFAULT_MAX_LEVEL: the gear test only outfits the cap
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
#include "ObjectMgr.h"   // DungeonEncounterList: which bit of the mask is which boss
#include "Player.h"
#include "PlayerbotAI.h"   // IsRealPlayer
#include "PlayerbotFactory.h"   // AutoGear: the gear-test cohort is outfitted by the module's own factory
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"

#include <algorithm>
#include <unordered_map>
#include <limits>
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
    // The count is cleared every time the event is seen running, so this bounds the
    // consecutive refusals rather than the whole run: Violet Hold resets itself to
    // NOT_STARTED the moment the last standing member of the party goes down
    // (InstanceCleanup, reached from the five-second EVENT_CHECK_PLAYERS when no
    // player in the instance is alive), and a run that asked once at the door had no
    // way to ask again afterwards.
    constexpr uint32 kVenueNudges = 4;

    // How long an event may sit on the same wave before the run is called. A Violet
    // Hold wave is a portal, a keeper and a handful of adds - minutes of work, not
    // ten of them - and twelve waves at this many ticks apiece is already more than
    // the whole dwell clock. An event that is running but not advancing is the same
    // failure the Forge of Souls taught: five bots held for an hour against content
    // that is not moving, and the clock alone will not notice.
    constexpr uint32 kEventStallTicks = 12;

    // Violet Hold's own vocabulary for the things worth killing, restated from
    // VioletHold/violet_hold.h beside the enums above and checked against it whenever
    // the core is bumped. Ordered the way the encounter is actually won:
    //  - Cyanigosa ends it.
    //  - The saboteur is what releases a boss at waves six and twelve; the boss then
    //    walks out to the middle of the room, so its cell - which is the only position
    //    the creature table knows - was never a place to steer a party to.
    //  - A keeper portal stays open for exactly as long as its Portal Guardian or
    //    Portal Keeper lives: the portal channels on it, and kills ITSELF the moment
    //    the channel finds nothing to hold (npc_vh_teleportation_portal::UpdateAI).
    //    The portal's own death is what schedules the next wave, so killing the thing
    //    holding it open is how the assault advances - grinding the adds it spawns
    //    every twenty seconds is losing by design.
    //  - An elite portal has no keeper; it goes invisible and dies when its summons
    //    are dead, so the portal's position is simply where the elites are.
    constexpr uint32 kVhCyanigosa = 31134;
    constexpr uint32 kVhSaboteur = 31079;
    constexpr uint32 kVhPortalGuardian = 30660;
    constexpr uint32 kVhPortalKeeper1 = 30695;
    constexpr uint32 kVhPortalKeeper2 = 30893;
    constexpr uint32 kVhPortal = 31011;
    // Raid target index 7. The module's own AttackersValue reads this slot, and the
    // Forge of Souls tactics write it, so it is the agreed way to say "kill this one".
    constexpr uint8 kSkullIcon = 7;
    // The six cell bosses, two of which a run meets. Worth naming even though their
    // cells are exactly what this stopped steering at, because a released one does not
    // wait at its door: StartBossEncounter opens the cell and walks it out to a fixed
    // spot in the middle of the room. Only ever chosen when it is attackable, which is
    // the flag the release itself clears - a boss still sealed in its cell carries
    // UNIT_FLAG_NON_ATTACKABLE and is passed over exactly like the position was.
    constexpr uint32 kVhBosses[] = {29266, 29312, 29313, 29314, 29315, 29316};
    constexpr uint32 kVhWaveCount = 33;          // DATA_WAVE_COUNT
    constexpr uint32 kHorWaveNumberData = 8;     // DATA_WAVE_NUMBER, same as above

    // How far across an event venue to look for the objective. Violet Hold's chamber
    // is about a hundred and fifty yards corner to corner and the portals stand at its
    // edges, so this reaches the whole room from anywhere in it without reaching out
    // of the instance.
    constexpr float kEventObjectiveRange = 220.0f;

    // Where Violet Hold's waves end up. Every one of the six portal routes finishes
    // within a yard or two of this spot - it is the last waypoint of all six waypoint
    // tables in violet_hold.h - because that is the prison door the assault is there
    // to break. A party standing on it fights every wave that spawns without walking
    // anywhere, which is the whole shape of the encounter; the party used to walk away
    // from it towards two sealed cells instead.
    constexpr float kVhMusterX = 1843.71f;
    constexpr float kVhMusterY = 805.81f;
    constexpr float kVhMusterZ = 44.14f;

    // The Trial of the Champion is the same shape and the clearest case yet for asking
    // more than once. Nothing in it is spawned: the creature table holds no Grand
    // Champion, no Argent Challenge and no Black Knight on map 650 - only mounts,
    // spectators and Tirion - because every encounter is summoned by the announcer
    // when a player takes her gossip. So the party has no boss position to walk at, no
    // enemy to meet, and 18 recorded runs there have produced 0 kills and 0 deaths
    // apiece while riding out a thirty-minute clock in an empty arena.
    //
    // The ask has to be made three separate times, which is exactly what a one-shot
    // nudge could never do: at INITIAL to summon the champions, again once they are
    // dead to bring on the Argent Challenge, and again once that is dead to bring on
    // the Black Knight. The instance guards it on its own progress counter and on
    // nothing else, so a request made at the wrong moment is simply ignored.
    // Restated from CrusadersColiseum/TrialOfTheChampion/trial_of_the_champion.h.
    constexpr uint32 kMapTrialOfChampion = 650;

    // The Culling of Stratholme, read out of the core's own
    // CullingOfStratholme/culling_of_stratholme.h rather than guessed: DATA_ARTHAS_EVENT
    // is the first entry of `enum Data`, and the progress words are its COS_PROGRESS_*
    // ladder. Nothing here can include that header - it belongs to a script library the
    // module does not link - so the values are named and cited instead.
    constexpr uint32 kMapCullingOfStratholme = 595;
    constexpr uint32 kCosArthasEvent = 0;        // DATA_ARTHAS_EVENT
    constexpr uint32 kCosNotStarted = 0;         // COS_PROGRESS_NOT_STARTED
    constexpr uint32 kCosStartIntro = 2;         // COS_PROGRESS_START_INTRO
    constexpr uint32 kCosFinished = 11;          // COS_PROGRESS_FINISHED
    constexpr uint32 kCosArthas = 26499;         // NPC_ARTHAS - the escort itself
    // Where Arthas waits before anybody wakes him, straight out of the creature table.
    // The party lands about eight hundred yards from him - a city block, not a room -
    // so the live-creature lookup that serves every other event venue finds nothing at
    // all here and the run had no destination whatsoever. Walk to the spawn until he is
    // close enough to see, then follow the man himself.
    constexpr float kCosArthasX = 1921.0f;
    constexpr float kCosArthasY = 1287.0f;
    constexpr float kCosArthasZ = 143.0f;
    // Arthas walks the city in acts and stops between them waiting to be spoken to -
    // the gossip flag is how he says so, and each stop has exactly one answer, which is
    // the DoAction its gossip option would have fired. Progress word on the left, the
    // action that ends that wait on the right, both from culling_of_stratholme.h.
    struct CosAct
    {
        uint32 progress;
        uint32 action;
    };
    constexpr CosAct kCosActs[] = {
        { 3,  2 },   // FINISHED_INTRO      -> ACTION_START_CITY
        { 7,  4 },   // REACHED_TOWN_HALL   -> ACTION_START_TOWN_HALL
        { 8,  5 },   // KILLED_EPOCH        -> ACTION_START_SECRET_PASSAGE
        { 9,  6 },   // LAST_CITY           -> ACTION_START_LAST_CITY
        { 10, 8 },   // BEFORE_MALGANIS     -> ACTION_START_MALGANIS
    };

    uint32 CosActionFor(uint32 progress)
    {
        for (CosAct const& act : kCosActs)
            if (act.progress == progress)
                return act.action;
        return 0;
    }

    // The Eye of Eternity, from the core's Nexus/EyeOfEternity/eye_of_eternity.h.
    // Malygos spawns flagged NON_ATTACKABLE and only clears it on EVENT_INTRO_LAND,
    // which is scheduled from JustEngagedWith - so nothing a party does on its own can
    // ever start him. What starts him is a player USING the Focusing Iris, which fires
    // this event id; the instance then picks the nearest player and calls AttackStart
    // on the boss. Measured before this was understood: 29 runs, zero kills and 0.1
    // deaths a run at item levels comfortably over the floor - they were never in a
    // fight at all. His phase-3 drakes are a separate matter and the module already
    // implements them (RaidEoEStrategy: "eoe fly drake", "eoe drake attack"), with no
    // master gate of the kind that stops Ulduar.
    constexpr uint32 kMapEyeOfEternity = 616;
    constexpr uint32 kEoeIrisActivated = 20711;   // EVENT_IRIS_ACTIVATED
    constexpr uint32 kEoeMalygosBoss = 0;         // DATA_MALYGOS, the only boss state
    constexpr uint32 kEoeMalygos = 28859;         // the dragon himself, for steering
    constexpr uint32 kTocInstanceProgress = 4;   // DATA_INSTANCE_PROGRESS
    constexpr uint32 kTocGossipSelect = 6;       // DATA_ANNOUNCER_GOSSIP_SELECT
    constexpr uint32 kTocProgressInitial = 0;
    constexpr uint32 kTocProgressChampionsDead = 6;
    constexpr uint32 kTocProgressChallengeDead = 8;
    constexpr uint32 kTocProgressFinished = 9;
    // Non-zero asks for the short version, which starts the fight instead of playing
    // an introduction to an arena with nobody in the stands who can hear it.
    constexpr uint32 kTocStartShort = 1;

    // The middle of the arena. Every summoned encounter is walked here - the champions
    // to 746.9/660 and 746.9/635, the Argent Challenge to 747.1/628, the Black Knight
    // to 746.8/623 - so a party standing on it is standing where the fight comes.
    constexpr float kTocMusterX = 746.60f;
    constexpr float kTocMusterY = 630.00f;
    constexpr float kTocMusterZ = 411.30f;

    // How many ticks of continuous fighting the steering waits out before it insists
    // again. Long enough for any real pull to finish, short enough that a party
    // deadlocked in a fight it cannot win still gets moved along.
    constexpr uint32 kCombatHoldTicks = 8;

    // What "not ready for the next pack" looks like from outside. Deliberately
    // generous - a party at four fifths health with a healer near full is fine to
    // walk on, and holding for perfection would spend the whole dwell clock
    // standing. Out of combat a bot eats and drinks, so a single held tick is
    // usually the whole recovery.
    constexpr float kBreatherHealthPct = 70.0f;
    constexpr float kBreatherManaPct = 65.0f;

    // What counts as getting somewhere. A party closes roughly twenty yards of ground
    // a tick once it is fighting its way in, so five is a generous floor, and six
    // ticks of failing to clear it is four minutes of a walk that is going nowhere.
    constexpr float kAimProgressYards = 5.0f;
    constexpr uint32 kAimStallTicks = 6;

    // How long a party may stand toe to toe with a living boss before the run
    // accepts it cannot win this one. Generous on purpose: at a minute a tick this
    // is a quarter of an hour of trying, which is a real attempt rather than a
    // formality, and the wipe watch ends a hopeless fight long before it expires.
    constexpr uint32 kBossFightTicks = 15;

    // How close the leader must have come to a living, attackable boss for the run
    // to stop walking and start pulling. Drakos the Interrogator never aggros on
    // sight - his script blanks MoveInLineOfSight - and hovers over a ring the mover
    // will not cross, so parties closed to 22 yards, made no further progress, and
    // the stall watch struck him "unreachable" while he floated there attackable.
    // Inside this range the boss is skulled for the party instead, which is what
    // sends the ranged half of it to pull.
    constexpr float kEngageRange = 45.0f;
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
        // The Culling is an escort across a whole city at Arthas's walking pace, with
        // four bosses strung along it. It takes what a raid wing takes.
        { kMapCullingOfStratholme, 2 },
    };

    uint32 VenueClockMult(uint32 mapId)
    {
        for (VenueClock const& venue : kVenueClocks)
            if (venue.map == mapId)
                return venue.mult;
        return 1;
    }

    // Venues whose FIRST encounter a party of bots alone structurally cannot perform,
    // however long it is given and however well geared it is. This is a different kind
    // of "no" from the gear one: gear says not yet, this says not by us.
    //
    // Ulduar is the only member, and it is one for three reasons that had to hold
    // together, each read out of the source rather than assumed:
    //  - Flame Leviathan is a vehicle fight. The salvaged Siege Engine, Demolisher and
    //    Chopper are boarded by spell click and the damage comes from the vehicle's
    //    own bar; a party on foot has no way to hurt it.
    //  - mod-playerbots does implement that fight - RaidUlduarStrategy carries
    //    "flame leviathan enter vehicle" and a per-vehicle combat action, and this
    //    realm has ApplyInstanceStrategies on, so map 603 does load it. But the
    //    trigger that starts it, FlameLeviathanVehicleNearTrigger, requires the bot's
    //    MASTER to already be sitting in a vehicle. The whole implementation is
    //    bootstrapped by a human boarding first. In a bot-only raid the leader has no
    //    master and every member's master is the leader, so nobody is ever first and
    //    not one bot ever boards.
    //  - Flame Leviathan is not skippable: GO_LIGHTNING_WALL1 is registered against
    //    BOSS_LEVIATHAN as DOOR_TYPE_PASSAGE, and a passage door opens only on DONE.
    //    The rest of the instance is behind it.
    // So a bot-only raid sent here parks 10 to 25 characters for up to two hours to
    // achieve nothing. A human-led adopted run is exactly the case the module's
    // strategy was written for and is not refused - only the assembler's own picking
    // consults this.
    struct Unperformable
    {
        uint32 map;
        char const* why;
    };
    // Nothing is refused outright any more. Ulduar sat here because its first boss is a
    // vehicle fight and the module's implementation of it waits for a human: the trigger
    // that boards a bot asks whether its MASTER is already sitting in one, and in a
    // bot-only raid the leader has no master, so nobody was ever first. That is a
    // bootstrap, not an impossibility - see BoardSiegeVehicles, which puts the leader in
    // a seat and lets the module's own Flame Leviathan actions take it from there.
    // The list is empty of real venues but not of entries: a zero-length array is a
    // compiler extension rather than C++, and this build treats warnings as fatal. The
    // sentinel is a map id no realm can hold.
    constexpr Unperformable kUnperformable[] = {
        { 0xFFFFFFFF, "no venue is refused outright" },
    };

    // Ulduar's siege yard. The instance summons these itself when the Leviathan
    // encounter arms, and despawns them when it ends, so their mere presence is the
    // signal that the vehicle phase is live - which is what keeps the steering below
    // from hijacking the other thirteen bosses of the raid.
    constexpr uint32 kMapUlduar = 603;
    constexpr uint32 kUldVehicles[] = {
        33060,  // NPC_SALVAGED_SIEGE_ENGINE
        33109,  // NPC_SALVAGED_DEMOLISHER
        33062,  // NPC_VEHICLE_CHOPPER
    };
    constexpr float kUldVehicleRange = 200.0f;
    // The rest of the yard, from boss_flame_leviathan.cpp and instance_ulduar.cpp:
    // the two colossi that must die before the Leviathan will so much as speak
    // (his start loop waits while one lives within 250 yards), the instance data
    // word that summons the salvaged vehicles and the mode that puts them at the
    // start positions, and where he parks so a vehicle can be driven into his
    // start range. Nothing here is guessed; each is the script's own number.
    constexpr uint32 kUldColossus = 33237;         // NPC_ULDUAR_COLOSSUS
    constexpr uint32 kUldFlameLeviathan = 33113;   // his creature entry
    constexpr uint32 kUldVehicleSpawn = 100;       // DATA_VEHICLE_SPAWN
    constexpr uint32 kUldVehiclePosStart = 0;      // VEHICLE_POS_START
    constexpr float kUldYardX = -784.4f;           // a siege engine's start slot
    constexpr float kUldYardY = -33.3f;
    constexpr float kUldYardZ = 429.9f;
    constexpr float kUldLeviathanX = 322.4f;       // homePos
    constexpr float kUldLeviathanY = -14.5f;
    constexpr float kUldLeviathanZ = 409.8f;

    // The Oculus, from the module's own OCTriggers.h: the ruby essence is the item a
    // player uses to summon and mount a drake, and this is the spell it casts.
    constexpr uint32 kMapOculus = 578;
    constexpr uint32 kOcRubyEssenceItem = 37860;   // ITEM_RUBY_ESSENCE
    constexpr uint32 kOcRubyEssenceSpell = 49462;  // SPELL_RUBY_ESSENCE

    char const* CannotPerform(uint32 mapId)
    {
        for (Unperformable const& venue : kUnperformable)
            if (venue.map == mapId)
                return venue.why;
        return nullptr;
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
    // Off unless the operator asks for it: this hands out gear nobody earned,
    // which is only ever acceptable as a measurement with an end date.
    _gearTestShare = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.GearTestShare", 0);
    _gearTestIlvl = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.GearTestIlvl", 200);
    _gearTestPerTick = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.GearTestPerTick", 5);
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
    _wipeBonusPerBoss =
        sConfigMgr->GetOption<int32>("AiPlayerbot.Party.WipeBonusPerBoss", 1);
    _wipeBonusCap = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.WipeBonusCap", 4);
    _progressExtendTicks =
        sConfigMgr->GetOption<int32>("AiPlayerbot.Party.ProgressExtendTicks", 10);
    _progressExtendMax =
        sConfigMgr->GetOption<int32>("AiPlayerbot.Party.ProgressExtendMax", 4);
    // Zero would make every door either open or shut on the exact item level, which
    // is the precise-gear veto this deliberately is not.
    _gearStretch = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.GearStretch", 20);
    if (!_gearStretch)
        _gearStretch = 1;
    _summonTicks = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.SummonTicks", 4);
    _exhaustGrace = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.ExhaustGrace", 4);
    _settleTicks = sConfigMgr->GetOption<int32>("AiPlayerbot.Party.SettleTicks", 2);
    _regroupRange = sConfigMgr->GetOption<float>("AiPlayerbot.Party.RegroupRange", 40.0f);
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
    _mapFloorAny.clear();
    if (QueryResult floors = WorldDatabase.Query(
            "SELECT map_id, difficulty, min_avg_item_level FROM dungeon_access_template "
            "WHERE min_avg_item_level > 0"))
    {
        do
        {
            Field* f = floors->Fetch();
            uint32 const map = f[0].Get<uint32>();
            uint16 const floor = f[2].Get<uint16>();
            _ilvlFloor[(map << 8) | f[1].Get<uint8>()] = floor;
            // The venue's own lowest word on what it takes, whichever mode said it.
            // Kept apart from the exact per-mode number because a floor borrowed from
            // another difficulty is a weaker claim and only ever gets used where the
            // mode being run said nothing at all.
            auto const any = _mapFloorAny.find(map);
            if (any == _mapFloorAny.end() || floor < any->second)
                _mapFloorAny[map] = floor;
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
        float const z = f[3].Get<float>();
        // The floor reads every row, not just the sample: the deepest creature on
        // the map is the deepest ground anything is meant to stand on.
        auto const floorIt = _floorZ.find(map);
        if (floorIt == _floorZ.end() || z < floorIt->second)
            _floorZ[map] = z;
        auto& list = _spawns[map];
        if (list.size() >= _spawnsPerMap)
            continue;
        list.push_back(Entrance{map, f[1].Get<float>(), f[2].Get<float>(), z});
    } while (result->NextRow());

    LOG_INFO("playerbots", "Party assembler: loaded spawn points for {} instance maps",
             _spawns.size());
}

bool PartyAssembler::BelowVenueFloor(uint32 mapId, Player const* who) const
{
    // Twenty-five yards under the deepest spawn is through the world in any wing
    // this realm runs: legitimate stairs and pits all keep creatures standing on
    // them, so the spawn table's own minimum is the honest bottom.
    auto const it = _floorZ.find(mapId);
    return it != _floorZ.end() && who->GetPositionZ() < it->second - 25.0f;
}

PartyAssembler::Entrance const* PartyAssembler::NearestGroundSpot(uint32 mapId, float x, float y) const
{
    // Fished bots used to go back to the arrival point, which turned every fall
    // into starting the wing over: the Forge of Souls burned 174 leader recalls
    // in 45 minutes and still never met its first boss. The route resumes at the
    // spawn nearest the fall instead - measured flat, because the faller's own z
    // is the abyss and the ground it fell from is directly above.
    auto const it = _spawns.find(mapId);
    if (it == _spawns.end() || it->second.empty())
        return nullptr;

    Entrance const* best = nullptr;
    float bestSq = 0.0f;
    for (Entrance const& spot : it->second)
    {
        float const dx = x - spot.x;
        float const dy = y - spot.y;
        float const sq = dx * dx + dy * dy;
        if (!best || sq < bestSq)
        {
            best = &spot;
            bestSq = sq;
        }
    }
    return best;
}

void PartyAssembler::LoadBossPositions()
{
    // creditType 0 means the encounter is credited by killing a creature, so its entry
    // resolves to a spawn. The other 34 encounters are credited by spell and have no
    // position; trash steering covers those maps.
    // The credit entry travels with the position. It is the only thing that ties a
    // spot on the floor to a bit in the instance's completed-encounter mask, and
    // without that tie a dead boss can only be recognised by walking onto its corpse.
    // OrderIndex rides along from the seeded DBC table. The core's own struct
    // comments that column out, so the database is the only place it survives.
    QueryResult result = WorldDatabase.Query(
        "SELECT c.map, c.position_x, c.position_y, c.position_z, ie.creditEntry, "
        "COALESCE(MIN(de.OrderIndex), 0) "
        "FROM instance_encounters ie "
        "JOIN creature c ON c.id = ie.creditEntry "
        "LEFT JOIN dungeonencounter_dbc de ON de.ID = ie.entry "
        "WHERE ie.creditType = 0 "
        "GROUP BY c.map, c.position_x, c.position_y, c.position_z, ie.creditEntry");
    if (!result)
        return;

    do
    {
        Field* f = result->Fetch();
        uint32 const map = f[0].Get<uint32>();
        _bosses[map].push_back(BossSpot{f[1].Get<float>(), f[2].Get<float>(),
                                        f[3].Get<float>(), f[4].Get<uint32>(),
                                        f[5].Get<int32>()});
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
        for (BossSpot const& boss : entry.second)
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
        uint32 recalled = 0, fished = 0;
        for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
        {
            Player* m = mi->GetSource();
            if (!m || m == leader || !m->IsInWorld() || m->IsBeingTeleported())
                continue;
            // Two ways to be gone: off the map entirely, or through its floor.
            // The Forge of Souls' rail-less bridges drop members into an abyss a
            // hundred yards below every spawn on the map, where follow can no more
            // retrieve them than it can cross a map boundary.
            bool const fell = m->GetMapId() == trip.dungeonMap &&
                              BelowVenueFloor(trip.dungeonMap, m);
            if (m->GetMapId() == trip.dungeonMap && !fell)
                continue;
            // To the leader, not the door: the leader is on the map and on its feet
            // whenever this runs, and a member sent to the arrival point re-walks
            // the wing alone through everything the party already cleared.
            m->TeleportTo(trip.dungeonMap, leader->GetPositionX(),
                          leader->GetPositionY(), leader->GetPositionZ(), 0.0f);
            if (fell)
                ++fished;
            else
                ++recalled;
        }
        if (recalled)
            LOG_INFO("playerbots", "Party assembler: {} rejoin the party in {}",
                     recalled, trip.name);
        if (fished)
            LOG_INFO("playerbots",
                     "Party assembler: {} fished out of the pit under {}",
                     fished, trip.name);
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
    {
        // Counted against the run, not just logged. Deaths short of a wipe are the
        // ones pacing is meant to remove, and the ledger had no column that named
        // them - a party that lost a member on every pull and picked it back up read
        // exactly as clean as one that never lost anybody.
        trip.deaths += raised;
        if (trip.runId)
            CharacterDatabase.Execute(
                "UPDATE aetherion_run_history SET deaths = {} WHERE id = {}",
                trip.deaths, trip.runId);
        LOG_INFO("playerbots", "Party assembler: {} back on their feet in {}",
                 raised, trip.name);
    }
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

void PartyAssembler::GearTestPass()
{
    if (!_gearTestShare)
        return;

    uint32 done = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd() && done < _gearTestPerTick; ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld() || bot->IsBeingTeleported())
            continue;
        uint32 const low = bot->GetGUID().GetCounter();
        if (low % _gearTestShare || _gearTested.count(low))
            continue;
        // Only the level cap. The gear floors this experiment is meant to clear
        // are all end-game ones, and re-rolling a levelling bot's kit would
        // measure nothing.
        if (bot->GetLevel() < DEFAULT_MAX_LEVEL)
            continue;
        // Never mid-run and never mid-fight: the refit destroys what it replaces,
        // and stripping a character standing in a boss room would corrupt the very
        // outcome this experiment exists to read.
        if (bot->IsInCombat() || (bot->GetMap() && bot->GetMap()->IsDungeon()))
            continue;

        PlayerbotFactory::DestroyEquippedGear(bot);
        PlayerbotFactory::AutoGear(bot, ITEM_QUALITY_EPIC, _gearTestIlvl, /*incremental*/ false);

        _gearTested.insert(low);
        CharacterDatabase.Execute(
            "INSERT IGNORE INTO aetherion_geartest (guid, name, at, ilvl) "
            "VALUES ({}, '{}', UNIX_TIMESTAMP(), {})",
            low, Sql(bot->GetName()), _gearTestIlvl);
        ++done;
    }

    if (done)
        LOG_INFO("playerbots",
                 "Gear test: outfitted {} in epics at ilvl {} - {} of the cohort so far",
                 done, _gearTestIlvl, uint32(_gearTested.size()));
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
        // Whatever the venue is worth, plus whatever this particular run has earned by
        // actually killing things. The bonus is only ever awarded against a boss that
        // fell, so a raid going nowhere still ends on the same clock it always did.
        uint32 const budget =
            (trip.phase == Phase::Inside ? _insideTicks : _maxTripTicks) * mult +
            trip.bonusTicks;
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
            else if (group)
            {
                // Out of the instance first where they got in. Disbanding alone
                // leaves five characters standing in a dungeon nothing will ever
                // move them out of; a party that died on the road just disbands.
                if (trip.phase == Phase::Inside)
                {
                    uint32 const out = SendGroupOutside(group, trip.dungeonMap);
                    if (out)
                        LOG_INFO("playerbots",
                                 "Party assembler: {} leave {} after their run",
                                 out, trip.name);
                }
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
            else if (group)
                group->Disband();
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

            // The leader is the run's single point of failure and nothing below
            // survives losing it: the steering, the member recall and the exhausted
            // close-out are all anchored on the leader standing in the instance, and
            // the recall explicitly skips the leader itself. So a leader that died
            // and ghost-released - instance graveyards are outside - decapitated the
            // run: members were revived in place while nobody steered, and the run
            // burned its whole budget to a silent 'ended'. Naxxramas raids logged
            // two hours inside without one steering line; both Forge of Souls
            // parties sampled had their leader standing at the door outside. The
            // tank-leads-the-pull change made this the COMMON case, because the
            // leader is now the character that dies first by design. A full party
            // that releases and spirit-revives between two of these ticks is the
            // same failure - alive at the graveyard, never counted as a wipe - and
            // the same recall heals it, one tick for the leader and the next for
            // everyone the member recall can now see again.
            // Off the map and under it are the same loss: a leader that fell off a
            // Forge of Souls bridge stands a hundred yards beneath the boss it is
            // steering at, close enough in x and y to keep every appearance of
            // progress while nothing can ever happen again.
            if (trip.arrived && !trip.adopted &&
                !leader->IsBeingTeleported() && !leader->InBattleground())
            {
                bool const offMap = leader->GetMapId() != trip.dungeonMap;
                bool const fell = !offMap && BelowVenueFloor(trip.dungeonMap, leader);
                auto const rally = _insides.find(trip.dungeonMap);
                // If nobody at all is still standing inside, this is not a lost
                // leader, it is a wipe that released before the tick saw it. Left to
                // the wipe watch below, which counts it and reseats everyone at once,
                // rather than recalled here one character at a time with no tally.
                bool anyoneStanding = false;
                for (GroupReference* mi = group->GetFirstMember(); mi != nullptr && !anyoneStanding;
                     mi = mi->next())
                    if (Player* m = mi->GetSource())
                        if (m->IsInWorld() && m->IsAlive() && m->GetMapId() == trip.dungeonMap)
                            anyoneStanding = true;
                if ((offMap || fell) && rally != _insides.end() && anyoneStanding)
                {
                    if (!leader->IsAlive())
                    {
                        leader->ResurrectPlayer(0.5f);
                        leader->SpawnCorpseBones();
                        if (PlayerbotAI* lAI = GET_PLAYERBOT_AI(leader))
                        {
                            lAI->ResetStrategies(false);
                            lAI->ChangeStrategy("-lfg,-bg", BOT_STATE_NON_COMBAT);
                        }
                    }
                    // A faller resumes at the ground nearest the fall; only a leader
                    // gone from the map entirely restarts at the arrival point.
                    float tx = rally->second.x, ty = rally->second.y, tz = rally->second.z;
                    if (fell)
                        if (Entrance const* ground = NearestGroundSpot(
                                trip.dungeonMap, leader->GetPositionX(), leader->GetPositionY()))
                        {
                            tx = ground->x;
                            ty = ground->y;
                            tz = ground->z;
                        }
                    // Members in the pit leave with the leader, to the same spot. The
                    // early continue below used to skip their recovery on exactly the
                    // ticks the leader was lost too, which in a venue where the whole
                    // party falls together meant nobody was ever fished at all.
                    uint32 fished = 0;
                    for (GroupReference* mi = group->GetFirstMember(); mi != nullptr;
                         mi = mi->next())
                    {
                        Player* m = mi->GetSource();
                        if (!m || m == leader || !m->IsInWorld() || m->IsBeingTeleported())
                            continue;
                        if (m->GetMapId() != trip.dungeonMap ||
                            !BelowVenueFloor(trip.dungeonMap, m))
                            continue;
                        if (!m->IsAlive())
                        {
                            m->ResurrectPlayer(0.5f);
                            m->SpawnCorpseBones();
                        }
                        m->TeleportTo(trip.dungeonMap, tx, ty, tz, 0.0f);
                        ++fished;
                    }
                    leader->TeleportTo(trip.dungeonMap, tx, ty, tz, 0.0f);
                    LOG_INFO("playerbots",
                             "Party assembler: {} {} {} mid-run - the party regroups "
                             "on solid ground ({} fished along)",
                             leader->GetName(), offMap ? "walked out of" : "fell under",
                             trip.name, fished);
                    ++it;
                    continue;
                }
            }

            // What the instance itself says has died, read while it still exists.
            // Ordered before the wipe watch because how many bosses are down is what
            // decides how much determination this run has bought.
            NoteKills(group, leader, trip);

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
            // "Alive" means alive IN the instance. A raid that dies and releases
            // between two of these ticks stands alive at the graveyard outside, and
            // counting that as alive is how a Naxxramas raid wiped four times in
            // 48 minutes with wipes=0 in the ledger: the leader was recalled, the
            // nine outside were pulled in after it, and the wipe budget that should
            // have ended the run never moved.
            uint32 alive = 0, present = 0;
            bool humanAboard = false;
            for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
                if (Player* m = mi->GetSource())
                    if (m->IsInWorld())
                    {
                        ++present;
                        if (m->IsAlive() && m->GetMapId() == trip.dungeonMap)
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
                // A wipe is also `present` deaths. Counted here rather than in the
                // recovery loop below, which the retry ceiling can skip past.
                trip.deaths += present;
                if (trip.runId)
                    CharacterDatabase.Execute(
                        "UPDATE aetherion_run_history SET wipes = {}, deaths = {}"
                        " WHERE id = {}",
                        trip.wipes, trip.deaths, trip.runId);
                // Determination, again paid for rather than configured. A raid four
                // bosses deep is demonstrably in business and gets to keep pulling; a
                // party that wipes on the way to the first still calls it after the
                // base retries. That keeps "try harder" from becoming "hold ten bots
                // hostage in a fight nobody can win".
                uint32 const allowed =
                    _wipeRetries +
                    std::min(_wipeBonusCap, trip.bossesDown * _wipeBonusPerBoss);
                if (trip.wipes > allowed)
                {
                    LOG_INFO("playerbots",
                             "Party assembler: wipe {} of {} in {} breaks the raid - "
                             "they call it {} bosses in",
                             trip.wipes, allowed, trip.name, trip.bossesDown);
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
                // A whole fresh attempt needs something to attempt. Boss positions are
                // retired the moment the leader stands within eight yards of one, and
                // standing on a boss is not killing it - so a party that reached the
                // first boss, wiped on it, and was reseated at the door came back to an
                // empty list and spent the rest of its clock in the trash. The ones the
                // instance mask says are dead are re-struck by NoteKills on the next
                // pass; the ones the party demonstrably could not walk to stay struck.
                trip.visitedBosses = trip.unreachableBosses;
                trip.aimBoss = kNoBossAim;
                trip.aimStalls = 0;
                trip.idleTicks = 0;

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

            // Two venues hold their content behind an opening request, and holding it
            // open is a job that lasts the whole run rather than one knock at the
            // door. One member carries it: the instance answers any player standing on
            // its map, and the leader is the one character guaranteed to be off
            // somewhere swinging at something when the ask is needed.
            bool eventLive = false;
            uint32 eventStage = 0;
            if (IsEventVenue(trip.dungeonMap))
            {
                Player* warden = PickWarden(group, trip);
                eventLive = VenueEventLive(warden, trip.dungeonMap, eventStage);

                // How long the venue has sat on the same number, kept whether or not it
                // reports itself running. A hall stuck on a wave nobody can kill and an
                // arena stuck at a gate nobody can open are the same failure, and the
                // dwell clock alone is far too slow to notice either.
                if (eventStage != trip.eventStage)
                {
                    if (trip.eventStage != Trip::kNoStage)
                        LOG_INFO("playerbots",
                                 "Party assembler: {} holds {} at stage {}",
                                 warden ? warden->GetName() : leader->GetName(),
                                 trip.name, eventStage);
                    trip.eventStage = eventStage;
                    trip.stageTicks = 0;
                }
                else if (leader->IsInCombat())
                    // A party swinging at something is not a stalled event, whatever the
                    // counter says. Violet Hold spends whole waves on one stage number.
                    trip.stageTicks = 0;
                else
                    ++trip.stageTicks;

                // A venue that is running has earned a fresh budget of asks. This is
                // what makes the role last the run: Violet Hold winds itself back to
                // NOT_STARTED as soon as nobody in the instance is alive, and the old
                // one-shot flag meant a party that wiped once stood in a hall that
                // would never start again.
                if (eventLive)
                    trip.nudges = 0;

                // The Trial's counter does not move when its ask lands, so asking
                // whenever it looks stopped would summon the next act over and over.
                // Once per stage there; Violet Hold and the Halls refuse a repeat
                // themselves and are asked whenever they say they are not running.
                bool const wantsAsk =
                    !eventLive && (trip.dungeonMap != kMapTrialOfChampion ||
                                   trip.askedStage != eventStage);

                if (wantsAsk && warden && trip.nudges < kVenueNudges)
                {
                    ++trip.nudges;
                    if (StartVenueEvent(warden, trip))
                    {
                        if (trip.started)
                            LOG_INFO("playerbots",
                                     "Party assembler: {} asks {} for the next act",
                                     warden->GetName(), trip.name);
                        else
                            LOG_INFO("playerbots",
                                     "Party assembler: {} asks for the event to start "
                                     "in {}",
                                     warden->GetName(), trip.name);
                        trip.started = true;
                        trip.askedStage = eventStage;
                        // The ask landed this tick, so there is content coming and the
                        // close-out below must not see an idle hall.
                        eventLive = true;
                    }
                }
            }

            // Bosses first; trash where no boss position is known, and again once
            // every boss has had its visit. Held as pointers rather than iterators:
            // the two maps are separate containers and comparing an iterator from one
            // against the other's end() is undefined.
            std::vector<BossSpot> const* bossSpots = nullptr;
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
                // An event venue's boss positions are the addresses of locked cells.
                // Violet Hold's two - Erekem and Moragg, the only encounters in the
                // place with a creature spawned at all - sit sealed until a saboteur
                // releases one at wave six, and the released boss then walks out to the
                // middle of the room rather than waiting at its door. So the party
                // walked seventy-eight yards to one cell, ninety-nine to the other,
                // retired both for having been reached, found nothing left and called
                // the run exhausted: four of five runs ended that way at ten to
                // fourteen minutes with not a single death between them, while the
                // assault they had just asked for was still on its first wave. Struck
                // off as unreachable rather than visited, so a wipe does not hand them
                // back.
                if (bossSpots && IsEventVenue(trip.dungeonMap))
                    for (uint32 i = 0; i < bossSpots->size(); ++i)
                    {
                        trip.unreachableBosses.insert(i);
                        trip.visitedBosses.insert(i);
                    }

                if (bossSpots)
                    for (uint32 i = 0; i < bossSpots->size(); ++i)
                    {
                        BossSpot const& spot = (*bossSpots)[i];
                        float const dx = leader->GetPositionX() - spot.x;
                        float const dy = leader->GetPositionY() - spot.y;
                        // All three axes, because instanced ground stacks: a leader
                        // in the Forge of Souls' abyss passed within eight flat
                        // yards of both bosses a hundred yards overhead, retired
                        // them unfought, and the run ended clean at zero deaths.
                        float const dz = leader->GetPositionZ() - spot.z;
                        if (dx * dx + dy * dy + dz * dz >= _huntRange * _huntRange)
                            continue;

                        if (trip.reachedBosses.insert(i).second)
                            LOG_INFO("playerbots",
                                     "Party assembler: {}'s party reaches boss {} of "
                                     "{} in {}",
                                     leader->GetName(), uint32(trip.reachedBosses.size()),
                                     uint32(bossSpots->size()), trip.name);

                        // Arrival is not victory. Retirement belongs to the instance
                        // mask - NoteKills strikes off whatever it says is dead - so
                        // all this does is give the fight a budget and give up only
                        // when that budget is spent. Retiring on arrival is what made
                        // the Forge of Souls unwinnable: the party walked to Bronjahm,
                        // struck him off before a blow landed, and spent the rest of a
                        // two-hour clock in the trash while he stood there alive.
                        if (BossIsDown(trip, i))
                            continue;
                        if (++trip.bossFightTicks[i] > kBossFightTicks)
                        {
                            trip.unreachableBosses.insert(i);
                            trip.visitedBosses.insert(i);
                            LOG_INFO("playerbots",
                                     "Party assembler: {}'s party cannot bring down a "
                                     "boss in {} after {} ticks at its feet - moving on",
                                     leader->GetName(), trip.name, kBossFightTicks);
                        }
                    }

                // Head for the nearest boss nobody has been to yet. Members follow the
                // leader, so steering one character walks the whole party through the
                // dungeon.
                // Nobody is re-tasked mid-fight. The mover has no combat guard of its
                // own beyond the one this patch chain adds, and a leader sent to the
                // next pack while the current one is still swinging walks the whole
                // party out of the fight - members follow the leader, the pack resets,
                // and the run nets a couple of dozen yards per tick against a wing
                // five hundred yards long. Held rather than cancelled: the
                // destination stands, so the walk resumes the moment the fight ends.
                // Asked before the steering rather than after it, because whether the
                // party is swinging is also what decides whether a run with nothing
                // left to walk at is finished or merely between pulls.
                bool fighting = leader->IsInCombat();
                for (GroupReference* mi = group->GetFirstMember(); !fighting && mi != nullptr;
                     mi = mi->next())
                    if (Player* m = mi->GetSource())
                        if (m->IsInWorld() && m->IsInCombat())
                            fighting = true;

                // The beat between pulls. A fight arms it and its end spends it, so
                // the pause is taken where a party would take one - after the pack
                // is down, not on a clock of its own - and only while the party
                // still has something to stand there for. Read before the counter
                // moves, or a one-tick grace would be spent on the tick that armed
                // it and the party would walk on with the corpses still warm.
                bool const settling = !fighting && trip.settleTicks &&
                                      NeedsBreather(group, leader);
                if (fighting)
                    trip.settleTicks = _settleTicks;
                else if (trip.settleTicks)
                    --trip.settleTicks;

                bool haveBest = false;
                float bestX = 0.0f, bestY = 0.0f, bestZ = 0.0f;
                float bestDist = 0.0f;
                int32 bestOrder = 0;
                uint32 bestBoss = kNoBossAim;
                if (bossSpots)
                    for (uint32 i = 0; i < bossSpots->size(); ++i)
                    {
                        if (trip.visitedBosses.count(i))
                            continue;
                        BossSpot const& spot = (*bossSpots)[i];
                        float const dx = leader->GetPositionX() - spot.x;
                        float const dy = leader->GetPositionY() - spot.y;
                        // Three-dimensional like the retirement above, and for the
                        // stall watch too: descending a spiral toward a boss is
                        // progress even on the ticks its flat distance widens.
                        float const dz = leader->GetPositionZ() - spot.z;
                        float const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        // Encounter order first, distance second. "Nearest" is the
                        // right rule for a corridor and the wrong one for a hub: from
                        // the Naxxramas entry Gothik and the Horsemen are as near as
                        // Anub'Rekhan and stand behind two closed gates, and 15 raids
                        // in a day struck them off unreachable one six-tick stall at a
                        // time - 544 bot-hours, two kills. Walking the wings in the
                        // order the place was built for never aims through a door.
                        if (!haveBest || spot.order < bestOrder ||
                            (spot.order == bestOrder && dist < bestDist))
                        {
                            haveBest = true;
                            bestX = spot.x;
                            bestY = spot.y;
                            bestZ = spot.z;
                            bestDist = dist;
                            bestBoss = i;
                            bestOrder = spot.order;
                        }
                    }

                // Every boss this venue has is either dead or given up on. Standing
                // the party in the trash for the rest of an hour-and-a-half clock is
                // not persistence, it is 10 to 25 bots held out of the world for
                // nothing, so the run is closed out instead - after a short grace, so
                // a fight in progress and a spell-credited encounter both still land.
                //
                // Two things this used to get wrong. It refused to close out at all
                // while anybody was in combat, and a wing thick with trash is a party
                // in combat more or less permanently: the Forge of Souls reached both
                // its bosses, killed neither, and then ground trash for the rest of a
                // ninety-minute clock - 102 minutes and 0.2 bosses across six runs, the
                // longest runs on the realm and among the emptiest. Combat now buys a
                // longer grace rather than an unlimited one. And a venue whose content
                // is an event has no boss to walk at BY DESIGN for most of the run, so
                // a running event holds the door open however empty the boss list is;
                // an event that will not start is exactly the run that should end.
                // A boss the party has closed on is pulled, not walked at. The skull
                // is the module's own "attack this" (AttackersValue reads it), and it
                // reaches the casters standing where the mover stopped. Set once per
                // target so the icon is not rebroadcast every tick.
                bool engaged = false;
                if (bestBoss != kNoBossAim && bossSpots)
                    if (Creature* live = leader->FindNearestCreature(
                            (*bossSpots)[bestBoss].creditEntry, kEngageRange, true))
                        if (leader->IsValidAttackTarget(live))
                        {
                            engaged = true;
                            if (group->GetTargetIcon(kSkullIcon) != live->GetGUID())
                                group->SetTargetIcon(kSkullIcon, leader->GetGUID(), live->GetGUID());
                        }

                bool nothingLeft = bossSpots && !haveBest;
                if (IsEventVenue(trip.dungeonMap))
                    // An event venue always has somewhere to stand, so its boss list
                    // says nothing about whether the run is over. Two things end it
                    // instead: the event refusing to run at all after its whole budget
                    // of asks, and the event running but going nowhere - a party that
                    // cannot get past a wave is not going to get past the eleven
                    // behind it, and the alternative is holding five bots for the rest
                    // of an hour to watch it fail again.
                    nothingLeft = (!eventLive && trip.nudges >= kVenueNudges) ||
                                  trip.stageTicks > kEventStallTicks;
                // Ulduar keeps its other thirteen bosses behind a passage door that only
                // the Leviathan's death opens. A raid that has given up on him has
                // nothing left it can walk to, whatever the list still says - and
                // letting the list have its way cost 142 six-tick stalls in one raid.
                if (trip.dungeonMap == kMapUlduar)
                    if (uint32 const fl = BossIndexFor(kMapUlduar, kUldFlameLeviathan);
                        fl != kNoBossAim && trip.unreachableBosses.count(fl) &&
                        !BossIsDown(trip, fl))
                        nothingLeft = true;
                if (nothingLeft)
                {
                    bool const cleared = trip.encounters &&
                                         CountBits(trip.killMask) >= trip.encounters;
                    uint32 const grace =
                        fighting ? _exhaustGrace + kCombatHoldTicks : _exhaustGrace;
                    if (cleared || ++trip.idleTicks > grace)
                    {
                        char const* verdict = cleared
                            ? (trip.bossesDown ? "cleared" : "locked_out")
                            : "exhausted";
                        LOG_INFO("playerbots",
                                 "Party assembler: {}'s party is done with {} - {} of {} "
                                 "down, {}",
                                 leader->GetName(), trip.name, trip.bossesDown,
                                 trip.encounters, verdict);
                        EndRun(trip.runId, trip.dungeonMap, verdict);
                        if (trip.adopted)
                            ReleaseAdopted(group);
                        else
                        {
                            SendGroupOutside(group, trip.dungeonMap);
                            group->Disband();
                        }
                        it = _trips.erase(it);
                        continue;
                    }
                }

                // An event venue is steered by what the event has on the floor right
                // now, not by a spawn table. The order the objective is chosen in is
                // the order the encounter is actually won: in Violet Hold a portal
                // stays open for exactly as long as the Portal Guardian or Keeper it
                // is channelling on lives, and the portal's death is what schedules the
                // next wave - so the party that kills the keeper advances the assault
                // and the party that grinds the adds it spawns every twenty seconds
                // does not. Aimed at the creature's live position rather than at a
                // radius: the bots' own target search only reaches about thirty yards
                // and is leashed to fifteen from whoever they follow, and neither of
                // those is ours to widen from here, so putting the party on top of the
                // thing that matters is what actually finds it. Between waves the
                // objective is the prison door every route ends at.
                // Ulduar's siege yard, before anything else this tick: a raid on foot in
                // front of Flame Leviathan has no fight to have. Self-limiting, because
                // the vehicles only exist while that encounter is armed - once it ends
                // the instance despawns them and the ordinary boss steering resumes for
                // the thirteen bosses behind it.
                {
                    float vx, vy, vz;
                    if (BoardSiegeVehicles(leader, trip, vx, vy, vz))
                    {
                        haveBest = true;
                        bestX = vx;
                        bestY = vy;
                        bestZ = vz;
                        bestBoss = kNoBossAim;
                    }
                    // The Oculus needs no destination of its own - the boss list
                    // already points at the next platform - only a leader on a drake
                    // so the party can follow it into the air.
                    OculusDrakes(leader, trip);
                }

                if (IsEventVenue(trip.dungeonMap))
                {
                    // What to hit, before where to stand: the party can be in exactly
                    // the right place and still lose the wave by spreading its damage.
                    FocusEventTarget(group, leader, trip);

                    float ex, ey, ez;
                    if (EventObjective(leader, trip, ex, ey, ez))
                    {
                        haveBest = true;
                        bestX = ex;
                        bestY = ey;
                        bestZ = ez;
                        bestBoss = kNoBossAim;
                        float const dx = leader->GetPositionX() - ex;
                        float const dy = leader->GetPositionY() - ey;
                        bestDist = std::sqrt(dx * dx + dy * dy);
                    }
                }

                // Nothing left worth a name: hunt trash rather than stand still for
                // the grace ticks above.
                if (!haveBest && trashSpots)
                    for (Entrance const& spot : *trashSpots)
                    {
                        float const dx = leader->GetPositionX() - spot.x;
                        float const dy = leader->GetPositionY() - spot.y;
                        float const dz = leader->GetPositionZ() - spot.z;
                        float const dist = std::sqrt(dx * dx + dy * dy + dz * dz);
                        // Only skip a target we are practically standing on. Skipping
                        // anything within a generous radius sent a party that had
                        // closed to 20 yards off towards a different pack, so it
                        // oscillated between them and committed to neither.
                        if (dist < _huntRange)
                            continue;
                        if (!haveBest || dist < bestDist)
                        {
                            haveBest = true;
                            bestX = spot.x;
                            bestY = spot.y;
                            bestZ = spot.z;
                            bestDist = dist;
                        }
                    }

                if ((fighting && trip.combatTicks < kCombatHoldTicks) || settling)
                {
                    if (fighting)
                        ++trip.combatTicks;
                    // Held where it stands, not merely left untasked. patch_rpgcombat
                    // already stops rpg movement while the bot ITSELF is fighting,
                    // but the pull that matters here is the other one: the leader
                    // finishes its target, drops combat while the party behind it is
                    // still swinging, and walks on into the next pack every bot tick
                    // against the destination it was last given - whatever this
                    // object does or does not re-issue at forty-five second
                    // intervals. Withholding the next destination was never the same
                    // thing as stopping. This is.
                    HoldLeader(leader, trip, true);
                }
                else if (haveBest)
                {
                    // A fight that never ends - one member kiting, or a pack that
                    // cannot be reached - would otherwise hold the party still until
                    // the clock runs out, so past the hold the steering resumes.
                    HoldLeader(leader, trip, false);
                    trip.combatTicks = 0;

                    // A boss the party cannot get any closer to is not a boss to keep
                    // walking at. Naxxramas keeps its wings behind teleporters no
                    // mover paths through, so a leader aimed at the nearest one simply
                    // stands in the entry hub: 38 recorded runs there spent over an
                    // hour inside apiece without a single kill or even a wipe. Give up
                    // on that one and try the next. Only counted out of combat - a
                    // party holding still because it is fighting is not stuck.
                    // Only while still walking. A party standing at its boss between
                    // pulls has not failed to reach anything, and counting those ticks
                    // as a failure to walk would retire the boss out from under the
                    // fight budget the arrival check just granted it.
                    // Nor while the boss itself is in sight and marked: standing at
                    // pulling range of a living boss is the walk having succeeded.
                    if (bestBoss != kNoBossAim && !fighting && !engaged && bestDist > _huntRange)
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
                            // Struck off for good, wipes included: a boss no mover can
                            // path to from inside is no more reachable on the second
                            // attempt than the first.
                            trip.unreachableBosses.insert(bestBoss);
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
                    if (Unit* base = leader->GetVehicleBase())
                    {
                        // A seated leader is driven, not walked. The rpg mover has no
                        // notion of a vehicle, so the vehicle itself is moved, the
                        // way the module moves a drake - straight at the aim, no
                        // ground path, because a drake has no ground and a siege
                        // engine's yard has no walls. Only between fights: once the
                        // vehicle is swinging the encounter tactics own the wheel.
                        if (!base->IsInCombat())
                        {
                            // This core's MovePoint takes forced-movement, speed and
                            // orientation before the path flag; the last argument is
                            // what says "straight there, no ground path".
                            base->GetMotionMaster()->MovePoint(0, bestX, bestY, bestZ,
                                                               FORCED_MOVEMENT_NONE, 0.f, 0.0f,
                                                               /*generatePath*/ false);
                            base->SendMovementFlagUpdate();
                        }
                    }
                    else if (std::fabs(bestX - trip.aimX) > 1.0f ||
                             std::fabs(bestY - trip.aimY) > 1.0f ||
                             GET_PLAYERBOT_AI(leader)->rpgInfo.GetStatus() != RPG_GO_GRIND)
                    {
                        trip.aimX = bestX;
                        trip.aimY = bestY;
                        GET_PLAYERBOT_AI(leader)->rpgInfo.ChangeToGoGrind(
                            WorldPosition(trip.dungeonMap, bestX, bestY, bestZ));
                    }
                }
                else
                {
                    // Nothing left to walk at and nothing to wait for. The run is on
                    // its way out through the grace above, but a leader must never be
                    // left standing with its own engine switched off.
                    HoldLeader(leader, trip, false);
                }
            }
            ++it;
            continue;
        }

        if (trip.phase == Phase::Summoning)
        {
            // Gathering at the stone is scenery, and some doorways will not allow it.
            // The entry test asks that every member be within sixty yards of the
            // leader in THREE dimensions, and at some doors that never becomes true,
            // so the party stands outside until the whole trip budget runs out.
            // Measured on this realm before this was added:
            //   Ulduar   37 runs, 32 reached the door,  4 entered, 13 door_timeout
            //   Uldaman   8 runs,  6 reached the door,  0 entered,  3 door_timeout
            //   Zul'Farrak 15 runs, 6 reached the door, 5 entered,  1 door_timeout
            // Height alone does not explain it: Ulduar's trigger stands at z=1320 on a
            // mountaintop platform, but Uldaman's is at z=214 and fails just as
            // completely. What both have in common is a leader standing on ground the
            // heightmap does not agree with, and a summon target that comes from
            // GetClosePoint - which snaps its z to the allowed position. A member put
            // down at the snapped height is far enough away in z alone to fail a
            // three-dimensional sixty-yard test, and is summoned to the same place
            // again next tick. That is the leading explanation and it is NOT proven
            // here; the escape valve deliberately does not depend on which door is
            // awkward or why, because the instance teleport does not care where anyone
            // was standing when it fired.
            // Tested before the summon rather than after it: a member teleported this
            // very tick is mid-flight, and EnterInstance skips those, so summoning and
            // giving up in the same pass would leave the scattered members behind.
            // RecoverInside collects the stragglers on the far side.
            // Four ticks is well clear of the one or two an ordinary door takes, so
            // the maps that converge today are untouched by it.
            bool const gaveUpGathering = ++trip.summonTicks > _summonTicks;

            // Meeting stones sit at dungeon entrances, so "leader arrives, everyone
            // else gets summoned" is exactly what a real group does.
            uint32 scattered = 0, summoned = 0;
            for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
            {
                Player* member = ref->GetSource();
                if (!member || member == leader || member->IsBeingTeleported())
                    continue;
                if (member->GetMapId() == leader->GetMapId() &&
                    member->GetDistance(leader) <= _arriveRange)
                    continue;

                ++scattered;
                if (gaveUpGathering)
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
            else if (gaveUpGathering && scattered)
                LOG_INFO("playerbots",
                         "Party assembler: the stone at {} will not hold the party - "
                         "{} still scattered, they go in from where they stand",
                         trip.name, scattered);

            // Zone in on the following tick, once summons have landed.
            if (!summoned || gaveUpGathering)
            {
                if (!EnterInstance(group, trip))
                {
                    EndRun(trip.runId, trip.dungeonMap, "enter_failed");
                    if (!trip.adopted)
                        group->Disband();
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
bool PartyAssembler::StartVenueEvent(Player* asker, Trip const& trip)
{
    // Only meaningful once the asker is actually standing in the place.
    if (asker->GetMapId() != trip.dungeonMap)
        return false;

    InstanceScript* instance = asker->GetInstanceScript();
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

    if (trip.dungeonMap == kMapEyeOfEternity)
    {
        // Turning the Focusing Iris, one step further down than the click. The instance
        // answers this event by picking the nearest player within 250 yards and setting
        // Malygos on them, which is exactly what the gameobject's own use does - and
        // unlike the gameobject it does not care whether a bot knows how to click.
        // Only from a standing start: he is woken once.
        if (instance->GetBossState(kEoeMalygosBoss) != NOT_STARTED)
            return false;
        instance->ProcessEvent(nullptr, kEoeIrisActivated);
        return true;
    }

    if (trip.dungeonMap == kMapCullingOfStratholme)
    {
        // Normally a player walks up to Chromie, takes the errand and hunts five crates
        // before Arthas will move. None of that is reachable from here, and none of it
        // is the point - the city is what the run came for. So the same rung the crate
        // hunt would eventually set is set directly, which hands Arthas his own
        // ACTION_START_EVENT and puts the escort on the road. Asked only from a
        // standing start, because setting it twice would restart the intro over a city
        // already burning.
        uint32 const progress = instance->GetData(kCosArthasEvent);
        if (progress == kCosNotStarted)
        {
            instance->SetData(kCosArthasEvent, kCosStartIntro);
            return true;
        }

        // Every act after the first is unlocked the same way a player unlocks it, one
        // step below the conversation. Without this the escort finished its intro and
        // stopped there for good - measured as stage 3, zero deaths, zero kills.
        uint32 const action = CosActionFor(progress);
        if (!action)
            return false;
        Creature* arthas = asker->FindNearestCreature(kCosArthas, kEventObjectiveRange, true);
        if (!arthas || !arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) || !arthas->AI())
            return false;
        arthas->AI()->DoAction(action);
        arthas->RemoveNpcFlag(UNIT_NPC_FLAG_GOSSIP);
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

    if (trip.dungeonMap == kMapTrialOfChampion)
    {
        // Asked at each of the three stops and nowhere else. The instance ignores the
        // request at any other progress value, but asking anyway would spend a nudge
        // and read in the log as an event that refuses to start.
        uint32 const progress = instance->GetData(kTocInstanceProgress);
        if (progress != kTocProgressInitial &&
            progress != kTocProgressChampionsDead &&
            progress != kTocProgressChallengeDead)
            return false;
        instance->SetData(kTocGossipSelect, kTocStartShort);
        return true;
    }

    return false;
}

bool PartyAssembler::IsEventVenue(uint32 mapId)
{
    return mapId == kMapVioletHold || mapId == kMapHallsOfReflection ||
           mapId == kMapTrialOfChampion || mapId == kMapCullingOfStratholme ||
           mapId == kMapEyeOfEternity;
}

bool PartyAssembler::VenueEventLive(Player* onMap, uint32 mapId, uint32& stage) const
{
    stage = 0;
    if (!onMap || onMap->GetMapId() != mapId)
        return false;

    InstanceScript* instance = onMap->GetInstanceScript();
    if (!instance)
        return false;

    if (mapId == kMapVioletHold)
    {
        // The instance keeps its own three-state answer and hands it out through
        // GetData, which is the same door StartVenueEvent already knocks on. DONE is
        // Cyanigosa dead, and a finished venue is not a running one.
        stage = instance->GetData(kVhWaveCount);
        return instance->GetData(kVhEncounterStatus) == IN_PROGRESS;
    }

    if (mapId == kMapHallsOfReflection)
    {
        // The Halls has no status word, only a wave counter that the intro starts and
        // nothing ever winds back. Non-zero is running.
        stage = instance->GetData(kHorWaveNumberData);
        return stage != 0;
    }

    if (mapId == kMapEyeOfEternity)
    {
        // Health remaining, coarsely, rather than the boss state. State was the obvious
        // choice and it was wrong twice over: it sits on IN_PROGRESS for the whole
        // fight, so a stalled encounter never trips the stall watch - one run burned
        // 230 minutes that way - and it drops back to NOT_STARTED every time the party
        // wipes out of the instance, which reads as fresh progress and rearms the ask.
        // Damage dealt is the honest measure of a fight going somewhere.
        if (Creature* malygos = onMap->FindNearestCreature(kEoeMalygos, kEventObjectiveRange, true))
        {
            stage = 100 - std::min<uint32>(100, malygos->GetHealthPct());
            return instance->GetBossState(kEoeMalygosBoss) == IN_PROGRESS;
        }
        stage = 0;
        return false;
    }

    if (mapId == kMapCullingOfStratholme)
    {
        // One ladder from not-started to finished, climbed by the escort itself. Any
        // rung between the ends means Arthas is walking and waves are coming, which is
        // exactly what "running" means here. The rung number is also the stall signal:
        // a city that stops advancing has lost its escort.
        stage = instance->GetData(kCosArthasEvent);
        if (stage <= kCosNotStarted || stage >= kCosFinished)
            return false;
        // Walking counts as running; standing at the end of an act waiting to be asked
        // does not. Reporting those pauses as "live" is what left the escort parked at
        // stage 3 with the party watching: the ask only fires against a stopped event.
        if (Creature* arthas = onMap->FindNearestCreature(kCosArthas, kEventObjectiveRange, true))
            if (arthas->HasNpcFlag(UNIT_NPC_FLAG_GOSSIP) && CosActionFor(stage))
                return false;
        return true;
    }

    if (mapId == kMapTrialOfChampion)
    {
        // One counter that walks from nothing to finished, and it stops dead at three
        // places waiting to be asked for the next act. Those three, and the end, are
        // the only values that are not "a fight is under way".
        stage = instance->GetData(kTocInstanceProgress);
        return stage != kTocProgressInitial && stage != kTocProgressChampionsDead &&
               stage != kTocProgressChallengeDead && stage != kTocProgressFinished;
    }

    return false;
}

Creature* PartyAssembler::NearestSiegeVehicle(Player* leader) const
{
    Creature* best = nullptr;
    for (uint32 entry : kUldVehicles)
        if (Creature* found = leader->FindNearestCreature(entry, kUldVehicleRange, true))
            if (!best || leader->GetDistance(found) < leader->GetDistance(best))
                best = found;
    return best;
}

uint32 PartyAssembler::BossIndexFor(uint32 mapId, uint32 creditEntry) const
{
    auto const it = _bosses.find(mapId);
    if (it == _bosses.end())
        return kNoBossAim;
    for (uint32 i = 0; i < it->second.size(); ++i)
        if (it->second[i].creditEntry == creditEntry)
            return i;
    return kNoBossAim;
}

bool PartyAssembler::OculusDrakes(Player* leader, Trip& trip) const
{
    if (trip.dungeonMap != kMapOculus || leader->GetVehicle() || leader->IsInCombat())
        return false;

    // Drakes only once the first boss is down. Everything before him is on foot, and
    // the three after him are reached by flying between platforms - Drakos has died
    // once on this realm and nothing past him ever, because nobody could get there.
    if (!CountBits(trip.killMask))
        return false;

    // The essence is what a player uses; its spell is what actually summons and seats
    // the drake. Handed to the leader the way the module hands one to a member, then
    // cast the way the item would cast it. The module's own MountDrakeAction mounts
    // everyone else once its master - now the leader - is seen on a drake.
    if (!leader->HasItemCount(kOcRubyEssenceItem, 1))
        leader->AddItem(kOcRubyEssenceItem, 1);
    leader->CastSpell(leader, kOcRubyEssenceSpell, true);

    if (!trip.boarded)
    {
        trip.boarded = true;
        LOG_INFO("playerbots",
                 "Party assembler: {} takes a ruby drake in {} - the party can mount now "
                 "that someone went first",
                 leader->GetName(), trip.name);
    }
    return false;
}

bool PartyAssembler::BoardSiegeVehicles(Player* leader, Trip& trip, float& x, float& y,
                                        float& z) const
{
    if (trip.dungeonMap != kMapUlduar)
        return false;

    // The yard is finished with once the Leviathan is, and so is this.
    uint32 const fl = BossIndexFor(kMapUlduar, kUldFlameLeviathan);
    if (fl != kNoBossAim && BossIsDown(trip, fl))
        return false;

    // Seated: the steering drives the vehicle at the boss, the encounter tactics fight
    // from it, and the colossi that must die before he arms are met on the way.
    if (leader->GetVehicle())
        return false;

    Creature* veh = NearestSiegeVehicle(leader);
    if (!veh && !trip.vehiclesSummoned)
    {
        // This core never parks the salvaged vehicles in the yard on its own: the only
        // spawner is the Leviathan's reset after a first engage, so a raid that has not
        // fought him yet finds nothing to board. Ask the instance for the yard the way
        // his script does, at the start positions rather than the fight ones.
        if (InstanceScript* instance = leader->GetInstanceScript())
        {
            instance->SetData(kUldVehicleSpawn, kUldVehiclePosStart);
            trip.vehiclesSummoned = true;
            LOG_INFO("playerbots",
                     "Party assembler: the siege yard is summoned for {}'s raid in {}",
                     leader->GetName(), trip.name);
        }
    }
    if (!veh)
    {
        // Summoned but out of sight, or refused: walk to where the yard stands.
        x = kUldYardX;
        y = kUldYardY;
        z = kUldYardZ;
        return true;
    }

    // Far off: hand the position back as the destination and let the ordinary steering
    // walk them to the yard.
    if (leader->GetDistance(veh) > INTERACTION_DISTANCE)
    {
        x = veh->GetPositionX();
        y = veh->GetPositionY();
        z = veh->GetPositionZ();
        return true;
    }

    // Close enough to climb in. Boarded the way the module boards anything - through
    // the vehicle's own spell click rather than a raw seat assignment - so whatever the
    // vehicle wants to do to its passenger still happens.
    leader->GetMotionMaster()->Clear();
    leader->StopMoving();
    veh->HandleSpellClick(leader);

    if (leader->GetVehicle() && !trip.boarded)
    {
        trip.boarded = true;
        // The whole point of doing this for one character. Every other member's master
        // IS the leader, so a seated leader satisfies the module's own boarding trigger
        // and the raid mounts itself from here.
        LOG_INFO("playerbots",
                 "Party assembler: {} takes a salvaged vehicle in {} - the raid can "
                 "board now that someone went first",
                 leader->GetName(), trip.name);
    }
    return false;
}

void PartyAssembler::FocusEventTarget(Group* group, Player* onMap, Trip const& trip) const
{
    // Violet Hold is lost to arithmetic, not to tactics: 153 deaths against 43 kills in
    // two hours, stalling at wave one to three of twelve, while the venues that work
    // trade about one for one. The reason is that a portal keeps pouring adds out until
    // the thing channelling it dies, and a bot picks its own target by proximity - so
    // five bots fight five different adds while the keeper stands untouched and the
    // portal spawns five more. The module already reads the skull icon when it chooses
    // whom to attack (AttackersValue), and the Forge of Souls tactics set it the same
    // way, so naming the one target that ends the wave is all this needs to do.
    if (trip.dungeonMap != kMapVioletHold || !group || !onMap)
        return;

    Creature* focus = nullptr;
    auto pick = [&](uint32 entry) {
        if (focus)
            return;
        Creature* who = onMap->FindNearestCreature(entry, kEventObjectiveRange, true);
        if (who && onMap->IsValidAttackTarget(who))
            focus = who;
    };

    // The order the encounter is actually won in, highest first.
    pick(kVhCyanigosa);
    for (uint32 entry : kVhBosses)
        pick(entry);
    pick(kVhSaboteur);
    pick(kVhPortalGuardian);
    pick(kVhPortalKeeper1);
    pick(kVhPortalKeeper2);

    if (!focus)
        return;
    // Only when it changes: setting an icon broadcasts to the whole party every tick.
    if (group->GetTargetIcon(kSkullIcon) != focus->GetGUID())
        group->SetTargetIcon(kSkullIcon, onMap->GetGUID(), focus->GetGUID());
}

Player* PartyAssembler::PickWarden(Group* group, Trip& trip) const
{
    // The sitting holder keeps the role for as long as it can serve it. Stability is
    // most of the point: a role that changes hands every tick is not a role, and the
    // log could never say who had it.
    if (trip.warden)
    {
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(trip.warden);
        if (Player* held = ObjectAccessor::FindConnectedPlayer(guid))
            if (held->IsInWorld() && held->IsAlive() && !held->IsBeingTeleported() &&
                held->GetMapId() == trip.dungeonMap && group->IsMember(guid))
                return held;
    }

    for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
    {
        Player* m = mi->GetSource();
        if (!m || !m->IsInWorld() || !m->IsAlive() || m->IsBeingTeleported())
            continue;
        if (m->GetMapId() != trip.dungeonMap)
            continue;
        trip.warden = m->GetGUID().GetCounter();
        return m;
    }

    trip.warden = 0;
    return nullptr;
}

bool PartyAssembler::EventObjective(Player* onMap, Trip const& trip, float& x, float& y,
                                    float& z) const
{
    if (!onMap || onMap->GetMapId() != trip.dungeonMap)
        return false;

    if (trip.dungeonMap == kMapVioletHold)
    {
        // Highest thing on the board first. The order is the encounter's own: the
        // dragon that ends it, then a released cell boss, then the saboteur that
        // releases one, then whatever is holding the current portal open, then the
        // portal itself. Only the bosses need the attackable test - everything else
        // on this list exists only while it is in play - but it is applied to all of
        // them, because a target nobody in the party can hit is not a destination
        // whatever the reason.
        auto aimAt = [&](uint32 entry) {
            Creature* who = onMap->FindNearestCreature(entry, kEventObjectiveRange, true);
            if (!who || !onMap->IsValidAttackTarget(who))
                return false;
            x = who->GetPositionX();
            y = who->GetPositionY();
            z = who->GetPositionZ();
            return true;
        };

        if (aimAt(kVhCyanigosa))
            return true;
        for (uint32 entry : kVhBosses)
            if (aimAt(entry))
                return true;
        if (aimAt(kVhSaboteur) || aimAt(kVhPortalGuardian) || aimAt(kVhPortalKeeper1) ||
            aimAt(kVhPortalKeeper2) || aimAt(kVhPortal))
            return true;

        // Between waves, and while a wave is still walking. Standing where every route
        // ends is how this fight is held, and it is where the released boss is brought
        // to as well.
        x = kVhMusterX;
        y = kVhMusterY;
        z = kVhMusterZ;
        return true;
    }

    if (trip.dungeonMap == kMapEyeOfEternity)
    {
        // Malygos moves through all three phases - hovering, centred, and finally
        // fought from drake-back - so his live position is the only honest destination.
        // The spawn row would point at where he waits before anyone wakes him.
        if (Creature* malygos = onMap->FindNearestCreature(kEoeMalygos, kEventObjectiveRange, true))
        {
            x = malygos->GetPositionX();
            y = malygos->GetPositionY();
            z = malygos->GetPositionZ();
            return true;
        }
        return false;
    }

    if (trip.dungeonMap == kMapCullingOfStratholme)
    {
        // Follow Arthas, which is the entire tactic this instance has ever had. Not one
        // of its bosses is a spawn on the floor - Meathook, Salramm and Epoch are all
        // summoned as the escort reaches them, so the boss steering that carries every
        // other venue has literally nothing to aim at here and 50 runs walked the city
        // at random killing nothing. He is where the waves arrive and where the bosses
        // appear, so his feet are the only destination worth having.
        if (Creature* arthas = onMap->FindNearestCreature(kCosArthas, kEventObjectiveRange, true))
        {
            x = arthas->GetPositionX();
            y = arthas->GetPositionY();
            z = arthas->GetPositionZ();
            return true;
        }
        // Out of sight, which at the arrival point he always is. Head for where he
        // stands rather than giving up and wandering the city, which is exactly what
        // returning false here bought: runs that entered, never found him, never fought
        // anything, and timed out at zero deaths.
        x = kCosArthasX;
        y = kCosArthasY;
        z = kCosArthasZ;
        return true;
    }

    if (trip.dungeonMap == kMapTrialOfChampion)
    {
        // Nothing to hunt: every encounter here is summoned and ridden to the middle.
        // Standing there is the whole of the party's job.
        x = kTocMusterX;
        y = kTocMusterY;
        z = kTocMusterZ;
        return true;
    }

    // The Halls' waves walk to the party rather than the party to them, and the
    // arrival point is where they are walked to. Nothing better to say than "stand
    // where you came in", which is still a great deal better than walking off at a
    // boss that has not spawned.
    auto const inside = _insides.find(trip.dungeonMap);
    if (inside == _insides.end())
        return false;
    x = inside->second.x;
    y = inside->second.y;
    z = inside->second.z;
    return true;
}

void PartyAssembler::HoldLeader(Player* leader, Trip& trip, bool hold)
{
    PlayerbotAI* ai = GET_PLAYERBOT_AI(leader);
    if (!ai)
        return;

    uint32 const low = leader->GetGUID().GetCounter();
    // Asked rather than asserted. Adding or removing a strategy rebuilds the
    // engine's whole trigger and action list, and the answer is unchanged on the
    // overwhelming majority of ticks - a run holds through one fight and walks
    // through the rest. The state is also read back from the engine rather than
    // from the trip's own flag, because a death restores the free-bot defaults and
    // hands the leader its rpg engine back without this object hearing about it.
    bool const held = ai->HasStrategy("stay", BOT_STATE_NON_COMBAT) &&
                      !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT);
    if (hold == held)
    {
        trip.parked = hold;
        if (hold)
            _parked.insert(low);
        else
            _parked.erase(low);
        return;
    }

    // "stay" is what refuses the grind target - AttackAnythingAction asks for it by
    // name and stands down - so this is the blind pull switched off. Taking the rpg
    // strategy off the engine is the other half: without it the leader keeps walking
    // the destination it was last given, every bot tick, for as long as the party
    // behind it is still fighting.
    // Movement strategies are siblings, so adding "stay" also drops "follow" from
    // the leader's non-combat engine and removing it does not put it back. That is
    // the right way round here - the leader is steered by destination and has no
    // master to follow, which is why the discipline pass gives "follow" to everyone
    // EXCEPT the leader - and a bot that later joins a party as a member is reset
    // and re-given it there.
    ai->ChangeStrategy(hold ? "+stay,-new rpg" : "-stay,+new rpg", BOT_STATE_NON_COMBAT);
    trip.parked = hold;
    if (hold)
        _parked.insert(low);
    else
        _parked.erase(low);
}

void PartyAssembler::ReleaseParked()
{
    if (_parked.empty())
        return;

    std::unordered_set<uint32> stillHeld;
    for (auto const& entry : _trips)
        if (entry.second.parked)
            if (Group* group = sGroupMgr->GetGroupByGUID(entry.first))
                stillHeld.insert(group->GetLeaderGUID().GetCounter());

    for (auto it = _parked.begin(); it != _parked.end();)
    {
        if (stillHeld.count(*it))
        {
            ++it;
            continue;
        }
        ObjectGuid const guid = ObjectGuid::Create<HighGuid::Player>(*it);
        if (Player* who = ObjectAccessor::FindConnectedPlayer(guid))
            if (PlayerbotAI* ai = GET_PLAYERBOT_AI(who))
            {
                // The rpg engine only ever belongs to a bot that is free or leads
                // its own group - AiFactory hands it out on exactly that condition -
                // and a released leader may have been picked up by a party in the
                // meantime, where the follow strategy is what it should be running.
                // The hold on the grind target comes off either way.
                Group* const now = who->GetGroup();
                bool const ownsItself =
                    !now || now->GetLeaderGUID() == who->GetGUID();
                if (ai->HasStrategy("stay", BOT_STATE_NON_COMBAT) ||
                    (ownsItself && !ai->HasStrategy("new rpg", BOT_STATE_NON_COMBAT)))
                    ai->ChangeStrategy(ownsItself ? "-stay,+new rpg" : "-stay",
                                       BOT_STATE_NON_COMBAT);
            }
        it = _parked.erase(it);
    }
}

bool PartyAssembler::NeedsBreather(Group* group, Player* leader) const
{
    for (GroupReference* mi = group->GetFirstMember(); mi != nullptr; mi = mi->next())
    {
        Player* m = mi->GetSource();
        if (!m || !m->IsInWorld() || !m->IsAlive())
            continue;
        // Only the ones actually here. A member on another map is the recall's
        // problem, and waiting on it would hold the run for the whole clock.
        if (m->GetMapId() != leader->GetMapId())
            continue;
        if (m != leader && m->GetDistance(leader) > _regroupRange)
            return true;
        if (m->GetHealthPct() < kBreatherHealthPct)
            return true;
        if (PlayerbotAI::IsHeal(m) && m->getPowerType() == POWER_MANA &&
            m->GetPowerPct(POWER_MANA) < kBreatherManaPct)
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
// floor the full knob applies; the chance ramps linearly to zero GearStretch item
// levels below it. The party's AVERAGE decides, so one green member drags the
// odds instead of vetoing the run, and an all-green party still stays home.
// Shares the ramp with JudgeGear rather than repeating the number, so "slightly
// under still marches" means one thing everywhere in this file.
uint32 PartyAssembler::GearScaledPct(Group* group, uint32 mapId, uint8 difficulty,
                                     uint32 fullPct) const
{
    GearVerdict const gear = JudgeGear(group, mapId, difficulty);
    if (!gear.floor)
        return fullPct;
    return fullPct * gear.weight / 100;
}

// One place that answers "can we do this", so the log line, the pick and the ledger
// row cannot disagree. Deliberately the middle path the operator asked for: a door
// whose floor the party clears is fully wanted, a door slightly above it is wanted
// less the further above it stands, and a door far above is named as out of reach
// rather than walked to and refused. Nobody is judged member by member - the party
// average decides, so one member in greens costs the group odds rather than a veto.
PartyAssembler::GearVerdict PartyAssembler::JudgeGear(Group* group, uint32 mapId,
                                                      uint8 difficulty) const
{
    GearVerdict out;
    uint16 floor = 0;
    if (auto const it = _ilvlFloor.find((mapId << 8) | difficulty); it != _ilvlFloor.end())
        floor = it->second;
    // Nothing for this mode. Fall back to the lowest floor the venue declares for any
    // of them rather than calling a Wrath five-man unmeasured, which is what left
    // every normal-difficulty dungeon handed out with no feasibility check at all -
    // the realm writes a heroic floor for those maps and no normal one. The borrowed
    // number is not a guess: measured across 570 Wrath five-man runs on this realm,
    // parties under it killed something in 3.7% of runs against 23.7% at or above it,
    // and wiped 0.54 times a run against 0.19. Classic and Burning Crusade dungeons
    // declare no floor in any mode and stay unmeasured; 69 runs there is not a sample
    // that can name one.
    if (!floor)
        if (auto const any = _mapFloorAny.find(mapId); any != _mapFloorAny.end())
            floor = any->second;
    if (!floor)
        return out;   // the realm never said; never a reason to refuse

    float const avg = PartyAvgIlvl(group);
    out.floor = floor;
    out.margin = int16(int32(avg) - int32(floor));

    if (out.margin >= 0)
    {
        out.band = "within reach";
        out.weight = 100;
        return out;
    }

    uint32 const under = uint32(-out.margin);
    if (under >= _gearStretch)
    {
        out.band = "beyond us";
        out.weight = 0;
        return out;
    }

    out.band = "a stretch";
    // Same ramp the heroic appetite has always used, so "slightly under still
    // marches" means the same thing everywhere in this file.
    out.weight = std::max<uint32>(1, 100 * (_gearStretch - under) / _gearStretch);
    return out;
}

uint32 PartyAssembler::CountBits(uint32 mask)
{
    uint32 n = 0;
    for (; mask; mask &= mask - 1)
        ++n;
    return n;
}

namespace
{
    // The core credits a kill against sObjectMgr's encounter list for exactly this
    // (map, difficulty), so the same list is the only honest answer to "which bit is
    // which boss" and "how many are there". Icecrown and the Ruby Sanctum carry no
    // heroic list of their own - Map::UpdateEncounterState reads their normal one on
    // heroic - so a missing list falls back across the modes rather than reporting a
    // venue with no bosses in it.
    DungeonEncounterList const* EncountersFor(uint32 mapId, uint8 difficulty)
    {
        if (DungeonEncounterList const* list =
                sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(difficulty)))
            if (!list->empty())
                return list;

        DungeonEncounterList const* best = nullptr;
        for (uint8 d = 0; d < MAX_DIFFICULTY; ++d)
            if (DungeonEncounterList const* list =
                    sObjectMgr->GetDungeonEncounterList(mapId, Difficulty(d)))
                if (!list->empty() && (!best || list->size() > best->size()))
                    best = list;
        return best;
    }

    // Which bit of the completed-encounter mask this encounter owns.
    uint32 EncounterBit(DungeonEncounter const* encounter)
    {
        uint32 const index = encounter->dbcEntry->encounterIndex;
        return index < 32 ? (1u << index) : 0u;
    }
}

uint32 PartyAssembler::EncounterCount(uint32 mapId, uint8 difficulty)
{
    DungeonEncounterList const* list = EncountersFor(mapId, difficulty);
    return list ? uint32(list->size()) : 0u;
}

bool PartyAssembler::BossIsDown(Trip const& trip, uint32 index) const
{
    auto const spots = _bosses.find(trip.dungeonMap);
    if (spots == _bosses.end() || index >= spots->second.size())
        return false;

    DungeonEncounterList const* encounters = EncountersFor(trip.dungeonMap, trip.difficulty);
    if (!encounters)
        return false;

    for (DungeonEncounter const* encounter : *encounters)
        if (encounter->creditEntry == spots->second[index].creditEntry)
            return (EncounterBit(encounter) & trip.killMask) != 0;
    return false;
}

void PartyAssembler::NoteKills(Group* group, Player* leader, Trip& trip)
{
    if (leader->GetMapId() != trip.dungeonMap)
        return;

    // Only an instance with a script of its own keeps a mask. Everything else falls
    // back to the behaviour that was here before: walk until the clock stops.
    InstanceScript* instance = leader->GetInstanceScript();
    if (!instance)
        return;

    DungeonEncounterList const* encounters = EncountersFor(trip.dungeonMap, trip.difficulty);
    uint32 const live = instance->GetCompletedEncounterMask();

    if (!trip.maskSeeded)
    {
        trip.maskSeeded = true;
        // A raid holds its lockout for days, so a party can walk into a wing somebody
        // else already spent. Whatever was down before they arrived is theirs to walk
        // past, not theirs to be credited with.
        trip.entryMask = live;
        trip.killMask = live;
        trip.encounters = EncounterCount(trip.dungeonMap, trip.difficulty);
        if (trip.runId)
            CharacterDatabase.Execute(
                "UPDATE aetherion_run_history SET encounters = {}, bosses_at_entry = {}"
                " WHERE id = {}",
                trip.encounters, CountBits(trip.entryMask), trip.runId);
        if (trip.entryMask)
            LOG_INFO("playerbots",
                     "Party assembler: {} take up a lockout in {} with {} of {} already "
                     "down - they pick up where it was left",
                     leader->GetName(), trip.name, CountBits(trip.entryMask),
                     trip.encounters);
    }

    uint32 const gained = live & ~trip.killMask;
    trip.killMask |= live;

    // Boss positions the mask says are finished with. Done on every pass rather than
    // only when something dies, so a lockout inherited at the door is walked past on
    // the first tick inside instead of one corpse at a time.
    if (encounters)
        if (auto const spotsIt = _bosses.find(trip.dungeonMap); spotsIt != _bosses.end())
            for (uint32 i = 0; i < spotsIt->second.size(); ++i)
            {
                if (trip.visitedBosses.count(i))
                    continue;
                for (DungeonEncounter const* encounter : *encounters)
                    if (encounter->creditEntry == spotsIt->second[i].creditEntry &&
                        (EncounterBit(encounter) & trip.killMask))
                    {
                        trip.visitedBosses.insert(i);
                        break;
                    }
            }

    if (!gained)
        return;

    trip.bossesDown = CountBits(trip.killMask & ~trip.entryMask);
    trip.idleTicks = 0;

    // Stamped now, while the instance still exists. The old count was read once at
    // EndRun from the `instance` table, and that table is live state pruned on reset -
    // a run whose instance had gone recorded zero however deep it got.
    if (trip.runId)
        CharacterDatabase.Execute(
            "UPDATE aetherion_run_history SET bosses_downed = GREATEST(bosses_downed, {})"
            " WHERE id = {}",
            trip.bossesDown, trip.runId);

    // Named, durably, for every expansion. The core's own encounter log returns early
    // for anything that is not a WotLK raid, so nothing before Naxxramas has ever been
    // nameable once its instance reset.
    if (encounters)
        for (DungeonEncounter const* encounter : *encounters)
        {
            uint32 const bit = EncounterBit(encounter);
            if (!(bit & gained))
                continue;
            char const* who = encounter->dbcEntry->encounterName[0];
            LOG_INFO("playerbots", "Party assembler: {} falls to {}'s party in {} ({} of {})",
                     who ? who : "something", leader->GetName(), trip.name,
                     trip.bossesDown, trip.encounters);
            if (trip.runId)
                CharacterDatabase.Execute(
                    "INSERT INTO aetherion_run_kills (run_id, at, map, difficulty,"
                    " encounter_index, name, party_size) VALUES ({}, UNIX_TIMESTAMP(),"
                    " {}, {}, {}, '{}', {})",
                    trip.runId, trip.dungeonMap, uint32(trip.difficulty),
                    encounter->dbcEntry->encounterIndex,
                    Sql(who ? who : "unnamed"), group->GetMembersCount());
        }

    // Determination, earned. A run that is still killing things is not shown the door
    // mid-progress; a run that is killing nothing gets exactly the clock it always had.
    // Bounded, because a bigger flat budget was measured to buy oscillation rather than
    // depth - this only pays out against evidence.
    if (_progressExtendTicks && trip.extensions < _progressExtendMax)
    {
        ++trip.extensions;
        trip.bonusTicks += _progressExtendTicks;
        if (trip.runId)
            CharacterDatabase.Execute(
                "UPDATE aetherion_run_history SET extensions = {} WHERE id = {}",
                trip.extensions, trip.runId);
    }
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

    // Closest-first, then a pick among the nearest few: keeps some variety without
    // sending a party to the far side of the continent.
    std::sort(options.begin(), options.end(), [leader](Option const& a, Option const& b) {
        return PlanarDistance(leader, a.where) < PlanarDistance(leader, b.where);
    });
    size_t const shortlist = std::min<size_t>(options.size(), _nearestChoices);

    // The same question a raid has always been asked, now asked of a dungeon too.
    // Every dungeon row in the ledger reads 'unmeasured' because the judgement was
    // only ever wired into the raid path, which is how parties at 147 item levels kept
    // being handed Drak'Tharon Keep: 80 such runs killed something three times between
    // them. Deliberately the middle the operator asked for and not a veto - a door
    // slightly above the party is wanted less rather than refused, and a party with
    // nothing within reach still goes somewhere.
    std::vector<uint32> weights(shortlist, 100);
    uint32 total = 0;
    for (size_t i = 0; i < shortlist; ++i)
    {
        GearVerdict const v = JudgeGear(group, options[i].dungeonMap,
                                        uint8(DUNGEON_DIFFICULTY_NORMAL));
        weights[i] = v.weight;
        total += v.weight;
    }

    Option chosen = options[0];
    if (total)
    {
        uint32 roll = urand(1, total);
        for (size_t i = 0; i < shortlist; ++i)
        {
            if (roll <= weights[i])
            {
                chosen = options[i];
                break;
            }
            roll -= weights[i];
        }
    }
    else
    {
        // Nothing nearby this party is dressed for. Old content is the honest answer
        // before a doomed run is: a party that outgrew Zul'Farrak clears it, and the
        // collector path already knows how to get there. It refuses parties under
        // level seventy and parties with no old doorway in reach, and then this falls
        // back to the least hopeless of the near ones rather than forming a party that
        // never leaves - a run that gets nowhere is still worth more than a group that
        // sits in the cap doing nothing.
        if (SendPartyToOldContent(group, leader))
            return true;

        ++_statGearRefused;
        int16 best = std::numeric_limits<int16>::min();
        for (size_t i = 0; i < shortlist; ++i)
        {
            GearVerdict const v = JudgeGear(group, options[i].dungeonMap,
                                            uint8(DUNGEON_DIFFICULTY_NORMAL));
            if (v.margin > best)
            {
                best = v.margin;
                chosen = options[i];
            }
        }
        LOG_INFO("playerbots",
                 "Party assembler: {}'s party is dressed for none of the doors near "
                 "them - {} is the least of it ({} short)",
                 leader->GetName(), chosen.name, uint32(-best));
    }

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
    GearVerdict const gear = JudgeGear(group, chosen.dungeonMap, uint8(dungeonDiff));
    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{chosen.dungeonMap, chosen.where, chosen.name, Phase::Travelling, how,
             start.place, start.actor, 0};
    trip.difficulty = uint8(dungeonDiff);
    trip.runId = RecordRunStart(group, leader, chosen.name, chosen.dungeonMap, false,
                                uint8(dungeonDiff), how, uint32(away), gear, gear.floor);

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
    // Every bot that could take point: in this group, in this instance, ours to
    // steer. Gathered before the crown is decided because the same list answers both
    // questions - who may lead at all, and which of them can hold a pull.
    std::vector<Player*> eligible;
    for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        if (Player* m = ref->GetSource())
            if (GET_PLAYERBOT_AI(m) && m->IsInWorld() && m->GetMapId() == mapId)
                eligible.push_back(m);

    Player* tank = nullptr;
    for (Player* m : eligible)
        if (PlayerbotAI::IsTank(m))
        {
            tank = m;
            break;
        }

    if (!GET_PLAYERBOT_AI(leader))
    {
        // The bot the player named wins when it can tank; otherwise a tank from the
        // group, and only then the named bot. "Lead us through" is a request for
        // somebody to walk in front, and what walks in front should be what can
        // hold what is standing there.
        Player* const named =
            claimedBot && claimedBot->GetGroup() == group && claimedBot->GetMapId() == mapId
                ? claimedBot : nullptr;
        Player* promote = named && PlayerbotAI::IsTank(named) ? named : tank;
        if (!promote)
            promote = named;
        if (!promote && !eligible.empty())
            promote = eligible.front();
        if (!promote)
            return false;
        group->ChangeLeader(promote->GetGUID());
        leader = promote;
    }
    else if (tank && tank != leader && !PlayerbotAI::IsTank(leader))
    {
        // A bot already leads, but not one that can take the hit. Same handover,
        // bot to bot, and only where there is a tank to hand it to.
        group->ChangeLeader(tank->GetGUID());
        leader = tank;
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

    uint8 const adoptedDiff = uint8(requester->GetDifficulty(map->IsRaid()));
    GearVerdict const gear = JudgeGear(group, mapId, adoptedDiff);
    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{mapId, Entrance{}, map->GetMapName(), Phase::Inside, Travel::Foot,
             map->GetMapName(), leader->GetName(), 0};
    trip.adopted = true;
    trip.difficulty = adoptedDiff;
    trip.runId = RecordRunStart(group, leader, trip.name, mapId, group->isRaidGroup(),
                                adoptedDiff, Travel::Foot, 0, gear, gear.floor);

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
                                      uint32 startYards, GearVerdict const& gear,
                                      uint16 gearCeiling)
{
    uint32 const id = ++_runSeq;
    CharacterDatabase.Execute(
        "INSERT INTO aetherion_run_history (id, group_id, started_at, dungeon, map, is_raid,"
        " difficulty, size, leader, leader_class, avg_ilvl, via, start_yards,"
        " ilvl_floor, gear_margin, gear_verdict, gear_ceiling, encounters)"
        " VALUES ({}, {}, UNIX_TIMESTAMP(), '{}', {}, {}, {}, {}, '{}', {}, {}, '{}', {},"
        " {}, {}, '{}', {}, {})",
        id, group->GetGUID().GetCounter(), Sql(name), mapId, isRaid ? 1 : 0, uint32(difficulty),
        group->GetMembersCount(), Sql(leader->GetName()), uint32(leader->getClass()),
        uint32(PartyAvgIlvl(group)), TravelName(how), startYards,
        gear.floor, int32(gear.margin), gear.band, gearCeiling,
        EncounterCount(mapId, difficulty));

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
    // Depth is stamped at kill time now, from the instance's own mask while the
    // instance still exists - see NoteKills. This snapshot from the members' saves
    // stays as a floor under it, for runs whose leader never had a script to read
    // (adopted or upstream groups), and can only ever raise the number: the `instance`
    // table is live state pruned on reset, so a run whose instance had gone used to
    // overwrite a real count with zero.
    CharacterDatabase.Execute(
        "UPDATE aetherion_run_history SET ended_at = UNIX_TIMESTAMP(), outcome = '{}',"
        " bosses_downed = IF(bosses_at_entry > 0, bosses_downed,"
        "   GREATEST(bosses_downed,"
        "     COALESCE((SELECT MAX(BIT_COUNT(i.completedEncounters))"
        "     FROM aetherion_run_members m"
        "     JOIN character_instance ci ON ci.guid = m.guid"
        "     JOIN instance i ON i.id = ci.instance AND i.map = {}"
        "    WHERE m.run_id = {}), 0)))"
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
    // Who the gear experiment outfitted, and when. Durable because the analysis
    // needs to tell a cohort run from a control run long after the fact, and
    // because reading it back is what stops a restart re-rolling the same bots.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_geartest ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " name VARCHAR(24) NOT NULL,"
        " at INT UNSIGNED NOT NULL,"
        " ilvl SMALLINT UNSIGNED NOT NULL)");
    if (QueryResult geared = CharacterDatabase.Query("SELECT guid FROM aetherion_geartest"))
        do
            _gearTested.insert((*geared)[0].Get<uint32>());
        while (geared->NextRow());

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

    // The gear judgement, recorded so the dashboard can say WHY a party went where it
    // went: what the door asked, how far over or under the party's average stood, the
    // word for it, and the deepest floor anything open to them asked. Without these
    // the ledger holds an item level with nothing to compare it against.
    if (!CharacterDatabase.Query(
            "SHOW COLUMNS FROM aetherion_run_history LIKE 'ilvl_floor'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN ilvl_floor SMALLINT UNSIGNED NOT NULL DEFAULT 0,"
            " ADD COLUMN gear_margin SMALLINT NOT NULL DEFAULT 0,"
            " ADD COLUMN gear_verdict VARCHAR(12) NOT NULL DEFAULT '',"
            " ADD COLUMN gear_ceiling SMALLINT UNSIGNED NOT NULL DEFAULT 0");

    // Depth, told truthfully. encounters is how many the venue holds, bosses_at_entry
    // how many were already down when the party walked in - a raid lockout survives
    // for days, so a run can inherit progress it did not make - and extensions how
    // many times its own kills bought it more clock.
    if (!CharacterDatabase.Query(
            "SHOW COLUMNS FROM aetherion_run_history LIKE 'encounters'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN encounters TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            " ADD COLUMN bosses_at_entry TINYINT UNSIGNED NOT NULL DEFAULT 0,"
            " ADD COLUMN extensions TINYINT UNSIGNED NOT NULL DEFAULT 0");

    // What the run cost in bodies. The wipe count only ever named the falls that
    // took everybody, so a party losing one member per pull and picking it back up
    // read as identically clean as one that lost nobody - which is the exact
    // difference pacing is supposed to move, and it was not being recorded.
    if (!CharacterDatabase.Query("SHOW COLUMNS FROM aetherion_run_history LIKE 'deaths'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_run_history"
            " ADD COLUMN deaths SMALLINT UNSIGNED NOT NULL DEFAULT 0");

    // Which bosses actually died, stamped as they fall. The core's own encounter log
    // is durable but holds ONLY WotLK raids - Map::LogEncounterFinished returns early
    // for anything else - and the `instance` table that could name the rest is pruned
    // on reset. So nothing before Naxxramas has ever been nameable after the fact.
    // This is read from the instance's live mask while the party is standing in it,
    // which works for every expansion and every difficulty.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_run_kills ("
        " id INT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " run_id INT UNSIGNED NOT NULL, at INT UNSIGNED NOT NULL,"
        " map INT UNSIGNED NOT NULL, difficulty TINYINT UNSIGNED NOT NULL,"
        " encounter_index TINYINT UNSIGNED NOT NULL,"
        " name VARCHAR(64) NOT NULL, party_size TINYINT UNSIGNED NOT NULL DEFAULT 0,"
        " KEY at (at), KEY run_id (run_id), KEY map (map)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // Every run this object was steering when the process last stopped is still marked
    // underway and always will be - nothing outlives the restart to close it. Measured
    // on three days of history: 1079 rows stuck underway against 535 that ended, which
    // makes every rate computed off this table wrong. They are closed here, named for
    // what actually happened to them. ended_at is deliberately left at zero: nothing
    // observed these runs ending, and inventing a time for them would put a fictional
    // duration into the same column the real ones live in.
    CharacterDatabase.DirectExecute(
        "UPDATE aetherion_run_history SET outcome = 'interrupted' WHERE outcome = 'underway'");

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

    // Two weeks of history is the analysis window; members and kills prune with runs.
    CharacterDatabase.Execute(
        "DELETE m FROM aetherion_run_members m JOIN aetherion_run_history h"
        " ON h.id = m.run_id WHERE h.started_at < UNIX_TIMESTAMP() - 14*86400");
    CharacterDatabase.Execute(
        "DELETE FROM aetherion_run_kills WHERE at < UNIX_TIMESTAMP() - 14*86400");
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

    // Two ways a forming raid says no to a destination, counted apart: gear_refused is
    // "not yet", unperformable is "not by a party of bots at all".
    if (!CharacterDatabase.Query(
            "SHOW COLUMNS FROM aetherion_assembler LIKE 'gear_refused'"))
        CharacterDatabase.DirectExecute(
            "ALTER TABLE aetherion_assembler"
            " ADD COLUMN gear_refused INT UNSIGNED NOT NULL DEFAULT 0,"
            " ADD COLUMN unperformable INT UNSIGNED NOT NULL DEFAULT 0");

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
        "entered, active, entrances, arrival_points, raid_maps, updated_at, "
        "gear_refused, unperformable) "
        "VALUES (1, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, UNIX_TIMESTAMP(), {}, {}) "
        "ON DUPLICATE KEY UPDATE formed=VALUES(formed), raids=VALUES(raids), "
        "trips=VALUES(trips), stalls=VALUES(stalls), arrived=VALUES(arrived), "
        "entered=VALUES(entered), active=VALUES(active), entrances=VALUES(entrances), "
        "arrival_points=VALUES(arrival_points), raid_maps=VALUES(raid_maps), "
        "updated_at=VALUES(updated_at), gear_refused=VALUES(gear_refused), "
        "unperformable=VALUES(unperformable)",
        _statFormed, _statRaids, _statTrips, _statStalls, _statArrived, _statEntered,
        uint32(_assembled.size()), uint32(_entrances.size()), uint32(_insides.size()),
        _raidMapCount, _statGearRefused, _statUnperformable);
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
        // Same answer the trip check gives, asked at formation time: a raid raised
        // for a venue its own bots cannot perform would just dissolve on the doorstep.
        if (CannotPerform(entry.first))
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

    uint8 const oldDiff = uint8(chosen.raid ? RAID_DIFFICULTY_10MAN_NORMAL
                                            : DUNGEON_DIFFICULTY_NORMAL);
    // Recorded, never consulted: being massively over-geared is the whole point of a
    // collector's night out, so the verdict here is a fact about the trip rather than
    // a test it had to pass.
    GearVerdict const gear = JudgeGear(group, chosen.mapId, oldDiff);
    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{chosen.mapId, chosen.where, chosen.name, Phase::Travelling, start.how,
             start.place, start.actor, 0};
    trip.collector = true;
    trip.difficulty = oldDiff;
    trip.runId = RecordRunStart(group, leader, chosen.name, chosen.mapId, chosen.raid,
                                oldDiff, start.how, uint32(away), gear, gear.floor);
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

    struct Option
    {
        Entrance where;
        std::string name;
        uint32 raidMap;
        Difficulty diff;
        GearVerdict gear;
    };
    std::vector<Option> options;

    // What the party's gear says about the whole shelf, not just about where it ends
    // up going. The ceiling is the deepest door it could plausibly stand in; the
    // refused count and the name of the thing just out of reach are what make "we
    // cannot do that yet" visible instead of silent.
    uint16 ceiling = 0;
    uint16 outOfReachFloor = 0;
    std::string outOfReachName;
    uint32 refusedOnGear = 0;
    uint32 refusedOnCapability = 0;

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

        // Some doors are not shut by gear or by level but by what a party of bots can
        // physically do once it is inside. Asked before anything else, because no
        // amount of clock or determination changes the answer.
        if (char const* why = CannotPerform(raidMap))
        {
            if (!refusedOnCapability++)
                LOG_INFO("playerbots",
                         "Party assembler: {} passes over {} - {}",
                         leader->GetName(), map->name[0] ? map->name[0] : "a raid", why);
            continue;
        }

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

        // Gear decides, out loud. A door the party clears is fully wanted; one a
        // little above it is wanted less the further above it stands; one far above
        // is named as out of reach rather than walked to and turned away. The party
        // average is what is judged, so a single member in greens costs the group
        // odds instead of vetoing the destination.
        GearVerdict gear = JudgeGear(group, raidMap, uint8(diff));
        if (!gear.weight)
        {
            ++refusedOnGear;
            if (gear.floor > outOfReachFloor)
            {
                outOfReachFloor = gear.floor;
                outOfReachName = map->name[0] ? map->name[0] : "a raid";
            }
            continue;
        }
        ceiling = std::max(ceiling, gear.floor);

        Difficulty const heroic =
            size > 10 ? RAID_DIFFICULTY_25MAN_HEROIC : RAID_DIFFICULTY_10MAN_HEROIC;
        if (_raidHeroicPct && GetMapDifficultyData(raidMap, heroic) &&
            urand(1, 100) <=
                GearScaledPct(group, raidMap, uint8(heroic), _raidHeroicPct) &&
            allSatisfy(raidMap, heroic))
        {
            diff = heroic;
            // The harder mode asks for more, so the verdict is re-taken against the
            // door they are actually going to knock on.
            GearVerdict const harder = JudgeGear(group, raidMap, uint8(diff));
            if (harder.weight)
            {
                gear = harder;
                ceiling = std::max(ceiling, gear.floor);
            }
        }

        if (!allSatisfy(raidMap, diff))
            continue;

        options.push_back(
            {door, map->name[0] ? map->name[0] : "a raid", raidMap, diff, gear});
    }

    _statGearRefused += refusedOnGear;
    _statUnperformable += refusedOnCapability;

    if (options.empty())
    {
        // Only worth saying when something was actually turned down. A continent with
        // no raid door at this party's level is an ordinary miss, not a judgement.
        if (refusedOnGear)
            LOG_INFO("playerbots",
                     "Party assembler: {}'s raid averages {} item levels - {} raids are "
                     "out of reach and nothing else will take them; the closest, {}, "
                     "asks {}",
                     leader->GetName(), uint32(PartyAvgIlvl(group)), refusedOnGear,
                     outOfReachName.empty() ? "nothing" : outOfReachName.c_str(),
                     outOfReachFloor);
        return false;
    }

    // Closest-first, then a pick among the nearest few - but weighted by what the
    // gear says rather than uniform. A door the party clears outright is the likely
    // night out; one it is stretching for still comes up, less often the further it
    // reaches. That is the middle path: neither all-greens suicide runs nor a
    // precise-gear veto.
    std::sort(options.begin(), options.end(), [leader](Option const& a, Option const& b) {
        return PlanarDistance(leader, a.where) < PlanarDistance(leader, b.where);
    });
    size_t const shortlist = std::min<size_t>(options.size(), _nearestChoices);
    uint32 total = 0;
    for (size_t i = 0; i < shortlist; ++i)
        total += options[i].gear.weight;
    size_t pick = 0;
    if (total)
    {
        uint32 roll = urand(1, total);
        for (; pick + 1 < shortlist; ++pick)
        {
            if (roll <= options[pick].gear.weight)
                break;
            roll -= options[pick].gear.weight;
        }
    }
    else
        pick = urand(0, shortlist - 1);
    Option const& chosen = options[pick];

    if (!GET_PLAYERBOT_AI(leader))
        return false;

    // Direct call, not the client opcode: the handler refuses changes once
    // anyone stands on a dungeon map, and every member here is outdoors at
    // trip start. Propagates to all members and persists.
    group->SetRaidDifficulty(chosen.diff);

    float const away = PlanarDistance(leader, chosen.where);
    Departure const start = BeginTravel(group, leader, chosen.where);
    Travel const how = start.how;

    // The judgement, said out loud, so the dashboard and the log agree on WHY this
    // party went where it went - and on what it decided it could not do yet.
    LOG_INFO("playerbots",
             "Party assembler: {} leads a {}-strong raid to {} ({:.0f} yards, {}, "
             "difficulty {}) - {} item levels against a floor of {}, {}; {} raids ruled "
             "out on gear, deepest door open to them asks {}",
             leader->GetName(), group->GetMembersCount(), chosen.name, away, TravelName(how),
             uint32(chosen.diff), uint32(PartyAvgIlvl(group)), chosen.gear.floor,
             chosen.gear.band, refusedOnGear, ceiling);

    Trip& trip = _trips[group->GetGUID().GetCounter()] =
        Trip{chosen.raidMap, chosen.where, chosen.name, Phase::Travelling, how,
             start.place, start.actor, 0};
    trip.difficulty = uint8(chosen.diff);
    trip.runId = RecordRunStart(group, leader, chosen.name, chosen.raidMap, true,
                                uint8(chosen.diff), how, uint32(away), chosen.gear,
                                ceiling);
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

Player* PartyAssembler::PickLeader(std::vector<Player*> const& from)
{
    if (from.empty())
        return nullptr;

    std::vector<Player*> tanks;
    for (Player* bot : from)
        if (PlayerbotAI::IsTank(bot))
            tanks.push_back(bot);

    std::vector<Player*> const& draw = tanks.empty() ? from : tanks;
    return draw[urand(0, draw.size() - 1)];
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
            musterLeader = PickLeader(doorled.empty() ? pool : doorled);
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

    // Leader first, then everyone compatible with the leader. A tank where the pool
    // holds one, and uniformly among the tanks otherwise, which keeps parties spread
    // across the level brackets instead of always forming at the cap. Measured on
    // 398 five-man runs that set out with both a tank and a healer aboard: 45.5
    // percent of the 66 that happened to draw a tank leader put a boss down against
    // 27.9 percent of the 298 led by damage, and the tank-led parties were the
    // POORER geared of the two (161 average item level against 178), so the
    // difference is not the gear. Healer-led was the floor at 14.7 percent of 34 -
    // which is the same finding read from the other end, because whoever leads is
    // whoever walks into the pack first.
    Player* leader = musterLeader ? musterLeader : PickLeader(pool);

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

void PartyAssembler::ReapOrphanGroups()
{
    // Collect the distinct groups the bot population is standing in. GroupStore
    // is protected, and walking the fleet is the pattern this file already uses.
    std::unordered_map<uint32, Group*> seen;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld())
            continue;
        if (Group* group = bot->GetGroup())
            seen.emplace(group->GetGUID().GetCounter(), group);
    }

    uint32 reaped = 0;
    for (auto const& [low, group] : seen)
    {
        // A group on a live run is exactly what this must never touch.
        if (_trips.count(low))
        {
            _orphanStrikes.erase(low);
            continue;
        }

        // Conservative membership test: every slot must resolve to an ONLINE
        // bot standing in the open world. An offline slot could be a human, a
        // member inside an instance or battleground is mid-something, and a
        // human member makes the group theirs, not ours. Any doubt spares it.
        bool reapable = true;
        for (Group::MemberSlotList::const_iterator ms = group->GetMemberSlots().begin();
             ms != group->GetMemberSlots().end(); ++ms)
        {
            Player* member = ObjectAccessor::FindConnectedPlayer(ms->guid);
            if (!member || !member->IsInWorld() || !GET_PLAYERBOT_AI(member) ||
                member->GetMap()->IsDungeon() || member->GetMap()->IsBattlegroundOrArena())
            {
                reapable = false;
                break;
            }
        }
        if (!reapable)
        {
            _orphanStrikes.erase(low);
            continue;
        }

        // Ten consecutive tripless sightings before the axe: a party that is
        // forming, mustering or between decisions gets minutes of grace, while
        // a zombie from a restart gets collected within the quarter hour.
        if (++_orphanStrikes[low] < 10)
            continue;

        _orphanStrikes.erase(low);
        group->Disband();
        ++reaped;
    }

    // Strikes for groups that vanished on their own.
    for (auto it = _orphanStrikes.begin(); it != _orphanStrikes.end();)
        it = seen.count(it->first) ? std::next(it) : _orphanStrikes.erase(it);

    if (reaped)
        LOG_INFO("playerbots",
                 "Party assembler: reaped {} orphaned groups - their bots rejoin the pool",
                 reaped);
}

void PartyAssembler::Tick(uint32 diff)
{
    if (!_enabled)
        return;

    _timer += diff;
    if (_timer < _intervalMs)
        return;
    _timer = 0;

    ReapOrphanGroups();
    GearTestPass();

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

    // Anyone still held by a run that no longer exists gets its engine back. Runs
    // end from half a dozen places - the clock, a wipe, a group disbanded out from
    // under one - and asking each of them to remember is how a character ends up
    // standing at a door with its wandering switched off for the rest of the uptime.
    ReleaseParked();

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
