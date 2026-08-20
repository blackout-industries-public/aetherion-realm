#!/usr/bin/env bash
# Extract continent map art from a client you own. Runs in a container so the host
# needs no Python toolchain, and mounts the client read-only.
#
#   ./extract-maps.sh "/path/to/WoW 3.3.5a" [output-dir]
set -euo pipefail

CLIENT=${1:?usage: extract-maps.sh <client-dir> [out-dir]}
OUT=${2:-$(dirname "$(readlink -f "$0")")/../maps}
HERE=$(dirname "$(readlink -f "$0")")

[[ -d $CLIENT/Data ]] || { echo "no Data/ directory under $CLIENT" >&2; exit 1; }
mkdir -p "$OUT"

docker run --rm \
    -v "$CLIENT/Data:/client:ro" \
    -v "$OUT:/out" \
    -v "$HERE:/tools:ro" \
    python:3.12-slim bash -c \
    "pip install --quiet --disable-pip-version-check mpyq pillow && \
     python3 /tools/extract_maps.py /client /out"
