#!/usr/bin/env bash
# Provision the test box's Pi 5 (docs/TEST_BOX.md).
#
# Runs as root. Idempotent: safe to re-run after a failure or on a rebuilt card.
#   First boot: cloud-init runs it from the SD card's boot partition
#               (/boot/firmware/blinky-testbox/setup.sh; docs/TEST_BOX.md, Software setup).
#   Later:      sudo ~/blinky_time/scripts/testbox/setup.sh
#
# What it does (each step logs OK/FAIL; any FAIL makes the run exit non-zero):
#   1. apt upgrade + the packages the box needs (python, ffmpeg, uhubctl, gh, node...)
#   2. clone blinky_time into the user's home (never pulls an existing clone), with its corpus
#      submodule (the only one)
#   3. blinky-server venv with the `firmware` and `ble` extras (the server imports bleak
#      unconditionally; the box's BLE guard is --no-ble plus dtoverlay=disable-bt)
#   4. build blinky-console into blinky-server/web/
#   5. passwordless sudo for the user: the box's administrator is the Claude session
#      that develops on it (sudoers.in)
#   6. Bluetooth off at the OS level (the box server must never adopt a production device),
#      and a udev rule so the user can drive the XIAO Debug Mate (SWD recovery) with OpenOCD
#   7. an SSH key for the user
#   8. the deploy worktree (~/blinky-deploy -> ~/blinky-deploy-a|b, master only) and
#      blinky-server.service running from it with --no-ble; blinky-deploy.timer keeps it at
#      master once CI and review pass (deploy.py). The dev clone is never what the server runs.
#   9. Grafana Alloy, shipping the box's journal, host metrics and traces to the central LGTM
#      stack (scripts/testbox/alloy/testbox.alloy). Its push endpoints come from
#      the private ~/.config/blinky/observability.env (template: observability.env.example);
#      this step fails, naming the file and keys, until it exists
#  9b. the private host config ~/.config/blinky/hosts.env (template: ../hosts.env.example):
#      BLINKY_TESTBOX_URL, the box's URL as the home network reaches it, for the staging
#      runner's report links. This step fails, naming the file and key, until it's set
#  10. development tooling for this repo: ~/.local/bin on PATH (a managed ~/.profile block),
#      blinky-serial-mcp, git hooks, Claude Code (with Linux MCP servers: blinky-serial on this
#      box, playwright + its Chromium). No dev venv: ml analysis runs on CI or the Spark, and
#      blinky-server's tests use its own venv (blinky-server/venv)
#
# No firmware toolchain (arduino-cli, the pinned core, cppcheck, GCC 13, adafruit-nrfutil): the box
# flashes published images with blinky-server's own host tools. To develop firmware on the box,
# clone Jdubz/blinky-firmware and follow its docs/BUILD_GUIDE.md (arduino-cli at the version its
# tools/firmware_toolchain.json pins, then `bash scripts/setup_toolchain.sh`).
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
    -h|--help) sed -n '2,/^# Log:/p' "$0"; exit 0 ;;
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
DEPLOY_DIR="$HOME_DIR/blinky-deploy"   # a symlink to the live slot (deploy.py)
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
    openocd                         # SWD recovery through the XIAO Debug Mate (swd_recover.sh)
    gh                              # release downloads (hosttools/firmware_release.py), staging.py
    nodejs npm                      # blinky-console build
    rpicam-apps-lite i2c-tools      # camera bring-up, I2C debugging
    avahi-daemon
    # development (the box is also a dev machine: Claude sessions in tmux)
    ripgrep fd-find shellcheck sox picocom unzip pipx
    python3-numpy python3-scipy python3-yaml python3-matplotlib python3-cryptography
    python3-pytest                  # tools/ and CI's python checks on the system python
    python3-picamera2               # the box camera (scripts/testbox/camera/)
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
  else
    as_user git clone "$REPO_URL" "$REPO_DIR" || return 1
  fi
  # blinky-corpus at the commit this repo pins (#357): the server imports its blinky_scoring.
  ( cd "$REPO_DIR" && as_user git submodule update --init corpus )
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
    as_user "$SERVER_DIR/venv/bin/pip" install -q -e "${SERVER_DIR}[firmware,ble,dev]"
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
    printf '\n[all]\n# blinky test box: no Bluetooth until per-server device ownership exists\n# (docs/TEST_BOX.md). Remove to re-enable, then reboot.\ndtoverlay=disable-bt\n' >> "$cfg" || return 1
    touch /var/run/reboot-required
  fi
  systemctl disable --now hciuart.service bluetooth.service 2>/dev/null
  return 0
}

