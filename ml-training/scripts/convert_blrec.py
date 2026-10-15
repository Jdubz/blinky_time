#!/usr/bin/env python3
"""Convert a phone record-mode session (.blrec) into analysis-ready files.

A .blrec is exported by the console's Field Recorder page (#record) after a
Web Bluetooth session against one device. See docs/PHONE_RECORD_MODE_PLAN.md.
Wire formats mirrored here (keep in sync):
  - packets: blinky-things/comms/RecordStream.h (RecordProto, VERSION 1)
  - file:    blinky-console/src/services/record/blrecFormat.ts (fileVersion 1)

Outputs (in --out, default <input stem>/):
  seg{S}_run{R}.wav   16 kHz mono int16 — one file per CONTIGUOUS run of
                      device samples. Gaps are never zero-filled.
  frames_segNN.jsonl  per-segment analysis frames (segments are not contiguous —
                      consume these individually). frames.jsonl has all segments.
  frames.jsonl        device analysis per spectral frame. capture-nn-style
                      field names (ts, nna, lvl, gain, onset, bpm, phase,
                      rstr, mel) plus sample_index (relative to the segment
                      start) and seg — usable by replay_device_capture.py /
                      mel_distribution_check.py when recorded with mel.
  status.jsonl        1 Hz device counters + the END packet per segment.
  text.jsonl          device text lines received during the session.
  manifest.json       session header, runs, gaps, integrity + clipping summary.

Exit status is 0 even when data loss occurred (the data is still useful),
but every loss is printed as a WARNING and recorded in manifest.json.

Usage:
    python scripts/convert_blrec.py session.blrec
    python scripts/convert_blrec.py session.blrec --out /mnt/nvme/field/2026-10-18-club
"""

from __future__ import annotations

import argparse
import json
import math
import struct
import sys
import wave
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

MAGIC = b"BLREC"
FILE_VERSION = 1
PROTOCOL_VERSION = 1
SAMPLE_RATE = 16000

KIND_PACKET = 0
KIND_TEXT = 1

PKT_AUDIO, PKT_FRAME, PKT_STATUS, PKT_END = 1, 2, 3, 4
FRAME_NN_READY, FRAME_PULSE, FRAME_FFT_OVERRUN, FRAME_HAS_MEL = 0x01, 0x02, 0x04, 0x08

HDR = struct.Struct("<BBH")  # type, flags, seq
REC_HDR = struct.Struct("<HBBI")  # len, kind, segment, rxMs
FRAME_FIXED = struct.Struct("<IIHHHHHBBBB")  # 22 B
STATUS = struct.Struct("<IIIIIIIIHHHHbBH")  # 44 B
assert FRAME_FIXED.size == 22 and STATUS.size == 44

STATUS_FIELDS = (
    "ms", "audioSamplesSent", "audioSamplesDropped", "framesQueued", "framesDropped",
    "clipSamples", "fftOverruns", "fftOverrunSamples", "peakAbs", "fpsX10",
    "maxFrameMs", "ringHighWater", "rssi", "hwGain", "_reserved",
)

CLIP_THRESHOLD = 32767


class BlrecError(Exception):
    pass


@dataclass
class Run:
    start: int  # device sample index (relative to segment start)
    chunks: list = field(default_factory=list)

    @property
    def n(self) -> int:
        return sum(len(c) for c in self.chunks)


@dataclass
class Segment:
    index: int
    start_idx: int | None = None  # absolute device sample index (from start reply)
    gain: int | None = None
    runs: list = field(default_factory=list)
    gaps: list = field(default_factory=list)  # {"at": rel sample, "missing": n}
    expected: int | None = None  # next absolute sample index
    seq_expect: dict = field(default_factory=dict)
    seq_gaps: dict = field(default_factory=dict)
    end: dict | None = None
    last_status: dict | None = None
    frames: int = 0
    frames_skipped_nn: int = 0
    fft_overrun_frames: int = 0

    def rel(self, abs_idx: int) -> int:
        """Signed sample offset from the segment start (uint32 wrap-safe).

        Negative for the first frame(s) of a segment, whose FFT window ended
        before the record tap was armed.
        """
        base = self.start_idx if self.start_idx is not None else 0
        d = (abs_idx - base) & 0xFFFFFFFF
        return d - 0x100000000 if d >= 0x80000000 else d


