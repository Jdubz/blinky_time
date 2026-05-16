"""Wire format for fleet-broadcast packets.

Mirrors ``blinky-things/comms/BleProtocol.h`` so the bytes the firmware's
``BleScanner`` decodes are exactly what the server emits. Do NOT change a
number here without changing the firmware header at the same time — the
firmware silently drops packets whose protocol version it doesn't know.

The 0xFFFF company ID is the public "testing / development" reservation
in the Bluetooth assigned-numbers list. Fine for an event-installation
fleet that never leaves a private LemonCart-class radio environment.

Packet layout (this is what we hand BlueZ as ManufacturerData):
  bytes[0]   version    = PROTOCOL_VERSION (0x01)
  bytes[1]   type       = PacketType (SETTINGS / SCENE / COMMAND)
  bytes[2]   sequence   = rolling 0..255, dedup'd on the receiver by
                          (source BD addr, sequence)
  bytes[3]   fragment   = high nibble = total fragments, low nibble = index
                          (0x10 = single-packet, no reassembly)
  bytes[4:]  payload

BlueZ prepends the company ID (0xFFFF, little-endian) itself when it serializes
the ManufacturerData AD structure, so callers MUST NOT include it here.
"""

from __future__ import annotations

from enum import IntEnum

COMPANY_ID = 0xFFFF
PROTOCOL_VERSION = 0x01


class PacketType(IntEnum):
    SETTINGS = 0x01
    SCENE = 0x02
    COMMAND = 0x03


HEADER_SIZE = 4
# Legacy LE advertising data limit is 31 bytes total for the whole AD payload.
# Subtract the manufacturer-specific data AD wrapper (2 bytes type+len),
# the company ID (2 bytes), and our 4-byte header = 23 bytes payload room
# in a single legacy ad. With BLE 5 extended advertising (which BlueZ supports
# on the Pi 4 BCM43455) we can go up to ~240. The protocol's `fragment` byte
# is already plumbed; payloads larger than the legacy budget should split
# into total/index pairs at the call site.
LEGACY_PAYLOAD_MAX = 23
EXTENDED_PAYLOAD_MAX = 240

# Unfragmented single packet — high nibble = 1 total, low nibble = 0 index.
FRAGMENT_SINGLE = 0x10


def build_packet(
    packet_type: PacketType,
    payload: bytes | str,
    sequence: int,
    fragment: int = FRAGMENT_SINGLE,
) -> bytes:
    """Pack a single broadcast packet.

    ``payload`` may be a string (UTF-8 encoded) or raw bytes. Returns the
    manufacturer-data bytes to hand to BlueZ via D-Bus. ``sequence`` should
    advance monotonically across calls; the receiver dedups by
    (source BD addr, sequence). Reusing the same sequence number for the
    same source intentionally makes the packet a no-op on the receiver
    — useful for re-arming a slot without re-emitting a command.
    """
    if not (0 <= sequence <= 0xFF):
        raise ValueError(f"sequence must fit in a byte, got {sequence}")
    if not (0 <= fragment <= 0xFF):
        raise ValueError(f"fragment must fit in a byte, got {fragment}")
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    header = bytes((PROTOCOL_VERSION, int(packet_type), sequence, fragment))
    return header + payload