# The XIAO Debug Mate's CMSIS-DAP (0d28:0204) is root-only by default; OpenOCD
# runs as the user (blinky-firmware scripts/swd_recover.sh, docs/SWD_RECOVERY.md).
debug_mate_udev() {
  cat > /etc/udev/rules.d/60-xiao-debug-mate.rules <<'RULES' || return 1
SUBSYSTEM=="hidraw", ATTRS{idVendor}=="0d28", ATTRS{idProduct}=="0204", MODE="0660", GROUP="plugdev", TAG+="uaccess"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0d28", ATTRS{idProduct}=="0204", MODE="0660", GROUP="plugdev", TAG+="uaccess"
RULES
  udevadm control --reload || return 1
  udevadm trigger --subsystem-match=hidraw --action=add || return 1
  udevadm trigger --subsystem-match=usb --attr-match=idVendor=0d28 --action=add || return 1
  return 0
}

user_ssh_key() {
  local key="$HOME_DIR/.ssh/id_ed25519"
  if [[ ! -f "$key" ]]; then
    as_user mkdir -p -m 700 "$HOME_DIR/.ssh" &&
      as_user ssh-keygen -q -t ed25519 -N "" -C "$TARGET_USER@$(hostname)" -f "$key" || return 1
  fi
}

deploy_worktree() {
  # ~/blinky-deploy -> ~/blinky-deploy-a: a worktree of the dev clone (shared objects; of the
  # submodules only the corpus is checked out), detached at master, with its own venv and
  # console build. deploy.py owns it from here on, alternating between -a and -b.
  as_user python3 "$REPO_DIR/scripts/testbox/deploy.py" --init
}

install_service() {
  local unit
  for unit in blinky-server.service blinky-deploy.service blinky-deploy.timer \
    blinky-staging.service blinky-staging.timer; do
    sed -e "s|@USER@|$TARGET_USER|g" -e "s|@SERVER_DIR@|$DEPLOY_DIR/blinky-server|g" \
      -e "s|@DEPLOY_DIR@|$DEPLOY_DIR|g" -e "s|@HOME_DIR@|$HOME_DIR|g" \
      "$HERE/$unit.in" > "/etc/systemd/system/$unit" || return 1
  done
  systemctl daemon-reload &&
    systemctl enable blinky-server.service &&
    systemctl restart blinky-server.service &&
    systemctl enable --now blinky-deploy.timer &&
    systemctl enable --now blinky-staging.timer || return 1
  local i
  for i in $(seq 1 30); do
    curl -fsS -o /dev/null http://127.0.0.1:8420/api/devices && return 0
    sleep 2
  done
  echo "blinky-server not answering on :8420 after 60 s; journalctl -u blinky-server" >&2
  return 1
}

# --- development tooling ----------------------------------------------------------
# The box doubles as a dev machine for this repo (Claude sessions in tmux). The firmware's
# toolchain is blinky-firmware's to set up (the header says how).

# Grafana Alloy: the box's telemetry forwarder (logs, host metrics, traces) to the central LGTM
# stack (Josh-Wentworth/observability). Pinned to the version the central stack and the
# Spark's forwarder run. deploy.py keeps /etc/alloy/config.alloy in step with the live slot.
ALLOY_VERSION="1.16.0-1"
# The stack's push endpoints are topology, and this repo is public: they live only in a private
# file on the box (mode 600), never here. observability.env.example is the template.
OBS_ENV="$HOME_DIR/.config/blinky/observability.env"
OBS_KEYS=(LOKI_PUSH_URL PROM_RW_URL TEMPO_OTLP_ENDPOINT)

