/**
 * `.blrec` session file format (version 1). Parsed offline by
 * ml-training/scripts/convert_blrec.py — keep in sync.
 *
 *   magic        5 B   "BLREC"
 *   fileVersion  u8    1
 *   headerLen    u32   LE
 *   header       JSON  UTF-8 (SessionMeta: device, segments + start replies, ...)
 *   records...         repeated until EOF:
 *     len        u16   payload length
 *     kind       u8    0 = device record packet, 1 = device text line (UTF-8)
 *     segment    u8    segment index (one per `record start`)
 *     rxMs       u32   phone receive time, ms since session creation
 *     payload    len B
 *
 * Phone receive time is informational only (BLE delivery jitter). Alignment
 * between audio and analysis uses device sample indices in the payloads.
 */

import type { SessionMeta } from './recordStore';

export const BLREC_MAGIC = 'BLREC';
export const BLREC_FILE_VERSION = 1;
export const RECORD_HDR_LEN = 8;

export enum RecordKind {
  PACKET = 0,
  TEXT = 1,
}

export function encodeRecord(
  kind: RecordKind,
  segment: number,
  rxMs: number,
  payload: Uint8Array
): Uint8Array {
  if (payload.length > 0xffff) throw new Error(`record payload too large (${payload.length})`);
  const out = new Uint8Array(RECORD_HDR_LEN + payload.length);
  const dv = new DataView(out.buffer);
  dv.setUint16(0, payload.length, true);
  dv.setUint8(2, kind);
  dv.setUint8(3, segment & 0xff);
  dv.setUint32(4, Math.max(0, Math.round(rxMs)) >>> 0, true);
  out.set(payload, RECORD_HDR_LEN);
  return out;
}

export function concatChunks(parts: Uint8Array[]): ArrayBuffer {
  const total = parts.reduce((n, p) => n + p.length, 0);
  const out = new Uint8Array(total);
  let off = 0;
  for (const p of parts) {
    out.set(p, off);
    off += p.length;
  }
  return out.buffer;
}

export function buildBlrec(meta: SessionMeta, chunks: ArrayBuffer[]): Blob {
  const header = new TextEncoder().encode(
    JSON.stringify({ format: 'blrec', fileVersion: BLREC_FILE_VERSION, ...meta })
  );
  const pre = new Uint8Array(BLREC_MAGIC.length + 1 + 4);
  for (let i = 0; i < BLREC_MAGIC.length; i++) pre[i] = BLREC_MAGIC.charCodeAt(i);
  const dv = new DataView(pre.buffer);
  dv.setUint8(BLREC_MAGIC.length, BLREC_FILE_VERSION);
  dv.setUint32(BLREC_MAGIC.length + 1, header.length, true);
  return new Blob([pre, header, ...chunks], { type: 'application/octet-stream' });
}

export interface DecodedRecord {
  kind: RecordKind;
  segment: number;
  rxMs: number;
  payload: DataView;
}

/** Iterate records in a concatenated chunk buffer (used by tests/tools). */
export function* iterRecords(buf: ArrayBuffer): Generator<DecodedRecord> {
  const dv = new DataView(buf);
  let off = 0;
  while (off < buf.byteLength) {
    if (off + RECORD_HDR_LEN > buf.byteLength) throw new Error(`truncated record header at ${off}`);
    const len = dv.getUint16(off, true);
    const end = off + RECORD_HDR_LEN + len;
    if (end > buf.byteLength) throw new Error(`truncated record payload at ${off}`);
    yield {
      kind: dv.getUint8(off + 2) as RecordKind,
      segment: dv.getUint8(off + 3),
      rxMs: dv.getUint32(off + 4, true),
      payload: new DataView(buf, off + RECORD_HDR_LEN, len),
    };
    off = end;
  }
}
