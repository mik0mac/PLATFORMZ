#!/usr/bin/env bash
# Client-side tests. The server has its own suite under server/test/.
#
#   test/run.sh
#
# Two halves, because profile.h has two storage backends and a bug in either one
# is invisible from the other:
#   profile_test.cpp      the native file path  (round trip, corrupt files,
#                         hand-edited values, atomic replace)
#   web_profile_test.py   the localStorage path (runs emcc's own emitted JS
#                         under node; skips if there is no web build)
set -uo pipefail
cd "$(dirname "$0")"

status=0

echo "=== profile_test (native) ==="
# -I../server picks up the headless raylib stub, so this builds with no raylib
# install. HOME is redirected so the test can never touch the real profile.
BIN="$(mktemp -d)/profile_test"
if g++ -std=c++17 -O1 -I../server -I/opt/homebrew/include profile_test.cpp -o "$BIN"; then
    SANDBOX="$(mktemp -d)"
    PROFILE_TEST_HOME="$SANDBOX" "$BIN" || status=1
    rm -rf "$SANDBOX"
else
    echo "BUILD FAILED"; status=1
fi

echo
echo "=== web_profile_test (localStorage) ==="
python3 web_profile_test.py || status=1

echo
[ $status -eq 0 ] && echo "ALL CLIENT TESTS PASSED" || echo "CLIENT TESTS FAILED"
exit $status
