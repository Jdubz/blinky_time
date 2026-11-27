"""The hardware checks (testing/hardware_checks.py): the stimulus, each phase's rules, the rig
errors that must never read as firmware failures, and the camera worker's protocol. The whole
job, killed mid-phase and resumed, is in test_durable_work.py."""

from __future__ import annotations

import itertools
import json
import sys
import wave
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from blinky_server.testing import hardware_checks as hc
from blinky_server.testing import stimulus

from . import hw_synth

# -- the stimulus ------------------------------------------------------------------------------


def test_the_stimulus_is_deterministic_and_written_once(tmp_path: Path) -> None:
    a = stimulus.ensure("response", tmp_path)
    b = stimulus.ensure("response", tmp_path / "again")
    assert a.name == b.name and a.read_bytes() == b.read_bytes()
    mtime = a.stat().st_mtime_ns
    assert stimulus.ensure("response", tmp_path) == a and a.stat().st_mtime_ns == mtime
    with wave.open(str(a)) as w:
        assert w.getframerate() == stimulus.SAMPLE_RATE and w.getnchannels() == 1
        assert abs(w.getnframes() / w.getframerate() - stimulus.RESPONSE_S) < 0.01
    noise = stimulus.ensure("noise", tmp_path)
    with wave.open(str(noise)) as w:
        assert abs(w.getnframes() / w.getframerate() - stimulus.NOISE_S) < 0.01


def test_the_sections_are_isolated_with_silence_between() -> None:
    secs = stimulus.RESPONSE_SECTIONS
    assert [s.kind for s in secs if s.kind != "silence"] == ["kick", "snare", "hat", "groove"]
    for prev, s in itertools.pairwise(secs):
        assert s.start == prev.end
        if s.kind != "silence":
            assert prev.kind == "silence"  # every section starts from silence
    kick, groove = secs[1], secs[7]
    assert len(stimulus.hit_times(kick)) == 16
    assert len(stimulus.hit_times(groove, "snare")) == len(stimulus.hit_times(groove)) // 2
    assert groove.seconds >= 20  # long enough for the tempo and the phase to lock


# -- 3. audio ----------------------------------------------------------------------------------


def _audio(**over: Any) -> list[dict[str, Any]]:
    silence = hw_synth.level(1_000.0, hc.SILENCE_S, over.pop("quiet", 0.002))
    noise = hw_synth.level(10_000.0, stimulus.NOISE_S, over.pop("loud", 0.08), over.pop("alive", 1))
    before = {"audioOverruns": 2, "audioSamplesLost": 100}
    after = {**before, **over}
    return hc.judge_audio("grid", silence, noise, before, after)


def test_the_mic_check_needs_a_clear_rise_and_no_lost_samples() -> None:
    mic, samples = _audio()
    assert mic["result"] == hc.YES and samples["result"] == hc.YES, (mic, samples)
    assert _audio(loud=0.005)[0]["result"] == hc.NO  # 2.5x: not a clear rise
    assert _audio(alive=0)[0]["result"] == hc.NO  # the PDM stopped
    assert _audio(audioSamplesLost=356)[1]["result"] == hc.NO
    assert _audio(audioOverruns=3)[1]["result"] == hc.NO


# -- 4. response -------------------------------------------------------------------------------


def _verdicts(cap: dict[str, Any]) -> dict[str, str]:
    checks, _meta = hc.judge_response("grid", cap)
    assert [c["name"] for c in checks] == [f.name for f in hc.FEATURES]
    return {c["name"]: c["result"] for c in checks}


def test_every_feature_responds_on_a_chip_that_hears_the_stimulus() -> None:
    got = _verdicts(hw_synth.response())
    assert set(got.values()) == {hc.YES}, got
    _checks, meta = hc.judge_response("grid", hw_synth.response(lag_ms=450))
    assert meta["lag_ms"] == 450 and meta["lag_source"] == "detections"


