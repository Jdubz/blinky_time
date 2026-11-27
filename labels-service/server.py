#!/usr/bin/env python3
"""Drum-hit ground-truth reviewer: hand-curate kick/snare labels for the edm/ corpus.

Serves one page (index.html, no external fetches) and a small JSON API. All audio analysis
(band envelopes, onset strength, spectrogram) runs in the browser. Each track has ONE shared set of
labels that everyone edits together (state.py): the page sends every edit as it happens and pulls
everyone else's. There are no users, drafts or saves.

Local mode (a corpus clone):

    python labels-service/server.py [--port 8766] [--host 127.0.0.1] [--edm edm/]

    Every change is written to edm/drums_human/<stem>.drums.json at once (the shared state lives
    in edm/drums_human/.state/, gitignored).

Hosted mode (a public URL; see README.md "Hosted"):

    python labels-service/server.py --host 0.0.0.0 \\
        --state /state --checkout /corpus --git-url https://github.com/<owner>/blinky-corpus.git \\
        --github-repo <owner>/blinky-corpus --allow-host <public name> --trust-cf-connecting-ip

    The corpus is the service's own clone, refreshed from GitHub. The shared state and logs live in
    --state. The service holds no credential: blinky-corpus's scheduled workflow labels-sync.yml
    pulls the changed tracks from GET /api/export and publishes them as an auto-merging batch
    (hosted.Publication).

Files (paths relative to edm/):
    <stem>.mp3                          the audio
    drums_human/<stem>.drums.json       human ground truth
    <seed dir>/<stem><suffix>           seed labels, in the precedence of seeds.json (read-only)
    drums_suspect/<stem>.json           the suspect map (scripts/drum_suspect.py): which bars to check
                                        first and the hits their pattern expects (hints, read-only)

A track's shared state starts from its human file, else the first seeds.json source that has it.
Python standard library only.
"""

from __future__ import annotations

import argparse
import hashlib
import ipaddress
import json
import os
import re
import socket
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any, NamedTuple
from urllib.parse import parse_qs, unquote, urlparse

STATIC_DIR = Path(__file__).resolve().parent
if str(STATIC_DIR) not in sys.path:  # labelcheck.py and hosted.py sit next to this file
    sys.path.insert(0, str(STATIC_DIR))

import hosted  # noqa: E402
import labelcheck  # noqa: E402
import state  # noqa: E402
from labelcheck import (  # noqa: E402
    DURATION_TOL_S, END_TOL_S, LABEL_TYPES, MAX_ONSETS, MAX_RANGES, SCHEMA, STEM_RE, dumps_doc,
    f_measure, file_sha256, is_complete, merge_ranges,
)

SUSPECT_DIR = "drums_suspect"
SUSPECT_TIERS = ("high", "medium")  # the page shows these; "low" stays hidden
DEFAULT_EDM = STATIC_DIR.parent.parent / "edm"  # a corpus clone; /edm in the image (unused there)
MAX_POST_BYTES = 2 * 1024 * 1024  # a 1000-hit track is ~50 KB
# The service's version: the git tree hash of labels-service/ (the image's build arg). The site
# stamps the same hash into the page, which says "reload" when the two differ.
VERSION = os.environ.get("LABELS_SERVICE_VERSION") or "dev"


class ApiError(Exception):
    def __init__(self, status: int, message: str, **extra: Any):
        super().__init__(message)
        self.status = status
        self.message = message
        self.extra = extra


# --- Corpus layout ----------------------------------------------------------


class Corpus:
    def __init__(self, edm: Path, seeds_file: Path | None = None):
        self.edm = Path(edm).resolve()
        self.human_dir = self.edm / labelcheck.HUMAN_DIR
        self._seeds_file = seeds_file
        self._seeds: tuple[float, tuple[list[labelcheck.SeedSource], list[labelcheck.SeedSource]]] | None = None
        self._sha_cache: dict[str, tuple[float, int, str]] = {}
        self._suspect_cache: dict[str, tuple[float, int, dict[str, Any] | str]] = {}
        self._lock = threading.Lock()

    def seeds_file(self) -> Path:
        """The corpus's own seeds.json when it has one (a new seed source takes effect when it's
        merged), else the one next to this file."""
        if self._seeds_file is not None:
            return self._seeds_file
        own = self.edm.parent / "tools" / "labels" / "seeds.json"
        return own if own.is_file() else labelcheck.DEFAULT_SEEDS_FILE

    def _sources(self) -> tuple[list[labelcheck.SeedSource], list[labelcheck.SeedSource]]:
        f = self.seeds_file()
        mtime = f.stat().st_mtime
        with self._lock:
            if self._seeds is None or self._seeds[0] != mtime:
                self._seeds = (mtime, labelcheck.load_sources(f))
            return self._seeds[1]

    def seeds(self) -> list[labelcheck.SeedSource]:
        return self._sources()[0]

    def references(self) -> list[labelcheck.SeedSource]:
        """Reference-only label sets (seeds.json "references"): ghost ticks, never a seed."""
        return self._sources()[1]

    # Every path built from a request goes through validate_stem() first, and the
    # result is checked to resolve inside its directory (belt and braces).
    def validate_stem(self, stem: Any) -> str:
        if not isinstance(stem, str) or not STEM_RE.match(stem) or ".." in stem:
            raise ApiError(400, f"invalid track name: {stem!r}")
        if not (self.edm / f"{stem}.mp3").is_file():
            raise ApiError(404, f"no such track: {stem}")
        return stem

    def _inside(self, path: Path, parent: Path) -> Path:
        if path.parent.resolve() != parent.resolve() or Path(path.name).name != path.name:
            raise ApiError(400, "path escapes its directory")
        return path

    def audio_path(self, stem: str) -> Path:
        return self._inside(self.edm / f"{stem}.mp3", self.edm)

    def human_path(self, stem: str) -> Path:
        return self._inside(self.human_dir / f"{stem}{labelcheck.HUMAN_SUFFIX}", self.human_dir)

    def seed_path(self, seed: labelcheck.SeedSource, stem: str) -> Path:
        return self._inside(seed.path(self.edm, stem), self.edm / seed.dir)

    def suspect_path(self, stem: str) -> Path:
        return self._inside(self.edm / SUSPECT_DIR / f"{stem}.json", self.edm / SUSPECT_DIR)

    def suspect(self, stem: str) -> dict[str, Any] | str | None:
        """The track's suspect map as the page uses it (suspect_public), None when it has none, or
        the reason it's unreadable. Cached until the file changes (the track list reads all 18)."""
        path = self.suspect_path(stem)
        try:
            st = path.stat()
        except FileNotFoundError:
            return None
        with self._lock:
            hit = self._suspect_cache.get(stem)
            if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
                return hit[2]
        try:
            out: dict[str, Any] | str = suspect_public(json.loads(path.read_text()))
        except (ValueError, TypeError, KeyError) as e:
            out = f"{path.name}: {e}"[:300]
        with self._lock:
            self._suspect_cache[stem] = (st.st_mtime, st.st_size, out)
        return out

    def stems(self) -> list[str]:
        return sorted(p.stem for p in self.edm.glob("*.mp3") if STEM_RE.match(p.stem))

    def audio_sha256(self, stem: str) -> str:
        path = self.audio_path(stem)
        st = path.stat()
        with self._lock:
            hit = self._sha_cache.get(stem)
            if hit and hit[0] == st.st_mtime and hit[1] == st.st_size:
                return hit[2]
        sha = file_sha256(path)
        with self._lock:
            self._sha_cache[stem] = (st.st_mtime, st.st_size, sha)
        return sha

    def manifest_duration(self, stem: str) -> float | None:
        return labelcheck.manifest_duration(self.edm, stem)


