/**
 * RecordSession — drives one phone record session against one device over
 * Web Bluetooth, persisting everything to IndexedDB as it arrives.
 *
 * Integrity accounting (docs/PHONE_RECORD_MODE_PLAN.md):
 *   - audio gaps are measured by device sample index, per segment (this
 *     includes device-side ring overruns, which surface as index jumps)
 *   - per-type sequence gaps are counted for FRAME/STATUS packets (this
 *     includes device-side queue drops, which consume a sequence number)
 *   - malformed packets are counted (and still stored raw for forensics)
 * Nothing is silently discarded: every notification is persisted verbatim.
 *
 * A BLE disconnect ends the device-side session. If the user hasn't pressed
 * Stop, we reconnect and start a new *segment* — an explicit boundary in the
 * file, never a silent splice.
 */

import { logger } from '../../lib/logger';
import { TransportEvent } from '../transport/types';
import { WebBluetoothTransport } from '../transport/WebBluetoothTransport';
import { concatChunks, encodeRecord, RecordKind } from './blrecFormat';
import {
  FramePacket,
  isStartReply,
  PacketType,
  parsePacket,
  peakToDbfs,
  RECORD_DATA_CHAR,
  RECORD_PROTOCOL_VERSION,
  RECORD_SERVICE,
  RecordStartReply,
  StatusPacket,
  u16Diff,
  u32Diff,
} from './recordProtocol';
import {
  appendChunk,
  deleteSession,
  putSession,
  requestPersistence,
  SessionMeta,
} from './recordStore';

export type RecordState =
  | 'idle'
  | 'connecting'
  | 'connected'
  | 'starting'
  | 'recording'
  | 'stopping'
  | 'reconnecting';

export interface RecordStats {
  state: RecordState;
  deviceName: string | null;
  sessionId: string | null;
  segment: number;
  elapsedMs: number;
  bytes: number;
  kBps: number;
  audioSamples: number;
  audioGapSamples: number;
  frames: number;
  frameSeqGaps: number;
  statusSeqGaps: number;
  protocolErrors: number;
  /** Bytes waiting in memory because IndexedDB writes are failing. */
  unsavedBytes: number;
  lastStatus: StatusPacket | null;
  lastFrame: FramePacket | null;
  peakDbfs: number;
  startReply: RecordStartReply | null;
  message: string | null;
}

const FLUSH_INTERVAL_MS = 1000;
const START_TIMEOUT_MS = 4000;
const END_TIMEOUT_MS = 6000;
const RECONNECT_DELAY_MS = 1500;

type Listener = (stats: RecordStats) => void;

export class RecordSession {
  readonly transport = new WebBluetoothTransport();
  private dataChar: BluetoothRemoteGATTCharacteristic | null = null;
  private listeners: Listener[] = [];
  private meta: SessionMeta | null = null;
  private sessionT0 = 0;
  private pending: Uint8Array[] = [];
  private flushTimer: ReturnType<typeof setInterval> | null = null;
  private flushing: Promise<void> = Promise.resolve();
  private closing: Promise<void> | null = null;
  /** User intent: true from Start until Stop/Disconnect. Only user actions clear it. */
  private wantRecording = false;
  private withMel = false;
  private startPromise: Promise<void> | null = null;
  private reconnectLoopRunning = false;
  private lineWaiters: ((obj: Record<string, unknown>) => boolean)[] = [];
  private endWaiter: (() => void) | null = null;

  // Per-segment integrity tracking
  private segStartIdx: number | null = null;
  private expectedSampleIdx: number | null = null;
  private expectedSeq: Partial<Record<PacketType, number>> = {};

  // Throughput window
  private rateWindow: { t: number; bytes: number }[] = [];

  private stats: RecordStats = {
    state: 'idle',
    deviceName: null,
    sessionId: null,
    segment: -1,
    elapsedMs: 0,
    bytes: 0,
    kBps: 0,
    audioSamples: 0,
    audioGapSamples: 0,
    frames: 0,
    frameSeqGaps: 0,
    statusSeqGaps: 0,
    protocolErrors: 0,
    unsavedBytes: 0,
    lastStatus: null,
    lastFrame: null,
    peakDbfs: -Infinity,
    startReply: null,
    message: null,
  };

