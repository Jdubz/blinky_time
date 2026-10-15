/**
 * Record-mode wire protocol (phone ⇄ device over BLE).
 *
 * Mirror of the canonical layout in blinky-things/comms/RecordStream.h —
 * keep in sync, and bump RECORD_PROTOCOL_VERSION there and here together.
 * See docs/PHONE_RECORD_MODE_PLAN.md.
 *
 * Every BLE notification on the record data characteristic is exactly one
 * packet: a 4-byte header (type, flags, per-type seq) + a typed payload.
 * All multi-byte fields are little-endian.
 */

export const RECORD_PROTOCOL_VERSION = 1;

export const NUS_SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e';
export const NUS_RX_CHAR = '6e400002-b5a3-f393-e0a9-e50e24dcca9e'; // phone → device (write)
export const NUS_TX_CHAR = '6e400003-b5a3-f393-e0a9-e50e24dcca9e'; // device → phone (notify)
export const RECORD_SERVICE = 'b1a70000-7d1e-4f0e-9c3a-5265636f7264';
export const RECORD_DATA_CHAR = 'b1a70001-7d1e-4f0e-9c3a-5265636f7264';

export const SAMPLE_RATE = 16000;

export enum PacketType {
  AUDIO = 1,
  FRAME = 2,
  STATUS = 3,
  END = 4,
}

export const FrameFlags = {
  NN_READY: 0x01,
  PULSE: 0x02,
  FFT_OVERRUN: 0x04,
  HAS_MEL: 0x08,
} as const;

const HEADER_LEN = 4;
const AUDIO_HDR_LEN = HEADER_LEN + 4;
const FRAME_FIXED_LEN = HEADER_LEN + 22;
const STATUS_LEN = HEADER_LEN + 44;

export interface PacketHeader {
  type: PacketType;
  flags: number;
  seq: number;
}

export interface AudioPacket extends PacketHeader {
  type: PacketType.AUDIO;
  sampleIndex: number;
  /** View into the packet buffer — copy if you keep it past the event. */
  samples: Int16Array;
}

export interface FramePacket extends PacketHeader {
  type: PacketType.FRAME;
  sampleIndex: number;
  ms: number;
  nna: number;
  pulse: number;
  level: number;
  rawLevel: number;
  bpm: number;
  phase: number;
  rhythmStrength: number;
  skipped: number;
  /** Raw mel bands 0..1, present iff flags & HAS_MEL. */
  mel?: Float32Array;
}

export interface StatusPacket extends PacketHeader {
  type: PacketType.STATUS | PacketType.END;
  ms: number;
  audioSamplesSent: number;
  audioSamplesDropped: number;
  framesQueued: number;
  framesDropped: number;
  clipSamples: number;
  fftOverruns: number;
  fftOverrunSamples: number;
  peakAbs: number;
  fps: number;
  maxFrameMs: number;
  ringHighWater: number;
  rssi: number;
  hwGain: number;
}

export type RecordPacket = AudioPacket | FramePacket | StatusPacket;

export class RecordProtocolError extends Error {
  constructor(message: string) {
    super(message);
    this.name = 'RecordProtocolError';
  }
}

/**
 * Parse one notification payload. Throws {@link RecordProtocolError} on a
 * malformed packet — callers must count/surface it, never ignore it.
 */
