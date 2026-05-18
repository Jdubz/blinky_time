"""Orchestrator-context guard for the write implementations.

The lockdown invariant (see ``docs/FLASH_LOCKDOWN_PLAN.md`` §L2) is
that exactly one code path produces flashes: ``FleetManager.flash_device()``
→ ``_run_flash_job`` → either ``_uf2_write_impl_for_job`` or
``_ble_dfu_write_impl``. Anything else that calls those write impls
directly is bypassing the per-device lock, the canonical-key resolver,
the dedup window, and the broadcaster guard — exactly the parallel-
path-existing-in-the-first-place that caused the 2026-05-17 cascade.

This module is the boundary. ``_run_flash_job`` sets the ContextVar
to ``True`` before invoking either impl; each impl's first line asks
``inside_orchestrator()`` and raises ``OutsideFlashJobContextError``
on False. Tests can prove the invariant by importing an impl directly,
calling it, and asserting the exception.

As of L3d, there are NO legacy wrappers around the impls anymore.
The only function in the codebase that calls ``enter_orchestrator_context``
is ``_run_flash_job`` itself. Any future caller that wants to write
firmware MUST route through ``FleetManager.flash_device()``.

ContextVar (vs. plain global) is the right primitive: asyncio task
isolation means concurrent flashes on different devices each get
their own bool, and a finally block on the orchestrator cleanly
restores the prior value (False or — if the orchestrator was called
recursively for some reason — the outer True).
"""

from __future__ import annotations

from contextvars import ContextVar, Token

# True iff we're inside _run_flash_job → _run_*_flash → _impl. The
# orchestrator sets this; the impls assert it. Default False so a
# stray import-and-call from a test or script raises immediately.
_inside_flash_job_orchestrator: ContextVar[bool] = ContextVar(
    "_inside_flash_job_orchestrator",
    default=False,
)


class OutsideFlashJobContextError(RuntimeError):
    """Raised when a write impl is invoked outside ``_run_flash_job``.

    Message points the caller at the canonical entry point. If you see
    this from production code, you've reached a write impl through a
    back door — the impl was renamed _-prefixed deliberately so that
    only the orchestrator (which sets the ContextVar) can call it.
    Route the call through ``FleetManager.flash_device()`` instead.

    Tests routinely raise this to prove the invariant; they're the
    only legitimate non-orchestrator callers.
    """


def assert_inside_orchestrator(impl_name: str) -> None:
    """Raise ``OutsideFlashJobContextError`` if the orchestrator
    ContextVar isn't set.

    Called at the entry of every write impl. ``impl_name`` goes into
    the error message so the caller knows which function they hit;
    when multiple impls are stacked (e.g. ``_uf2_write_impl_for_job``
    delegates internally), the most-specific name is useful.
    """
    if not _inside_flash_job_orchestrator.get():
        raise OutsideFlashJobContextError(
            f"{impl_name} called outside _run_flash_job — every flash "
            "must route through FleetManager.flash_device(). "
            "Direct calls to write impls are forbidden by the lockdown "
            "(see docs/FLASH_LOCKDOWN_PLAN.md §L2)."
        )


def enter_orchestrator_context() -> Token[bool]:
    """Set the ContextVar to True; return a token to reset with.

    Use::

        token = enter_orchestrator_context()
        try:
            await self._run_uf2_flash(job)
        finally:
            reset_orchestrator_context(token)

    The token-based reset (rather than ``set(False)``) means recursive
    orchestrator entries restore the prior True, not False — defensive
    against future code that might compose orchestrator calls.
    """
    return _inside_flash_job_orchestrator.set(True)


def reset_orchestrator_context(token: Token[bool]) -> None:
    """Restore the ContextVar to whatever it was before
    ``enter_orchestrator_context()`` was called. Pass the token
    that ``enter_orchestrator_context()`` returned."""
    _inside_flash_job_orchestrator.reset(token)
