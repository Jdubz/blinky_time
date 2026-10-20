import type { DeviceLink } from './deviceLink';
import { FirmwareError } from './errors';
import { crc16, toBase64, toHex } from './otaManifest';
import type { OtaRelease } from './releases';

/**
 * Host side of the firmware's signed OTA protocol (blinky-things/ota/
 * QspiOtaStaging.h): send the signed manifest, stream the image in order
 * with a CRC per chunk, then commit. The device verifies the signature,
 * hashes the image as it lands, re-reads it before committing, and only
 * then asks the bootloader to install it. Until `commit`, the running
 * firmware is untouched: anything that goes wrong before it is a retry.
 */

export interface OtaStatus {
  state: 'idle' | 'receiving' | 'complete';
  next: number;
  size: number;
  build: number;
  current: number;
  key: string;
  maxChunk: number;
  mtu: number;
}

/** Device refusals of the manifest, in words an owner can act on. */
const MANIFEST_REFUSALS: Record<string, string> = {
  'not-newer': 'The device already runs this release or a newer one.',
  'bad-signature':
    "The device didn't accept the update's signature, so it was not installed. The device's firmware is unchanged.",
  'wrong-board': 'This update is for a different kind of device.',
  'bad-size': 'The update is too large for this device.',
  'bad-load-addr': 'The update is built for a different memory layout.',
  'unknown-flags': "This update needs a newer device firmware to install; it wasn't installed.",
  'bad-format': "This update's format is newer than the device's firmware understands.",
};

export function parseOtaStatus(raw: Record<string, unknown>): OtaStatus {
  const n = (k: string) => {
    const v = raw[k];
    if (typeof v !== 'number' || !Number.isFinite(v)) {
      throw new FirmwareError(`The device's update status is missing "${k}"`);
    }
    return v;
  };
  const state = raw.state;
  if (state !== 'idle' && state !== 'receiving' && state !== 'complete') {
    throw new FirmwareError(`The device reported an unknown update state "${String(state)}"`);
  }
  if (n('proto') !== 1)
    throw new FirmwareError(`The device speaks update protocol ${n('proto')}; this page speaks 1`);
  return {
    state,
    next: n('next'),
    size: n('size'),
    build: n('build'),
    current: n('current'),
    key: typeof raw.key === 'string' ? raw.key : '?',
    maxChunk: n('maxChunk'),
    mtu: n('mtu'),
  };
}

export interface BulkWrites {
  enable(chunk: number): void;
  disable(): void;
}

export interface TransferOptions {
  onProgress?: (sent: number, total: number) => void;
  log?: (message: string, level?: 'info' | 'warn' | 'error') => void;
  signal?: AbortSignal;
  /** Per-chunk reply timeout. */
  chunkTimeoutMs?: number;
}

export class OtaClient {
  constructor(
    private readonly link: DeviceLink,
    /** BLE only: switch the transport to MTU-sized write-without-response. */
    private readonly bulk?: BulkWrites
  ) {}

  async status(): Promise<OtaStatus> {
    let raw: Record<string, unknown>;
    try {
      raw = await this.link.otaStatus();
    } catch (err) {
      // An `ERR ota ...` answer comes from firmware that HAS signed OTA (e.g.
      // a line glued to a lost fragment): report it as it is, not as "too old".
      if (err instanceof FirmwareError && err.message.includes('refused `ota status`')) throw err;
      throw new FirmwareError(
        "This device's firmware is too old to update from the browser (it doesn't answer `ota status`). " +
          `It needs one update another way first. (${err instanceof Error ? err.message : String(err)})`
      );
    }
    return parseOtaStatus(raw);
  }

  /**
   * End a line the device may have half-received (a lost BLE packet can
   * carry its newline; the device would glue the next command onto it), then
   * let whatever it answers to that fragment pass unread.
   */
  async resync(): Promise<void> {
    await this.link.fire('');
    await new Promise(r => setTimeout(r, RESYNC_DRAIN_MS));
  }

