#!/bin/bash
# Run blinky-server with auto-restart watchdog.
# Designed for tmux: `tmux new-session -d -s blinky ./run-server.sh`
#
# Restarts the server on crash with backoff (5s → 10s → 20s → 30s max).
# Resets backoff after 60s of stable runtime.
# Logs each restart with timestamp.

set -euo pipefail
cd "$(dirname "$0")"

VENV="./venv/bin/python"
LOG="/tmp/blinky-server.log"
BACKOFF=5
MAX_BACKOFF=30
STABLE_THRESHOLD=60  # seconds of uptime before resetting backoff

echo "=== Blinky Server Watchdog ==="
echo "Log: $LOG"
echo "Press Ctrl+C to stop"
echo ""

while true; do
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting server (backoff=${BACKOFF}s)..."
    START=$(date +%s)

    # Run server, append to log (preserves crash traces from previous runs)
    echo "--- server start $(date '+%Y-%m-%d %H:%M:%S') ---" >> "$LOG"
    $VENV -m blinky_server >> "$LOG" 2>&1 || true
    EXIT_CODE=$?
    UPTIME=$(( $(date +%s) - START ))

    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Server exited (code=$EXIT_CODE, uptime=${UPTIME}s)"

    # Reset backoff if server ran for a while (not a startup crash loop)
    if [ "$UPTIME" -ge "$STABLE_THRESHOLD" ]; then
        BACKOFF=5
    else
        BACKOFF=$(( BACKOFF * 2 ))
        if [ "$BACKOFF" -gt "$MAX_BACKOFF" ]; then
            BACKOFF=$MAX_BACKOFF
        fi
    fi

    echo "[$(date '+%Y-%m-%d %H:%M:%S')] Restarting in ${BACKOFF}s..."
    sleep "$BACKOFF"
done
