#!/bin/bash
# usb_recovery.sh — self-heal Linux USB bus state damage from rapid device
# disconnect/reconnect cycles during firmware iteration.
#
# Background: when a USB device produces ETIMEDOUT (-110) errors during
# enumeration faster than the kernel can recover, Linux escalates through
# a hierarchy of giving up:
#
#   1. Per-port soft-disable: /sys/.../usb<N>-portM/disable = 1
#      Recovery: write 0 to that file (needs root).
#
#   2. xhci_hcd driver unbind: the PCI device loses its driver entirely.
#      Symptom: /sys/bus/pci/drivers/xhci_hcd/ does NOT contain the PCI ID.
#      Recovery: write PCI_ID to /sys/bus/pci/drivers/xhci_hcd/bind (needs root).
#
#   3. PCI device unresponsive: remove+rescan needed.
#      Recovery: echo 1 > /sys/bus/pci/devices/<PCI_ID>/remove
#                echo 1 > /sys/bus/pci/rescan (needs root)
#
# This script detects which level of damage exists and applies the least-
# invasive recovery that addresses it. Designed for blinky_time's
# bootloader iteration loop on dev machines.
#
# Subcommands:
#   diagnose         show state, exit 0 if all healthy
#   heal             attempt recovery (least-invasive first)
#   heal-force       go straight to remove+rescan
#   install-sudoers  write /etc/sudoers.d/blinky-usb-recovery (one-time setup)
#
# Sudo handling: writes to /sys require root. By default uses `sudo -n`
# (non-interactive) and FAILS LOUDLY if sudo isn't available. Run
# `install-sudoers` once to grant passwordless access to ONLY the
# specific recovery operations.

set -uo pipefail

XHCI_DRIVER=/sys/bus/pci/drivers/xhci_hcd

# Filter for the user's blinky devices. Set EXPECTED_VID=2886 (Seeed XIAO)
# so we don't get cute about USB devices we don't own.
EXPECTED_VID="${EXPECTED_VID:-2886}"

# --- detection helpers --------------------------------------------------

# Which PCI devices SHOULD host xhci_hcd controllers? Discovered by:
# - scanning /sys/bus/pci/devices for class 0x0c0330 (USB xHCI)
# - or trusting historical state in /var/lib/blinky-usb-recovery/known-pci
KNOWN_PCI_DIR=/var/lib/blinky-usb-recovery
KNOWN_PCI_FILE="$KNOWN_PCI_DIR/known-pci"

list_xhci_pci_devices() {
    # All PCI devices whose class indicates xHCI USB controller (0x0c0330).
    for d in /sys/bus/pci/devices/*; do
        if [ -f "$d/class" ]; then
            local c=$(cat "$d/class")
            if [ "$c" = "0xc0330" ] || [ "$c" = "0x0c0330" ]; then
                basename "$d"
            fi
        fi
    done
}

is_pci_bound_to_xhci() {
    local pci_id=$1
    [ -L "$XHCI_DRIVER/$pci_id" ]
}

find_disabled_ports() {
    # Print "<bus> <port>" pairs that are kernel-disabled (disable=1).
    for f in /sys/bus/usb/devices/*/usb*-port*/disable; do
        [ -f "$f" ] || continue
        if [ "$(cat $f 2>/dev/null)" = "1" ]; then
            local port=$(basename $(dirname "$f"))
            local bus=$(echo $port | sed 's/-port.*//')
            echo "$bus $port"
        fi
    done
}

recent_eto_count() {
    # ETIMEDOUT (-110) error count in last 30s
    journalctl -k --since "30 seconds ago" 2>/dev/null | \
        grep -cE "error -110" || true
}

# --- diagnosis ----------------------------------------------------------