# obs_value <key>: the key's value from $OBS_ENV (KEY=value or KEY="value"; the last one wins).
# Parsed, never sourced: this runs as root and the file is the user's.
obs_value() {
  local v
  v=$(sed -n "s/^[[:space:]]*$1[[:space:]]*=[[:space:]]*//p" "$OBS_ENV" | tail -n 1) || return 1
  v="${v%"${v##*[![:space:]]}"}"            # trailing whitespace
  if [[ $v =~ ^\"(.*)\"$ || $v =~ ^\'(.*)\'$ ]]; then v="${BASH_REMATCH[1]}"; fi
  printf '%s' "$v"
}

# read_observability_env: set the OBS_KEYS from $OBS_ENV, or say which file and keys to supply.
# First boot: a copy next to this script (the card's blinky-testbox/observability.env) seeds it.
read_observability_env() {
  if [[ ! -f "$OBS_ENV" && -f "$HERE/observability.env" ]]; then
    # As the user, so a ~/.config that doesn't exist yet isn't created root-owned.
    as_user install -d -m 0700 "$(dirname "$OBS_ENV")" &&
      install -m 0600 -o "$TARGET_USER" -g "$(id -gn "$TARGET_USER")" \
        "$HERE/observability.env" "$OBS_ENV" || return 1
    log "seeded $OBS_ENV from $HERE/observability.env (delete that copy from the card)"
  fi
  if [[ ! -f "$OBS_ENV" ]]; then
    echo "missing $OBS_ENV: it holds the LGTM stack's push endpoints (${OBS_KEYS[*]})." >&2
    echo "Copy $HERE/observability.env.example there, fill in the values (the owner's private" >&2
    echo "notes, or /etc/default/alloy on a provisioned box), chmod 600, and re-run setup.sh." >&2
    return 1
  fi
  local key val bad=()
  # URLs for Loki and Prometheus, host:port for Tempo's OTLP/gRPC. No quotes, spaces, $ or `,
  # which /etc/default/alloy (a systemd EnvironmentFile) would read differently.
  local url='^https?://[A-Za-z0-9.-]+(:[0-9]+)?(/[^[:space:]"$`\\]*)?$' hostport='^[A-Za-z0-9.-]+:[0-9]+$'
  for key in "${OBS_KEYS[@]}"; do
    val=$(obs_value "$key") || return 1
    case "$key" in
      TEMPO_OTLP_ENDPOINT) [[ $val =~ $hostport ]] || bad+=("$key") ;;
      *)                   [[ $val =~ $url ]] || bad+=("$key") ;;
    esac
    printf -v "$key" '%s' "$val"
  done
  if [[ ${#bad[@]} -gt 0 ]]; then
    echo "$OBS_ENV: missing, empty or malformed: ${bad[*]} (URLs for LOKI_PUSH_URL and" >&2
    echo "PROM_RW_URL, host:port for TEMPO_OTLP_ENDPOINT; see observability.env.example)." >&2
    return 1
  fi
}

# host_config: ~/.config/blinky/hosts.env (the repo is public, so host endpoints live only there).
# First boot: a copy next to this script (the card's blinky-testbox/hosts.env) seeds it.
# staging.py refuses to run without BLINKY_TESTBOX_URL, so say now which file and key it needs.
HOSTS_ENV="$HOME_DIR/.config/blinky/hosts.env"
host_config() {
  if [[ ! -f "$HOSTS_ENV" && -f "$HERE/hosts.env" ]]; then
    as_user install -d -m 0700 "$(dirname "$HOSTS_ENV")" &&
      install -m 0600 -o "$TARGET_USER" -g "$(id -gn "$TARGET_USER")" \
        "$HERE/hosts.env" "$HOSTS_ENV" || return 1
    log "seeded $HOSTS_ENV from $HERE/hosts.env (delete that copy from the card)"
  fi
  # The file, not the environment: the staging timer's unit has no environment of its own, so
  # staging.py on the box finds the URL only in the file.
  local v=""
  [[ -f "$HOSTS_ENV" ]] && v=$(sed -n 's/^[[:space:]]*BLINKY_TESTBOX_URL[[:space:]]*=[[:space:]]*//p' "$HOSTS_ENV" | tail -n 1)
  v="${v%$'\r'}"                           # a file saved on Windows
  v="${v%"${v##*[![:space:]]}"}"            # trailing whitespace
  if [[ $v =~ ^\"(.*)\"$ || $v =~ ^\'(.*)\'$ ]]; then v="${BASH_REMATCH[1]}"; fi
  if [[ ! $v =~ ^https?://[^[:space:]\<\>]+$ ]]; then
    echo "$HOSTS_ENV: BLINKY_TESTBOX_URL missing or a placeholder. Copy the repo's" >&2
    echo "scripts/hosts.env.example there (mode 600), set BLINKY_TESTBOX_URL=http://<this box>:8420" >&2
    echo "(the owner's private notes), and re-run setup.sh. The staging runner won't run until then." >&2
    return 1
  fi
}

install_alloy() {
  read_observability_env || return 1
  install -d -m 0755 /etc/apt/keyrings || return 1
  if [[ ! -s /etc/apt/keyrings/grafana.gpg ]]; then
    curl -fsSL https://apt.grafana.com/gpg.key | gpg --dearmor -o /etc/apt/keyrings/grafana.gpg || return 1
  fi
  echo "deb [signed-by=/etc/apt/keyrings/grafana.gpg] https://apt.grafana.com stable main" \
    > /etc/apt/sources.list.d/grafana.list
  "${APT[@]}" update || return 1
  apt-mark unhold alloy >/dev/null 2>&1
  "${APT[@]}" install --allow-downgrades "alloy=$ALLOY_VERSION" || return 1
  apt-mark hold alloy >/dev/null || return 1
  usermod -aG systemd-journal alloy || return 1   # loki.source.journal reads the system journal
  cat > /etc/default/alloy <<ENV
# Grafana Alloy on the test box. Written by blinky_time scripts/testbox/setup.sh (install_alloy);
# re-run setup.sh rather than editing by hand. The config itself is installed by deploy.py.
CONFIG_FILE="/etc/alloy/config.alloy"
# UI and /metrics on loopback only (the central stack is reached by push, never scraped).
CUSTOM_ARGS="--server.http.listen-addr=127.0.0.1:12345 --disable-reporting"
RESTART_ON_UPGRADE=true
# The central LGTM stack's push endpoints (tailnet-only, no auth), from $OBS_ENV.
LOKI_PUSH_URL="$LOKI_PUSH_URL"
PROM_RW_URL="$PROM_RW_URL"
TEMPO_OTLP_ENDPOINT="$TEMPO_OTLP_ENDPOINT"
ENV
  local src="$DEPLOY_DIR/scripts/testbox/alloy/testbox.alloy"
  [[ -f "$src" ]] || src="$HERE/alloy/testbox.alloy"
  install -m 0644 "$src" /etc/alloy/config.alloy || return 1
  systemctl enable alloy || return 1
  systemctl restart alloy
}

user_profile() {
  # Login shells (tmux starts them) get ~/.local/bin first on PATH. setup.sh owns the block
  # between the markers and rewrites it on every run; the rest of ~/.profile is left alone.
  # No dev venv and no CXX: nothing here compiles C++, and ml analysis runs on CI or the
  # Spark, not the box. An older block also put ~/.venvs/blinky on PATH and exported CXX.
  local profile="$HOME_DIR/.profile" block
  block=$(cat <<'EOF'
# >>> blinky test box (scripts/testbox/setup.sh) >>>
# Per-user tools (claude, pipx apps).
PATH="$HOME/.local/bin:$PATH"
# <<< blinky test box <<<
EOF
)
  if grep -qs '^# >>> blinky test box' "$profile"; then
    # The block goes in as an argument: sudo doesn't pass the environment through.
    as_user python3 - "$profile" "$block" <<'PY' || return 1
import re, sys
path, block = sys.argv[1], sys.argv[2]
text = open(path, encoding="utf-8").read()
new, n = re.subn(r"^# >>> blinky test box.*?^# <<< blinky test box <<<$", lambda _: block,
                 text, count=1, flags=re.M | re.S)
if n != 1:  # an opening marker with no closing one: report it, never keep the old block
    sys.exit(f"{path}: the blinky test box block has no closing marker; fix it by hand")
if new != text:
    open(path, "w", encoding="utf-8").write(new)
PY
  else
    printf '\n%s\n' "$block" | as_user tee -a "$profile" >/dev/null
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
  # The repo's .mcp.json reaches blinky-serial over ssh to the fleet server. On the box it
  # runs locally against this box's own blinky-server; local scope overrides project
  # scope for this checkout only.
  # Each `remove` only clears an earlier run's entry (absent on the first run, so its
  # status is ignored); every `add` must succeed.
  ( cd "$REPO_DIR" || exit 1
    as_user "$claude" mcp remove --scope local blinky-serial >/dev/null 2>&1
    as_user "$claude" mcp add --scope local blinky-serial -- node "$REPO_DIR/blinky-serial-mcp/dist/index.js" || exit 1
    # .mcp.json starts playwright through Windows' `cmd /c`; on Linux, npx directly.
    as_user "$claude" mcp remove --scope local playwright >/dev/null 2>&1
    as_user "$claude" mcp add --scope local playwright -- npx -y @executeautomation/playwright-mcp-server || exit 1
  ) || return 1
  # Its browser, installed by the playwright the server bundles (a bare `npx playwright`
  # resolves a different version, whose browser build the server can't launch).
  ( cd "$HOME_DIR" &&
    as_user npx -y -p @executeautomation/playwright-mcp-server sh -c \
      'node "$(dirname "$(dirname "$(command -v playwright-mcp-server)")")/playwright/cli.js" install chromium' )
}

step "network"            wait_for_network
step "apt packages"       install_packages
step "user groups"        add_groups
step "clone blinky_time"  clone_repo
step "blinky-server venv" server_venv
step "blinky-console"     build_console
step "sudoers"            install_sudoers
step "bluetooth off"      disable_bluetooth
step "debug mate udev"    debug_mate_udev
step "user ssh key"       user_ssh_key
step "deploy worktree"    deploy_worktree
step "blinky-server unit" install_service
step "alloy (telemetry)"  install_alloy
step "host config"        host_config
step "user profile"       user_profile
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
log "  cat ~/.blinky-api-key              # this server's own API key"
log "  claude                             # sign in to Claude Code (once)"
[[ ${#FAILED[@]} -eq 0 ]]
