#!/usr/bin/env bash
# Phase 1 observability baseline (BRD s15). Plain text so it works over ssh and
# can be scraped later without pulling in a metrics stack.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

q() { dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" -N -B -e "$1" 2>/dev/null; }

echo "== host"
uptime
free -h | head -2
df -h / | tail -1

echo; echo "== containers"
dc ps --format 'table {{.Name}}\t{{.Status}}'

echo; echo "== database size (MB)"
q "SELECT table_schema, ROUND(SUM(data_length+index_length)/1048576) FROM information_schema.tables WHERE table_schema LIKE 'acore%' GROUP BY table_schema;"

echo; echo "== characters online / total"
q "SELECT SUM(online), COUNT(*) FROM acore_characters.characters;"

echo; echo "== bot population by level bracket"
# Group by the alias: MySQL 8.4 runs ONLY_FULL_GROUP_BY and will not accept the
# repeated expression as functionally dependent.
q "SELECT CONCAT(FLOOR((level-1)/10)*10+1,'-',FLOOR((level-1)/10)*10+10) AS bracket, COUNT(*)
   FROM acore_characters.characters GROUP BY bracket ORDER BY MIN(level);"

echo; echo "== bot population by class"
q "SELECT class, COUNT(*) FROM acore_characters.characters GROUP BY class ORDER BY 2 DESC;"

echo; echo "== top 15 zones by population"
q "SELECT zone, COUNT(*) FROM acore_characters.characters GROUP BY zone ORDER BY 2 DESC LIMIT 15;"

echo; echo "== auction house"
q "SELECT COUNT(*) AS listings, ROUND(AVG(buyoutprice)/10000) AS avg_buyout_gold FROM acore_characters.auctionhouse;"

echo; echo "== worldserver restarts / recent errors"
docker inspect -f '{{.RestartCount}} restarts, started {{.State.StartedAt}}' ac-worldserver
dc logs --tail 200 ac-worldserver 2>/dev/null | grep -icE 'error|crash' | sed 's/^/error lines in last 200: /'
