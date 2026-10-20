import { FirmwareError, hex32 } from './errors';
import { APP_START } from './flashLayout';

/**
 * Client-side reading of the signed OTA manifest (152 bytes, layout in
 * blinky-things/ota/OtaManifest.h, produced only by tools/ota_manifest.py).
 *
 * The page cannot check the signature meaningfully (it would only be
 * trusting the same mirror it downloaded from); the DEVICE verifies it with
 * the key built into its firmware. The page checks the unsigned facts so it
 * fails early and clearly: the layout, that the manifest describes this
 * release's build, and that the image it downloaded is the one the manifest
 * hashes.
 */

export const OTA_MANIFEST_LEN = 152;
const MAGIC = 0x544f4c42; // "BLOT"
const FORMAT = 1;
export const BOARD_XIAO_NRF52840_SENSE = 1;

export interface OtaManifestFields {
  flags: number;
  board: number;
  build: number;
  size: number;
  loadAddr: number;
  sha512: Uint8Array;
}

export function parseOtaManifest(bytes: Uint8Array): OtaManifestFields {
  if (bytes.length !== OTA_MANIFEST_LEN) {
    throw new FirmwareError(
      `The update manifest is ${bytes.length} bytes, expected ${OTA_MANIFEST_LEN}`
    );
  }
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (v.getUint32(0, true) !== MAGIC)
    throw new FirmwareError('The update manifest has the wrong format (magic)');
  if (v.getUint16(4, true) !== FORMAT) {
    throw new FirmwareError(
      `The update manifest is format ${v.getUint16(4, true)}; this page reads format ${FORMAT}`
    );
  }
  return {
    flags: v.getUint16(6, true),
    board: v.getUint32(8, true),
    build: v.getUint32(12, true),
    size: v.getUint32(16, true),
    loadAddr: v.getUint32(20, true),
    sha512: bytes.slice(24, 88),
  };
}

/** Check the downloaded image against the manifest and the release it was listed under. */
export async function checkImageAgainstManifest(
  image: Uint8Array<ArrayBuffer>,
  m: OtaManifestFields,
  expectedBuild: number
): Promise<void> {
  if (m.build !== expectedBuild) {
    throw new FirmwareError(
      `The update manifest is for build ${m.build}, but the release is b${expectedBuild}`
    );
  }
  if (m.board !== BOARD_XIAO_NRF52840_SENSE)
    throw new FirmwareError(`The update is for board ${m.board}`);
  if (m.loadAddr !== APP_START) {
    throw new FirmwareError(
      `The update loads at ${hex32(m.loadAddr)}, not the application start ${hex32(APP_START)}`
    );
  }
  if (m.size !== image.length) {
    throw new FirmwareError(`The image is ${image.length} bytes, the manifest says ${m.size}`);
  }
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-512', image));
  if (digest.some((b, i) => b !== m.sha512[i])) {
    throw new FirmwareError('The image does not match its signed manifest (SHA-512)');
  }
}

/**
 * CRC-16/CCITT as the firmware computes it per chunk (QspiOtaStaging::crc16,
 * initial 0xFFFF; the same function the bootloader uses for the image).
 */
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

export function toHex(bytes: Uint8Array): string {
  return Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
}

export function toBase64(bytes: Uint8Array): string {
  let s = '';
  for (const b of bytes) s += String.fromCharCode(b);
  return btoa(s);
}
