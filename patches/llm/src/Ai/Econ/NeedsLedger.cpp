#include "NeedsLedger.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "BankDepositAction.h"
#include "CharacterCache.h"
#include "CraftPlanner.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Group.h"
#include "Mail.h"
#include "Map.h"
#include "ScriptMgr.h"
#include "SharedDefines.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
#include "Random.h"
#include "RandomPlayerbotMgr.h"
#include "StatsWeightCalculator.h"
#include "Trainer.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    bool sEventsEnabled = false;
    bool sPreemptEnabled = false;
    bool sVendorEnabled = false;
    bool sProtectTradeGoods = false;
    bool sPaidRepairs = false;
    bool sRemoteMail = false;
    uint32 sVendorFreeSlotsPct = 20;
    uint64 sVendorBrokeMinValue = 500;

    uint32 sVendorFarMaxYards = 3000;
    bool sGatherEnabled = false;
    // A node this far below the bot's skill is not worth the walk - keeps
    // veterans off copper veins (the operator's no-pointless-gathering rule).
    uint32 sGatherMaxTierGap = 150;

    // Economic personas: every bot gets a stable disposition - farmers live
    // at the nodes and the auction house, warlords in the battlegrounds,
    // adventurers on the road - so specialists supply each other through the
    // market the way a real population does. Derived from the bot's own
    // profession set plus a guid hash, so it survives restarts with no state.
    uint8 constexpr PERSONA_ADVENTURER = 0;
    uint8 constexpr PERSONA_FARMER = 1;
    uint8 constexpr PERSONA_MERCHANT = 2;
    uint8 constexpr PERSONA_WARLORD = 3;
    // The collector goes back for what the world has already moved past: old
    // raids for the achievements and the legendaries nobody drops any more.
    // Carved out of the adventurer share rather than added on top, so the
    // population still sums to itself and current content keeps its numbers.
    uint8 constexpr PERSONA_COLLECTOR = 4;
    // The hunter goes after named rare spawns - the trophy mobs a player farms
    // for a mount or a collection. Carved out of the adventurer band by the same
    // rule the collector was, and deliberately NOT out of the collector band:
    // the party assembler reads IsCollector to decide who goes back for old
    // content, and that share was expensive to tune. Every collector hash still
    // returns collector, so party formation is bit-for-bit unchanged.
    uint8 constexpr PERSONA_HUNTER = 5;
    char const* PERSONA_NAMES[] = {"adventurer", "farmer", "merchant", "warlord",
                                   "collector", "hunter"};
    // Idle-beat errand appetite per persona. Measured before this existed:
    // every errand claimed every beat, and party formation fell 86% in a day.
    // A collector runs errands about as often as an adventurer - its
    // distinctiveness is where it goes, not how much time it spends shopping.
    // A hunter sits between the two: hunting is mostly travel, and a hunter
    // that claimed most of its beats would stop being available for anything.
    uint8 constexpr PERSONA_DUTY[] = {22, 65, 50, 10, 20, 30};
    uint8 constexpr PERSONA_LAST = PERSONA_HUNTER;
    uint32 sDutyScale = 100;
    // World-thread only: written during the pass, read by WriteTelemetry.
    std::unordered_map<uint32, uint8> sPersona;

    uint8 ArchetypeOf(Player* bot)
    {
        auto skilled = [bot](uint32 skill)
        { return bot->HasSkill(skill) && bot->GetSkillValue(skill) > 0; };
        bool const gathers =
            skilled(SKILL_HERBALISM) || skilled(SKILL_MINING) || skilled(SKILL_SKINNING);
        bool const crafts = skilled(SKILL_ALCHEMY) || skilled(SKILL_BLACKSMITHING) ||
                            skilled(SKILL_ENCHANTING) || skilled(SKILL_ENGINEERING) ||
                            skilled(SKILL_LEATHERWORKING) || skilled(SKILL_TAILORING) ||
                            skilled(SKILL_JEWELCRAFTING) || skilled(SKILL_INSCRIPTION);
        uint32 const h = (bot->GetGUID().GetCounter() * 2654435761u) % 100u;
        // Collectors come out of the top of each band - the same hash, so a bot
        // keeps its disposition for life across restarts, and roughly one in
        // eight of the realm ends up chasing old content.
        // Hunters take the top slice of each ADVENTURER band; every collector
        // threshold below (88, 90, 87, 85) is untouched on purpose, so the
        // population still sums to itself and IsCollector answers identically
        // for every bot on the realm.
        if (gathers && crafts)
            return h < 70 ? PERSONA_FARMER
                          : (h < 82 ? PERSONA_ADVENTURER
                                    : (h < 88 ? PERSONA_HUNTER : PERSONA_COLLECTOR));
        if (gathers)
            return h < 50 ? PERSONA_FARMER
                          : (h < 75 ? PERSONA_WARLORD
                                    : (h < 85 ? PERSONA_ADVENTURER
                                              : (h < 90 ? PERSONA_HUNTER : PERSONA_COLLECTOR)));
        if (crafts)
            return h < 60 ? PERSONA_MERCHANT
                          : (h < 81 ? PERSONA_ADVENTURER
                                    : (h < 87 ? PERSONA_HUNTER : PERSONA_COLLECTOR));
        return h < 45 ? PERSONA_WARLORD
                      : (h < 79 ? PERSONA_ADVENTURER
                                : (h < 85 ? PERSONA_HUNTER : PERSONA_COLLECTOR));
    }

    struct Verdict
    {
        uint8 kind;
        bool hasFar;
        uint16 farMap;
        float x, y, z;
        // Mailbox target identity (E5.2); zero when the verdict is not mailbox.
        uint32 goEntry = 0;
        uint32 goSpawnId = 0;
        // Persona duty: the chance (percent) this bot spends an idle beat on
        // its errand rather than a pastime. Stamped from the archetype at
        // verdict time; the map thread rolls against it in ClaimErrandBeat.
        uint8 dutyPct = 30;
    };

    // World thread builds verdicts during the pass and swaps them into the
    // mirror at pass end; map-thread preemption only ever locks and looks up.
    std::mutex sVerdictMx;
    std::unordered_map<uint32, Verdict> sVerdictMirror;
    std::unordered_map<uint32, Verdict> sVerdictBuild;
    // What each bot still cannot pay for, mirrored beside the verdicts and under the
    // same lock: the disposal split runs on map threads and the needs themselves are
    // world-thread state. Absent means solvent.
    std::unordered_map<uint32, uint64> sShortfallMirror;
    std::unordered_map<uint32, uint64> sShortfallBuild;

    // Vendor spawn positions per map, from static spawn data at startup. The
    // far leg's whole cost is one linear scan per broke bot per pass.
    std::unordered_map<uint16, std::vector<std::array<float, 3>>> sVendorSpawns;

    // E5.2: mailbox GameObject spawns per map - entry and spawnId carried so
    // the near case can target the exact object.
    struct MailboxSpawn
    {
        uint32 entry;
        uint32 spawnId;
        float x, y, z;
    };
    std::unordered_map<uint16, std::vector<MailboxSpawn>> sMailboxSpawns;
    bool sMailboxVisits = false;
    bool sPaidTraining = false;
    bool sCraftEnabled = false;

    // E3.2: class-trainer spawns per map, entry kept so validity for the
    // specific bot's class is checked at verdict time.
    struct TrainerSpawn
    {
        uint32 entry;
        float x, y, z;
    };
    std::unordered_map<uint16, std::vector<TrainerSpawn>> sTrainerSpawns;

    // E4.2: auctioneer spawns per map, and the trip trigger threshold.
    std::unordered_map<uint16, std::vector<std::array<float, 3>>> sAuctioneerSpawns;
    // Bankers stand in the same city blocks as the auctioneers above, and are found
    // the same way: position only, because the action at the other end locates the
    // actual creature through the map once the bot has walked into range.
    std::unordered_map<uint16, std::vector<std::array<float, 3>>> sBankerSpawns;
    bool sAhEnabled = false;
    // E6.3b: the banker errand rides the same switch the deposit action already
    // obeys, so turning banking on turns on both the trip and the arrival.
    bool sBankEnabled = false;
    uint32 sAhMinItemsForTrip = 3;
    // How deep the vault must already be in something before the next stack is worth
    // more sold than stored, counted in stacks of that item. Zero turns the per-item
    // split off entirely and restores the old whole-bot rivalry between the two
    // errands. See PlanDisposal.
    uint32 sVaultPlentyStacks = 3;

    // E7 focus trips: spell-focus GameObjects (anvil, forge, cooking fire) per
    // map, keyed by their SpellFocusObject id.
    struct FocusSpawn
    {
        uint32 focusId;
        uint32 entry;
        uint32 spawnId;
        float x, y, z;
    };
    std::unordered_map<uint16, std::vector<FocusSpawn>> sFocusSpawns;

    // E7.5 gathering nodes: herb and vein chest spawns per map with the skill
    // and rank their lock demands, so a trip can be tier-matched up front.
    struct GatherSpawn
    {
        uint32 skill;
        uint32 reqSkill;
        uint32 entry;
        uint32 spawnId;
        float x, y, z;
    };
    std::unordered_map<uint16, std::vector<GatherSpawn>> sGatherSpawns;

    // E10 rare geography: named rare and rare-elite spawns per map, with the
    // rank and level the hunt has to be sized against. maxlevel rather than
    // minlevel, because a spawn rolls somewhere in the band and the bot has to
    // survive the top of it.
    struct RareSpawn
    {
        uint32 entry;
        uint32 spawnId;
        uint8 rank;
        uint8 level;
        float x, y, z;
    };
    std::unordered_map<uint16, std::vector<RareSpawn>> sRareSpawns;
    bool sRareEnabled = false;
    uint32 sRareFarMaxYards = 2000;
    // Nothing to prove killing something this far below the bot. Also keeps a
    // level 80 out of Elwynn Forest, where the low-level rares are dense.
    uint32 sRareMaxLevelGap = 25;
    // Ceiling on hunts held at once, realm-wide. The persona share already caps
    // who CAN hunt; this caps how much of the fleet is walking cross-zone at any
    // moment, which is the load the economy errands and party formation compete
    // with. Counted per pass, because the mirror is rebuilt whole every pass.
    uint32 sRareMaxConcurrent = 60;
    uint32 sRareIssued = 0;
    // The shortlist a hunter draws its target from. Strictly-nearest would send
    // every hunter in a zone at the same spawn, and the losers would arrive at a
    // corpse; drawing by guid hash spreads them with no claim protocol to keep
    // consistent across threads.
    uint32 constexpr RARE_SHORTLIST = 6;
    // What each bot is currently aimed at, world thread only. An aim event fires
    // when this changes, so a bot holding the same target across passes is one
    // commitment in the log rather than one row a minute.
    std::unordered_map<uint32, uint32> sRareAimed;

    // E8.1 listings mirror, rebuilt each pass on the world thread.
    std::mutex sListingsMx;
    std::vector<NeedsLedger::AhListing> sListings;

    // E11 gear market. Measured before any of this was written: of 360 live
    // auctions, nine were equippable, and the buy side had managed 45 bids in
    // six days. The demand side was never the problem - there was nothing on
    // the shelf. Rescue puts gear on it, the shopping trip sends buyers to it.
    bool sGearRescue = false;
    uint32 sGearRescueMax = 6;
    bool sShopEnabled = false;
    // Ceiling on shopping trips held at once, realm-wide. Same reasoning as the
    // rare hunt's cap: the persona duty roll decides how eagerly a bot runs an
    // errand, but something has to bound how much of the fleet is walking to a
    // city at all.
    uint32 sShopMaxConcurrent = 80;
    uint32 sShopIssued = 0;
    // What each shopper is currently walking to the market FOR, world thread
    // only. This is an aim log, not a cooldown. The first cut stamped a 900s
    // cooldown at issue time, and because the verdict mirror is rebuilt whole
    // every pass, that did not stop a bot pacing the auction house - it deleted
    // the bot's errand 60 seconds after granting it and refused to grant it
    // again for fifteen minutes. Measured: held shopping trips collapsed from
    // 41 to 2 and stayed there while the cumulative aim count kept climbing, so
    // nearly every trip issued was one that evaporated before the bot could act
    // on it - and the arrival aim, which asks UrgentVerdict at the door, would
    // have found nothing by then either. The rare hunt had this right: re-issue
    // while the reason holds, and log only when the aim changes.
    std::unordered_map<uint32, uint64> sShopAimed;

    // The gear half of the listings mirror, distilled once per pass. The screen
    // it feeds runs for every bot on the realm, so it must not be a full-market
    // walk with an Item allocation per listing - which is exactly what the
    // priced gear need used to be, capped at the first 40 listings and
    // therefore blind to gear sitting behind them. World thread only, like the
    // pass that writes it and the pass that reads it.
    struct GearOffer
    {
        uint32 item;
        int32 randomPropertyId;
        uint32 buyout;
        uint8 houseId;
        uint8 invType;
        uint16 ilvl;
    };
    std::vector<GearOffer> sGearOffers;

    // Which equipment slots an item of a given InventoryType can occupy. Spelled
    // out rather than asked of the core, whose resolver wants a constructed Item;
    // this runs per bot per offer. Cosmetic and container types map to nothing,
    // which is how they fall out of the screen.
    uint8 EquipSlotsFor(uint8 invType, uint8 (&out)[2])
    {
        switch (invType)
        {
            case INVTYPE_HEAD: out[0] = EQUIPMENT_SLOT_HEAD; return 1;
            case INVTYPE_NECK: out[0] = EQUIPMENT_SLOT_NECK; return 1;
            case INVTYPE_SHOULDERS: out[0] = EQUIPMENT_SLOT_SHOULDERS; return 1;
            case INVTYPE_CHEST:
            case INVTYPE_ROBE: out[0] = EQUIPMENT_SLOT_CHEST; return 1;
            case INVTYPE_WAIST: out[0] = EQUIPMENT_SLOT_WAIST; return 1;
            case INVTYPE_LEGS: out[0] = EQUIPMENT_SLOT_LEGS; return 1;
            case INVTYPE_FEET: out[0] = EQUIPMENT_SLOT_FEET; return 1;
            case INVTYPE_WRISTS: out[0] = EQUIPMENT_SLOT_WRISTS; return 1;
            case INVTYPE_HANDS: out[0] = EQUIPMENT_SLOT_HANDS; return 1;
            case INVTYPE_CLOAK: out[0] = EQUIPMENT_SLOT_BACK; return 1;
            case INVTYPE_FINGER:
                out[0] = EQUIPMENT_SLOT_FINGER1;
                out[1] = EQUIPMENT_SLOT_FINGER2;
                return 2;
            case INVTYPE_TRINKET:
                out[0] = EQUIPMENT_SLOT_TRINKET1;
                out[1] = EQUIPMENT_SLOT_TRINKET2;
                return 2;
            case INVTYPE_WEAPON:
                out[0] = EQUIPMENT_SLOT_MAINHAND;
                out[1] = EQUIPMENT_SLOT_OFFHAND;
                return 2;
            case INVTYPE_2HWEAPON:
            case INVTYPE_WEAPONMAINHAND: out[0] = EQUIPMENT_SLOT_MAINHAND; return 1;
            case INVTYPE_SHIELD:
            case INVTYPE_WEAPONOFFHAND:
            case INVTYPE_HOLDABLE: out[0] = EQUIPMENT_SLOT_OFFHAND; return 1;
            case INVTYPE_RANGED:
            case INVTYPE_THROWN:
            case INVTYPE_RANGEDRIGHT:
            case INVTYPE_RELIC: out[0] = EQUIPMENT_SLOT_RANGED; return 1;
            default: return 0;
        }
    }

    struct Need
    {
        std::string type;
        std::string target;
        uint64 amount;
        uint64 freeMoney;
        double since;
    };

    std::unordered_map<uint32, std::vector<Need>> sNeeds;
    // First-seen times survive recomputation so the dashboard can show how long a
    // need has been starved.
    std::unordered_map<uint32, std::map<std::string, double>> sFirstSeen;

    // Class-trainer creature entries, collected once. The per-bot pass then walks
    // ~200 trainer objects instead of the full 30k creature-template container -
    // the difference between a per-minute fleet pass and a tick stall (BRD E1.2).
    std::vector<uint32> sTrainerEntries;
    bool sTrainerCacheBuilt = false;

    // Riding tiers: spell learned at level, trainer price plus a modest mount
    // item. Prices are DBC/DB-stable for 3.3.5; verified against npc_trainer.
    struct RidingTier { uint8 level; uint32 spell; uint64 cost; };
    RidingTier const RIDING[] = {
        { 20, 33388, 50000 },      // apprentice 4g + ~1g mount
        { 40, 33391, 600000 },     // journeyman 50g + 10g mount
        { 60, 34090, 3000000 },    // expert 250g + 50g mount
        { 70, 34091, 55000000 },   // artisan 5000g + 500g mount
    };

    uint64 RepairCost(Player* bot)
    {
        // Same DBC math as the module's RepairCostValue, over equipped and
        // carried gear.
        uint64 total = 0;
        for (int i = EQUIPMENT_SLOT_START; i < INVENTORY_SLOT_ITEM_END; ++i)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, i);
            if (!item)
                continue;
            uint32 maxDur = item->GetUInt32Value(ITEM_FIELD_MAXDURABILITY);
            if (!maxDur)
                continue;
            uint32 lost = maxDur - item->GetUInt32Value(ITEM_FIELD_DURABILITY);
            if (!lost)
                continue;
            ItemTemplate const* proto = item->GetTemplate();
            DurabilityCostsEntry const* dcost = sDurabilityCostsStore.LookupEntry(proto->ItemLevel);
            if (!dcost)
                continue;
            DurabilityQualityEntry const* qual =
                sDurabilityQualityStore.LookupEntry((proto->Quality + 1) * 2);
            if (!qual)
                continue;
            uint32 mult =
                dcost->multiplier[ItemSubClassToDurabilityMultiplierId(proto->Class, proto->SubClass)];
            total += uint64(lost * mult * double(qual->quality_mod));
        }
        return total;
    }

    void BagPressure(Player* bot, uint32& freeSlots, uint32& totalSlots, uint64& trashValue)
    {
        freeSlots = 0;
        totalSlots = 16;
        trashValue = 0;
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        {
            Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
            if (!item)
            {
                ++freeSlots;
                continue;
            }
            if (item->GetTemplate()->Quality == ITEM_QUALITY_POOR)
                trashValue += uint64(item->GetTemplate()->SellPrice) * item->GetCount();
        }
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        {
            Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot);
            if (!bag)
                continue;
            totalSlots += bag->GetBagSize();
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
            {
                Item* item = bag->GetItemByPos(slot);
                if (!item)
                {
                    ++freeSlots;
                    continue;
                }
                if (item->GetTemplate()->Quality == ITEM_QUALITY_POOR)
                    trashValue += uint64(item->GetTemplate()->SellPrice) * item->GetCount();
            }
        }
    }

    // E5.1 interim mail collection - the program's single deliberate P2
    // exception, world-thread only. Mirrors HandleMailTakeMoney/TakeItem minus
    // the mailbox proximity check (which needs a GM-tier permission to skip on
    // the packet path). COD mail is never auto-paid. Gold and inventory save
    // through the public fast-save paths; the mail rows themselves persist on
    // the periodic character save (m_mailsUpdated), a <=60s window that at
    // bot copper scale is an accepted tradeoff. E5.2 replaces all of this with
    // real mailbox visits.
    void CollectMail(Player* bot)
    {
        if (bot->GetMails().empty())
            return;

        time_t const now = time(nullptr);
        for (Mail* m : bot->GetMails())
        {
            if (!m || m->state == MAIL_STATE_DELETED || m->deliver_time > now || m->COD)
                continue;

            if (m->money)
            {
                uint64 const amount = m->money;
                if (bot->ModifyMoney(m->money, false))
                {
                    m->money = 0;
                    m->state = MAIL_STATE_CHANGED;
                    bot->m_mailsUpdated = true;
                    CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                    bot->SaveGoldToDB(trans);
                    CharacterDatabase.CommitTransaction(trans);
                    NeedsLedger::LogEvent("mail_money", bot->GetGUID().GetCounter(), 0, 0,
                                          std::to_string(amount));
                }
            }

            if (m->items.empty())
                continue;

            std::vector<uint32> guids;
            for (auto const& mi : m->items)
                guids.push_back(mi.item_guid);
            for (uint32 ig : guids)
            {
                Item* it = bot->GetMItem(ig);
                if (!it)
                    continue;
                ItemPosCountVec dest;
                if (bot->CanStoreItem(NULL_BAG, NULL_SLOT, dest, it, false) != EQUIP_ERR_OK)
                    return;  // bags full: stop the whole collection, retry next pass
                uint32 const entry = it->GetEntry();
                uint32 const count = it->GetCount();
                m->RemoveItem(ig);
                m->removedItems.push_back(ig);
                m->state = MAIL_STATE_CHANGED;
                bot->m_mailsUpdated = true;
                bot->RemoveMItem(ig);
                it->SetState(ITEM_UNCHANGED);
                bot->MoveItemToInventory(dest, it, true);
                CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
                bot->SaveInventoryAndGoldToDB(trans);
                CharacterDatabase.CommitTransaction(trans);
                NeedsLedger::LogEvent("mail_item", bot->GetGUID().GetCounter(), entry, count, "");
            }
        }
    }

    // E4.2 trip trigger: how much of the bags is worth an auction-house
    // journey. Trade goods a bot's own recipes consume stay home; greens and
    // better travel regardless.
    // 'spokenFor' holds the items the disposal split has already promised to the
    // guild vault. They are still perfectly sellable - that is the whole reason the
    // two errands used to fight over them - so the auction branch has to be told
    // which ones are no longer its business, or it counts them and captures the bot
    // exactly as before.
    uint32 CountListable(Player* bot, std::unordered_set<ObjectGuid> const* spokenFor = nullptr)
    {
        std::unordered_set<uint32> keep;
        std::vector<CraftOption> options;
        CraftPlanner::Enumerate(bot, options, 0);
        for (CraftOption const& opt : options)
        {
            for (auto const& r : opt.reagents)
                keep.insert(r.first);
            for (uint32 t : opt.tools)
                keep.insert(t);
        }

        uint32 n = 0;
        auto consider = [&](Item* item)
        {
            if (!item || item->IsSoulBound())
                return;
            if (spokenFor && spokenFor->count(item->GetGUID()))
                return;
            ItemTemplate const* proto = item->GetTemplate();
            if (!proto->SellPrice || proto->Quality < ITEM_QUALITY_NORMAL)
                return;
            if (proto->Class == ITEM_CLASS_TRADE_GOODS ? !keep.count(proto->ItemId)
                                                       : proto->Quality >= ITEM_QUALITY_UNCOMMON)
                ++n;
        };
        for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
            consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
        for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
            if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
                for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                    consider(bag->GetItemByPos(slot));
        return n;
    }

    bool HasCollectibleMail(Player* bot)
    {
        time_t const now = time(nullptr);
        for (Mail* m : bot->GetMails())
            if (m && m->state != MAIL_STATE_DELETED && m->deliver_time <= now && !m->COD &&
                (m->money || !m->items.empty()))
                return true;
        return false;
    }

    // E4.4: the market's lifecycle, from the core's own hooks - the only place
    // sold and expired can be told apart. Seller and buyer each get an event so
    // the dashboard can rank both sides.
    class EconAuctionScript : public AuctionHouseScript
    {
    public:
        EconAuctionScript() : AuctionHouseScript("EconAuctionScript") {}

        void OnAuctionAdd(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
        {
            NeedsLedger::LogEvent("ah_listed", entry->owner.GetCounter(), entry->item_template,
                                  entry->itemCount, std::to_string(entry->buyout));
        }

        void OnAuctionSuccessful(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
        {
            NeedsLedger::LogEvent("ah_sold", entry->owner.GetCounter(), entry->item_template,
                                  entry->itemCount, std::to_string(entry->bid));
            NeedsLedger::LogEvent("ah_bought", entry->bidder.GetCounter(), entry->item_template,
                                  entry->itemCount, std::to_string(entry->bid));
        }

        void OnAuctionExpire(AuctionHouseObject* /*ah*/, AuctionEntry* entry) override
        {
            NeedsLedger::LogEvent("ah_expired", entry->owner.GetCounter(), entry->item_template,
                                  entry->itemCount, "");
        }
    };

    // The rare a bot is currently hunting, or 0. Under the verdict lock like
    // every other mirror read, because the hooks below run on map threads.
    uint32 HuntedEntry(uint32 guid)
    {
        std::lock_guard<std::mutex> lock(sVerdictMx);
        auto it = sVerdictMirror.find(guid);
        return (it != sVerdictMirror.end() && it->second.kind == NeedsLedger::VERDICT_RARE)
                   ? it->second.goEntry
                   : 0;
    }

    // E10: a hunt's outcome, recorded where the world already knows it. Without
    // these two hooks the only observable is that a bot was aimed at something,
    // which cannot tell a kill from a death from a wasted walk.
    class RareHuntScript : public PlayerScript
    {
    public:
        RareHuntScript()
            : PlayerScript("AetherionRareHuntScript",
                           {PLAYERHOOK_ON_CREATURE_KILL, PLAYERHOOK_ON_PLAYER_JUST_DIED})
        {
        }

        void OnPlayerCreatureKill(Player* killer, Creature* killed) override
        {
            if (!killer || !killed)
                return;
            CreatureTemplate const* proto = killed->GetCreatureTemplate();
            if (!proto || (proto->rank != CREATURE_ELITE_RARE &&
                           proto->rank != CREATURE_ELITE_RAREELITE))
                return;
            // A hunt that landed and a rare that happened to walk into a bot are
            // the same kill to the world and completely different numbers to the
            // operator, so the held verdict decides which one this was.
            uint32 const guid = killer->GetGUID().GetCounter();
            bool const hunted = HuntedEntry(guid) == killed->GetEntry();
            NeedsLedger::LogEvent("rare_hunt", guid, killed->GetEntry(),
                                  uint32(killed->GetLevel()),
                                  std::string(hunted ? "kill|" : "bonus|") +
                                      std::to_string(killer->GetMapId()) + "|" +
                                      std::string(killed->GetName()));
            if (hunted)
                NeedsLedger::RetireRare(guid);
        }

        void OnPlayerJustDied(Player* player) override
        {
            if (!player)
                return;
            uint32 const guid = player->GetGUID().GetCounter();
            // Real players never appear in the mirror - the ledger only walks
            // the bot fleet - so this is a bot-only path without asking.
            uint32 const entry = HuntedEntry(guid);
            if (!entry)
                return;
            CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(entry);
            NeedsLedger::LogEvent("rare_hunt", guid, entry, uint32(player->GetLevel()),
                                  "died|" + std::to_string(player->GetMapId()) + "|" +
                                      (proto ? proto->Name : std::string()));
            NeedsLedger::RetireRare(guid);
        }
    };

    uint64 TrainingCost(Player* bot)
    {
        uint64 total = 0;
        for (uint32 entry : sTrainerEntries)
        {
            Trainer::Trainer* trainer = sObjectMgr->GetTrainer(entry);
            if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
                continue;
            for (auto const& spell : trainer->GetSpells())
                if (trainer->CanTeachSpell(bot, &spell))
                    total += spell.MoneyCost;
        }
        return total;
    }
}

NeedsLedger* NeedsLedger::instance()
{
    static NeedsLedger inst;
    return &inst;
}

void NeedsLedger::LoadConfig()
{
    _enabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Needs.Enabled", false);
    _tickMs = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Needs.TickMs", 6000);
    _shards = std::max(1u, sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Needs.Shards", 10));
    sEventsEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Events.Enabled", false);
    sPreemptEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Preempt.Enabled", false);
    sVendorEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Vendor.Enabled", false);
    sProtectTradeGoods = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.ProtectTradeGoods", false);
    sPaidRepairs = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.PaidRepairs", false);
    sRemoteMail = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.RemoteMail", false);
    sMailboxVisits = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Mailbox.Enabled", false);
    sPaidTraining = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.PaidTraining", false);
    sCraftEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Craft.Enabled", false);
    sAhEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Ah.Enabled", false);
    sBankEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Bank.Enabled", false);
    sAhMinItemsForTrip = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Ah.MinItemsForTrip", 3);
    sVaultPlentyStacks =
        sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Guild.PlentyStacks", 3);
    sVendorFreeSlotsPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Vendor.FreeSlotsPct", 20);
    sVendorBrokeMinValue = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Vendor.BrokeMinValue", 500);
    sVendorFarMaxYards = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Vendor.FarMaxYards", 3000);
    sGatherEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Gather.Enabled", false);
    sGatherMaxTierGap = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Gather.MaxTierGap", 150);
    sDutyScale = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Duty.Scale", 100);
    sRareEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Rare.Enabled", false);
    sRareFarMaxYards = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Rare.FarMaxYards", 2000);
    sRareMaxLevelGap = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Rare.MaxLevelGap", 25);
    sRareMaxConcurrent = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Rare.MaxConcurrent", 60);
    sGearRescue = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Gear.Rescue", false);
    sGearRescueMax = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Gear.RescueMax", 6);
    sShopEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Gear.Shop.Enabled", false);
    sShopMaxConcurrent =
        sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Gear.Shop.MaxConcurrent", 80);

    if (_enabled || sEventsEnabled)
        EnsureTables();
}

void NeedsLedger::EnsureTables()
{
    CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS aetherion_needs");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE aetherion_needs ("
        " guid INT UNSIGNED NOT NULL, need_type VARCHAR(16) NOT NULL,"
        " target VARCHAR(64) NOT NULL DEFAULT '',"
        " amount BIGINT UNSIGNED NOT NULL, free_money BIGINT UNSIGNED NOT NULL,"
        " since_ts DOUBLE NOT NULL,"
        " PRIMARY KEY (guid, need_type, target)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    CharacterDatabase.DirectExecute("DROP TABLE IF EXISTS aetherion_gold_now");
    CharacterDatabase.DirectExecute(
        "CREATE TABLE aetherion_gold_now ("
        " guid INT UNSIGNED NOT NULL PRIMARY KEY,"
        " level TINYINT UNSIGNED NOT NULL, money BIGINT UNSIGNED NOT NULL"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    // History tables append and are never dropped: the income-rate panel and the
    // destruction baseline are exactly the data a restart must not erase.
    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_gold_bands ("
        " ts INT UNSIGNED NOT NULL, band TINYINT UNSIGNED NOT NULL,"
        " n INT UNSIGNED NOT NULL, total BIGINT UNSIGNED NOT NULL,"
        " KEY by_ts (ts)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");

    CharacterDatabase.DirectExecute(
        "CREATE TABLE IF NOT EXISTS aetherion_econ_events ("
        " id BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        " ts INT UNSIGNED NOT NULL, kind VARCHAR(24) NOT NULL,"
        " guid INT UNSIGNED NOT NULL, item INT UNSIGNED NOT NULL DEFAULT 0,"
        " count INT UNSIGNED NOT NULL DEFAULT 0,"
        " detail VARCHAR(120) NOT NULL DEFAULT '',"
        " KEY by_kind_ts (kind, ts)"
        ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4");
}

void NeedsLedger::LogEvent(char const* kind, uint32 guid, uint32 item, uint32 count,
                           std::string const& detail)
{
    if (!sEventsEnabled)
        return;

    std::string safeDetail = detail.substr(0, 120);
    CharacterDatabase.EscapeString(safeDetail);
    std::ostringstream sql;
    sql << "INSERT INTO aetherion_econ_events (ts, kind, guid, item, count, detail)"
        << " VALUES (UNIX_TIMESTAMP(), '" << kind << "'," << guid << "," << item << ","
        << count << ",'" << safeDetail << "')";
    CharacterDatabase.Execute(sql.str());
}

uint8 NeedsLedger::UrgentVerdict(uint32 guid)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto it = sVerdictMirror.find(guid);
    return it != sVerdictMirror.end() ? it->second.kind : VERDICT_NONE;
}

uint8 NeedsLedger::ClaimErrandBeat(uint32 guid)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto it = sVerdictMirror.find(guid);
    if (it == sVerdictMirror.end())
        return VERDICT_NONE;
    // The persona duty roll: a farmer spends most idle beats on the errand,
    // a warlord almost none. Losing the roll only defers the errand - the
    // verdict stands and a later beat claims it.
    if (urand(0, 99) >= it->second.dutyPct)
        return VERDICT_NONE;
    return it->second.kind;
}

bool NeedsLedger::FarVendor(uint32 guid, uint32 botMap, float& x, float& y, float& z)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto it = sVerdictMirror.find(guid);
    if (it == sVerdictMirror.end() || !it->second.hasFar || it->second.farMap != uint16(botMap))
        return false;
    x = it->second.x;
    y = it->second.y;
    z = it->second.z;
    return true;
}

bool NeedsLedger::MailboxTarget(uint32 guid, uint32 botMap, uint32& entry, uint32& spawnId,
                                float& x, float& y, float& z)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto it = sVerdictMirror.find(guid);
    if (it == sVerdictMirror.end() ||
        (it->second.kind != VERDICT_MAILBOX && it->second.kind != VERDICT_FOCUS &&
         it->second.kind != VERDICT_GATHER) || !it->second.hasFar ||
        it->second.farMap != uint16(botMap))
        return false;
    entry = it->second.goEntry;
    spawnId = it->second.goSpawnId;
    x = it->second.x;
    y = it->second.y;
    z = it->second.z;
    return true;
}

