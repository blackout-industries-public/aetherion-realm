#include "RareHuntAction.h"

#include "Creature.h"
#include "Map.h"
#include "NeedsLedger.h"
#include "Player.h"
#include "Playerbots.h"

bool RareHuntAction::Execute(Event /*event*/)
{
    if (!NeedsLedger::RareHuntEnabled() || !bot->IsAlive())
        return false;

    // Already fighting something. Yanking the bot onto a new target mid-pull is
    // how a hunt turns into two deaths instead of one kill.
    if (bot->IsInCombat())
        return false;

    uint32 entry, spawnId;
    float x, y, z;
    if (!NeedsLedger::RareTarget(bot->GetGUID().GetCounter(), bot->GetMapId(), entry, spawnId, x,
                                 y, z))
        return false;

    // DB spawns carry a map-generated low guid, so an ObjectGuid built from the
    // spawn id resolves nothing; the map's spawn-id store is the only way back
    // to the live object. The entry check matters because the store is keyed on
    // spawn id alone and a multispawn can put a different creature on it.
    Creature* rare = nullptr;
    auto bounds = bot->GetMap()->GetCreatureBySpawnIdStore().equal_range(spawnId);
    for (auto it = bounds.first; it != bounds.second; ++it)
        if (it->second && it->second->GetEntry() == entry)
        {
            rare = it->second;
            break;
        }

    if (!rare || !rare->IsAlive() || !bot->IsValidAttackTarget(rare))
        return false;

    return Attack(rare);
}
