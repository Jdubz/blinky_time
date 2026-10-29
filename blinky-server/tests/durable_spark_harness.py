"""Runs the real ``spark.separate`` workflow against a fake Spark service in its own process, so
test_durable_spark.py can kill it for real and start it again (#325).

    python -m tests.durable_spark_harness start  OUT KILL   # none | crash:N
    python -m tests.durable_spark_harness resume OUT

Every submission is appended to OUT/effects.log; ``crash:N`` exits in the middle of the Nth.
"""

from __future__ import annotations

import asyncio
import json
import os
import sys
from pathlib import Path
from types import SimpleNamespace
from typing import Any

from blinky_server.paths import data_dir
from blinky_server.spark import service as spark
from blinky_server.spark import workflows
from blinky_server.work import durable

TRACKS = [f"/music/{t}.mp3" for t in ("a", "b", "c", "d")]

mode, out = sys.argv[1], Path(sys.argv[2])
kill = sys.argv[3] if len(sys.argv) > 3 else "none"
effects = out / "effects.log"
submits = 0


def effect(line: str) -> None:
    with open(effects, "a") as f:
        f.write(line + "\n")


def separate_one(path: str, sha: str, model_type: str, stems: list[str], chunk: Any) -> dict:
    global submits
    submits += 1
    effect(f"submit {Path(path).name} start")
    if kill == f"crash:{submits}":
        os._exit(9)
    effect(f"submit {Path(path).name} end")
    return {"path": path, "action": "submitted", "job_id": f"job-{sha}"}


fake = SimpleNamespace(_audio_path=lambda f: Path(f), separate_one=separate_one)
workflows._svc = lambda: fake  # type: ignore[assignment]
spark.sha256_file = lambda p: Path(p).stem  # type: ignore[assignment]


async def main() -> None:
    durable.configure(data_dir())
    await durable.launch()
    # Both modes send the same request: on ``resume`` that's the client sending it again
    # after the restart, and it attaches to the workflow DBOS has just recovered.
    args = (TRACKS, "htdemucs", ["drums"], None)
    print(json.dumps(await workflows.run("separate", workflows.separate, *args)))
    await durable.shutdown()


asyncio.run(main())
