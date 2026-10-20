import { FirmwareError, hex32 } from './errors';

export interface HexImage {
  /** Lowest address with data. */
  start: number;
  /** One past the highest address with data. */
  end: number;
  /**
   * start..end as one buffer, gaps filled with 0x00 — the same bytes
   * `arm-none-eabi-objcopy -I ihex -O binary` produces, which is what the
   * fleet's BLE DFU package carries (blinky_server/firmware/compile.py).
   */
  bin: Uint8Array<ArrayBuffer>;
}

/**
 * Parse an Intel HEX file strictly (port of tools/pre_upload_check.py
 * parse_intel_hex, plus overlap detection). Any malformed record, bad
 * checksum, overlapping data, data after EOF or missing EOF is an error.
 */
export function parseIntelHex(text: string): HexImage {
  const chunks: { addr: number; data: Uint8Array }[] = [];
  let base = 0;
  let sawEof = false;
  const lines = text.split(/\r?\n/);

  for (let i = 0; i < lines.length; i++) {
    const line = lines[i].trim();
    if (!line) continue;
    const where = `hex line ${i + 1}`;
    if (sawEof) throw new FirmwareError(`${where}: data after the EOF record`);
    if (line[0] !== ':' || line.length < 11 || line.length % 2 !== 1) {
      throw new FirmwareError(`${where}: not an Intel HEX record`);
    }
    const bytes = new Uint8Array((line.length - 1) / 2);
    for (let j = 0; j < bytes.length; j++) {
      const b = parseInt(line.substr(1 + j * 2, 2), 16);
      if (Number.isNaN(b)) throw new FirmwareError(`${where}: non-hex character`);
      bytes[j] = b;
    }
    const count = bytes[0];
    if (bytes.length !== count + 5) {
      throw new FirmwareError(`${where}: byte count ${count} does not match record length`);
    }
    let sum = 0;
    for (const b of bytes) sum = (sum + b) & 0xff;
    if (sum !== 0) throw new FirmwareError(`${where}: checksum mismatch`);

    const offset = (bytes[1] << 8) | bytes[2];
    const type = bytes[3];
    const data = bytes.subarray(4, 4 + count);
    switch (type) {
      case 0x00:
        chunks.push({ addr: base + offset, data });
        break;
      case 0x01:
        sawEof = true;
        break;
      case 0x02:
        if (count !== 2) throw new FirmwareError(`${where}: bad extended segment address record`);
        base = ((data[0] << 8) | data[1]) * 16;
        break;
      case 0x04:
        if (count !== 2) throw new FirmwareError(`${where}: bad extended linear address record`);
        base = ((data[0] << 8) | data[1]) * 0x10000;
        break;
      case 0x03:
      case 0x05:
        break; // start address records: no data (pre_upload_check ignores them too)
      default:
        throw new FirmwareError(`${where}: unknown record type ${type}`);
    }
  }
  if (!sawEof) throw new FirmwareError('hex has no EOF record (truncated file?)');
  if (chunks.length === 0) throw new FirmwareError('hex contains no data');

  chunks.sort((a, b) => a.addr - b.addr);
  const start = chunks[0].addr;
  let end = start;
  for (const c of chunks) {
    if (c.addr < end) throw new FirmwareError(`hex data overlaps at ${hex32(c.addr)}`);
    end = c.addr + c.data.length;
  }
  const bin = new Uint8Array(end - start);
  for (const c of chunks) bin.set(c.data, c.addr - start);
  return { start, end, bin };
}
