"""The shared labels of each track: one state per track that everyone edits together.

There are no users, drafts or saves. A page sends each edit as an operation as soon as it happens,
the server applies operations in order (each gets the next revision number), and every open page
pulls the operations it hasn't seen yet. Hits have stable ids, so two people editing the same track
change different hits and never overwrite each other; on the same hit, the later edit wins.

    {"op": "add",    "id": "...", "time": 1.2345, "type": "kick"}   (an existing id: move + retype)
    {"op": "move",   "id": "...", "time": 1.2361}
    {"op": "type",   "id": "...", "type": "snare"}
    {"op": "del",    "id": "..."}
    {"op": "review", "a": 0.0, "b": 6.0, "on": true}                 (mark a range reviewed or not)
    {"op": "reset"}                                                   (server only: reload the track)

State lives in <root>/<stem>.json, written atomically after every change. A track's state starts
from its human file (or its best seed) the first time anyone opens it. Python standard library only.
"""

from __future__ import annotations

import json
import math
import re
import threading
import time
from pathlib import Path
from typing import Any, Callable

import hosted
import labelcheck

ID_RE = re.compile(r"^[A-Za-z0-9_-]{1,40}$")
TAB_RE = re.compile(r"^[A-Za-z0-9_-]{4,40}$")
MAX_OPS_PER_REQUEST = 500
MAX_HITS = 50_000
KEEP_OPS = 2000  # recent operations kept for pulling; a page further behind reloads the track


class OpError(ValueError):
    pass


def _finite(x: Any) -> bool:
    return isinstance(x, (int, float)) and not isinstance(x, bool) and math.isfinite(x)


