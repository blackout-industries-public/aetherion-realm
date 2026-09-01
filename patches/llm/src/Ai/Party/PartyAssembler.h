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

class Creature;
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

    // Narrower than Owns, and the right test for anything destructive. Owns stays
    // true for as long as the group exists, including for a party whose journey
    // ended without it being disbanded - a door timeout, a lost leader, a refused
    // entry. Shielding those from the maintenance cycle would freeze their members
    // out of it for the rest of the uptime, which is how a stalled party stops being
    // part of the living population. OnRun is true only while a journey is actually
    // underway, and every journey is bounded by its own tick budget, so the shield
    // expires on its own the moment a party stops progressing.
    static bool OnRun(uint32 groupLowGuid);
    static bool DriveGroupedBots();

    // True for characters the assembler is actively steering along a journey: the trip
    // leader while travelling, the whole party while being summoned. Read from
    // AllowActive on hot paths, hence a guid set rather than a group lookup.
    static bool IsDriven(uint32 charLowGuid);

    // Diagnostic, callable from any thread. Records that some call site was about to
    // take a member out of an owned party, naming the site and whether the guard
    // stopped it. Stamped where the removal is attempted rather than reconstructed
    // afterwards from a roster: group ids are reused across restarts, so matching a
    // departed bot against a stored roster names the wrong run about as often as the
    // right one. No-op for anyone outside an owned party.
    static void NoteRemoval(Player const* bot, char const* site, bool suppressed);

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

    // The collector's journey: back to content the realm has long outgrown, for the
    // achievements and the legendaries that only drop there. Same trip machinery,
    // same ledger, same clocks - only the destination is chosen differently.
    bool SendPartyToOldContent(Group* group, Player* leader);

    // Checked before a raid is committed to. A ten-man that forms with nowhere to
    // go cannot be converted back, so it would just stand in place forever.
    // lowestFloor reports the smallest min-level among the reachable raids, so
    // member selection can refuse anyone the trip check would later trip over.
    bool HasRaidTarget(Player const* leader, uint8& lowestFloor) const;

    // Roughly "can hold threat" / "can heal", by class. Deliberately loose: a party
    // of five that is merely plausible beats waiting for a perfect one.
    static bool CanTank(Player const* bot);
    static bool CanHeal(Player const* bot);

    // Who takes point. The leader is not a title here: it is the character the
    // steering aims, the one that arrives first, and the master every member
    // follows and opens fire behind - so whatever leads is what walks into the
    // pack. A tank is picked wherever the hand holds one, and uniformly among the
    // tanks rather than by level, so parties still form across the brackets
    // instead of piling up at the cap. Falls back to the old uniform draw when
    // there is no tank to be had; refusing to form over it would cost far more
    // than a rogue taking point.
    static Player* PickLeader(std::vector<Player*> const& from);

    struct Entrance
    {
        uint32 map{0};
        float x{0.f}, y{0.f}, z{0.f};
    };
    std::unordered_map<uint32, Entrance> _entrances;   // instance map id -> outdoor spot
    std::unordered_map<uint32, Entrance> _insides;     // instance map id -> inside spot
    // Where a boss stands, and which creature's death credits its encounter. The
    // credit entry is what ties a position on the floor to a bit in the instance's
    // own completed-encounter mask, so a boss the mask says is already dead is never
    // walked at - whether it died to this party ten minutes ago or to the party that
    // held this lockout yesterday.
    struct BossSpot
    {
        float x{0.f}, y{0.f}, z{0.f};
        uint32 creditEntry{0};
    };

    std::unordered_map<uint32, std::vector<Entrance>> _spawns;  // instance map id -> packs
    std::unordered_map<uint32, std::vector<BossSpot>> _bosses;  // instance map id -> bosses
    // Dwell-clock multiplier per map, derived from how far the farthest boss stands
    // from the arrival point. A party closes roughly twenty yards a tick once it is
    // fighting its way in, so a five-hundred-yard wing spends its whole budget
    // walking and never reaches anything worth killing.
    std::unordered_map<uint32, uint32> _travelMult;
    uint32 TravelMultFor(uint32 mapId) const;
    // The deepest z any creature stands at, per instance map. A character well below
    // it is not on a lower floor - there is no lower floor - it has fallen off the
    // world's walkable part. The Forge of Souls keeps its whole spawn table between
    // z=613 and z=742 and its bridges have no rails; sampled parties stood at z=519.
    std::unordered_map<uint32, float> _floorZ;
    bool BelowVenueFloor(uint32 mapId, Player const* who) const;
    // The closest place on the map something is meant to stand, judged flat: a
    // faller's own z is the abyss, so the ground above it is what nearest means.
    Entrance const* NearestGroundSpot(uint32 mapId, float x, float y) const;
    std::unordered_map<uint32, uint32> _mapMinLevel;  // instance map id -> level floor
    // Gear floors per (map << 8 | difficulty), from dungeon_access_template.
    // Consumed by the soft gearing math, never as a member-by-member veto.
    std::unordered_map<uint32, uint16> _ilvlFloor;
    // The lowest floor a map declares for ANY of its modes. The realm writes an
    // item-level floor for 25 heroic dungeon rows and only 13 normal ones, so every
    // Wrath five-man run at normal difficulty was judged 'unmeasured' and handed out
    // with no feasibility check at all. The heroic number is still a fact about the
    // venue, and this realm's own record says it is the right one: across 570 Wrath
    // five-man runs, parties below that floor produced a kill in 3.7% of runs and
    // wiped 0.54 times a run, parties at or above it 23.7% and 0.19. Used only where
    // the mode being run declares nothing of its own.
    std::unordered_map<uint32, uint16> _mapFloorAny;
    static float PartyAvgIlvl(Group* group);
    uint32 GearScaledPct(Group* group, uint32 mapId, uint8 difficulty, uint32 fullPct) const;

    // What the party's gear says about one destination. The operator's question -
    // "is this raid even possible for us" - has three honest answers and this is
    // where they are decided, once, so the log line, the appetite roll and the
    // ledger row all say the same thing.
    struct GearVerdict
    {
        uint16 floor{0};     // what the door asks, 0 when the realm never said
        int16 margin{0};     // party average minus that floor
        uint32 weight{100};  // relative appetite, 0 when it is out of reach
        char const* band{"unmeasured"};
    };
    GearVerdict JudgeGear(Group* group, uint32 mapId, uint8 difficulty) const;

    // How many encounters the place holds at this difficulty, from the same list
    // the core credits kills against. Zero when the realm has no encounter data,
    // which is the signal to fall back to "walk until the clock stops".
    static uint32 EncounterCount(uint32 mapId, uint8 difficulty);
    static uint32 CountBits(uint32 mask);


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
                          bool isRaid, uint8 difficulty, Travel how, uint32 startYards,
                          GearVerdict const& gear, uint16 gearCeiling);
    void EndRun(uint32 runId, uint32 mapId, char const* outcome);

    // "Not walking at any boss right now". Zero is a real index into the boss list,
    // so the empty slot needs a value of its own.
    static constexpr uint32 kNoBossAim = 0xFFFFFFFFu;

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
        // The count is cleared every time the instance is seen actually running, so
        // an event that stops - Violet Hold resets itself to NOT_STARTED the moment
        // the last member of the party is down - is asked for again on a fresh
        // budget rather than being asked once at the door and never again.
        uint32 nudges{0};
        bool started{false};
        // The member holding this venue's event: the character the instance is asked
        // through and the one the party musters on. A role rather than a title, and
        // re-let whenever its holder is dead, gone, or standing on another map -
        // asking through the leader alone put the whole event behind one character
        // that spends the run walking into things.
        uint32 warden{0};
        // How far the venue's own event has got - Violet Hold's wave count, the Halls'
        // wave number, the Trial's progress counter. Kept so a run can tell an event
        // that is ramping from one that has stalled, and so the log names progress the
        // encounter mask never sees: waves are not encounters and credit nothing until
        // a boss is released. Starts at a value no venue reports, so the first reading
        // is a change rather than a stall.
        static constexpr uint32 kNoStage = 0xFFFFFFFFu;
        uint32 eventStage{kNoStage};
        uint32 stageTicks{0};
        // What the stage counter read when this run last asked the venue for
        // something. The Trial of the Champion stops at three separate gates and its
        // counter does NOT move when the ask lands - the champions die at stage six
        // and the arena stays at six through the whole Argent Challenge the ask brings
        // on - so "have we already asked at this stage" is the only safe test there.
        // Asking twice would summon the next act twice.
        uint32 askedStage{kNoStage};
        // Set the first tick the leader is confirmed standing on the dungeon map,
        // which is a different thing from having been sent there.
        bool arrived{false};
        // The attunement this run is for, if it is one: the quest the party carries
        // in its log so the bosses it kills count towards a door it cannot yet open.
        uint32 earnQuest{0};
        // Where the leader was last pointed, so an unchanged destination is not
        // re-issued every tick - re-issuing restarts the mover's stuck clock.
        float aimX{0.0f}, aimY{0.0f};
        // Boss positions this run is done with, by index into the map's boss list.
        // The list is spawn data and never changes, so a boss stays in it after it
        // dies; without this the nearest-boss rule walks the party back to the corpse
        // it just made. Also collects bosses the party demonstrably cannot walk to.
        std::unordered_set<uint32> visitedBosses;
        // The subset of the above that was retired for being out of reach rather than
        // for having been stood on. A wipe reseats the party at the door and buys it a
        // whole fresh attempt, and that attempt needs something to attempt: standing on
        // a boss is not killing it, so the visits are forgiven and only the ones the
        // party demonstrably cannot walk to stay struck off. Measured before this: the
        // Forge of Souls reached both of its bosses, wiped, and then spent the rest of
        // a ninety-minute clock in the trash with an empty list - 102 minutes and 0.2
        // bosses a run across six runs.
        std::unordered_set<uint32> unreachableBosses;
        // Bosses the party has stood next to, for telemetry only. Reaching is not
        // beating, so this set steers nothing - it exists so the log can still say
        // a party found its boss even when the boss then killed them.
        std::unordered_set<uint32> reachedBosses;
        // Ticks spent standing next to each still-living boss. Retirement used to
        // happen on arrival, which told the run a boss was finished the moment
        // somebody touched its doorstep: 35 Forge of Souls runs reached Bronjahm,
        // struck him off unfought, and ground trash for up to two hours with 8 to
        // 23 deaths and not one wipe to reset the list. A fight now gets a real
        // budget of ticks, and only running that budget out retires the boss.
        std::unordered_map<uint32, uint32> bossFightTicks;
        // Whether this run has already put its leader in a siege vehicle, so the log
        // says it once rather than every tick the raid spends riding.
        bool boarded{false};
        // Which boss the leader is currently walking at, the closest it has got, and
        // how many ticks it has failed to get closer. A wing whose bosses sit behind
        // a teleporter is never reached on foot, and staring at one is the whole run.
        uint32 aimBoss{kNoBossAim};
        float aimDist{0.0f};
        uint32 aimStalls{0};
        // Consecutive ticks the party has spent fighting. A fight nobody can win
        // must not hold the run still forever.
        uint32 combatTicks{0};
        // Ticks still owed to the party after a fight ends, before the next advance.
        // Armed while the party is swinging and spent afterwards, so the beat is
        // taken between pulls rather than on a timer of its own.
        uint32 settleTicks{0};
        // Whether the leader is currently held in place - rpg steering off its
        // engine and the grind target refused. Tracked so the hold is applied and
        // lifted once rather than re-asserted into the strategy engine every tick.
        bool parked{false};
        // Deaths this run has taken that were not full wipes. The wipe count only
        // ever named the falls that took everybody; a party that loses one member
        // per pull and recovers looked identical to one that lost nobody.
        uint32 deaths{0};
        // A trip into content the realm has outgrown, chasing achievements and old
        // legendaries rather than progression.
        bool collector{false};
        // Which mode the group set out in. Needed inside the run to ask the core for
        // this venue's encounter list, which is keyed by (map, difficulty).
        uint8 difficulty{0};
        // The instance's own completed-encounter mask, read live each tick while the
        // party is standing in it. entryMask is whatever was already down when they
        // walked in - a lockout somebody else spent - so credit for this run is the
        // difference and never the total. killMask only ever grows: an instance that
        // resets under a party must not be able to take its record away.
        uint32 entryMask{0};
        uint32 killMask{0};
        bool maskSeeded{false};
        uint32 bossesDown{0};        // encounters this run put down itself
        uint32 encounters{0};        // how many the venue holds at this difficulty
        // Determination, earned rather than configured. Every encounter that falls
        // buys the run a bounded extension of its dwell clock, so a raid that is
        // still killing things is not shown the door mid-progress - and a raid that
        // is killing nothing gets exactly the clock it always had.
        uint32 bonusTicks{0};
        uint32 extensions{0};
        // Consecutive ticks with every boss position visited and nothing new dead.
        // A run with nothing left to walk at is finished; without this it hunts
        // trash for the rest of an hour-and-a-half clock holding 10-25 bots.
        uint32 idleTicks{0};
        // Ticks spent trying to pull the party to the leader's stone. At some doors
        // the sixty-yard arrival test never comes true and the party stands outside
        // until its whole budget expires: Ulduar 32 runs reached the door for 4
        // entries, Uldaman 6 for none.
        uint32 summonTicks{0};
        // Who was in the party at the end of the previous tick. Diffed against the
        // live roster so a member that disappears is named at the moment it happens,
        // against this run's own id rather than a group id that gets reused.
        std::unordered_set<uint32> roster;
    };
    std::unordered_map<uint32, Trip> _trips;   // group low guid -> journey

    // Groups outlive the process - the database reloads them at boot while the
    // trips that owned them die with memory - so restarts strand whole parties
    // as zombies whose members never errand and never re-form. The reaper
    // disbands any all-bot group that has stood tripless in the open world for
    // ten consecutive ticks. Measured need: 631 groups holding 97 percent of
    // the realm, 7 of them on a live run.
    void ReapOrphanGroups();
    std::unordered_map<uint32, uint32> _orphanStrikes;

    // A controlled gear experiment: outfit a deterministic share of the realm in
    // epics and watch whether progression follows. Membership is the character's
    // own guid modulo the share, so the cohort is reproducible, needs no roster,
    // and is trivially checkable from SQL months later. The table is the record
    // of who was outfitted and when - it doubles as the pass's own idempotency
    // marker, so restarts never re-gear anyone.
    void GearTestPass();
    std::unordered_set<uint32> _gearTested;
    uint32 _gearTestShare{0};    // 0 disables; 4 means one bot in four
    uint32 _gearTestIlvl{200};
    uint32 _gearTestPerTick{5};

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

    // Which faction, if either, already has enough ungrouped level-capped bots on the
    // bench to seat a twenty-five right this moment. Returns a TeamId, or -1 when
    // neither does. Same test the muster answer uses, asked before the wait.
    int32 FactionWithFullBench() const;

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

    // Reads the instance's own completed-encounter mask and turns it into a durable
    // record: every encounter that falls is stamped to the ledger at the moment it
    // dies, named from the encounter list. The old count was read once at EndRun from
    // the `instance` table, which is live state pruned on reset - a run whose instance
    // had gone recorded zero however deep it got, and nothing outside WotLK raids was
    // ever named at all, because the core's own encounter log refuses everything else.
    // Also retires the boss positions the mask says are done, which is what lets a
    // party that inherits a spent lockout walk past the corpses to what is left.
    void NoteKills(Group* group, Player* leader, Trip& trip);
    // Whether the instance's own completed-encounter mask says the boss standing at
    // boss-list index `index` is dead. The mask is the only authority on that; a
    // party's position next to it says nothing at all.
    bool BossIsDown(Trip const& trip, uint32 index) const;

    // Everything a party inside an instance needs to still be a party next tick:
    // dead members put back on their feet once the fight is over, members that ended
    // up off the dungeon map brought back, and departures recorded. Partial deaths
    // were nobody's job - only a full wipe was handled - so a member that died to
    // trash simply stayed dead until the random-bot manager collected it.
    void RecoverInside(Group* group, Player* leader, Trip& trip);

    // Violet Hold and Halls of Reflection hold their content behind a conversation
    // no bot ever has. Asks the instance for it directly instead; true when the ask
    // landed. World thread only, like everything else AdvanceTrips does. Takes
    // whichever member is standing in the place rather than the leader specifically:
    // the instance answers any player on its map, and the leader is the one character
    // guaranteed to be somewhere else fighting something.
    bool StartVenueEvent(Player* asker, Trip const& trip);

    // Venues whose content is an event rather than a floor plan. Their bosses sit in
    // sealed cells until the event releases them, so the spawn positions the rest of
    // this file steers by are addresses of locked doors: a party walks onto both of
    // Violet Hold's, retires them for having been reached, finds nothing left to walk
    // at and declares itself exhausted ten minutes in - four of five runs, zero deaths
    // apiece, while the assault it had just started was still on its first wave.
    static bool IsEventVenue(uint32 mapId);

    // Whether the venue's own event is running, and how far it has got. Read from the
    // instance script every tick through a member standing in it, because nothing else
    // knows: waves are not encounters, so the completed-encounter mask stays empty for
    // the whole first half of a Violet Hold run that is going perfectly well.
    bool VenueEventLive(Player* onMap, uint32 mapId, uint32& stage) const;

    // Who holds the event this tick. Prefers the member that already held it, so the
    // role is stable across a run; re-let to any live member standing on the dungeon
    // map when its holder cannot serve.
    Player* PickWarden(Group* group, Trip& trip) const;

    // Where an event venue's party should actually be standing. The live objective if
    // the venue has one - the thing whose death advances the event, not the nearest
    // corpse-to-be - and otherwise the point every wave walks to, which is a far better
    // place to wait than a cell door.
    bool EventObjective(Player* onMap, Trip const& trip, float& x, float& y,
                        float& z) const;

    // Names the one target that ends the current wave, by putting the party's skull on
    // it. Steering moves the party's feet; this moves what they swing at, which in a
    // venue that spawns faster than it can be killed is the half that decides the run.
    void FocusEventTarget(Group* group, Player* onMap, Trip const& trip) const;

    // Ulduar's siege yard. The module can fight Flame Leviathan from a vehicle but
    // cannot get into one on its own: its boarding trigger waits for the bot's master
    // to be seated first, which never happens in a raid of bots. Seating the leader is
    // the whole bootstrap. Returns true when it wants the party walked somewhere.
    Creature* NearestSiegeVehicle(Player* leader) const;
    bool BoardSiegeVehicles(Player* leader, Trip& trip, float& x, float& y, float& z) const;

    // Stops the leader where it stands, or lets it walk again. Withholding the next
    // destination is not the same thing as holding still: the rpg engine keeps
    // calling MoveFarTo against the destination it already has, every bot tick,
    // whether or not this object re-issues it. patch_rpgcombat covers the bot's own
    // combat; nothing covered the party's, so a leader that dropped combat while the
    // members behind it were still swinging walked on and took them with it. Taking
    // the rpg strategy off the engine is what actually stops the walk; "stay"
    // alongside it is what stops the leader picking a fresh grind target, which is
    // the blind pull itself - AttackAnythingAction asks for that strategy by name
    // and stands down. Both are lifted together.
    void HoldLeader(Player* leader, Trip& trip, bool hold);

    // Leaders held by a run that has since ended - the group disbanded under it, the
    // clock expired mid-fight - handed their engine back. Without this a character
    // could be left standing at a door with its wandering switched off for the rest
    // of the uptime, which is exactly the failure the sweep below exists to prevent
    // for stranded bots.
    void ReleaseParked();
    std::unordered_set<uint32> _parked;

    // Whether the party has anything to stand still for. Somebody still walking
    // back, somebody hurt, or a healer with no mana to open the next pull with. When
    // none of that is true the party is already settled and the advance resumes at
    // no cost, so the grace is paid only where it buys something.
    bool NeedsBreather(Group* group, Player* leader) const;

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
    // Extra attempts a run earns per encounter it has already put down, capped.
    // Determination that has to be paid for: a raid four bosses deep is in business
    // and gets to keep pulling, a raid that wipes on the trash still calls it after
    // the base retries.
    uint32 _wipeBonusPerBoss{1};
    uint32 _wipeBonusCap{4};
    // What a kill buys, in dwell ticks, and how many times over. Bounded so a venue
    // that keeps feeding a party kills cannot hold a raid's worth of bots forever.
    uint32 _progressExtendTicks{10};
    uint32 _progressExtendMax{4};
    // How far under a door's item-level floor a party will still try. At the floor
    // the appetite is whole; it ramps to nothing this far below, which is the same
    // ramp the heroic roll has always used. Deeper than this and the destination is
    // named as out of reach rather than quietly attempted.
    uint32 _gearStretch{20};
    // Ticks the summon at the door is given before the party is taken in anyway.
    // Standing at the stone is scenery; the instance teleport does not care where
    // anyone was standing when it fired.
    uint32 _summonTicks{4};
    // Ticks a run with nothing left to walk at is given before it is closed out, so
    // a fight in progress and a spell-credited encounter both still have room to land.
    uint32 _exhaustGrace{4};
    // The beat between pulls. How many ticks the party may stand after a fight ends
    // before the steering insists again, and how far a member may trail the leader
    // and still count as having come back. Capped rather than open-ended: a
    // straggler that will never arrive must not be able to hold the run still.
    uint32 _settleTicks{2};
    float _regroupRange{40.0f};
    // Consecutive assemblies refused for want of a tank or a healer, indexed by size
    // class (party, raid). Without a ceiling a bracket that truly holds neither would
    // stop forming parties altogether, so the run of refusals is what releases one.
    uint32 _shortAborts[2]{0, 0};
    uint32 _shortAbortLimit{3};
    // How often a collector-led party actually goes back for old content rather
    // than running the current tier like everyone else. The persona share is the
    // real cap on this; the percentage only decides how single-minded they are.
    uint32 _collectorPct{60};
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
    // Destinations a forming party looked at and put back: the first because the party
    // is not geared for it yet, the second because a party of bots cannot perform it
    // at all. Two different answers to "is this even possible", counted apart so the
    // board can tell "come back better dressed" from "never, not by us". The gear
    // count covers dungeons as well as raids now - a five-man was never asked the
    // question, which is how parties at 147 item levels kept being handed Wrath
    // wings they killed nothing in.
    uint32 _statGearRefused{0};
    uint32 _statUnperformable{0};
    uint32 _raidMapCount{0};

    // Parties this assembler built, so it can stop once the world holds enough of
    // them rather than grouping every bot on the realm.
    std::unordered_set<uint32> _assembled;
};

#define sPartyAssembler PartyAssembler::instance()

#endif
