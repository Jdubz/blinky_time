"""#251: uploads are stored immutably, and only server-verified stable images
can become the auto-recovery image."""

from __future__ import annotations

from collections.abc import AsyncGenerator
from pathlib import Path
from typing import Any

import pytest
import pytest_asyncio
from httpx import ASGITransport, AsyncClient

from blinky_server.api import routes_firmware
from blinky_server.api.app import create_app
from blinky_server.api.deps import require_api_key, require_deploy_tool, set_fleet
from blinky_server.device.manager import FleetManager
from blinky_server.firmware import images

HEX = b":00000001FF\n" * 200


@pytest_asyncio.fixture
async def client() -> AsyncGenerator[tuple[AsyncClient, FleetManager], None]:
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    set_fleet(fleet)
    app = create_app()
    app.dependency_overrides[require_api_key] = lambda: None
    app.dependency_overrides[require_deploy_tool] = lambda: None
    async with AsyncClient(transport=ASGITransport(app=app), base_url="http://test") as c:  # type: ignore[arg-type]
        yield c, fleet
    set_fleet(None)  # type: ignore[arg-type]


async def _upload(c: AsyncClient, content: bytes, version: str | None) -> dict[str, Any]:
    data = {"version": version} if version else {}
    r = await c.post(
        "/api/fleet/upload", files={"firmware": ("blinky-things.ino.hex", content)}, data=data
    )
    assert r.status_code == 200, r.text
    return dict(r.json())


async def test_upload_of_a_verified_stable_release_is_recovery_eligible(
    client: tuple[AsyncClient, FleetManager], monkeypatch: pytest.MonkeyPatch
) -> None:
    c, _ = client
    monkeypatch.setattr(images, "verify_stable", lambda content, tag, d: (True, "matches b215"))
    body = await _upload(c, HEX, "b215")
    assert body["stable_verified"] is True
    img = images.load(body["firmware_path"])
    assert img is not None and img.stable and img.version == "b215"
    assert Path(body["firmware_path"]).name == f"{body['sha256']}.hex"  # immutable, by hash


async def test_upload_that_fails_verification_is_stored_but_never_stable(
    client: tuple[AsyncClient, FleetManager], monkeypatch: pytest.MonkeyPatch
) -> None:
    c, _ = client
    monkeypatch.setattr(
        images, "verify_stable", lambda content, tag, d: (False, "b216 is not a stable release")
    )
    body = await _upload(c, HEX, "b216")
    assert body["stable_verified"] is False and "not a stable release" in body["stable_reason"]
    img = images.load(body["firmware_path"])
    assert img is not None and not img.stable


async def test_upload_without_a_version_is_never_stable(
    client: tuple[AsyncClient, FleetManager], monkeypatch: pytest.MonkeyPatch
) -> None:
    c, _ = client

    def boom(*_: Any) -> Any:
        raise AssertionError("no version: nothing to verify against")

    monkeypatch.setattr(images, "verify_stable", boom)
    body = await _upload(c, HEX, None)
    assert body["stable_verified"] is False


async def test_a_later_upload_does_not_change_the_armed_recovery_image(
    client: tuple[AsyncClient, FleetManager], monkeypatch: pytest.MonkeyPatch, stable_image: Any
) -> None:
    """The #251 scenario: recovery armed with a stable image, then a
    non-stable upload whose deploy stops. The old shared firmware.hex was
    overwritten; now the armed image is untouched and still verified."""
    c, fleet = client
    armed = stable_image()
    fleet.set_recovery_firmware(str(armed), ["dev-A"])
    before = armed.read_bytes()
    monkeypatch.setattr(images, "verify_stable", lambda content, tag, d: (False, "candidate"))
    body = await _upload(c, b"candidate-bytes" * 200, "b216")
    assert body["firmware_path"] != str(armed)
    assert armed.read_bytes() == before
    assert fleet.recovery_firmware_path == str(armed)
    assert fleet._load_recovery_firmware() == (str(armed), {"dev-A"})


def test_arming_helper_refuses_unverified_images(stable_image: Any) -> None:
    fleet = FleetManager(enable_ble=False, enable_serial=False)
    unverified = images.store(b"z" * 2000, version="b216", stable=False, source="upload").path
    out = routes_firmware._arm_recovery_if_stable(fleet, unverified, ["dev-A"])
    assert out["recovery_armed"] is False and "#251" in out["recovery_note"]
    assert fleet.recovery_firmware_path is None

    good = stable_image()
    out = routes_firmware._arm_recovery_if_stable(fleet, good, ["dev-A"])
    assert out == {"recovery_armed": True, "recovery_version": "b215"}
    assert fleet.recovery_firmware_path == str(good)


def test_store_keeps_the_armed_image_when_pruning(stable_image: Any) -> None:
    armed = stable_image(b"armed" * 400)
    for i in range(images.KEEP_IMAGES + 3):
        images.store(
            f"other-{i}".encode() * 300,
            version=None,
            stable=False,
            source="upload",
            keep=str(armed),
        )
    assert armed.exists() and images.load(armed) is not None
    assert len(list(images.images_dir().glob("*.hex"))) <= images.KEEP_IMAGES + 1
