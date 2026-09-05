#!/usr/bin/env bash
# Build and run every server-side test. Each needs different include flags, which
# is exactly why this exists.
#
#   ./server/test/run_all.sh          from the repo root
#
# These are standalone binaries, not a framework: each returns non-zero on
# failure and prints what it checked. The live-server checks (probe.py,
# probe_idle.py, probe_load.py) need a running gameserver and are not run here.
set -uo pipefail
cd "$(dirname "$0")/../.."          # repo root

OUT="${TMPDIR:-/tmp}/platformz-tests"
mkdir -p "$OUT"
BREW="$(brew --prefix 2>/dev/null || echo /usr)"
CXXFLAGS="-std=c++17 -O2 -I server -I . -I${BREW}/include -DPLATFORMZ_SERVER"
fail=0

run() {   # run <name> <source> [extra sources...]
  local name="$1"; shift
  printf '\n=== %s ===\n' "$name"
  if ! g++ $CXXFLAGS "$@" -o "$OUT/$name"; then
    echo "BUILD FAILED"; fail=1; return
  fi
  "$OUT/$name" || fail=1
}

run reassembly  server/test/reassembly_test.cpp
run grid_equiv  server/test/grid_equiv_test.cpp collisions.cpp
run registry    server/test/registry_test.cpp

printf '\n=== benches (informational, not pass/fail) ===\n'
g++ $CXXFLAGS server/test/bench_grid_split.cpp collisions.cpp -o "$OUT/bench" \
  && "$OUT/bench"

if [ "$fail" -ne 0 ]; then echo; echo "SOME TESTS FAILED"; exit 1; fi
echo; echo "all server tests passed"