def test_a_deaf_chip_fails_every_feature_that_follows_the_hits() -> None:
    got = _verdicts(hw_synth.response(deaf=True))
    for name in ("onsets", "activation", "level", "energy", "flux", "centroid", "phase", "render"):
        assert got[name] == hc.NO, name


@pytest.mark.parametrize(
    ("flat", "feature"),
    [
        (("cent",), "centroid"),
        (("roll",), "rolloff"),
        (("act",), "render"),
        (("nn",), "activation"),
        (("e",), "energy"),
        (("pp",), "plp pulse"),
        (("hfc",), "hfc"),
    ],
)
def test_each_feature_is_judged_on_its_own(flat: tuple[str, ...], feature: str) -> None:
    got = _verdicts(hw_synth.response(flat=flat))
    assert got[feature] == hc.NO
    assert [n for n, r in got.items() if r == hc.NO] == [feature]


def test_tempo_allows_half_and_double_but_not_another_tempo() -> None:
    assert _verdicts(hw_synth.response(bpm=60.0))["tempo"] == hc.YES
    got = _verdicts(hw_synth.response(bpm=100.0))
    assert got["tempo"] == hc.NO and got["period"] == hc.NO


def test_detections_in_silence_alone_never_pass_the_onsets_rule() -> None:
    """The box's chip detects ~3.5/s in the room's silence: detections everywhere (as many
    between hits as at them) is no response."""
    cap = hw_synth.response(deaf=True)
    t0 = cap["start_ms"]
    cap["devices"]["grid"]["onsets"] = [t0 + i * 120.0 for i in range(500)]
    assert _verdicts(cap)["onsets"] == hc.NO


# -- 1. upload ---------------------------------------------------------------------------------


def test_a_deploy_failure_before_the_flash_is_a_rig_error() -> None:
    before = {"grid": "b220"}
    ok = {"status": "ok", "version": "b221-dev-x"}
    assert hc.judge_deploy(ok, before)[0] == hc.YES
    for step in ("targets", "release", "store"):
        dep = {"status": "error", "failed_step": step, "message": "build service down"}
        assert hc.judge_deploy(dep, before)[0] == "rig", step
    # The write never took: still the old version. The shape is run_deploy's own failed step
    # (the flash result under "detail"), not a hand-made one.
    from blinky_server.firmware import deploy_pipeline

    def flash_failed(version: str | None) -> dict[str, Any]:
        result = {
            "status": "error",
            "message": "0/1 devices flashed",
            "per_device": {"grid": {"flash": "error", "version": version}},
        }

        class Job:
            progress, progress_message = 0, ""

        async def flash(*a: Any) -> dict[str, Any]:
            return result

        async def targets(fleet: Any, ids: list[str]) -> list[str]:
            return ids

        async def release(build_id: str, d: Any) -> dict[str, Any]:
            return {"tag": "dev", "hex": "x.hex", "version": "b221-dev-x", "sha": "a"}

        async def store(fn: Any, rel: dict[str, Any]) -> str:
            return "x.hex"

        async def flash_t() -> float:
            return 0.0

        import asyncio

        mp = pytest.MonkeyPatch()
        mp.setattr(deploy_pipeline, "_staging_targets", targets)
        mp.setattr(deploy_pipeline, "_dev_build", release)
        mp.setattr(deploy_pipeline, "_store_only", store)
        mp.setattr(deploy_pipeline, "_flash_time", flash_t)
        try:
            dep = asyncio.run(
                deploy_pipeline.run_deploy(
                    None, Job(), release_tag=None, device_ids=["grid"], download_dir=Path("."),
                    store_firmware=lambda p, v: p, flash=flash, dev_build="b1",
                )
            )  # fmt: skip
        finally:
            mp.undo()
        assert dep["failed_step"] == "flash" and "detail" in dep["steps"]["flash"]
        return dep

    assert hc.judge_deploy(flash_failed("b220"), before)[0] == "rig"
    # It took and doesn't come back, or comes back wrong: the firmware's.
    assert hc.judge_deploy(flash_failed(None), before)[0] == hc.NO
    assert hc.judge_deploy(flash_failed("b221-dev-x"), before)[0] == hc.NO
    bad = {"status": "error", "failed_step": "verify", "message": "version=b220"}
    assert hc.judge_deploy(bad, before)[0] == hc.NO
    assert hc.judge_deploy({**bad, "failed_step": "settings"}, before)[0] == hc.NO


