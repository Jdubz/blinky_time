#!/usr/bin/env python3
"""Label reviewer server — browse and review onset/beat labels against audio.

Serves a web UI for visually reviewing training labels. Shows waveform + spectrogram
with beat and onset markers overlaid. Supports pagination through all tracks,
flagging as reviewed/problematic, and (future) manual label correction.

Usage:
    cd ml-training && python tools/label-reviewer/server.py
    python tools/label-reviewer/server.py --port 8765
    python tools/label-reviewer/server.py --audio-dir /path/to/audio

Then open http://localhost:8765 in browser.
"""

from __future__ import annotations

import argparse
import json
import mimetypes
from http.server import HTTPServer, SimpleHTTPRequestHandler
from pathlib import Path
from urllib.parse import parse_qs, urlparse

# Defaults
AUDIO_DIR = Path("/mnt/storage/blinky-ml-data/audio/combined")
BEAT_LABELS_DIR = Path("/mnt/storage/blinky-ml-data/labels/consensus_v5")
ONSET_LABELS_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_consensus")
LIBROSA_ONSET_DIR = Path("/mnt/storage/blinky-ml-data/labels/onsets_librosa")
REVIEW_STATE_PATH = Path("/mnt/storage/blinky-ml-data/labels/review_state.json")
STATIC_DIR = Path(__file__).parent

# Global state
tracks: list[dict] = []
review_state: dict = {}


def load_tracks():
    """Build track index from beat labels + audio files."""
    global tracks
    audio_extensions = {".mp3", ".wav", ".flac", ".ogg"}
    audio_index = {}
    for f in AUDIO_DIR.iterdir():
        if f.suffix.lower() in audio_extensions:
            audio_index[f.stem] = f

    tracks = []
    for label_file in sorted(BEAT_LABELS_DIR.glob("*.beats.json")):
        stem = label_file.stem.replace(".beats", "")
        if stem not in audio_index:
            continue

        has_onset_consensus = (ONSET_LABELS_DIR / f"{stem}.onsets.json").exists()
        has_librosa_onsets = (LIBROSA_ONSET_DIR / f"{stem}.onsets.json").exists()

        tracks.append({
            "stem": stem,
            "audio_path": str(audio_index[stem]),
            "audio_ext": audio_index[stem].suffix,
            "has_beat_labels": True,
            "has_onset_consensus": has_onset_consensus,
            "has_librosa_onsets": has_librosa_onsets,
            "review_status": review_state.get(stem, {}).get("status", "unreviewed"),
        })


def load_review_state():
    """Load review state from disk."""
    global review_state
    if REVIEW_STATE_PATH.exists():
        with open(REVIEW_STATE_PATH) as f:
            review_state = json.load(f)


def save_review_state():
    """Save review state to disk."""
    REVIEW_STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(REVIEW_STATE_PATH, "w") as f:
        json.dump(review_state, f, indent=2)


def get_track_labels(stem: str) -> dict:
    """Load all available labels for a track."""
    result = {"stem": stem, "beats": [], "onsets_consensus": [], "onsets_librosa": []}

    # Beat labels (consensus_v5)
    beat_path = BEAT_LABELS_DIR / f"{stem}.beats.json"
    if beat_path.exists():
        with open(beat_path) as f:
            data = json.load(f)
        result["beats"] = [
            {"time": h["time"], "strength": h.get("strength", 1.0),
             "isDownbeat": h.get("isDownbeat", False)}
            for h in data.get("hits", []) if h.get("expectTrigger", True)
        ]

    # Onset consensus labels
    onset_path = ONSET_LABELS_DIR / f"{stem}.onsets.json"
    if onset_path.exists():
        with open(onset_path) as f:
            data = json.load(f)
        result["onsets_consensus"] = data.get("onsets", [])
        result["onset_systems"] = data.get("total_systems", 0)
        result["onset_systems_succeeded"] = data.get("systems_succeeded", 0)

    # Librosa onsets (simpler format)
    librosa_path = LIBROSA_ONSET_DIR / f"{stem}.onsets.json"
    if librosa_path.exists():
        with open(librosa_path) as f:
            data = json.load(f)
        result["onsets_librosa"] = [{"time": t} for t in data.get("onsets", [])]

    return result


