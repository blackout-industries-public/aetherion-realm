/*
 * Aetherion economy: the needs ledger (Economy BRD E1).
 *
 * Observe-only. Walks the bot fleet in shards on the world thread, computes what
 * each bot genuinely needs money for (repair, training, mount, gear, ammo), and
 * exports the ledger plus per-bot gold snapshots to aetherion_* tables for the
 * dashboard. Changes no bot behavior: the whole point of shipping this first is
 * watching needs arise before anything acts on them.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_NEEDSLEDGER_H
#define AETHERION_NEEDSLEDGER_H

#include "Define.h"

#include <string>
#include <vector>

class Player;

class NeedsLedger
{
public:
    static NeedsLedger* instance();

    void LoadConfig();
    void Tick(uint32 diff);

    // Emitter for economy events (Economy BRD E1.8). Called from patched
    // destruction sites; safe from the world thread only. No-op unless
    // AiPlayerbot.Econ.Events.Enabled.
    static void LogEvent(char const* kind, uint32 guid, uint32 item, uint32 count,
                         std::string const& detail);

    // Urgent-errand verdicts (Economy BRD E2.0/E2.5). Computed on the world
    // thread from the needs pass, read by map-thread RPG preemption through a
    // mutex-guarded mirror - map threads must never touch the ledger itself.
    static uint8 constexpr VERDICT_NONE = 0;
    static uint8 constexpr VERDICT_VENDOR = 1;
    static uint8 constexpr VERDICT_MAILBOX = 2;
    static uint8 constexpr VERDICT_TRAINER = 3;
    static uint8 UrgentVerdict(uint32 guid);
    static bool PaidTraining();
    static bool CraftEnabled();

    // E5.2: the mailbox errand's target - a real mailbox GameObject chosen on
    // the world thread. Near case hands back the spawn identity so the trip can
    // target the exact object; far case only needs the walk position.
    static bool MailboxTarget(uint32 guid, uint32 botMap, uint32& entry, uint32& spawnId,
                              float& x, float& y, float& z);

    // E2.1b far leg: nearest vendor spawn for a bot beyond the nearby scan,
    // resolved on the world thread from a startup spawn index. Returns false
    // when no vendor within the configured range shares the bot's map.
    static bool FarVendor(uint32 guid, uint32 botMap, float& x, float& y, float& z);

    // Behavior gates read by patched sites (map or world thread; plain bools
    // written once at LoadConfig).
    static bool SellOnVendorVisit();
    static bool ProtectTradeGoods();
    static bool PaidRepairs();

    // E4.4: auction lifecycle hooks (listed/sold/bought/expired events). Called
    // once from AddPlayerbotsScripts; the script gates itself on the events key.
    static void RegisterAuctionScript();

    // E8.1: a per-pass snapshot of live listings, so map-thread buy decisions
    // never touch the unlocked auction maps. ownerAccount powers the
    // self-dealing guard the core cannot provide for always-online bots.
    struct AhListing
    {
        uint32 auctionId;
        uint32 item;
        int32 randomPropertyId;
        uint32 count;
        uint32 buyout;
        uint32 ownerAccount;
        uint8 houseId;
    };
    static std::vector<AhListing> ListingsForHouse(uint8 houseId);

private:
    void EnsureTables();
    void ProcessShard();
    void ComputeNeeds(Player* bot);
    void WriteTelemetry();
    void BuildTrainerCache();

    bool _enabled = false;
    uint32 _tickMs = 6000;
    uint32 _shards = 10;
    uint32 _elapsed = 0;
    uint32 _shard = 0;
    uint32 _passes = 0;
};

#define sNeedsLedger NeedsLedger::instance()

#endif
