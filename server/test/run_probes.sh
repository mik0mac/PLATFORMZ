#!/usr/bin/env bash
# Run every live-server probe, each against its OWN fresh gameserver.
#
#   ./server/test/run_probes.sh
#
# THE FRESH SERVER PER PROBE IS THE POINT. These probes mutate server state that
# outlives them: probe_directory fills the match registry to MATCH_MAX_CONCURRENT,
# so anything run afterwards cannot create a room and fails for a reason that has
# nothing to do with the code under test. Sharing one server produced exactly that
# false failure once already.
#
# Complements run_all.sh, which builds the standalone C++ tests and needs no
# server at all.
set -uo pipefail
cd "$(dirname "$0")/../.."

BIN=server/gameserver
[ -x "$BIN" ] || { echo "build the server first: make -C server"; exit 1; }
TMP="${TMPDIR:-/tmp}/platformz-probes"
mkdir -p "$TMP"
fail=0
SERVER_PID=""

# Never leave a server behind. An interrupted run - Ctrl-C, or a pipeline whose
# reader exits and SIGPIPEs this script - would otherwise orphan a gameserver
# still holding port 9000, and the NEXT run refuses to start because of it.
cleanup() {
  [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null
  return 0
}
trap cleanup EXIT INT TERM PIPE

# Wait until nothing holds the game port. Starting the next server before the
# previous one lets go means it dies on bind and the probe silently talks to the
# OLD server - which is how a stale registry full of another probe's rooms once
# produced a page of nonsense failures.
port_free() { ! lsof -nP -iTCP:9000 -sTCP:LISTEN >/dev/null 2>&1; }

for probe in probe probe_leaderboard probe_directory probe_multimatch probe_joinprogress; do
  log="$TMP/$probe.log"
  rm -f "$TMP/$probe.scores"

  for _ in $(seq 1 40); do port_free && break; sleep 0.25; done
  if ! port_free; then echo "port 9000 still held; cannot run $probe"; exit 1; fi

  # `exec` matters: without it the subshell forks gameserver as a CHILD, so $!
  # is the subshell's pid and killing it leaves the server running - which is how
  # strays end up holding port 9000 and the next run refuses to start. With exec
  # the subshell BECOMES gameserver, so $! is the thing we actually want to kill.
  ( cd server && exec env PLATFORMZ_SCORES="$TMP/$probe.scores" ./gameserver >"$log" 2>&1 ) &
  SERVER_PID=$!
  for _ in $(seq 1 40); do grep -q "lobby ready" "$log" 2>/dev/null && break; sleep 0.3; done

  # Prove THIS server is the one answering, not a survivor from the last probe.
  if grep -q "Address already in use" "$log" 2>/dev/null; then
    echo "server for $probe failed to bind - a previous one is still running"; exit 1
  fi
  if ! grep -q "lobby ready" "$log" 2>/dev/null; then
    echo "server for $probe never came up; see $log"; exit 1
  fi

  printf '\n=== %s ===\n' "$probe"
  python3 "server/test/$probe.py" || fail=1

  kill "$SERVER_PID" 2>/dev/null
  for _ in $(seq 1 40); do kill -0 "$SERVER_PID" 2>/dev/null || break; sleep 0.25; done
  SERVER_PID=""
done

if [ "$fail" -ne 0 ]; then echo; echo "SOME PROBES FAILED"; exit 1; fi
echo; echo "all probes passed"
