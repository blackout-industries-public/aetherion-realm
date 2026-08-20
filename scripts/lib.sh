# Shared helpers. Sourced, never executed.
set -euo pipefail

ROOT=/opt/warcraft
AC=$ROOT/azerothcore
CONF=$ROOT/config

log()  { printf '[%s] %s\n' "$(date -u +%H:%M:%S)" "$*" >&2; }
die()  { printf '[FATAL] %s\n' "$*" >&2; exit 1; }

dc() { docker compose --project-directory "$AC" "$@"; }

# Rewrite "Key = value" in an AzerothCore .conf, uncommenting it if needed.
# Appends when the key is absent so it also works on module configs that ship
# only a subset of their keys.
set_conf() {
    local file=$1 key=$2 value=$3
    [[ -f $file ]] || die "config not found: $file"
    if grep -qE "^[[:space:]]*#?[[:space:]]*${key}[[:space:]]*=" "$file"; then
        KEY="$key" VALUE="$value" awk '
            BEGIN { k=ENVIRON["KEY"]; v=ENVIRON["VALUE"]; done=0 }
            !done && $0 ~ "^[[:space:]]*#?[[:space:]]*" k "[[:space:]]*=" {
                print k " = " v; done=1; next
            }
            { print }
        ' "$file" > "$file.tmp" && mv "$file.tmp" "$file"
    else
        printf '%s = %s\n' "$key" "$value" >> "$file"
    fi
}

require_stack() {
    [[ -d $AC ]] || die "stack not bootstrapped; run scripts/bootstrap.sh first"
}
