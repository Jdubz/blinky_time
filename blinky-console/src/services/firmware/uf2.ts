import { FirmwareError, hex32 } from './errors';
import { APP_END, APP_START, UF2_FAMILY_NRF52840_APP } from './flashLayout';

const BLOCK = 512;
const MAGIC_START0 = 0x0a324655;
const MAGIC_START1 = 0x9e5d5157;
const MAGIC_END = 0x0ab16f30;
const FLAG_NOT_MAIN_FLASH = 0x00000001;
const FLAG_FAMILY_ID_PRESENT = 0x00002000;
const MAX_PAYLOAD = 476;

export interface Uf2Block {
  addr: number;
  payload: Uint8Array;
}

/**
 * Validate a release UF2 for the nRF52840 application slot (port of
 * tools/uf2_upload.py validate_uf2, stricter): every block must carry the
 * app family ID, number its blocks 0..N-1 consistently, and write only inside
 * [APP_START, APP_END) — never the MBR, SoftDevice, InternalFS config,
 * bootloader or UICR.
 */
export function validateUf2(data: Uint8Array): Uf2Block[] {
  if (data.length === 0 || data.length % BLOCK !== 0) {
    throw new FirmwareError(`UF2 size ${data.length} is not a non-zero multiple of 512`);
  }
  const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const n = data.length / BLOCK;
  const blocks: Uf2Block[] = [];
  for (let i = 0; i < n; i++) {
    const o = i * BLOCK;
    const u32 = (k: number) => view.getUint32(o + k, true);
    const where = `UF2 block ${i}`;
    if (u32(0) !== MAGIC_START0 || u32(4) !== MAGIC_START1 || u32(508) !== MAGIC_END) {
      throw new FirmwareError(`${where}: invalid magic`);
    }
    const flags = u32(8);
    const addr = u32(12);
    const size = u32(16);
    const blockNo = u32(20);
    const numBlocks = u32(24);
    const family = u32(28);
    if (flags & FLAG_NOT_MAIN_FLASH) throw new FirmwareError(`${where}: not-main-flash block`);
    if (!(flags & FLAG_FAMILY_ID_PRESENT) || family !== UF2_FAMILY_NRF52840_APP) {
      throw new FirmwareError(
        `${where}: family ${hex32(family)} is not the nRF52840 application family ${hex32(UF2_FAMILY_NRF52840_APP)}`
      );
    }
    if (blockNo !== i || numBlocks !== n) {
      throw new FirmwareError(`${where}: numbered ${blockNo}/${numBlocks}, expected ${i}/${n}`);
    }
    if (size === 0 || size > MAX_PAYLOAD) throw new FirmwareError(`${where}: payload size ${size}`);
    if (addr < APP_START || addr + size > APP_END) {
      throw new FirmwareError(
        `${where}: writes ${hex32(addr)}..${hex32(addr + size)}, outside the application region ` +
          `${hex32(APP_START)}..${hex32(APP_END)}`
      );
    }
    blocks.push({ addr, payload: data.subarray(o + 32, o + 32 + size) });
  }
  return blocks;
}
