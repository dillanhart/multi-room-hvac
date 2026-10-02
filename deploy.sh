#!/usr/bin/env bash
# Sync pi/ to the thermostat and optionally build/run it there.
#
#   ./deploy.sh          push changed files
#   ./deploy.sh diff     show what a push would change, change nothing
#   ./deploy.sh run      push, then build and run (compile.py) on the Pi; Ctrl-C stops it
#
# pi/ here is the source of truth: files deleted here are deleted on the Pi.
# Build output and binaries are excluded, so they are never sent or deleted.
set -euo pipefail

HOST=thermostat@thermostat
DEST=thermostat/                         # ~/thermostat on the Pi
SRC="$(cd "$(dirname "$0")" && pwd)/pi/"

RSYNC=(rsync -az --delete --itemize-changes
       --exclude=/build/ --exclude=/thermostat_control --exclude=/calibrate
       --exclude=.git)

case "${1:-push}" in
    push) "${RSYNC[@]}" "$SRC" "$HOST:$DEST" ;;
    diff) "${RSYNC[@]}" --dry-run "$SRC" "$HOST:$DEST" ;;
    run)  "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
          ssh -t "$HOST" "cd $DEST && python3 compile.py" ;;
    *)    echo "usage: $0 [push|diff|run]" >&2; exit 1 ;;
esac
