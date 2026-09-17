#!/usr/bin/env bash
# Is the committed web bundle newer than the protocol it has to speak?
#
#   ./scripts/check-web-bundle-fresh.sh
#
# WHY THIS IS NOT THE SAME AS "DOES THE WEB CLIENT BUILD".
#
# The CI job next to this one compiles the web client from source, which proves
# the source is buildable. It does NOT prove the `web/platformz.*` artifacts
# COMMITTED TO THIS REPO correspond to that source - and those artifacts are what
# actually ships. The box serves `web/` straight off the checkout.
#
# So the failure this catches is: somebody bumps a protocol tag in netbin.h,
# rebuilds and deploys the server, and forgets `make web`. The server now speaks
# 0x0B while the committed wasm still speaks 0x0A, every browser client latches
# SERVER VERSION MISMATCH, and nothing in CI noticed because the source compiled
# perfectly well.
#
# WHY IT COMPARES COMMIT DATES RATHER THAN CONTENT. Rebuilding and diffing the
# wasm would be the direct check, and it does not work: emcc output depends on the
# toolchain version and on absolute paths, so a CI rebuild never matches a
# developer's byte for byte. This asks a weaker question that is still the right
# one - "was the bundle built after the last protocol change?" - using only git.
#
# SCOPED TO THE PROTOCOL HEADERS on purpose. A stale bundle missing a UI tweak is
# cosmetic and nobody should be blocked over it. A stale bundle with the wrong
# wire tags is a hard break with no graceful degradation, because the client
# latches the mismatch and never retries.
set -uo pipefail
cd "$(dirname "$0")/.."

BUNDLE="web/platformz.wasm"
# The files that decide whether a client and server can talk at all.
PROTOCOL_FILES=(netbin.h wire.h)

[ -f "$BUNDLE" ] || { echo "no $BUNDLE committed - nothing to check"; exit 0; }

# A shallow clone has no history to compare, which is most CI checkouts by
# default. Say so rather than passing vacuously.
if [ "$(git rev-parse --is-shallow-repository 2>/dev/null)" = "true" ]; then
  echo "shallow checkout - cannot compare commit dates."
  echo "Use actions/checkout with fetch-depth: 0 to make this check meaningful."
  exit 0
fi

stamp() { git log -1 --format=%ct -- "$1" 2>/dev/null; }

bundle_at=$(stamp "$BUNDLE")
[ -n "$bundle_at" ] || { echo "$BUNDLE is untracked or has no history - skipping"; exit 0; }

fail=0
for f in "${PROTOCOL_FILES[@]}"; do
  at=$(stamp "$f")
  [ -n "$at" ] || continue
  if [ "$at" -gt "$bundle_at" ]; then
    echo "STALE: $f was last changed after $BUNDLE was last rebuilt"
    echo "    $f      $(git log -1 --format='%ad  %h  %s' --date=short -- "$f")"
    echo "    $BUNDLE $(git log -1 --format='%ad  %h  %s' --date=short -- "$BUNDLE")"
    fail=1
  fi
done

if [ "$fail" -ne 0 ]; then
  cat <<'MSG'

The committed browser bundle predates a change to the wire protocol, so it may
speak different tags from the server built out of this same commit. Browsers
would get SERVER VERSION MISMATCH and never recover from it.

Rebuild and commit it:

    export EMSDK_PYTHON=/opt/homebrew/opt/python@3.14/bin/python3.14
    make web RAYLIB_WEB_DIR=$HOME/raylib
    git add web/ && git commit

MSG
  exit 1
fi

echo "web bundle is newer than ${PROTOCOL_FILES[*]} - ok"