def now_iso() -> str:
    return hosted.now_iso()


def read_json(path: Path) -> dict[str, Any]:
    try:
        doc = json.loads(path.read_text())
    except json.JSONDecodeError as e:
        raise ApiError(500, f"{path.name} is not valid JSON: {e}") from e
    if not isinstance(doc, dict):
        raise ApiError(500, f"{path.name}: expected a JSON object")
    return doc


# --- Labels -----------------------------------------------------------------


def split_auto_labels(doc: dict[str, Any], name: str) -> dict[str, Any]:
    try:
        return labelcheck.split_auto_labels(doc, name)
    except ValueError as e:
        raise ApiError(500, str(e)) from e


def provenance(path: Path, source: str, doc: dict[str, Any], rel_root: Path) -> dict[str, Any]:
    gen = doc.get("generator")
    if gen is None and "source" in doc:  # kick_weighted files name their detector here
        gen = {"source": doc["source"], "bands_hz": doc.get("bands_hz")}
    try:
        rel = str(path.relative_to(rel_root))
    except ValueError:
        rel = path.name
    return {"source": source, "file": rel, "sha256": file_sha256(path), "generator": gen}


def covered(ranges: list[list[float]], duration: float | None) -> float:
    total = sum(b - a for a, b in merge_ranges(ranges))
    if not duration:
        return total
    return min(total, duration)


def human_onsets(doc: dict[str, Any], name: str) -> list[dict[str, Any]]:
    onsets = doc.get("onsets")
    if not isinstance(onsets, list):
        raise ApiError(500, f"{name}: no 'onsets' list")
    for o in onsets:
        if not isinstance(o, dict) or o.get("type") not in LABEL_TYPES or not isinstance(
            o.get("time"), (int, float)
        ):
            raise ApiError(500, f"{name}: malformed onset {o!r}")
    return onsets


class SeedDoc(NamedTuple):
    seed: labelcheck.SeedSource
    path: Path
    doc: dict[str, Any]
    labels: dict[str, Any]


def seed_docs(c: Corpus, stem: str) -> tuple[list[SeedDoc], list[str]]:
    """The seed files of a track that exist and aren't skipped, best first; plus the names of
    those marked skipped."""
    out, skipped = [], []
    for s in c.seeds():
        p = c.seed_path(s, stem)
        if not p.is_file():
            continue
        doc = read_json(p)
        if doc.get("skipped"):
            skipped.append(s.name)
            continue
        out.append(SeedDoc(s, p, doc, split_auto_labels(doc, p.name)))
    return out, skipped


# --- The suspect map (hints, never labels) ------------------------------------


def _num(x: Any, what: str) -> float:
    if isinstance(x, bool) or not isinstance(x, (int, float)) or x != x or x in (float("inf"), float("-inf")):
        raise ValueError(f"{what} is not a number")
    return float(x)