  constructor() {
    this.transport.addEventListener(this.onTransportEvent);
  }

  subscribe(fn: Listener): () => void {
    this.listeners.push(fn);
    fn({ ...this.stats });
    return () => {
      this.listeners = this.listeners.filter(l => l !== fn);
    };
  }

  private update(patch: Partial<RecordStats>): void {
    this.stats = { ...this.stats, ...patch };
    if (this.meta) this.stats.elapsedMs = Date.now() - this.sessionT0;
    const snapshot = { ...this.stats };
    this.listeners.forEach(l => l(snapshot));
  }

  get state(): RecordState {
    return this.stats.state;
  }

  // ---------------------------------------------------------------------
  // Connection
  // ---------------------------------------------------------------------

  async connect(): Promise<void> {
    this.update({ state: 'connecting', message: null });
    try {
      await this.transport.connect();
      await this.attachDataChar();
      this.update({
        state: 'connected',
        deviceName: this.transport.getDevice()?.name ?? 'Blinky',
      });
    } catch (err) {
      // Don't leave a half-open GATT link behind a failed attach.
      await this.transport.disconnect().catch(() => undefined);
      this.update({ state: 'idle', message: String(err) });
      throw err;
    }
  }

  async disconnect(): Promise<void> {
    this.wantRecording = false;
    if (this.meta) this.closeSegment('disconnected by user');
    await this.closeSession();
    await this.transport.disconnect();
    this.dataChar = null;
    this.update({ state: 'idle' });
  }

  private async attachDataChar(): Promise<void> {
    const server = this.transport.getServer();
    if (!server) throw new Error('GATT server not available');
    let svc: BluetoothRemoteGATTService;
    try {
      svc = await server.getPrimaryService(RECORD_SERVICE);
    } catch {
      throw new Error(
        'Device has no record service — flash firmware with record mode (docs/PHONE_RECORD_MODE_PLAN.md)'
      );
    }
    const chr = await svc.getCharacteristic(RECORD_DATA_CHAR);
    // Same function reference: EventTarget dedupes if the browser hands back
    // the same characteristic object after a reconnect.
    chr.addEventListener('characteristicvaluechanged', this.onDataNotify);
    await chr.startNotifications();
    this.dataChar = chr;
  }

  // ---------------------------------------------------------------------
  // Session control
  // ---------------------------------------------------------------------

  async start(withMel: boolean): Promise<void> {
    if (this.stats.state !== 'connected') throw new Error(`cannot start from ${this.stats.state}`);
    this.withMel = withMel;
    this.wantRecording = true;
    await requestPersistence();
    this.openSession();
    const sessionId = this.meta!.id;
    try {
      await this.runStartSegment();
    } catch (err) {
      // Nothing was recorded — don't leave an empty session behind.
      this.wantRecording = false;
      await this.closeSession();
      await deleteSession(sessionId).catch(() => undefined);
      this.update({ sessionId: null });
      throw err;
    }
  }

  async stop(): Promise<void> {
    this.wantRecording = false;
    // A start in flight resolves (recording) or rejects (cleaned up) first.
    if (this.startPromise) await this.startPromise.catch(() => undefined);

    if (this.stats.state !== 'recording') {
      // Reconnecting, or between reconnect and start: no device session to end.
      if (this.meta) {
        this.closeSegment('stopped while not recording');
        await this.closeSession();
      }
      this.update({ state: this.transport.isConnected() ? 'connected' : 'idle' });
      return;
    }

    this.update({ state: 'stopping' });
    const ended = new Promise<boolean>(resolve => {
      const t = setTimeout(() => {
        this.endWaiter = null;
        resolve(false);
      }, END_TIMEOUT_MS);
      this.endWaiter = () => {
        clearTimeout(t);
        resolve(true);
      };
    });
    try {
      await this.transport.send('record stop');
    } catch (err) {
      logger.error('record stop send failed', { err: String(err) });
    }
    const gotEnd = await ended;
    if (!gotEnd) {
      this.closeSegment('stop: no END packet within timeout');
      this.update({ message: 'Device did not confirm stop (no END packet) — tail may be missing' });
    }
    await this.closeSession();
    this.update({ state: this.transport.isConnected() ? 'connected' : 'idle' });
  }