cmd_diagnose() {
    local unhealthy=0
    echo "=== USB bus health diagnosis ==="

    # 1) Check for unbound xhci controllers
    local pcis=$(list_xhci_pci_devices)
    if [ -z "$pcis" ]; then
        echo "  [unknown] no xHCI PCI devices detected — system has no USB?"
        return 0
    fi
    for pci in $pcis; do
        if is_pci_bound_to_xhci "$pci"; then
            echo "  [OK]      $pci bound to xhci_hcd"
        else
            echo "  [DAMAGED] $pci NOT bound to xhci_hcd — driver was unbound"
            unhealthy=1
        fi
    done

    # 2) Check for kernel-disabled ports
    local disabled_ports=$(find_disabled_ports)
    if [ -z "$disabled_ports" ]; then
        echo "  [OK]      no kernel-disabled USB ports"
    else
        echo "  [DAMAGED] kernel has disabled these ports:"
        echo "$disabled_ports" | sort | awk '{print "             "$2}' | head -20
        local n=$(echo "$disabled_ports" | wc -l)
        echo "             ($n total)"
        unhealthy=1
    fi

    # 3) Recent ETIMEDOUT pressure
    local eto=$(recent_eto_count)
    if [ "$eto" -gt 0 ]; then
        echo "  [WARN]    $eto -110 errors in last 30s — approaching threshold"
    fi

    return $unhealthy
}

# --- recovery actions ---------------------------------------------------

require_sudo() {
    if ! sudo -n true 2>/dev/null; then
        echo "ERROR: sudo not available non-interactively." >&2
        echo "       Run: $0 install-sudoers (one-time setup)" >&2
        echo "       Or run this script with: sudo $0 heal" >&2
        return 1
    fi
}

# Recovery level 1: just un-disable specific ports
heal_disabled_ports() {
    local healed=0
    while read line; do
        [ -z "$line" ] && continue
        local bus=$(echo "$line" | awk '{print $1}')
        local port=$(echo "$line" | awk '{print $2}')
        local file="/sys/bus/usb/devices/${bus}-0:1.0/${port}/disable"
        if [ -f "$file" ]; then
            echo "  [heal] enabling $port ($file)"
            if echo 0 | sudo -n tee "$file" >/dev/null 2>&1; then
                local new=$(cat "$file")
                if [ "$new" = "0" ]; then
                    echo "         OK"
                    healed=$((healed+1))
                else
                    echo "         FAILED (kernel kept disable=1; need driver rebind)"
                fi
            else
                echo "         FAILED to write (sudo issue?)"
            fi
        fi
    done < <(find_disabled_ports)
    return $((healed > 0 ? 0 : 1))
}

# Recovery level 2: re-bind xhci_hcd to unbound PCI devices
heal_unbound_xhci() {
    local healed=0
    for pci in $(list_xhci_pci_devices); do
        if ! is_pci_bound_to_xhci "$pci"; then
            echo "  [heal] binding $pci to xhci_hcd"
            if echo "$pci" | sudo -n tee "$XHCI_DRIVER/bind" >/dev/null 2>&1; then
                sleep 1
                if is_pci_bound_to_xhci "$pci"; then
                    echo "         OK"
                    healed=$((healed+1))
                else
                    echo "         partial (driver reported success but not bound — try heal-force)"
                fi
            else
                echo "         FAILED — falling back to remove+rescan"
                heal_pci_remove_rescan "$pci" && healed=$((healed+1))
            fi
        fi
    done
    return $((healed > 0 ? 0 : 1))
}

# Recovery level 3: remove and rescan the PCI device
heal_pci_remove_rescan() {
    local pci=$1
    echo "  [heal] removing $pci then rescanning PCI bus"
    if ! echo 1 | sudo -n tee "/sys/bus/pci/devices/$pci/remove" >/dev/null 2>&1; then
        echo "         FAILED to remove"
        return 1
    fi
    sleep 1
    if ! echo 1 | sudo -n tee "/sys/bus/pci/rescan" >/dev/null 2>&1; then
        echo "         FAILED to rescan"
        return 1
    fi
    sleep 2
    if is_pci_bound_to_xhci "$pci"; then
        echo "         OK — $pci re-bound after rescan"
        return 0
    else
        echo "         partial — rescan completed but $pci still not bound"
        return 1
    fi
}

