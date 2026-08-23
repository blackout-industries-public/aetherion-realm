#include "NeedsLedger.h"

#include "AuctionHouseMgr.h"
#include "Bag.h"
#include "CharacterCache.h"
#include "CraftPlanner.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "Mail.h"
#include "ScriptMgr.h"
#include "DBCStores.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ItemUsageValue.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "Playerbots.h"
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

    struct Verdict
    {
        uint8 kind;
        bool hasFar;
        uint16 farMap;
        float x, y, z;
        // Mailbox target identity (E5.2); zero when the verdict is not mailbox.
        uint32 goEntry = 0;
        uint32 goSpawnId = 0;
    };

    // World thread builds verdicts during the pass and swaps them into the
    // mirror at pass end; map-thread preemption only ever locks and looks up.
    std::mutex sVerdictMx;
    std::unordered_map<uint32, Verdict> sVerdictMirror;
    std::unordered_map<uint32, Verdict> sVerdictBuild;

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
    bool sAhEnabled = false;
    uint32 sAhMinItemsForTrip = 3;

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

    // E8.1 listings mirror, rebuilt each pass on the world thread.
    std::mutex sListingsMx;
    std::vector<NeedsLedger::AhListing> sListings;

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
    uint32 CountListable(Player* bot)
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
    sAhMinItemsForTrip = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Ah.MinItemsForTrip", 3);
    sVendorFreeSlotsPct = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Vendor.FreeSlotsPct", 20);
    sVendorBrokeMinValue = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Vendor.BrokeMinValue", 500);
    sVendorFarMaxYards = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Vendor.FarMaxYards", 3000);
    sGatherEnabled = sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Gather.Enabled", false);
    sGatherMaxTierGap = sConfigMgr->GetOption<uint32>("AiPlayerbot.Econ.Gather.MaxTierGap", 150);

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

bool NeedsLedger::SellOnVendorVisit() { return sVendorEnabled; }
bool NeedsLedger::ProtectTradeGoods() { return sProtectTradeGoods; }
bool NeedsLedger::PaidRepairs() { return sPaidRepairs; }
bool NeedsLedger::PaidTraining() { return sPaidTraining; }
bool NeedsLedger::CraftEnabled() { return sCraftEnabled; }

void NeedsLedger::RegisterAuctionScript()
{
    // Registration is one-way in the script system; the hooks no-op through the
    // LogEvent gate when events are off.
    new EconAuctionScript();
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
    }
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
    if (PlayerbotAI* botAI = GET_PLAYERBOT_AI(bot))
    {
        uint8 const house = bot->GetTeamId() == TEAM_ALLIANCE ? 2 : 6;
        uint64 cheapest = 0;
        uint32 evaluated = 0;
        for (AhListing const& l : ListingsForHouse(house))
        {
            if (++evaluated > 40)
                break;
            if (!l.buyout || (cheapest && l.buyout >= cheapest))
                continue;
            std::string qualifier =
                std::to_string(l.item) + "," + std::to_string(l.randomPropertyId);
            ItemUsage usage =
                botAI->GetAiObjectContext()->GetValue<ItemUsage>("item usage", qualifier)->Get();
            if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE)
                cheapest = l.buyout;
        }
        if (cheapest)
            push("gear", "upgrade", cheapest);
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
    // E4.2: a bag worth listing sends the bot to the auction house on purpose
    // instead of waiting for wander luck.
    else if (sPreemptEnabled && sAhEnabled && CountListable(bot) >= sAhMinItemsForTrip)
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

        static char const* KINDS[] = {"none", "vendor", "mailbox", "trainer", "ah", "focus", "gather"};
        std::ostringstream errandSql;
        uint32 batched = 0;
        for (auto const& pair : sVerdictMirror)
        {
            if (batched++)
                errandSql << ",";
            errandSql << "(" << pair.first << ",'errand','"
                      << KINDS[pair.second.kind <= 6 ? pair.second.kind : 0] << "',0,0,"
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
