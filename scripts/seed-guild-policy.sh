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
SELECT bg.guildid, 0, 'Materials', '', ''
FROM bot_guilds bg
WHERE NOT EXISTS (SELECT 1 FROM guild_bank_tab t WHERE t.guildid = bg.guildid AND t.TabId = 0);

UPDATE guild_rank gr JOIN bot_guilds bg ON bg.guildid = gr.guildid
SET gr.rights = gr.rights | 0x40000,
    gr.BankMoneyPerDay = GREATEST(gr.BankMoneyPerDay, 50000)
WHERE gr.rid >= 2;

UPDATE guild_rank gr JOIN bot_guilds bg ON bg.guildid = gr.guildid
SET gr.rights = gr.rights | 0xC0000,
    gr.BankMoneyPerDay = GREATEST(gr.BankMoneyPerDay, 200000)
WHERE gr.rid = 1;

INSERT INTO guild_bank_right (guildid, TabId, rid, gbright, SlotPerDay)
SELECT gr.guildid, 0, gr.rid, IF(gr.rid = 1, 0xFF, 0x03), IF(gr.rid = 1, 25, 0)
FROM guild_rank gr JOIN bot_guilds bg ON bg.guildid = gr.guildid
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
