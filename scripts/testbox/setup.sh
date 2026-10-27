#!/usr/bin/env bash
# Provision the acoustic test box's Pi 5 (docs/TEST_BOX_BUILD_PLAN.md).
#
# Runs as root. Idempotent: safe to re-run after a failure or on a rebuilt card.
#   First boot: cloud-init runs it from the SD card's boot partition
#               (/boot/firmware/blinky-testbox/setup.sh; docs/TEST_BOX_BUILD_PLAN.md, Software setup).
#   Later:      sudo ~/blinky_time/scripts/testbox/setup.sh
#
# What it does (each step logs OK/FAIL; any FAIL makes the run exit non-zero):
#   1. apt upgrade + the packages the box needs (python, ffmpeg, uhubctl, gh, node...)
#   2. clone blinky_time into the user's home (never pulls an existing clone)
#   3. blinky-server venv with the `firmware` and `ble` extras (the server imports bleak
#      unconditionally; the box's BLE guard is --no-ble plus dtoverlay=disable-bt)
#   4. build blinky-console into blinky-server/web/
#   5. passwordless sudo for the user: the box's administrator is the Claude session
#      that develops on it (sudoers.in)
#   6. Bluetooth off at the OS level (the box server must never adopt a production device)
#   7. an SSH key for the user (for reaching the SWD Pi Zero)
#   8. blinky-server.service, enabled and started with --no-ble
#   9. development tooling, so the box can run every script, tool and CI check itself:
#      arduino-cli (pinned) + the firmware toolchain, cppcheck (pinned, CI's build),
#      adafruit-nrfutil, a dev Python venv, blinky-serial-mcp, git hooks, Claude Code
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
    -h|--help) sed -n 2,25p "$0"; exit 0 ;;
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
    # development (the box is also a dev machine: Claude sessions in tmux)
    cmake                           # cppcheck build, blinky-simulator
    g++-13                          # CI's host compiler: GCC 14 rejects TFLite's bundled flatbuffers
    ripgrep fd-find shellcheck sox picocom unzip pipx
    python3-numpy python3-scipy python3-yaml python3-matplotlib python3-cryptography
    python3-pytest                  # tools/ and CI's python checks on the system python
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
  # `ble` is required, not optional: device/manager.py imports ble.advertiser (dbus_fast) and
  # firmware/ble_dfu.py (bleak) at startup. BLE stays off via --no-ble and dtoverlay=disable-bt.
  # `dev` is CI's ruff/mypy/pytest (pr-validation.yml, blinky-server-tests), in the same venv.
  as_user "$SERVER_DIR/venv/bin/pip" install -q --upgrade pip &&
    as_user "$SERVER_DIR/venv/bin/pip" install -q -e "$SERVER_DIR[firmware,ble,dev]"
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
  sed -e "s|@USER@|$TARGET_USER|g" "$HERE/sudoers.in" > "$tmp" || return 1
  if ! visudo -cf "$tmp"; then
    echo "generated sudoers failed visudo; not installed" >&2
    rm -f "$tmp"; return 1
  fi
  install -m 0440 -o root -g root "$tmp" "$out"
  rm -f "$tmp"
  # Earlier versions installed a narrow rule on a root-owned usb_reset.py copy;
  # the grant above replaces both.
  rm -rf /usr/local/libexec/blinky
  # As the user, with no cached ticket: it must work without a password.
  if ! as_user sudo -n -k true </dev/null; then
    echo "passwordless sudo is not in effect for $TARGET_USER after installing $out" >&2
    return 1
  fi
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

# --- development tooling ----------------------------------------------------------
# The box doubles as a dev machine (Claude sessions in tmux), so it runs the same
# scripts and CI checks as a workstation, with the same pins.

install_arduino_cli() {
  # The version is pinned in tools/firmware_toolchain.json; build.sh refuses any other.
  local want have tgz tmp rc
  [[ "$(uname -m)" == aarch64 ]] || { echo "arduino-cli step expects aarch64, got $(uname -m)" >&2; return 1; }
  want="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["arduino_cli"])' \
    "$REPO_DIR/tools/firmware_toolchain.json")" || return 1
  have="$(/usr/local/bin/arduino-cli version 2>/dev/null | sed -n 's/.*Version: *\([^ ]*\).*/\1/p')"
  [[ "$have" == "$want" ]] && return 0
  tgz="arduino-cli_${want}_Linux_ARM64.tar.gz"
  tmp="$(mktemp -d)"
  ( cd "$tmp" &&
    curl -fsSLO "https://github.com/arduino/arduino-cli/releases/download/v$want/$tgz" &&
    curl -fsSLO "https://github.com/arduino/arduino-cli/releases/download/v$want/$want-checksums.txt" &&
    grep -E "[[:space:]]$tgz\$" "$want-checksums.txt" | sha256sum -c --strict - &&
    tar -xzf "$tgz" arduino-cli &&
    install -m 0755 arduino-cli /usr/local/bin/arduino-cli )
  rc=$?; rm -rf "$tmp"; return $rc
}

firmware_toolchain() {
  # The pinned core, libraries, TFLite and core patches into <repo>/.arduino (#252).
  ( cd "$REPO_DIR" && as_user bash scripts/setup_toolchain.sh )
}

