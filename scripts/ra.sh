#!/usr/bin/env bash
# Run worldserver console commands non-interactively over Remote Access.
# The built-in console requires a real TTY, which rules it out for scripts and cron.
#
# usage:  ra.sh "server info" ["account onlinelist" ...]
#         RA_USER=admin RA_PASS=... ra.sh "..."
source "$(dirname "$(readlink -f "$0")")/lib.sh"
require_stack
set -a; source "$ROOT/overlay/.env"; set +a

: "${RA_USER:?set RA_USER (a gmlevel>=3 account)}"
: "${RA_PASS:?set RA_PASS}"
[[ $# -gt 0 ]] || die 'usage: ra.sh "<command>" ["<command>" ...]'

# Run from a container so the host needs no Python. --network host reaches the
# loopback-published RA port. RA speaks CRLF; bare LF silently hangs the session.
printf '%s\n' "$@" | docker run --rm -i --network host \
    -e RA_USER -e RA_PASS \
    -e RA_PORT="${DOCKER_RA_EXTERNAL_PORT:-3443}" \
    python:3-alpine python -c '
import os, socket, sys, time

cmds = [l.strip() for l in sys.stdin if l.strip()]
s = socket.create_connection(("127.0.0.1", int(os.environ["RA_PORT"])), timeout=15)
s.settimeout(15)

def until(token, limit=20.0):
    buf, deadline = b"", time.monotonic() + limit
    while time.monotonic() < deadline:
        try:
            chunk = s.recv(8192)
        except socket.timeout:
            break
        if not chunk:
            break
        buf += chunk
        if buf.rstrip().endswith(token):
            break
    return buf.decode(errors="replace")

def send(line):
    s.sendall(line.encode() + b"\r\n")

until(b"Username:"); send(os.environ["RA_USER"])
until(b"Password:"); send(os.environ["RA_PASS"])
banner = until(b"AC>")
if "Welcome" not in banner:
    sys.exit("RA authentication failed: " + banner.strip())

for c in cmds:
    send(c)
    out = until(b"AC>")
    print(out.removesuffix("AC>").strip())
send("quit")
'