class LabelReviewerHandler(SimpleHTTPRequestHandler):
    """HTTP handler for the label reviewer."""

    def do_GET(self):
        parsed = urlparse(self.path)
        path = parsed.path
        params = parse_qs(parsed.query)

        if path == "/" or path == "/index.html":
            self.serve_file(STATIC_DIR / "index.html", "text/html")
        elif path == "/api/tracks":
            page = int(params.get("page", ["0"])[0])
            per_page = int(params.get("per_page", ["50"])[0])
            status_filter = params.get("status", [None])[0]

            filtered = tracks
            if status_filter and status_filter != "all":
                filtered = [t for t in tracks if t["review_status"] == status_filter]

            start = page * per_page
            end = start + per_page
            self.send_json({
                "tracks": filtered[start:end],
                "total": len(filtered),
                "page": page,
                "per_page": per_page,
            })
        elif path == "/api/labels":
            stem = params.get("stem", [None])[0]
            if not stem:
                self.send_json({"error": "stem parameter required"}, 400)
                return
            labels = get_track_labels(stem)
            labels["review"] = review_state.get(stem, {})
            self.send_json(labels)
        elif path.startswith("/audio/"):
            stem = path.replace("/audio/", "").split(".")[0]
            # Find audio file
            for t in tracks:
                if t["stem"] == stem:
                    audio_path = Path(t["audio_path"])
                    if audio_path.exists():
                        content_type = mimetypes.guess_type(str(audio_path))[0] or "audio/mpeg"
                        self.send_response(200)
                        self.send_header("Content-Type", content_type)
                        self.send_header("Content-Length", str(audio_path.stat().st_size))
                        self.send_header("Accept-Ranges", "bytes")
                        self.end_headers()
                        with open(audio_path, "rb") as f:
                            self.wfile.write(f.read())
                        return
            self.send_error(404)
        else:
            # Try to serve static files
            file_path = STATIC_DIR / path.lstrip("/")
            if file_path.exists() and file_path.is_file():
                content_type = mimetypes.guess_type(str(file_path))[0] or "text/plain"
                self.serve_file(file_path, content_type)
            else:
                self.send_error(404)

    def do_POST(self):
        parsed = urlparse(self.path)
        path = parsed.path

        if path == "/api/review":
            content_length = int(self.headers.get("Content-Length", 0))
            body = json.loads(self.rfile.read(content_length))
            stem = body.get("stem")
            if not stem:
                self.send_json({"error": "stem required"}, 400)
                return

            review_state[stem] = {
                "status": body.get("status", "reviewed"),
                "notes": body.get("notes", ""),
                "flags": body.get("flags", []),
            }
            # Update track list
            for t in tracks:
                if t["stem"] == stem:
                    t["review_status"] = review_state[stem]["status"]
                    break
            save_review_state()
            self.send_json({"ok": True})
        else:
            self.send_error(404)

    def send_json(self, data, status=200):
        body = json.dumps(data).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def serve_file(self, path: Path, content_type: str):
        with open(path, "rb") as f:
            content = f.read()
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(content)))
        self.end_headers()
        self.wfile.write(content)

    def log_message(self, format, *args):
        # Suppress access logs for cleaner output
        if "/api/" in str(args[0]) or "/audio/" in str(args[0]):
            return
        super().log_message(format, *args)


def main():
    global AUDIO_DIR, BEAT_LABELS_DIR, ONSET_LABELS_DIR, LIBROSA_ONSET_DIR, REVIEW_STATE_PATH

    parser = argparse.ArgumentParser(description="Label reviewer web UI")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--audio-dir", type=Path, default=AUDIO_DIR)
    parser.add_argument("--beat-labels", type=Path, default=BEAT_LABELS_DIR)
    parser.add_argument("--onset-labels", type=Path, default=ONSET_LABELS_DIR)
    parser.add_argument("--librosa-onsets", type=Path, default=LIBROSA_ONSET_DIR)
    args = parser.parse_args()

    AUDIO_DIR = args.audio_dir
    BEAT_LABELS_DIR = args.beat_labels
    ONSET_LABELS_DIR = args.onset_labels
    LIBROSA_ONSET_DIR = args.librosa_onsets

    load_review_state()
    load_tracks()

    print(f"Label Reviewer")
    print(f"  Tracks: {len(tracks)}")
    print(f"  With onset consensus: {sum(1 for t in tracks if t['has_onset_consensus'])}")
    print(f"  Reviewed: {sum(1 for t in tracks if t['review_status'] != 'unreviewed')}")
    print(f"  http://localhost:{args.port}")

    server = HTTPServer(("0.0.0.0", args.port), LabelReviewerHandler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nShutdown.")


if __name__ == "__main__":
    main()
