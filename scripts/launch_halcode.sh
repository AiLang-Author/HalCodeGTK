#!/usr/bin/env bash
# Click-to-launch HalCode GTK from the Applications menu. No rebuild.
# One desk + one GTK shell. Replaces a previous GUI instance in place.
# Does not touch HalCode9000.x --mcp.
#
# Copyright (c) 2026 Sean Collins, 2 Paws Machine and Engineering. MIT.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SHELL_DIR="$ROOT/shell"
STATE="${HALCODE_APP_STATE:-/tmp/halcode_app}"
DESK="$SHELL_DIR/halcode_desk.x"
HOST="$SHELL_DIR/halcode_shell_gtk"
LOG="$STATE/launch.log"

if [[ -z "${DISPLAY:-}" ]]; then
    if [[ -S /tmp/.X11-unix/X0 ]]; then export DISPLAY=:0
    elif [[ -S /tmp/.X11-unix/X1 ]]; then export DISPLAY=:1
    else
        echo "ERROR: no DISPLAY. Log into a Linux desktop first." >&2
        exit 1
    fi
fi

[[ -x "$DESK" ]] || { echo "missing $DESK — make -C $SHELL_DIR desk" >&2; exit 1; }
[[ -x "$HOST" ]] || { echo "missing $HOST — make -C $SHELL_DIR" >&2; exit 1; }

mkdir -p "$STATE"
echo "---- $(date -Iseconds) launch_halcode ----" >> "$LOG"

# Exact /proc comm only. Never -f (would catch HalCode9000.x --mcp).
# Linux comm is 15 chars: halcode_shell_gtk → halcode_shell_g
pkill -x halcode_desk.x 2>/dev/null || true
pkill -x halcode_shell_g 2>/dev/null || true
if [[ -f /tmp/halcode_gtk.pid ]]; then kill "$(cat /tmp/halcode_gtk.pid)" 2>/dev/null || true; fi
if [[ -f /tmp/halcode_desk.pid ]]; then kill "$(cat /tmp/halcode_desk.pid)" 2>/dev/null || true; fi
sleep 0.2
rm -f /tmp/halcode_gtk.pid /tmp/halcode_desk.pid
: > "$STATE/cmd.txt"

echo "HalCode GTK: $DESK + $HOST"
export HALCODE_ROOT="$ROOT"
cd "$ROOT"
"$DESK" >>"$LOG" 2>&1 &
DESK_PID=$!
echo "$DESK_PID" > /tmp/halcode_desk.pid
cleanup() { kill "$DESK_PID" 2>/dev/null || true; wait "$DESK_PID" 2>/dev/null || true; }
trap cleanup EXIT INT TERM
sleep 0.4
if ! kill -0 "$DESK_PID" 2>/dev/null; then
    echo "ERROR: halcode_desk.x exited — see $LOG"
    tail -20 "$LOG" >&2 || true
    exit 1
fi
export HALCODE_APP_STATE="$STATE"
"$HOST" "$STATE"
