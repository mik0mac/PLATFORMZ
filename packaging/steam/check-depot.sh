#!/bin/bash
# packaging/steam/check-depot.sh - verify Steam depot content BEFORE it is uploaded
# (F4, #95). The Steam counterpart of dist-pack's Gatekeeper assertion: check what
# actually landed in the folder instead of trusting that each step succeeded.
#
#   packaging/steam/check-depot.sh [dist/steam]
#
# Checks each platform folder that exists under the content root:
#   mac/       PLATFORMZ.app, from `make steam-depot`
#   windows/   platformz.exe and friends, from `pack.ps1 -Steam` on Windows
# At least one must exist. Exits non-zero on the first problem; upload.sh runs
# this first and uploads nothing if it fails.

set -euo pipefail

ROOT="${1:-dist/steam}"
fail() { echo "ERROR: $*" >&2; exit 1; }
ok()   { echo "==> $*"; }

[ -d "$ROOT" ] || fail "no content root at $ROOT - build first: make steam-depot"

#MARK: Everywhere
# steam_appid.txt tells the Steam library which game it is when Steam did NOT
# launch it. Shipped to players, it would let a copy start outside Steam as that
# app, and it is meaningless in a build Steam installs. Development only.
leak=$(find "$ROOT/mac" "$ROOT/windows" -iname 'steam_appid.txt' 2>/dev/null || true)
[ -z "$leak" ] || fail "steam_appid.txt must never ship - found: $leak"
litter=$(find "$ROOT/mac" "$ROOT/windows" -name '.DS_Store' 2>/dev/null || true)
[ -z "$litter" ] || fail ".DS_Store in the depot: $litter"

checked=0

#MARK: macOS
MAC="$ROOT/mac"
if [ -d "$MAC" ]; then
    APP="$MAC/PLATFORMZ.app"
    EXE="$APP/Contents/MacOS/platformz"
    [ -d "$APP" ] || fail "$MAC exists but holds no PLATFORMZ.app"
    extra=$(find "$MAC" -mindepth 1 -maxdepth 1 ! -name 'PLATFORMZ.app')
    [ -z "$extra" ] || fail "the Mac depot should hold only PLATFORMZ.app, also found: $extra"
    [ -x "$EXE" ] || fail "no executable at $EXE"
    [ -f "$APP/Contents/MacOS/libsteam_api.dylib" ] || fail "libsteam_api.dylib missing beside the executable"
    [ -d "$APP/Contents/Resources/assets" ] || fail "no assets in Contents/Resources"

    # Linked against Steam, and against nothing a player's Mac lacks.
    libs=$(otool -L "$EXE" | tail -n +2)
    grep -q '@loader_path/libsteam_api.dylib' <<<"$libs" ||
        fail "the executable does not link libsteam_api.dylib - this is not the Steam edition"
    if grep -qE '/opt/homebrew|/usr/local/' <<<"$libs"; then
        fail "the executable links a Homebrew library a player will not have: $(grep -E '/opt/homebrew|/usr/local/' <<<"$libs")"
    fi
    # The handout's floor is macOS 13 (raylib built for it); a newer one means it
    # was linked against Homebrew's raylib.
    minos=$(otool -l "$EXE" | awk '/LC_BUILD_VERSION/{f=1} f&&/minos/{print $2; exit}')
    [ "$minos" = "13.0" ] || fail "built for macOS $minos, not 13.0 - was it linked against the macOS 13 raylib?"

    # Signed, with the entitlements the overlay needs, and notarized.
    codesign --verify --deep --strict "$APP" 2>/dev/null || fail "codesign does not verify $APP"
    ents=$(codesign -d --entitlements - --xml "$APP" 2>/dev/null || true)
    for e in com.apple.security.cs.disable-library-validation \
             com.apple.security.cs.allow-dyld-environment-variables; do
        grep -q "$e" <<<"$ents" || fail "signature lacks the $e entitlement (packaging/steam/entitlements.plist)"
    done
    gk=$(spctl -a -vv -t exec "$APP" 2>&1 || true)
    grep -q 'source=Notarized Developer ID' <<<"$gk" ||
        fail "Gatekeeper does not report 'Notarized Developer ID' - run make steam-depot, not make STEAM=1 app"
    ok "mac: PLATFORMZ.app - Steam edition, macOS $minos, notarized, overlay entitlements"
    checked=$((checked + 1))
fi

#MARK: Windows
# Built and checked properly on Windows by pack.ps1 -Steam (imports, version
# resource); this only confirms the folder that arrived here is complete.
WIN="$ROOT/windows"
if [ -d "$WIN" ]; then
    for f in platformz.exe steam_api64.dll; do
        [ -f "$WIN/$f" ] || fail "$f missing from the Windows depot"
    done
    [ -d "$WIN/assets" ] || fail "no assets folder in the Windows depot"
    ok "windows: platformz.exe + steam_api64.dll + assets"
    checked=$((checked + 1))
fi

[ "$checked" -gt 0 ] || fail "nothing to upload: neither $MAC nor $WIN exists"
ok "depot content OK ($checked platform(s))"
