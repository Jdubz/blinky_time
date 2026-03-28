#!/bin/bash
# Launch blinky-server in a tmux session with auto-restart on crash.
# No sudo needed. Run from anywhere.
#
# Usage:
#   ./run.sh              Start server (detached tmux session)
#   ./run.sh stop         Stop server
#   ./run.sh status       Check if running
#   ./run.sh logs         Attach to tmux session (Ctrl-B D to detach)
#   ./run.sh restart      Stop + start
#   ./run.sh start --no-ble --wifi-device 192.168.86.238:3333

set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
SESSION="blinky-server"
VENV="$SCRIPT_DIR/venv"
LOG="/tmp/blinky-server.log"

cmd="${1:-start}"
shift 2>/dev/null || true  # Remove the command, forward remaining args
EXTRA_ARGS="$*"  # Capture remaining args for embedding in tmux command

case "$cmd" in
  stop)
    tmux kill-session -t "$SESSION" 2>/dev/null && echo "Stopped." || echo "Not running."
    ;;

  status)
    if tmux has-session -t "$SESSION" 2>/dev/null; then
      echo "Running (tmux session '$SESSION')"
      echo "Log: $LOG"
      tail -5 "$LOG" 2>/dev/null
    else
      echo "Not running."
    fi
    ;;

  logs)
    exec tmux attach -t "$SESSION"
    ;;

  restart)
    tmux kill-session -t "$SESSION" 2>/dev/null || true
    sleep 1
    exec "$0" start $EXTRA_ARGS
    ;;

  start)
    if tmux has-session -t "$SESSION" 2>/dev/null; then
      echo "Already running. Use '$0 restart' to restart."
      exit 0
    fi

    # Ensure venv exists
    if [ ! -d "$VENV" ]; then
      echo "Creating venv..."
      python3 -m venv "$VENV"
      "$VENV/bin/pip" install -e "$SCRIPT_DIR[ble]"
    fi

    # Launch in tmux with auto-restart loop
    tmux new-session -d -s "$SESSION" "
      cd '$SCRIPT_DIR'
      while true; do
        echo '=== blinky-server starting at \$(date) ===' | tee -a '$LOG'
        '$VENV/bin/python' -m blinky_server $EXTRA_ARGS 2>&1 | tee -a '$LOG'
        echo '=== crashed at \$(date), restarting in 3s ===' | tee -a '$LOG'
        sleep 3
      done
    "
    echo "Started in tmux session '$SESSION'"
    echo "  Logs:    $0 logs  (or tail -f $LOG)"
    echo "  Stop:    $0 stop"
    echo "  Status:  $0 status"
    ;;

  *)
    echo "Usage: $0 [start|stop|status|logs|restart]"
    exit 1
    ;;
esac
