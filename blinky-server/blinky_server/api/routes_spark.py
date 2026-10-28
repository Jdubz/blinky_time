"""Spark Job API routes (#288, #305): submit `separate` and training jobs, read the ledger,
receive webhooks."""

from __future__ import annotations

import asyncio
import hmac
import logging
from typing import Any

from fastapi import APIRouter, Depends, Header, HTTPException
from pydantic import BaseModel, Field

from ..spark import service as spark
from ..spark.client import SparkError
from .deps import require_api_key

log = logging.getLogger(__name__)

router = APIRouter(prefix="/spark", tags=["spark"])


def _svc() -> spark.SparkService:
    svc = spark.get_service()
    if svc is None:
        raise HTTPException(503, spark.disabled_reason())
    return svc


class SeparateRequest(BaseModel):
    files: list[str] = Field(..., min_length=1)
    model_type: str = "htdemucs"
    stems: list[str] = Field(default_factory=lambda: ["drums"])
    chunk_seconds: int | None = None


@router.post("/separate", dependencies=[Depends(require_api_key)])
async def separate(req: SeparateRequest) -> dict[str, Any]:
    svc = _svc()
    try:
        results = await asyncio.to_thread(
            svc.separate, req.files, req.model_type, req.stems, req.chunk_seconds
        )
    except SparkError as e:
        raise HTTPException(400, str(e)) from None
    return {"results": results}


class TrainRequest(BaseModel):
    config: str = Field(..., description="config path inside ml-training/, e.g. configs/x.yaml")
    train_args: list[str] = Field(default_factory=list)
    prep_args: list[str] = Field(default_factory=list)
    audio_dir: str | None = "audio"
    labels_dir: str | None = None
    augment: bool = True
    memory_gb: float = Field(spark.TRAIN_MEMORY_GB, gt=0)


@router.post("/train", dependencies=[Depends(require_api_key)])
async def train(req: TrainRequest) -> dict[str, Any]:
    svc = _svc()
    try:
        return await asyncio.to_thread(
            svc.train,
            req.config,
            req.train_args,
            req.prep_args,
            req.audio_dir,
            req.labels_dir,
            req.augment,
            req.memory_gb,
        )
    except SparkError as e:
        raise HTTPException(400, str(e)) from None


@router.get("/jobs")
async def jobs() -> dict[str, Any]:
    return await asyncio.to_thread(_svc().summary)


@router.post("/reconcile", dependencies=[Depends(require_api_key)])
async def reconcile() -> dict[str, Any]:
    svc = _svc()
    try:
        return {"counts": await asyncio.to_thread(svc.reconcile)}
    except SparkError as e:
        raise HTTPException(502, str(e)) from None


@router.post("/webhook")
async def webhook(
    body: dict[str, Any],
    authorization: str | None = Header(None),
    x_spark_delivery: str | None = Header(None),
) -> dict[str, str]:
    """The Spark's job-transition webhook (my-spark docs/architecture/webhooks.md). Authenticated
    by the shared secret, not blinky's API key; must answer 2xx within ~10 s."""
    svc = _svc()
    if svc.webhook_secret is None:
        raise HTTPException(503, "SPARK_WEBHOOK_SECRET is not set; webhooks are off")
    expected = f"Bearer {svc.webhook_secret}"
    if not authorization or not hmac.compare_digest(authorization, expected):
        raise HTTPException(401, "invalid webhook secret")
    outcome = await asyncio.to_thread(svc.on_webhook, body)
    log.info("spark webhook %s: %s", x_spark_delivery, outcome)
    return {"outcome": outcome}
