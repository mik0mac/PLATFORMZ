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
# Three environment overrides:
#   PLATFORMZ_SERVER_BIN   which binary in server/ to run (e.g. gameserver-tsan)
#   PLATFORMZ_PROBE_SET    a space-separated subset, instead of all of them
#   PLATFORMZ_NO_BUILD     skip the rebuild below and run the binary as it is
#   PLATFORMZ_PORT         listen somewhere other than 9000. The escape hatch for
#                          a stray server that cannot be killed: a process wedged
#                          mid-exit holds the port and, without this, makes the
#                          whole suite unrunnable until the machine is rebooted.
#                          That is not hypothetical - a TSan build did exactly
#                          that, which is how this option came to exist.
# Defaults reproduce exactly what a bare run has always done.
set -uo pipefail
cd "$(dirname "$0")/../.."

SERVER_BIN="${PLATFORMZ_SERVER_BIN:-gameserver}"
BIN="server/$SERVER_BIN"

# BUILD FIRST, always. This used to only check the binary existed, which meant a
# source change you had not compiled was silently tested as the PREVIOUS build -
# and a suite that passes against the wrong binary is worse than one that fails,
# because you believe it. That has now happened twice: once with a stale
# `loadtest` reporting 0% delivered after a wire-version bump, and once with a
# whole probe run passing against a server compiled from different options.h.
#
# Each binary name is its own make target (gameserver, gameserver-tsan,
# loadtest), so this works for whichever one PLATFORMZ_SERVER_BIN names. make is
# incremental, so it is a no-op line when nothing changed - including in CI,
# which builds before it gets here.
if [ -z "${PLATFORMZ_NO_BUILD:-}" ]; then
  make -C server "$SERVER_BIN" || { echo "build failed: $SERVER_BIN"; exit 1; }
fi
[ -x "$BIN" ] || { echo "build the server first: make -C server"; exit 1; }
TMP="${TMPDIR:-/tmp}/platformz-probes"
mkdir -p "$TMP"
fail=0
SERVER_PID=""

# ThreadSanitizer: point the runtime at our suppressions ourselves, from a path
# with no spaces in it.
#
# TSAN_OPTIONS is a space-separated key=value list with NO quoting of any kind,
# so a checkout under e.g. "~/Dropbox (Personal)/VS_CODE/PLATFORMZ" cannot be
# named in it at all - the runtime aborts with "expected '=' in TSAN_OPTIONS"
# before main() ever runs, and all you see is a server that never came up.
# Copying the file somewhere plain sidesteps that, and means neither the caller
# nor the CI workflow has to know where the repo lives. An explicit
# suppressions= from the caller always wins.
case "$SERVER_BIN" in
  *tsan*)
    SUPP_DIR="$TMP"
    case "$SUPP_DIR" in *" "*) SUPP_DIR="/tmp/platformz-tsan"; mkdir -p "$SUPP_DIR";; esac
    if [ -f server/test/tsan.supp ]; then
      cp server/test/tsan.supp "$SUPP_DIR/tsan.supp"
      case "${TSAN_OPTIONS:-}" in
        *suppressions=*) ;;
        *) export TSAN_OPTIONS="${TSAN_OPTIONS:-} suppressions=$SUPP_DIR/tsan.supp" ;;
      esac
      echo "TSan suppressions: $SUPP_DIR/tsan.supp"
    fi
    ;;
esac

# Every probe, in an order chosen so the cheap ones fail first.
ALL_PROBES="probe probe_cookie probe_identity probe_scoreboard probe_botnames probe_capacity probe_leaderboard probe_directory \
probe_official probe_host probe_mapsize probe_multimatch probe_joinprogress probe_reconnect \
probe_maxbots probe_minhumans probe_roomcap probe_unseated probe_listorder probe_leaverace"
PROBES="${PLATFORMZ_PROBE_SET:-$ALL_PROBES}"

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
PORT="${PLATFORMZ_PORT:-9000}"
export PLATFORMZ_PORT="$PORT"   # both the server and probe.py read this
port_free() { ! lsof -nP -iTCP:"$PORT" -sTCP:LISTEN >/dev/null 2>&1; }

# shellcheck disable=SC2086  # $PROBES is a word list on purpose
for probe in $PROBES; do
  log="$TMP/$probe.log"
  rm -f "$TMP/$probe.scores"

  for _ in $(seq 1 40); do port_free && break; sleep 0.25; done
  if ! port_free; then echo "port $PORT still held; cannot run $probe (PLATFORMZ_PORT picks another)"; exit 1; fi

  # `exec` matters: without it the subshell forks gameserver as a CHILD, so $!
  # is the subshell's pid and killing it leaves the server running - which is how
  # strays end up holding port 9000 and the next run refuses to start. With exec
  # the subshell BECOMES gameserver, so $! is the thing we actually want to kill.
  # Per-probe server environment. probe_directory fills the registry to prove
  # paging works, and probe_listorder fills past one page to prove the paging
  # SNAPSHOT holds; every probe client shares one source address (127.0.0.1), so
  # E2's per-address creation budget would stop either at three rooms - the probe
  # would then fail for a reason that has nothing to do with paging. 0 turns that
  # budget off, which is the same knob an operator turns for a LAN party behind
  # one NAT. probe_capacity tests the budget itself, so it gets the default.
  # A plain string, deliberately unquoted below so it word-splits to nothing when
  # empty. An empty ARRAY would be cleaner but trips `set -u` on macOS's bash 3.2.
  env_extra=""
  case "$probe" in
    probe_directory|probe_listorder) env_extra="PLATFORMZ_MAX_ROOMS_PER_ADDR=0" ;;
  esac

  # shellcheck disable=SC2086  # $env_extra must split
  ( cd server && exec env PLATFORMZ_SCORES="$TMP/$probe.scores" $env_extra "./$SERVER_BIN" >"$log" 2>&1 ) &
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

  # A sanitizer says nothing about the probe's own pass/fail - it writes to the
  # server's stderr and the server carries on. Unnoticed, a TSan job would go
  # green on a run that reported a data race, which is the one outcome that would
  # make having the job actively misleading.
  if grep -q "WARNING: ThreadSanitizer" "$log" 2>/dev/null; then
    echo "ThreadSanitizer reported a race during $probe (see $log)"
    grep -c "WARNING: ThreadSanitizer" "$log" | sed 's/^/  reports: /'
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then echo; echo "SOME PROBES FAILED"; exit 1; fi
echo; echo "all probes passed"