  /** Send the signed manifest; the device verifies it and erases its staging area (a few seconds). */
  async begin(release: OtaRelease): Promise<void> {
    const reply = await this.link.otaReply(`ota manifest ${toHex(release.manifest)}`, 60_000);
    if (reply.startsWith('OK ota ready')) return;
    const reason = /^ERR ota (\S+)/.exec(reply)?.[1] ?? reply;
    throw new FirmwareError(MANIFEST_REFUSALS[reason] ?? `The device refused the update: ${reply}`);
  }

  /** Stream the image from `from` (0, or the device's `next` when resuming). */
  async transfer(release: OtaRelease, from: number, opts: TransferOptions = {}): Promise<void> {
    const image = release.image;
    const total = image.length;
    const chunkTimeoutMs = opts.chunkTimeoutMs ?? 10_000;
    let off = from;
    let crcFailures = 0;
    let timeouts = 0;
    while (off < total) {
      if (opts.signal?.aborted) throw new FirmwareError('Cancelled');
      const piece = image.subarray(off, Math.min(off + CHUNK, total));
      const crc = crc16(piece).toString(16).toUpperCase();
      let reply: string;
      try {
        reply = await this.link.otaReply(
          `ota chunk ${off} ${crc} ${toBase64(piece)}`,
          chunkTimeoutMs
        );
      } catch (err) {
        // No reply: the line never ended on the device (e.g. its newline was
        // lost). Close it, ask where the device is, carry on from there. A
        // disconnect (or repeated silence) goes up to the caller.
        if (
          !(err instanceof FirmwareError) ||
          !err.message.startsWith('No reply') ||
          ++timeouts > 3
        )
          throw err;
        opts.log?.('No reply to a chunk; resynchronising with the device', 'warn');
        await this.resync();
        const st = await this.status();
        if (st.state === 'complete' && st.build === release.fields.build) {
          off = total;
        } else if (st.state === 'receiving' && st.build === release.fields.build) {
          off = st.next;
        } else {
          throw new FirmwareError(
            'The device lost the transfer; start the update again (nothing was installed)'
          );
        }
        opts.onProgress?.(off, total);
        continue;
      }
      const next = /^OK ota next=(\d+)/.exec(reply);
      const expected = /^ERR ota offset expected=(\d+)/.exec(reply);
      if (next) {
        const n = Number(next[1]);
        // A reply to an earlier (timed-out) chunk can arrive late; trust the device's count.
        off = n === off + piece.length ? n : (await this.status()).next;
        crcFailures = 0;
      } else if (reply.startsWith('OK ota complete')) {
        off = total;
      } else if (expected) {
        opts.log?.(`Resynchronising at byte ${expected[1]}`, 'warn');
        off = Number(expected[1]);
      } else if (reply.startsWith('ERR ota chunk-crc') || reply.startsWith('ERR ota bad-chunk')) {
        // Damaged on the way (cut, or glued to a lost fragment): resend.
        crcFailures++;
        if (this.bulk && crcFailures === 2) {
          // Packets are being cut on the way: drop to small acknowledged writes.
          opts.log?.('Bluetooth packets arrived damaged; switching to smaller writes', 'warn');
          this.bulk.disable();
        }
        if (crcFailures > 6)
          throw new FirmwareError('Chunks keep arriving damaged; the connection is too unreliable');
      } else {
        throw new FirmwareError(`The device stopped the transfer: ${reply}`);
      }
      opts.onProgress?.(off, total);
    }
  }

  /** Verify and install: the device re-reads the image, checks its hash, and resets into the bootloader. */
  async commit(): Promise<void> {
    // Re-reading ~465 KB from QSPI takes ~1 s before the reply.
    const reply = await this.link.otaReply('ota commit', 30_000);
    if (reply.startsWith('OK ota applying')) return;
    throw new FirmwareError(`The device did not install the update: ${reply}`);
  }

  /** Clear a partial transfer (the running firmware was never touched). */
  async abort(): Promise<void> {
    await this.link.otaReply('ota abort', 10_000);
  }
}

/** How long resync() lets the device's answer to a stray fragment pass. */
const RESYNC_DRAIN_MS = 1000;

/** Image bytes per chunk line: the firmware's MAX_CHUNK (QspiOtaStaging.h). */
export const CHUNK = 512;
