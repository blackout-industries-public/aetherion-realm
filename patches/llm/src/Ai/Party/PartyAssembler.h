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

#include <array>
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

    // World thread only (called from the LLM intent drain). Adopts the
    // requester's live group as an Inside trip: the bot leader is steered
    // boss to boss, wipe determination and the run-history ledger apply, and
    // the human walks with the caravan. Promotes the claimed bot to leader
    // when the human holds the crown; taking the crown back ends the run.
    bool AdoptRun(Player* requester, Player* claimedBot);

    // Callable from ANY thread: RandomBotUpdateAction reaches ProcessBot from map
    // threads, so ownership is answered from a mutex-guarded mirror of _assembled
    // rather than the world-thread set itself.
    static bool Owns(uint32 groupLowGuid);
    static bool DriveGroupedBots();

    // True for characters the assembler is actively steering along a journey: the trip
    // leader while travelling, the whole party while being summoned. Read from
    // AllowActive on hot paths, hence a guid set rather than a group lookup.
    static bool IsDriven(uint32 charLowGuid);

private:
    bool AssembleOne();
    std::vector<Player*> Candidates() const;

    // Puts the finished party into the dungeon finder. Without this the assembler
    // only produces parties that stand around: LfgJoinAction lets a bot queue only
    // when it leads its own group, and it has no reason to do so on its own.
    bool QueueForDungeon(Player* leader) const;
    static uint32 RoleMask(Player const* bot);

    // Outdoor entrance per instance map, read once from areatrigger data. Travelling
    // to the entrance and walking in is what makes this read as a party going to a
    // dungeon rather than a party blinking into one.
    void LoadEntrances();
    void LoadInsides();

    // Creature spawn points per instance map. A party zones in to an empty doorway -
    // the nearest pack is typically far beyond aggro range - so something has to walk
    // them towards the content or they stand on the entry point until the run expires.
    void LoadInstanceSpawns();

    // Where the bosses stand. Steering at a boss walks a party through the trash in
    // between, which is what actually clears a dungeon; steering at the nearest trash
    // pack just wanders the entry hall.
    void LoadBossPositions();
    bool SendPartyToDungeon(Group* group, Player* leader);

    // A raid is the same journey with more people and the group flipped to raid
    // mode. Kept as a share of assemblies so the world holds a mix of both rather
    // than swinging entirely to one.
    bool SendPartyToRaid(Group* group, Player* leader);

    // Checked before a raid is committed to. A ten-man that forms with nowhere to
    // go cannot be converted back, so it would just stand in place forever.
    // lowestFloor reports the smallest min-level among the reachable raids, so
    // member selection can refuse anyone the trip check would later trip over.
    bool HasRaidTarget(Player const* leader, uint8& lowestFloor) const;

    // Roughly "can hold threat" / "can heal", by class. Deliberately loose: a party
    // of five that is merely plausible beats waiting for a perfect one.
    static bool CanTank(Player const* bot);
    static bool CanHeal(Player const* bot);

    struct Entrance
    {
        uint32 map{0};
        float x{0.f}, y{0.f}, z{0.f};
    };
    std::unordered_map<uint32, Entrance> _entrances;   // instance map id -> outdoor spot
    std::unordered_map<uint32, Entrance> _insides;     // instance map id -> inside spot
    std::unordered_map<uint32, std::vector<Entrance>> _spawns;  // instance map id -> packs
    std::unordered_map<uint32, std::vector<Entrance>> _bosses;  // instance map id -> bosses
    std::unordered_map<uint32, uint32> _mapMinLevel;  // instance map id -> level floor
    // Gear floors per (map << 8 | difficulty), from dungeon_access_template.
    // Consumed by the soft gearing math, never as a member-by-member veto.
    std::unordered_map<uint32, uint16> _ilvlFloor;
    static float PartyAvgIlvl(Group* group);
    uint32 GearScaledPct(Group* group, uint32 mapId, uint8 difficulty, uint32 fullPct) const;


    // How a real group gets to a dungeon: one player travels, everyone else waits,
    // then they are summoned at the meeting stone by the entrance. Modelling that is
    // both authentic and far more reliable than trying to walk five bots across a
    // continent while the RPG state machine keeps re-rolling their destination.
    // How the leader covers the ground. Real players do not all walk: they hearth to
    // a nearby inn or take a mage portal, then walk the last stretch.
    enum class Travel : uint8
    {
        Foot,     // walk and take flight paths the whole way
        Hearth,   // leader hearthstones to the nearest capital, then walks
        Portal,   // a mage opens a portal and the whole group steps through
    };

    enum class Phase : uint8
    {
        Travelling,   // leader is en route; the rest are waiting where they formed
        Summoning,    // leader has arrived; pulling everyone to the stone
        Inside,       // party has zoned in
    };

    struct Departure
    {
        Travel how{Travel::Foot};
        std::string place;   // capital hearthed or portalled to, empty on foot
        std::string actor;   // the mage who opened the portal, when there was one
    };

    // The run-history ledger. RecordRunStart returns the new row id; EndRun
    // stamps the ending and snapshots boss progress from the instance save.
    // Declared after Travel, which the signature needs.
    uint32 _runSeq{0};
    uint32 RecordRunStart(Group* group, Player* leader, std::string const& name, uint32 mapId,
                          bool isRaid, uint8 difficulty, Travel how, uint32 startYards);
    void EndRun(uint32 runId, uint32 mapId, char const* outcome);

    struct Trip
    {
        uint32 dungeonMap{0};
        Entrance door;
        std::string name;
        Phase phase{Phase::Travelling};
        Travel how{Travel::Foot};
        std::string place;
        std::string actor;
        uint32 ticks{0};
        float lastDist{0.0f};
        uint32 stalls{0};
        // History ledger row for this journey; zero means none was written.
        uint32 runId{0};
        // Full-party falls survived so far; determination runs out past the
        // configured retries.
        uint32 wipes{0};
        // A run adopted from a live group that contains a real player. The
        // assembler steers and keeps the ledger, but the group is not its to
        // teleport out or disband when the run ends - the player owns it.
        bool adopted{false};
        // Venues whose content does not begin until somebody asks for it. Counted
        // so a venue that refuses stops being asked instead of being asked forever.
        uint32 nudges{0};
        bool started{false};
        // Set the first tick the leader is confirmed standing on the dungeon map,
        // which is a different thing from having been sent there.
        bool arrived{false};
        // The attunement this run is for, if it is one: the quest the party carries
        // in its log so the bosses it kills count towards a door it cannot yet open.
        uint32 earnQuest{0};
        // Where the leader was last pointed, so an unchanged destination is not
        // re-issued every tick - re-issuing restarts the mover's stuck clock.
        float aimX{0.0f}, aimY{0.0f};
        // Consecutive ticks the party has spent fighting. A fight nobody can win
        // must not hold the run still forever.
        uint32 combatTicks{0};
    };
    std::unordered_map<uint32, Trip> _trips;   // group low guid -> journey

    // Hands an adopted run's bots back to the human (or sets them free when
    // none is left): master restored, strategies rebuilt.
    void ReleaseAdopted(Group* group);

    // Picks and performs the opening move of a journey, returning how it was made.
    Departure BeginTravel(Group* group, Player* leader, Entrance const& door);
    static char const* TravelName(Travel how);
    static char const* PhaseName(Phase phase);
    bool NearestCapital(uint32 map, uint32 teamId, Entrance& out, std::string& name) const;
    static Player* FindClassMember(Group* group, uint8 cls, uint8 minLevel);
    static float PlanarDistance(Player const* from, Entrance const& to);
    void PartySay(Group* group, Player* speaker, std::string const& text) const;

    // Trip state lives only in this object, so the dashboard has no way to see a
    // party mid-journey. Mirrored to the character database each tick instead of
    // exposing a socket from the world thread.
    void EnsureTelemetryTables();
    void WriteTelemetry();

    // The party is teleported in, so it must be teleported out. Nothing else does it:
    // the random-bot manager skips grouped bots, and a bot left inside a finished
    // instance simply stays there.
    uint32 SendGroupOutside(Group* group, uint32 dungeonMap);

    // Safety net for characters that got left inside anyway - by a restart, a disband
    // this object never saw, or an upstream group. Without it they accumulate for the
    // rest of the uptime.
    void SweepStrandedBots();

    // Refreshes the cross-thread ownership mirror from _assembled.
    void SyncOwnedMirror();

    // Doors that ask for a quest, and where that quest is earned. Read from
    // dungeon_access_requirements at startup rather than listed here, so the chain
    // is whatever the world says it is. Only normal difficulty: the heroic rows are
    // keys and achievements, which the existing Satisfy check already handles.
    struct QuestGate
    {
        uint32 quest{0};      // what the door asks for
        uint32 earnedOn{0};   // the instance whose bosses complete it
    };
    void LoadQuestGates();
    // Indexed by TeamId, so a chain that forks by faction stays one table.
    using TeamGates = std::array<QuestGate, 2>;
    std::unordered_map<uint32, TeamGates> _questGates;      // gated map -> what it asks
    std::unordered_map<uint32, std::array<uint32, 2>> _teaches;  // map -> quest it completes
    // Doors asking for something with no run behind it - an achievement, a key, or a
    // quest earned out in the world. Named once at load, then simply never chosen.
    std::unordered_set<uint32> _unearnable;

    // The heart of attunement: walks back along the gate chain to the deepest
    // dungeon this party may actually enter, and reports the quest they should be
    // carrying while they run it. Returns 0 when nothing in the chain is reachable.
    // Judged by the least-progressed member, not the leader: the door is checked
    // member by member, so a party that splits on the doorstep has not arrived.
    uint32 ResolveGate(Group* group, Player const* leader, uint32 wantMap,
                       uint32& earnQuest) const;
    // Entrance and name for a map the redirect landed on, which was never in the
    // candidate list the party chose from.
    bool DungeonInfo(Player const* leader, uint32 mapId, Entrance& where,
                     std::string& name) const;
    // Puts the attunement quest into every member's log, and takes the reward for
    // everyone who has finished it. Both are server-side: the turn-in NPCs stand
    // inside the instance the party is about to leave.
    uint32 OfferGateQuest(Group* group, uint32 questId) const;
    uint32 SettleGateQuest(Group* group, uint32 questId) const;

    void AdvanceTrips();
    bool EnterInstance(Group* group, Trip const& trip);

    // Violet Hold and Halls of Reflection hold their content behind a conversation
    // no bot ever has. Asks the instance for it directly instead; true when the ask
    // landed. World thread only, like everything else AdvanceTrips does.
    bool StartVenueEvent(Player* leader, Trip const& trip);

    bool _enabled{false};
    uint32 _intervalMs{60000};
    uint32 _targetSize{5};
    uint32 _maxParties{20};
    // One party per tick takes an hour to populate a realm this size.
    uint32 _perTick{3};
    uint32 _minLevel{15};
    uint32 _levelSpread{4};
    bool _teleport{true};
    // Parties drawn from the whole world start scattered across continents and take
    // an age to converge, which reads as five strangers wandering rather than a
    // party. Candidates are restricted to the leader's map, and anyone still far
    // away is brought in.
    bool _sameMapOnly{true};
    float _gatherRange{400.0f};
    float _arriveRange{60.0f};
    uint32 _maxTripTicks{40};
    // Uniform picks across a continent produced 10,000-yard walks that expired long
    // before the party arrived, which is why parties never reached a dungeon at all.
    // A bot in a group is skipped by the random-bot manager, so a grouped leader
    // frequently never walks at all. After this many ticks without progress the
    // journey is finished by teleport, exactly as upstream's own MoveFarTo does.
    uint32 _stallTicks{2};
    // A party that has had its run is released so its bots return to the pool.
    // Without this the assembler fills its quota once and never forms another.
    uint32 _insideTicks{20};
    // A raid is a whole evening, not a wing sweep. One dwell clock for both means
    // raids are shown the door several bosses short of the end.
    uint32 _insideTicksRaidMult{3};
    // Bounded per tick so a backlog drains steadily instead of teleporting hundreds of
    // characters in a single world update.
    uint32 _sweepPerTick{25};
    // How far ahead to look for the next pack, and how many spawn points to keep per
    // map. The full table is 5000 rows; a sample is enough to lead a party around.
    float _huntRange{8.0f};
    uint32 _spawnsPerMap{60};
    uint32 _nearestChoices{4};
    float _footRange{1200.0f};
    uint32 _portalPct{50};
    uint32 _raidPct{20};
    uint32 _raidSize{10};
    // Share of raids that expand to 25 members when the bench allows, and
    // share of raid trips that attempt the heroic difficulty where the
    // destination offers one and every member clears its access check.
    uint32 _raid25Pct{25};
    uint32 _raidHeroicPct{15};
    // The muster: ordinary formation consumes capped bots so fast that a free
    // bench of 25 never occurs naturally. Every EveryMin minutes one faction's
    // level-capped candidates are held back from party formation until they
    // number enough for a 25-raid or the muster times out.
    uint32 _musterEveryMin{45};
    uint32 _musterTimeoutMin{12};
    // How many full wipes a run absorbs before the group calls it. Zero
    // restores the old behavior: first wipe quietly burns the clock out.
    uint32 _wipeRetries{3};
    // Consecutive assemblies refused for want of a tank or a healer, indexed by size
    // class (party, raid). Without a ceiling a bracket that truly holds neither would
    // stop forming parties altogether, so the run of refusals is what releases one.
    uint32 _shortAborts[2]{0, 0};
    uint32 _shortAbortLimit{3};
    // Set when the last assembly was refused purely over party shape. That is a bad
    // hand rather than an empty pool, so the tick is allowed to deal another.
    bool _refusedShape{false};
    uint32 _musterCooldownTicks{0};
    uint32 _musterAgeTicks{0};
    int32 _musterTeam{-1};
    bool _queueLfg{true};
    bool _travelToDungeon{true};

    uint32 _timer{0};

    // Cumulative since boot, for the assembly funnel on the dashboard.
    uint32 _statFormed{0};
    uint32 _statRaids{0};
    uint32 _statTrips{0};
    uint32 _statStalls{0};
    uint32 _statArrived{0};
    uint32 _statEntered{0};
    uint32 _raidMapCount{0};

    // Parties this assembler built, so it can stop once the world holds enough of
    // them rather than grouping every bot on the realm.
    std::unordered_set<uint32> _assembled;
};

#define sPartyAssembler PartyAssembler::instance()

#endif
