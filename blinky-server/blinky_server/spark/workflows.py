"""Spark submissions as durable workflows (#325).

``separate`` (up to SEPARATE_BATCH_MAX tracks: hash, upload, submit each) and ``train`` (bundle,
upload, submit) used to run inside their HTTP request; a restart part-way through left the rest
undone until someone sent the request again. Now each runs as a workflow: one step per track,
one for the training submission. The ledger and the Job API's idempotency keys make every step
safe to repeat (rule 2 in work/durable.py).

``run`` is what the routes use. It keeps their synchronous API: the request waits for its
workflow's result. A request whose connection a restart cut can simply be sent again: while its
workflow is still pending (resuming after the restart), the same request attaches to it rather
than starting another.
"""

from __future__ import annotations

import asyncio
import hashlib
import json
import uuid
from collections.abc import Callable, Coroutine
from typing import Any

from dbos import DBOS

from ..work import durable
from . import service as spark
from .client import SparkError


def _svc() -> spark.SparkService:
    svc = spark.get_service()
    if svc is None:
        raise SparkError(spark.disabled_reason())
    return svc


@durable.step
async def _tracks(files: list[str]) -> list[tuple[str, str]]:
    """The tracks and their sha256, fixed for the whole job (hashing reads the files)."""
    svc = _svc()

    def hashed() -> list[tuple[str, str]]:
        return [(str(p), spark.sha256_file(p)) for p in (svc._audio_path(f) for f in files)]

    return await asyncio.to_thread(hashed)


@durable.step
async def _separate_one(
    path: str, sha: str, model_type: str, stems: list[str], chunk_seconds: int | None
) -> dict[str, Any]:
    return await asyncio.to_thread(_svc().separate_one, path, sha, model_type, stems, chunk_seconds)


@durable.workflow("spark.separate")
async def separate(
    files: list[str], model_type: str, stems: list[str], chunk_seconds: int | None
) -> list[dict[str, Any]]:
    out = []
    for path, sha in await _tracks(files):
        out.append(await _separate_one(path, sha, model_type, stems, chunk_seconds))
    return out


@durable.step
async def _train(req: dict[str, Any]) -> dict[str, Any]:
    svc = _svc()
    return await asyncio.to_thread(
        svc.train,
        req["config"],
        req["train_args"],
        req["prep_args"],
        req["audio_dir"],
        req["labels_dir"],
        req["augment"],
        req["memory_gb"],
    )


@durable.workflow("spark.train")
async def train(req: dict[str, Any]) -> dict[str, Any]:
    return await _train(req)


def request_key(kind: str, args: Any) -> str:
    """The same request, the same key (and so the same pending workflow to attach to)."""
    raw = json.dumps([kind, args], sort_keys=True, default=str).encode()
    return f"spark-{kind}-{hashlib.sha256(raw).hexdigest()[:16]}"


async def run(kind: str, fn: Callable[..., Coroutine[Any, Any, Any]], *args: Any) -> Any:
    """Run ``fn`` as a durable workflow and wait for its result, attaching to the same
    request's workflow if one is still pending (the caller retrying after a restart)."""
    if not durable.launched():
        raise durable.NotRunning("the durable work store isn't running")
    key = request_key(kind, args)
    pending = await DBOS.list_workflows_async(
        workflow_id_prefix=key,
        status=["ENQUEUED", "PENDING", "DELAYED"],
        load_input=False,
        load_output=False,
    )
    if pending:
        workflow_id = pending[0].workflow_id
    else:
        workflow_id = await durable.submit(fn, *args, workflow_id=f"{key}-{uuid.uuid4().hex[:8]}")
    handle: Any = await DBOS.retrieve_workflow_async(workflow_id)
    return await handle.get_result()