  private openSession(): void {
    this.sessionT0 = Date.now();
    this.closing = null;
    this.meta = {
      id: `rec-${new Date(this.sessionT0).toISOString().replace(/[:.]/g, '-')}`,
      createdAt: this.sessionT0,
      updatedAt: this.sessionT0,
      deviceName: this.stats.deviceName ?? 'Blinky',
      withMel: this.withMel,
      segments: [],
      chunkCount: 0,
      bytes: 0,
      audioSamples: 0,
      audioGapSamples: 0,
      frameSeqGaps: 0,
      protocolErrors: 0,
      userAgent: navigator.userAgent,
    };
    this.pending = [];
    this.rateWindow = [];
    this.update({
      sessionId: this.meta.id,
      segment: -1,
      bytes: 0,
      kBps: 0,
      audioSamples: 0,
      audioGapSamples: 0,
      frames: 0,
      frameSeqGaps: 0,
      statusSeqGaps: 0,
      protocolErrors: 0,
      unsavedBytes: 0,
      lastStatus: null,
      lastFrame: null,
      peakDbfs: -Infinity,
      message: null,
    });
    void putSession({ ...this.meta }).catch(err => this.reportStorageError(err));
    this.flushTimer = setInterval(() => void this.flush(), FLUSH_INTERVAL_MS);
  }

  /** Idempotent: concurrent callers share one close. */
  private closeSession(): Promise<void> {
    if (!this.meta) return this.closing ?? Promise.resolve();
    if (this.closing) return this.closing;
    this.closing = (async () => {
      if (this.flushTimer) clearInterval(this.flushTimer);
      this.flushTimer = null;
      await this.flush();
      if (this.meta) {
        if (this.pending.length > 0) {
          this.update({
            message: `STORAGE: ${this.stats.unsavedBytes} bytes could not be saved before closing`,
          });
        }
        await putSession({ ...this.meta }).catch(err => this.reportStorageError(err));
      }
      this.meta = null;
      this.pending = [];
    })();
    return this.closing;
  }

  private runStartSegment(): Promise<void> {
    const p = this.startSegment();
    this.startPromise = p;
    return p.finally(() => {
      if (this.startPromise === p) this.startPromise = null;
    });
  }

  /** Send `record start` and wait for the device's reply. Never touches wantRecording. */
  private async startSegment(): Promise<void> {
    if (!this.meta) throw new Error('no session');
    this.update({ state: 'starting' });
    const segIndex = this.meta.segments.length;
    this.segStartIdx = null;
    this.expectedSampleIdx = null;
    this.expectedSeq = {};
    // Tag records with the new segment index before any packet can arrive.
    this.update({ segment: segIndex });

    const reply = this.waitForLine(
      obj => obj.record === 'started' || typeof obj.error === 'string',
      START_TIMEOUT_MS
    );
    try {
      await this.transport.send(this.withMel ? 'record start mel' : 'record start');
    } catch (err) {
      reply.catch(() => undefined); // superseded by the send failure
      this.update({ state: this.transport.isConnected() ? 'connected' : 'reconnecting' });
      throw err;
    }
    let obj: Record<string, unknown>;
    try {
      obj = await reply;
    } catch (err) {
      this.update({
        state: this.transport.isConnected() ? 'connected' : 'reconnecting',
        message: String(err),
      });
      throw err;
    }
    if (!isStartReply(obj)) {
      const msg = `record start refused: ${String(obj.error ?? JSON.stringify(obj))}`;
      this.update({ state: 'connected', message: msg });
      throw new Error(msg);
    }
    if (obj.v !== RECORD_PROTOCOL_VERSION) {
      // Refuse to record data we'd misparse.
      await this.transport.send('record stop').catch(() => undefined);
      const msg = `protocol version mismatch: device v${obj.v}, app v${RECORD_PROTOCOL_VERSION}`;
      this.update({ state: 'connected', message: msg });
      throw new Error(msg);
    }
    if (!this.meta) throw new Error('session closed while starting');
    this.meta.segments.push({ index: segIndex, startedAt: Date.now(), startReply: { ...obj } });
    this.segStartIdx = obj.startIdx >>> 0;
    if (this.expectedSampleIdx === null) {
      this.expectedSampleIdx = this.segStartIdx;
    }
    void putSession({ ...this.meta }).catch(err => this.reportStorageError(err));
    this.update({ state: 'recording', startReply: obj });
  }

