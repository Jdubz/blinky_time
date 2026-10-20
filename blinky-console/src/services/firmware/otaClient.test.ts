import { describe, expect, it, vi } from 'vitest';
import type { Transport, TransportEvent, TransportEventCallback } from '../transport';
import vectors from './__fixtures__/ota_vectors.json';
import { DeviceLink } from './deviceLink';
import { CHUNK, OtaClient } from './otaClient';
import { crc16, parseOtaManifest } from './otaManifest';
import type { OtaRelease } from './releases';

const image = Uint8Array.from(atob(vectors.imageBase64), c => c.charCodeAt(0));
const manifest = Uint8Array.from(vectors.manifestHex.match(/../g)!, b => parseInt(b, 16));
const release = {
  entry: { tag: 'b215', build: 215, version: 'b215-aaaaaaaa' },
  image,
  manifest,
  fields: parseOtaManifest(manifest),
} as unknown as OtaRelease;

/**
 * The firmware's signed-OTA console (ota/QspiOtaStaging.h) in miniature:
 * in-order chunks, CRC per chunk, idempotent resends, one-line replies.
 */
class SimDevice implements Transport {
  staged = new Uint8Array(image.length);
  next = 0;
  state: 'idle' | 'receiving' | 'complete' = 'idle';
  manifestReply = 'OK ota ready build=215 size=4099 chunk=512';
  /** Damage the next N chunk lines (as a truncating BLE link would). */
  corruptNext = 0;
  /** Answer `ota status` like firmware without signed OTA. */
  legacy = false;
  lines: string[] = [];
  private listeners: TransportEventCallback[] = [];

  isSupported() {
    return true;
  }
  isConnected() {
    return true;
  }
  async connect() {}
  async disconnect() {}
  addEventListener(cb: TransportEventCallback) {
    this.listeners.push(cb);
  }
  removeEventListener(cb: TransportEventCallback) {
    this.listeners = this.listeners.filter(l => l !== cb);
  }
  private reply(line: string) {
    queueMicrotask(() => this.listeners.forEach(l => l({ type: 'line', line } as TransportEvent)));
  }

  async send(cmd: string) {
    this.lines.push(cmd);
    if (cmd === 'ota status') {
      if (this.legacy) return this.reply('Staging: no valid firmware staged');
      return this.reply(
        JSON.stringify({
          ota: {
            proto: 1,
            state: this.state,
            next: this.next,
            size: this.state === 'idle' ? 0 : image.length,
            build: this.state === 'idle' ? 0 : 215,
            current: 214,
            key: 'release',
            maxChunk: 512,
            mtu: 247,
            qspi: true,
          },
        })
      );
    }
    if (cmd.startsWith('ota manifest ')) {
      if (this.manifestReply.startsWith('OK')) {
        this.state = 'receiving';
        this.next = 0;
      }
      return this.reply(this.manifestReply);
    }
    const m = /^ota chunk (\d+) ([0-9A-F]+) (\S+)$/.exec(cmd);
    if (m) {
      const off = Number(m[1]);
      let data = Uint8Array.from(atob(m[3]), c => c.charCodeAt(0));
      if (this.corruptNext > 0) {
        this.corruptNext--;
        data = data.subarray(0, data.length - 7);
      }
      if (crc16(data) !== parseInt(m[2], 16)) return this.reply('ERR ota chunk-crc');
      if (off < this.next && off + data.length <= this.next)
        return this.reply(`OK ota next=${this.next}`);
      if (off !== this.next) return this.reply(`ERR ota offset expected=${this.next}`);
      this.staged.set(data, off);
      this.next = off + data.length;
      if (this.next === image.length) {
        this.state = 'complete';
        return this.reply(`OK ota complete size=${this.next}`);
      }
      return this.reply(`OK ota next=${this.next}`);
    }
    if (cmd === 'ota commit') {
      return this.reply(
        this.state === 'complete' ? 'OK ota applying build=215' : 'ERR ota not-complete'
      );
    }
  }
}