def suspect_public(doc: Any) -> dict[str, Any]:
    """What the page needs from edm/drums_suspect/<stem>.json (blinky-corpus scripts/drum_suspect.py):
    the high and medium bars with their evidence (the suspect lane, Next suspect), and every bar's
    expected-but-missing hits (schema 2; the hollow markers). A schema-1 map has no expected hits.
    Raises ValueError on a malformed map."""
    if not isinstance(doc, dict) or not isinstance(doc.get("bars"), list):
        raise ValueError("no bars")
    schema = doc.get("schema")
    if not isinstance(schema, int) or schema < 1:
        raise ValueError(f"bad schema {schema!r}")
    bars, expected = [], []
    for b in doc["bars"]:
        if not isinstance(b, dict):
            raise ValueError("a bar is not an object")
        for e in b.get("expected") or []:
            if not isinstance(e, dict) or e.get("type") not in LABEL_TYPES:
                raise ValueError(f"bad expected hit {e!r:.80}")
            expected.append({"time": round(_num(e["time"], "an expected hit's time"), 4), "type": e["type"],
                             "p": round(_num(e.get("p", 0), "p"), 2), "at": "attack" if e.get("at") == "attack" else "grid"})
        if b.get("tier") not in SUSPECT_TIERS:
            continue
        row = {"start": round(_num(b["start"], "start"), 4), "end": round(_num(b["end"], "end"), 4),
               "tier": b["tier"], "rank": b.get("rank") if isinstance(b.get("rank"), int) else None,
               "filter": bool(b.get("filter"))}
        for k in ("missing_kick", "missing_snare", "disagree", "unexpected_kick", "unexpected_snare"):
            v = b.get(k, 0)
            row[k] = v if isinstance(v, int) and not isinstance(v, bool) and v >= 0 else 0
        if row["end"] > row["start"]:
            bars.append(row)
    bars.sort(key=lambda r: r["start"])
    expected.sort(key=lambda e: e["time"])
    return {"schema": schema, "bars": bars, "expected": expected}


def suspect_left(sus: dict[str, Any], ranges: list[list[float]]) -> tuple[float, float | None]:
    """The high and medium bars' seconds, and the share of them not yet marked reviewed."""
    total = left = 0.0
    merged = merge_ranges(ranges) if ranges else []
    for b in sus["bars"]:
        a, z = b["start"], b["end"]
        total += z - a
        left += (z - a) - sum(max(0.0, min(z, y) - max(a, x)) for x, y in merged)
    return round(total, 3), (round(left / total, 4) if total > 0 else None)


# --- Track list and seeding ---------------------------------------------------


def track_summary(c: Corpus, stem: str, human_override: dict[str, Any] | None = None) -> dict[str, Any]:
    hp = c.human_path(stem)
    human = human_override if human_override is not None else (
        read_json(hp) if hp.is_file() else None)
    seeds, _ = seed_docs(c, stem)

    duration = None
    if human and isinstance(human.get("duration"), (int, float)):
        duration = float(human["duration"])
    if duration is None:
        duration = c.manifest_duration(stem)

    if human is not None:
        ons = human_onsets(human, hp.name)
        counts = {t: sum(1 for o in ons if o["type"] == t) for t in LABEL_TYPES}
        label_source = "human"
        ranges = human.get("reviewed_ranges") or []
    elif seeds:
        counts = {t: len(seeds[0].labels[t]) for t in LABEL_TYPES}
        label_source, ranges = seeds[0].seed.name, []
    else:
        counts = {t: 0 for t in LABEL_TYPES}
        label_source, ranges = "none", []

    # Ranking hint: how much the two best seeds disagree, per class.
    disagreement = None
    if len(seeds) >= 2:
        a, b = seeds[0].labels, seeds[1].labels
        fs = [f for f in (f_measure(a[t], b[t]) for t in LABEL_TYPES) if f is not None]
        disagreement = round(1 - sum(fs) / len(fs), 3) if fs else None

    reviewed_s = covered(ranges, duration)
    sus = c.suspect(stem)
    sus_s, sus_left = suspect_left(sus, ranges) if isinstance(sus, dict) else (None, None)
    return {
        "stem": stem,
        "duration": duration,
        "label_source": label_source,
        "counts": counts,
        "has_human": human is not None,
        "has_adtof": any(s.seed.name == "drums_adtof" for s in seeds),
        "complete": bool(human and human.get("complete")),
        "reviewed_s": round(reviewed_s, 3),
        "reviewed_share": round(reviewed_s / duration, 4) if duration else None,
        "disagreement": disagreement,
        "disagreement_between": [s.seed.name for s in seeds[:2]] if len(seeds) >= 2 else None,
        "updated": human.get("updated") if human else None,
        "suspect_s": sus_s,
        "suspect_left": sus_left,
    }


def list_tracks(c: Corpus) -> list[dict[str, Any]]:
    return [track_summary(c, s) for s in c.stems()]


def seeds_public(c: Corpus) -> list[dict[str, Any]]:
    return ([{"name": s.name, "label": s.label, "warning": s.warning} for s in c.seeds()]
            + [{"name": s.name, "label": s.label, "warning": s.warning, "reference": True}
               for s in c.references()])