bool NeedsLedger::RareTarget(uint32 guid, uint32 botMap, uint32& entry, uint32& spawnId,
                             float& x, float& y, float& z)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto it = sVerdictMirror.find(guid);
    if (it == sVerdictMirror.end() || it->second.kind != VERDICT_RARE || !it->second.hasFar ||
        it->second.farMap != uint16(botMap))
        return false;
    entry = it->second.goEntry;
    spawnId = it->second.goSpawnId;
    x = it->second.x;
    y = it->second.y;
    z = it->second.z;
    return true;
}

void NeedsLedger::RetireRare(uint32 guid)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto it = sVerdictMirror.find(guid);
    // Only ever drops a rare verdict: an errand this bot also holds is the
    // world thread's to withdraw, not a hunt outcome's.
    if (it != sVerdictMirror.end() && it->second.kind == VERDICT_RARE)
        sVerdictMirror.erase(it);
}

bool NeedsLedger::RareHuntEnabled() { return sRareEnabled; }

bool NeedsLedger::SellOnVendorVisit() { return sVendorEnabled; }
bool NeedsLedger::ProtectTradeGoods() { return sProtectTradeGoods; }
uint32 NeedsLedger::RescueGearMax() { return sGearRescueMax; }

bool NeedsLedger::WorthRescuing(Item* item)
{
    if (!sGearRescue || !item)
        return false;

    ItemTemplate const* proto = item->GetTemplate();
    if (!proto)
        return false;

    // Only what an auction house will actually take AND price. The listing
    // action anchors its price on the vendor value, so a piece without one has
    // no price the market could read; bind-on-pickup and already-soulbound gear
    // cannot be listed at all. That last clause is also the safety property
    // that keeps this out of ClearAllItems' way: equipping a bind-on-equip
    // piece binds it, so a bot's worn set is still cleared on a level reset
    // exactly as before.
    if (proto->Class != ITEM_CLASS_ARMOR && proto->Class != ITEM_CLASS_WEAPON)
        return false;
    if (proto->Quality < ITEM_QUALITY_UNCOMMON || !proto->SellPrice)
        return false;
    if (proto->Bonding == BIND_WHEN_PICKED_UP || proto->Bonding == BIND_QUEST_ITEM ||
        proto->Bonding == BIND_QUEST_ITEM1)
        return false;

    uint8 slots[2] = {0, 0};
    if (!EquipSlotsFor(uint8(proto->InventoryType), slots))
        return false;

    return !item->IsSoulBound() && item->CanBeTraded();
}
bool NeedsLedger::PaidRepairs() { return sPaidRepairs; }
bool NeedsLedger::PaidTraining() { return sPaidTraining; }
bool NeedsLedger::CraftEnabled() { return sCraftEnabled; }

