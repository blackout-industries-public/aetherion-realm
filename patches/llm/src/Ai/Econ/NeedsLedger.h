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

#include <mutex>
#include <string>
#include <unordered_map>
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
    static uint8 constexpr VERDICT_AH = 4;
    static uint8 constexpr VERDICT_FOCUS = 5;
    static uint8 constexpr VERDICT_GATHER = 6;
    static uint8 UrgentVerdict(uint32 guid);
    // Persona duty roll for one idle beat: the held verdict kind when the
    // bot's disposition claims the beat, VERDICT_NONE otherwise (or when no
    // verdict is held). Map-thread safe.
    static uint8 ClaimErrandBeat(uint32 guid);
    static bool PaidTraining();
    static bool CraftEnabled();

    // Disposition, asked rather than stored: it is derived from the bot's own
    // professions plus a hash of its guid, so it is stable for life and costs
    // nothing to look up. The party assembler asks this to decide who goes back
    // for old content. Safe from any thread - it only reads the bot's skills.
    static bool IsCollector(Player* bot);

    // E8.2: what the guild vault holds, refreshed from guild_bank_item on the
    // world thread every few minutes. Crafters use it to pull missing
    // reagents back OUT of the Materials tab. Map-thread safe.
    static bool FindVaultReagent(uint32 guildId, uint32 itemEntry, uint8& tab, uint8& slot);

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
    struct VaultSlot
    {
        uint8 tab;
        uint8 slot;
        uint32 entry;
    };
    std::unordered_map<uint32, std::vector<VaultSlot>> _vault;   // guildid -> slots
    std::mutex _vaultMutex;
    uint32 _vaultAgeMs{0};
    void RefreshVaultCache();

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
