"""UF2 firmware upload for nRF52840 devices.

Delegates to tools/uf2_upload.py (the battle-tested standalone tool) rather
than reimplementing upload logic. The server handles transport release and
reconnection; the tool handles bootloader entry, UF2 detection, firmware
copy, and verification.
"""
from __future__ import annotations

import asyncio
import logging
import os
import time
from pathlib import Path

log = logging.getLogger(__name__)


def _find_uf2_tool() -> Path | None:
    """Find uf2_upload.py in the project."""
    # Walk up from this file to find tools/
    p = Path(__file__).resolve()
    for _ in range(10):
        p = p.parent
        candidate = p / "tools" / "uf2_upload.py"
        if candidate.exists():
            return candidate
    return None


async def upload_uf2(
    serial_port: str,
    firmware_path: str,
    transport,
    protocol=None,
    progress_callback: callable | None = None,
) -> dict:
    """Upload firmware to an nRF52840 device via UF2.

    Disconnects the server's transport, runs uf2_upload.py as a subprocess
    (which handles bootloader entry with retries, UF2 detection, firmware
    copy, and verification), then lets the fleet manager reconnect.

    Args:
        serial_port: The serial port path (e.g., /dev/ttyACM3)
        firmware_path: Path to .hex or .uf2 firmware file
        transport: The serial Transport instance (disconnected before upload)
        protocol: Unused (kept for API compatibility)
        progress_callback: Optional callable(phase, msg, pct)

    Returns:
        dict with status, message, elapsed_s
    """
    t0 = time.monotonic()
    result = {"status": "error", "message": "", "elapsed_s": 0}

    def progress(phase, msg, pct=None):
        log.info("[OTA %s] %s", phase, msg)
        if progress_callback:
            progress_callback(phase, msg, pct)

    # Find the upload tool
    tool = _find_uf2_tool()
    if not tool:
        result["message"] = "uf2_upload.py not found in tools/"
        return result

    # Validate firmware file
    if not os.path.isfile(firmware_path):
        result["message"] = f"Firmware file not found: {firmware_path}"
        return result

    # Send bootloader command via server's transport (port is already open,
    # TinyUSB CDC is in good state). Then disconnect and let the tool handle
    # UF2 detection + firmware copy. This avoids the DTR-drop-before-tool-opens
    # issue that corrupts TinyUSB state.
    progress("prepare", "Sending bootloader command...", 10)
    try:
        await transport.write_line("bootloader")
        await asyncio.sleep(3)  # Wait for device to process + reset
    except Exception as e:
        log.debug("Bootloader command (device may have reset): %s", e)

    try:
        await transport.disconnect()
    except Exception:
        pass
    await asyncio.sleep(2)

    # Run uf2_upload.py with --already-in-bootloader since we already
    # sent the command. The tool handles UF2 detection, mount, copy.
    progress("upload", f"Running uf2_upload.py on {serial_port}...", 20)
    cmd = [
        "python3", str(tool),
        "--hex", firmware_path,
        "--already-in-bootloader",
        "-v",
    ]

    try:
        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.STDOUT,
            cwd=str(tool.parent),  # Run from tools/ dir so serial_lock.py is found
        )
        stdout, _ = await asyncio.wait_for(proc.communicate(), timeout=120)
        output = stdout.decode("utf-8", errors="replace")

        if proc.returncode == 0:
            progress("done", "Upload complete!", 100)
            elapsed = time.monotonic() - t0
            result.update(
                status="ok",
                message="Firmware uploaded successfully",
                elapsed_s=round(elapsed, 1),
                output=output[-500:],  # Last 500 chars of tool output
            )
        else:
            # Extract error message from tool output
            lines = output.strip().split("\n")
            error_lines = [l for l in lines if "ERROR" in l or "FAILED" in l]
            msg = error_lines[-1] if error_lines else lines[-1] if lines else "Unknown error"
            result["message"] = msg.strip()
            result["output"] = output[-500:]

    except asyncio.TimeoutError:
        result["message"] = "Upload timed out after 120s"
    except Exception as e:
        result["message"] = f"Upload failed: {e}"

    result["elapsed_s"] = round(time.monotonic() - t0, 1)
    return result
