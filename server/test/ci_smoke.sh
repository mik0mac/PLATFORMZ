#!/usr/bin/env bash
# Does the built server actually serve clients, and speak the protocol they were
# built against?
#
#   ./server/test/ci_smoke.sh
#
# One server boot, three questions:
#   1. do the wire tags it was COMPILED with match netbin.h - the header the
#      client will be compiled with?
#   2. does a WebSocket client get a welcome and a stream of state? (the browser
#      build's transport, and until E3 the only one with no automated coverage)
#   3. does the load harness still drive a real match end to end?
#
# WHY THIS EXISTS. The tags are the one part of the protocol with no graceful
# degradation: a client that disagrees latches SERVER VERSION MISMATCH and never
# recovers. And this project has shipped a mismatched server before, because
# `server/Makefile` listed only the .cpp files, so a header-only pull rebuilt
# nothing and `systemctl restart` swapped in the identical old binary. The
# Makefile now wildcards ../*.h, and this is the check that says so out loud
# rather than trusting it.
#
# It also exercises /status itself, which is the endpoint a deploy is verified
# with - an endpoint nobody tests is an endpoint that quietly stops answering.
set -uo pipefail
cd "$(dirname "$0")/../.."

SERVER_BIN="${PLATFORMZ_SERVER_BIN:-gameserver}"
[ -x "server/$SERVER_BIN" ] || { echo "build the server first: make -C server"; exit 1; }
[ -x server/loadtest ] || { echo "build the harness first: make -C server loadtest"; exit 1; }

TMP="${TMPDIR:-/tmp}/platformz-protocol"
mkdir -p "$TMP"
LOG="$TMP/server.log"
SERVER_PID=""
cleanup() { [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null; return 0; }
trap cleanup EXIT INT TERM PIPE

# PLATFORMZ_MAX_ROOMS_PER_ADDR=0: the harness makes every room from one address,
# which is exactly what E2's budget exists to refuse. The budget itself is tested
# by probe_capacity, not here.
( cd server && exec env PLATFORMZ_SCORES="$TMP/scores" PLATFORMZ_MAX_ROOMS_PER_ADDR=0 \
    "./$SERVER_BIN" >"$LOG" 2>&1 ) &
SERVER_PID=$!
for _ in $(seq 1 40); do grep -q "lobby ready" "$LOG" 2>/dev/null && break; sleep 0.3; done
if ! grep -q "lobby ready" "$LOG" 2>/dev/null; then
  echo "server never came up:"; cat "$LOG"; exit 1
fi

# What the header says the CLIENT will speak. Read out of netbin.h rather than
# duplicated here, so this file can never drift into asserting stale constants -
# the only way to change the expectation is to change the protocol.
want_state=$(sed -n 's/.*STATE_BIN_VERSION *= *0x\([0-9A-Fa-f]*\).*/\1/p'   netbin.h | head -1)
want_welcome=$(sed -n 's/.*WELCOME_BIN_VERSION *= *0x\([0-9A-Fa-f]*\).*/\1/p' netbin.h | head -1)
if [ -z "$want_state" ] || [ -z "$want_welcome" ]; then
  echo "could not read the tags out of netbin.h - has the declaration changed shape?"; exit 1
fi
# /status reports them as decimal ints (see MaybeServeStatus).
want_state=$((16#$want_state))
want_welcome=$((16#$want_welcome))

status=$(curl -fsS --max-time 5 "http://127.0.0.1:9000/status" 2>&1) || {
  echo "GET /status failed: $status"; tail -20 "$LOG"; exit 1
}
echo "status: $status"

field() { printf '%s' "$status" | sed -n "s/.*\"$1\":\([0-9-]*\).*/\1/p"; }
got_state=$(field stateTag)
got_welcome=$(field welcomeTag)

fail=0
if [ "$got_state" != "$want_state" ]; then
  echo "FAIL state tag: server says $got_state, netbin.h says $want_state"; fail=1
else
  echo "  ok   state tag $got_state matches netbin.h"
fi
if [ "$got_welcome" != "$want_welcome" ]; then
  echo "FAIL welcome tag: server says $got_welcome, netbin.h says $want_welcome"; fail=1
else
  echo "  ok   welcome tag $got_welcome matches netbin.h"
fi

# The caps the browser and the deploy docs quote. Not protocol, but /status is
# where an operator reads them, and a zero here means the endpoint is reporting
# defaults it does not actually hold.
for f in maxMatches maxActive maxPlayers; do
  v=$(field "$f")
  if [ -z "$v" ] || [ "$v" -le 0 ] 2>/dev/null; then
    echo "FAIL /status is missing a usable $f (got '${v:-}')"; fail=1
  else
    echo "  ok   $f = $v"
  fi
done

# E1's cookie is not visible on /status, so check the thing that would silently
# disable it: a boot with no configured secret. CI has none, so the warning is
# EXPECTED here - what matters is that the feature announced itself at all.
if grep -q "UDP handshake cookie: ON" "$LOG"; then
  echo "  ok   UDP handshake cookie is on"
else
  echo "FAIL the server did not report the UDP handshake cookie at boot"; fail=1
fi

# --- The transports actually carrying clients -----------------------------
echo
if ./server/loadtest --mode ws-smoke; then
  echo "  ok   WebSocket transport"
else
  echo "FAIL WebSocket smoke test"; fail=1
fi

echo
# Small on purpose: this is a "does the harness still work" gate, not a
# measurement. Real numbers come from running it against the box with the server
# on its own machine - see the header of server/loadtest.cpp.
if ./server/loadtest --matches 2 --clients 2 --seconds 10; then
  echo "  ok   UDP load harness drove two matches"
else
  echo "FAIL load harness"; fail=1
fi

[ "$fail" -eq 0 ] || { echo; echo "SMOKE TEST FAILED"; exit 1; }
echo; echo "smoke test passed"