def read_blrec(path: Path):
    data = path.read_bytes()
    if data[:5] != MAGIC:
        raise BlrecError(f"{path}: not a .blrec file (bad magic)")
    ver = data[5]
    if ver != FILE_VERSION:
        raise BlrecError(f"{path}: unsupported fileVersion {ver} (expected {FILE_VERSION})")
    (hlen,) = struct.unpack_from("<I", data, 6)
    header = json.loads(data[10:10 + hlen].decode("utf-8"))
    off = 10 + hlen
    while off < len(data):
        if off + REC_HDR.size > len(data):
            raise BlrecError(f"truncated record header at byte {off}")
        ln, kind, seg, rx = REC_HDR.unpack_from(data, off)
        end = off + REC_HDR.size + ln
        if end > len(data):
            raise BlrecError(f"truncated record payload at byte {off}")
        yield header, kind, seg, rx, data[off + REC_HDR.size:end]
        off = end
    if off == 10 + hlen:
        yield header, None, None, None, None  # header-only file


def write_wav(path: Path, samples: np.ndarray) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(samples.astype("<i2").tobytes())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("blrec", type=Path)
    ap.add_argument("--out", type=Path, default=None)
    args = ap.parse_args()

    out = args.out or args.blrec.with_suffix("")
    out.mkdir(parents=True, exist_ok=True)

    segments: dict[int, Segment] = {}
    header: dict | None = None
    protocol_errors: list[str] = []
    frames_f = open(out / "frames.jsonl", "w", encoding="utf-8")
    seg_frame_files: dict = {}
    status_f = open(out / "status.jsonl", "w", encoding="utf-8")
    text_f = open(out / "text.jsonl", "w", encoding="utf-8")

    def seg_for(i: int) -> Segment:
        if i not in segments:
            s = Segment(index=i)
            meta = next((m for m in (header or {}).get("segments", []) if m.get("index") == i), None)
            if meta:
                reply = meta.get("startReply", {})
                if reply.get("v") != PROTOCOL_VERSION:
                    raise BlrecError(f"segment {i}: protocol v{reply.get('v')} != {PROTOCOL_VERSION}")
                s.start_idx = int(reply["startIdx"])
                s.expected = s.start_idx
                s.gain = reply.get("gain")
            else:
                protocol_errors.append(f"segment {i}: no start reply in header (gaps measured from first packet)")
            segments[i] = s
        return segments[i]

    try:
        for hdr, kind, segi, rx, payload in read_blrec(args.blrec):
            header = hdr
            if kind is None:
                break
            if kind == KIND_TEXT:
                text_f.write(json.dumps({"seg": segi, "rx_ms": rx, "line": payload.decode("utf-8", "replace")}) + "\n")
                continue
            if kind != KIND_PACKET:
                protocol_errors.append(f"unknown record kind {kind} at rx_ms={rx}")
                continue
            seg = seg_for(segi)
            if len(payload) < HDR.size:
                protocol_errors.append(f"seg {segi}: short packet ({len(payload)} B)")
                continue
            ptype, flags, seq = HDR.unpack_from(payload, 0)
            # Firmware resets every per-type sequence to 0 at session start,
            # so the first packet of a type is expected to be seq 0 — a first
            # FRAME with seq 3 means 3 FRAMEs were lost.
            gap = (seq - seg.seq_expect.get(ptype, 0)) & 0xFFFF
            if gap:
                seg.seq_gaps[ptype] = seg.seq_gaps.get(ptype, 0) + gap
            seg.seq_expect[ptype] = (seq + 1) & 0xFFFF
            body = payload[HDR.size:]

            if ptype == PKT_AUDIO:
                if len(body) < 4 or (len(body) - 4) % 2:
                    protocol_errors.append(f"seg {segi}: bad AUDIO length {len(payload)}")
                    continue
                (idx,) = struct.unpack_from("<I", body, 0)
                samples = np.frombuffer(body[4:], dtype="<i2")
                if seg.expected is None:
                    seg.expected = idx
                    seg.start_idx = idx if seg.start_idx is None else seg.start_idx
                d = (idx - seg.expected) & 0xFFFFFFFF
                if d >= 0x80000000:
                    protocol_errors.append(f"seg {segi}: audio rewind/overlap at idx {idx}")
                    continue
                if d > 0:
                    seg.gaps.append({"at": seg.rel(seg.expected), "missing": d})
                if d > 0 or not seg.runs:
                    seg.runs.append(Run(start=seg.rel(idx)))
                seg.runs[-1].chunks.append(samples.copy())
                seg.expected = (idx + len(samples)) & 0xFFFFFFFF

            elif ptype == PKT_FRAME:
                if len(body) < FRAME_FIXED.size:
                    protocol_errors.append(f"seg {segi}: bad FRAME length {len(payload)}")
                    continue
                (sidx, ms, nna, pulse, lvl, raw, bpm10, phase, rstr, skipped, _r) = FRAME_FIXED.unpack_from(body, 0)
                rel = seg.rel(sidx)
                rec = {
                    "type": "NN",
                    "seg": segi,
                    "sample_index": rel,
                    # `ts` on the 16 kHz sample clock (ms since segment start),
                    # so frame spacing is the true 16 ms hop even for catch-up
                    # frames processed back-to-back. Device processing time is
                    # kept separately. Segments are NOT contiguous with each
                    # other: consume frames_segNN.jsonl per segment.
                    "ts": round(rel / (SAMPLE_RATE / 1000.0), 3),
                    "proc_ms": ms,
                    "nna": nna / 65535,
                    "onset": pulse / 65535,
                    "lvl": lvl / 65535,
                    "raw": raw / 65535,
                    "bpm": bpm10 / 10,
                    "phase": phase / 255,
                    "rstr": rstr / 255,
                    "gain": seg.gain,
                    "nn": 1 if flags & FRAME_NN_READY else 0,
                    "pulse": 1 if flags & FRAME_PULSE else 0,
                    "fft_overrun": 1 if flags & FRAME_FFT_OVERRUN else 0,
                    "skipped": skipped,
                }
                if flags & FRAME_HAS_MEL:
                    mel = body[FRAME_FIXED.size:]
                    if not mel:
                        protocol_errors.append(f"seg {segi}: FRAME HAS_MEL with no mel bytes")
                    rec["mel"] = [round(b / 255, 4) for b in mel]
                seg.frames += 1
                seg.frames_skipped_nn += skipped
                seg.fft_overrun_frames += rec["fft_overrun"]
                line = json.dumps(rec) + "\n"
                frames_f.write(line)
                if segi not in seg_frame_files:
                    seg_frame_files[segi] = open(out / f"frames_seg{segi:02d}.jsonl", "w", encoding="utf-8")
                seg_frame_files[segi].write(line)

            elif ptype in (PKT_STATUS, PKT_END):
                if len(body) < STATUS.size:
                    protocol_errors.append(f"seg {segi}: bad STATUS length {len(payload)}")
                    continue
                st = dict(zip(STATUS_FIELDS, STATUS.unpack_from(body, 0)))
                st.pop("_reserved")
                st["fps"] = st.pop("fpsX10") / 10
                st["type"] = "END" if ptype == PKT_END else "STATUS"
                st["seg"] = segi
                st["rx_ms"] = rx
                status_f.write(json.dumps(st) + "\n")
                if ptype == PKT_END:
                    seg.end = st
                else:
                    seg.last_status = st
            else:
                protocol_errors.append(f"seg {segi}: unknown packet type {ptype}")
    finally:
        frames_f.close()
        for fh in seg_frame_files.values():
            fh.close()
        status_f.close()
        text_f.close()

    if header is None:
        raise BlrecError("empty file")

    manifest_segments = []
    total_samples = total_gap = total_clips = 0
    global_peak = 0
    warnings: list[str] = []
    for i in sorted(segments):
        seg = segments[i]
        runs_out = []
        for r_i, run in enumerate(seg.runs):
            audio = np.concatenate(run.chunks) if run.chunks else np.zeros(0, dtype="<i2")
            fname = f"seg{i:02d}_run{r_i:02d}.wav"
            write_wav(out / fname, audio)
            a = np.abs(audio.astype(np.int32))
            clips = int(np.count_nonzero(a >= CLIP_THRESHOLD))
            peak = int(a.max()) if len(a) else 0
            rms = float(np.sqrt(np.mean((audio.astype(np.float64) / 32768) ** 2))) if len(audio) else 0.0
            runs_out.append({
                "file": fname,
                "start_sample": run.start,
                "samples": int(len(audio)),
                "seconds": len(audio) / SAMPLE_RATE,
                "clip_samples": clips,
                "clip_fraction": clips / len(audio) if len(audio) else 0.0,
                "peak_dbfs": 20 * math.log10(peak / 32768) if peak else None,
                "rms_dbfs": 20 * math.log10(rms) if rms > 0 else None,
            })
            total_samples += len(audio)
            total_clips += clips
            global_peak = max(global_peak, peak)
        gap_samples = sum(g["missing"] for g in seg.gaps)
        total_gap += gap_samples
        dev = seg.end or seg.last_status or {}
        if gap_samples:
            warnings.append(f"segment {i}: {gap_samples} audio samples missing in {len(seg.gaps)} gap(s)")
        if seg.seq_gaps:
            warnings.append(f"segment {i}: packet sequence gaps {seg.seq_gaps} (type: count)")
        if dev.get("framesDropped"):
            warnings.append(f"segment {i}: device dropped {dev['framesDropped']} analysis/status packets")
        if seg.end is None:
            warnings.append(f"segment {i}: no END packet (link lost or app closed) — tail may be missing")
        else:
            # Index/sequence checks can't see a missing TAIL (no later packet
            # to reveal the gap). END carries the device's own totals.
            recovered = sum(len(c) for run in seg.runs for c in run.chunks)
            sent = seg.end.get("audioSamplesSent", 0)
            if recovered != sent:
                warnings.append(f"segment {i}: recovered {recovered} audio samples but device reports {sent} sent")
            dev_dropped = seg.end.get("audioSamplesDropped", 0)
            if dev_dropped > gap_samples:
                warnings.append(
                    f"segment {i}: device dropped {dev_dropped} samples but only {gap_samples} are located as gaps")
        if seg.fft_overrun_frames:
            warnings.append(f"segment {i}: {seg.fft_overrun_frames} frames flagged FFT overrun (device analysis corrupted there)")
        manifest_segments.append({
            "index": i,
            "start_idx": seg.start_idx,
            "gain": seg.gain,
            "runs": runs_out,
            "gaps": seg.gaps,
            "gap_samples": gap_samples,
            "seq_gaps": {str(k): v for k, v in seg.seq_gaps.items()},
            "frames": seg.frames,
            "frames_skipped_by_nn_catchup": seg.frames_skipped_nn,
            "fft_overrun_frames": seg.fft_overrun_frames,
            "device_final_counters": dev,
            "ended_cleanly": seg.end is not None,
        })

    warnings.extend(protocol_errors)
    manifest = {
        "source": str(args.blrec),
        "header": header,
        "segments": manifest_segments,
        "summary": {
            "audio_seconds": total_samples / SAMPLE_RATE,
            "gap_samples": total_gap,
            "clip_samples": total_clips,
            "clip_fraction": total_clips / total_samples if total_samples else 0.0,
            "peak_dbfs": 20 * math.log10(global_peak / 32768) if global_peak else None,
            "protocol_errors": len(protocol_errors),
            "lossless": not warnings,
        },
        "warnings": warnings,
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2))

    s = manifest["summary"]
    print(f"Wrote {out}")
    print(f"  audio: {s['audio_seconds']:.1f} s in {sum(len(m['runs']) for m in manifest_segments)} run(s), "
          f"{len(manifest_segments)} segment(s)")
    pk = f"{s['peak_dbfs']:.1f} dBFS" if s["peak_dbfs"] is not None else "n/a"
    print(f"  clipping: {s['clip_samples']} samples ({100 * s['clip_fraction']:.3f}%), peak {pk}")
    for w in warnings:
        print(f"  WARNING: {w}", file=sys.stderr)
    if not warnings:
        print("  integrity: lossless")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except BlrecError as e:
        print(f"ERROR: {e}", file=sys.stderr)
        sys.exit(2)