bool NeedsLedger::IsCollector(Player* bot)
{
    return bot && ArchetypeOf(bot) == PERSONA_COLLECTOR;
}

void NeedsLedger::RegisterAuctionScript()
{
    // Registration is one-way in the script system; the hooks no-op through the
    // LogEvent gate when events are off.
    new EconAuctionScript();
}

void NeedsLedger::RegisterHuntScript()
{
    // Same one-way registration as the auction script; both hooks no-op through
    // the LogEvent gate when events are off, and the kill hook additionally
    // costs nothing until something rare actually dies.
    new RareHuntScript();
}

void NeedsLedger::BuildTrainerCache()
{
    CreatureTemplateContainer const* ctc = sObjectMgr->GetCreatureTemplates();
    for (auto const& pair : *ctc)
    {
        if (!(pair.second.npcflag & UNIT_NPC_FLAG_TRAINER))
            continue;
        Trainer::Trainer* trainer = sObjectMgr->GetTrainer(pair.first);
        if (trainer && trainer->GetTrainerType() == Trainer::Type::Class)
            sTrainerEntries.push_back(pair.first);
    }

    // E2.1b: static vendor geography, so the far leg can aim a walk without
    // any grid queries.
    for (auto const& pair : sObjectMgr->GetAllCreatureData())
    {
        CreatureData const& data = pair.second;
        CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
        if (!proto || !(proto->npcflag & UNIT_NPC_FLAG_VENDOR))
            continue;
        sVendorSpawns[data.mapid].push_back({data.posX, data.posY, data.posZ});
    }

    // E5.2 mailboxes and E7 spell-focus objects share one geography walk.
    for (auto const& pair : sObjectMgr->GetAllGOData())
    {
        GameObjectData const& data = pair.second;
        GameObjectTemplate const* proto = sObjectMgr->GetGameObjectTemplate(data.id);
        if (!proto)
            continue;
        if (proto->type == GAMEOBJECT_TYPE_MAILBOX)
            sMailboxSpawns[data.mapid].push_back(
                {data.id, uint32(data.spawnId), data.posX, data.posY, data.posZ});
        else if (proto->type == GAMEOBJECT_TYPE_SPELL_FOCUS && proto->spellFocus.focusId)
            sFocusSpawns[data.mapid].push_back({proto->spellFocus.focusId, data.id,
                                                uint32(data.spawnId), data.posX, data.posY,
                                                data.posZ});
        else if (proto->type == GAMEOBJECT_TYPE_CHEST && proto->chest.lockId)
        {
            // E7.5: herb and vein chests reveal their profession through the
            // lock table, the same derivation the loot path uses at cast time.
            if (LockEntry const* lock = sLockStore.LookupEntry(proto->chest.lockId))
                for (uint8 i = 0; i < MAX_LOCK_CASE; ++i)
                {
                    if (lock->Type[i] != LOCK_KEY_SKILL)
                        continue;
                    uint32 const skill = SkillByLockType(LockType(lock->Index[i]));
                    if (skill != SKILL_HERBALISM && skill != SKILL_MINING)
                        continue;
                    sGatherSpawns[data.mapid].push_back(
                        {skill, std::max<uint32>(1, lock->Skill[i]), data.id,
                         uint32(data.spawnId), data.posX, data.posY, data.posZ});
                    break;
                }
        }
    }

    // E3.2: class-trainer geography from the entries collected above. E4.2
    // rides the same spawn walk for auctioneers.
    {
        std::unordered_set<uint32> trainerSet(sTrainerEntries.begin(), sTrainerEntries.end());
        for (auto const& pair : sObjectMgr->GetAllCreatureData())
        {
            CreatureData const& data = pair.second;
            if (trainerSet.count(data.id))
                sTrainerSpawns[data.mapid].push_back({data.id, data.posX, data.posY, data.posZ});
            CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(data.id);
            if (proto && (proto->npcflag & UNIT_NPC_FLAG_AUCTIONEER))
                sAuctioneerSpawns[data.mapid].push_back({data.posX, data.posY, data.posZ});
            if (proto && (proto->npcflag & UNIT_NPC_FLAG_BANKER))
                sBankerSpawns[data.mapid].push_back({data.posX, data.posY, data.posZ});

            // E10: rare geography, off the same walk. rank 4 is a rare and
            // rank 2 a rare elite (SharedDefines CreatureEliteType). World
            // bosses are rank 3 and are deliberately absent - a hunt has to be
            // something one bot, or one small party, can finish. An NPC that
            // offers a service is not prey however rare its model is, and a
            // civilian will not fight back.
            if (proto && (proto->rank == CREATURE_ELITE_RARE ||
                          proto->rank == CREATURE_ELITE_RAREELITE) &&
                !proto->npcflag && !proto->HasFlagsExtra(CREATURE_FLAG_EXTRA_CIVILIAN))
                sRareSpawns[data.mapid].push_back({data.id, uint32(data.spawnId),
                                                   uint8(proto->rank), proto->maxlevel,
                                                   data.posX, data.posY, data.posZ});
        }
    }

    sTrainerCacheBuilt = true;
}

