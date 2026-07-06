#!/usr/bin/env bash
#
# prune-large-history.sh — reclaim git-history space taken by SUPERSEDED web
# build artifacts.
#
# The web client (web/platformz.{data,wasm,js,html}) is a `make web` output that
# gets re-committed on every rebuild, so old copies pile up in history. This
# removes every NON-current version of those files from history and GCs. BFG
# protects HEAD by default, so the CURRENT committed copies stay intact (the
# GitHub Pages deploy keeps working) — only the out-of-date versions are purged.
#
# Safe to re-run periodically as new builds accumulate.
#
# History is rewritten, so every commit SHA from the first touched commit onward
# changes and a force-push is required. That step is gated behind --push and is
# never automatic.
#
# Requires: bfg (`brew install bfg`) + a JRE. macOS/Homebrew repo.
#
# Usage:
#   scripts/prune-large-history.sh          # rewrite locally, show size delta
#   scripts/prune-large-history.sh --push   # ...then force-push main

set -euo pipefail

# Artifact filenames whose old versions we strip. BFG matches by basename; these
# are unique to web/, and each one's copy in HEAD is protected (kept).
ARTIFACTS='{platformz.data,platformz.wasm,platformz.js,platformz.html}'

PUSH=0
[ "${1:-}" = "--push" ] && PUSH=1

cd "$(git rev-parse --show-toplevel)"

# --- safety gates -----------------------------------------------------------
branch=$(git branch --show-current)
if [ "$branch" != "main" ]; then
    echo "Refusing: expected branch 'main', but on '$branch'." >&2
    exit 1
fi
if ! git diff --quiet || ! git diff --cached --quiet; then
    echo "Refusing: working tree/index not clean. Commit or stash first." >&2
    exit 1
fi
if ! command -v bfg >/dev/null 2>&1; then
    echo "bfg not found. Install with: brew install bfg" >&2
    exit 1
fi

# Capture the true remote main SHA BEFORE rewriting. BFG rewrites ALL refs,
# including refs/remotes/origin/main, which would make a default
# --force-with-lease compare against a rewritten (wrong) value and fail with
# "stale info". We pin the lease to the real remote SHA instead, so the push
# still refuses if someone else moved origin/main in the meantime.
lease_sha=$(git ls-remote origin refs/heads/main 2>/dev/null | cut -f1)

# --- backup (all refs, so the pre-rewrite state is fully recoverable) --------
stamp=$(date +%Y%m%d-%H%M%S)
backup="../platformz-backup-$stamp.bundle"
git bundle create "$backup" --all
echo "Backup written: $backup"

before=$(du -sh .git | cut -f1)

# --- strip non-HEAD versions of the artifacts, then GC ----------------------
# (No --no-blob-protection: HEAD protection is exactly what keeps the current
#  build in place while older versions are removed.)
bfg --delete-files "$ARTIFACTS"
git reflog expire --expire=now --all
git gc --prune=now --aggressive

after=$(du -sh .git | cut -f1)
echo
echo ".git size: $before -> $after"
echo "Current artifacts (protected, still present in HEAD):"
for f in platformz.data platformz.wasm platformz.js platformz.html; do
    sz=$(git cat-file -s "HEAD:web/$f" 2>/dev/null || echo 0)
    printf "  web/%s: %d KB\n" "$f" "$((sz / 1024))"
done

# --- publish ----------------------------------------------------------------
if [ -n "$lease_sha" ]; then
    push_cmd="git push --force-with-lease=main:$lease_sha origin main"
else
    push_cmd="git push --force origin main" # no remote main yet; nothing to lease against
fi

if [ "$PUSH" -eq 1 ]; then
    echo
    echo "Force-pushing rewritten history to origin/main..."
    eval "$push_cmd"
else
    echo
    echo "Local history rewritten. Review, then publish with:"
    echo "  $push_cmd"
    echo "(or re-run this script with --push)"
fi
