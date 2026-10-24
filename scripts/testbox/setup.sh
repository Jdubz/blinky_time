#!/usr/bin/env bash
# Provision the acoustic test box's Pi 5 (docs/TEST_BOX_BUILD_PLAN.md).
#
# Runs as root. Idempotent: safe to re-run after a failure or on a rebuilt card.
#   First boot: cloud-init runs it from the SD card's boot partition
#               (/boot/firmware/blinky-testbox/setup.sh, see that dir's README).
#   Later:      sudo ~/blinky_time/scripts/testbox/setup.sh
#
# What it does (each step logs OK/FAIL; any FAIL makes the run exit non-zero):
#   1. apt upgrade + the packages the box needs (python, ffmpeg, uhubctl, gh, node...)
#   2. clone blinky_time into the user's home (never pulls an existing clone)
#   3. blinky-server venv with the `firmware` extra, NOT `ble` (the box runs --no-ble)
#   4. build blinky-console into blinky-server/web/
#   5. narrow sudoers rule for the commands the flash path runs under sudo
#   6. Bluetooth off at the OS level (the box server must never adopt a production device)
#   7. an SSH key for the user (for reaching the SWD Pi Zero)
#   8. blinky-server.service, enabled and started with --no-ble
#
# Log: /var/log/blinky-testbox-setup.log

set -uo pipefail

TARGET_USER="${SUDO_USER:-jdubz}"
REPO_URL="https://github.com/Jdubz/blinky_time.git"
SKIP_UPGRADE=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --user) TARGET_USER="$2"; shift 2 ;;
    --repo) REPO_URL="$2"; shift 2 ;;
    --skip-upgrade) SKIP_UPGRADE=1; shift ;;
    -h|--help) sed -n 2,22p "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

if [[ $EUID -ne 0 ]]; then
  echo "setup.sh must run as root (sudo $0)" >&2
  exit 2
fi
if ! id "$TARGET_USER" >/dev/null 2>&1; then
  echo "user '$TARGET_USER' does not exist" >&2
  exit 2
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
HOME_DIR="$(getent passwd "$TARGET_USER" | cut -d: -f6)"
REPO_DIR="$HOME_DIR/blinky_time"
SERVER_DIR="$REPO_DIR/blinky-server"
LOG=/var/log/blinky-testbox-setup.log
STATE_DIR=/var/lib/blinky-testbox

exec > >(tee -a "$LOG") 2>&1
echo "===== blinky test box setup $(date -Is) (user=$TARGET_USER, from $HERE) ====="

FAILED=()
log()  { echo "[setup] $*"; }
step() {  # step <name> <command...>: run, record OK/FAIL, never abort the run
  local name="$1"; shift
  log ">>> $name"
  if "$@"; then
    log "OK   $name"
  else
    log "FAIL $name (exit $?)"
    FAILED+=("$name")
  fi
}
as_user() { sudo -u "$TARGET_USER" -H "$@"; }
APT=(apt-get -y -o DPkg::Lock::Timeout=600 -o Dpkg::Options::=--force-confold)
export DEBIAN_FRONTEND=noninteractive

wait_for_network() {
  local i
  for i in $(seq 1 60); do
    getent hosts github.com >/dev/null && getent hosts deb.debian.org >/dev/null && return 0
    sleep 5
  done
  echo "no DNS for github.com / deb.debian.org after 5 min" >&2
  return 1
}

install_packages() {
  "${APT[@]}" update || return 1
  if [[ $SKIP_UPGRADE -eq 0 ]]; then
    "${APT[@]}" full-upgrade || return 1
  fi
  local pkgs=(
    git tmux curl jq htop rsync
    python3 python3-venv python3-dev python3-pip python3-serial build-essential
    ffmpeg alsa-utils               # ffplay for validation playback; aplay/amixer for the USB speaker
    uhubctl udisks2                 # USB port power-cycling; UF2 drive mounting
    gh                              # tools/firmware_release.py downloads releases with gh
    nodejs npm                      # blinky-console build
    rpicam-apps-lite i2c-tools      # camera bring-up, I2C debugging
    avahi-daemon
  )
  "${APT[@]}" install "${pkgs[@]}"
}

add_groups() {
  local g
  for g in dialout audio video plugdev gpio i2c; do
    getent group "$g" >/dev/null && usermod -aG "$g" "$TARGET_USER"
  done
  return 0
}

clone_repo() {
  if [[ -d "$REPO_DIR/.git" ]]; then
    log "repo already at $REPO_DIR; not pulling (update it yourself)"
    return 0
  fi
  as_user git clone "$REPO_URL" "$REPO_DIR"
}

server_venv() {
  [[ -d "$SERVER_DIR" ]] || { echo "$SERVER_DIR missing" >&2; return 1; }
  if [[ ! -x "$SERVER_DIR/venv/bin/python" ]]; then
    as_user python3 -m venv "$SERVER_DIR/venv" || return 1
  fi
  # firmware extra only: without bleak the server cannot open BLE even if --no-ble is dropped.
  as_user "$SERVER_DIR/venv/bin/pip" install -q --upgrade pip &&
    as_user "$SERVER_DIR/venv/bin/pip" install -q -e "$SERVER_DIR[firmware]"
}

