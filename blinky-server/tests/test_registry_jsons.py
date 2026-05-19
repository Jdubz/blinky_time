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


@pytest.mark.parametrize("registry_path", _registry_json_files(), ids=lambda p: p.name)
def test_registry_json_required_fields_present(registry_path: Path) -> None:
    """Every registry JSON must declare the fields the firmware's
    ``SerialConsole::handleDeviceUpload`` reads. Missing fields fall
    through to default values (ledWidth=0, ledPin=10, etc.), which
    produces a silently-broken device (no LEDs, or LEDs on the wrong
    pin). The firmware does NOT log a warning when keys are absent
    from the JSON; the only signal is the device behaving wrong
    post-flash. Catch the omission at lint time instead.

    PR #144 review item 8. The ``ledType``-only check that preceded
    this test caught the 2026-05-18 incident-shape; this widens to
    the rest of the firmware-required surface so a new field-omission
    typo can't escape the test gate the way ``ledType: 12390`` did.
    """
    cfg = json.loads(registry_path.read_text())

    # These keys are read at upload time. Their default-fallback values
    # are functional but silently wrong for a real device — e.g. ledWidth
    # default is 0 (no LEDs), ledPin default is 10 (wrong for big_bucket
    # which is on D0). Require explicit declaration.
    required_keys = ("deviceId", "deviceName", "ledWidth", "ledHeight", "ledPin", "ledType")
    for key in required_keys:
        assert key in cfg, (
            f"{registry_path.name}: missing required field '{key}'. "
            f"The firmware's JSON parser defaults this silently, which "
            f"produces a broken-but-not-erroring device post-flash."
        )


@pytest.mark.parametrize("registry_path", _registry_json_files(), ids=lambda p: p.name)
def test_registry_json_device_id_matches_filename(registry_path: Path) -> None:
    """The fleet manager + deploy.sh both key off the filename stem,
    while the firmware reads ``deviceId`` from the JSON body. If the
    two disagree, a device flashed with this config reports a different
    identity to the server than the server thinks it deployed — which
    breaks dedup, deploy whitelisting (``--devices=<id>``), and the
    recovery whitelist's canonical-id resolver. Catch the drift here."""
    cfg = json.loads(registry_path.read_text())
    declared_id = cfg.get("deviceId")
    filename_stem = registry_path.stem
    assert declared_id == filename_stem, (
        f"{registry_path.name}: deviceId={declared_id!r} disagrees with "
        f"filename stem {filename_stem!r}. The fleet manager and "
        f"deploy.sh key off the filename; the firmware reports the JSON "
        f"value. They MUST match."
    )


@pytest.mark.parametrize("registry_path", _registry_json_files(), ids=lambda p: p.name)
def test_registry_json_led_dimensions_are_positive(registry_path: Path) -> None:
    """``ledWidth * ledHeight`` is the total LED count. Zero in either
    dimension defaults the renderer to a no-op pixel matrix — the
    device boots into apparent normality but the LEDs never light up.
    PR #144 review item 8."""
    cfg = json.loads(registry_path.read_text())
    w = cfg.get("ledWidth", 0)
    h = cfg.get("ledHeight", 0)
    assert isinstance(w, int) and isinstance(h, int), (
        f"{registry_path.name}: ledWidth/ledHeight must be ints, "
        f"got {type(w).__name__}/{type(h).__name__}"
    )
    assert w >= 1 and h >= 1, (
        f"{registry_path.name}: ledWidth={w} x ledHeight={h} = "
        f"{w * h} LEDs. Both dimensions must be ≥1."
    )
    # Sanity cap. The largest device in the fleet today is display_v1
    # (32x32 = 1024). A 4-digit total likely indicates a typo (the
    # earlier ``ledType: 12390`` bug pattern in 4-digit territory was
    # already silently flashed and bricked two carts). 2048 leaves
    # comfortable headroom and would catch e.g. an off-by-orders-of-
    # magnitude typo.
    assert w * h <= 2048, (
        f"{registry_path.name}: {w}x{h} = {w * h} LEDs exceeds the 2048 "
        f"sanity cap. If this is intentional (new device class), raise "
        f"the cap here AND add a firmware test for the larger size."
    )


@pytest.mark.parametrize("registry_path", _registry_json_files(), ids=lambda p: p.name)
def test_registry_json_orientation_and_layout_in_range(registry_path: Path) -> None:
    """``orientation`` and ``layoutType`` are firmware enums. Out-of-
    range values silently cast and dispatch to a default branch, which
    is again the silent-failure surface we want to close at lint
    time. Mirror the enum ranges in
    ``blinky-things/devices/DeviceConfig.h`` (MatrixOrientation +
    LayoutType): 0..4 and 0..2 respectively. PR #144 review item 8."""
    cfg = json.loads(registry_path.read_text())
    orientation = cfg.get("orientation")
    layout = cfg.get("layoutType")
    if orientation is not None:
        assert isinstance(orientation, int) and 0 <= orientation <= 4, (
            f"{registry_path.name}: orientation={orientation} "
            f"outside the MatrixOrientation enum range 0..4. See "
            f"blinky-things/devices/DeviceConfig.h."
        )
    if layout is not None:
        assert isinstance(layout, int) and 0 <= layout <= 2, (
            f"{registry_path.name}: layoutType={layout} "
            f"outside the LayoutType enum range 0..2. See "
            f"blinky-things/devices/DeviceConfig.h."
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