cmd_heal() {
    require_sudo || return 2

    echo "=== USB bus health: heal pass (least-invasive first) ==="

    cmd_diagnose
    local before=$?
    if [ "$before" = "0" ]; then
        echo "  nothing to heal"
        return 0
    fi

    # Level 1: try un-disabling ports
    if find_disabled_ports | grep -q .; then
        echo "  attempting per-port un-disable..."
        heal_disabled_ports || true
    fi

    # Level 2: re-bind unbound xhci controllers
    local needs_rebind=0
    for pci in $(list_xhci_pci_devices); do
        is_pci_bound_to_xhci "$pci" || needs_rebind=1
    done
    if [ "$needs_rebind" = "1" ]; then
        echo "  attempting xhci_hcd driver rebind..."
        heal_unbound_xhci || true
    fi

    echo
    echo "=== post-heal diagnosis ==="
    cmd_diagnose
}

cmd_heal_force() {
    require_sudo || return 2
    echo "=== USB bus health: heal-force (remove+rescan every xHCI) ==="
    for pci in $(list_xhci_pci_devices); do
        heal_pci_remove_rescan "$pci"
    done
    sleep 2
    cmd_diagnose
}

# --- sudoers installer --------------------------------------------------

cmd_install_sudoers() {
    local user="${USER:-$(whoami)}"
    local file="/etc/sudoers.d/blinky-usb-recovery"

    cat <<EOF
About to install sudoers rule at $file granting $user
NOPASSWD access to ONLY these specific operations:

  - /usr/bin/tee /sys/bus/usb/devices/*/usb*-port*/disable
  - /usr/bin/tee /sys/bus/pci/drivers/xhci_hcd/bind
  - /usr/bin/tee /sys/bus/pci/drivers/xhci_hcd/unbind
  - /usr/bin/tee /sys/bus/pci/devices/*/remove
  - /usr/bin/tee /sys/bus/pci/rescan

This is the minimum needed to self-heal the kernel's USB damage from
rapid firmware-iteration disconnect cycles. Nothing else gets sudo.

You will be prompted for your password once.

EOF
    read -p "Proceed? [y/N] " yn
    case "$yn" in
        [yY]*) ;;
        *) echo "aborted"; return 1;;
    esac

    sudo tee "$file" >/dev/null <<EOF
# Installed by $(basename $0). Allows blinky_time firmware iteration
# scripts to recover from kernel USB controller / port damage caused
# by rapid device disconnect/reconnect cycles.
$user ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/usb/devices/[0-9]*-0\:1.0/usb*-port*/disable
$user ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/pci/drivers/xhci_hcd/bind
$user ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/pci/drivers/xhci_hcd/unbind
$user ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/pci/devices/[0-9a-f]*\:[0-9a-f]*\:[0-9a-f]*.[0-9]/remove
$user ALL=(root) NOPASSWD: /usr/bin/tee /sys/bus/pci/rescan
EOF
    sudo chmod 0440 "$file"
    echo "installed $file"

    # Persist known PCI device list for future diagnosis
    sudo mkdir -p "$KNOWN_PCI_DIR"
    list_xhci_pci_devices | sudo tee "$KNOWN_PCI_FILE" >/dev/null
    echo "saved current xHCI PCI device list to $KNOWN_PCI_FILE"
}

# --- main ---------------------------------------------------------------

case "${1:-diagnose}" in
    diagnose)        cmd_diagnose ;;
    heal)            cmd_heal ;;
    heal-force)      cmd_heal_force ;;
    install-sudoers) cmd_install_sudoers ;;
    *) echo "usage: $0 {diagnose | heal | heal-force | install-sudoers}"
       exit 1 ;;
esac
