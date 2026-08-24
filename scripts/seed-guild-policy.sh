#!/usr/bin/env bash
# Provision every BOT guild's bank with best-practice policy (Economy E8):
#   - a first bank tab named Materials, so item deposits have somewhere to land
#   - all member ranks may repair on guild funds (GR_RIGHT_WITHDRAW_REPAIR,
#     0x40000) with a modest daily allowance; the officer rank (rid 1) may also
#     withdraw gold (0x80000) with a bigger one
#   - tab rights: officers get full tab access with withdrawals, everyone else
#     deposits and views only
#
# Bot guilds are identified by their leader belonging to a rndbot% account, so
# a human player's own guild is never touched. Idempotent: rights are OR-ed,
# allowances raised with GREATEST, rows inserted only where missing. Guild
# data loads at worldserver startup - restart to apply.
set -euo pipefail
cd "$(dirname "$0")/.."
. scripts/lib.sh

SQL=$(cat <<'EOF'
CREATE TEMPORARY TABLE bot_guilds AS
SELECT g.guildid FROM guild g
JOIN characters c ON c.guid = g.leaderguid
JOIN acore_auth.account a ON a.id = c.account
WHERE a.username LIKE 'rndbot%';

INSERT INTO guild_bank_tab (guildid, TabId, TabName, TabIcon, TabText)
SELECT bg.guildid, tabs.TabId, tabs.TabName, '', ''
FROM bot_guilds bg
JOIN (SELECT 0 AS TabId, 'Materials' AS TabName
      UNION ALL SELECT 1, 'Consumables'
      UNION ALL SELECT 2, 'Gear') tabs
WHERE NOT EXISTS (SELECT 1 FROM guild_bank_tab t
                  WHERE t.guildid = bg.guildid AND t.TabId = tabs.TabId);

-- A vault that opens empty teaches nobody to use it. A modest starter
-- treasury (100g) makes repair allowances real from day one; everything
-- after that is tithes in, repairs out.
UPDATE guild g JOIN bot_guilds bg ON bg.guildid = g.guildid
SET g.BankMoney = GREATEST(g.BankMoney, 1000000);

UPDATE guild_rank gr JOIN bot_guilds bg ON bg.guildid = gr.guildid
SET gr.rights = gr.rights | 262144,
    gr.BankMoneyPerDay = GREATEST(gr.BankMoneyPerDay, 50000)
WHERE gr.rid >= 2;

UPDATE guild_rank gr JOIN bot_guilds bg ON bg.guildid = gr.guildid
SET gr.rights = gr.rights | 786432,
    gr.BankMoneyPerDay = GREATEST(gr.BankMoneyPerDay, 200000)
WHERE gr.rid = 1;

INSERT INTO guild_bank_right (guildid, TabId, rid, gbright, SlotPerDay)
SELECT gr.guildid, tabs.TabId, gr.rid, IF(gr.rid = 1, 255, 3), IF(gr.rid = 1, 25, 0)
FROM guild_rank gr
JOIN bot_guilds bg ON bg.guildid = gr.guildid
JOIN (SELECT 0 AS TabId UNION ALL SELECT 1 UNION ALL SELECT 2) tabs
WHERE gr.rid >= 1
ON DUPLICATE KEY UPDATE
  gbright = GREATEST(gbright, VALUES(gbright)),
  SlotPerDay = GREATEST(SlotPerDay, VALUES(SlotPerDay));

SELECT COUNT(*) AS bot_guilds FROM bot_guilds;
SELECT COUNT(*) AS provisioned_tabs FROM guild_bank_tab t JOIN bot_guilds bg ON bg.guildid = t.guildid WHERE t.TabId = 0;
EOF
)

docker exec ac-database sh -c "mysql -uroot -p\"\$MYSQL_ROOT_PASSWORD\" acore_characters -e \"$SQL\""
echo "[seed-guild-policy] done - restart worldserver to load guild rights"
