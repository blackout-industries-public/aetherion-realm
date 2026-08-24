#!/usr/bin/env python3
"""Economy BRD E1: wire the NeedsLedger and the destruction emitters.

Observe-only - Tick computes and exports needs, the emitters record item
destruction that already happens. Both are config-gated (AiPlayerbot.Econ.*)
and default off, so the patched binary is behavior-identical until armed.

Ordering (BRD 3.4): runs last in apply.sh. Playerbots.cpp anchors exist after
the step-2 inline patch; PlayerbotFactory.cpp and DestroyItemAction.cpp anchors
are pristine-source text untouched by the earlier patchers.
"""
import sys

module = sys.argv[1]


def patch(path, edits, marker):
    src = open(path).read()
    if marker in src:
        print(f"econ already applied: {path}")
        return
    for anchor, replacement in edits:
        assert src.count(anchor) == 1, f"anchor not unique in {path}: {anchor[:60]!r}"
        src = src.replace(anchor, replacement, 1)
    open(path, "w").write(src)
    print(f"patched {path}")


SCRIPT = f"{module}/src/Script/Playerbots.cpp"
patch(SCRIPT, [
    ('#include "PartyAssembler.h"\n',
     '#include "PartyAssembler.h"\n#include "NeedsLedger.h"\n'),
    ("        sPartyAssembler->Tick(diff);\n",
     "        sPartyAssembler->Tick(diff);\n        sNeedsLedger->Tick(diff);\n"),
    ("        sPartyAssembler->LoadConfig();\n",
     "        sPartyAssembler->LoadConfig();\n        sNeedsLedger->LoadConfig();\n"),
    ("    new PlayerbotsScript();",
     "    new PlayerbotsScript();\n    NeedsLedger::RegisterAuctionScript();"),
    # Marker must be a string only THIS patch writes: the harvest hook in the
    # apply.sh script section also says "NeedsLedger", and a bare class-name
    # marker made this whole block skip as already-applied.
], "sNeedsLedger->Tick")

FACTORY = f"{module}/src/Bot/Factory/PlayerbotFactory.cpp"
VISIT = """        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        return true;
    }"""
KEEP = """    bool Visit(Item* item) override
    {
        uint32 id = item->GetTemplate()->ItemId;
        if (CanKeep(id))"""
patch(FACTORY, [
    ('#include "PlayerbotFactory.h"\n',
     '#include "PlayerbotFactory.h"\n#include "NeedsLedger.h"\n'),
    # E2.3/E2.4: every stack survives, so this sits before CanKeep's
    # one-stack-per-id dedup.
    (KEEP,
     """    bool Visit(Item* item) override
    {
        uint32 id = item->GetTemplate()->ItemId;
        // Economy: trade goods are the economy's raw supply - the factory
        // reset must not destroy them once the economy is armed.
        if (NeedsLedger::ProtectTradeGoods() &&
            item->GetTemplate()->Class == ITEM_CLASS_TRADE_GOODS)
            return true;
        if (CanKeep(id))"""),
    (VISIT,
     """        // Economy BRD E1.8: destruction is the baseline the economy is judged
        // against, so it is recorded before it happens.
        NeedsLedger::LogEvent("destroy", bot->GetGUID().GetCounter(), id,
                              item->GetCount(), "clear_inventory");
        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);
        return true;
    }"""),
], "NeedsLedger")

