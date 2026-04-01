"""QSPI-staged OTA firmware transfer.

Transfers firmware to a device's external QSPI flash via serial or BLE NUS
commands, then triggers the bootloader to apply it. The device's current
firmware stays intact until the complete image is validated in QSPI.

Protocol (text commands over existing serial/BLE NUS transport):
  ota begin <size> <crc16_hex>  -> erase QSPI staging, prepare for chunks
  ota chunk <offset> <base64>   -> write decoded data to QSPI
  ota status                    -> report staged bytes
  ota commit                    -> validate CRC, apply via bootloader
  ota abort                     -> clear staging
"""

from __future__ import annotations

import asyncio
import base64
import logging
import subprocess
import tempfile
import time
from pathlib import Path
from typing import Any

from .compile import _crc16

log = logging.getLogger(__name__)

CHUNK_SIZE = 128  # Raw bytes per chunk (base64 encoded ~172 chars, fits BLE NUS)


async def upload_qspi_ota(
    protocol: Any,
    firmware_path: str,
    progress_callback: Any | None = None,
) -> dict[str, Any]:
    """Transfer firmware to device QSPI flash and trigger bootloader apply.

    Uses the device's existing serial/BLE NUS command interface -- no special
    DFU protocol needed. The device stays running its current firmware
    throughout the transfer. Only after commit does it reset.

    Args:
        protocol: DeviceProtocol instance (send_command)
        firmware_path: Path to .bin firmware file
        progress_callback: Optional callable(phase, msg, pct)

    Returns:
        dict with status, message, elapsed_s
    """
    t0 = time.monotonic()

    def progress(phase: str, msg: str, pct: int = 0) -> None:
        log.info("[QSPI-OTA %s] %s", phase, msg)
        if progress_callback:
            progress_callback(phase, msg, pct)

    # Load firmware binary
    fw_path = Path(firmware_path)
    if not fw_path.is_file():
        return {
            "status": "error",
            "message": f"Firmware not found: {firmware_path}",
            "elapsed_s": 0,
        }

    # Accept .bin directly, or extract from .hex
    if fw_path.suffix == ".hex":
        from .compile import _find_objcopy

        objcopy = _find_objcopy()
        if not objcopy:
            return {"status": "error", "message": "objcopy not found for hex->bin", "elapsed_s": 0}
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as tmp:
            bin_path = tmp.name
        try:
            subprocess.run(
                [objcopy, "-I", "ihex", "-O", "binary", str(fw_path), bin_path],
                capture_output=True,
                timeout=30,
            )
            firmware = Path(bin_path).read_bytes()
        finally:
            Path(bin_path).unlink(missing_ok=True)
    else:
        firmware = fw_path.read_bytes()

    if len(firmware) == 0:
        return {"status": "error", "message": "Firmware is empty", "elapsed_s": 0}

    crc = _crc16(firmware)
    progress("init", f"{len(firmware)} bytes, CRC=0x{crc:04X}", 5)

    # Step 1: Begin -- erase QSPI staging area
    progress("begin", "Erasing QSPI staging area...", 10)
    try:
        resp = await protocol.send_command(
            f"ota begin {len(firmware)} {crc:04X}",
            timeout=30.0,  # Erase 512KB ~6.5s, give headroom
        )
    except Exception as e:
        return {"status": "error", "message": f"ota begin failed: {e}", "elapsed_s": 0}

    if "ERR" in resp:
        return {"status": "error", "message": f"ota begin rejected: {resp}", "elapsed_s": 0}
    progress("begin", "QSPI ready", 15)

    # Step 2: Transfer chunks
    total = len(firmware)
    sent = 0
    chunk_errors = 0
    max_chunk_errors = 5

    while sent < total:
        end = min(sent + CHUNK_SIZE, total)
        chunk = firmware[sent:end]
        b64 = base64.b64encode(chunk).decode("ascii")

        try:
            resp = await protocol.send_command(
                f"ota chunk {sent} {b64}",
                timeout=5.0,
            )
        except Exception as e:
            chunk_errors += 1
            if chunk_errors > max_chunk_errors:
                return {
                    "status": "error",
                    "message": f"Too many chunk errors at offset {sent}: {e}",
                    "elapsed_s": round(time.monotonic() - t0, 1),
                }
            log.warning("Chunk error at %d, retrying: %s", sent, e)
            await asyncio.sleep(0.5)
            continue

        if "ERR" in resp:
            return {
                "status": "error",
                "message": f"ota chunk rejected at offset {sent}: {resp}",
                "elapsed_s": round(time.monotonic() - t0, 1),
            }

        sent = end
        pct = 15 + (sent * 70) // total
        if sent % (CHUNK_SIZE * 50) < CHUNK_SIZE:
            progress("transfer", f"{sent * 100 // total}%", pct)

    progress("transfer", "100%", 85)

    # Step 3: Verify status
    progress("verify", "Checking staged data...", 88)
    try:
        resp = await protocol.send_command("ota status", timeout=10.0)
    except Exception as e:
        return {
            "status": "error",
            "message": f"ota status failed: {e}",
            "elapsed_s": round(time.monotonic() - t0, 1),
        }
    progress("verify", resp.strip(), 90)

    # Step 4: Commit -- device validates CRC, writes header, resets
    progress("commit", "Committing firmware (device will reset)...", 95)
    try:
        await protocol.send_command("ota commit", timeout=30.0)
    except asyncio.TimeoutError:
        pass  # Expected: device resets on commit before responding
    except Exception as e:
        elapsed = round(time.monotonic() - t0, 1)
        return {
            "status": "error",
            "message": f"ota commit failed: {e}",
            "elapsed_s": elapsed,
        }

    elapsed = round(time.monotonic() - t0, 1)
    progress("done", f"QSPI OTA complete ({elapsed}s)", 100)

    return {
        "status": "ok",
        "message": f"QSPI OTA staged and committed ({len(firmware)} bytes, CRC=0x{crc:04X})",
        "elapsed_s": elapsed,
    }