void NeedsLedger::Tick(uint32 diff)
{
    if (!_enabled)
        return;

    _elapsed += diff;
    if (_elapsed < _tickMs)
        return;
    _elapsed = 0;

    if (!sTrainerCacheBuilt)
        BuildTrainerCache();

    ProcessShard();

    if (++_shard >= _shards)
    {
        _shard = 0;
        ++_passes;
        WriteTelemetry();
        // The pass just published is the set of hunts now held; the next pass
        // starts its concurrency budget over. Reset after the swap, never
        // before, or the cap would count two passes' worth.
        sRareIssued = 0;
        sShopIssued = 0;
    }

    // E8.2: refresh the vault view every five minutes. Guild bank writes go
    // through immediate transactions, so the table is near-live; the sync
    // query is a few hundred rows against a primary key join.
    _vaultAgeMs += _tickMs;
    if (_vaultAgeMs >= 300000 || _vault.empty())
    {
        _vaultAgeMs = 0;
        RefreshVaultCache();
    }
}

void NeedsLedger::RefreshVaultCache()
{
    std::unordered_map<uint32, std::vector<VaultSlot>> fresh;
    if (QueryResult result = CharacterDatabase.Query(
            "SELECT gbi.guildid, gbi.TabId, gbi.SlotId, ii.itemEntry, ii.count "
            "FROM guild_bank_item gbi JOIN item_instance ii ON ii.guid = gbi.item_guid"))
        do
        {
            Field* f = result->Fetch();
            fresh[f[0].Get<uint32>()].push_back(
                VaultSlot{f[1].Get<uint8>(), f[2].Get<uint8>(), f[3].Get<uint32>(),
                          f[4].Get<uint32>()});
        } while (result->NextRow());

    std::lock_guard<std::mutex> lock(_vaultMutex);
    _vault.swap(fresh);
}

