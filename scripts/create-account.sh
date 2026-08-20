#!/usr/bin/env bash
# Create (or reset) a game account without the worldserver console, which needs an
# interactive TTY and so cannot be driven from automation.
#
# Reproduces AccountMgr::CreateAccount exactly: username and password are upper-cased,
# salt is 32 random bytes, verifier = g^SHA1(salt || SHA1(USER:PASS)) mod N, and both
# BigNumber values are stored little-endian to match BigNumber::ToByteArray.
#
# usage: create-account.sh <username> <password> [gmlevel] [realmid]
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

USER_NAME=${1:?username required}
PASSWORD=${2:?password required}
GMLEVEL=${3:-3}
REALMID=${4:--1}

# Computed in a throwaway container so the host needs no Python toolchain.
read -r SALT VERIFIER < <(docker run --rm -e U="$USER_NAME" -e P="$PASSWORD" python:3-alpine python -c '
import hashlib, os
N = 0x894B645E89E1535BBDAD5B8B290650530801B18EBFBF5E8FAB3C82872A3E9BB7
g = 7
u = os.environ["U"].upper().encode()
p = os.environ["P"].upper().encode()
salt = os.urandom(32)
inner = hashlib.sha1(u + b":" + p).digest()
x = int.from_bytes(hashlib.sha1(salt + inner).digest(), "little")
v = pow(g, x, N).to_bytes(32, "little")
print(salt.hex(), v.hex())
')
[[ ${#SALT} == 64 && ${#VERIFIER} == 64 ]] || die "SRP6 computation failed"

dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" acore_auth <<SQL
INSERT INTO account (username, salt, verifier, expansion)
VALUES (UPPER('$USER_NAME'), UNHEX('$SALT'), UNHEX('$VERIFIER'), 2)
ON DUPLICATE KEY UPDATE salt=UNHEX('$SALT'), verifier=UNHEX('$VERIFIER');
INSERT INTO account_access (id, gmlevel, RealmID)
SELECT id, $GMLEVEL, $REALMID FROM account WHERE username=UPPER('$USER_NAME')
ON DUPLICATE KEY UPDATE gmlevel=$GMLEVEL;
SQL

log "account '$USER_NAME' ready (gmlevel $GMLEVEL, realm $REALMID)"
