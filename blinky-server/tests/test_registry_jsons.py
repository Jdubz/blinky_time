"""Lints every device registry JSON for buildable validity.

The 2026-05-18 postmortem identified `ledType: 12390` in
`cart_inner.json` and `cart_outer.json` as the root cause of a
configured-mode boot crash that pushed both carts into BLE-DFU recovery.
12390 (0x3066) decodes to rOffset=2, gOffset=1, bOffset=2 — r and b
both writing the same byte. The firmware's `Nrf52PwmLedStrip` ctor
correctly refused to construct (PR 142 buffer-overflow fix), the setup()
haltWithError loop tripped the WDT, and after three boot cycles the
device fell into recovery.

This module pins the rule for every JSON in `devices/registry/` so a
"next 12390" typo is caught before it lands on a device. The check
mirrors the firmware validation in `DeviceConfigLoader::validate()` and
`Nrf52PwmLedStrip` ctor exactly.
"""

from __future__ import annotations

import json
from pathlib import Path

import pytest

REGISTRY_DIR = Path(__file__).resolve().parents[2] / "devices" / "registry"


def _registry_json_files() -> list[Path]:
    """Every non-status JSON in the registry. Sorted for deterministic ids."""
    if not REGISTRY_DIR.is_dir():
        pytest.skip(f"registry dir missing: {REGISTRY_DIR}")
    return sorted(p for p in REGISTRY_DIR.glob("*.json") if "_status" not in p.name)


def _decode_offsets(led_type: int) -> tuple[int, int, int]:
    """Decode (r, g, b) byte offsets from a NEO_*-style ledType.

    Matches `Nrf52PwmLedStrip::Nrf52PwmLedStrip` (blinky-things/hal/hardware/
    Nrf52PwmLedStrip.cpp:20-23): lower 8 bits encode three 2-bit fields,
    b=bits[1:0], g=bits[3:2], r=bits[5:4].
    """
    order = led_type & 0xFF
    b = (order >> 0) & 0x3
    g = (order >> 2) & 0x3
    r = (order >> 4) & 0x3
    return r, g, b


@pytest.mark.parametrize("registry_path", _registry_json_files(), ids=lambda p: p.name)
def test_registry_json_ledtype_is_buildable(registry_path: Path) -> None:
    """Every registry JSON must have a ledType the firmware will accept.

    Rules (must mirror DeviceConfigLoader::validate and Nrf52PwmLedStrip
    ctor in firmware):
      - each of r, g, b offsets is 0-2
      - r, g, b are pairwise distinct (no two channels writing to the
        same byte slot)

    Examples:
      82  (NEO_GRB + NEO_KHZ800) → r=1, g=0, b=2  ✓ standard WS2812B
      6   (NEO_RGB)              → r=0, g=1, b=2  ✓ native-RGB strips
      12390 (0x3066)             → r=2, g=1, b=2  ✗ r and b collide (REJECT)
      18  (RGBW low nibble)      → r=0, g=0, b=2  ✗ r and g collide (REJECT)
    """
    cfg = json.loads(registry_path.read_text())
    led_type = cfg.get("ledType")
    if led_type is None:
        # ledType is required by the firmware; the registry README documents it.
        pytest.fail(f"{registry_path.name}: missing required 'ledType' field")

    r, g, b = _decode_offsets(led_type)
    in_range = (r <= 2) and (g <= 2) and (b <= 2)
    distinct = (r != g) and (r != b) and (g != b)

    assert in_range and distinct, (
        f"{registry_path.name}: ledType={led_type} (0x{led_type:X}) decodes to "
        f"r={r} g={g} b={b}. "
        + (
            "Offsets must be distinct; this value silently maps two color "
            "channels to the same byte slot."
            if not distinct
            else "Offsets must be 0-2; RGBW types are not supported."
        )
        + " Use 82 (NEO_GRB + NEO_KHZ800) for WS2812B strips or 6 (NEO_RGB)"
        " for native-RGB strips. See devices/registry/README.md."
    )


def test_decoder_matches_firmware_examples() -> None:
    """Pin the decode helper against the firmware's known-good examples.

    These are the values present in production registry JSONs and the
    invalid value that triggered the 2026-05-18 postmortem. If the decode
    helper drifts from the firmware's interpretation, ALL the per-JSON
    tests above are silently wrong; this anchors the helper itself.
    """
    # NEO_GRB + NEO_KHZ800 = 0x52 = 82  → "Transmit as G,R,B" → r=1, g=0, b=2
    assert _decode_offsets(82) == (1, 0, 2)
    # NEO_RGB = 0x06 = 6  → "Transmit as R,G,B" → r=0, g=1, b=2
    assert _decode_offsets(6) == (0, 1, 2)
    # 12390 = 0x3066, lower byte 0x66 = 0b01100110 → r=2, g=1, b=2 (DUPLICATE)
    assert _decode_offsets(12390) == (2, 1, 2)
