#!/usr/bin/env bash
# Put this machine back to a FIRST LAUNCH state, so the game's defaults can be
# tested as a new player would meet them.
#
#   scripts/reset-prefs.sh                 back up and clear everything
#   scripts/reset-prefs.sh --keep-identity reset the rules, KEEP who you are
#   scripts/reset-prefs.sh --scores        only the local high-score board
#   scripts/reset-prefs.sh --server        also the local dev server's scoreboard
#   scripts/reset-prefs.sh --restore       put the most recent backup back
#   scripts/reset-prefs.sh --list          show what is there; change nothing
#   scripts/reset-prefs.sh --force         clear even with a client running (see below)
#
# Nothing is ever deleted outright - every file is copied to a timestamped
# .bak-<when> beside it first, and --restore brings the newest one back. The
# backups are a few hundred bytes each; keep them or bin them by hand.
#
#MARK: Why --keep-identity exists, and when you want it
# profile.json holds two different KINDS of thing. Most of it is preferences -
# your name, volume, the rules you last played with - and clearing those is the
# whole point of this script. But it also holds `clientId` and the server-issued
# identity `token` (D3), and THAT is how the server knows your leaderboard runs
# are yours. Throw the token away and the rows stay on the board keyed to an
# identity you no longer hold: you cannot beat your own score any more, because
# the server no longer believes it was you.
#
# So: a full reset is right for "what does a brand-new player see". Use
# --keep-identity when you only want the RULES back at their defaults and would
# rather not orphan yourself on the online board.
#
#MARK: The web build is a separate drawer
# The browser client keeps the same two blobs in localStorage, which no shell
# script can reach. This prints the incantation instead - see the end.
set -uo pipefail

#MARK: Where the game keeps things
# Mirrors profile::StorageDir() in profile.h. Kept in step by hand, so if that
# function ever moves, this breaks loudly (the dir simply will not exist) rather
# than quietly clearing nothing.
case "$(uname -s)" in
    Darwin)  PREF_DIR="$HOME/Library/Application Support/PLATFORMZ" ;;
    Linux)   PREF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/platformz" ;;
    MINGW*|MSYS*|CYGWIN*) PREF_DIR="${APPDATA:-$HOME}/PLATFORMZ" ;;
    *)       PREF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/platformz" ;;
esac

PROFILE="$PREF_DIR/profile.json"     # profile.h    StoragePath()
SCORES="$PREF_DIR/scores.json"       # local_scores.h  STORAGE_LEAF
# The local dev server's board - NOT a preference, and not touched unless asked.
# constants.h SCOREBOARD_FILEPATH is relative, so it lands beside the binary.
REPO="$(cd "$(dirname "$0")/.." && pwd)"
SERVER_SCORES="$REPO/server/scores"

STAMP="$(date +%Y%m%d-%H%M%S)"
mode="all"
also_server=0
force=0

while [ $# -gt 0 ]; do
    case "$1" in
        --keep-identity) mode="keep-identity" ;;
        --scores)        mode="scores" ;;
        --restore)       mode="restore" ;;
        --list)          mode="list" ;;
        --server)        also_server=1 ;;
        --force)         force=1 ;;
        -h|--help)       sed -n '2,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1  (try --help)" >&2; exit 2 ;;
    esac
    shift
done

say()  { printf '%s\n' "$*"; }
size() { [ -f "$1" ] && wc -c < "$1" | tr -d ' ' || echo "-"; }

#MARK: Refuse while the game is running
# The client samples its live values into the profile every frame and lets
# profile::Autosave write them at most every AUTOSAVE_INTERVAL (2 s), plus a
# final profile::Save() on exit. So clearing these files under a running client
# does NOTHING: the process rewrites them from memory seconds later, and you are
# left testing the very preferences you thought you had just cleared. This guard
# is the whole reason the script is worth having rather than an rm you type.
if [ "$mode" != "list" ] && [ "$force" -eq 0 ]; then
    if pgrep -f "[p]latformz" > /dev/null 2>&1; then
        say "REFUSING: a platformz client is running."
        say ""
        say "  It rewrites profile.json from memory every ~2s (profile::Autosave)"
        say "  and again on exit, so anything cleared now would simply come back."
        say "  Quit the game first, then re-run this."
        say ""
        pgrep -fl "[p]latformz" | sed 's/^/    /'
        say ""
        say "  --force overrides this, for a wedged process or a different install."
        say "  It does not make the write stick if that client is really alive."
        exit 1
    fi
fi

#MARK: list
if [ "$mode" = "list" ]; then
    say "pref dir: $PREF_DIR"
    say ""
    for f in "$PROFILE" "$SCORES"; do
        if [ -f "$f" ]; then
            say "  $(basename "$f")  $(size "$f") bytes"
        else
            say "  $(basename "$f")  (absent - already at first-launch defaults)"
        fi
    done
    if [ -f "$PROFILE" ] && command -v python3 > /dev/null 2>&1; then
        say ""
        say "  profile holds:"
        python3 - "$PROFILE" <<'PY' 2>/dev/null || say "    (unreadable - it will load as defaults)"
