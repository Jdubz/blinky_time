import { FirmwareError } from './errors';
import type { LegacyDfuImage } from './legacyDfuPackage';

/**
 * Nordic legacy DFU (SDK v11) client — the protocol the fleet bootloader's
 * BLE transport speaks. Port of blinky_server/firmware/ble_dfu.py
 * `_dfu_transfer_inner`, over an injected GATT interface so it can run on Web
 * Bluetooth in the browser and against a simulated bootloader in tests.
 */

export const DFU_SERVICE = '00001530-1212-efde-1523-785feabcd123';
export const DFU_CONTROL = '00001531-1212-efde-1523-785feabcd123';
export const DFU_PACKET = '00001532-1212-efde-1523-785feabcd123';
export const DFU_REVISION = '00001534-1212-efde-1523-785feabcd123';
/** Bootloader advertising name (dfu_transport_ble.c DEVICE_NAME). */
export const DFU_DEVICE_NAME = 'AdaDFU';

const OP_START = 0x01;
const OP_INIT = 0x02;
const OP_RECEIVE = 0x03;
const OP_VALIDATE = 0x04;
const OP_ACTIVATE = 0x05;
const OP_RESET = 0x06;
const OP_PRN = 0x08;
const NOTIF_RESPONSE = 0x10;
const NOTIF_PRN = 0x11;
const RESULT_SUCCESS = 0x01;
const RESULT_INVALID_STATE = 0x02;
const IMAGE_APPLICATION = 0x04;

/** Packet Receipt Notification interval (ble_dfu.py PRN_INTERVAL). */
export const PRN_INTERVAL = 8;
/**
 * DFU packet payload. Web Bluetooth does not expose the negotiated ATT MTU,
 * and a write-without-response larger than MTU-3 can be truncated by the OS,
 * so use the ATT-minimum payload every link supports (ble_dfu.py DFU_CHUNK_MIN).
 */
export const DFU_CHUNK = 20;

export interface DfuGatt {
  /** Write to DFU Control with response. */
  writeControl(data: Uint8Array<ArrayBuffer>): Promise<void>;
  /** Write to DFU Packet without response. */
  writePacket(data: Uint8Array<ArrayBuffer>): Promise<void>;
  /** DFU Revision (bootloader > 1); null if the characteristic is absent. */
  readRevision(): Promise<number | null>;
  /** Control-point notifications. */
  setNotifyHandler(cb: (data: Uint8Array) => void): void;
  disconnect(): void;
}

export class DfuTransferError extends FirmwareError {
  constructor(
    message: string,
    /** START_DFU may have reached the bootloader: the app may already be erased. */
    readonly eraseStarted: boolean,
    /** The bootloader rejected START_DFU (INVALID_STATE) and was reset; retry is safe. */
    readonly invalidState = false
  ) {
    super(message);
    this.name = 'DfuTransferError';
  }
}

export interface DfuProgress {
  phase: 'start' | 'init' | 'transfer' | 'validate' | 'activate';
  /** Image bytes acknowledged so far. */
  sent: number;
  total: number;
}

const sleep = (ms: number) => new Promise(r => setTimeout(r, ms));

class NotifyQueue {
  private items: Uint8Array[] = [];
  private waiter: ((d: Uint8Array) => void) | null = null;

  push = (d: Uint8Array): void => {
    if (this.waiter) {
      const w = this.waiter;
      this.waiter = null;
      w(d);
    } else {
      this.items.push(d);
    }
  };

  unshift(d: Uint8Array): void {
    this.items.unshift(d);
  }

  /** Next notification, or null on timeout. */
  next(timeoutMs: number): Promise<Uint8Array | null> {
    const head = this.items.shift();
    if (head) return Promise.resolve(head);
    return new Promise(resolve => {
      const t = setTimeout(() => {
        this.waiter = null;
        resolve(null);
      }, timeoutMs);
      this.waiter = d => {
        clearTimeout(t);
        resolve(d);
      };
    });
  }
}

function u32le(...values: number[]): Uint8Array<ArrayBuffer> {
  const b = new Uint8Array(values.length * 4);
  const v = new DataView(b.buffer);
  values.forEach((x, i) => v.setUint32(i * 4, x, true));
  return b;
}