def test_json_info_sanity() -> None:
    board = hc.BOARDS["testbox_grid"]
    info = {
        "version": "b221",
        "platform": "nrf52840",
        "sn": "0123456789ABCDEF",
        "millis": 90_000,
        "audioOverruns": 0,
        "audioSamplesLost": 0,
        "fps": 112.0,
        "maxFrameMs": 15,
        "mallocIsr": 0,
        "device": {"leds": 64},
    }
    assert hc.sane(info, "0123456789ABCDEF", board) == []
    assert hc.sane({**info, "fps": 12.0}, "0123456789ABCDEF", board) == ["fps 12.0 < 30"]
    assert hc.sane({**info, "mallocIsr": 2}, "0123456789ABCDEF", board) == ["mallocIsr 2"]
    assert "no audioSamplesLost" in hc.sane({**info, "audioSamplesLost": None}, "x", board)
    assert hc.sane({**info, "device": {"leds": 89}}, "x", board) == ["89 LEDs, not 64"]
    assert hc.sane(info, "0000000000000001", board)[0].startswith("sn 0123")


# -- 2. lights ---------------------------------------------------------------------------------


class Chip:
    """A box chip answering the lights check's commands."""

    def __init__(self, brightness_reply: int | None = None) -> None:
        self.id = "grid"
        self.sent: list[str] = []
        self.brightness_reply = brightness_reply
        self.protocol = SimpleNamespace(send_command=self.send)

    async def send(self, cmd: str, timeout: float | None = None) -> str:
        self.sent.append(cmd)
        if cmd.startswith("leds "):
            b = cmd.split()[1]
            n = int(b) if b.isdigit() else 0
            if self.brightness_reply is not None:
                n = self.brightness_reply
            return json.dumps({"brightness": n, "config": 0, "override": b != "config"})
        if cmd.startswith("testpattern"):
            return f"OK {cmd}"
        return "{}"


class Cam:
    """camera_lights.py: every LED lights its own region, unless the brightness is 0 (dark),
    or ``misfit`` (light everywhere but in the regions)."""

    def __init__(self, chip: Chip, misfit: bool = False) -> None:
        self.chip, self.misfit = chip, misfit
        self.lit: dict[str, list[int]] = {}

    def dark(self) -> bool:
        levels = [c.split()[1] for c in self.chip.sent if c.startswith("leds ")]
        return levels[-1] == "0"

    async def ask(self, req: dict[str, Any]) -> dict[str, Any]:
        if req["op"] == "dark":
            return {"frame_mean": 27.8}
        if req["op"] == "lit":
            rise = 0.0 if self.dark() else 0.5
            if self.misfit:
                return {"lit": [], "frame_rise": rise}
            if "key" not in req:
                return {"lit": [] if self.dark() else list(range(64)), "frame_rise": rise}
            x, y = (int(v) for v in req["key"].split(","))
            self.lit[req["key"]] = [] if self.dark() else [y * 8 + x]
            return {"lit": self.lit[req["key"]], "frame_rise": rise}
        dead = [k for k, v in self.lit.items() if not v]
        return {"ok": not dead, "dead": dead, "unseen": [], "ambiguous": [], "shared": {}}

    async def close(self) -> None:
        pass


@pytest.fixture
def lights(monkeypatch: pytest.MonkeyPatch) -> Any:
    async def info(d: Any) -> dict[str, Any]:
        return {"device": {"width": 8, "height": 8}}

    monkeypatch.setattr(hc, "_info", info)

    def run(chip: Chip, brightness: int = 40, misfit: bool = False) -> list[dict[str, Any]]:
        import asyncio

        async def open_(regions: str) -> tuple[Cam, dict[str, Any]]:
            return Cam(chip, misfit), {"ready": True, "regions": 64}

        monkeypatch.setattr(hc.Camera, "open", open_)
        return asyncio.run(hc._lights_one(chip, "map.npz", brightness))

    return run


