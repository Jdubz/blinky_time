import { crc16 } from '../legacyDfuPackage';
import type { DfuGatt } from '../legacyDfu';

// Test-only: a simulated fleet bootloader for the legacy DFU client.

export interface SimOptions {
  startResult?: number;
  /** Bytes of each packet the "radio" actually delivers (simulates OS truncation). */
  deliverBytes?: number;
  /** Drop the RECEIVE_FIRMWARE completion notification. */
  loseReceiveResponse?: boolean;
  revision?: number | null;
}

/** A legacy-DFU bootloader that behaves like the fleet's (SDK v11 dfu_transport_ble). */
export class SimBootloader implements DfuGatt {
  notify: (d: Uint8Array) => void = () => {};
  log: number[][] = [];
  received: number[] = [];
  imageSize = 0;
  expectedCrc = -1;
  prn = 0;
  packets = 0;
  state: 'idle' | 'size' | 'init' | 'receive' = 'idle';
  activated = false;
  resetCount = 0;
  disconnected = false;

  constructor(private readonly opts: SimOptions = {}) {}

  private send(...bytes: number[]) {
    queueMicrotask(() => this.notify(new Uint8Array(bytes)));
  }

  async readRevision() {
    return this.opts.revision === undefined ? 8 : this.opts.revision;
  }

  setNotifyHandler(cb: (d: Uint8Array) => void) {
    this.notify = cb;
  }

  disconnect() {
    this.disconnected = true;
  }

  async writeControl(d: Uint8Array) {
    this.log.push([...d]);
    switch (d[0]) {
      case 0x01:
        this.state = 'size';
        break;
      case 0x02:
        if (d[1] === 0x00) this.state = 'init';
        else this.send(0x10, 0x02, 0x01);
        break;
      case 0x08:
        this.prn = d[1] | (d[2] << 8);
        break;
      case 0x03:
        this.state = 'receive';
        break;
      case 0x04: {
        const img = Uint8Array.from(this.received.slice(0, this.imageSize));
        const ok = img.length === this.imageSize && crc16(img) === this.expectedCrc;
        this.send(0x10, 0x04, ok ? 0x01 : 0x05);
        break;
      }
      case 0x05:
        this.activated = true;
        break;
      case 0x06:
        this.resetCount++;
        break;
    }
  }

  async writePacket(d: Uint8Array) {
    if (this.state === 'size') {
      this.imageSize = new DataView(d.buffer, d.byteOffset).getUint32(8, true);
      const r = this.opts.startResult ?? 0x01;
      this.send(0x10, 0x01, r);
      this.state = 'idle';
    } else if (this.state === 'init') {
      this.expectedCrc = new DataView(d.buffer, d.byteOffset).getUint16(12, true);
      this.state = 'idle';
    } else if (this.state === 'receive') {
      const n = Math.min(d.length, this.opts.deliverBytes ?? d.length);
      this.received.push(...d.subarray(0, n));
      this.packets++;
      if (this.prn && this.packets % this.prn === 0) {
        const c = this.received.length;
        this.send(0x11, c & 0xff, (c >> 8) & 0xff, (c >> 16) & 0xff, (c >> 24) & 0xff);
      }
      if (this.received.length >= this.imageSize && !this.opts.loseReceiveResponse) {
        this.send(0x10, 0x03, 0x01);
      }
    }
  }
}

export function image(size: number) {
  const firmware = Uint8Array.from({ length: size }, (_, i) => (i * 7 + 3) & 0xff);
  const initPacket = new Uint8Array(14);
  new DataView(initPacket.buffer).setUint16(12, crc16(firmware), true);
  return { firmware, initPacket };
}
