#!/usr/bin/env python3
"""
deploy_local.py - deploy firmware to ONE USB-attached nRF52840 with the same
safety gates as scripts/deploy.sh + blinky-server, for when the device is on
this machine's USB instead of blinkyhost's (e.g. a Windows workstation).

Pipeline (fails loud at every step, exit codes match deploy.sh):
  1. Pre-flight (read-only, over CDC):
     - `json info` identifies the device; --device-id must match device.id
       (explicit target, like deploy.sh's required --devices).
     - device must be configured (not safe mode) unless --allow-unconfigured.
     - uptime >= 5 min 15 s: the entry touch is a reset, and b190-era
       firmware counts every boot toward the 5-boot config quarantine until
       5 min of uptime clears it (stricter than CLAUDE.md's 60-second rule).
     - snapshot of `json info` + `device show` saved for rollback/forensics.
  2. Compile via scripts/build.sh (build-number bump + Version.h), unless
     --skip-compile.
  3. Validate: tools/uf2_upload.py's validate_hex (pre_upload_check address
     safety), convert_to_uf2, validate_uf2 (family ID, no bootloader/UICR
     blocks). Same functions the server's UF2 path runs.
  4. Refuse if any XIAO-SENSE drive is already mounted (ambiguous target).
  5. Enter the bootloader (1200-baud touch), wait for exactly ONE new
     XIAO-SENSE drive, and check it is THIS chip: the drive's USB serial must
     equal the app-mode USB serial, and INFO_UF2.TXT must report the expected
     bootloader (default 0.8.0-10, the fleet BL with USB on every DFU path).
     Any mismatch aborts BEFORE copying; the old app stays intact.
  6. Copy the UF2, wait for the drive to disappear (bootloader done) and the
     same-serial CDC port to come back.
  7. Post-deploy assertions (same as deploy.sh step 7): version == b<build>,
     same device.id and sn, configured, overrun counters present; fps < 30 or
     overruns > 5 are warnings.
  8. restore_runtime_settings + save (deploy.sh step 6), unless
     --keep-settings.
  9. Wait until uptime >= 75 s so markStable() fires, confirm no reboot
     happened meanwhile, then report "safe to reset".
 Every attempt appends one JSONL line to
 ~/.local/share/blinky/local_flash_attempts.jsonl.

Usage:
  python tools/deploy_local.py --port COM3 --device-id hat_v1
  python tools/deploy_local.py --port COM3 --device-id hat_v1 --dry-run
  python tools/deploy_local.py --port COM3 --device-id hat_v1 --skip-compile

Exit codes: 1 compile, 2 upload/flash, 4 post-deploy assertion, 6 bad args or
pre-flight refusal.
"""

import argparse
import datetime
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import serial
import serial.tools.list_ports

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "tools"))
import uf2_upload  # noqa: E402  (validate_hex / convert_to_uf2 / validate_uf2)
import firmware_release  # noqa: E402  (stable/candidate download, test-device list)

VID = 0x2886
APP_PID = 0x8045
BL_PID = 0x0045
DRIVE_LABEL = "XIAO-SENSE"
HEX = "/tmp/blinky-build/blinky-things.ino.hex"  # POSIX; cygwin_to_native() maps it on Windows
STABLE_UPTIME_MS = 75_000      # CLAUDE.md: wait >= 75 s so markStable() fires
# Pre-flight is stricter: firmware before e3296379 (b190 and older) counts
# EVERY boot toward the 5-boot quarantine and only clears the counter after
# 5 min of uptime (RebootFrequencyCounter::STABLE_UPTIME_MS). Our bootloader
# touch is a boot, so don't start one inside that window.
PREFLIGHT_UPTIME_MS = 5 * 60_000 + 15_000
LOOP_METRICS_SETTLE_S = 6      # deploy.sh: let the 5 s LoopMetrics window close
BL_ENTRY_TIMEOUT_S = 30
WRITE_TIMEOUT_S = 90           # copy + bootloader flash + drive removal
REENUM_TIMEOUT_S = 45
AUDIT_LOG = Path.home() / ".local/share/blinky/local_flash_attempts.jsonl"
IS_WINDOWS = platform.system() == "Windows"


class DeployError(Exception):
    def __init__(self, msg, code):
        super().__init__(msg)
        self.code = code


def log(msg=""):
    print(msg, flush=True)


def section(title):
    log("")
    log(f"=== {title} ===")


