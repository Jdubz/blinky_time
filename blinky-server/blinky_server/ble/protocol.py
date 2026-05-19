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
    # Same payload as COMMAND, but carries a 16-bit command_id token
    # immediately after the 4-byte header. Firmware tracks the last
    # command_id seen per source and short-circuits any packet whose
    # command_id matches the source's last accepted value, so N re-emits
    # of one LOGICAL command (the broadcaster's redundancy strategy)
    # apply exactly once. See BLE_FLEET_RELIABILITY_PLAN item #2.
    COMMAND_V2 = 0x04


HEADER_SIZE = 4
# Size of the command_id token COMMAND_V2 carries after the header.
COMMAND_V2_TOKEN_SIZE = 2
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

    Raises ``ValueError`` if the payload exceeds ``EXTENDED_PAYLOAD_MAX``.
    Without this guard, BlueZ silently truncates or rejects the
    advertisement update — the exact silent-failure pattern CLAUDE.md
    "no silent fallbacks" rule forbids (PR #140 review).
    """
    if not (0 <= sequence <= 0xFF):
        raise ValueError(f"sequence must fit in a byte, got {sequence}")
    if not (0 <= fragment <= 0xFF):
        raise ValueError(f"fragment must fit in a byte, got {fragment}")
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    if len(payload) > EXTENDED_PAYLOAD_MAX:
        raise ValueError(
            f"payload is {len(payload)} bytes; max is {EXTENDED_PAYLOAD_MAX} "
            f"(EXTENDED_PAYLOAD_MAX). Split the command, or use the fragment "
            f"field for multi-packet delivery."
        )
    header = bytes((PROTOCOL_VERSION, int(packet_type), sequence, fragment))
    return header + payload


def build_command_v2_packet(
    payload: bytes | str,
    sequence: int,
    command_id: int,
    fragment: int = FRAGMENT_SINGLE,
) -> bytes:
    """Pack a COMMAND_V2 broadcast packet with idempotency token.

    All N re-emits of a single LOGICAL command must share the same
    ``command_id`` so the receiver can identify them as one event.
    ``sequence`` should still advance monotonically across emits (the
    seq-dedup ring relies on that to collapse bus-level retransmissions
    of identical (src, seq) packets); the command_id is the higher-level
    "same logical command" key, decoupled from the BLE-level sequence.

    Layout on the wire (after BlueZ adds the 0xFFFF company ID prefix):
      bytes[0]    version
      bytes[1]    type = COMMAND_V2 (0x04)
      bytes[2]    sequence
      bytes[3]    fragment
      bytes[4:6]  command_id (little-endian uint16)
      bytes[6:]   command string

    Raises ``ValueError`` for out-of-range arguments.

    Two relevant size ceilings — the docstring of the older version
    quoted only the looser one and that was misleading:

      * **API limit (loose):** ``build_packet`` rejects payloads longer
        than ``EXTENDED_PAYLOAD_MAX = 240``. The command_id token is
        part of the payload from ``build_packet``'s perspective, so
        the API ceiling on the command STRING is
        ``EXTENDED_PAYLOAD_MAX - COMMAND_V2_TOKEN_SIZE = 238`` bytes.

      * **On-wire effective limit (tight):** the firmware scanner
        (``BleScanner`` on nRF52840) today watches only legacy BLE
        adv (31-byte total adv payload). After BlueZ's manufacturer-
        data AD wrapper (2 B), company ID (2 B), our 4-byte header,
        and the COMMAND_V2 2-byte token, the on-air command-string
        budget shrinks to ``LEGACY_PAYLOAD_MAX - COMMAND_V2_TOKEN_SIZE
        = 21`` bytes. Anything longer rides extended-adv on the air
        (BlueZ silently promotes it) and the firmware scanner will
        NEVER see it. Until the firmware scanner gains extended-adv
        support (OPEN_ISSUES §6), commands MUST stay ≤21 bytes to
        actually reach devices.

    The hard ``ValueError`` is only thrown at the loose limit (238 B);
    payloads in 22..238 B build successfully and even reach BlueZ
    successfully, but are dropped on the firmware scanner side. That
    silent-on-air-but-no-effect failure mode is the reason the scene
    system was retired in favour of short ``gen <name>`` commands;
    see OPEN_ISSUES §1.2 / §6.
    """
    if not (0 <= command_id <= 0xFFFF):
        raise ValueError(f"command_id must fit in a uint16, got {command_id}")
    if isinstance(payload, str):
        payload = payload.encode("utf-8")
    token = bytes((command_id & 0xFF, (command_id >> 8) & 0xFF))
    return build_packet(
        PacketType.COMMAND_V2,
        token + payload,
        sequence=sequence,
        fragment=fragment,
    )


# ─── Gossip-ACK (firmware → server, BLE_FLEET_RELIABILITY_PLAN item #5) ─────
#
# Each device exposes the last COMMAND_V2 command_id it accepted in its own
# BLE adv's scan-response manufacturer data so the server can detect lagged
# devices (missed every emit of a broadcast) and re-broadcast.
#
# Layout (after BlueZ has stripped its 2-byte company-ID prefix, which is
# what bleak hands us via ``AdvertisementData.manufacturer_data[0xFFFF]``):
#   bytes[0]   marker = GOSSIP_ACK_MARKER (0xA0)
#   bytes[1:3] command_id (little-endian uint16)
#
# 3 bytes total. The marker disambiguates this block from the broadcaster's
# COMMAND_V2 manufacturer data (which the firmware emits and we also see in
# scan results); both use COMPANY_ID = 0xFFFF but the broadcaster's payload
# starts with PROTOCOL_VERSION = 0x01, never 0xA0, so the marker distinguishes.
GOSSIP_ACK_MARKER = 0xA0
GOSSIP_ACK_SIZE = 3  # marker (1B) + command_id LE (2B)


def parse_gossip_ack(mfg_payload: bytes | bytearray | memoryview | None) -> int | None:
    """Extract the last-accepted ``command_id`` from a scan-response ACK block.

    ``mfg_payload`` is the bytes bleak returns from
    ``AdvertisementData.manufacturer_data.get(COMPANY_ID)`` — i.e. the
    manufacturer data with the company-ID prefix already stripped.

    Returns the device's last accepted ``command_id`` if the payload is
    a valid gossip-ACK block, otherwise ``None``. ``None`` covers every
    not-an-ACK case (missing payload, wrong marker, wrong size, the
    broadcaster's own COMMAND_V2 manufacturer data echoed in someone
    else's scan) — callers should treat ``None`` as "this device's ACK
    is unknown right now," not "this device is at command_id 0."

    A return value of 0 specifically means "device boot default — no
    command applied yet." The broadcaster reserves command_id 0 (see
    ``_next_command_id``) so a real command's ACK can never be 0.
    """
    if mfg_payload is None:
        return None
    if len(mfg_payload) != GOSSIP_ACK_SIZE:
        return None
    if mfg_payload[0] != GOSSIP_ACK_MARKER:
        return None
    return mfg_payload[1] | (mfg_payload[2] << 8)
