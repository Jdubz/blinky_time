#!/usr/bin/env python3
"""Label reviewer server — browse, review, and curate onset labels.

Serves a web UI for visually reviewing onset training/validation labels
against audio. Supports multiple corpora simultaneously: switch between
them in the UI without restarting the server.

Defaults register two corpora:
  * `training` — devtop training corpus (large, devtop-only)
  * `edm`      — in-repo validation corpus (18 EDM tracks, syncs via git)

Override or replace the defaults with one or more `--corpus name=audio_dir,
onset_dir,human_edits_dir[,beat_dir,librosa_dir]`.

Usage:
    python tools/label-reviewer/server.py
    python tools/label-reviewer/server.py --port 8765

Then open http://localhost:8765 in a browser.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
from dataclasses import dataclass
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from typing import Any
from urllib.parse import parse_qs, urlparse

STATIC_DIR = Path(__file__).parent
REPO_ROOT = Path(__file__).resolve().parents[3]

# Built-in corpus defaults.
_TRAINING_LABELS_BASE = Path("/mnt/storage/blinky-ml-data/labels")
_TRAINING_AUDIO = Path("/mnt/storage/blinky-ml-data/audio/combined")
_EDM_DIR = REPO_ROOT / "blinky-test-player" / "music" / "edm"


@dataclass
class Corpus:
    """A reviewable corpus: audio + auto onset labels + human edit overlay.

    Multiple corpora can coexist; the UI picks one at a time. Each corpus owns
    its own track index, audio dir, and edit overlay dir, so corpus switches
    are pure runtime config — no restart.
    """

    name: str
    audio_dir: Path
    onset_dir: Path
    human_edits_dir: Path
    beat_dir: Path | None = None
    librosa_dir: Path | None = None
    description: str = ""

    @classmethod
    def parse(cls, spec: str) -> Corpus:
        """Parse a CLI spec like ``name=audio,onset,human[,beat[,librosa]]``."""
        if "=" not in spec:
            raise argparse.ArgumentTypeError(
                f"corpus spec must be 'name=audio_dir,onset_dir,human_edits_dir"
                f"[,beat_dir,librosa_dir]', got {spec!r}"
            )
        name, paths_str = spec.split("=", 1)
        parts = [p.strip() for p in paths_str.split(",")]
        if len(parts) < 3 or len(parts) > 5:
            raise argparse.ArgumentTypeError(
                f"corpus {name!r}: expected 3-5 comma-separated paths, got {len(parts)}"
            )
        audio, onset, human = parts[:3]
        beat = parts[3] if len(parts) > 3 and parts[3] else None
        librosa = parts[4] if len(parts) > 4 and parts[4] else None
        return cls(
            name=name.strip(),
            audio_dir=Path(audio),
            onset_dir=Path(onset),
            human_edits_dir=Path(human),
            beat_dir=Path(beat) if beat else None,
            librosa_dir=Path(librosa) if librosa else None,
        )


def _default_corpora() -> list[Corpus]:
    return [
        Corpus(
            name="training",
            audio_dir=_TRAINING_AUDIO,
            onset_dir=_TRAINING_LABELS_BASE / "onsets_consensus",
            human_edits_dir=_TRAINING_LABELS_BASE / "onsets_human",
            beat_dir=_TRAINING_LABELS_BASE / "consensus_v5",
            librosa_dir=_TRAINING_LABELS_BASE / "onsets_librosa",
            description="Training corpus (devtop-local). Edits stay on devtop and feed prepare_dataset.py.",
        ),
        Corpus(
            name="edm",
            audio_dir=_EDM_DIR,
            onset_dir=_EDM_DIR,
            human_edits_dir=_EDM_DIR,
            description="In-repo validation corpus. Edits commit to git and ride to blinkyhost via pull.",
        ),
    ]


# Defaults; replaced from argparse in main().
REVIEW_STATE_PATH = _TRAINING_LABELS_BASE / "review_state.json"

# Global runtime state, populated by main().
corpora: dict[str, Corpus] = {}
tracks_by_corpus: dict[str, list[dict[str, Any]]] = {}
review_state: dict[
    str, dict[str, Any]
] = {}  # corpus_name -> {stem: {status, notes, flags}}


# --- Track index ----------------------------------------------------------


def _onset_consensus_path(corpus: Corpus, stem: str) -> Path | None:
    """Return the auto-onset file for a stem, probing both filename forms.

    NOTE: callers must `_validate_stem(stem)` first if `stem` comes from
    user input. This function is also called during filesystem iteration
    where stems may legitimately start with `.` (e.g., hidden files); the
    untrusted-input gate lives at the request-handler boundary instead.
    """
    candidates = [
        corpus.onset_dir / f"{stem}.onsets_consensus.json",
        corpus.onset_dir / f"{stem}.onsets.json",
    ]
    for p in candidates:
        if p.exists():
            return p
    return None


def load_tracks_for(corpus: Corpus) -> list[dict[str, Any]]:
    """Build a track index for a single corpus.

    A track is included if it has audio AND at least one of (onset consensus,
    beat labels). Same logic as before, scoped per corpus.
    """
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    if not corpus.audio_dir.is_dir():
        print(f"[{corpus.name}] audio dir missing: {corpus.audio_dir}")
        return []

    audio_index: dict[str, Path] = {}
    for f in corpus.audio_dir.iterdir():
        if f.suffix.lower() in audio_extensions:
            audio_index[f.stem] = f

    tracks: list[dict[str, Any]] = []
    state = review_state.setdefault(corpus.name, {})
    for stem, audio_path in sorted(audio_index.items()):
        has_beat = bool(
            corpus.beat_dir and (corpus.beat_dir / f"{stem}.beats.json").exists()
        )
        has_onset = _onset_consensus_path(corpus, stem) is not None
        has_librosa = bool(
            corpus.librosa_dir and (corpus.librosa_dir / f"{stem}.onsets.json").exists()
        )
        if not (has_beat or has_onset):
            continue
        tracks.append(
            {
                "stem": stem,
                "audio_path": str(audio_path),
                "audio_ext": audio_path.suffix,
                "has_beat_labels": has_beat,
                "has_onset_consensus": has_onset,
                "has_librosa_onsets": has_librosa,
                "review_status": state.get(stem, {}).get("status", "unreviewed"),
            }
        )
    return tracks


def reload_corpus(name: str) -> None:
    """Rebuild the track index for a single corpus (e.g. after edits)."""
    if name in corpora:
        tracks_by_corpus[name] = load_tracks_for(corpora[name])


# --- Review state ---------------------------------------------------------


def load_review_state() -> None:
    """Load and migrate review state from disk.

    Old format was ``{stem: {...}}`` (single implicit corpus). New format is
    ``{corpus_name: {stem: {...}}}``. If we detect the old format on disk,
    transparently migrate it under the ``training`` key (the historical
    default corpus).
    """
    global review_state
    if not REVIEW_STATE_PATH.exists():
        review_state = {}
        return
    with open(REVIEW_STATE_PATH) as f:
        loaded = json.load(f)

    if not isinstance(loaded, dict):
        review_state = {}
        return

    # Detect legacy flat format: top-level values look like per-track records.
    is_legacy = bool(loaded) and all(
        isinstance(v, dict) and ("status" in v or "notes" in v or "flags" in v)
        for v in loaded.values()
    )
    if is_legacy:
        review_state = {"training": loaded}
        save_review_state()  # rewrite as nested
        print("Migrated legacy review_state.json to nested-by-corpus format.")
    else:
        # Already nested: ensure every value is a dict of stem→record.
        review_state = {k: v for k, v in loaded.items() if isinstance(v, dict)}


def save_review_state() -> None:
    REVIEW_STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    tmp = REVIEW_STATE_PATH.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(review_state, f, indent=2)
    tmp.replace(REVIEW_STATE_PATH)


# --- Human edit overlay ---------------------------------------------------


def _empty_edits_doc(stem: str, source_count: int = 0) -> dict[str, Any]:
    return {
        "stem": stem,
        "source": "onsets_consensus",
        "source_count": source_count,
        "edits": {},
        "created": [],
    }


def _validate_stem(stem: str) -> str:
    """Reject any stem that could traverse out of its corpus directory.

    `stem` comes from query parameters or POST body and is used to construct
    paths under `corpus.human_edits_dir` / `corpus.onset_dir`. A value
    containing `/`, `\\`, or starting with `.` could escape the intended
    directory. We reject loudly instead of silently sanitizing so callers
    notice their bug rather than a request succeeding on the wrong file.
    """
    if not stem or not isinstance(stem, str):
        raise ValueError(f"invalid stem: {stem!r}")
    if "/" in stem or "\\" in stem or stem.startswith("."):
        raise ValueError(f"invalid stem (path traversal): {stem!r}")
    # Belt-and-suspenders: after the explicit checks above, `Path(stem).name`
    # should equal `stem` for any input that survives. If it doesn't, we have
    # a sneakier traversal vector that the explicit rules above missed.
    if Path(stem).name != stem:
        raise ValueError(f"invalid stem (Path.name mismatch): {stem!r}")
    return stem


def _human_edits_path(corpus: Corpus, stem: str) -> Path:
    return corpus.human_edits_dir / f"{_validate_stem(stem)}.onsets_human.json"


def load_human_edits(
    corpus: Corpus, stem: str, source_count: int = 0
) -> dict[str, Any]:
    path = _human_edits_path(corpus, stem)
    if not path.exists():
        return _empty_edits_doc(stem, source_count)
    with open(path) as f:
        doc: dict[str, Any] = json.load(f)
    doc.setdefault("stem", stem)
    doc.setdefault("source", "onsets_consensus")
    doc.setdefault("source_count", source_count)
    doc.setdefault("edits", {})
    doc.setdefault("created", [])
    return doc


def save_human_edits(corpus: Corpus, stem: str, doc: dict[str, Any]) -> None:
    corpus.human_edits_dir.mkdir(parents=True, exist_ok=True)
    path = _human_edits_path(corpus, stem)
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(doc, f, indent=2)
    tmp.replace(path)


def get_track_labels(corpus: Corpus, stem: str) -> dict[str, Any]:
    """Load all available labels for a track in a corpus."""
    result: dict[str, Any] = {
        "stem": stem,
        "corpus": corpus.name,
        "beats": [],
        "onsets_consensus": [],
        "onsets_librosa": [],
    }

    # Beats (only if the corpus has a beat dir)
    if corpus.beat_dir:
        beat_path = corpus.beat_dir / f"{stem}.beats.json"
        if beat_path.exists():
            with open(beat_path) as f:
                data = json.load(f)
            result["beats"] = [
                {
                    "time": h["time"],
                    "strength": h.get("strength", 1.0),
                    "isDownbeat": h.get("isDownbeat", False),
                }
                for h in data.get("hits", [])
                if h.get("expectTrigger", True)
            ]

    # Auto onsets — probe both filename forms (dir-name vs filename convention).
    onset_path = _onset_consensus_path(corpus, stem)
    if onset_path is not None:
        with open(onset_path) as f:
            data = json.load(f)
        result["onsets_consensus"] = data.get("onsets", [])
        result["onset_systems"] = data.get("total_systems", 0)
        result["onset_systems_succeeded"] = data.get("systems_succeeded", 0)
        result["onset_source_file"] = onset_path.name

    # Librosa onsets (cheap reference; optional)
    if corpus.librosa_dir:
        librosa_path = corpus.librosa_dir / f"{stem}.onsets.json"
        if librosa_path.exists():
            with open(librosa_path) as f:
                data = json.load(f)
            result["onsets_librosa"] = [{"time": t} for t in data.get("onsets", [])]

    result["human_edits"] = load_human_edits(
        corpus, stem, len(result["onsets_consensus"])
    )
    return result


# --- HTTP handler ---------------------------------------------------------


class LabelReviewerHandler(SimpleHTTPRequestHandler):
    """HTTP handler — endpoints scope by ?corpus=<name> (or audio path /audio/<corpus>/...)."""

    # Helpers ---------------------------------------------------------

    def _get_corpus_param(self, params: dict[str, list[str]]) -> Corpus | None:
        """Resolve ?corpus=<name>. If only one corpus is registered, default to it."""
        name = params.get("corpus", [None])[0]
        if name is None:
            if len(corpora) == 1:
                return next(iter(corpora.values()))
            self.send_json(
                {"error": "corpus parameter required", "available": sorted(corpora)},
                400,
            )
            return None
        if name not in corpora:
            self.send_json(
                {"error": f"unknown corpus: {name}", "available": sorted(corpora)}, 400
            )
            return None
        return corpora[name]

    # GET -------------------------------------------------------------

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)

        if path in ("/", "/index.html"):
            self.serve_file(STATIC_DIR / "index.html", "text/html")
            return

        if path == "/api/corpora":
            self.send_json(
                {
                    "corpora": [
                        {
                            "name": c.name,
                            "description": c.description,
                            "audio_dir": str(c.audio_dir),
                            "onset_dir": str(c.onset_dir),
                            "human_edits_dir": str(c.human_edits_dir),
                            "track_count": len(tracks_by_corpus.get(c.name, [])),
                        }
                        for c in corpora.values()
                    ]
                }
            )
            return

        if path == "/api/tracks":
            corpus = self._get_corpus_param(params)
            if corpus is None:
                return
            page = int(params.get("page", ["0"])[0])
            per_page = int(params.get("per_page", ["50"])[0])
            status_filter = params.get("status", [None])[0]
            tracks = tracks_by_corpus.get(corpus.name, [])
            filtered = (
                [t for t in tracks if t["review_status"] == status_filter]
                if status_filter and status_filter != "all"
                else tracks
            )
            start, end = page * per_page, page * per_page + per_page
            self.send_json(
                {
                    "corpus": corpus.name,
                    "tracks": filtered[start:end],
                    "total": len(filtered),
                    "page": page,
                    "per_page": per_page,
                }
            )
            return

        if path == "/api/labels":
            corpus = self._get_corpus_param(params)
            if corpus is None:
                return
            stem = params.get("stem", [None])[0]
            if not stem:
                self.send_json({"error": "stem parameter required"}, 400)
                return
            try:
                _validate_stem(stem)
            except ValueError as exc:
                self.send_json({"error": str(exc)}, 400)
                return
            labels = get_track_labels(corpus, stem)
            labels["review"] = review_state.get(corpus.name, {}).get(stem, {})
            self.send_json(labels)
            return

        if path.startswith("/audio/"):
            # /audio/<corpus>/<filename>
            tail = path[len("/audio/") :]
            if "/" not in tail:
                self.send_error(400, "audio path must be /audio/<corpus>/<filename>")
                return
            corpus_name, filename = tail.split("/", 1)
            corpus = corpora.get(corpus_name)
            if corpus is None:
                self.send_error(404, f"unknown corpus: {corpus_name}")
                return
            stem = Path(filename).stem
            for t in tracks_by_corpus.get(corpus.name, []):
                if t["stem"] == stem:
                    audio_path = Path(t["audio_path"])
                    if audio_path.exists():
                        self.serve_audio(audio_path)
                        return
            self.send_error(404)
            return

        # Static files (fall-through). Resolve + prefix-check defends against
        # `/../etc/passwd`-style requests that would otherwise traverse out of
        # STATIC_DIR. Even though the reviewer is a localhost dev tool, the
        # default bind is 0.0.0.0 (LAN-reachable) — cheap to fix, no reason not to.
        try:
            file_path = (STATIC_DIR / path.lstrip("/")).resolve()
            static_root = STATIC_DIR.resolve()
            file_path.relative_to(static_root)  # raises if not under STATIC_DIR
        except (ValueError, OSError):
            self.send_error(403)
            return
        if file_path.exists() and file_path.is_file():
            content_type = mimetypes.guess_type(str(file_path))[0] or "text/plain"
            self.serve_file(file_path, content_type)
        else:
            self.send_error(404)

    # POST ------------------------------------------------------------

    def do_POST(self) -> None:
        parsed = urlparse(self.path)
        path = parsed.path
        if path not in ("/api/edits", "/api/review"):
            self.send_error(404)
            return

        # Cap request body to prevent memory exhaustion. 1 MB is generous —
        # the largest legitimate POST is an `/api/edits` doc for a single
        # track, which is well under that.
        MAX_BODY_BYTES = 1 * 1024 * 1024
        content_length = int(self.headers.get("Content-Length", 0))
        if content_length < 0 or content_length > MAX_BODY_BYTES:
            self.send_error(413, f"Content-Length must be 0..{MAX_BODY_BYTES}")
            return
        body = json.loads(self.rfile.read(content_length))

        corpus_name = body.get("corpus")
        if not corpus_name or corpus_name not in corpora:
            self.send_json(
                {"error": "valid corpus field required", "available": sorted(corpora)},
                400,
            )
            return
        corpus = corpora[corpus_name]
        stem = body.get("stem")
        if not stem:
            self.send_json({"error": "stem required"}, 400)
            return
        try:
            _validate_stem(stem)
        except ValueError as exc:
            self.send_json({"error": str(exc)}, 400)
            return

        if path == "/api/edits":
            edits = body.get("edits", {})
            created = body.get("created", [])
            if not isinstance(edits, dict) or not isinstance(created, list):
                self.send_json(
                    {"error": "edits must be dict, created must be list"}, 400
                )
                return
            doc = {
                "stem": stem,
                "source": body.get("source", "onsets_consensus"),
                "source_count": int(body.get("source_count", 0)),
                "edits": edits,
                "created": created,
            }
            save_human_edits(corpus, stem, doc)
            self.send_json({"ok": True, "doc": doc})
            return

        # /api/review
        state = review_state.setdefault(corpus_name, {})
        state[stem] = {
            "status": body.get("status", "reviewed"),
            "notes": body.get("notes", ""),
            "flags": body.get("flags", []),
        }
        for t in tracks_by_corpus.get(corpus_name, []):
            if t["stem"] == stem:
                t["review_status"] = state[stem]["status"]
                break
        save_review_state()
        self.send_json({"ok": True})

    # Plumbing --------------------------------------------------------

    def send_json(self, data: Any, status: int = 200) -> None:
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_file(self, path: Path, content_type: str) -> None:
        with open(path, "rb") as f:
            content = f.read()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def serve_audio(self, audio_path: Path) -> None:
        """Serve audio with HTTP Range support for seeking."""
        file_size = audio_path.stat().st_size
        content_type = mimetypes.guess_type(str(audio_path))[0] or "audio/mpeg"
        range_header = self.headers.get("Range")

        if range_header:
            try:
                range_spec = range_header.replace("bytes=", "")
                parts = range_spec.split("-")
                start = int(parts[0]) if parts[0] else 0
                end = int(parts[1]) if parts[1] else file_size - 1
            except (ValueError, IndexError):
                self.send_error(416, "Invalid Range")
                return
            if start >= file_size or end >= file_size or start > end:
                self.send_error(416, "Range Not Satisfiable")
                self.send_header("Content-Range", f"bytes */{file_size}")
                self.end_headers()
                return

            content_length = end - start + 1
            self.send_response(206)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Range", f"bytes {start}-{end}/{file_size}")
            self.send_header("Content-Length", str(content_length))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(audio_path, "rb") as f:
                f.seek(start)
                self.wfile.write(f.read(content_length))
        else:
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(file_size))
            self.send_header("Accept-Ranges", "bytes")
            self.end_headers()
            with open(audio_path, "rb") as f:
                while True:
                    chunk = f.read(65536)
                    if not chunk:
                        break
                    self.wfile.write(chunk)

    def log_message(self, format: str, *args: Any) -> None:
        # Suppress noisy access logs for API and audio chunks.
        if args and ("/api/" in str(args[0]) or "/audio/" in str(args[0])):
            return
        super().log_message(format, *args)


# --- Entrypoint -----------------------------------------------------------


def main() -> None:
    global REVIEW_STATE_PATH

    parser = argparse.ArgumentParser(description="Label reviewer web UI")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument(
        "--corpus",
        type=Corpus.parse,
        action="append",
        default=None,
        metavar="NAME=AUDIO,ONSET,HUMAN[,BEAT,LIBROSA]",
        help="Register a corpus. Repeat to add multiple. If unset, the default "
        "training + edm corpora are registered.",
    )
    parser.add_argument(
        "--no-defaults",
        action="store_true",
        help="Skip the built-in default corpora (use only --corpus entries).",
    )
    parser.add_argument(
        "--review-state",
        type=Path,
        default=REVIEW_STATE_PATH,
        help="Path to the per-corpus review_state.json (default: %(default)s)",
    )
    args = parser.parse_args()

    REVIEW_STATE_PATH = args.review_state

    chosen: list[Corpus] = []
    if not args.no_defaults:
        chosen.extend(_default_corpora())
    if args.corpus:
        # User-specified corpora override defaults of the same name.
        by_name = {c.name: c for c in chosen}
        for c in args.corpus:
            by_name[c.name] = c
        chosen = list(by_name.values())

    # Filter out corpora whose audio_dir doesn't exist — the user shouldn't see
    # a registered corpus that has nothing in it. Print why each was dropped
    # so a missing /mnt/storage isn't silent.
    available: list[Corpus] = []
    for c in chosen:
        if not c.audio_dir.is_dir():
            print(f"[skip] corpus {c.name!r}: audio dir missing ({c.audio_dir})")
            continue
        available.append(c)

    if not available:
        parser.error(
            "no corpora available — every audio dir is missing. "
            "Pass --corpus name=audio,onset,human to register one."
        )

    for c in available:
        corpora[c.name] = c

    load_review_state()
    for c in corpora.values():
        tracks_by_corpus[c.name] = load_tracks_for(c)

    print("Label Reviewer")
    for c in corpora.values():
        n = len(tracks_by_corpus.get(c.name, []))
        rev = sum(
            1 for t in tracks_by_corpus[c.name] if t["review_status"] != "unreviewed"
        )
        print(f"  [{c.name}] {n} tracks · {rev} reviewed · audio={c.audio_dir}")
    print(f"  http://localhost:{args.port}")

    server = HTTPServer(("0.0.0.0", args.port), LabelReviewerHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutdown.")


if __name__ == "__main__":
    main()
