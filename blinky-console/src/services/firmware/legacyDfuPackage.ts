import { FirmwareError, hex32 } from './errors';
import { APP_END, APP_START } from './flashLayout';
import type { HexImage } from './intelHex';
import type { Uf2Block } from './uf2';

/** CRC-16/CCITT as the Nordic legacy DFU init packet uses it (port of compile.py crc16). */
export function crc16(data: Uint8Array): number {
  let crc = 0xffff;
  for (const byte of data) {
    crc = ((crc >> 8) & 0xff) | ((crc << 8) & 0xffff);
    crc ^= byte;
    crc ^= (crc & 0xff) >> 4;
    crc ^= (crc << 12) & 0xffff;
    crc ^= ((crc & 0xff) << 5) & 0xffff;
  }
  return crc & 0xffff;
}

export interface LegacyDfuImage {
  /** Application binary sent as the DFU image (written at APP_START). */
  firmware: Uint8Array<ArrayBuffer>;
  /** 14-byte SDK v11 init packet. */
  initPacket: Uint8Array<ArrayBuffer>;
}

/**
 * The legacy (SDK v11) DFU package the fleet sends over BLE, built from the
 * release hex exactly as blinky_server/firmware/compile.py generate_dfu_package
 * does: objcopy binary + init packet {device_type 0x0052 (the bootloader
 * checks it), device_rev 0xFFFF, app_version 0xFFFFFFFF, 1 SoftDevice req
 * 0xFFFE (any), CRC16(firmware)}.
 *
 * The bootloader writes the image at APP_START, so the hex must start there.
 */
export function buildLegacyDfuImage(hex: HexImage): LegacyDfuImage {
  if (hex.start !== APP_START) {
    throw new FirmwareError(
      `hex starts at ${hex32(hex.start)}; a BLE DFU image must start at ${hex32(APP_START)}`
    );
  }
  if (hex.end > APP_END) {
    throw new FirmwareError(
      `hex ends at ${hex32(hex.end)}, past the application region ${hex32(APP_END)}`
    );
  }
  const initPacket = new Uint8Array(14);
  const v = new DataView(initPacket.buffer);
  v.setUint16(0, 0x0052, true);
  v.setUint16(2, 0xffff, true);
  v.setUint32(4, 0xffffffff, true);
  v.setUint16(8, 1, true);
  v.setUint16(10, 0xfffe, true);
  v.setUint16(12, crc16(hex.bin), true);
  return { firmware: hex.bin, initPacket };
}

/**
 * The release's .uf2 and .hex are produced from the same build; both are
 * sha256-verified, but the USB path flashes one and the BLE path the other.
 * Require them to describe byte-identical flash contents, so a packaging bug
 * can't make the two transports install different firmware:
 *   - every byte a hex data record wrote (zeros included) must be in the UF2
 *     with the same value;
 *   - anything else the UF2 writes (uf2conv pads each 256-byte page, and
 *     zero-fills short gaps inside a page) must be 0x00.
 * Addresses neither file covers are never written by USB; BLE writes the
 * objcopy zero fill there. No code lives in such gaps.
 */
export function assertUf2MatchesHex(blocks: Uf2Block[], hex: HexImage): void {
  const covered = new Uint8Array(hex.bin.length);
  for (const { addr, payload } of blocks) {
    for (let i = 0; i < payload.length; i++) {
      const a = addr + i;
      const b = payload[i];
      const k = a - hex.start;
      if (a >= hex.start && a < hex.end && hex.recorded[k]) {
        if (hex.bin[k] !== b) throw new FirmwareError(`UF2 and hex differ at ${hex32(a)}`);
        covered[k] = 1;
      } else if (b !== 0) {
        throw new FirmwareError(`UF2 writes data at ${hex32(a)}, which the hex does not`);
      }
    }
  }
  for (let k = 0; k < covered.length; k++) {
    if (hex.recorded[k] && !covered[k]) {
      throw new FirmwareError(`hex data at ${hex32(hex.start + k)} is missing from the UF2`);
    }
  }
}