describe('OtaClient', () => {
  it('stages the whole image in order, 512 bytes per chunk, and commits', async () => {
    const dev = new SimDevice();
    const client = new OtaClient(new DeviceLink(dev));
    expect((await client.status()).state).toBe('idle');
    await client.begin(release);
    const progress: number[] = [];
    await client.transfer(release, 0, { onProgress: sent => progress.push(sent) });
    expect(dev.staged).toEqual(image);
    expect(progress).toEqual([512, 1024, 1536, 2048, 2560, 3072, 3584, 4096, 4099]);
    expect(dev.lines.filter(l => l.startsWith('ota chunk')).length).toBe(
      Math.ceil(image.length / CHUNK)
    );
    await client.commit();
  });

  it('resumes from the offset the device reports', async () => {
    const dev = new SimDevice();
    const client = new OtaClient(new DeviceLink(dev));
    await client.begin(release);
    await client
      .transfer(release, 0, {
        onProgress: sent => {
          if (sent === 2048) throw new Error('link dropped');
        },
      })
      .catch(() => {});
    const st = await client.status();
    expect(st).toMatchObject({ state: 'receiving', next: 2048, build: 215 });
    await client.transfer(release, st.next);
    expect(dev.staged).toEqual(image);
    expect(dev.lines.filter(l => l.startsWith('ota chunk 0 ')).length).toBe(1);
  });

  it('follows the device when it is ahead (a resend) or behind (a lost chunk)', async () => {
    const dev = new SimDevice();
    const client = new OtaClient(new DeviceLink(dev));
    await client.begin(release);
    // Host restarts from 0 while the device already has 1024 bytes: the
    // resends are acknowledged with the device's count, nothing is rewritten.
    dev.staged.set(image.subarray(0, 1024));
    dev.next = 1024;
    await client.transfer(release, 0);
    expect(dev.staged).toEqual(image);

    // Host skips ahead: the device names the offset it expects.
    const dev2 = new SimDevice();
    const client2 = new OtaClient(new DeviceLink(dev2));
    await client2.begin(release);
    const log = vi.fn();
    await client2.transfer(release, 1024, { log });
    expect(dev2.staged).toEqual(image);
    expect(log).toHaveBeenCalledWith('Resynchronising at byte 0', 'warn');
  });

  it('drops to small Bluetooth writes when packets keep arriving damaged', async () => {
    const dev = new SimDevice();
    const bulk = { enable: vi.fn(), disable: vi.fn() };
    const client = new OtaClient(new DeviceLink(dev), bulk);
    await client.begin(release);
    dev.corruptNext = 2;
    await client.transfer(release, 0, { log: () => {} });
    expect(bulk.disable).toHaveBeenCalledTimes(1);
    expect(dev.staged).toEqual(image);
  });

  it('gives up on a link that keeps damaging chunks', async () => {
    const dev = new SimDevice();
    const client = new OtaClient(new DeviceLink(dev));
    await client.begin(release);
    dev.corruptNext = 100;
    await expect(client.transfer(release, 0)).rejects.toThrow(/too unreliable/);
  });

  it.each([
    ['ERR ota not-newer current=216', /already runs this release or a newer one/],
    ['ERR ota bad-signature current=214', /didn't accept the update's signature/],
    ['ERR ota wrong-board current=214', /different kind of device/],
    ['ERR ota qspi-erase', /refused the update: ERR ota qspi-erase/],
  ])('explains a refused manifest: %s', async (reply, message) => {
    const dev = new SimDevice();
    dev.manifestReply = reply;
    await expect(new OtaClient(new DeviceLink(dev)).begin(release)).rejects.toThrow(message);
  });

  it('reports firmware without signed OTA as too old', async () => {
    vi.useFakeTimers();
    const dev = new SimDevice();
    dev.legacy = true;
    const p = new OtaClient(new DeviceLink(dev)).status();
    const done = expect(p).rejects.toThrow(/too old to update from the browser/);
    await vi.advanceTimersByTimeAsync(6000);
    await done;
    vi.useRealTimers();
  });

  it('refuses to commit an incomplete transfer', async () => {
    const dev = new SimDevice();
    const client = new OtaClient(new DeviceLink(dev));
    await client.begin(release);
    await expect(client.commit()).rejects.toThrow(
      /did not install the update: ERR ota not-complete/
    );
  });
});

describe('crc16', () => {
  it('is CRC-16/CCITT-FALSE, as the firmware and bootloader compute it', () => {
    expect(crc16(new TextEncoder().encode('123456789'))).toBe(0x29b1);
  });
});
