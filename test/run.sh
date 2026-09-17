#!/usr/bin/env bash
# Client-side tests. The server has its own suite under server/test/.
#
#   test/run.sh
#
# Three parts. The first two are profile.h's two storage backends, because a bug
# in either one is invisible from the other; the third is the local score board,
# which rides on the same storage layer but has rules of its own:
#   profile_test.cpp      the native file path  (round trip, corrupt files,
#                         hand-edited values, atomic replace)
#   web_profile_test.py   the localStorage path (runs emcc's own emitted JS
#                         under node; skips if there is no web build)
#   local_scores_test.cpp the LOCAL high-score board (the per-identity cap that
#                         bounds the file, ranking, and hand-edited files)
set -uo pipefail
cd "$(dirname "$0")"

status=0

# -I../server picks up the headless raylib stub, so these build with no raylib
# install. HOME is redirected so they can never touch the real profile.
run_native() {   # run_native <source> <label>
    local bin sandbox
    bin="$(mktemp -d)/$2"
    if g++ -std=c++17 -O1 -I../server -I/opt/homebrew/include "$1" -o "$bin"; then
        sandbox="$(mktemp -d)"
        PROFILE_TEST_HOME="$sandbox" "$bin" || status=1
        rm -rf "$sandbox"
    else
        echo "BUILD FAILED"; status=1
    fi
}

echo "=== profile_test (native) ==="
run_native profile_test.cpp profile_test

echo
echo "=== local_scores_test (native) ==="
run_native local_scores_test.cpp local_scores_test

echo
echo "=== web_profile_test (localStorage) ==="
python3 web_profile_test.py || status=1

echo
[ $status -eq 0 ] && echo "ALL CLIENT TESTS PASSED" || echo "CLIENT TESTS FAILED"
exit $status