def test_the_lights_check_passes_when_every_led_lights(lights: Any) -> None:
    chip = Chip()
    got = lights(chip)
    assert [(c["name"], c["result"]) for c in got] == [("leds", hc.YES), ("walk", hc.YES)]
    assert sum(1 for c in chip.sent if c.startswith("testpattern pixel")) == 64


def test_the_lights_check_fails_with_the_leds_at_zero(lights: Any) -> None:
    got = lights(Chip(), brightness=0)
    walk = got[-1]
    assert walk["name"] == "walk" and walk["result"] == hc.NO
    assert "64 dead" in walk["evidence"]


def test_a_brightness_override_that_doesnt_apply_fails_the_leds_check(lights: Any) -> None:
    got = lights(Chip(brightness_reply=0))
    assert [(c["name"], c["result"]) for c in got] == [("leds", hc.NO)]


def test_a_region_map_that_doesnt_fit_is_a_rig_error(lights: Any) -> None:
    with pytest.raises(hc.RigError, match="doesn't fit"):
        lights(Chip(), misfit=True)


# -- the camera worker -------------------------------------------------------------------------


async def test_the_camera_worker_speaks_json_lines(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    script = tmp_path / "camera_lights.py"
    script.write_text(
        "import json, sys\n"
        "print(json.dumps({'ready': True, 'regions': 64}), flush=True)\n"
        "for line in sys.stdin:\n"
        "    req = json.loads(line)\n"
        "    if req['op'] == 'quit': break\n"
        "    out = {'error': 'boom'} if req['op'] == 'bad' else {'echo': req['op']}\n"
        "    print(json.dumps(out), flush=True)\n"
    )
    monkeypatch.setattr(hc, "_camera_script", lambda: script)
    monkeypatch.setattr(hc, "CAMERA_PYTHON", sys.executable)
    cam, ready = await hc.Camera.open("map.npz")
    assert ready["regions"] == 64
    assert await cam.ask({"op": "dark"}) == {"echo": "dark"}
    with pytest.raises(hc.RigError, match="boom"):
        await cam.ask({"op": "bad"})
    await cam.close()
    assert cam.proc.returncode == 0


async def test_a_camera_that_cant_open_is_a_rig_error(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    script = tmp_path / "camera_lights.py"
    script.write_text(
        "import json, sys\n"
        "print(json.dumps({'rig_error': 'camera: no region map at /x.npz'}), flush=True)\n"
        "sys.exit(3)\n"
    )
    monkeypatch.setattr(hc, "_camera_script", lambda: script)
    monkeypatch.setattr(hc, "CAMERA_PYTHON", sys.executable)
    with pytest.raises(hc.RigError, match="no region map"):
        await hc.Camera.open("/x.npz")
    monkeypatch.setattr(hc, "_camera_script", lambda: tmp_path / "missing.py")
    with pytest.raises(hc.RigError, match="missing"):
        await hc.Camera.open("/x.npz")


# -- 5. interfaces -----------------------------------------------------------------------------


def test_settings_replies_parse() -> None:
    assert hc._settings_value("hueshift = 0.250  [effect]", "hueshift") == 0.25
    assert hc._settings_value("Unknown command", "hueshift") is None


async def test_no_adapter_makes_the_ble_scan_not_applicable() -> None:
    got = await hc._ble_scan(SimpleNamespace(id="grid", ble_address="FB:3F"), None)
    assert got["result"] == hc.NA and "no Bluetooth adapter" in got["evidence"]


# -- the verdict -------------------------------------------------------------------------------


def test_the_run_stops_at_the_first_failing_phase_and_a_rig_error_is_no_verdict() -> None:
    report: dict[str, Any] = {
        "plan": list(hc.PHASES),
        "phases": [],
        "checks": [],
        "rig_error": None,
    }
    ok = {"checks": [hc.check("upload", "version", "g", hc.YES, "r", "e")], "seconds": 1.0}
    assert hc._record_phase(report, "upload", ok) is True
    no = {"checks": [hc.check("lights", "walk", "g", hc.NO, "r", "64 dead")], "seconds": 2.0}
    assert hc._record_phase(report, "lights", no) is False
    assert hc.verdict(report) is False and report["rig_error"] is None
    rig: dict[str, Any] = {"plan": ["lights"], "phases": [], "checks": [], "rig_error": None}
    assert hc._record_phase(rig, "lights", {"checks": [], "rig_error": "camera: busy"}) is False
    assert rig["rig_error"] == "lights: camera: busy" and hc.verdict(rig) is False
    na = {"checks": [hc.check("interfaces", "ble scan", "g", hc.NA, "r", "e")]}
    full: dict[str, Any] = {"plan": ["interfaces"], "phases": [], "checks": [], "rig_error": None}
    assert hc._record_phase(full, "interfaces", na) is True and hc.verdict(full) is True


def test_every_board_names_a_known_platform() -> None:
    for name, board in hc.BOARDS.items():
        assert board.platform in ("nrf52840", "esp32s3"), name
        if board.regions:
            assert board.regions.endswith(".npz")


# -- a chip that stops answering is the firmware's, never a rig error --------------------------


async def test_a_chip_that_stops_answering_mid_phase_is_a_no_not_a_rig_error() -> None:
    async def times_out() -> dict[str, Any]:
        raise TimeoutError("json info: no reply in 20 s")

    got = await hc._guard("audio", ["grid", "other"], times_out())
    assert got["rig_error"] is None
    assert [(c["name"], c["device"], c["result"]) for c in got["checks"]] == [
        ("chip answers", "grid", hc.NO),
        ("chip answers", "other", hc.NO),
    ]
    assert "TimeoutError: json info: no reply" in got["checks"][0]["evidence"]

    async def camera_gone() -> dict[str, Any]:
        raise hc.RigError("camera: the worker went away")

    got = await hc._guard("lights", ["grid"], camera_gone())
    assert got["rig_error"] == "camera: the worker went away" and got["checks"] == []


async def test_a_chip_that_disconnected_is_never_dropped_silently(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    async def connected(ids: list[str]) -> list[Any]:
        return [SimpleNamespace(id="grid")]  # "other" crashed and is gone

    monkeypatch.setattr(hc, "_connected", connected)
    assert [d.id for d in await hc._chips(["grid"])] == ["grid"]
    with pytest.raises(hc.ChipGone, match="other"):
        await hc._chips(["grid", "other"])

    async def body() -> dict[str, Any]:
        await hc._chips(["grid", "other"])
        return {"checks": []}

    got = await hc._guard("response", ["grid", "other"], body())
    assert {c["result"] for c in got["checks"]} == {hc.NO} and got["rig_error"] is None
    assert "not connected: other" in got["checks"][0]["evidence"]


async def test_an_enrolled_chip_is_checked_as_its_registry_entry(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """A renamed entry: the grid's chip still reports its old id until a reboot applies the new
    config (maybe this job's flash), so its board is the entry the box keeps it on."""
    (tmp_path / "box_chips.json").write_text(json.dumps({"0123456789ABCDEF": "testbox_grid"}))
    monkeypatch.setattr(hc.box_config, "CONFIG_DIR", tmp_path)

    async def connected(ids: list[str]) -> list[Any]:
        return [
            SimpleNamespace(id="0123456789ABCDEF", hardware_sn=None, device_type="testbox_grid_v1"),
            SimpleNamespace(id="other", hardware_sn="OTHER", device_type="bench_chip"),
        ]

    monkeypatch.setattr(hc, "_connected", connected)
    got = await hc.boards.__wrapped__(["0123456789ABCDEF", "other"])  # type: ignore[attr-defined]
    assert got == {"0123456789ABCDEF": "testbox_grid", "other": "bench_chip"}
    assert got["0123456789ABCDEF"] in hc.BOARDS