uint32 NeedsLedger::VaultStock(uint32 guildId, uint32 itemEntry)
{
    NeedsLedger* self = instance();
    std::lock_guard<std::mutex> lock(self->_vaultMutex);
    auto const it = self->_vault.find(guildId);
    if (it == self->_vault.end())
        return 0;
    uint32 held = 0;
    for (VaultSlot const& vs : it->second)
        if (vs.entry == itemEntry)
            held += vs.count;
    return held;
}

uint32 NeedsLedger::VaultPlentyStacks()
{
    return sVaultPlentyStacks;
}

uint64 NeedsLedger::Shortfall(uint32 guid)
{
    std::lock_guard<std::mutex> lock(sVerdictMx);
    auto const it = sShortfallMirror.find(guid);
    return it != sShortfallMirror.end() ? it->second : 0;
}

void NeedsLedger::PlanDisposal(Player* bot, std::unordered_set<ObjectGuid>& bank)
{
    bank.clear();
    uint32 const guildId = bot ? bot->GetGuildId() : 0;
    // No guild, no shared inventory to stock, so nothing changes for these bots.
    if (!guildId || !sVaultPlentyStacks)
        return;

    // Solvency first. What the bot still cannot pay for is covered out of the bags
    // before any of them are set aside, so a bot with an unpaid repair bill sells
    // rather than donates. Vendor price is the yardstick: it understates an auction
    // and never overstates it, so the bot errs towards holding one stack too many
    // back for itself rather than one too few.
    uint64 quota = Shortfall(bot->GetGUID().GetCounter());

    // Cheap gate first, for the same reason CountDepositable has one: this now runs
    // realm-wide on every needs pass, and the two expensive halves below are a recipe
    // enumeration and a lock on a structure the world thread swaps. A bot carrying
    // nothing a vault has a tab for can be answered by a bag walk alone.
    bool anyCandidate = false;
    auto const couldMatter = [](Item* item)
    {
        ItemTemplate const* proto = item ? item->GetTemplate() : nullptr;
        if (!proto)
            return false;
        if (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_CONSUMABLE)
            return true;
        return (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
               proto->Quality >= ITEM_QUALITY_UNCOMMON;
    };
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; !anyCandidate && slot < INVENTORY_SLOT_ITEM_END;
         ++slot)
        anyCandidate = couldMatter(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START;
         !anyCandidate && bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 slot = 0; !anyCandidate && slot < bag->GetBagSize(); ++slot)
                anyCandidate = couldMatter(bag->GetItemByPos(slot));
    if (!anyCandidate)
        return;

    // One lock for the whole bag rather than one per item: this runs on map threads
    // for a large share of the fleet, and the vault view is a shared structure the
    // world thread swaps out every five minutes.
    std::unordered_map<uint32, uint32> stock;
    {
        NeedsLedger* self = instance();
        std::lock_guard<std::mutex> lock(self->_vaultMutex);
        auto const it = self->_vault.find(guildId);
        if (it != self->_vault.end())
            for (VaultSlot const& vs : it->second)
                stock[vs.entry] += vs.count;
    }

    // A crafter's working stock is not surplus. Both the selling and the banking
    // paths already refuse to part with reagents the bot's own recipes consume, and
    // the split has to refuse for the same reason or it would hand the guild the
    // very items that make the bot useful to it.
    std::unordered_set<uint32> ownReagents;
    {
        std::vector<CraftOption> options;
        CraftPlanner::Enumerate(bot, options, 0);
        for (CraftOption const& opt : options)
        {
            for (auto const& reagent : opt.reagents)
                ownReagents.insert(reagent.first);
            for (uint32 tool : opt.tools)
                ownReagents.insert(tool);
        }
    }

    auto const wantedTab = [](ItemTemplate const* proto)
    {
        // The seeded vault layout: Materials, Consumables, Gear. Anything with no
        // tab to live in is not the guild's business and stays on the sell side.
        if (proto->Class == ITEM_CLASS_TRADE_GOODS || proto->Class == ITEM_CLASS_CONSUMABLE)
            return true;
        return (proto->Class == ITEM_CLASS_WEAPON || proto->Class == ITEM_CLASS_ARMOR) &&
               proto->Quality >= ITEM_QUALITY_UNCOMMON;
    };

    auto const consider = [&](Item* item)
    {
        if (!item || item->IsSoulBound() || !item->CanBeTraded() || item->IsNotEmptyBag())
            return;
        ItemTemplate const* proto = item->GetTemplate();
        if (!proto)
            return;
        if (quota)
        {
            uint64 const worth = uint64(proto->SellPrice) * item->GetCount();
            quota = worth >= quota ? 0 : quota - worth;
            return;
        }
        if (!wantedTab(proto) || ownReagents.count(proto->ItemId))
            return;
        // Plenty is counted in stacks, not items: three stacks of ore and three
        // stacks of gems are very different numbers, and a threshold that ignored
        // that would fill the vault with whatever happens to stack highest.
        uint32 const stack = std::max<uint32>(1, proto->GetMaxStackSize());
        auto const held = stock.find(proto->ItemId);
        if (held != stock.end() && held->second >= stack * sVaultPlentyStacks)
            return;
        bank.insert(item->GetGUID());
    };

    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
        consider(bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot));
    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
        if (Bag* bag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bagSlot))
            for (uint32 slot = 0; slot < bag->GetBagSize(); ++slot)
                consider(bag->GetItemByPos(slot));
}

bool NeedsLedger::FindVaultReagent(uint32 guildId, uint32 itemEntry, uint8& tab, uint8& slot)
{
    NeedsLedger* self = instance();
    std::lock_guard<std::mutex> lock(self->_vaultMutex);
    auto const it = self->_vault.find(guildId);
    if (it == self->_vault.end())
        return false;
    for (VaultSlot const& vs : it->second)
        if (vs.entry == itemEntry)
        {
            tab = vs.tab;
            slot = vs.slot;
            return true;
        }
    return false;
}

void NeedsLedger::ProcessShard()
{
    uint32 index = 0;
    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it, ++index)
    {
        if (index % _shards != _shard)
            continue;
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld())
            continue;
        ComputeNeeds(bot);
    }
}

