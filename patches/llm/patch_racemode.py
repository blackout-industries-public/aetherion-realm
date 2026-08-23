#!/usr/bin/env python3
"""Race mode: bots keep only what they earn.

Entirely gated on AiPlayerbot.DisableRandomLevels, so the patched binary behaves
identically to the unpatched one until race mode is armed. With it armed:

- RandomPlayerbotMgr::Randomize becomes a one-time init: the factory runs once for
  a freshly created character (pinned to RandombotStartingLevel) and then never
  again. Without this, every bot below level 3 is re-rolled by RandomizeFirst on
  every periodic randomize - an earned level 2 goes back to 1 and the quest log is
  wiped - and every bot above it gets gold re-rolls and consumable refills.
- The factory's hardcoded grants are cut: 10-50g per level of gold, four free
  bags, and the food/potion/reagent/consumable stocking. A fresh bot owns exactly
  its start outfit.
- Refresh() keeps its function (spells, skills, pets, ammo, reagents - bots have
  no trainer behaviour, so removing these would break classes, not purify them)
  but loses ClearInventory, the free supplies and the money floor.

Takes the module root; patches RandomPlayerbotMgr.cpp and PlayerbotFactory.cpp.
"""
import sys

module = sys.argv[1]

GATE = "sPlayerbotAIConfig.disableRandomLevels"


def patch(path, edits, marker):
    src = open(path).read()
    if marker in src:
        print(f"race mode already applied: {path}")
        return
    for anchor, replacement in edits:
        assert src.count(anchor) == 1, f"anchor not unique in {path}: {anchor[:60]!r}"
        src = src.replace(anchor, replacement, 1)
    open(path, "w").write(src)
    print(f"patched {path}")


MGR = f"{module}/src/Bot/RandomPlayerbotMgr.cpp"

# RandomPlayerbotMgr::Refresh trickles free gold on every teleport/revive event -
# small, but a money printer over race timescales. Separate marker so the gate can
# be added to an already-dispatched file.
TRICKLE = """    uint32 money = bot->GetMoney();
    bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));"""
patch(MGR, [(TRICKLE, f"""    // Race mode: no gold trickle - income is loot and quests only.
    if (!{GATE})
    {{
        uint32 money = bot->GetMoney();
        bot->SetMoney(money + 500 * sqrt(urand(1, bot->GetLevel() * 5)));
    }}""")], "// Race mode: no gold trickle")

DISPATCH_ANCHOR = """void RandomPlayerbotMgr::Randomize(Player* bot)
{
    if (bot->InBattleground())
        return;
"""
DISPATCH_NEW = DISPATCH_ANCHOR + f"""
    // Race mode: the factory ran once at creation and never runs again - level,
    // quests, gear and gold stay exactly what the bot earned. The GetLevel guard
    // backs up the stored "level" event value, which expires and so cannot alone
    // mean "initialized".
    if ({GATE})
    {{
        if (!GetValue(bot, "level") &&
            (bot->GetLevel() <= 1 ||
             (bot->getClass() == CLASS_DEATH_KNIGHT &&
              bot->GetLevel() <= sWorld->getIntConfig(CONFIG_START_HEROIC_PLAYER_LEVEL))))
        {{
            RandomizeFirst(bot);
        }}
        return;
    }}
"""
patch(MGR, [(DISPATCH_ANCHOR, DISPATCH_NEW)], "// Race mode: the factory ran once")

FACTORY = f"{module}/src/Bot/Factory/PlayerbotFactory.cpp"

edits = []

# One-time init grants: a fresh character owns its start outfit and nothing else.
for thing, call in [
    ("bags", "InitBags();"),
    ("ammo", "InitAmmo();"),
    ("food", "InitFood();"),
    ("potions", "InitPotions();"),
    ("reagents", "InitReagents();"),
    ("keys", "InitKeyring();"),
    ("consumables", "InitConsumables();"),
]:
    anchor = f'LOG_DEBUG("playerbots", "Initializing {thing}...");\n    {call}'
    edits.append((anchor, anchor.replace(
        f"\n    {call}",
        f"\n    if (!{GATE})\n        {call}"
    )))

# Wealth is earned, never granted.
MONEY = "    bot->SetMoney(urand(level * 100000, level * 5 * 100000));"
edits.append((MONEY, f"    if (!{GATE})\n    {MONEY}"))

# Refresh: keep the class function, cut the freebies and the inventory wipe.
CLEAR = "    InitAttunementQuests();\n    ClearInventory();"
edits.append((CLEAR, f"""    InitAttunementQuests();
    // Race mode: never destroy what a bot earned.
    if (!{GATE})
        ClearInventory();"""))

RESTOCK = """    InitAmmo();
    InitFood();
    InitReagents();
    InitConsumables();
    InitPotions();
    InitPet();"""
edits.append((RESTOCK, f"""    InitAmmo();
    InitReagents();
    if (!{GATE})
    {{
        InitFood();
        InitConsumables();
        InitPotions();
    }}
    InitPet();"""))

FLOOR = """    uint32 money = urand(level * 1000, level * 5 * 1000);
    if (bot->GetMoney() < money)
        bot->SetMoney(money);"""
edits.append((FLOOR, f"""    if (!{GATE})
    {{
        uint32 money = urand(level * 1000, level * 5 * 1000);
        if (bot->GetMoney() < money)
            bot->SetMoney(money);
    }}"""))

patch(FACTORY, edits, "// Race mode: never destroy")