def load_track(c: Corpus, stem: str, human_override: dict[str, Any] | None = None) -> dict[str, Any]:
    """Everything the page needs to open a track, seeded human > the seeds.json sources."""
    stem = c.validate_stem(stem)
    hp = c.human_path(stem)
    sha = c.audio_sha256(stem)
    seeds, skipped = seed_docs(c, stem)
    refs: dict[str, Any] = {s.seed.name: s.labels for s in seeds}
    for r in c.references():  # reference-only sets: offered as ghost ticks, never seeded from
        rp = c.seed_path(r, stem)
        if rp.is_file():
            rdoc = read_json(rp)
            if not rdoc.get("skipped"):
                refs[r.name] = split_auto_labels(rdoc, rp.name)

    out: dict[str, Any] = {
        "stem": stem,
        "audio_url": f"/audio/{stem}.mp3",
        "audio_sha256": sha,
        "manifest_duration": c.manifest_duration(stem),
        "references": refs,
        "warning": None,
        "error": None,
        "reviewed_ranges": [],
        "notes": [],
        "complete": False,
        "updated": None,
    }
    human = human_override if human_override is not None else (
        read_json(hp) if hp.is_file() else None)
    if human is not None:
        doc = human
        out["seed"] = "human"
        out["started_from"] = doc.get("started_from")
        out["onsets"] = [
            {"time": float(o["time"]), "type": o["type"]} for o in human_onsets(doc, hp.name)
        ]
        out["reviewed_ranges"] = merge_ranges(doc.get("reviewed_ranges") or [])
        out["notes"] = [n for n in doc.get(labelcheck.NOTES_KEY) or [] if isinstance(n, dict)]
        out["complete"] = bool(doc.get("complete"))
        out["updated"] = doc.get("updated")
        if doc.get("audio_sha256") != sha:
            out["error"] = (
                f"{hp.name} was made against different audio (audio_sha256 "
                f"{str(doc.get('audio_sha256'))[:12]}... vs the mp3's {sha[:12]}...). "
                "Editing is refused until this is resolved by hand."
            )
    elif seeds:
        best = seeds[0]
        out["seed"] = best.seed.name
        out["started_from"] = provenance(best.path, best.seed.name, best.doc, c.edm.parent)
        out["onsets"] = [{"time": t, "type": k} for k in LABEL_TYPES for t in best.labels[k]]
        skipped_better = [n for n in skipped
                          if [s.name for s in c.seeds()].index(n)
                          < [s.name for s in c.seeds()].index(best.seed.name)]
        if best.seed.warning or skipped_better:
            out["warning"] = " ".join(filter(None, [best.seed.warning] + [
                f"(The {n} file is marked skipped.)" for n in skipped_better]))
    else:
        out["seed"] = "empty"
        out["started_from"] = {"source": "empty"}
        out["onsets"] = []
        out["warning"] = "No labels found for this track: starting from nothing."
    out["onsets"].sort(key=lambda o: o["time"])
    sus = c.suspect(stem)
    out["suspect"] = sus if isinstance(sus, dict) else None
    if isinstance(sus, str):
        out["warning"] = " ".join(filter(None, [out["warning"], f"The suspect map is unreadable ({sus}): no suspect lane."]))

    return out


# --- The shared state, and writing or publishing it --------------------------------------


# Hosted mode: token buckets per client address (capacity, per seconds).
DEFAULT_LIMITS: dict[str, tuple[int, float]] = {
    "request": (600, 300.0),  # any request, per client address: 600 per 5 min
    "ops": (300, 300.0),  # edit batches (the page sends one a second at most while editing)
    # GET /api/export runs the label checks on every edited track; blinky-corpus's labels-sync pulls
    # it once per run (every 5 min), so this only bounds what anyone else can make it cost.
    "export": (20, 300.0),
}


def initial_state(c: Corpus, stem: str) -> dict[str, Any]:
    """A track's first shared state: its human file, else its best seed (load_track)."""
    t = load_track(c, stem)
    if t["error"]:
        raise ApiError(409, t["error"])
    hp = c.human_path(stem)
    human = read_json(hp) if hp.is_file() else None
    return {"onsets": t["onsets"], "reviewed_ranges": t["reviewed_ranges"],
            "started_from": t["started_from"], "seed": t["seed"], "audio_sha256": t["audio_sha256"],
            "duration": (human or {}).get("duration") or t["manifest_duration"],
            "updated": t["updated"], "notes": (human or {}).get(labelcheck.NOTES_KEY) or [],
            "base_blob": hosted.git_blob_sha(hp.read_bytes()) if hp.is_file() else None}


