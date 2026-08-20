#!/usr/bin/env bash
# Controlled upgrade: back up, re-pin to the commits currently in overlay/.env,
# rebuild, migrate, smoke test. Rollback is: restore the dump and git checkout the
# previous commits recorded in MANIFEST.md.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack

log "step 1/5: backup"
"$ROOT/scripts/backup.sh"

log "step 2/5: re-pin to overlay/.env revisions"
"$ROOT/scripts/bootstrap.sh"

log "step 3/5: rebuild"
dc build

log "step 4/5: recreate (db-import runs migrations)"
dc up -d

log "step 5/5: smoke test"
sleep 30
"$ROOT/scripts/status.sh"
log "review the output above, then accept or roll back with scripts/restore.sh"
