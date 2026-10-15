import { describe, expect, it } from 'vitest';
import { encodeRecord, iterRecords, RecordKind, concatChunks } from './blrecFormat';
import {
  FrameFlags,
  PacketType,
  parsePacket,
  RecordProtocolError,
  u16Diff,
  u32Diff,
} from './recordProtocol';

// Builders mirror the packed structs in blinky-things/comms/RecordStream.h.
function audioPacket(seq: number, sampleIndex: number, samples: number[]): DataView {
  const dv = new DataView(new ArrayBuffer(8 + samples.length * 2));
  dv.setUint8(0, PacketType.AUDIO);
  dv.setUint16(2, seq, true);
  dv.setUint32(4, sampleIndex, true);
  samples.forEach((s, i) => dv.setInt16(8 + i * 2, s, true));
  return dv;
}

function framePacket(withMel: boolean): DataView {
  const len = 4 + 22 + (withMel ? 50 : 0);
  const dv = new DataView(new ArrayBuffer(len));
  dv.setUint8(0, PacketType.FRAME);
  dv.setUint8(1, FrameFlags.NN_READY | FrameFlags.PULSE | (withMel ? FrameFlags.HAS_MEL : 0));
  dv.setUint16(2, 7, true);
  dv.setUint32(4, 123456, true); // sampleIndex
  dv.setUint32(8, 9999, true); // ms
  dv.setUint16(12, 65535, true); // nna
  dv.setUint16(14, 32768, true); // pulse
  dv.setUint16(16, 0, true); // level
  dv.setUint16(18, 6554, true); // rawLevel
  dv.setUint16(20, 1285, true); // bpm10
  dv.setUint8(22, 255); // phase
  dv.setUint8(23, 0); // rstr
  dv.setUint8(24, 3); // skipped
  if (withMel) for (let i = 0; i < 50; i++) dv.setUint8(26 + i, i * 5);
  return dv;
}

function statusPacket(type: PacketType.STATUS | PacketType.END): DataView {
  const dv = new DataView(new ArrayBuffer(48));
  dv.setUint8(0, type);
  dv.setUint16(2, 1, true);
  const o = 4;
  dv.setUint32(o, 5000, true);
  dv.setUint32(o + 4, 80000, true);
  dv.setUint32(o + 8, 12, true);
  dv.setUint32(o + 12, 300, true);
  dv.setUint32(o + 16, 2, true);
  dv.setUint32(o + 20, 44, true);
  dv.setUint32(o + 24, 1, true);
  dv.setUint32(o + 28, 256, true);
  dv.setUint16(o + 32, 32767, true);
  dv.setUint16(o + 34, 612, true);
  dv.setUint16(o + 36, 41, true);
  dv.setUint16(o + 38, 900, true);
  dv.setInt8(o + 40, -67);
  dv.setUint8(o + 41, 32);
  return dv;
}

describe('parsePacket', () => {
  it('parses AUDIO with sample index and samples', () => {
    const p = parsePacket(audioPacket(5, 0xfffffff0, [1, -2, 32767, -32768]));
    expect(p.type).toBe(PacketType.AUDIO);
    if (p.type !== PacketType.AUDIO) return;
    expect(p.seq).toBe(5);
    expect(p.sampleIndex).toBe(0xfffffff0);
    expect(Array.from(p.samples)).toEqual([1, -2, 32767, -32768]);
  });

  it('rejects odd-length AUDIO', () => {
    const dv = new DataView(new ArrayBuffer(9));
    dv.setUint8(0, PacketType.AUDIO);
    expect(() => parsePacket(dv)).toThrow(RecordProtocolError);
  });

  it('parses FRAME without and with mel', () => {
    const a = parsePacket(framePacket(false));
    expect(a.type).toBe(PacketType.FRAME);
    if (a.type !== PacketType.FRAME) return;
    expect(a.sampleIndex).toBe(123456);
    expect(a.ms).toBe(9999);
    expect(a.nna).toBe(1);
    expect(a.pulse).toBeCloseTo(0.5, 3);
    expect(a.rawLevel).toBeCloseTo(0.1, 3);
    expect(a.bpm).toBeCloseTo(128.5, 5);
    expect(a.phase).toBe(1);
    expect(a.skipped).toBe(3);
    expect(a.mel).toBeUndefined();

    const b = parsePacket(framePacket(true));
    if (b.type !== PacketType.FRAME) throw new Error('expected FRAME');
    expect(b.mel?.length).toBe(50);
    expect(b.mel?.[10]).toBeCloseTo(50 / 255, 5);
  });

  it('parses STATUS and END with the same layout', () => {
    for (const t of [PacketType.STATUS, PacketType.END] as const) {
      const p = parsePacket(statusPacket(t));
      if (p.type !== PacketType.STATUS && p.type !== PacketType.END) throw new Error('bad type');
      expect(p.type).toBe(t);
      expect(p.audioSamplesSent).toBe(80000);
      expect(p.audioSamplesDropped).toBe(12);
      expect(p.clipSamples).toBe(44);
      expect(p.peakAbs).toBe(32767);
      expect(p.fps).toBeCloseTo(61.2, 5);
      expect(p.rssi).toBe(-67);
      expect(p.hwGain).toBe(32);
    }
  });

  it('rejects unknown types and short packets', () => {
    expect(() => parsePacket(new DataView(new ArrayBuffer(2)))).toThrow(RecordProtocolError);
    const dv = new DataView(new ArrayBuffer(8));
    dv.setUint8(0, 99);
    expect(() => parsePacket(dv)).toThrow(/unknown packet type/);
  });
});

describe('modular index math', () => {
  it('handles uint32 wrap', () => {
    expect(u32Diff(5, 0xfffffffb)).toBe(10);
    expect(u32Diff(100, 100)).toBe(0);
    expect(u32Diff(90, 100)).toBe(-10);
  });
  it('handles uint16 wrap', () => {
    expect(u16Diff(2, 65534)).toBe(4);
    expect(u16Diff(10, 10)).toBe(0);
  });
});

describe('blrec records', () => {
  it('round-trips records through encode/iterate', () => {
    const a = encodeRecord(RecordKind.PACKET, 0, 12.4, new Uint8Array([1, 2, 3]));
    const b = encodeRecord(RecordKind.TEXT, 2, 99999, new TextEncoder().encode('{"x":1}'));
    const recs = Array.from(iterRecords(concatChunks([a, b])));
    expect(recs).toHaveLength(2);
    expect(recs[0].kind).toBe(RecordKind.PACKET);
    expect(recs[0].rxMs).toBe(12);
    expect(new Uint8Array(recs[0].payload.buffer, recs[0].payload.byteOffset, 3)).toEqual(
      new Uint8Array([1, 2, 3])
    );
    expect(recs[1].segment).toBe(2);
    expect(new TextDecoder().decode(recs[1].payload)).toBe('{"x":1}');
  });

  it('throws on truncated data', () => {
    const a = encodeRecord(RecordKind.PACKET, 0, 0, new Uint8Array([1, 2, 3]));
    expect(() => Array.from(iterRecords(a.slice(0, a.length - 1).buffer))).toThrow(/truncated/);
  });
});
