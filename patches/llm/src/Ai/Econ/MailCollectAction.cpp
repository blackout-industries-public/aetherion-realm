/*
 * Aetherion economy: mailbox collection action (Economy BRD E5.2).
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#include "MailCollectAction.h"
#include "NeedsLedger.h"

#include "Config.h"
#include "GameObject.h"
#include "Mail.h"
#include "Opcodes.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "WorldPacket.h"
#include "WorldSession.h"

#include <ctime>
#include <string>
#include <vector>

namespace
{
bool MailboxEnabled()
{
    static bool const enabled =
        sConfigMgr->GetOption<bool>("AiPlayerbot.Econ.Mailbox.Enabled", false);
    return enabled;
}

GameObject* NearbyMailbox(Player* bot, AiObjectContext* context)
{
    GuidVector gos = context->GetValue<GuidVector>("nearest game objects")->Get();
    for (ObjectGuid const guid : gos)
        if (GameObject* go = bot->GetGameObjectIfCanInteractWith(guid, GAMEOBJECT_TYPE_MAILBOX))
            return go;
    return nullptr;
}
}  // namespace

bool MailCollectAction::isUseful()
{
    return MailboxEnabled() && !bot->GetMails().empty();
}

bool MailCollectAction::Execute(Event /*event*/)
{
    if (!MailboxEnabled())
        return false;

    GameObject* mailbox = NearbyMailbox(bot, context);
    if (!mailbox)
        return false;

    ObjectGuid const mailboxGuid = mailbox->GetGUID();
    time_t const now = time(nullptr);
    uint32 queued = 0;

    for (Mail* m : bot->GetMails())
    {
        if (!m || m->state == MAIL_STATE_DELETED || m->deliver_time > now || m->COD)
            continue;

        if (m->money)
        {
            WorldPacket* packet = new WorldPacket(CMSG_MAIL_TAKE_MONEY, 8 + 4);
            *packet << mailboxGuid;
            *packet << uint32(m->messageID);
            bot->GetSession()->QueuePacket(packet);
            ++queued;
        }

        for (MailItemInfo const& info : m->items)
        {
            WorldPacket* packet = new WorldPacket(CMSG_MAIL_TAKE_ITEM, 8 + 4 + 4);
            *packet << mailboxGuid;
            *packet << uint32(m->messageID);
            *packet << uint32(info.item_guid);
            bot->GetSession()->QueuePacket(packet);
            ++queued;
        }
    }

    if (queued)
        NeedsLedger::LogEvent("mail_collect", bot->GetGUID().GetCounter(), 0, queued,
                              "at mailbox");
    return queued > 0;
}
