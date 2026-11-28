"""firmware_release.verify_dev_package, the box's dev loop (docs/DEV_LOOP.md): beyond
download_staging's manifest checks (test_staging_download.py), nothing unverified may sit beside
the image: every file is a regular file listed in the manifest with a matching sha256."""

from __future__ import annotations

import hashlib
import json
import subprocess
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import firmware_release as fr  # noqa: E402

SHA = "cd" * 20
HEX, UF2 = b":00000001FF\n", b"UF2"


def package(d: Path, **files: bytes) -> Path:
    d.mkdir(parents=True, exist_ok=True)
    content = {"blinky-things-b220.hex": HEX, "blinky-things-b220.uf2": UF2, **files}
    m = {"build": 220, "version": f"b220-dev-{SHA[:8]}", "channel": "dev", "git_sha": SHA,
         "files": {n: hashlib.sha256(b).hexdigest() for n, b in content.items()}}
    (d / "manifest.json").write_text(json.dumps(m))
    for n, b in content.items():
        (d / n).write_bytes(b)
    return d


def test_an_unlisted_file_beside_the_image_is_refused(tmp_path: Path) -> None:
    d = package(tmp_path / "p")
    fr.verify_dev_package(d, SHA)
    (d / "extra.hex").write_bytes(b"x")
    with pytest.raises(fr.ReleaseError, match="not in the manifest"):
        fr.verify_dev_package(d, SHA)


def test_every_listed_file_is_checked_not_only_hex_and_uf2(tmp_path: Path) -> None:
    d = package(tmp_path / "p", **{"blinky-things-b220.bin": b"bin"})
    fr.verify_dev_package(d, SHA)
    (d / "blinky-things-b220.bin").write_bytes(b"other")
    with pytest.raises(fr.ReleaseError, match="sha256 mismatch"):
        fr.verify_dev_package(d, SHA)


def test_a_symlink_or_directory_is_refused(tmp_path: Path) -> None:
    d = package(tmp_path / "a")
    (d / "link.hex").symlink_to(d / "blinky-things-b220.hex")
    with pytest.raises(fr.ReleaseError, match="not a regular file"):
        fr.verify_dev_package(d, SHA)
    d = package(tmp_path / "b")
    (d / "sub").mkdir()
    with pytest.raises(fr.ReleaseError, match="not a regular file"):
        fr.verify_dev_package(d, SHA)


def test_the_cli_prints_what_the_server_reads(tmp_path: Path) -> None:
    d = package(tmp_path / "p")
    tool = Path(fr.__file__)
    args = [sys.executable, str(tool), "verify-dev-package", "--dir", str(d), "--sha"]
    out = json.loads(subprocess.run([*args, SHA], capture_output=True, text=True,
                                    check=True).stdout)
    assert out["sha"] == SHA and out["version"] == f"b220-dev-{SHA[:8]}"
    assert Path(out["hex"]).is_file() and Path(out["uf2"]).is_file()
    bad = subprocess.run([*args, "ab" * 20], capture_output=True, text=True)
    assert bad.returncode == 1 and "not the dev build" in bad.stderr


# [blinky_time: the ESP32-S3 image, docs/TEST_BOX.md "ESP32-S3 flash and recovery"]
def test_the_esp32s3_image_is_optional_and_verified_when_listed(tmp_path: Path) -> None:
    assert fr.verify_dev_package(package(tmp_path / "a"), SHA)["esp32s3"] is None
    s3 = "blinky-things-b220-esp32s3.bin"
    d = package(tmp_path / "b", **{s3: b"\xe9merged"})
    assert fr.verify_dev_package(d, SHA)["esp32s3"] == d / s3
    (d / s3).write_bytes(b"\xe9other")
    with pytest.raises(fr.ReleaseError, match="sha256 mismatch"):
        fr.verify_dev_package(d, SHA)
    (d / s3).unlink()
    with pytest.raises(fr.ReleaseError, match="lists blinky-things-b220-esp32s3.bin but it is missing"):
        fr.verify_dev_package(d, SHA)


def test_the_manifests_esp32s3_block_must_match_the_image(tmp_path: Path) -> None:
    """Its NVS range is what a routine flash keeps: it must be the image's own partition
    table's, which hosttools/esp32_flash.py skips."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from test_esp32_flash import merged

    s3 = "blinky-things-b220-esp32s3.bin"
    good = {"image": s3, "image_offset": 0, "nvs": {"offset": 0x9000, "size": 0x5000}}

    def pkg(where: str, block: dict) -> Path:
        d = package(tmp_path / where, **{s3: merged()})
        m = json.loads((d / "manifest.json").read_text())
        (d / "manifest.json").write_text(json.dumps({**m, "esp32s3": block}))
        return d

    assert fr.verify_dev_package(pkg("ok", good), SHA)["esp32s3"] is not None
    with pytest.raises(fr.ReleaseError, match="manifest NVS"):
        fr.verify_dev_package(pkg("nvs", {**good, "nvs": {"offset": 0x9000, "size": 0x6000}}), SHA)
    with pytest.raises(fr.ReleaseError, match="at 0"):
        fr.verify_dev_package(pkg("off", {**good, "image_offset": 0x10000}), SHA)
    d = package(tmp_path / "app", **{s3: merged()[0x10000:]})  # an app-only bin under that name
    m = json.loads((d / "manifest.json").read_text())
    (d / "manifest.json").write_text(json.dumps({**m, "esp32s3": good}))
    with pytest.raises(fr.ReleaseError, match="app-only"):
        fr.verify_dev_package(d, SHA)
