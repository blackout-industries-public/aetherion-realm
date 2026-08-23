#!/usr/bin/env bash
# Clone and pin the AzerothCore Playerbot fork plus modules, then stage compose
# config. Idempotent: safe to re-run to re-pin after editing overlay/.env.
source "$(dirname "$(readlink -f "$0")")/lib.sh"

ENV_FILE=$ROOT/overlay/.env
[[ -f $ENV_FILE ]] || {
    log "generating $ENV_FILE with a fresh DB password"
    install -m 600 /dev/null "$ENV_FILE"
    sed "s|^DOCKER_DB_ROOT_PASSWORD=.*|DOCKER_DB_ROOT_PASSWORD=$(openssl rand -hex 24)|" \
        "$ROOT/overlay/.env.example" > "$ENV_FILE"
}
chmod 600 "$ENV_FILE"
set -a; source "$ENV_FILE"; set +a

mkdir -p "$CONF" "$ROOT/logs" "$ROOT/backups"

pin() {  # pin <dir> <repo> <commit> [branch]
    local dir=$1 repo=$2 commit=$3 branch=${4:-}
    if [[ ! -d $dir/.git ]]; then
        log "cloning $repo -> $dir"
        if [[ -n $branch ]]; then git clone --branch "$branch" "$repo" "$dir"
        else git clone "$repo" "$dir"; fi
    fi
    git -C "$dir" fetch --all --tags --quiet
    log "pinning $(basename "$dir") to $commit"
    git -C "$dir" checkout --quiet --detach "$commit"
    git -C "$dir" rev-parse HEAD
}

pin "$AC"                        "$AC_REPO"         "$AC_COMMIT" "$AC_BRANCH"
pin "$AC/modules/mod-playerbots" "$PLAYERBOTS_REPO" "$PLAYERBOTS_COMMIT"
pin "$AC/modules/mod-ah-bot"     "$AHBOT_REPO"      "$AHBOT_COMMIT"

# Upstream hardcodes `-j $(nproc)+1`. On a 7 GB host that is nine concurrent clang
# jobs and a guaranteed OOM, so make the job count a build arg.
DF=$AC/apps/docker/Dockerfile
if ! grep -q 'ARG BUILD_JOBS' "$DF"; then
    log "patching Dockerfile to honour BUILD_JOBS"
    # tmp-and-move instead of sed -i, which differs between GNU and BSD sed and
    # this script now also runs on macOS for the staging stack.
    sed -e 's|^ARG CMAKE_EXTRA_OPTIONS=""|ARG CMAKE_EXTRA_OPTIONS=""\
ARG BUILD_JOBS=""|' \
        -e 's|cmake --build \. --config "\$CTYPE" -j \$((\$(nproc) + 1))|cmake --build . --config "$CTYPE" -j ${BUILD_JOBS:-$(($(nproc) + 1))}|' \
        "$DF" > "$DF.tmp" && mv "$DF.tmp" "$DF"
    grep -q 'ARG BUILD_JOBS' "$DF" && grep -q 'BUILD_JOBS:-' "$DF" \
        || die "Dockerfile patch did not apply; upstream layout changed"
fi

# Phase 2 hook. Re-applied on every bootstrap because pinning does a detached
# checkout, which would otherwise revert the patched files.
if [[ -x $ROOT/patches/llm/apply.sh ]]; then
    log "applying LLM bridge patch"
    "$ROOT/patches/llm/apply.sh" "$AC/modules/mod-playerbots"
fi

cp "$ROOT/overlay/docker-compose.override.yml" "$AC/docker-compose.override.yml"
cp "$ENV_FILE" "$AC/.env"; chmod 600 "$AC/.env"

log "bootstrap complete"