build_console() {
  local console="$REPO_DIR/blinky-console"
  [[ -d "$console" ]] || { echo "$console missing" >&2; return 1; }
  ( cd "$console" &&
    as_user npm ci --no-audit --no-fund --loglevel=error &&
    as_user npm run build --silent ) || return 1
  [[ -f "$SERVER_DIR/web/index.html" ]] || { echo "build produced no $SERVER_DIR/web/index.html" >&2; return 1; }
}

install_sudoers() {
  local tmp out=/etc/sudoers.d/020_blinky-testbox
  tmp="$(mktemp)"
  sed -e "s|@USER@|$TARGET_USER|g" \
      -e "s|@REPO@|$REPO_DIR|g" \
      -e "s|@UHUBCTL@|$(command -v uhubctl)|g" \
      -e "s|@MOUNT@|$(command -v mount)|g" \
      -e "s|@UMOUNT@|$(command -v umount)|g" \
      -e "s|@MKDIR@|$(command -v mkdir)|g" \
      -e "s|@RMDIR@|$(command -v rmdir)|g" \
      -e "s|@SYSTEMCTL@|$(command -v systemctl)|g" \
      -e "s|@PYTHON3@|$(command -v python3)|g" \
      "$HERE/sudoers.in" > "$tmp" || return 1
  if ! visudo -cf "$tmp"; then
    echo "generated sudoers failed visudo; not installed" >&2
    rm -f "$tmp"; return 1
  fi
  install -m 0440 -o root -g root "$tmp" "$out"
  rm -f "$tmp"
}

disable_bluetooth() {
  local cfg=/boot/firmware/config.txt
  if ! grep -q '^dtoverlay=disable-bt' "$cfg"; then
    printf '\n[all]\n# blinky test box: no Bluetooth until per-server device ownership exists\n# (docs/TEST_BOX_BUILD_PLAN.md). Remove to re-enable, then reboot.\ndtoverlay=disable-bt\n' >> "$cfg" || return 1
    touch /var/run/reboot-required
  fi
  systemctl disable --now hciuart.service bluetooth.service 2>/dev/null
  return 0
}

user_ssh_key() {
  local key="$HOME_DIR/.ssh/id_ed25519"
  if [[ ! -f "$key" ]]; then
    as_user mkdir -p -m 700 "$HOME_DIR/.ssh" &&
      as_user ssh-keygen -q -t ed25519 -N "" -C "$TARGET_USER@$(hostname)" -f "$key" || return 1
  fi
  local cfg="$HOME_DIR/.ssh/config"
  if ! grep -qs '^Host swd-flash' "$cfg"; then
    as_user tee -a "$cfg" >/dev/null <<'EOF'
# SWD recovery Pi Zero W (see CLAUDE.md, "SWD recovery for fully-bricked devices")
Host swd-flash swd-flash.local
    HostName swd-flash.local
    User jdubz
    IdentityFile ~/.ssh/id_ed25519
EOF
    chmod 600 "$cfg"
  fi
}

install_service() {
  sed -e "s|@USER@|$TARGET_USER|g" -e "s|@SERVER_DIR@|$SERVER_DIR|g" \
    "$HERE/blinky-server.service.in" > /etc/systemd/system/blinky-server.service || return 1
  systemctl daemon-reload &&
    systemctl enable blinky-server.service &&
    systemctl restart blinky-server.service || return 1
  local i
  for i in $(seq 1 30); do
    curl -fsS -o /dev/null http://127.0.0.1:8420/api/devices && return 0
    sleep 2
  done
  echo "blinky-server not answering on :8420 after 60 s; journalctl -u blinky-server" >&2
  return 1
}

step "network"            wait_for_network
step "apt packages"       install_packages
step "user groups"        add_groups
step "clone blinky_time"  clone_repo
step "blinky-server venv" server_venv
step "blinky-console"     build_console
step "sudoers"            install_sudoers
step "bluetooth off"      disable_bluetooth
step "user ssh key"       user_ssh_key
step "blinky-server unit" install_service

mkdir -p "$STATE_DIR"
echo
echo "===== summary $(date -Is) ====="
if [[ ${#FAILED[@]} -eq 0 ]]; then
  log "all steps OK"
  date -Is > "$STATE_DIR/setup-ok"
  rm -f "$STATE_DIR/setup-failed"
else
  log "FAILED: ${FAILED[*]}"
  printf '%s\n' "${FAILED[@]}" > "$STATE_DIR/setup-failed"
fi
[[ -f /var/run/reboot-required ]] && log "a reboot is needed (kernel/firmware update or Bluetooth overlay)"
echo
log "next steps (manual):"
log "  gh auth login                      # release downloads"
log "  cat ~/.ssh/id_ed25519.pub          # add to swd-flash's authorized_keys"
log "  cat ~/.blinky-api-key              # this server's own API key"
[[ ${#FAILED[@]} -eq 0 ]]
