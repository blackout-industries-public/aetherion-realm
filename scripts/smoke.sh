#!/usr/bin/env bash
# Mechanical checks for the BRD s18 criteria that do not need a game client.
# Exit code is the number of failures, so it can gate an upgrade.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

FAIL=0
check() { # check <label> <command...>
    local label=$1; shift
    if "$@" >/dev/null 2>&1; then printf 'PASS  %s\n' "$label"
    else printf 'FAIL  %s\n' "$label"; FAIL=$((FAIL+1)); fi
}
q() { dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" -N -B -e "$1"; }
gt0() { [[ $(q "$1" 2>/dev/null | head -1) -gt 0 ]]; }

check "authserver container running"   bash -c '[[ $(docker inspect -f "{{.State.Running}}" ac-authserver) == true ]]'
check "worldserver container running"  bash -c '[[ $(docker inspect -f "{{.State.Running}}" ac-worldserver) == true ]]'
check "database container healthy"     bash -c '[[ $(docker inspect -f "{{.State.Health.Status}}" ac-database) == healthy ]]'
check "auth port 3724 accepts TCP"     bash -c 'timeout 3 bash -c "</dev/tcp/127.0.0.1/3724"'
check "world port 8085 accepts TCP"    bash -c 'timeout 3 bash -c "</dev/tcp/127.0.0.1/8085"'

check "acore_auth populated"           gt0 "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_auth'"
check "acore_world populated"          gt0 "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_world'"
check "acore_characters populated"     gt0 "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_characters'"
check "acore_playerbots populated"     gt0 "SELECT COUNT(*) FROM information_schema.tables WHERE table_schema='acore_playerbots'"
# Must be a function, not `bash -c`: `dc` and `q` are shell functions and do not
# survive into a subshell.
realmlist_ok() { [[ $(q "SELECT address FROM acore_auth.realmlist WHERE id=1" | head -1) == "$REALM_ADDRESS" ]]; }
check "realmlist points at ${REALM_ADDRESS}" realmlist_ok
check "bot characters exist"           gt0 "SELECT COUNT(*) FROM acore_characters.characters"
check "bots currently online"          gt0 "SELECT COUNT(*) FROM acore_characters.characters WHERE online=1"
check "client data extracted"          bash -c 'docker run --rm -v ac-client-data:/d alpine sh -c "[ -d /d/maps ] && [ -d /d/mmaps ] && [ -d /d/vmaps ] && [ -d /d/dbc ]"'

printf '\n%d failure(s)\n' "$FAIL"
exit "$FAIL"