export function parsePacket(view: DataView): RecordPacket {
  if (view.byteLength < HEADER_LEN) {
    throw new RecordProtocolError(`packet too short (${view.byteLength} B)`);
  }
  const type = view.getUint8(0);
  const flags = view.getUint8(1);
  const seq = view.getUint16(2, true);

  switch (type) {
    case PacketType.AUDIO: {
      const payload = view.byteLength - AUDIO_HDR_LEN;
      if (payload < 0 || payload % 2 !== 0) {
        throw new RecordProtocolError(`bad AUDIO length ${view.byteLength}`);
      }
      const sampleIndex = view.getUint32(HEADER_LEN, true);
      const n = payload / 2;
      // Int16Array needs 2-byte alignment; copy if the view is misaligned.
      const off = view.byteOffset + AUDIO_HDR_LEN;
      const samples =
        off % 2 === 0
          ? new Int16Array(view.buffer, off, n)
          : new Int16Array(view.buffer.slice(off, off + payload));
      return { type, flags, seq, sampleIndex, samples };
    }
    case PacketType.FRAME: {
      if (view.byteLength < FRAME_FIXED_LEN) {
        throw new RecordProtocolError(`bad FRAME length ${view.byteLength}`);
      }
      const o = HEADER_LEN;
      const pkt: FramePacket = {
        type,
        flags,
        seq,
        sampleIndex: view.getUint32(o, true),
        ms: view.getUint32(o + 4, true),
        nna: view.getUint16(o + 8, true) / 65535,
        pulse: view.getUint16(o + 10, true) / 65535,
        level: view.getUint16(o + 12, true) / 65535,
        rawLevel: view.getUint16(o + 14, true) / 65535,
        bpm: view.getUint16(o + 16, true) / 10,
        phase: view.getUint8(o + 18) / 255,
        rhythmStrength: view.getUint8(o + 19) / 255,
        skipped: view.getUint8(o + 20),
      };
      if (flags & FrameFlags.HAS_MEL) {
        const nMel = view.byteLength - FRAME_FIXED_LEN;
        if (nMel <= 0) throw new RecordProtocolError('FRAME has HAS_MEL but no mel bytes');
        const mel = new Float32Array(nMel);
        for (let i = 0; i < nMel; i++) mel[i] = view.getUint8(FRAME_FIXED_LEN + i) / 255;
        pkt.mel = mel;
      }
      return pkt;
    }
    case PacketType.STATUS:
    case PacketType.END: {
      if (view.byteLength < STATUS_LEN) {
        throw new RecordProtocolError(`bad STATUS/END length ${view.byteLength}`);
      }
      const o = HEADER_LEN;
      return {
        type,
        flags,
        seq,
        ms: view.getUint32(o, true),
        audioSamplesSent: view.getUint32(o + 4, true),
        audioSamplesDropped: view.getUint32(o + 8, true),
        framesQueued: view.getUint32(o + 12, true),
        framesDropped: view.getUint32(o + 16, true),
        clipSamples: view.getUint32(o + 20, true),
        fftOverruns: view.getUint32(o + 24, true),
        fftOverrunSamples: view.getUint32(o + 28, true),
        peakAbs: view.getUint16(o + 32, true),
        fps: view.getUint16(o + 34, true) / 10,
        maxFrameMs: view.getUint16(o + 36, true),
        ringHighWater: view.getUint16(o + 38, true),
        rssi: view.getInt8(o + 40),
        hwGain: view.getUint8(o + 41),
      };
    }
    default:
      throw new RecordProtocolError(`unknown packet type ${type}`);
  }
}

/** Reply line the device sends on a successful `record start`. */
export interface RecordStartReply {
  record: 'started';
  v: number;
  fw: string;
  sn: string;
  sr: number;
  gain: number;
  mtu: number;
  spp: number;
  mel: number;
  startIdx: number;
  ms: number;
  ring: number;
}

export function isStartReply(obj: unknown): obj is RecordStartReply {
  return (
    typeof obj === 'object' &&
    obj !== null &&
    (obj as { record?: unknown }).record === 'started' &&
    typeof (obj as { startIdx?: unknown }).startIdx === 'number'
  );
}

/** Modular uint32 difference a - b, as a signed number. */
export function u32Diff(a: number, b: number): number {
  return ((a - b) | 0) as number;
}

/** Modular uint16 difference a - b in [0, 65535]. */
export function u16Diff(a: number, b: number): number {
  return (a - b) & 0xffff;
}

export function peakToDbfs(peakAbs: number): number {
  return peakAbs > 0 ? 20 * Math.log10(peakAbs / 32768) : -Infinity;
}