class TrackStore:
    """All tracks' shared state. `initial(stem)` builds a track's first state (from the corpus);
    `on_change(stem)` is called after every applied batch (local mode writes the file there)."""

    def __init__(self, root: Path, initial: Callable[[str], dict[str, Any]],
                 on_change: Callable[[str], None] | None = None, clock: Callable[[], float] = time.time):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self.initial, self.on_change, self.clock = initial, on_change, clock
        self._lock = threading.RLock()
        self._cache: dict[str, dict[str, Any]] = {}

    # -- storage
    def _path(self, stem: str) -> Path:
        return self.root / f"{stem}.json"

    def _load(self, stem: str) -> dict[str, Any]:
        st = self._cache.get(stem)
        if st is None:
            p = self._path(stem)
            st = json.loads(p.read_text()) if p.is_file() else self._fresh(stem, self.initial(stem))
            self._cache[stem] = st
        return st

    def _save(self, st: dict[str, Any]) -> None:
        hosted.atomic_write_text(self._path(st["stem"]), json.dumps(st, separators=(",", ":")))

    def _fresh(self, stem: str, init: dict[str, Any], rev: int = 0) -> dict[str, Any]:
        """A state from {"onsets": [{time, type}], "reviewed_ranges", "started_from", "seed",
        "audio_sha256", "duration", "updated"}. `base_rev` marks it as matching its source: only
        edits after it are worth publishing."""
        # Ids are unique per generation of the state (h1.. at first, r<rev>h1.. after a reset), so an
        # operation a page built from an older generation finds no hit and changes nothing.
        prefix = "h" if rev == 0 else f"r{rev}h"
        hits = {f"{prefix}{i + 1}": [round(float(o["time"]), 4), o["type"]]
                for i, o in enumerate(sorted(init["onsets"], key=lambda o: (o["time"], o["type"])))
                if o["type"] in labelcheck.LABEL_TYPES}
        return {"stem": stem, "rev": rev, "base_rev": rev, "hits": hits,
                "reviewed": labelcheck.merge_ranges(init.get("reviewed_ranges") or []),
                "started_from": init.get("started_from"), "seed": init.get("seed"),
                "audio_sha256": init["audio_sha256"], "duration": init.get("duration"),
                "updated": init.get("updated"), "edited_at": None, "ops": [],
                "base_blob": init.get("base_blob")}

    def stems(self) -> list[str]:
        with self._lock:
            return sorted({p.stem for p in self.root.glob("*.json")} | set(self._cache))

    def has(self, stem: str) -> bool:
        with self._lock:
            return stem in self._cache or self._path(stem).is_file()

    # -- reading
    def view(self, stem: str) -> dict[str, Any]:
        with self._lock:
            st = self._load(stem)
            return {"rev": st["rev"], "reviewed_ranges": st["reviewed"],
                    "onsets": sorted(({"id": i, "time": t, "type": ty} for i, (t, ty) in st["hits"].items()),
                                     key=lambda o: o["time"]),
                    "updated": st["updated"], "seed": st["seed"], "started_from": st["started_from"],
                    "edited": st["rev"] > st["base_rev"]}

    def since(self, stem: str, rev: int) -> dict[str, Any]:
        """The operations after `rev`, or {"reset": True} when the page is too far behind (or the
        track was reset): it reloads the track."""
        with self._lock:
            st = self._load(stem)
            if rev == st["rev"]:
                return {"rev": rev, "ops": []}
            ops = [o for o in st["ops"] if o[0] > rev]
            if rev > st["rev"] or not ops or ops[0][0] != rev + 1 or any(o[2]["op"] == "reset" for o in ops):
                return {"rev": st["rev"], "reset": True}
            return {"rev": st["rev"], "ops": ops}

    def edited_since_base(self, stem: str) -> bool:
        with self._lock:
            st = self._load(stem)
            return st["rev"] > st["base_rev"]

    def base_blob(self, stem: str) -> str | None:
        """The blob of main's file this state started from (None: it started from a seed)."""
        with self._lock:
            return self._load(stem).get("base_blob")

    def edited_at(self, stem: str) -> float | None:
        with self._lock:
            return self._load(stem).get("edited_at")

    # -- writing
    def apply(self, stem: str, ops: Any, tab: str, audio_sha256: Any, duration: Any,
              manifest_duration: float | None) -> int:
        if not isinstance(tab, str) or not TAB_RE.match(tab):
            raise OpError("bad tab id")
        if not isinstance(ops, list) or not 0 < len(ops) <= MAX_OPS_PER_REQUEST:
            raise OpError(f"ops must be a list of 1-{MAX_OPS_PER_REQUEST} operations")
        with self._lock:
            st = self._load(stem)
            if audio_sha256 != st["audio_sha256"]:
                raise OpError("the audio changed since this page loaded it: reload")
            dur = st["duration"]
            if dur is None and _finite(duration) and duration > 0:  # the decoded length, first seen
                if manifest_duration is not None and abs(manifest_duration - duration) > labelcheck.DURATION_TOL_S:
                    raise OpError(f"duration {duration:.3f}s disagrees with track_manifest.json")
                st["duration"] = dur = round(float(duration), 4)
            limit = (dur or manifest_duration or 3600.0) + labelcheck.END_TOL_S
            clean = [self._check(o, limit) for o in ops]  # all or nothing
            if len(st["hits"]) + sum(1 for o in clean if o["op"] == "add") > MAX_HITS:
                raise OpError("too many hits")
            now = self.clock()
            for op in clean:
                if self._apply_one(st, op):
                    st["rev"] += 1
                    st["ops"].append([st["rev"], tab, op])
            del st["ops"][:-KEEP_OPS]
            st["edited_at"] = now
            st["updated"] = hosted.now_iso()
            self._save(st)
            rev = st["rev"]
        if self.on_change:
            self.on_change(stem)
        return rev

    @staticmethod
    def _check(o: Any, limit: float) -> dict[str, Any]:
        if not isinstance(o, dict) or o.get("op") not in ("add", "move", "type", "del", "review"):
            raise OpError(f"bad operation {o!r}")
        kind = o["op"]
        if kind == "review":
            a, b = o.get("a"), o.get("b")
            if not (_finite(a) and _finite(b) and 0 <= a < b <= limit) or not isinstance(o.get("on"), bool):
                raise OpError(f"bad review range {o!r}")
            return {"op": kind, "a": round(float(a), 3), "b": round(float(b), 3), "on": o["on"]}
        if not isinstance(o.get("id"), str) or not ID_RE.match(o["id"]):
            raise OpError(f"bad hit id in {o!r}")
        out: dict[str, Any] = {"op": kind, "id": o["id"]}
        if kind in ("add", "move"):
            t = o.get("time")
            if not _finite(t) or not 0 <= t <= limit:
                raise OpError(f"hit time out of range in {o!r}")
            out["time"] = round(float(t), 4)
        if kind in ("add", "type"):
            if o.get("type") not in labelcheck.LABEL_TYPES:
                raise OpError(f"type must be kick or snare in {o!r}")
            out["type"] = o["type"]
        return out

    @staticmethod
    def _apply_one(st: dict[str, Any], op: dict[str, Any]) -> bool:
        """Apply one operation; False when it changes nothing (e.g. it names a hit someone deleted)."""
        hits, kind = st["hits"], op["op"]
        if kind == "review":
            before = st["reviewed"]
            if op["on"]:
                st["reviewed"] = labelcheck.merge_ranges(before + [[op["a"], op["b"]]])
            else:
                out = []
                for x, y in before:
                    if y <= op["a"] or x >= op["b"]:
                        out.append([x, y])
                        continue
                    if x < op["a"]:
                        out.append([x, op["a"]])
                    if y > op["b"]:
                        out.append([op["b"], y])
                st["reviewed"] = labelcheck.merge_ranges(out)
            return st["reviewed"] != before
        cur = hits.get(op["id"])
        if kind == "add":
            new = [op["time"], op["type"]]
        elif cur is None:
            return False
        elif kind == "del":
            del hits[op["id"]]
            return True
        elif kind == "move":
            new = [op["time"], cur[1]]
        else:
            new = [cur[0], op["type"]]
        if new == cur:
            return False
        hits[op["id"]] = new
        return True

    def reset(self, stem: str, init: dict[str, Any]) -> int:
        """Replace a track's state (its file changed on main from outside, e.g. a revert). Pages
        reload it."""
        with self._lock:
            old = self._load(stem)
            st = self._fresh(stem, init, rev=old["rev"] + 1)
            st["ops"] = [[st["rev"], "server", {"op": "reset"}]]
            self._cache[stem] = st
            self._save(st)
            return st["rev"]

    def import_doc(self, stem: str, init: dict[str, Any]) -> None:
        """Make `init` the track's state as an edit (it gets published), e.g. a migrated draft."""
        with self._lock:
            rev = self._load(stem)["rev"] + 1 if self.has(stem) else 1
            st = self._fresh(stem, init, rev=rev)
            st["base_rev"] = rev - 1  # an edit: it gets published
            st["edited_at"], st["updated"] = self.clock(), init.get("updated") or hosted.now_iso()
            if rev > 1:
                st["ops"] = [[rev, "server", {"op": "reset"}]]
            self._cache[stem] = st
            self._save(st)

    # -- the label file
    def doc(self, stem: str) -> dict[str, Any] | None:
        """The track's human label file (edm/drums_human/<stem>.drums.json), or None while its
        duration is unknown (no page has decoded it and the manifest doesn't list it)."""
        with self._lock:
            st = self._load(stem)
            dur = st["duration"]
            if dur is None:
                return None
            onsets = sorted({(t, ty) for t, ty in st["hits"].values()})
            ranges = [[round(a, 3), round(min(b, dur), 3)] for a, b in st["reviewed"] if min(b, dur) > a]
            ranges = labelcheck.merge_ranges(ranges)
            return {"schema": labelcheck.SCHEMA, "track": stem, "audio_sha256": st["audio_sha256"],
                    "duration": dur, "started_from": st["started_from"], "reviewed_ranges": ranges,
                    "complete": labelcheck.is_complete(ranges, dur),
                    "onsets": [{"time": t, "type": ty} for t, ty in onsets],
                    "updated": st["updated"] or hosted.now_iso()}

    def text(self, stem: str) -> str | None:
        d = self.doc(stem)
        return labelcheck.dumps_doc(d) if d else None