  private closeSegment(reason: string): void {
    const seg = this.meta?.segments[this.meta.segments.length - 1];
    if (seg && seg.endedAt === undefined) {
      seg.endedAt = Date.now();
      seg.endReason = reason;
    }
  }

  private waitForLine(
    match: (obj: Record<string, unknown>) => boolean,
    timeoutMs: number
  ): Promise<Record<string, unknown>> {
    return new Promise((resolve, reject) => {
      const waiter = (obj: Record<string, unknown>): boolean => {
        if (!match(obj)) return false;
        clearTimeout(t);
        resolve(obj);
        return true;
      };
      const t = setTimeout(() => {
        this.lineWaiters = this.lineWaiters.filter(w => w !== waiter);
        reject(new Error('timed out waiting for device reply'));
      }, timeoutMs);
      this.lineWaiters.push(waiter);
    });
  }

  // ---------------------------------------------------------------------
  // Inbound data
  // ---------------------------------------------------------------------

  private now(): number {
    return Date.now() - this.sessionT0;
  }

  private persist(kind: RecordKind, payload: Uint8Array): void {
    if (!this.meta) return;
    this.pending.push(encodeRecord(kind, Math.max(0, this.stats.segment), this.now(), payload));
  }

  private onTransportEvent = (event: TransportEvent): void => {
    if (event.type === 'line' && event.line) {
      const line = event.line;
      if (this.meta) this.persist(RecordKind.TEXT, new TextEncoder().encode(line));
      if (line.startsWith('{')) {
        let obj: Record<string, unknown> | null = null;
        try {
          obj = JSON.parse(line) as Record<string, unknown>;
        } catch {
          // Non-JSON or truncated line — persisted above as text for forensics.
        }
        if (obj) {
          const o = obj;
          this.lineWaiters = this.lineWaiters.filter(w => !w(o));
        }
      }
    } else if (event.type === 'disconnected') {
      this.dataChar = null;
      void this.onLinkLost();
    }
  };

  private async onLinkLost(): Promise<void> {
    if (this.endWaiter) {
      // Lost the link mid-stop: the device ended the session itself.
      this.closeSegment('link lost during stop');
      const w = this.endWaiter;
      this.endWaiter = null;
      w();
      return;
    }
    if (!(this.wantRecording && this.meta)) {
      if (this.stats.state !== 'idle') this.update({ state: 'idle', message: 'Disconnected' });
      return;
    }
    this.closeSegment('link lost');
    if (this.reconnectLoopRunning) return; // an attempt is already in progress
    this.reconnectLoopRunning = true;
    this.update({ state: 'reconnecting', message: 'Link lost — reconnecting (new segment)' });
    try {
      while (this.wantRecording && this.meta) {
        await new Promise(r => setTimeout(r, RECONNECT_DELAY_MS));
        if (!this.wantRecording || !this.meta) break;
        try {
          if (!this.transport.isConnected()) await this.transport.reconnect();
          if (!this.dataChar) await this.attachDataChar();
          if (!this.wantRecording || !this.meta) break;
          await this.runStartSegment();
          this.update({ message: 'Reconnected — recording new segment' });
          return;
        } catch (err) {
          logger.warn('reconnect attempt failed', { err: String(err) });
          if (this.wantRecording && this.meta) {
            this.update({ state: 'reconnecting', message: `Reconnecting… (${String(err)})` });
          }
        }
      }
    } finally {
      this.reconnectLoopRunning = false;
    }
  }

