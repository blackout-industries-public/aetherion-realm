#!/usr/bin/env bash
# Give the auction house bot a dedicated market identity.
#
# mod-ah-bot resolves its sellers with `SELECT guid FROM characters WHERE account = N`
# and then builds a Player around that GUID, so the account must own a real character
# row. Rather than hand-writing ~90 columns, clone an existing valid row and rewrite
# only identity: every NOT NULL column then carries a value the core already accepts.
#
# The account name deliberately does not use the random-bot prefix, so Playerbots
# will not adopt this character and play it.
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

AH_ACCOUNT=${AHBOT_ACCOUNT_NAME:-ahbot}
AH_CHAR=${AHBOT_CHAR_NAME:-Marketeer}

# Default schema matters: CREATE TEMPORARY TABLE fails with "No database selected"
# without one. stderr is kept so failures are visible rather than silent.
mysql_q() {
    dc exec -T ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" -N -B acore_characters -e "$1" \
        2> >(grep -v "Using a password" >&2)
}

log "creating account '$AH_ACCOUNT'"
"$ROOT/scripts/create-account.sh" "$AH_ACCOUNT" "$(openssl rand -hex 16)" 0 -1 >/dev/null

ACC_ID=$(mysql_q "SELECT id FROM acore_auth.account WHERE username=UPPER('$AH_ACCOUNT');")
[[ -n $ACC_ID ]] || die "could not resolve account id for $AH_ACCOUNT"

EXISTING=$(mysql_q "SELECT guid FROM acore_characters.characters WHERE account=$ACC_ID LIMIT 1;")
if [[ -n $EXISTING ]]; then
    CHAR_GUID=$EXISTING
    log "market character already exists (guid $CHAR_GUID)"
else
    log "cloning a template character row"
    mysql_q "
      SET @src := (SELECT guid FROM acore_characters.characters ORDER BY guid LIMIT 1);
      SET @new := (SELECT MAX(guid)+1 FROM acore_characters.characters);
      CREATE TEMPORARY TABLE ahtmp AS SELECT * FROM acore_characters.characters WHERE guid=@src;
      -- identity is the only thing that changes; every other column stays as the core wrote it
      UPDATE ahtmp SET guid=@new, account=$ACC_ID, name='$AH_CHAR', online=0, money=0,
                       totaltime=0, leveltime=0, logout_time=UNIX_TIMESTAMP();
      INSERT INTO acore_characters.characters SELECT * FROM ahtmp;
    "
    CHAR_GUID=$(mysql_q "SELECT guid FROM acore_characters.characters WHERE account=$ACC_ID LIMIT 1;")
fi
[[ -n $CHAR_GUID ]] || die "market character was not created"

log "market identity: account=$ACC_ID character=$CHAR_GUID ($AH_CHAR)"

# Persist so configure.sh keeps applying it on every future run.
sed -i "/^AHBOT_ACCOUNT_ID=/d;/^AHBOT_CHAR_GUID=/d" "$ROOT/overlay/.env"
printf 'AHBOT_ACCOUNT_ID=%s\nAHBOT_CHAR_GUID=%s\n' "$ACC_ID" "$CHAR_GUID" >> "$ROOT/overlay/.env"
cp "$ROOT/overlay/.env" "$AC/.env"; chmod 600 "$ROOT/overlay/.env" "$AC/.env"

log "run scripts/configure.sh then restart ac-worldserver"