/**
 * One DFU attempt against a connected bootloader. Throws DfuTransferError on
 * any failure (after trying SYSTEM_RESET so the bootloader isn't left stuck);
 * resolves once ACTIVATE_AND_RESET has been sent. The caller owns retries and
 * the post-flash version check — a resolved transfer is not a verified flash.
 */
export async function transferLegacyDfu(
  gatt: DfuGatt,
  image: LegacyDfuImage,
  onProgress: (p: DfuProgress) => void = () => {},
  timing = { settleMs: 2000, prnTimeoutMs: 15_000, receiveTimeoutMs: 120_000 },
  /** Cancellation; honoured up to START_DFU, the point of no return. */
  signal?: AbortSignal
): Promise<void> {
  const { firmware, initPacket } = image;
  const total = firmware.length;
  const queue = new NotifyQueue();
  let eraseStarted = false;

  const fail = (msg: string, invalidState = false) =>
    new DfuTransferError(msg, eraseStarted, invalidState);

  const waitResponse = async (name: string, opcode: number, timeoutMs: number): Promise<number> => {
    const deadline = Date.now() + timeoutMs;
    for (;;) {
      const remaining = deadline - Date.now();
      const d = remaining > 0 ? await queue.next(remaining) : null;
      if (!d) throw fail(`${name}: no response from the bootloader within ${timeoutMs / 1000}s`);
      // Stale PRNs and responses to other opcodes are skipped (ble_dfu.py wait_response).
      if (d.length < 3 || d[0] !== NOTIF_RESPONSE || d[1] !== opcode) continue;
      return d[2];
    }
  };
  const expectSuccess = async (name: string, opcode: number, timeoutMs: number) => {
    const code = await waitResponse(name, opcode, timeoutMs);
    if (code !== RESULT_SUCCESS) throw fail(`${name} failed (result 0x${code.toString(16)})`);
  };

  // Outside the try: a refusal here must not send SYSTEM_RESET to an app.
  const rev = await gatt.readRevision();
  if (rev !== null && rev <= 1) {
    gatt.disconnect();
    throw fail(`this is the app, not the bootloader (DFU revision ${rev})`);
  }

  try {
    gatt.setNotifyHandler(queue.push);
    await sleep(timing.settleMs);

    // Last chance to cancel: no await between this check and the START_DFU
    // write. The catch below resets the bootloader, which boots the intact app.
    if (signal?.aborted) throw fail('Cancelled');
    // START_DFU. Latch eraseStarted BEFORE the write: once it may have
    // reached the bootloader, the app may be gone (ble_dfu.py #218).
    eraseStarted = true;
    onProgress({ phase: 'start', sent: 0, total });
    await gatt.writeControl(new Uint8Array([OP_START, IMAGE_APPLICATION]));
    await gatt.writePacket(u32le(0, 0, total));
    // Erasing ~500 KB takes ~25 s (85 ms per 4 KB page).
    const startCode = await waitResponse('START_DFU', OP_START, 60_000);
    if (startCode === RESULT_INVALID_STATE) {
      // Explicit rejection: nothing was erased. Reset the bootloader; retry is safe.
      eraseStarted = false;
      throw fail('the bootloader was in an invalid state and has been reset', true);
    }
    if (startCode !== RESULT_SUCCESS)
      throw fail(`START_DFU failed (result 0x${startCode.toString(16)})`);

    onProgress({ phase: 'init', sent: 0, total });
    await gatt.writeControl(new Uint8Array([OP_INIT, 0x00]));
    await gatt.writePacket(initPacket);
    await gatt.writeControl(new Uint8Array([OP_INIT, 0x01]));
    await expectSuccess('INIT_DFU', OP_INIT, 60_000);

    await gatt.writeControl(new Uint8Array([OP_PRN, PRN_INTERVAL & 0xff, PRN_INTERVAL >> 8]));
    await gatt.writeControl(new Uint8Array([OP_RECEIVE]));

    let sent = 0;
    let written = 0;
    let packets = 0;
    let misses = 0;
    while (sent < total) {
      const end = Math.min(sent + DFU_CHUNK, total);
      let chunk = firmware.subarray(sent, end);
      if (end >= total && chunk.length % 4 !== 0) {
        const padded = new Uint8Array(chunk.length + (4 - (chunk.length % 4)));
        padded.set(chunk);
        chunk = padded;
      }
      await gatt.writePacket(chunk);
      sent = end;
      written += chunk.length;
      packets++;
      if (packets % PRN_INTERVAL === 0) {
        const d = await queue.next(timing.prnTimeoutMs);
        if (!d) {
          if (++misses >= 3)
            throw fail(`connection lost at ${sent}/${total} bytes (3 missed receipts)`);
        } else if (d[0] === NOTIF_PRN && d.length >= 5) {
          misses = 0;
          const rcvd = new DataView(d.buffer, d.byteOffset).getUint32(1, true);
          // Stricter than ble_dfu.py: a short count means the link dropped or
          // truncated packets, and the image would fail CRC anyway.
          if (rcvd !== written) {
            throw fail(`the bootloader received ${rcvd} bytes but ${written} were sent`);
          }
        } else if (d[0] === NOTIF_RESPONSE) {
          misses = 0;
          queue.unshift(d); // a command response; read it after the loop
        }
        onProgress({ phase: 'transfer', sent, total });
      }
    }
    onProgress({ phase: 'transfer', sent, total });

    const receiveCode = await waitResponse(
      'RECEIVE_FIRMWARE',
      OP_RECEIVE,
      timing.receiveTimeoutMs
    ).catch(() => null);
    if (receiveCode !== null && receiveCode !== RESULT_SUCCESS) {
      throw fail(`RECEIVE_FIRMWARE failed (result 0x${receiveCode.toString(16)})`);
    }
    // receiveCode null: the completion notification was lost. VALIDATE anyway:
    // it passes only if the bootloader holds the whole image (CRC match).
    onProgress({ phase: 'validate', sent, total });
    await gatt.writeControl(new Uint8Array([OP_VALIDATE]));
    await expectSuccess('VALIDATE', OP_VALIDATE, 30_000);

    onProgress({ phase: 'activate', sent, total });
    await gatt.writeControl(new Uint8Array([OP_ACTIVATE])).catch(() => {
      // The bootloader resets as soon as it activates; the write may not be acknowledged.
    });
    gatt.disconnect();
  } catch (err) {
    const e = err instanceof DfuTransferError ? err : fail(`DFU error: ${String(err)}`);
    // Don't leave the bootloader stuck mid-DFU (ble_dfu.py _reset_bootloader).
    await gatt.writeControl(new Uint8Array([OP_RESET])).catch(() => {});
    gatt.disconnect();
    throw e;
  }
}