  private onDataNotify = (event: Event): void => {
    const value = (event.target as BluetoothRemoteGATTCharacteristic).value;
    if (!value || !this.meta) return;
    // Persist verbatim first — the raw bytes are the record of truth.
    const bytes = new Uint8Array(
      value.buffer.slice(value.byteOffset, value.byteOffset + value.byteLength)
    );
    this.persist(RecordKind.PACKET, bytes);
    this.trackRate(bytes.length);

    let pkt;
    try {
      pkt = parsePacket(new DataView(bytes.buffer));
    } catch (err) {
      this.meta.protocolErrors++;
      this.update({ protocolErrors: this.meta.protocolErrors, message: String(err) });
      return;
    }

    const prevSeq = this.expectedSeq[pkt.type];
    const seqGap = prevSeq === undefined ? 0 : u16Diff(pkt.seq, prevSeq);
    this.expectedSeq[pkt.type] = (pkt.seq + 1) & 0xffff;

    switch (pkt.type) {
      case PacketType.AUDIO: {
        const n = pkt.samples.length;
        if (this.expectedSampleIdx === null) {
          // Audio beat the start reply; the reply's startIdx can't be checked
          // against it here (convert_blrec.py measures it from the header).
          this.expectedSampleIdx = pkt.sampleIndex;
        }
        const d = u32Diff(pkt.sampleIndex, this.expectedSampleIdx);
        if (d > 0) this.meta.audioGapSamples += d;
        else if (d < 0) this.meta.protocolErrors++; // overlap/rewind: should never happen
        this.expectedSampleIdx = (pkt.sampleIndex + n) >>> 0;
        this.meta.audioSamples += n;
        break;
      }
      case PacketType.FRAME:
        this.meta.frameSeqGaps += seqGap;
        this.stats.frames++;
        this.stats.lastFrame = pkt;
        break;
      case PacketType.STATUS:
        this.stats.statusSeqGaps += seqGap;
        this.stats.lastStatus = pkt;
        this.stats.peakDbfs = peakToDbfs(pkt.peakAbs);
        break;
      case PacketType.END:
        this.stats.lastStatus = pkt;
        this.closeSegment('stopped');
        if (this.endWaiter) {
          const w = this.endWaiter;
          this.endWaiter = null;
          w();
        }
        break;
    }
  };

  private trackRate(n: number): void {
    const t = Date.now();
    this.rateWindow.push({ t, bytes: n });
    while (this.rateWindow.length && t - this.rateWindow[0].t > 3000) this.rateWindow.shift();
  }

  private reportStorageError(err: unknown): void {
    logger.error('IndexedDB write failed', { err: String(err) });
    this.update({ message: `STORAGE WRITE FAILED: ${String(err)} — data is held in memory` });
  }

  private flush(): Promise<void> {
    // Serialize flushes so chunk indices stay ordered. Chain off a caught
    // promise so one failure can't wedge every later flush.
    this.flushing = this.flushing
      .catch(() => undefined)
      .then(async () => {
        const meta = this.meta;
        if (!meta) return;
        const parts = this.pending;
        this.pending = [];
        if (parts.length > 0) {
          try {
            await appendChunk(meta, concatChunks(parts));
          } catch (err) {
            // Keep the data and retry on the next flush; say so loudly.
            this.pending = parts.concat(this.pending);
            this.reportStorageError(err);
          }
        }
        const windowBytes = this.rateWindow.reduce((s, r) => s + r.bytes, 0);
        const span = this.rateWindow.length > 1 ? (Date.now() - this.rateWindow[0].t) / 1000 : 1;
        this.update({
          bytes: meta.bytes,
          kBps: windowBytes / 1024 / Math.max(span, 0.5),
          audioSamples: meta.audioSamples,
          audioGapSamples: meta.audioGapSamples,
          frameSeqGaps: meta.frameSeqGaps,
          protocolErrors: meta.protocolErrors,
          unsavedBytes: this.pending.reduce((s, p) => s + p.length, 0),
        });
      });
    return this.flushing;
  }
}
