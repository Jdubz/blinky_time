# BLE reference

What blinky-server and the firmware do over Bluetooth today, and the known limits. The build history is in `archive/BLUETOOTH_IMPLEMENTATION_PLAN.md` and `archive/BLE_FLEET_RELIABILITY_PLAN.md`. Flashing rules (deploy.sh only, the single flash path) are in `CLAUDE.md` and `SAFETY.md`; browser updates over Bluetooth are in blinky-firmware's [WEB_FLASHER.md](https://github.com/Jdubz/blinky-firmware/blob/master/docs/WEB_FLASHER.md).

The test box runs with BLE off (`--no-ble` and `dtoverlay=disable-bt`; see `TEST_BOX.md`). Everything below is about blinkyhost and the fleet.

## Connections (NUS)

- Each device runs the Nordic UART Service (`6e400001-b5a3-f393-e0a9-e50e24dcca9e`). Command output goes through `Print&`, tee'd to USB serial and NUS, so every command works over either.
- `json info` reports `platform`, `sn` (FICR DEVICEID) and `ble` (the app's BLE MAC). The server dedups a device seen on both transports by `hardware_sn` only.
- `blinky_server/transport/ble_transport.py`: before each *re*connect (not the first connect of a transport, including the first one after a server restart) it runs `bluetoothctl remove <addr>`, because a stale GATT cache stacks notifications so each arrives N times. The first connect skips it to save about 1 s, so stale subscriptions from before a restart aren't evicted until the next reconnect. It also forces `StartNotify` rather than `AcquireNotify`, and bounds the whole connect at 25 s (15 s plus GATT rediscovery).
- On startup the server disconnects stale BlueZ connections left by a previous run (`transport/discovery.py`).
- `--no-serial` runs the server BLE-only; `--no-ble` runs it USB-only.

## Fleet broadcast (advertising)

Fleet-wide commands go out as BLE advertisements, not connections (`blinky_server/ble/advertiser.py`, wire format `ble/protocol.py`, mirrored in the firmware's `comms/BleProtocol.h`; change both together, since the firmware drops packets with an unknown protocol version).

- Manufacturer data, company ID `0xFFFF`. Header: version (`0x01`), type (`SETTINGS`, `SCENE`, `COMMAND`, `COMMAND_V2`), sequence, fragment nibbles. 23 payload bytes fit a legacy advertisement.
- The firmware's `BleScanner` dedups by (source address, sequence) and runs the payload through the same handler as a serial command.
- `COMMAND_V2` carries a 16-bit `command_id`. The broadcaster re-emits each command several times, and the firmware applies a given `command_id` once.
- Gossip-ACK: each device puts the last `command_id` it accepted in its scan response (marker `0xA0`, then the id, little-endian). The server re-broadcasts to devices that lag.

## BLE DFU (nRF52840)

`blinky_server/firmware/ble_dfu.py`, reached only through `FleetManager.flash_device`. Fleet bootloader 0.8.0-10 (our fork) speaks Nordic Legacy DFU (SDK v11), not Secure DFU.

- Enter it with `bootloader ble` over serial or NUS. The bootloader advertises at the app's address + 1 (last octet), with the DFU service `00001530-1212-efde-1523-785feabcd123`. Clear the BlueZ GATT cache between the app and bootloader connections.
- Sequence: START_DFU (60 s timeout; it erases flash) → INIT_DFU → SET_PRN (8) → RECEIVE_FW → VALIDATE → ACTIVATE_AND_RESET.
- Control-point writes use write-with-response; data-packet writes use write-without-response. The last chunk is padded to a 4-byte boundary.
- Chunk size is the negotiated ATT MTU − 3, from 20 up to 244 bytes.
- Init packet (built in pure Python by `firmware/compile.py`; adafruit-nrfutil is not used): `device_type` `0x0052` (the bootloader checks it exactly), `sd_req` `0xFFFE`, CRC-16/CCITT of the image.
- On `INVALID_STATE` at START_DFU, or any failure mid-transfer, the server sends SYSTEM_RESET (`0x06`) so the bootloader doesn't stay stuck, then retries the whole sequence (up to 3 attempts). The bootloader keeps its DFU state across BLE disconnects until a reset.
- After a DFU the server first looks for the app's original BLE address (`firmware/ble_dfu.py`). Only if that fails does it fall back to any nearby Blinky by name, NUS service and RSSI. That fallback can match a different nearby device, so it doesn't prove which device came back.

## Known limitations

- **BLE-DFU is slow:** transfers still negotiate MTU 23 (20-byte chunks), about 6.5 min per device, one device at a time ([blinky-firmware#11](https://github.com/Jdubz/blinky-firmware/issues/11)). The bootloader accepts up to 244-byte packets.
- **No clean all-BLE deploy yet:** one `deploy.sh` run over BLE has to exit 0 ([#215](https://github.com/Jdubz/blinky_time/issues/215)).
- **USB after BLE DFU:** USB serial may not re-enumerate after a BLE DFU without a real power cycle; BLE reconnects.
- **Weak signal:** below about −80 dBm, throughput can drop to about one notification per second, so large responses (`json settings`) can time out. Short commands and `json info` still work.
- **Connected devices stop advertising,** so discovery can't see them while the server holds the connection.
- **No discovery allowlist:** a server with BLE on adopts any blinky device in range, and its auto-recovery could flash it. That is why the test box runs `--no-ble` (per-server device ownership, `TEST_BOX.md`).
- **Unmeasured in the field:** gossip-ACK delivery across the whole fleet at range ([#221](https://github.com/Jdubz/blinky_time/issues/221)), and broadcast reception while a central is connected ([blinky-firmware#16](https://github.com/Jdubz/blinky-firmware/issues/16)).
