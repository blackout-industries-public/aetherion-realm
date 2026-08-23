#!/usr/bin/env bash
# Seed acore_world.dungeonencounter_dbc from the client's own DungeonEncounter.dbc.
#
# WHY this exists: completedEncounters is a bitmask, and the only table carrying the
# (MapID, Difficulty, Bit) -> boss mapping is this one, which ships EMPTY on this build -
# as does every other *_dbc stub. The worldserver reads the real DBC from disk and
# bypasses them entirely. Without this seed a boss kill can be counted but never named.
#
# The bit is NOT derivable by ordering: ranked by entry id it is wrong for 16 encounters,
# by OrderIndex for 61. The real Bit column is the only source.
#
# WHY it runs here rather than on the realm: parsing needs Python, the realm host has no
# uv, and this repo is uv-only. Running the parse on the workstation keeps the host free
# of Python tooling and sends nothing but SQL over the wire.
#
# Like the map art, this is derived from a client the operator already owns and is never
# committed. Idempotent - it upserts, so it is safe to re-run after an upgrade.
set -euo pipefail

REALM=${REALM_SSH:-nlucansk@10.10.25.193}
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*"; }

log "fetching DungeonEncounter.dbc from the worldserver"
ssh "$REALM" 'docker cp ac-worldserver:/azerothcore/env/dist/data/dbc/DungeonEncounter.dbc /tmp/de.dbc >/dev/null && cat /tmp/de.dbc; rm -f /tmp/de.dbc' > "$WORK/DungeonEncounter.dbc"
[[ -s $WORK/DungeonEncounter.dbc ]] || { echo "empty DBC" >&2; exit 1; }

log "parsing"
uv run --no-project --quiet python3 - "$WORK/DungeonEncounter.dbc" > "$WORK/seed.sql" <<'PY'
import struct, sys

raw = open(sys.argv[1], "rb").read()
magic, count, fields, record_size, _string_size = struct.unpack_from("<4siiii", raw, 0)
if magic != b"WDBC":
    sys.exit(f"not a DBC file: {magic!r}")

HEADER = 20
strings = raw[HEADER + count * record_size:]

def text(offset: int) -> str:
    return strings[offset:strings.index(b"\0", offset)].decode("utf-8", "replace")

# Field order from DBCStructure.h DungeonEncounterEntry: ID, MapID, Difficulty,
# OrderIndex, Bit, Name[16]. Field 4 is what the worldserver shifts into the mask
# (Map.cpp: 1 << encounter->dbcEntry->encounterIndex).
rows = []
for i in range(count):
    v = struct.unpack_from(f"<{fields}i", raw, HEADER + i * record_size)
    name = text(v[5]).replace("\\", "\\\\").replace("'", "''")
    rows.append(f"({v[0]},{v[1]},{v[2]},{v[3]},{v[4]},'{name}')")

print("INSERT INTO acore_world.dungeonencounter_dbc")
print("  (ID, MapID, Difficulty, OrderIndex, Bit, Name_Lang_enUS) VALUES")
print(",\n".join(rows))
# Upsert, not truncate-and-replace: no destructive statement is needed at all, and a
# re-run after an upgrade refreshes rows in place.
print("ON DUPLICATE KEY UPDATE MapID=VALUES(MapID), Difficulty=VALUES(Difficulty),")
print("  OrderIndex=VALUES(OrderIndex), Bit=VALUES(Bit),")
print("  Name_Lang_enUS=VALUES(Name_Lang_enUS);")
PY

PARSED=$(grep -c '^(' "$WORK/seed.sql")
[[ $PARSED -ge 500 ]] || { echo "only $PARSED encounters parsed; refusing to seed" >&2; exit 1; }
log "parsed $PARSED encounters"

ssh "$REALM" 'set -a; . /opt/warcraft/azerothcore/.env; set +a
docker exec -i ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD"' < "$WORK/seed.sql"

ssh "$REALM" 'set -a; . /opt/warcraft/azerothcore/.env; set +a
docker exec ac-database mysql -uroot -p"$DOCKER_DB_ROOT_PASSWORD" -N -B -e "
SELECT CONCAT(\"dungeonencounter_dbc now holds \", COUNT(*), \" rows\") FROM acore_world.dungeonencounter_dbc;"' 2>/dev/null
