#!/bin/bash
# packaging/steam/upload.sh - upload the Steam depots (F4, #95). Run it through
# make, which passes the IDs:
#
#   make steam-upload STEAM_USER=<steam login>                    upload only
#   make steam-upload STEAM_USER=<login> STEAM_BRANCH=beta        upload + set live on beta
#   make steam-upload STEAM_USER=<login> STEAM_PREVIEW=1          dry run: scan, upload nothing
#
# Steps: check-depot.sh on dist/steam (nothing uploads if it fails), write
# Steam's build script for the platform folders present, run SteamCMD with it.
#
# Never sets the DEFAULT branch live - that is the button players get, and it is
# pressed by hand on partner.steamgames.com (Steam refuses it from here anyway).
# Login is interactive: SteamCMD asks for the password and Steam Guard code
# itself, so neither ever appears in a command line or a file.

set -euo pipefail
cd "$(dirname "$0")/../.."
REPO=$(pwd)

fail() { echo "ERROR: $*" >&2; exit 1; }

: "${STEAM_APP_ID:?STEAM_APP_ID is not set - fill it in the Makefile once the store entry exists}"
: "${STEAM_USER:?pass STEAM_USER=<the Steam account that uploads builds>}"
STEAM_DEPOT_MAC="${STEAM_DEPOT_MAC:-}"
STEAM_DEPOT_WIN="${STEAM_DEPOT_WIN:-}"
STEAM_BRANCH="${STEAM_BRANCH:-}"
STEAM_PREVIEW="${STEAM_PREVIEW:-0}"
STEAM_SDK="${STEAM_SDK:-../steamworks_sdk}"
[ -d "$STEAM_SDK/sdk" ] && [ ! -d "$STEAM_SDK/tools" ] && STEAM_SDK="$STEAM_SDK/sdk"

[ "$STEAM_BRANCH" != "default" ] || fail "the default branch is set live by hand, on partner.steamgames.com"

CONTENT="$REPO/dist/steam"
packaging/steam/check-depot.sh dist/steam

#MARK: Which depots
# A folder that exists must have a depot to go to, or half a build would upload
# silently. A depot ID with no folder is just a platform not built this time.
depots=""
add_depot() {   # <depot id> <folder under dist/steam>
    depots+=$'\t\t'"\"$1\""$'\n\t\t{\n\t\t\t"FileMapping"\n\t\t\t{\n'
    depots+=$'\t\t\t\t"LocalPath" "'"$2"$'/*"\n\t\t\t\t"DepotPath" "."\n\t\t\t\t"Recursive" "1"\n\t\t\t}\n\t\t}\n'
    echo "==> depot $1 <- dist/steam/$2"
}
if [ -d "$CONTENT/mac" ]; then
    [ -n "$STEAM_DEPOT_MAC" ] || fail "dist/steam/mac exists but STEAM_DEPOT_MAC is not set"
    add_depot "$STEAM_DEPOT_MAC" mac
fi
if [ -d "$CONTENT/windows" ]; then
    [ -n "$STEAM_DEPOT_WIN" ] || fail "dist/steam/windows exists but STEAM_DEPOT_WIN is not set"
    add_depot "$STEAM_DEPOT_WIN" windows
fi

#MARK: The build script
# Written to a temporary folder because SteamCMD takes its path on the command
# line and this checkout's path has spaces in it; inside the file, quoted, they
# are fine.
work=$(mktemp -d -t platformz-steam)
trap 'rm -rf "$work"' EXIT
desc="PLATFORMZ $(git describe --always --dirty 2>/dev/null || echo unknown)"
mkdir -p "$CONTENT/output"
cat > "$work/app_build.vdf" <<EOF
"AppBuild"
{
	"AppID" "$STEAM_APP_ID"
	"Desc" "$desc"
	"Preview" "$STEAM_PREVIEW"
	"SetLive" "$STEAM_BRANCH"
	"ContentRoot" "$CONTENT/"
	"BuildOutput" "$CONTENT/output/"
	"Depots"
	{
$depots	}
}
EOF
echo "==> $desc -> app $STEAM_APP_ID${STEAM_BRANCH:+, live on '$STEAM_BRANCH'}$([ "$STEAM_PREVIEW" = 1 ] && echo ' (PREVIEW - nothing uploads)')"

#MARK: SteamCMD
# The SDK's copy is run from a cache folder, not in place: its launcher script
# breaks on a path with spaces, and SteamCMD updates itself into its own folder,
# which should not be the SDK in Dropbox. Refreshed from the SDK when it changes.
# Unzipping the SDK drops the executable bits, and the browser download leaves it
# quarantined; both are put right on the copy (it is Valve's own tool, from the
# SDK downloaded from partner.steamgames.com). It is an Intel binary: Rosetta.
CMD_DIR="$HOME/Library/Caches/PLATFORMZ/steamcmd"
SRC="$STEAM_SDK/tools/ContentBuilder/builder_osx"
[ -f "$SRC/steamcmd" ] || fail "no SteamCMD at $SRC - is the Steamworks SDK beside the checkout?"
if [ ! -x "$CMD_DIR/steamcmd" ] || [ "$SRC/steamcmd" -nt "$CMD_DIR/.copied" ]; then
    mkdir -p "$CMD_DIR"
    cp -R "$SRC/" "$CMD_DIR/"
    chmod +x "$CMD_DIR/steamcmd" "$CMD_DIR/steamcmd.sh"
    xattr -dr com.apple.quarantine "$CMD_DIR" 2>/dev/null || true
    touch "$CMD_DIR/.copied"
fi
"$CMD_DIR/steamcmd.sh" +login "$STEAM_USER" +run_app_build "$work/app_build.vdf" +quit