# ─── serial helpers ──────────────────────────────────────────────────

def port_info(port):
    for p in serial.tools.list_ports.comports():
        if p.device.lower() == port.lower():
            return p
    return None


def find_app_port(usb_serial):
    for p in serial.tools.list_ports.comports():
        if p.vid == VID and p.pid == APP_PID and (p.serial_number or "").upper() == usb_serial:
            return p.device
    return None


def send(port, cmd, wait_s=1.5, read_bytes=16000):
    """Send one console command at 115200 (no bootloader touch) and return text."""
    try:
        s = serial.Serial(port, 115200, timeout=0.5)
    except serial.SerialException as e:
        raise DeployError(f"cannot open {port}: {e} (is another tool holding it?)", 6)
    try:
        time.sleep(0.3)
        s.reset_input_buffer()
        s.write((cmd + "\n").encode())
        deadline = time.monotonic() + wait_s
        buf = b""
        while time.monotonic() < deadline and len(buf) < read_bytes:
            try:
                buf += s.read(4096)
            except serial.SerialException as e:
                # The device reset or dropped off USB mid-read. Callers that
                # expect that (reboot) get what arrived; everyone else fails
                # on the missing reply.
                log(f"  ({port} went away during `{cmd}`: {e})")
                break
        return buf.decode("utf-8", "replace")
    finally:
        try:
            s.close()
        except serial.SerialException:
            pass


def json_info(port, retries=3):
    last = ""
    for _ in range(retries):
        out = send(port, "json info")
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("{") and '"version"' in line:
                try:
                    return json.loads(line)
                except json.JSONDecodeError:
                    pass
        last = out
        time.sleep(1)
    raise DeployError(f"no parseable `json info` from {port}; last output:\n{last[:800]}", 2)


# ─── bootloader drive helpers ────────────────────────────────────────

def _ps(script):
    r = subprocess.run(["powershell", "-NoProfile", "-Command", script],
                       capture_output=True, text=True, timeout=20)
    return r.stdout


def list_uf2_drives():
    """Mounted XIAO-SENSE volumes as [(root_path, usb_serial_or_None)]."""
    if not IS_WINDOWS:
        raise DeployError("drive discovery is implemented for Windows only; on Linux "
                          "use scripts/deploy.sh (blinky-server) or tools/uf2_upload.py", 6)
    # Volume -> partition -> disk; the disk's Path embeds the USB serial
    # (\\?\usbstor#disk&ven_...#<SERIAL>&0#{...}).
    out = _ps(
        f"Get-Volume | Where-Object {{ $_.FileSystemLabel -eq '{DRIVE_LABEL}' -and $_.DriveLetter }} | "
        "ForEach-Object { $d = $_ | Get-Partition | Get-Disk; "
        "\"$($_.DriveLetter)|$($d.Path)|$($d.SerialNumber)\" }")
    drives = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        letter, path, sn = (line.split("|") + ["", ""])[:3]
        # Get-Disk's SerialNumber is the USB iSerial (the chip sn). Fall back to
        # the disk path, whose instance segment is e.g.
        # "8&16d7b5ad&0&d63bd31605b36a15&0": take the 16-hex-digit token, never
        # a positional guess (the first token is a hub-instance prefix).
        serial_no = sn.strip().upper() or None
        if not serial_no:
            parts = path.split("#")
            if len(parts) >= 3:
                toks = [t for t in parts[2].split("&")
                        if len(t) == 16 and all(c in "0123456789abcdefABCDEF" for c in t)]
                if len(toks) == 1:
                    serial_no = toks[0].upper()
        drives.append((f"{letter}:\\", serial_no))
    return drives


def read_info_uf2(root):
    try:
        return (Path(root) / "INFO_UF2.TXT").read_text(errors="replace")
    except OSError as e:
        raise DeployError(f"cannot read INFO_UF2.TXT on {root}: {e}", 2)


def bootloader_version(info_text):
    for line in info_text.splitlines():
        if line.lower().startswith("uf2 bootloader"):
            return line.split()[2] if len(line.split()) > 2 else line
    return None


def touch_1200(port):
    try:
        s = serial.Serial(port, 1200, timeout=1)
        time.sleep(0.1)
        s.dtr = False
        time.sleep(0.05)
        s.close()
    except (serial.SerialException, OSError):
        pass  # the port vanishing mid-touch is the expected success path