def atomic_write(path: Path, text: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=path.parent, prefix=f".{path.name}.", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            f.write(text)
            f.flush()
            os.fsync(f.fileno())
        os.chmod(tmp, 0o644)  # mkstemp makes it 0600; a label file is ordinary repo content
        os.replace(tmp, path)
    except BaseException:
        try:
            os.unlink(tmp)
        except FileNotFoundError:
            pass
        raise


class App:
    """The tracks' shared state over HTTP. Local mode writes each change into the corpus clone;
    hosted mode offers changed tracks to the corpus's workflow (hosted.Publication)."""

    def __init__(self, corpus: Corpus, state_dir: Path, *, publisher: hosted.Publication | None = None,
                 checkout: hosted.Checkout | None = None, hosted_mode: bool = False,
                 trust_cf: bool = False, refresh_s: float = 60.0, log: hosted.JsonLog | None = None,
                 limits: dict[str, tuple[int, float]] | None = None):
        self.c, self.publisher, self.checkout = corpus, publisher, checkout
        self.hosted, self.trust_cf, self.refresh_s = hosted_mode, trust_cf, refresh_s
        self.log = log or hosted.JsonLog()
        self.state_dir = Path(state_dir)
        self.rw = checkout.rw if checkout else hosted.RWLock()
        self.store = state.TrackStore(self.state_dir / "tracks", lambda s: initial_state(self.c, s),
                                      on_change=None if hosted_mode else self._write_local)
        self.salt = hosted.Salt(self.state_dir / "ip-salt") if hosted_mode else None
        self.limiter = hosted.RateLimiter(limits or DEFAULT_LIMITS)
        self._stop = threading.Event()

    # -- who's asking (only for rate limits and logs: there are no users)
    def ip_hash(self, headers: Any, peer: str) -> str | None:
        if self.salt is None:
            return None
        ip = peer
        if self.trust_cf:
            try:
                ip = str(ipaddress.ip_address((headers.get("CF-Connecting-IP") or "").strip()))
            except ValueError:
                pass
        return self.salt.ip_hash(ip)

    def limit(self, kind: str, key: str | None) -> None:
        if key is None:
            return
        ok, wait = self.limiter.allow(kind, key)
        if not ok:
            self.log.emit("rate_limited", kind=kind, ip=key, retry_after=wait)
            raise ApiError(429, f"too many requests ({kind}): try again in {wait} s", retry_after=wait)

    # -- reading
    def _path(self, stem: str) -> str:
        return str(self.c.human_path(stem).relative_to(self.c.edm.parent))

    def status(self, stem: str) -> dict[str, Any]:
        """saved / published, for the page's status line."""
        if not self.store.has(stem) or not self.store.edited_since_base(stem):
            return {"published": None, "unpublished": False}
        if self.publisher is None:
            if self.hosted:  # nothing follows main without a clone: it all waits
                return {"published": None, "unpublished": True}
            return {"published": self.store.view(stem)["updated"], "unpublished": False}  # written at once
        return self.publisher.status(self._path(stem), self.store.text(stem))

    def config(self) -> dict[str, Any]:
        return {"hosted": self.hosted, "seeds": seeds_public(self.c), "version": VERSION,
                "publishing": "pulled" if self.publisher else ("local" if not self.hosted else "off")}

    def tracks(self) -> list[dict[str, Any]]:
        out = []
        with self.rw.read():
            for stem in self.c.stems():
                # The shared state only stands in for the human file once someone has edited it:
                # an opened, unedited track is still its seed (or main's file).
                override = (self.store.doc(stem)
                            if self.store.has(stem) and self.store.edited_since_base(stem) else None)
                t = track_summary(self.c, stem, human_override=override)
                t.update(self.status(stem))
                out.append(t)
        return out

    def track(self, stem: str) -> dict[str, Any]:
        with self.rw.read():
            stem = self.c.validate_stem(stem)
            t = load_track(self.c, stem)
            if not t["error"]:
                t.update(self.store.view(stem))  # the shared labels: rev, onsets with ids, ranges
        t.update(self.status(stem))
        return t

    def pull(self, stem: str, since: Any) -> dict[str, Any]:
        try:
            rev = int(since)
        except (TypeError, ValueError):
            raise ApiError(400, "since must be a revision number") from None
        return {**self.store.since(stem, rev), **self.status(stem)}

    # -- writing
    def ops(self, stem: str, body: Any, ip: str | None) -> dict[str, Any]:
        self.limit("ops", ip)
        if not isinstance(body, dict):
            raise ApiError(400, "body must be a JSON object")
        try:
            with self.rw.read():
                rev = self.store.apply(stem, body.get("ops"), body.get("tab"), body.get("audio_sha256"),
                                       body.get("duration"), self.c.manifest_duration(stem))
        except state.OpError as e:
            raise ApiError(409 if "audio changed" in str(e) else 400, str(e)) from None
        if self.hosted:
            self.log.emit("edit", stem=stem, ip=ip, ops=len(body["ops"]), rev=rev)
        return {"rev": rev, **self.status(stem)}

    def _write_local(self, stem: str) -> None:
        text = self.store.text(stem)
        if text is not None and self.store.edited_since_base(stem):
            atomic_write(self.c.human_path(stem), text)

    # -- hosted: refresh the clone, publish
    def _check(self, path: str, text: str, main_text: str | None) -> list[str]:
        stem = Path(path).name[: -len(labelcheck.HUMAN_SUFFIX)]
        base = json.loads(main_text) if main_text else None
        return labelcheck.check_human_doc(self.c.edm, stem, json.loads(text), base, self.c.seeds(),
                                          audio_sha256=self.c.audio_sha256(stem))  # cached per mp3

    def _adopt(self, path: str, main_text: str | None) -> None:
        """A track takes main's version (None: main has no file: back to its seed)."""
        stem = Path(path).name[: -len(labelcheck.HUMAN_SUFFIX)]
        if main_text is None:
            with self.rw.read():
                self.store.reset(stem, initial_state(self.c, stem))
            return
        doc = json.loads(main_text)
        self.store.reset(stem, {**doc, "seed": "human", "base_blob": hosted.git_blob_sha(main_text.encode())})

    def _edited(self) -> dict[str, tuple[str, str | None]]:
        out = {}
        for s in self.store.stems():
            if self.store.edited_since_base(s):
                text = self.store.text(s)
                if text is not None:
                    out[self._path(s)] = (text, self.store.base_blob(s))
        return out

    def export(self, ip: str | None = None) -> dict[str, Any]:
        """For blinky-corpus's labels-sync workflow (it pulls; the service writes nowhere)."""
        self.limit("export", ip)
        if self.publisher is None:
            raise ApiError(404, "no export: this server doesn't publish")
        edited = self._edited()
        last = max((self.store.edited_at(Path(p).name.split(".")[0]) or 0 for p in edited), default=0) or None
        with self.rw.read():
            out = self.publisher.export({p: t for p, (t, _) in edited.items()}, last, self._check)
        self.log.emit("export", ready=out["ready"], tracks=len(out["tracks"]), held=len(out["held"]))
        return out

    def tick(self) -> None:
        if self.checkout is not None:
            try:
                self.checkout.refresh()
            except Exception as e:  # GitHub down: keep serving what we have, say so
                self.log.emit("refresh_failed", error=str(e)[:400])
        self._refresh_unedited()
        if self.publisher is not None:
            self.publisher.tick(self._edited(), self._adopt)

    def _refresh_unedited(self) -> None:
        """A track someone opened but nobody has edited follows main: when its human file there
        changed, its state starts over from it (open pages reload)."""
        for stem in self.store.stems():
            if self.store.edited_since_base(stem):
                continue
            try:
                with self.rw.read():
                    hp = self.c.human_path(stem)
                    now = hosted.git_blob_sha(hp.read_bytes()) if hp.is_file() else None
                    if now != self.store.base_blob(stem):
                        self.store.reset(stem, initial_state(self.c, stem))
                        self.log.emit("refreshed_from_main", stem=stem)
            except Exception as e:  # one unreadable track must not stop the others (or publishing)
                msg = e.message if isinstance(e, ApiError) else repr(e)
                self.log.emit("tick_failed", stem=stem, error=msg[:400])

    def start(self) -> None:
        def loop() -> None:
            while not self._stop.is_set():
                try:
                    self.tick()
                except Exception as e:  # never let the loop die quietly
                    self.log.emit("tick_failed", error=repr(e)[:400])
                self._stop.wait(self.refresh_s)

        threading.Thread(target=loop, name="tick", daemon=True).start()

    def stop(self) -> None:
        self._stop.set()

    # -- moving old state in
    def import_doc(self, stem: str, doc: dict[str, Any]) -> None:
        """Make a label file (or a draft from the old reviewer) a track's shared state, as an edit."""
        stem = self.c.validate_stem(stem)
        if doc.get("audio_sha256") != self.c.audio_sha256(stem):
            raise ApiError(409, f"{stem}: made against different audio than the corpus's mp3")
        hp = self.c.human_path(stem)
        self.store.import_doc(stem, {
            "onsets": [o for o in doc.get("onsets", []) if o.get("type") in LABEL_TYPES],
            "reviewed_ranges": doc.get("reviewed_ranges") or [], "started_from": doc.get("started_from"),
            "notes": doc.get(labelcheck.NOTES_KEY) or [], "seed": "human", "audio_sha256": doc["audio_sha256"],
            "duration": doc.get("duration") or self.c.manifest_duration(stem), "updated": doc.get("updated"),
            "base_blob": hosted.git_blob_sha(hp.read_bytes()) if hp.is_file() else None})
        if self.store.on_change:
            self.store.on_change(stem)
        self.log.emit("imported", stem=stem, hits=len(doc.get("onsets", [])))

    def migrate_old_state(self) -> None:
        """The first hosted version kept per-person drafts, edit locks and pending saves. The
        newest draft of a track (if it has no shared state yet) becomes its shared state; the
        rest goes."""
        drafts = self.state_dir / "drafts"
        if drafts.is_dir():
            for track_dir in sorted(p for p in drafts.iterdir() if p.is_dir()):
                docs = []
                for f in track_dir.glob("*.json"):
                    try:
                        docs.append(json.loads(f.read_text()))
                    except ValueError:
                        continue
                if docs and not self.store.has(track_dir.name):
                    newest = max(docs, key=lambda d: str(d.get("updated") or ""))
                    try:
                        self.import_doc(track_dir.name, newest)
                    except ApiError as e:
                        self.log.emit("migrate_failed", stem=track_dir.name, error=e.message)
            import shutil
            shutil.rmtree(drafts)
        for name in ("locks.json", "pending.json"):
            (self.state_dir / name).unlink(missing_ok=True)


# --- HTTP ---------------------------------------------------------------------
#
# No auth, but other web pages open in the reviewer's browser must not be able to write labels:
#   * every request's Host must be an IP literal, localhost, this machine's name or an
#     --allow-host name, which defeats DNS rebinding (a hostile name resolving to this server);
#   * a POST must be Content-Type: application/json (a cross-origin page can't send that
#     without a CORS preflight) and, when it carries an Origin, the Origin must be this server
#     or one of the --cors-origin pages (hosted mode: the product site serves the page and this
#     server is its API). Preflights are answered for those origins only.


def _split_host(value: str) -> str:
    v = value.strip().lower()
    if v.startswith("["):  # [ipv6]:port
        return v[1 : v.find("]")] if "]" in v else v
    return v.rsplit(":", 1)[0] if v.count(":") == 1 else v


def local_names() -> set[str]:
    names = {"localhost"}
    for n in (socket.gethostname(), socket.getfqdn()):
        if n:
            names |= {n.lower(), n.lower().split(".")[0], n.lower().split(".")[0] + ".local"}
    return names


def host_allowed(host: str | None, allowed: set[str]) -> bool:
    if not host:
        return False
    h = _split_host(host)
    try:
        ipaddress.ip_address(h)
        return True
    except ValueError:
        return h in allowed


# The page is self-contained: inline script and styles, its analysis worker from a Blob URL.
HOSTED_HEADERS = {
    "Content-Security-Policy": (
        "default-src 'self'; script-src 'self' 'unsafe-inline'; worker-src blob:; "
        "style-src 'self' 'unsafe-inline'; img-src 'self' data:; media-src 'self' blob:; "
        "connect-src 'self'; frame-ancestors 'none'; base-uri 'none'; form-action 'none'"),
    "X-Content-Type-Options": "nosniff",
    "Referrer-Policy": "no-referrer",
    "X-Frame-Options": "DENY",
}


CORS_HEADERS = "Content-Type"


class Handler(BaseHTTPRequestHandler):
    corpus: Corpus  # set by make_server()
    allowed_hosts: set[str]  # set by make_server()
    app: "App"  # set by make_server()
    cors_origins: frozenset[str] = frozenset()  # pages on other origins allowed to call the API
    page_url: str | None = None  # hosted: where GET / sends people (the product page)
    server_version = "drum-reviewer/1"
    timeout = 60  # a stalled client can't hold a thread forever

    def log_message(self, fmt: str, *args: Any) -> None:  # quiet audio/api chatter
        if self.app.hosted:
            return  # hosted mode logs one JSON line per request instead
        if args and isinstance(args[0], str) and ("/audio/" in args[0] or "/api/" in args[0]):
            if len(args) > 1 and str(args[1]).startswith(("2", "3")):
                return
        super().log_message(fmt, *args)

    def send_response(self, code: int, message: str | None = None) -> None:
        self._status = code
        super().send_response(code, message)

    def _cors_origin(self) -> str | None:
        origin = self.headers.get("Origin")
        return origin if origin and origin in self.cors_origins else None

    def end_headers(self) -> None:
        if self.app.hosted:
            for k, v in HOSTED_HEADERS.items():
                self.send_header(k, v)
        if self.cors_origins:
            self.send_header("Vary", "Origin")
            origin = self._cors_origin()
            if origin:
                self.send_header("Access-Control-Allow-Origin", origin)
                self.send_header("Access-Control-Expose-Headers", "Retry-After")
        super().end_headers()

    def _send_json(self, data: Any, status: int = 200, headers: dict[str, str] | None = None) -> None:
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (headers or {}).items():
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def _stem_param(self, q: dict[str, list[str]]) -> str:
        vals = q.get("stem")
        if not vals:
            raise ApiError(400, "missing ?stem=")
        return self.corpus.validate_stem(vals[0])

    def _dispatch(self, fn) -> None:
        t0 = time.monotonic()
        self._status = 0
        self._ip = None
        try:
            self._ip = self.app.ip_hash(self.headers, self.client_address[0])
            if urlparse(self.path).path != "/healthz":
                self.app.limit("request", self._ip)
            fn()
        except ApiError as e:
            extra = {"Retry-After": str(e.extra["retry_after"])} if "retry_after" in e.extra else None
            self._send_json({"error": e.message, **e.extra}, e.status, extra)
        except (BrokenPipeError, ConnectionResetError):
            pass
        except Exception as e:  # a malformed label file, a bug: say so, loudly
            if self.app.hosted:
                self.app.log.emit("error", method=self.command, path=urlparse(self.path).path,
                                  error=repr(e)[:400])
            else:
                self.log_error("%s %s failed: %r", self.command, self.path, e)
            self._send_json({"error": f"server error: {e!r}"}, 500)
        finally:
            if self.app.hosted:
                url = urlparse(self.path)
                stem = (parse_qs(url.query).get("stem") or [None])[0]
                self.app.log.emit("http", method=self.command, path=url.path[:200],
                                  stem=stem[:80] if isinstance(stem, str) else None,
                                  status=self._status, ms=round(1000 * (time.monotonic() - t0), 1),
                                  ip=self._ip)

    def _check_host(self) -> None:
        if not host_allowed(self.headers.get("Host"), self.allowed_hosts):
            raise ApiError(403, f"Host {self.headers.get('Host')!r} not allowed (--allow-host)")

    def _check_post(self) -> None:
        ctype = (self.headers.get("Content-Type") or "").split(";")[0].strip().lower()
        if ctype != "application/json":
            raise ApiError(415, "POST must be Content-Type: application/json")
        origin = self.headers.get("Origin")
        if origin is not None and origin not in self.cors_origins:
            o = urlparse(origin)
            if o.netloc.lower() != (self.headers.get("Host") or "").lower():
                raise ApiError(403, f"cross-origin POST from {origin!r} refused")

    def do_GET(self) -> None:
        self._dispatch(lambda: (self._check_host(), self._get()))

    def do_POST(self) -> None:
        self._dispatch(lambda: (self._check_host(), self._check_post(), self._post()))

    def do_OPTIONS(self) -> None:
        self._dispatch(lambda: (self._check_host(), self._preflight()))

    def _preflight(self) -> None:
        """CORS preflight: answered only for a --cors-origin page asking for GET or POST."""
        if not self._cors_origin():
            raise ApiError(403, "origin not allowed")
        if (self.headers.get("Access-Control-Request-Method") or "") not in ("GET", "POST"):
            raise ApiError(405, "method not allowed")
        self.send_response(204)
        self.send_header("Access-Control-Allow-Methods", "GET, POST")
        self.send_header("Access-Control-Allow-Headers", CORS_HEADERS)
        self.send_header("Access-Control-Max-Age", "600")
        self.send_header("Content-Length", "0")
        self.end_headers()

    def _get(self) -> None:
        url = urlparse(self.path)
        path, q = url.path, parse_qs(url.query)
        app = self.app
        if path in ("/", "/index.html") and self.page_url:
            self.send_response(302)
            self.send_header("Location", self.page_url)
            self.send_header("Content-Length", "0")
            self.end_headers()
        elif path in ("/", "/index.html"):
            body = (STATIC_DIR / "index.html").read_bytes()
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)
        elif path == "/healthz":
            self._send_json({"ok": True})
        elif path == "/api/config":
            self._send_json(app.config())
        elif path == "/api/tracks":
            self._send_json({"tracks": app.tracks(), "seeds": seeds_public(self.corpus)})
        elif path == "/api/track":
            self._send_json(app.track(self._stem_param(q)))
        elif path == "/api/ops":
            self._send_json(app.pull(self._stem_param(q), (q.get("since") or [None])[0]))
        elif path == "/api/export":
            self._send_json(app.export(self._ip))
        elif path.startswith("/audio/"):
            name = unquote(path[len("/audio/"):])
            if not name.endswith(".mp3"):
                raise ApiError(404, "not found")
            stem = self.corpus.validate_stem(name[: -len(".mp3")])
            self._serve_audio(self.corpus.audio_path(stem))
        else:
            raise ApiError(404, "not found")

    def _read_body(self) -> Any:
        try:
            n = int(self.headers.get("Content-Length", ""))
        except ValueError:
            raise ApiError(411, "Content-Length required") from None
        if n < 0 or n > MAX_POST_BYTES:
            raise ApiError(413, f"body over {MAX_POST_BYTES} bytes")
        raw = self.rfile.read(n)
        try:
            return json.loads(raw)
        except (json.JSONDecodeError, UnicodeDecodeError) as e:
            raise ApiError(400, f"invalid JSON: {e}") from e

    def _post(self) -> None:
        url = urlparse(self.path)
        path, q = url.path, parse_qs(url.query)
        if path != "/api/ops":
            raise ApiError(404, "not found")
        stem = self._stem_param(q)
        self._send_json(self.app.ops(stem, self._read_body(), self._ip))

    def _serve_audio(self, path: Path) -> None:
        size = path.stat().st_size
        start, end = 0, size - 1
        rng = self.headers.get("Range")
        status = 200
        if rng:
            m = re.fullmatch(r"bytes=(\d*)-(\d*)", rng.strip())
            if not m or (not m.group(1) and not m.group(2)):
                raise ApiError(416, "bad Range")
            if m.group(1):
                start = int(m.group(1))
                end = int(m.group(2)) if m.group(2) else size - 1
            else:  # suffix range: the last N bytes
                start = max(0, size - int(m.group(2)))
            end = min(end, size - 1)
            if start > end:
                raise ApiError(416, "range not satisfiable")
            status = 206
        self.send_response(status)
        self.send_header("Content-Type", "audio/mpeg")
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Length", str(end - start + 1))
        self.send_header("Cache-Control", "public, max-age=300")
        if status == 206:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()
        with open(path, "rb") as f:
            f.seek(start)
            left = end - start + 1
            while left > 0:
                chunk = f.read(min(1 << 16, left))
                if not chunk:
                    break
                self.wfile.write(chunk)
                left -= len(chunk)


