#!/usr/bin/env bash
# Sync pi/ to the thermostat and optionally build/run it there.
#
#   ./deploy.sh          push changed files
#   ./deploy.sh diff     show what a push would change, change nothing
#   ./deploy.sh build    push, then build on the Pi without running (checks libraries/compile errors)
#   ./deploy.sh run      push, then build and run (compile.py) on the Pi; Ctrl-C stops it
#   ./deploy.sh sysroot  copy the Pi's headers into .sysroot/ so VS Code checks against the Pi;
#                        rerun after installing a -dev package on the Pi
#
# pi/ here is the source of truth: files deleted here are deleted on the Pi.
# Build output and binaries are excluded, so they are never sent or deleted.
set -euo pipefail

HOST=thermostat@thermostat
DEST=thermostat/                         # ~/thermostat on the Pi
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/pi/"

RSYNC=(rsync -az --delete --itemize-changes
       --exclude=/build/ --exclude=/thermostat_control --exclude=/calibrate
       --exclude=.git)

case "${1:-push}" in
    push)  "${RSYNC[@]}" "$SRC" "$HOST:$DEST" ;;
    diff)  "${RSYNC[@]}" --dry-run "$SRC" "$HOST:$DEST" ;;
    build) "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
           ssh "$HOST" "cd $DEST && python3 compile.py --no-run" ;;
    run)   "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
           ssh -t "$HOST" "cd $DEST && python3 compile.py" ;;
    sysroot)
           # the Pi compiler's include search paths (g++ -E -v), mirrored at the same paths;
           # /usr/lib/linux/uapi is where Debian's asm/* header symlinks point
           mkdir -p "$ROOT/.sysroot"
           rsync -a --delete --relative \
               "$HOST:/usr/include" \
               "$HOST:/usr/lib/gcc/aarch64-linux-gnu/14/include" \
               "$HOST:/usr/lib/linux/uapi" \
               "$ROOT/.sysroot/" ;;
    *)     echo "usage: $0 [push|diff|build|run|sysroot]" >&2; exit 1 ;;
esac