void NeedsLedger::ComputeNeeds(Player* bot)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    double const now = double(time(nullptr));
    auto& seen = sFirstSeen[guid];
    std::vector<Need> needs;

    // Real mailbox trips own collection once armed; the remote path stays as
    // the fallback below them (E5.2 retires E5.1).
    if (sRemoteMail && !sMailboxVisits)
        CollectMail(bot);

    // The reservation chain: each need's free money is what remains after every
    // higher-priority need is funded, mirroring the module's budget priorities.
    uint64 const money = bot->GetMoney();
    uint64 reserved = 0;
    auto push = [&](std::string const& type, std::string const& target, uint64 amount)
    {
        if (!amount)
            return;
        std::string key = type + "|" + target;
        if (!seen.count(key))
            seen[key] = now;
        uint64 freeMoney = money > reserved ? money - reserved : 0;
        needs.push_back({type, target, amount, freeMoney, seen[key]});
        reserved += amount;
    };

    if (uint64 repair = RepairCost(bot))
        push("repair", "", repair);

    if (uint64 training = TrainingCost(bot))
        push("training", "", training);

    // Ammo before mounts: a hunter without bullets is broken now, not aspiring.
    if (bot->getClass() == CLASS_HUNTER)
    {
        uint32 ammoId = bot->GetUInt32Value(PLAYER_AMMO_ID);
        if (!ammoId || bot->GetItemCount(ammoId) < 200)
            push("ammo", "", uint64(bot->GetLevel()) * 50);
    }

    for (RidingTier const& tier : RIDING)
        if (bot->GetLevel() >= tier.level && !bot->HasSpell(tier.spell))
        {
            push("mount", std::to_string(tier.level), tier.cost);
            break;
        }

    // Priced gear need: the cheapest live listing this bot could genuinely
    // equip as an upgrade. Once priced, gear enters the reservation chain, an
    // unfunded upgrade makes the bot "broke", and the sell-for-gold loop farms
    // toward it - the raid-readiness chain (BRD E8 + operator ask).
    //
    // E11 rewrite. The first cut asked ItemUsage - which constructs an Item per
    // question - about the first 40 listings in mirror order. On a market whose
    // listings are overwhelmingly herbs and arrows that is both the expensive
    // way to ask and a window too narrow to contain the gear: across the whole
    // realm's history it priced a gear need exactly zero times. This asks a
    // pre-distilled gear-only index instead, and the question it asks of the
    // bot allocates nothing. It is deliberately a SCREEN, not a verdict on the
    // item: item level and slot fit are enough to decide whether walking to the
    // city could pay off, and the strict class/stat test still runs at the
    // auctioneer where the money actually changes hands.
    uint64 gearUpgradePrice = 0;
    bool gearUpgradeFunded = false;
    {
        uint8 const house = bot->GetTeamId() == TEAM_ALLIANCE ? 2 : 6;
        for (GearOffer const& offer : sGearOffers)
        {
            if (offer.houseId != house || !offer.buyout)
                continue;
            if (gearUpgradePrice && offer.buyout >= gearUpgradePrice)
                continue;
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(offer.item);
            if (!proto || bot->BotCanUseItem(proto) != EQUIP_ERR_OK)
                continue;
            uint8 slots[2] = {0, 0};
            uint8 const slotCount = EquipSlotsFor(offer.invType, slots);
            for (uint8 i = 0; i < slotCount; ++i)
            {
                Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slots[i]);
                // An empty slot is the neediest state there is, and the dungeon
                // finder's own average counts it as a zero.
                uint32 const held = equipped ? equipped->GetTemplate()->ItemLevel : 0;
                if (offer.ilvl > held)
                {
                    gearUpgradePrice = offer.buyout;
                    break;
                }
            }
        }
        if (gearUpgradePrice)
        {
            push("gear", "upgrade", gearUpgradePrice);
            // Funded means: after repair, training, ammo and the mount the bot
            // is saving for, it can still pay this. An unfunded upgrade is a
            // reason to go earn money, never a reason to walk to a city.
            gearUpgradeFunded = needs.back().freeMoney >= needs.back().amount;
        }
    }

    // Materials need (E7.4 groundwork, observe-only): a crafter whose known
    // recipe lacks reagents wants those reagents - the ledger records it so
    // the gather/buy behaviors that come next have a measured demand to serve.
    {
        std::vector<CraftOption> options;
        CraftPlanner::Enumerate(bot, options, 5);
        for (CraftOption const& opt : options)
            if (!opt.missing.empty())
            {
                std::string key = "materials|" + std::to_string(opt.productItem);
                if (!seen.count(key))
                    seen[key] = now;
                needs.push_back({"materials", std::to_string(opt.productItem), 0,
                                 money > reserved ? money - reserved : 0, seen[key]});
                break;
            }
    }

    // Worst three equipped slots. Score 0 means empty - the neediest state of
    // all. Amounts stay 0 until the buy side can price an upgrade (BRD E8).
    {
        StatsWeightCalculator calc(bot);
        std::vector<std::pair<float, uint8>> scored;
        for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
        {
            if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
                continue;
            float score = 0;
            if (Item* eq = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
                score = calc.CalculateItem(eq->GetEntry(), eq->GetItemRandomPropertyId(), slot);
            scored.push_back({score, slot});
        }
        std::sort(scored.begin(), scored.end());
        for (size_t i = 0; i < 3 && i < scored.size(); ++i)
        {
            std::string key = "gear|" + std::to_string(scored[i].second);
            if (!seen.count(key))
                seen[key] = now;
            needs.push_back({"gear", std::to_string(scored[i].second), 0,
                             money > reserved ? money - reserved : 0, seen[key]});
        }
    }

    // The disposal split, asked once per bot and shared by both branches that care.
    // Computing it separately inside each let the errand that starts the walk and the
    // action waiting at the other end disagree about the same bag: a bot could satisfy
    // "has something depositable" and then correctly plan to bank nothing, and only
    // find that out after walking up to three thousand yards. Memoised rather than
    // computed up front because most bots are claimed by an earlier branch and never
    // need it, and PlanDisposal enumerates recipes.
    std::unordered_set<ObjectGuid> forVault;
    uint32 listableShare = 0;
    bool splitDone = false;
    auto const split = [&]
    {
        if (splitDone)
            return;
        splitDone = true;
        PlanDisposal(bot, forVault);
        listableShare = CountListable(bot, &forVault);
    };

    // Urgent-errand verdicts. Mailbox first: uncollected proceeds are the
    // cheapest money a bot can get, and collection unblocks everything else.
    if (sPreemptEnabled && sMailboxVisits && HasCollectibleMail(bot))
    {
        auto it = sMailboxSpawns.find(uint16(bot->GetMapId()));
        if (it != sMailboxSpawns.end())
        {
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
            Verdict v{VERDICT_MAILBOX, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
            for (MailboxSpawn const& mb : it->second)
            {
                float const dx = mb.x - bx, dy = mb.y - by;
                float const d2 = dx * dx + dy * dy;
                if (d2 < best)
                {
                    best = d2;
                    v.hasFar = true;
                    v.x = mb.x;
                    v.y = mb.y;
                    v.z = mb.z;
                    v.goEntry = mb.entry;
                    v.goSpawnId = mb.spawnId;
                }
            }
            if (v.hasFar)
                sVerdictBuild[guid] = v;
        }
    }
    // E3.2: a funded training bill sends the bot to its class trainer - spend
    // trips only run when the money is actually there.
    else if (sPreemptEnabled && sPaidTraining && [&]
             {
                 for (Need const& n : needs)
                     if (n.type == "training")
                         return n.amount > 0 && n.freeMoney >= n.amount;
                 return false;
             }())
    {
        auto it = sTrainerSpawns.find(uint16(bot->GetMapId()));
        if (it != sTrainerSpawns.end())
        {
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
            Verdict v{VERDICT_TRAINER, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
            for (TrainerSpawn const& ts : it->second)
            {
                float const dx = ts.x - bx, dy = ts.y - by;
                float const d2 = dx * dx + dy * dy;
                if (d2 >= best)
                    continue;
                Trainer::Trainer* trainer = sObjectMgr->GetTrainer(ts.entry);
                if (!trainer || !trainer->IsTrainerValidForPlayer(bot))
                    continue;
                best = d2;
                v.hasFar = true;
                v.x = ts.x;
                v.y = ts.y;
                v.z = ts.z;
            }
            if (v.hasFar)
                sVerdictBuild[guid] = v;
        }
    }
    // E7 focus trip: reagents complete but the recipe needs an anvil, forge or
    // fire - walk to the nearest matching focus object and craft there.
    else if (sPreemptEnabled && sCraftEnabled && [&]
             {
                 std::vector<CraftOption> options;
                 CraftPlanner::Enumerate(bot, options, 12);
                 auto it = sFocusSpawns.find(uint16(bot->GetMapId()));
                 if (it == sFocusSpawns.end())
                     return false;
                 for (CraftOption const& opt : options)
                 {
                     if (!opt.craftableNow || !opt.spellFocus)
                         continue;
                     float const bx = bot->GetPositionX(), by = bot->GetPositionY();
                     float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
                     Verdict v{VERDICT_FOCUS, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
                     for (FocusSpawn const& fs : it->second)
                     {
                         if (fs.focusId != opt.spellFocus)
                             continue;
                         float const dx = fs.x - bx, dy = fs.y - by;
                         float const d2 = dx * dx + dy * dy;
                         if (d2 < best)
                         {
                             best = d2;
                             v.hasFar = true;
                             v.x = fs.x;
                             v.y = fs.y;
                             v.z = fs.z;
                             v.goEntry = fs.entry;
                             v.goSpawnId = fs.spawnId;
                         }
                     }
                     if (v.hasFar)
                     {
                         sVerdictBuild[guid] = v;
                         return true;
                     }
                 }
                 return false;
             }())
    {
        // Verdict stored inside the lambda; nothing further here.
    }
    // E4.2/E9: a bag worth listing sends the bot to the auction house on purpose
    // instead of waiting for wander luck - but only the part of the bag that is
    // actually the auction house's. The two errands used to bid for the whole bot,
    // and this one was asked first, so a farmer with thirty stacks listed four per
    // visit and never once fell below the threshold; it could not reach the bank
    // branch at all. Now the split is decided per item first (see PlanDisposal) and
    // each branch counts only its own share.
    else if (sPreemptEnabled && sAhEnabled && (split(), listableShare >= sAhMinItemsForTrip))
    {
        auto it = sAuctioneerSpawns.find(uint16(bot->GetMapId()));
        if (it != sAuctioneerSpawns.end())
        {
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
            Verdict v{VERDICT_AH, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
            for (auto const& p : it->second)
            {
                float const dx = p[0] - bx, dy = p[1] - by;
                float const d2 = dx * dx + dy * dy;
                if (d2 < best)
                {
                    best = d2;
                    v.hasFar = true;
                    v.x = p[0];
                    v.y = p[1];
                    v.z = p[2];
                }
            }
            if (v.hasFar)
                sVerdictBuild[guid] = v;
        }
    }
    // E6.3b: strategic materials in the bags send the bot to a banker on purpose.
    // The personal deposit, the guild-tab share and the gold tithe all ride this one
    // visit, which is exactly why guild vaults sat empty - the plumbing was correct
    // and nothing ever decided to walk to it. Still below the auction-house trip, so
    // selling wins the beat for a bag that is stock; a bag holding something the
    // vault is short of no longer reaches that branch at all, because the split
    // above has already taken those items out of its count.
    // The trigger asks exactly what the arrival will ask. Asking a different question
    // here - "is anything depositable" while the action decides per item, solvency
    // first - is how a bot earned a bank errand, walked to a banker and had nothing to
    // do when it got there. A guilded bot goes when the split set something aside; an
    // unguilded one has no split, so its personal-bank rule stands unchanged. Either
    // may also go to fetch a reagent out of its own vault.
    else if (sPreemptEnabled && sBankEnabled &&
             ((split(), !forVault.empty()) ||
              (!bot->GetGuildId() && BankDepositAction::CountDepositable(bot, 1)) ||
              BankDepositAction::HasVaultedReagent(bot)))
    {
        auto it = sBankerSpawns.find(uint16(bot->GetMapId()));
        if (it != sBankerSpawns.end())
        {
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
            Verdict v{VERDICT_BANK, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
            for (auto const& p : it->second)
            {
                float const dx = p[0] - bx, dy = p[1] - by;
                float const d2 = dx * dx + dy * dy;
                if (d2 < best)
                {
                    best = d2;
                    v.hasFar = true;
                    v.x = p[0];
                    v.y = p[1];
                    v.z = p[2];
                }
            }
            if (v.hasFar)
                sVerdictBuild[guid] = v;
        }
    }
    // E2.1 bag pressure, E2.5 broke-with-sellables. The build map swaps into
    // the map-thread mirror when the pass completes. The whole test lives in
    // the condition: a config-only else-if would consume the chain for every
    // bot and starve any branch below it (that killed E7.5's first cut).
    else if (sPreemptEnabled && sVendorEnabled && [&]
             {
                 uint32 freeSlots, totalSlots;
                 uint64 trashValue;
                 BagPressure(bot, freeSlots, totalSlots, trashValue);
                 bool const pressure =
                     totalSlots && freeSlots * 100 / totalSlots < sVendorFreeSlotsPct;
                 bool broke = false;
                 for (Need const& n : needs)
                     if (n.amount > 0 && n.freeMoney < n.amount)
                     {
                         broke = true;
                         break;
                     }
                 if (!(pressure && trashValue > 0) &&
                     !(broke && trashValue >= sVendorBrokeMinValue))
                     return false;
                 Verdict v{VERDICT_VENDOR, false, 0, 0, 0, 0};
                 // Far leg: nearest same-map vendor spawn within range, so the
                 // preemption has somewhere to walk when its nearby scan is empty.
                 auto it = sVendorSpawns.find(uint16(bot->GetMapId()));
                 if (it != sVendorSpawns.end())
                 {
                     float const bx = bot->GetPositionX(), by = bot->GetPositionY();
                     float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
                     for (auto const& p : it->second)
                     {
                         float const dx = p[0] - bx, dy = p[1] - by;
                         float const d2 = dx * dx + dy * dy;
                         if (d2 < best)
                         {
                             best = d2;
                             v.hasFar = true;
                             v.farMap = uint16(bot->GetMapId());
                             v.x = p[0];
                             v.y = p[1];
                             v.z = p[2];
                         }
                     }
                 }
                 sVerdictBuild[guid] = v;
                 return true;
             }())
    {
        // Verdict stored inside the lambda; nothing further here.
    }
    // E7.5 needs-driven gathering: a gatherer with a live reason - reagents
    // its own recipes are short of, or an unfunded need to farm toward - walks
    // to node geography matched to its skill tier. No reason, no trip; that is
    // the operator's no-pointless-gathering rule, enforced structurally.
    else if (sPreemptEnabled && sGatherEnabled)
    {
        auto it = sGatherSpawns.find(uint16(bot->GetMapId()));
        uint32 freeSlots = 0, totalSlots = 0;
        uint64 trashValue = 0;
        if (it != sGatherSpawns.end())
            BagPressure(bot, freeSlots, totalSlots, trashValue);
        // Full bags cannot carry a harvest home.
        if (it != sGatherSpawns.end() && freeSlots >= 4)
        {
            bool unfunded = false;
            for (Need const& n : needs)
                if (n.amount > 0 && n.freeMoney < n.amount)
                {
                    unfunded = true;
                    break;
                }
            bool wantsHerbs = false, wantsOre = false;
            {
                std::vector<CraftOption> options;
                CraftPlanner::Enumerate(bot, options, 12);
                for (CraftOption const& opt : options)
                    for (auto const& miss : opt.missing)
                        if (ItemTemplate const* tmpl = sObjectMgr->GetItemTemplate(miss.first))
                        {
                            if (tmpl->Class != ITEM_CLASS_TRADE_GOODS)
                                continue;
                            if (tmpl->SubClass == ITEM_SUBCLASS_HERB)
                                wantsHerbs = true;
                            else if (tmpl->SubClass == ITEM_SUBCLASS_METAL_STONE)
                                wantsOre = true;
                        }
            }
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
            Verdict v{VERDICT_GATHER, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
            for (uint32 skill : {uint32(SKILL_HERBALISM), uint32(SKILL_MINING)})
            {
                if (!bot->HasSkill(skill))
                    continue;
                uint32 const value = bot->GetSkillValue(skill);
                if (!value)
                    continue;
                bool const wantsMats = skill == SKILL_HERBALISM ? wantsHerbs : wantsOre;
                if (!wantsMats && !unfunded)
                    continue;
                for (GatherSpawn const& gs : it->second)
                {
                    if (gs.skill != skill || gs.reqSkill > value ||
                        value - gs.reqSkill > sGatherMaxTierGap)
                        continue;
                    float const dx = gs.x - bx, dy = gs.y - by;
                    float const d2 = dx * dx + dy * dy;
                    if (d2 < best)
                    {
                        best = d2;
                        v.hasFar = true;
                        v.x = gs.x;
                        v.y = gs.y;
                        v.z = gs.z;
                        v.goEntry = gs.entry;
                        v.goSpawnId = gs.spawnId;
                    }
                }
            }
            if (v.hasFar)
                sVerdictBuild[guid] = v;
        }
    }
    // E10 rare hunt. Ranks below every branch above - each of those keeps a bot
    // solvent, fed or supplied, and none should ever lose a beat to a trophy -
    // but deliberately NOT as another link in that else-if chain. The gather
    // branch above ends in a config-only condition, so once gathering is armed
    // the chain terminates there for every bot on the realm and anything
    // appended below it is dead code. That is the same trap the chain's own
    // comment records killing E7.5's first cut, and it killed this branch's
    // first cut too: 2500 bots online, not one rare verdict issued. Asking
    // whether the chain actually stored anything gives last place without
    // depending on any branch above to decline politely.
    if (sPreemptEnabled && sRareEnabled && sRareIssued < sRareMaxConcurrent &&
        !sVerdictBuild.count(guid) && bot->IsAlive() && ArchetypeOf(bot) == PERSONA_HUNTER)
    {
        auto it = sRareSpawns.find(uint16(bot->GetMapId()));
        if (it != sRareSpawns.end())
        {
            Map* map = bot->GetMap();
            uint32 const level = bot->GetLevel();
            // An elite hits far above its level. Solo, the bot wants a real
            // margin; three or more can take one on the chin. The operator's
            // rule, structurally: a level 20 walking at a level 60 rare elite
            // is a death, not a hunt.
            uint32 const party = bot->GetGroup() ? bot->GetGroup()->GetMembersCount() : 1;
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float const maxD = float(sRareFarMaxYards) * float(sRareFarMaxYards);

            RareSpawn const* shortlist[RARE_SHORTLIST] = {};
            float shortDist[RARE_SHORTLIST] = {};
            uint32 held = 0;
            // What this bot is already walking towards. A hunt is a commitment,
            // and re-deciding it every pass is not a smaller version of hunting -
            // it is not hunting at all. Measured: bot 129 alternated between two
            // Storm Peaks rares 200 and 315 yards away for half an hour and
            // closed 29 yards, because the shortlist is drawn by index and the
            // index moves whenever the candidate count does.
            auto const aimIt = sRareAimed.find(guid);
            uint32 const currentAim = aimIt != sRareAimed.end() ? aimIt->second : 0;
            RareSpawn const* sticky = nullptr;

            for (RareSpawn const& rs : it->second)
            {
                if (rs.rank == CREATURE_ELITE_RAREELITE)
                {
                    if (party >= 3 ? uint32(rs.level) > level + 2
                                   : uint32(rs.level) + 5 > level)
                        continue;
                }
                else if (uint32(rs.level) > level + 2)
                    continue;
                if (uint32(rs.level) + sRareMaxLevelGap < level)
                    continue;

                float const dx = rs.x - bx, dy = rs.y - by;
                float const d2 = dx * dx + dy * dy;
                if (d2 >= maxD)
                    continue;
                // Dead and waiting out its timer. Rares respawn slowly, so this
                // is the difference between a hunt and a walk to a corpse - and
                // the respawn table answers even for a grid nobody has loaded,
                // which a live-object lookup cannot do.
                if (map && map->GetCreatureRespawnTime(rs.spawnId))
                    continue;

                // Still eligible and still the one we chose: keep walking. Every
                // filter above has already run on it, so this can only hold a
                // target that is alive, in range and the right size.
                if (currentAim && rs.spawnId == currentAim)
                {
                    sticky = &rs;
                    continue;
                }

                if (held < RARE_SHORTLIST)
                    ++held;
                else if (d2 >= shortDist[RARE_SHORTLIST - 1])
                    continue;
                uint32 slot = held - 1;
                while (slot && shortDist[slot - 1] > d2)
                {
                    shortDist[slot] = shortDist[slot - 1];
                    shortlist[slot] = shortlist[slot - 1];
                    --slot;
                }
                shortDist[slot] = d2;
                shortlist[slot] = &rs;
            }

            if (sticky || held)
            {
                RareSpawn const* pick = sticky ? sticky : shortlist[(guid * 2654435761u) % held];
                sVerdictBuild[guid] = Verdict{VERDICT_RARE, true, uint16(bot->GetMapId()),
                                              pick->x,      pick->y, pick->z,
                                              pick->entry,  pick->spawnId};
                ++sRareIssued;
                // One event per commitment, not one per pass: a bot still
                // walking toward the same rare a minute later is not news.
                uint32& aimed = sRareAimed[guid];
                if (aimed != pick->spawnId)
                {
                    aimed = pick->spawnId;
                    CreatureTemplate const* proto = sObjectMgr->GetCreatureTemplate(pick->entry);
                    NeedsLedger::LogEvent("rare_hunt", guid, pick->entry, uint32(pick->level),
                                          "aim|" + std::to_string(bot->GetMapId()) + "|" +
                                              (proto ? proto->Name : std::string()));
                }
            }
        }
    }

    // E11 shopping trip. The operator's rule was "occasionally check AH for
    // items", not "hunt for gear", so this is the weakest claim on the realm:
    // it only ever takes a bot the whole chain above declined, and it dodges
    // that chain for the reason the rare hunt records - the gather branch ends
    // in a config-only condition, so anything appended below it is dead code
    // the moment gathering is armed.
    //
    // Two gates beyond that. The funded gear need means the bot is not walking
    // to a city to window-shop or to spend its repair money. The realm-wide
    // ceiling means a market that suddenly fills with gear cannot empty the
    // world into Ironforge. Nothing here ends the trip on a timer: the reason
    // ends it. When the upgrade is bought - by this bot or by anyone else - the
    // screen above stops pricing one, and the verdict is simply not rebuilt.
    // The ceiling counts trips STARTED this pass, not trips held. A bot already
    // walking to a market keeps its errand regardless, because dropping it to
    // hand the slot to a stranger is the same mistake the cooldown made from the
    // other direction - a trip abandoned halfway is a trip that bought nothing.
    // The pool this draws from is bounded anyway: only bots the chain declined
    // that can also afford a real upgrade, measured at a couple of hundred.
    if (sPreemptEnabled && sShopEnabled && gearUpgradeFunded && !sVerdictBuild.count(guid) &&
        (sShopAimed.count(guid) || sShopIssued < sShopMaxConcurrent))
    {
        auto it = sAuctioneerSpawns.find(uint16(bot->GetMapId()));
        if (it != sAuctioneerSpawns.end())
        {
            float const bx = bot->GetPositionX(), by = bot->GetPositionY();
            float best = float(sVendorFarMaxYards) * float(sVendorFarMaxYards);
            Verdict v{VERDICT_SHOP, false, uint16(bot->GetMapId()), 0, 0, 0, 0, 0};
            for (auto const& p : it->second)
            {
                float const dx = p[0] - bx, dy = p[1] - by;
                float const d2 = dx * dx + dy * dy;
                if (d2 < best)
                {
                    best = d2;
                    v.hasFar = true;
                    v.x = p[0];
                    v.y = p[1];
                    v.z = p[2];
                }
            }
            if (v.hasFar)
            {
                sVerdictBuild[guid] = v;
                // The trip is re-issued every pass for as long as the reason
                // holds, so the event has to fire on a CHANGED aim or the log
                // becomes one row a minute per shopper. A bot whose upgrade
                // someone else buys simply stops pricing one and the verdict
                // falls away on its own - no cooldown needed to end the trip.
                auto const known = sShopAimed.find(guid);
                if (known == sShopAimed.end())
                    ++sShopIssued;
                uint64& aimed = sShopAimed[guid];
                if (aimed != gearUpgradePrice)
                {
                    aimed = gearUpgradePrice;
                    NeedsLedger::LogEvent("gear_shop", guid, 0, 0,
                                          "aim|" + std::to_string(bot->GetMapId()) + "|" +
                                              std::to_string(gearUpgradePrice));
                }
            }
        }
    }

    // A bot that is not shopping this pass forgets what it was shopping for.
    // Without this the aim log only ever grows: a finished shopper would keep
    // bypassing the ceiling as though still in flight, and its stale price would
    // suppress the event for its next real commitment.
    {
        auto const vit = sVerdictBuild.find(guid);
        if (vit == sVerdictBuild.end() || vit->second.kind != VERDICT_SHOP)
            sShopAimed.erase(guid);
    }

    // Persona: recorded every pass for the census, and stamped onto whatever
    // verdict the chain just stored so the map thread's duty roll is one
    // mirror lookup. A single stamp point here covers every branch above.
    {
        uint8 const persona = ArchetypeOf(bot);
        sPersona[guid] = persona;
        auto vit = sVerdictBuild.find(guid);
        if (vit != sVerdictBuild.end())
            vit->second.dutyPct = uint8(
                std::min<uint32>(100, PERSONA_DUTY[persona] * sDutyScale / 100));
    }

    // What this bot still cannot pay for, summed across its unfunded needs. The
    // disposal split spends this out of the bags before it sets anything aside for
    // the guild, so a broke bot sells and a solvent one can afford to stock a vault.
    {
        uint64 shortfall = 0;
        for (Need const& n : needs)
            if (n.amount > 0 && n.freeMoney < n.amount)
                shortfall += n.amount - n.freeMoney;
        if (shortfall)
            sShortfallBuild[guid] = shortfall;
    }

    // Drop first-seen entries for needs that no longer exist, so a re-arising
    // need gets a fresh clock.
    for (auto it = seen.begin(); it != seen.end();)
    {
        bool alive = false;
        for (Need const& n : needs)
            if (n.type + "|" + n.target == it->first)
            {
                alive = true;
                break;
            }
        it = alive ? std::next(it) : seen.erase(it);
    }

    sNeeds[guid] = std::move(needs);
}

void NeedsLedger::WriteTelemetry()
{
    CharacterDatabase.Execute("TRUNCATE TABLE aetherion_needs");
    CharacterDatabase.Execute("TRUNCATE TABLE aetherion_gold_now");

    std::ostringstream needsSql, goldSql;
    uint32 needsBatched = 0, goldBatched = 0;
    std::map<uint8, std::pair<uint32, uint64>> bands;

    auto flushNeeds = [&]()
    {
        if (!needsBatched)
            return;
        CharacterDatabase.Execute(
            "INSERT INTO aetherion_needs (guid, need_type, target, amount, free_money, since_ts)"
            " VALUES " + needsSql.str());
        needsSql.str("");
        needsBatched = 0;
    };
    auto flushGold = [&]()
    {
        if (!goldBatched)
            return;
        CharacterDatabase.Execute(
            "INSERT INTO aetherion_gold_now (guid, level, money) VALUES " + goldSql.str());
        goldSql.str("");
        goldBatched = 0;
    };

    for (auto it = sRandomPlayerbotMgr.GetPlayerBotsBegin();
         it != sRandomPlayerbotMgr.GetPlayerBotsEnd(); ++it)
    {
        Player* bot = it->second;
        if (!bot || !bot->IsInWorld())
            continue;
        uint32 guid = bot->GetGUID().GetCounter();

        if (goldBatched++)
            goldSql << ",";
        goldSql << "(" << guid << "," << uint32(bot->GetLevel()) << "," << bot->GetMoney() << ")";
        if (goldBatched >= 400)
            flushGold();

        auto& band = bands[bot->GetLevel() / 10];
        ++band.first;
        band.second += bot->GetMoney();

        auto needsIt = sNeeds.find(guid);
        if (needsIt == sNeeds.end())
            continue;
        for (Need const& n : needsIt->second)
        {
            if (needsBatched++)
                needsSql << ",";
            needsSql << "(" << guid << ",'" << n.type << "','" << n.target << "',"
                     << n.amount << "," << n.freeMoney << "," << n.since << ")";
            if (needsBatched >= 400)
                flushNeeds();
        }
    }
    flushNeeds();
    flushGold();

    // Band history every ~5 passes (about five minutes at default cadence), with a
    // fortnight retention so the income-rate panel has a bounded window.
    if (_passes % 5 == 0)
    {
        std::ostringstream bandSql;
        uint32 n = 0;
        for (auto const& b : bands)
        {
            if (n++)
                bandSql << ",";
            bandSql << "(UNIX_TIMESTAMP()," << uint32(b.first) << "," << b.second.first << ","
                    << b.second.second << ")";
        }
        if (n)
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_gold_bands (ts, band, n, total) VALUES " + bandSql.str());
        if (_passes % 500 == 0)
            CharacterDatabase.Execute(
                "DELETE FROM aetherion_gold_bands WHERE ts < UNIX_TIMESTAMP() - 14*86400");
    }

    // Publish this pass's urgent-errand verdicts for map-thread preemption -
    // and into the ledger, so "who holds which errand" is a query instead of
    // a guess (the zero-mail-pickups investigation).
    {
        std::lock_guard<std::mutex> lock(sVerdictMx);
        sVerdictMirror.swap(sVerdictBuild);
        sShortfallMirror.swap(sShortfallBuild);

        static char const* KINDS[] = {"none",  "vendor", "mailbox", "trainer",
                                      "ah",    "focus",  "gather",  "bank",
                                      "rare",  "shop"};
        std::ostringstream errandSql;
        uint32 batched = 0;
        for (auto const& pair : sVerdictMirror)
        {
            if (batched++)
                errandSql << ",";
            errandSql << "(" << pair.first << ",'errand','"
                      << KINDS[pair.second.kind < std::size(KINDS) ? pair.second.kind : 0] << "',0,0,"
                      << "UNIX_TIMESTAMP())";
            if (batched >= 400)
            {
                CharacterDatabase.Execute(
                    "INSERT INTO aetherion_needs (guid, need_type, target, amount, free_money,"
                    " since_ts) VALUES " + errandSql.str());
                errandSql.str("");
                batched = 0;
            }
        }
        if (batched)
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_needs (guid, need_type, target, amount, free_money,"
                " since_ts) VALUES " + errandSql.str());
    }
    sVerdictBuild.clear();
    sShortfallBuild.clear();

    // Persona census: who the population IS, alongside what it wants. The
    // duty percent rides in amount so the dashboard can show appetite too.
    {
        std::ostringstream personaSql;
        uint32 batched = 0;
        for (auto const& pair : sPersona)
        {
            if (batched++)
                personaSql << ",";
            personaSql << "(" << pair.first << ",'persona','"
                       << PERSONA_NAMES[pair.second <= PERSONA_LAST ? pair.second : 0] << "',"
                       << uint32(std::min<uint32>(
                              100, PERSONA_DUTY[pair.second <= PERSONA_LAST ? pair.second : 0] *
                                       sDutyScale / 100))
                       << ",0,UNIX_TIMESTAMP())";
            if (batched >= 400)
            {
                CharacterDatabase.Execute(
                    "INSERT INTO aetherion_needs (guid, need_type, target, amount, free_money,"
                    " since_ts) VALUES " + personaSql.str());
                personaSql.str("");
                batched = 0;
            }
        }
        if (batched)
            CharacterDatabase.Execute(
                "INSERT INTO aetherion_needs (guid, need_type, target, amount, free_money,"
                " since_ts) VALUES " + personaSql.str());
    }

    // E8.1: snapshot the live listings. World thread only - these maps have no
    // locking and the expiry sweep iterates them on this same thread.
    {
        std::vector<AhListing> fresh;
        for (AuctionHouseId houseId :
             {AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral})
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            if (!house)
                continue;
            for (auto const& pair : house->GetAuctions())
            {
                AuctionEntry const* entry = pair.second;
                if (!entry || !entry->buyout)
                    continue;
                Item* item = sAuctionMgr->GetAItem(entry->item_guid);
                fresh.push_back({entry->Id, entry->item_template,
                                 item ? item->GetItemRandomPropertyId() : 0, entry->itemCount,
                                 uint32(entry->buyout),
                                 sCharacterCache->GetCharacterAccountIdByGuid(entry->owner),
                                 uint8(houseId)});
            }
        }
        // E11: distil the gear half before publishing, on this same thread, so
        // the per-bot upgrade screen never re-derives it 2500 times.
        std::vector<GearOffer> gear;
        for (AhListing const& l : fresh)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(l.item);
            if (!proto || !l.buyout)
                continue;
            uint8 slots[2] = {0, 0};
            if (!EquipSlotsFor(uint8(proto->InventoryType), slots))
                continue;
            gear.push_back({l.item, l.randomPropertyId, l.buyout, l.houseId,
                            uint8(proto->InventoryType), uint16(proto->ItemLevel)});
        }
        sGearOffers.swap(gear);

        std::lock_guard<std::mutex> lock(sListingsMx);
        sListings.swap(fresh);
    }
}

std::vector<NeedsLedger::AhListing> NeedsLedger::ListingsForHouse(uint8 houseId)
{
    std::vector<AhListing> out;
    std::lock_guard<std::mutex> lock(sListingsMx);
    for (AhListing const& l : sListings)
        if (l.houseId == houseId)
            out.push_back(l);
    return out;
}
