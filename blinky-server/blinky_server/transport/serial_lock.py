"""Serial port lock — re-exports from the canonical tools/serial_lock.py.

The canonical implementation lives in tools/serial_lock.py (used by
uf2_upload.py and other standalone tools). This module re-exports it
so blinky-server code can import via the package namespace.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

# Find tools/serial_lock.py relative to the project root
_tools_lock = Path(__file__).resolve().parents[3] / "tools" / "serial_lock.py"
if not _tools_lock.exists():
    raise ImportError(f"Cannot find canonical serial_lock.py at {_tools_lock}")

_spec = importlib.util.spec_from_file_location("_serial_lock_impl", str(_tools_lock))
if _spec is None or _spec.loader is None:
    raise ImportError(f"Cannot load serial_lock.py from {_tools_lock}")
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)

# Re-export public API
acquire = _mod.acquire
release = _mod.release
force_release = _mod.force_release
is_locked = _mod.is_locked
LOCK_DIR = _mod.LOCK_DIR