# ─── audit ───────────────────────────────────────────────────────────

def audit(entry):
    AUDIT_LOG.parent.mkdir(parents=True, exist_ok=True)
    entry["ts"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    entry["host"] = platform.node()
    with AUDIT_LOG.open("a", encoding="utf-8") as f:
        f.write(json.dumps(entry) + "\n")


# ─── pipeline ────────────────────────────────────────────────────────

def preflight(args):
    section(f"Pre-flight: {args.port}")
    p = port_info(args.port)
    if p is None:
        raise DeployError(f"{args.port} not present", 6)
    if p.vid != VID or p.pid != APP_PID:
        raise DeployError(f"{args.port} is VID:PID {p.vid:04X}:{p.pid:04X}, not an nRF52840 "
                          f"XIAO in app mode ({VID:04X}:{APP_PID:04X})", 6)
    usb_serial = (p.serial_number or "").upper()
    if not usb_serial:
        raise DeployError(f"{args.port} reports no USB serial; can't pin the bootloader drive to it", 6)

    info = json_info(args.port)
    dev = info.get("device") or {}
    log(f"  device.id={dev.get('id')} name={dev.get('name')} version={info.get('version')} "
        f"sn={info.get('sn')} usb_serial={usb_serial} uptime={info.get('millis', 0) / 1000:.0f}s")
    if dev.get("id") != args.device_id:
        raise DeployError(f"device.id is {dev.get('id')!r}, expected {args.device_id!r} - refusing "
                          f"(explicit target required)", 6)
    if (info.get("sn") or "").upper() != usb_serial:
        raise DeployError(f"json sn {info.get('sn')} != USB serial {usb_serial}", 6)
    if not dev.get("configured") and not args.allow_unconfigured:
        raise DeployError("device is unconfigured/safe mode - investigate first (CLAUDE.md "
                          "recovery order), or pass --allow-unconfigured", 6)
    uptime = int(info.get("millis", 0))
    if uptime < PREFLIGHT_UPTIME_MS:
        wait = (PREFLIGHT_UPTIME_MS - uptime) / 1000 + 1
        log(f"  uptime {uptime / 1000:.0f}s < {PREFLIGHT_UPTIME_MS // 1000}s (reboot-counter stable "
            f"window) - waiting {wait:.0f}s")
        time.sleep(wait)
        info = json_info(args.port)
        if int(info.get("millis", 0)) < PREFLIGHT_UPTIME_MS:
            raise DeployError("device rebooted during the pre-flight wait", 6)

    snap_dir = Path(args.snapshot_dir)
    snap_dir.mkdir(parents=True, exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    snap = snap_dir / f"{args.device_id}-{info.get('version')}-{stamp}.txt"
    show = send(args.port, "device show", wait_s=2.0)
    snap.write_text(json.dumps(info, indent=1) + "\n\n# device show\n" + show, encoding="utf-8")
    log(f"  snapshot: {snap}")
    log("  [PASS] pre-flight")
    return info, usb_serial


def select_firmware(args):
    """Pick the firmware to flash (docs/RELEASE_PROCESS.md).

    default       latest STABLE release, downloaded and sha256-verified
    --release bN  that release; a candidate (pre-release) only for a test device
    --dev         local build of a clean tree, test devices only, version b<N>-dev-<sha>
    Returns (hex_path, expected_version, exact, channel, tag).
    """
    if args.dev:
        firmware_release.require_test_device(args.device_id, "--dev build")
        dirty = subprocess.run(["git", "status", "--porcelain", "--", "blinky-things", "scripts"],
                               cwd=REPO, capture_output=True, text=True).stdout.strip()
        # Version.h is regenerated by every build; anything else means the
        # source doesn't match a commit.
        dirty = [l for l in dirty.splitlines() if "types/Version.h" not in l]
        if dirty:
            raise DeployError("--dev needs a clean tree (commit or stash first): " + "; ".join(dirty), 6)
        if args.skip_compile:
            if not Path(cygwin_to_native(HEX)).exists():
                raise DeployError(f"no hex at {HEX} (run without --skip-compile)", 1)
        else:
            section("Compile (scripts/build.sh --dev)")
            if subprocess.run(["bash", "scripts/build.sh", "--dev"], cwd=REPO).returncode != 0:
                raise DeployError("compilation failed", 1)
        build = (REPO / "blinky-things/BUILD_NUMBER").read_text().strip()
        sha = subprocess.run(["git", "rev-parse", "--short=8", "HEAD"], cwd=REPO,
                             capture_output=True, text=True).stdout.strip()
        return Path(cygwin_to_native(HEX)), f"b{build}-dev-{sha}", True, "dev", None

    if args.skip_compile:
        raise DeployError("--skip-compile only applies to --dev (releases are downloaded, never compiled)", 6)
    section("Firmware release")
    try:
        tag = args.release or firmware_release.latest_stable_tag()
        rel = firmware_release.download(tag, Path(tempfile.gettempdir()) / "blinky-release")
        if not rel["stable"]:
            firmware_release.require_test_device(args.device_id, f"candidate {tag}")
    except firmware_release.ReleaseError as e:
        raise DeployError(str(e), 6)
    channel = "stable" if rel["stable"] else "candidate"
    log(f"  {tag} ({channel}) {rel['manifest']['version']}, sha256 verified")
    return rel["hex"], rel["manifest"]["version"], True, channel, tag


def cygwin_to_native(p):
    if IS_WINDOWS and shutil.which("cygpath"):
        return subprocess.run(["cygpath", "-w", str(p)], capture_output=True, text=True).stdout.strip()
    return str(p)


def validate(args, hex_path):
    if not hex_path.exists():
        raise DeployError(f"hex not found: {hex_path}", 1)
    if not uf2_upload.validate_hex(hex_path, verbose=args.verbose):
        raise DeployError("hex failed safety validation - upload blocked", 2)
    uf2_path = uf2_upload.convert_to_uf2(hex_path)
    if not uf2_upload.validate_uf2(uf2_path, verbose=args.verbose):
        raise DeployError("UF2 failed safety validation - upload blocked", 2)
    return uf2_path


def enter_bootloader(args, usb_serial):
    section("Bootloader entry")
    pre = list_uf2_drives()
    if args.already_in_bootloader:
        # Resuming a run that stopped at the drive check (device parked in the
        # bootloader, old app intact). The same identity checks apply below.
        if len(pre) != 1:
            raise DeployError(f"--already-in-bootloader needs exactly one {DRIVE_LABEL} drive, "
                              f"found {pre}", 2)
    elif pre:
        raise DeployError(f"{DRIVE_LABEL} drive(s) already mounted {pre} - ambiguous target; "
                          f"eject/resolve first (or --already-in-bootloader --usb-serial ...)", 2)
    else:
        touch_1200(args.port)
    deadline = time.monotonic() + BL_ENTRY_TIMEOUT_S
    while time.monotonic() < deadline:
        drives = list_uf2_drives()
        if len(drives) > 1:
            raise DeployError(f"multiple {DRIVE_LABEL} drives appeared {drives} - refusing", 2)
        if drives:
            root, drive_serial = drives[0]
            time.sleep(1.0)  # let the FAT finish mounting
            info_txt = read_info_uf2(root)
            bl = bootloader_version(info_txt)
            log(f"  drive {root} usb_serial={drive_serial} bootloader={bl}")
            if drive_serial != usb_serial:
                raise DeployError(f"bootloader drive serial {drive_serial} != app serial {usb_serial} - "
                                  f"NOT copying. Device is parked in the bootloader with its old app "
                                  f"intact. Resolve, then resume with --already-in-bootloader --usb-serial <sn> --skip-compile, or press reset once to return to the old app.", 2)
            if not (bl or "").startswith(args.expect_bl):
                raise DeployError(f"bootloader {bl!r} does not match --expect-bl {args.expect_bl!r} - NOT "
                                  f"copying (old app intact; resume with --already-in-bootloader --expect-bl ..., or press reset once). Pass --expect-bl to "
                                  f"override after reviewing the recovery implications.", 2)
            log("  [PASS] drive is this chip, bootloader as expected")
            return root, bl
        time.sleep(0.5)
    raise DeployError(f"no {DRIVE_LABEL} drive within {BL_ENTRY_TIMEOUT_S}s of the 1200-baud touch "
                      f"(app still running? check {args.port})", 2)


def copy_and_wait(root, uf2_path, usb_serial):
    section("Write")
    dst = Path(root) / "blinky-things.uf2"
    size = uf2_path.stat().st_size
    log(f"  copying {size:,} bytes -> {dst}")
    try:
        with open(uf2_path, "rb") as src, open(dst, "wb") as out:
            shutil.copyfileobj(src, out, 64 * 1024)
            out.flush()
            os.fsync(out.fileno())
    except OSError as e:
        # The bootloader resets as soon as the last block lands, which can
        # tear the handle down mid-close. Only trust it if the drive vanishes.
        log(f"  (copy raised {e!r}; checking whether the bootloader took it)")
    deadline = time.monotonic() + WRITE_TIMEOUT_S
    while time.monotonic() < deadline and list_uf2_drives():
        time.sleep(0.5)
    if list_uf2_drives():
        raise DeployError(f"{DRIVE_LABEL} still mounted {WRITE_TIMEOUT_S}s after copy - write did not "
                          f"complete. Bootloader self-recovery should boot the old app; if not, re-run.", 2)
    log("  [PASS] bootloader accepted the image (drive gone)")
    deadline = time.monotonic() + REENUM_TIMEOUT_S
    while time.monotonic() < deadline:
        port = find_app_port(usb_serial)
        if port:
            log(f"  [PASS] app re-enumerated on {port}")
            return port
        time.sleep(0.5)
    raise DeployError(f"app CDC (serial {usb_serial}) did not come back within {REENUM_TIMEOUT_S}s", 2)


def verify(port, args, expected_version, before):
    section("Post-deploy assertions")
    time.sleep(LOOP_METRICS_SETTLE_S)
    info = json_info(port)
    dev = info.get("device") or {}
    version = info.get("version", "?")
    fails, warns = [], []
    if version != expected_version:
        fails.append(f"version={version} expected={expected_version}")
    if dev.get("id") != args.device_id:
        fails.append(f"device.id={dev.get('id')} (identity lost?)")
    if (info.get("sn") or "").upper() != (before.get("sn") or "").upper():
        fails.append(f"sn={info.get('sn')} != {before.get('sn')}")
    if before.get("_resumed"):
        log("  (resumed from bootloader: no pre-flight snapshot, so 'configured' is checked absolutely)")
        if not dev.get("configured"):
            fails.append("device came up unconfigured")
    if not dev.get("configured") and before.get("device", {}).get("configured"):
        fails.append("device came up unconfigured (config lost / quarantine)")
    if "audioOverruns" not in info or "audioSamplesLost" not in info:
        fails.append("missing audio overrun counters")
    # `ble` is the NUS address, printed only when Bluefruit.begin() succeeded.
    # Empty means the SoftDevice is half-up: every InternalFS write then waits
    # forever on the SOC task begin() never started, so the first settings
    # save (or the 5-min counter write) hangs the loop into the watchdog. This
    # check runs BEFORE restore_settings' `save` for exactly that reason.
    if not info.get("ble"):
        raise DeployError(
            "BLE did not start (json info `ble` empty; boot log would show "
            "[FALLBACK] Bluefruit.begin() FAILED). Flash writes will hang. Do NOT send `set`/`save`; "
            f"within 5 min of boot send `bootloader` on {port} and drop a known-good UF2 "
            "(e.g. via --already-in-bootloader --skip-compile with a b190 build).", 4)
    fps = float(info.get("fps", 0.0))
    if fps == 0.0:
        warns.append("fps=0.0 (window unclosed)")
    elif fps < 30:
        warns.append(f"fps={fps:.1f}<30")
    if int(info.get("audioOverruns", 0)) > 5:
        warns.append(f"overruns={info.get('audioOverruns')}")
    log(f"  version={version} fps={fps:.1f} overruns={info.get('audioOverruns')} "
        f"uptime={info.get('millis', 0) / 1000:.0f}s")
    for w in warns:
        log(f"  [WARN] {w}")
    if fails:
        raise DeployError("post-deploy assertion failed: " + "; ".join(fails), 4)
    log("  [PASS] post-deploy state")
    return info


def restore_settings(port):
    section("Restoring runtime settings (deploy.sh step 6)")
    for cmd in ("restore_runtime_settings", "save"):
        out = send(port, cmd, wait_s=2.0)
        lines = {l.strip() for l in out.splitlines()}
        if "OK" not in lines or "ERROR" in lines:
            raise DeployError(f"`{cmd}` not acknowledged: {out.strip()[:300]!r}", 4)
        log(f"  {cmd}: OK")


def wait_stable(port, usb_serial):
    section("Waiting for markStable (75 s uptime)")
    info = json_info(port)
    up = int(info.get("millis", 0))
    if up < STABLE_UPTIME_MS:
        wait = (STABLE_UPTIME_MS - up) / 1000 + 1
        log(f"  uptime {up / 1000:.0f}s - waiting {wait:.0f}s; do NOT reset the device meanwhile")
        time.sleep(wait)
    port = find_app_port(usb_serial) or port
    info2 = json_info(port)
    if int(info2.get("millis", 0)) < max(up, STABLE_UPTIME_MS - 2000):
        raise DeployError(f"device rebooted during the stability window (uptime now "
                          f"{info2.get('millis')} ms) - investigate before any further reset", 4)
    log(f"  [PASS] stable at {info2.get('millis', 0) / 1000:.0f}s uptime - safe to reset again")
    return info2


def main():
    # Firmware replies contain non-cp1252 glyphs (e.g. a check mark); a
    # Windows console must not turn an error message into a crash.
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(errors="replace")
        except AttributeError:
            pass
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="app-mode CDC port, e.g. COM3")
    ap.add_argument("--device-id", required=True, help="expected device.id (e.g. hat_v1); must match")
    ap.add_argument("--expect-bl", default="0.8.0-10", help="required bootloader version prefix")
    ap.add_argument("--release", metavar="bN",
                    help="flash this release instead of the latest stable (a candidate needs a test device)")
    ap.add_argument("--dev", action="store_true",
                    help="flash a local build of a clean tree (test devices only; version b<N>-dev-<sha>)")
    ap.add_argument("--skip-compile", action="store_true",
                    help="with --dev: reuse the last dev build (e.g. to resume from the bootloader)")
    ap.add_argument("--keep-settings", action="store_true",
                    help="skip restore_runtime_settings + save (deploy.sh always does both)")
    ap.add_argument("--allow-unconfigured", action="store_true")
    ap.add_argument("--dry-run", action="store_true",
                    help="pre-flight + compile + validate only; no bootloader entry, no write")
    ap.add_argument("--already-in-bootloader", action="store_true",
                    help="resume a run parked at the drive check; needs --usb-serial and --skip-compile")
    ap.add_argument("--usb-serial", help="expected USB serial (chip sn) for --already-in-bootloader")
    ap.add_argument("--snapshot-dir", default=str(Path.home() / ".local/share/blinky/snapshots"))
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    entry = {"device_id": args.device_id, "port": args.port, "source": "deploy_local",
             "git": subprocess.run(["git", "rev-parse", "--short=12", "HEAD"], cwd=REPO,
                                   capture_output=True, text=True).stdout.strip()}
    try:
        if args.already_in_bootloader:
            if not args.usb_serial or args.dry_run:
                raise DeployError("--already-in-bootloader requires --usb-serial (and no --dry-run)", 6)
            usb_serial = args.usb_serial.upper()
            before = {"sn": usb_serial, "version": None, "_resumed": True}
        else:
            before, usb_serial = preflight(args)
        entry.update(from_version=before.get("version"), sn=before.get("sn"))
        if args.dev and args.release:
            raise DeployError("--dev and --release are mutually exclusive", 6)
        hex_path, expected_version, _, channel, tag = select_firmware(args)
        entry.update(channel=channel, release=tag, build=expected_version)
        uf2_path = validate(args, hex_path)
        if args.dry_run:
            log("\nDRY RUN complete: pre-flight, compile and validation passed; nothing written.")
            return 0
        root, bl = enter_bootloader(args, usb_serial)
        entry["bootloader"] = bl
        port = copy_and_wait(root, uf2_path, usb_serial)
        verify(port, args, expected_version, before)
        if not args.keep_settings:
            restore_settings(port)
        final = wait_stable(port, usb_serial)
        entry.update(state="COMPLETED", to_version=final.get("version"))
        audit(entry)
        log(f"\nDEPLOY OK: {args.device_id} {before.get('version')} -> {final.get('version')} on {port}")
        return 0
    except DeployError as e:
        entry.update(state="FAILED", error=str(e), code=e.code)
        audit(entry)
        log(f"\nDEPLOY FAILED: {e}")
        return e.code
    except KeyboardInterrupt:
        entry.update(state="ABANDONED")
        audit(entry)
        log("\nDEPLOY ABANDONED (Ctrl-C). If a write was in progress, do NOT reset - let the "
            "bootloader finish or self-recover, then check the device.")
        return 2


if __name__ == "__main__":
    sys.exit(main())