/** Open the DFU service of a Web Bluetooth device already chosen in the picker. */
export async function connectWebBluetoothDfu(device: BluetoothDevice): Promise<DfuGatt> {
  if (!device.gatt) throw new FirmwareError('This Bluetooth device has no GATT server');
  const server = await device.gatt.connect();
  try {
    const svc = await server.getPrimaryService(DFU_SERVICE);
    const control = await svc.getCharacteristic(DFU_CONTROL);
    const packet = await svc.getCharacteristic(DFU_PACKET);
    const revision = await svc.getCharacteristic(DFU_REVISION).catch(() => null);
    let handler: (d: Uint8Array) => void = () => {};
    control.addEventListener('characteristicvaluechanged', ev => {
      const v = (ev.target as BluetoothRemoteGATTCharacteristic).value;
      if (v) handler(new Uint8Array(v.buffer.slice(v.byteOffset, v.byteOffset + v.byteLength)));
    });
    await control.startNotifications();
    await sleep(1000);
    return {
      writeControl: d => control.writeValueWithResponse(d),
      writePacket: d => packet.writeValueWithoutResponse(d),
      readRevision: async () => {
        if (!revision) return null;
        const v = await revision.readValue();
        return v.byteLength >= 2 ? v.getUint16(0, true) : null;
      },
      setNotifyHandler: cb => {
        handler = cb;
      },
      disconnect: () => {
        if (server.connected) server.disconnect();
      },
    };
  } catch (err) {
    if (server.connected) server.disconnect();
    throw new FirmwareError(`Could not open the bootloader's DFU service: ${String(err)}`);
  }
}