# E4.1a: register the AH sell action alongside its old-rpg sibling.
ACTIONCTX = f"{module}/src/Ai/Base/ActionContext.h"
patch(ACTIONCTX, [
    ('#include "AddLootAction.h"\n',
     '#include "AddLootAction.h"\n#include "AhBuyAction.h"\n#include "AhSellAction.h"\n'
     '#include "BankDepositAction.h"\n#include "EconCraftAction.h"\n#include "MailCollectAction.h"\n'),
    ('        creators["rpg sell"] = &ActionContext::rpg_sell;',
     '        creators["rpg sell"] = &ActionContext::rpg_sell;\n'
     '        creators["ah sell"] = &ActionContext::ah_sell;\n'
     '        creators["ah buy"] = &ActionContext::ah_buy;\n'
     '        creators["mail collect"] = &ActionContext::mail_collect;\n'
     '        creators["econ craft"] = &ActionContext::econ_craft;\n'
     '        creators["bank deposit"] = &ActionContext::bank_deposit;'),
    ('    static Action* rpg_sell(PlayerbotAI* botAI) { return new RpgSellAction(botAI); }',
     '    static Action* rpg_sell(PlayerbotAI* botAI) { return new RpgSellAction(botAI); }\n'
     '    static Action* ah_sell(PlayerbotAI* botAI) { return new AhSellAction(botAI); }\n'
     '    static Action* ah_buy(PlayerbotAI* botAI) { return new AhBuyAction(botAI); }\n'
     '    static Action* mail_collect(PlayerbotAI* botAI) { return new MailCollectAction(botAI); }\n'
     '    static Action* econ_craft(PlayerbotAI* botAI) { return new EconCraftAction(botAI); }\n'
     '    static Action* bank_deposit(PlayerbotAI* botAI) { return new BankDepositAction(botAI); }'),
], "AhSellAction")

SELLACT = f"{module}/src/Ai/Base/Actions/SellAction.cpp"
patch(SELLACT, [
    ('#include "SellAction.h"\n',
     '#include "SellAction.h"\n#include "NeedsLedger.h"\n'),
    # E2.2: the mode the existing visitors could not express - greys plus
    # vendor-usage, never ITEM_USAGE_AH (the herbs the AH epics will list).
    ("""bool SellAction::Execute(Event event)
{
    std::string const text = event.getParam();""",
     """class SellTrashItemsVisitor : public SellItemsVisitor
{
public:
    SellTrashItemsVisitor(SellAction* action, AiObjectContext* con) : SellItemsVisitor(action)
    {
        context = con;
    }

    AiObjectContext* context;

    bool Visit(Item* item) override
    {
        if (item->GetTemplate()->Quality == ITEM_QUALITY_POOR)
            return SellItemsVisitor::Visit(item);
        ItemUsage usage = context->GetValue<ItemUsage>("item usage", item->GetEntry())->Get();
        if (usage != ITEM_USAGE_VENDOR)
            return true;
        return SellItemsVisitor::Visit(item);
    }
};

bool SellAction::Execute(Event event)
{
    std::string const text = event.getParam();
    if (text == "trash")
    {
        SellTrashItemsVisitor visitor(this, context);
        IterateItems(&visitor);
        return true;
    }"""),
    # E1.8/E2.2 acceptance instrumentation: the vendor-sale event with the real
    # money delta, captured before the gold-cheat restore can distort it.
    ("""        bot->GetSession()->HandleSellItemOpcode(nicePacket);""",
     """        bot->GetSession()->HandleSellItemOpcode(nicePacket);

        NeedsLedger::LogEvent("vendor_sell", bot->GetGUID().GetCounter(),
                              item->GetTemplate()->ItemId, count,
                              std::to_string(int64(bot->GetMoney()) - int64(botMoney)));"""),
], "NeedsLedger")

DESTROY = f"{module}/src/Ai/Base/Actions/DestroyItemAction.cpp"
SMART = """        botAI->TellMaster(out);

        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);"""
patch(DESTROY, [
    ('#include "DestroyItemAction.h"\n',
     '#include "DestroyItemAction.h"\n#include "NeedsLedger.h"\n'),
    (SMART,
     """        botAI->TellMaster(out);

        NeedsLedger::LogEvent("destroy", bot->GetGUID().GetCounter(),
                              item->GetTemplate()->ItemId, item->GetCount(),
                              "smart_destroy");
        bot->DestroyItem(item->GetBagSlot(), item->GetSlot(), true);"""),
], "NeedsLedger")
