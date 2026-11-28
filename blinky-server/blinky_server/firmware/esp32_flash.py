"""ESP32-S3 firmware writes over USB Serial/JTAG with esptool (docs/TEST_BOX.md, "ESP32-S3 flash
and recovery").

Runs ``hosttools/esp32_flash.py`` with the server's own interpreter (the pinned ``esptool`` is in
the server's venv, the ``firmware`` extra). The tool checks the image (a merged ESP32-S3 flash
image), the port (303a:1001 with the chip's MAC as its USB serial) and the chip itself in
download mode (an ESP32-S3 with that MAC) before writing anything, holds blinky's serial lock on
the port, writes, verifies and resets the chip into the new app.

GUARDED like the UF2 and BLE-DFU writes: callable only from inside ``_run_flash_job``
(``FleetManager.flash_device`` is the single flash entry point).
"""

from __future__ import annotations

import asyncio
import contextlib
import logging
import os
import re
import sys
from pathlib import Path
from typing import TYPE_CHECKING, Any

from ._guard import assert_inside_orchestrator

if TYPE_CHECKING:
    from .flash_job import FlashJob

log = logging.getLogger(__name__)

_WRITE_COMPLETE_RE = re.compile(r"\[PASS\] Wrote ([\d,]+) ?/ ?([\d,]+) bytes")
_RESET_RE = re.compile(r"\[PASS\] Reset into the app")
# The tool's whole run: download-mode entry, a full erase on recovery (8 MB, ~30 s), the write
# and its MD5 verify. Measured well inside this; the subprocess is killed past it.
ESP32_WRITE_TIMEOUT_S = 300.0


def _find_tool() -> Path | None:
    from ..paths import hosttools_dir

    candidate = hosttools_dir() / "esp32_flash.py"
    return candidate if candidate.exists() else None


async def _esp32_write_impl_for_job(
    job: FlashJob,
    serial_port: str,
    firmware_path: str,
    transport: Any,
    *,
    expect_mac: str,
    full: bool,
) -> bool:
    """Write an ESP32-S3 image to one chip, bound to a ``FlashJob``.

    ``full``: erase the whole chip first and write the image (recovery: NVS, so the device
    config, is cleared too). Otherwise everything but the NVS partition (the config survives).

    Returns True iff the tool reported the image written and verified (``[PASS] Wrote N / N``)
    and the chip reset into it. On False, ``job.error`` says why.
    """
    assert_inside_orchestrator("_esp32_write_impl_for_job")
    from .flash_job import FlashJobState  # avoid a top-level cycle

    assert job.state is FlashJobState.WRITING, (
        f"_esp32_write_impl_for_job expected WRITING, got {job.state.value}"
    )
    tool = _find_tool()
    if tool is None:
        job.set_error("hosttools/esp32_flash.py not found")
        return False
    if not os.path.isfile(firmware_path):
        job.set_error(f"firmware file not found: {firmware_path}")
        return False

    # Off the port before the tool opens it (the server holds its reconnect off the target).
    with contextlib.suppress(Exception):
        await transport.disconnect()

    cmd = [
        sys.executable,
        "-u",
        str(tool),
        "--port",
        serial_port,
        "--image",
        firmware_path,
        "--expect-mac",
        expect_mac,
        "--mode",
        "full" if full else "app",
    ]
    log.info("_esp32_write_impl_for_job %s: launching %s", job.job_id, " ".join(cmd))
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.STDOUT,
        cwd=str(tool.parent),
    )
    written = reset = False
    captured: list[str] = []

    async def _stream() -> None:
        nonlocal written, reset
        assert proc.stdout is not None
        while True:
            raw = await proc.stdout.readline()
            if not raw:
                return
            line = raw.decode("utf-8", errors="replace").rstrip()
            if not line:
                continue
            captured.append(line)
            log.info("[esp32_flash] %s", line)
            m = _WRITE_COMPLETE_RE.search(line)
            if m and not written:
                written = True
                n, total = (int(g.replace(",", "")) for g in m.groups())
                try:
                    job.record_progress(n, total)
                except Exception as exc:  # a progress bug must not derail the flash
                    log.warning("_esp32_write_impl_for_job: record_progress raised %s", exc)
            if _RESET_RE.search(line):
                reset = True

    try:
        await asyncio.wait_for(_stream(), timeout=ESP32_WRITE_TIMEOUT_S)
    except TimeoutError:
        log.warning(
            "_esp32_write_impl_for_job %s: %ss timeout (written=%s); killing the tool",
            job.job_id,
            ESP32_WRITE_TIMEOUT_S,
            written,
        )
        with contextlib.suppress(ProcessLookupError):
            proc.kill()
    with contextlib.suppress(asyncio.TimeoutError):
        await asyncio.wait_for(proc.wait(), timeout=10)

    if written and reset:
        return True
    errors = [li for li in captured if li.startswith("ERROR")]
    msg = errors[-1] if errors else (captured[-1] if captured else "no output")
    if written:
        # On flash and verified, but the reset into it wasn't confirmed: verify decides (the
        # chip may have reset anyway); say so.
        log.warning("_esp32_write_impl_for_job %s: written, reset unconfirmed: %s", job.job_id, msg)
        return True
    job.set_error(f"ESP32-S3 write did not complete: {msg}")
    return False
