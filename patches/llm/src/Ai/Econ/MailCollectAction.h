/*
 * Aetherion economy: mailbox collection action (Economy BRD E5.2).
 *
 * The bot stands at a real mailbox GameObject and takes its mail through the
 * real CMSG_MAIL_TAKE_MONEY / CMSG_MAIL_TAKE_ITEM handlers - proximity checks,
 * bag checks and persistence all core-side. This is what retires the E5.1
 * remote-collection exception.
 *
 * Licensed under AGPL v3, like the rest of the Aetherion patches.
 */
#ifndef AETHERION_MAILCOLLECTACTION_H
#define AETHERION_MAILCOLLECTACTION_H

#include "Action.h"

class PlayerbotAI;

class MailCollectAction : public Action
{
public:
    MailCollectAction(PlayerbotAI* botAI) : Action(botAI, "mail collect") {}

    bool Execute(Event event) override;
    bool isUseful() override;
};

#endif