import json, sys
j = json.load(open(sys.argv[1]))
tok = j.get("token", "")
print(f"    name       {j.get('name','')!r}")
print(f"    clientId   {j.get('clientId','')}")
print(f"    token      {'set (' + tok[:8] + '...)' if tok else 'none yet'}")
print(f"    volumeDb   {j.get('volumeDb', 0)}")
o = j.get("options", {})
print(f"    local rules map={o.get('map')} players={o.get('players')} "
      f"botDiff={o.get('botDiff')} walls={o.get('walls')} coast={o.get('coast')}")
PY
    fi
    backups=$(ls -1t "$PREF_DIR"/*.bak-* 2>/dev/null | head -3)
    if [ -n "$backups" ]; then
        say ""
        say "  recent backups:"
        printf '%s\n' "$backups" | sed 's|.*/|    |'
    fi
    exit 0
fi

#MARK: restore
if [ "$mode" = "restore" ]; then
    restored=0
    for leaf in profile.json scores.json; do
        newest=$(ls -1t "$PREF_DIR/$leaf".bak-* 2>/dev/null | head -1)
        if [ -n "$newest" ]; then
            cp "$newest" "$PREF_DIR/$leaf"
            say "restored $leaf  <-  $(basename "$newest")"
            restored=$((restored + 1))
        fi
    done
    [ "$restored" -eq 0 ] && { say "no backups found in $PREF_DIR"; exit 1; }
    exit 0
fi

mkdir -p "$PREF_DIR"

# Copy aside, then remove. Never a bare rm - see the header.
back_up() {
    [ -f "$1" ] || return 0
    cp "$1" "$1.bak-$STAMP"
    say "  backed up $(basename "$1") -> $(basename "$1").bak-$STAMP"
}

case "$mode" in
    scores)
        back_up "$SCORES"
        rm -f "$SCORES"
        say "  cleared the LOCAL high-score board"
        ;;

    keep-identity)
        if [ ! -f "$PROFILE" ]; then
            say "  no profile.json - already at defaults, nothing to keep"
        elif ! command -v python3 > /dev/null 2>&1; then
            say "ERROR: --keep-identity needs python3 to rewrite the JSON." >&2
            exit 1
        else
            back_up "$PROFILE"
            # Rewrite rather than delete: keep the three identity fields, drop
            # every preference so the loader falls back to MatchOptions{} - the
            # same values a first launch would get. profile.h's Deserialize
            # leaves any absent key at its default, which is exactly what makes
            # this safe: we do not have to KNOW the defaults, only omit the keys.
            python3 - "$PROFILE" <<'PY'
import json, sys, os
p = sys.argv[1]
try:
    j = json.load(open(p))
except Exception:
    j = {}
keep = {k: j[k] for k in ("name", "clientId", "token") if k in j}
keep["version"] = 1
tmp = p + ".tmp"
with open(tmp, "w") as f:
    json.dump(keep, f)
os.replace(tmp, p)          # atomic, same as profile.h's write-then-rename
os.chmod(p, 0o600)
PY
            say "  reset the rules, volume and remembered rooms"
            say "  KEPT name, clientId and identity token - your online board rows stay yours"
        fi
        back_up "$SCORES"
        rm -f "$SCORES"
        say "  cleared the LOCAL high-score board"
        ;;

    all)
        back_up "$PROFILE"; rm -f "$PROFILE"
        back_up "$SCORES";  rm -f "$SCORES"
        say "  cleared profile and local high scores - next launch is a FIRST launch"
        say "  (a fresh clientId is minted, and the server will issue a new identity"
        say "   token, so previous online leaderboard rows are no longer yours)"
        ;;
esac

if [ "$also_server" -eq 1 ]; then
    if [ -f "$SERVER_SCORES" ]; then
        cp "$SERVER_SCORES" "$SERVER_SCORES.bak-$STAMP"
        rm -f "$SERVER_SCORES"
        say "  cleared the local dev server's scoreboard ($SERVER_SCORES)"
    else
        say "  local dev server has no scoreboard file yet ($SERVER_SCORES)"
    fi
fi

say ""
say "next launch reads: $PREF_DIR"
say "undo with:         scripts/reset-prefs.sh --restore"
say ""
say "The WEB build keeps its own copy in the browser's localStorage, which this"
say "cannot reach. In the page's devtools console:"
say "    localStorage.removeItem('platformz.profile');"
say "    localStorage.removeItem('platformz.scores');"
say "Remember it is per ORIGIN and per BROWSER, so clearing it in Chrome leaves"
say "Safari's copy alone."
