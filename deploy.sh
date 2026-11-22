#!/usr/bin/env bash
# Sync pi/ to the thermostat and optionally build/run it there.
#
#   ./deploy.sh          push changed files
#   ./deploy.sh diff     show what a push would change, change nothing
#   ./deploy.sh build    push, then build on the Pi without running (checks libraries/compile errors)
#   ./deploy.sh run      push, then build and run (compile.py) on the Pi; Ctrl-C stops it
#   ./deploy.sh display-build   push, then build the display program on the Pi (pi/display/compile.py)
#   ./deploy.sh display-run     push, then build and run it; Ctrl-C stops it
#   ./deploy.sh sysroot  copy the Pi's headers into .sysroot/ so VS Code checks against the Pi;
#                        rerun after installing a -dev package on the Pi
#
# pi/ here is the source of truth: files deleted here are deleted on the Pi.
# Build output and binaries are excluded, so they are never sent or deleted.
# So are the saved settings: the Pi's copies are live data (written by the program),
# and pushing pi/'s example files would overwrite the user's set points and vacations.
set -euo pipefail

HOST=thermostat@thermostat
DEST=thermostat/                         # ~/thermostat on the Pi
ROOT="$(cd "$(dirname "$0")" && pwd)"
SRC="$ROOT/pi/"

RSYNC=(rsync -az --delete --itemize-changes
       --exclude=/build/ --exclude=/thermostat_control --exclude=/calibrate
       --exclude=/hvac_points.json --exclude=/vacation.json --exclude=/weather.json --exclude='/*.json.tmp'
       --exclude=/display/build/ --exclude=/display/thermostat_display
       --exclude=/display/display_preview --exclude=/display/preview_out/
       # LVGL: the Pi needs only src/, include/ and the top-level headers (~30 MB of ~200)
       --exclude=/display/lvgl/{docs,demos,examples,tests,scripts,env_support,libs,configs,zephyr,sbom}/
       --exclude=.git)

case "${1:-push}" in
    push)  "${RSYNC[@]}" "$SRC" "$HOST:$DEST" ;;
    diff)  "${RSYNC[@]}" --dry-run "$SRC" "$HOST:$DEST" ;;
    build) "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
           ssh "$HOST" "cd $DEST && python3 compile.py --no-run" ;;
    run)   "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
           ssh -t "$HOST" "cd $DEST && python3 compile.py" ;;
    display-build)
           "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
           ssh "$HOST" "cd $DEST/display && python3 compile.py --no-run" ;;
    display-run)
           "${RSYNC[@]}" "$SRC" "$HOST:$DEST"
           ssh -t "$HOST" "cd $DEST/display && python3 compile.py" ;;
    sysroot)
           # the Pi compiler's include search paths (g++ -E -v), mirrored at the same paths;
           # /usr/lib/linux/uapi is where Debian's asm/* header symlinks point
           mkdir -p "$ROOT/.sysroot"
           rsync -a --delete --relative \
               "$HOST:/usr/include" \
               "$HOST:/usr/lib/gcc/aarch64-linux-gnu/14/include" \
               "$HOST:/usr/lib/linux/uapi" \
               "$ROOT/.sysroot/" ;;
    *)     echo "usage: $0 [push|diff|build|run|display-build|display-run|sysroot]" >&2; exit 1 ;;
esac
