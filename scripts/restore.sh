#!/usr/bin/env bash
# Restore a dump produced by backup.sh. Destructive: it overwrites the live
# databases, so it demands the dump path and an explicit typed confirmation.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

DUMP=${1:-}
[[ -f $DUMP ]] || die "usage: restore.sh /opt/warcraft/backups/daily/db-<stamp>.sql.gz"

cat <<WARN
===========================================================
 DATA LOSS WARNING
 About to OVERWRITE these live databases on $(hostname):
   acore_auth  acore_characters  acore_world  acore_playerbots
 Source dump: $DUMP
 All character, bot and auction state newer than this dump
 will be permanently lost.
===========================================================
WARN
read -rp "Type the realm name (${REALM_NAME}) to proceed: " answer
[[ $answer == "$REALM_NAME" ]] || die "aborted"

log "stopping game services (database stays up)"
dc stop ac-worldserver ac-authserver

log "restoring"
gunzip -c "$DUMP" | dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD"

log "starting game services"
dc start ac-authserver ac-worldserver
log "restore complete - verify with scripts/status.sh"