CPPCHECK_VERSION=2.16.0   # = pr-validation.yml's cppcheck job: apt's version misses findings (#201)
install_cppcheck() {
  local prefix="/opt/cppcheck-$CPPCHECK_VERSION" src rc
  if [[ "$("$prefix/bin/cppcheck" --version 2>/dev/null)" != "Cppcheck $CPPCHECK_VERSION" ]]; then
    src="$(mktemp -d)"
    git clone -q --depth 1 --branch "$CPPCHECK_VERSION" https://github.com/danmar/cppcheck.git "$src/cppcheck" &&
      cmake -S "$src/cppcheck" -B "$src/build" -DCMAKE_BUILD_TYPE=Release -DUSE_MATCHCOMPILER=ON \
        -DCMAKE_INSTALL_PREFIX="$prefix" >/dev/null &&
      cmake --build "$src/build" -j"$(nproc)" >/dev/null &&
      cmake --install "$src/build" >/dev/null
    rc=$?; rm -rf "$src"; [[ $rc -eq 0 ]] || return $rc
  fi
  ln -sf "$prefix/bin/cppcheck" /usr/local/bin/cppcheck
}

DEV_VENV="$HOME_DIR/.venvs/blinky"
dev_python() {
  # adafruit-nrfutil: the core's .zip packager, which build.sh's compile runs from PATH.
  if ! as_user pipx list --short 2>/dev/null | grep -q '^adafruit-nrfutil '; then
    as_user pipx install adafruit-nrfutil || return 1
  fi
  # On top of the apt python3-* packages, what apt doesn't ship: tflite for
  # tools/check_model_input.py (CI's model-input-check); librosa, soundfile and
  # mir_eval for ml-training's CPU analysis. Training itself runs on the Spark.
  if [[ ! -x "$DEV_VENV/bin/python" ]]; then
    as_user python3 -m venv --system-site-packages "$DEV_VENV" || return 1
  fi
  as_user "$DEV_VENV/bin/pip" install -q --upgrade pip &&
    as_user "$DEV_VENV/bin/pip" install -q "tflite==2.18.0" librosa soundfile mir_eval || return 1
  # Login shells (tmux starts them) get the venv and ~/.local/bin first on PATH.
  local profile="$HOME_DIR/.profile"
  # Replace the managed block whenever it changes (sed drops the old one).
  if ! grep -qs 'export CXX=g++-13' "$profile"; then
    as_user sed -i '/^# >>> blinky test box/,/^# <<< blinky test box/d' "$profile" || return 1
    as_user tee -a "$profile" >/dev/null <<'EOF'

# >>> blinky test box (scripts/testbox/setup.sh) >>>
# The dev venv (system site packages included) and per-user tools (claude, pipx apps).
PATH="$HOME/.venvs/blinky/bin:$HOME/.local/bin:$PATH"
# CI's host compiler (ubuntu-latest, GCC 13). tests/tracker/build_host_nn.sh builds
# TFLite Micro with $CXX, and GCC 14 rejects its bundled flatbuffers (stl_emulation.h).
export CXX=g++-13
# <<< blinky test box <<<
EOF
  fi
}

serial_mcp() {
  local mcp="$REPO_DIR/blinky-serial-mcp"
  ( cd "$mcp" &&
    as_user npm ci --no-audit --no-fund --loglevel=error &&
    as_user npm run build --silent ) || return 1
  [[ -f "$mcp/dist/index.js" ]] || { echo "build produced no $mcp/dist/index.js" >&2; return 1; }
}

git_hooks() {
  ( cd "$REPO_DIR" && as_user bash scripts/install-hooks.sh )
}

tmux_conf() {
  local f="$HOME_DIR/.tmux.conf"
  [[ -f "$f" ]] && return 0
  as_user tee "$f" >/dev/null <<'EOF'
# Written once by scripts/testbox/setup.sh; yours to edit, it is never overwritten.
set -g history-limit 100000
set -g mouse on
set -g default-terminal "tmux-256color"
EOF
}

claude_code() {
  # Native installer: per user, self-updating, ~/.local/bin/claude. Sign in by running `claude` once.
  local claude="$HOME_DIR/.local/bin/claude"
  if [[ ! -x "$claude" ]]; then
    as_user bash -o pipefail -c 'curl -fsSL https://claude.ai/install.sh | bash' || return 1
  fi
  # The repo's .mcp.json reaches blinky-serial over ssh to blinkyhost. On the box it
  # runs locally against this box's own blinky-server; local scope overrides project
  # scope for this checkout only.
  ( cd "$REPO_DIR" &&
    as_user "$claude" mcp remove --scope local blinky-serial >/dev/null 2>&1
    as_user "$claude" mcp add --scope local blinky-serial -- node "$REPO_DIR/blinky-serial-mcp/dist/index.js" )
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
step "arduino-cli"        install_arduino_cli
step "firmware toolchain" firmware_toolchain
step "cppcheck"           install_cppcheck
step "dev python"         dev_python
step "blinky-serial-mcp"  serial_mcp
step "git hooks"          git_hooks
step "tmux config"        tmux_conf
step "claude code"        claude_code

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
log "  claude                             # sign in to Claude Code (once)"
[[ ${#FAILED[@]} -eq 0 ]]
