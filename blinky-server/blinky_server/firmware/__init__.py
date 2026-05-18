"""Firmware-flashing implementation modules.

The single entry point for any flash is ``FleetManager.flash_device()``
(in ``blinky_server.device.manager``); it dispatches to either
``_uf2_write_impl_for_job`` (UF2 over USB-MSC) or ``_ble_dfu_write_impl``
(Nordic Legacy DFU over BLE), both gated by the
``_inside_flash_job_orchestrator`` ContextVar so direct calls fail loud
with ``OutsideFlashJobContextError``. See ``docs/FLASH_LOCKDOWN_PLAN.md``
for the architecture and ``firmware/_guard.py`` for the lockdown
mechanism.

Modules in this package:

  - ``compile``: builds the ``.hex`` (and the DFU ``.zip`` adafruit-nrfutil
    expects for BLE-DFU) from the Arduino-CLI command, in a worker
    thread.
  - ``uf2_upload``: launches and watches the ``tools/uf2_upload.py``
    subprocess that writes a UF2 image to the device's bootloader MSC.
  - ``ble_dfu``: implements the Nordic Legacy DFU protocol over BLE
    using bleak.
  - ``flash_job``: ``FlashJob`` state machine + ``select_transport``
    transport-picker.
  - ``verify``: post-flash state-machine that polls device signals
    (re-enumeration, app-mode handshake, version string) and stamps
    ``FlashJob.verify_sub_state``.
  - ``anomalies``: post-hoc detectors that compare ``SignalHistory``
    timestamps against expected timing windows and surface anomalies
    on the ``FlashJob``.
  - ``_guard``: the lockdown ContextVar + ``OutsideFlashJobContextError``.

Nothing in this package should be called from outside its boundary
except via ``FleetManager.flash_device()``. Direct imports of the
``_impl`` functions in production code raise at the entry guard.
"""