def make_server(
    app: App, host: str = "127.0.0.1", port: int = 8766, allow_hosts: tuple[str, ...] = (),
    cors_origins: tuple[str, ...] = (), page_url: str | None = None,
) -> ThreadingHTTPServer:
    if not app.c.edm.is_dir():
        raise SystemExit(f"edm dir not found: {app.c.edm}")
    for o in cors_origins:
        u = urlparse(o)
        if u.scheme not in ("http", "https") or not u.netloc or u.path not in ("", "/") or u.query:
            raise SystemExit(f"--cors-origin must be scheme://host[:port], got {o!r}")
    allowed = local_names() | {h.lower() for h in allow_hosts}
    handler = type("BoundHandler", (Handler,), {
        "corpus": app.c, "allowed_hosts": allowed, "app": app, "page_url": page_url,
        "cors_origins": frozenset(o.rstrip("/") for o in cors_origins)})
    srv = ThreadingHTTPServer((host, port), handler)
    srv.daemon_threads = True
    return srv


def local_app(edm: Path) -> App:
    """Local mode: the shared state next to the human files, each change written at once."""
    c = Corpus(edm)
    return App(c, c.human_dir / ".state")


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8766)
    ap.add_argument("--host", default="127.0.0.1",
                    help="127.0.0.1 (default) for this machine only; 0.0.0.0 to serve the LAN")
    ap.add_argument("--edm", type=Path, default=None, help=f"corpus dir (default {DEFAULT_EDM}, or "
                    "<--checkout>/edm in hosted mode)")
    ap.add_argument("--allow-host", action="append", default=[], metavar="NAME",
                    help="extra hostname the page may be opened under (IPs, localhost and this "
                         "machine's name are always allowed)")
    ap.add_argument("--import-doc", type=Path, action="append", default=[], metavar="FILE",
                    help="make a label file (or an old draft) its track's shared state, then exit")
    h = ap.add_argument_group("hosted mode (README.md \"Hosted\")")
    h.add_argument("--state", type=Path, help="state dir (the shared labels, publisher state): turns "
                   "on hosted mode")
    h.add_argument("--checkout", type=Path, help="the service's own corpus clone (cloned if missing)")
    h.add_argument("--git-url", help="where --checkout is cloned and refreshed from")
    h.add_argument("--git-branch", default="main")
    h.add_argument("--refresh", type=float, default=60.0, metavar="S",
                   help="refresh the clone and consider publishing every S seconds")
    h.add_argument("--quiet", type=float, default=180.0, metavar="S",
                   help="the export is ready once nobody has edited for S seconds...")
    h.add_argument("--max-wait", type=float, default=900.0, metavar="S",
                   help="...or S seconds after the first unpublished edit, while people keep editing")
    h.add_argument("--cors-origin", action="append", default=[], metavar="URL",
                   help="a page origin (scheme://host) allowed to call this API, e.g. the product "
                        "site that serves the reviewer page")
    h.add_argument("--page-url", help="where GET / redirects (the product page), instead of serving "
                                      "the page here")
    h.add_argument("--trust-cf-connecting-ip", action="store_true",
                   help="take the client address from CF-Connecting-IP (only behind a Cloudflare "
                        "tunnel that is the sole way in)")
    args = ap.parse_args(argv)

    if args.state is None:
        app = local_app(args.edm or DEFAULT_EDM)
    else:
        log = hosted.JsonLog()
        if args.checkout is None or not args.git_url:
            ap.error("hosted mode needs --checkout and --git-url")
        checkout = hosted.Checkout(args.checkout.resolve(), args.git_url, args.git_branch, log=log)
        checkout.ensure()
        publisher = hosted.Publication(checkout, args.state / "publication.json", base=args.git_branch,
                                       quiet_s=args.quiet, max_wait_s=args.max_wait, log=log)
        app = App(Corpus(args.edm or checkout.root / "edm"), args.state, publisher=publisher,
                  checkout=checkout, hosted_mode=True, trust_cf=args.trust_cf_connecting_ip,
                  refresh_s=args.refresh, log=log)
        app.migrate_old_state()
    if args.import_doc:
        for f in args.import_doc:
            doc = json.loads(f.read_text())
            app.import_doc(doc.get("track") or f.name.split(".")[0], doc)
        print(f"imported {len(args.import_doc)} file(s)", file=sys.stderr)
        return

    srv = make_server(app, args.host, args.port, tuple(args.allow_host), tuple(args.cors_origin),
                      args.page_url)
    stems = app.c.stems()
    if app.hosted:
        app.log.emit("start", tracks=len(stems), port=args.port, version=VERSION)
        app.start()
    else:
        print(f"labels-service: {len(stems)} tracks in {app.c.edm}; every change is written to "
              f"{app.c.human_dir}")
        print(f"  open http://{'localhost' if args.host in ('127.0.0.1', 'localhost') else args.host}:{args.port}/")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        app.stop()


if __name__ == "__main__":
    main()
